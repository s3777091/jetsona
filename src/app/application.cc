#include "application.h"
#include "board.h"
#include "esp_log.h"
#include "esp_system.h"
#include "net/airplane_mode.h"
#include "net/bluetooth_manager.h"
#include "net/wifi_manager.h"
#include "platform/perf_governor.h"

#include <chrono>
#include <thread>

#define TAG "App"

static constexpr EventBits_t kAllEvents =
    MAIN_EVENT_SCHEDULE | MAIN_EVENT_CLOCK_TICK | MAIN_EVENT_TOGGLE_CHAT |
    MAIN_EVENT_STATE_CHANGED | MAIN_EVENT_NETWORK_CONNECTED |
    MAIN_EVENT_NETWORK_DISCONNECTED | MAIN_EVENT_ERROR;

Application::Application() {
    event_group_ = xEventGroupCreate();
}

Application::~Application() {
    if (clock_timer_) { esp_timer_stop(clock_timer_); esp_timer_delete(clock_timer_); }
    if (event_group_) xEventGroupDelete(event_group_);
}

bool Application::Initialize() {
    auto &board = Board::GetInstance();

    if (!board.GetDisplay()) {
        ESP_LOGE(TAG, "initialization aborted: no display owner");
        return false;
    }

    SetDeviceState(kDeviceStateStarting);

    Display *display = board.GetDisplay();
    if (display) {
        // Build the whole home UI at full clock; the token dies with this
        // scope and the boot-prefetch job re-boosts for its own work.
        auto boost = jetson::PerfGovernor::Instance().Acquire("boot-ui");
        display->SetupUI();
        display->SetStatus("Ready");
    }

    // Radio state can be reset by the kernel during a reboot. Re-apply a
    // persisted airplane mode off the UI thread so startup remains responsive.
    std::thread([]() {
        auto result = jetson::EnforcePersistedAirplaneMode(
            jetson::WifiManager::Instance(), jetson::BluetoothManager::Instance());
        if (!result.success)
            ESP_LOGE(TAG, "could not fully enforce persisted airplane mode: %s",
                     result.error.c_str());
    }).detach();

    // 1s clock tick -> status bar refresh.
    esp_timer_create_args_t args = {
        .callback = OnClockTimer,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&args, &clock_timer_);
    esp_timer_start_periodic(clock_timer_, 1000000);

    // Phase 1: no network/protocol. Go straight to idle.
    SetDeviceState(kDeviceStateIdle);
    ESP_LOGI(TAG, "Initialized (phase-1 UI shell)");
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
            auto &board = Board::GetInstance();
            if (auto *d = board.GetDisplay()) d->UpdateStatusBar(false);
        }
        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            ESP_LOGI(TAG, "toggle chat (phase-1: no protocol)");
        }
        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            ESP_LOGI(TAG, "network connected (phase-1: handled by OS)");
        }
        if (bits & MAIN_EVENT_ERROR) {
            ESP_LOGE(TAG, "error event");
        }
    }
    ESP_LOGI(TAG, "Leaving main event loop");
}

void Application::RequestStop() {
    running_.store(false);
    // Wake Run() if it is blocked waiting for UI events. This is called by the
    // normal signal-waiting thread, never from an asynchronous signal handler.
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
    auto *d = Board::GetInstance().GetDisplay();
    if (!d) return;
    switch (state_) {
    case kDeviceStateListening: d->SetStatus("Listening"); d->SetEmotion("happy"); break;
    case kDeviceStateSpeaking:  d->SetStatus("Speaking");  d->SetEmotion("happy"); break;
    case kDeviceStateIdle:      d->SetStatus("Ready");     d->SetEmotion("neutral"); break;
    case kDeviceStateConnecting: d->SetStatus("Connecting"); break;
    case kDeviceStateWifiConfiguring: d->SetStatus("WiFi config"); break;
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

void Application::ToggleChatState() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT);
}
void Application::StartListening() { SetDeviceState(kDeviceStateListening); }
void Application::StopListening() { SetDeviceState(kDeviceStateIdle); }

void Application::Reboot() {
    ESP_LOGW(TAG, "Reboot requested -> esp_restart()");
    esp_restart();
}

void Application::Alert(const char *status, const char *message, const char * /*emotion*/) {
    auto *d = Board::GetInstance().GetDisplay();
    if (d) d->ShowNotification(message ? message : (status ? status : ""));
}

void Application::OnClockTimer(void *arg) {
    auto *self = static_cast<Application *>(arg);
    xEventGroupSetBits(self->event_group_, MAIN_EVENT_CLOCK_TICK);
}
