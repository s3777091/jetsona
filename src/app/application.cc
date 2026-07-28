#include "application.h"
#include "board.h"
#include "esp_log.h"
#include "esp_system.h"
#include "platform/perf_governor.h"
#include "platform/fan_control.h"
#include "platform/shell_command.h"
#include "net/weather_client.h"
#include "agent/conversation.h"
#include "agent/tools.h"
#include "agent/system_tools.h"
#include "agent/device_bridge.h"
#include "audio/voice_loop.h"
#include "settings.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <thread>
#include <vector>

#define TAG "App"

namespace {

bool HasAlarmAudioExtension(const std::string &name) {
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = name.substr(dot);
    for (char &c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".mp3" || ext == ".wav" || ext == ".ogg" || ext == ".m4a";
}

std::string DefaultRingtonePath() {
    const std::string dir = std::string(JETSON_ASSETS_DIR) + "/ringtones";
    DIR *handle = opendir(dir.c_str());
    if (!handle) return "";

    std::vector<std::string> names;
    while (dirent *entry = readdir(handle)) {
        const std::string name = entry->d_name;
        if (HasAlarmAudioExtension(name)) names.push_back(name);
    }
    closedir(handle);
    if (names.empty()) return "";
    std::sort(names.begin(), names.end());
    return dir + "/" + names.front();
}

std::string AlarmAudioDevice() {
    const char *device = std::getenv("JETSON_MUSIC_DEVICE");
    if (!device || !*device) device = std::getenv("JETSON_VOICE_OUT");
    return device && *device ? device : "";
}

} // namespace

static constexpr EventBits_t kAllEvents =
    MAIN_EVENT_SCHEDULE | MAIN_EVENT_CLOCK_TICK | MAIN_EVENT_STATE_CHANGED |
    MAIN_EVENT_ERROR;

Application::Application() {
    event_group_ = xEventGroupCreate();
}

Application::~Application() {
    if (clock_timer_) { esp_timer_stop(clock_timer_); esp_timer_delete(clock_timer_); }
    if (event_group_) xEventGroupDelete(event_group_);
}

bool Application::Initialize() {
    SetDeviceState(kDeviceStateStarting);

    /* Quiet thermal profile: the fan curve self-adjusts to CPU+GPU temp, so the
     * profile only sets how hard it idles and how early it ramps. Balanced
     * holds a PWM 45 floor -- measured at 2660 rpm with the board sitting at
     * 30 C, audible across the room for a load that is nowhere near warm.
     * Quiet's floor is PWM 25 (~1450 rpm) and does not ramp until 50 C, twenty
     * degrees above idle here. The curve script's own measurements put the cost
     * at about one degree. On a device that listens for a wake word in a quiet
     * room, that degree is worth buying. */
    jetson::fan::SetMode(jetson::fan::Mode::Auto);
    jetson::fan::SetProfile(jetson::fan::Profile::Quiet);

    // ---- Headless AI core ------------------------------------------------
    // Same three lines the old home display used, now driven without a UI.
    conversation_ = std::make_shared<jetson::Conversation>();
    conversation_->SetTools(jetson::BuildDefaultToolRegistry());
    conversation_->SetOnToolEvent([](std::string name, std::string status) {
        ESP_LOGI(TAG, "tool %s: %s", name.c_str(), status.c_str());
        // A tool means this turn needs two LLM round trips; fill the silence.
        if (status == "start")
            jetson::audio::VoiceLoop::Instance().NotifyToolStarted();
    });
    conversation_->ReloadConfig();

    // DeviceBridge handlers run on this main loop via Schedule(). With no
    // display, open_app/brightness/notifier become logs (or TTS for notify);
    // volume still drives the real ALSA/PipeWire mixer through the codec.
    auto &bridge = jetson::DeviceBridge::Instance();
    bridge.SetVolumeSetter([](int volume, bool muted) {
        Board::GetInstance().GetAudioCodec()->SetOutputState(volume, muted);
        Settings("display", true).SetInt("volume", volume);
        Settings("display", true).SetBool("muted", muted);
    });
    bridge.SetBrightnessSetter([](int /*percent*/) {
        // No panel on Ekko Lite; acknowledged so the tool reply stays positive.
    });
    bridge.SetAppOpener([](const std::string &app_id) {
        ESP_LOGI(TAG, "open_app '%s' -> no display, ignored", app_id.c_str());
    });
    bridge.SetNotifier([this](const std::string &text) { Speak(text); });
    bridge.SetReminderReloader([]() { /* no live view to reload */ });
    bridge.SetCalendarReloader([]() { /* no live view to reload */ });

    // 1s clock tick -> battery watcher + alarm scheduler.
    esp_timer_create_args_t args = {
        .callback = OnClockTimer,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&args, &clock_timer_);
    esp_timer_start_periodic(clock_timer_, 1000000);

    // Voice loop: mic -> "Ekko" -> STT -> agent -> TTS -> speaker. Starts even
    // if the sherpa models aren't synced yet (it just won't hear anything);
    // once models land under assets/models/ it works without a restart because
    // the engine lazily builds each sub-model on first use.
    jetson::audio::VoiceLoop::Instance().Start();

    SetDeviceState(kDeviceStateIdle);
    ESP_LOGI(TAG, "Initialized (Ekko Lite, headless AI core ready)");
    return true;
}

void Application::Run() {
    ESP_LOGI(TAG, "Entering main event loop");
    while (running_.load()) {
        EventBits_t bits = xEventGroupWaitBits(event_group_, kAllEvents,
                                               pdTRUE, pdFALSE, portMAX_DELAY);
        if (!running_.load()) break;
        if (bits & MAIN_EVENT_SCHEDULE) {
            std::deque<std::function<void()>> tasks;
            {
                std::lock_guard<std::mutex> lk(tasks_mtx_);
                tasks.swap(main_tasks_);
            }
            for (auto &t : tasks) t();
        }
        if (bits & MAIN_EVENT_CLOCK_TICK) {
            TickClock();
        }
        if (bits & MAIN_EVENT_ERROR) {
            ESP_LOGE(TAG, "error event");
        }
    }
    ESP_LOGI(TAG, "Leaving main event loop");

    // Tear the voice loop down while the process is still whole. Its engine
    // holds onnxruntime handles that cannot be destroyed once static teardown
    // has started -- see the note in VoiceLoop::Stop().
    jetson::audio::VoiceLoop::Instance().Stop();
}

void Application::RequestStop() {
    running_.store(false);
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

bool Application::SetDeviceState(DeviceState state) {
    if (state == state_) return true;
    DeviceState old = state_;
    state_ = state;
    ESP_LOGI(TAG, "state %d -> %d", (int)old, (int)state);
    xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    HandleStateChanged();
    return true;
}

void Application::HandleStateChanged() {
    // No display: states are logged only. The voice loop reads state to decide
    // mic gating (Listening/Speaking) in Phase C.
    switch (state_) {
    case kDeviceStateListening: ESP_LOGI(TAG, "listening"); break;
    case kDeviceStateSpeaking:  ESP_LOGI(TAG, "speaking");  break;
    case kDeviceStateIdle:      ESP_LOGI(TAG, "idle");      break;
    default: break;
    }
}

void Application::Schedule(std::function<void()> &&callback) {
    {
        std::lock_guard<std::mutex> lk(tasks_mtx_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::Speak(const std::string &text) {
    // Once the voice loop is up, every spoken message -- battery alerts, the
    // alarm briefing, agent replies -- routes through its TTS speaker thread.
    // Before it starts (early boot), or if the mic/models aren't ready, log
    // so nothing fails silently.
    if (jetson::audio::VoiceLoop::Instance().running()) {
        jetson::audio::VoiceLoop::Instance().Speak(text);
        return;
    }
    ESP_LOGW(TAG, "SPEAK: %s", text.c_str());
}

void Application::Alert(const char *status, const char *message, const char * /*emotion*/) {
    Speak(message ? message : (status ? status : ""));
}

void Application::TickClock() {
    // Alarm scheduling is independent of battery state. This must happen
    // before the charging fast-path below: the old order returned here every
    // second while plugged in, silently disabling all alarms overnight.
    CheckAlarm();

    // ---- Low-battery alert: 50 / 30 / 10 %, once each, never silent --------
    // Ekko Lite has no screen, so a dying battery must be spoken, not shown.
    // Three alerts total (one per threshold); flags reset the moment the
    // charger appears so a brief blip doesn't burn the announcement.
    int level = 100;
    bool charging = false, discharging = false;
    Board::GetInstance().GetBatteryLevel(level, charging, discharging);

    if (charging) {
        battery_50_said_ = battery_30_said_ = battery_10_said_ = false;
        last_battery_level_ = level;
        return;
    }
    if (last_battery_level_ >= 0 && level > last_battery_level_ + 5)
        battery_50_said_ = battery_30_said_ = battery_10_said_ = false;

    if (!battery_50_said_ && level <= 50 && level > 30) {
        Speak("Pin con " + std::to_string(level) + " phan tram.");
        battery_50_said_ = true;
    } else if (!battery_30_said_ && level <= 30 && level > 10) {
        Speak("Pin con " + std::to_string(level) + " phan tram, nen sac ngay.");
        battery_30_said_ = true;
    } else if (!battery_10_said_ && level <= 10) {
        Speak("Pin chi con " + std::to_string(level) + " phan tram, sap het.");
        battery_10_said_ = true;
    }
    last_battery_level_ = level;

}

void Application::CheckAlarm() {
    Settings a("alarm", false);
    if (a.GetString("enabled", "0") != "1") return;
    const std::string target = a.GetString("time", "");
    if (target.size() != 5 || target[2] != ':') return;

    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char now_hhmm[6];
    std::snprintf(now_hhmm, sizeof(now_hhmm), "%02d:%02d", tm.tm_hour, tm.tm_min);
    char today[11];
    std::snprintf(today, sizeof(today), "%04d-%02d-%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);

    if (now_hhmm != target) return;
    // Persist last_fired so a reboot the same morning doesn't re-fire.
    if (a.GetString("last_fired_date", "") == today) return;
    Settings("alarm", true).SetString("last_fired_date", today);
    ESP_LOGI(TAG, "alarm firing at %s on %s", target.c_str(), today);
    FireAlarm();
}

void Application::FireAlarm() {
    // 1) Ringtone: loop the active ringtone in the background, pid to file so
    // AlarmTool 'stop' can kill it. Falls back to a repeating tone if no
    // ringtone is set or synced, so the alarm is never silent.
    Settings a("alarm", false);
    std::string ring = a.GetString("ringtone", "");
    std::string path;
    if (!ring.empty()) path = std::string(JETSON_ASSETS_DIR) + "/ringtones/" + ring;
    std::ifstream probe(path);
    if (path.empty() || !probe) path = DefaultRingtonePath();

    const std::string device = AlarmAudioDevice();
    if (!path.empty()) {
        std::string cmd = "mpv --no-video --really-quiet --no-terminal "
                          "--force-window=no --loop-file=inf";
        if (!device.empty())
            cmd += " " + jetson::platform::QuoteShellArgument(
                               "--audio-device=alsa/" + device);
        cmd += " -- " + jetson::platform::QuoteShellArgument(path) +
                          " >/dev/null 2>&1 & echo $! > " + jetson::AlarmPidFile();
        jetson::platform::RunShellCommand(cmd);
        ESP_LOGI(TAG, "alarm ringtone started: %s", path.c_str());
    } else {
        // Keep ringing until the user says "stop", on the physical reSpeaker
        // rather than ALSA's inaudible Tegra default.
        std::string cmd = "speaker-test";
        if (!device.empty())
            cmd += " -D " + jetson::platform::QuoteShellArgument(device);
        cmd += " -c 2 -t sine -f 880 -l 0 >/dev/null 2>&1 & echo $! > " +
               jetson::AlarmPidFile();
        jetson::platform::RunShellCommand(cmd);
        ESP_LOGW(TAG, "no ringtone asset available; started fallback alarm tone");
    }

    // 2) Da Nang weather briefing.
    jetson::WeatherInfo w;
    std::string werr;
    if (jetson::WeatherClient::Fetch(w, werr)) {
        Speak("Chao buoi sang. Thoi tiet Da Nang hom nay: " +
              jetson::WeatherClient::FormatLine(w));
    } else {
        ESP_LOGW(TAG, "alarm weather fetch failed: %s", werr.c_str());
        Speak("Chao buoi sang. Khong lay duoc thoi tiet.");
    }

    // 3) Morning reminders the user asked for last night ("mai bao toi").
    auto notes = jetson::TaskStore::Instance().PendingMorningNotes();
    if (!notes.empty()) {
        Speak("Ban co " + std::to_string(notes.size()) + " ghi chu hom qua:");
        for (const auto &n : notes) Speak(n);
        jetson::TaskStore::Instance().MarkMorningNotesSpoken();
    }
}

void Application::Reboot() {
    ESP_LOGW(TAG, "Reboot requested -> esp_restart()");
    esp_restart();
}

void Application::OnClockTimer(void *arg) {
    auto *self = static_cast<Application *>(arg);
    xEventGroupSetBits(self->event_group_, MAIN_EVENT_CLOCK_TICK);
}
