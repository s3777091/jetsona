#include "audio/realtime_client.h"

#include "agent/system_tools.h"
#include "audio/audio_output.h"
#include "esp_log.h"
#include "settings.h"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define TAG "Realtime"

namespace jetson::audio {

namespace {

// Frame types; must match scripts/gemini_live_runtime.py.
constexpr uint8_t kMsgAudioIn = 0x01;
constexpr uint8_t kMsgToolResult = 0x02;
constexpr uint8_t kMsgConfig = 0x03;

constexpr uint8_t kMsgAudioOut = 0x81;
constexpr uint8_t kMsgInterrupted = 0x82;
constexpr uint8_t kMsgToolCall = 0x83;
constexpr uint8_t kMsgTurnComplete = 0x84;
constexpr uint8_t kMsgTranscript = 0x85;

// The model streams 24 kHz mono; the microphone gives 16 kHz mono.
constexpr int kReplySampleRate = 24000;

std::string EnvOr(const char *key, const std::string &fallback) {
    const char *value = std::getenv(key);
    return value && *value ? value : fallback;
}

double EnvDouble(const char *key, double fallback) {
    const char *value = std::getenv(key);
    if (!value || !*value) return fallback;
    char *end = nullptr;
    const double parsed = std::strtod(value, &end);
    return (end && *end == '\0') ? parsed : fallback;
}

double NowSeconds() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

/* Daily budget.
 *
 * A realtime session is billed by the second it stays open, unlike the chat
 * path which is billed per exchange, so a session that fails to close -- or a
 * run of false wakes -- spends money for as long as nobody notices. The ledger
 * lives on disk so a firmware restart cannot reset the day's total, and the
 * running figure is logged after every session rather than left to be
 * discovered on a bill.
 *
 * Running out is not an outage: Start() refuses, VoiceLoop falls through to the
 * STT/LLM/TTS chain, and the device keeps answering at roughly a thousandth of
 * the cost until midnight. */
std::string LedgerPath() {
    const char *dir = std::getenv("JETSON_STATE_DIR");
    return std::string((dir && *dir) ? dir : "/var/lib/jetson-fw") +
           "/realtime_usage.json";
}

std::string Today() {
    const std::time_t now = std::time(nullptr);
    std::tm parts{};
    localtime_r(&now, &parts);
    char buffer[16];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &parts);
    return buffer;
}

double SecondsUsedToday() {
    std::ifstream file(LedgerPath());
    if (!file) return 0.0;
    auto parsed = nlohmann::json::parse(file, nullptr, false);
    if (!parsed.is_object() || parsed.value("date", "") != Today()) return 0.0;
    return parsed.value("seconds", 0.0);
}

void RecordSeconds(double seconds) {
    if (seconds <= 0.0) return;
    nlohmann::json ledger;
    ledger["date"] = Today();
    ledger["seconds"] = SecondsUsedToday() + seconds;
    std::ofstream file(LedgerPath(), std::ios::trunc);
    if (file) file << ledger.dump();
}

bool WriteAll(int fd, const void *data, size_t size) {
    const auto *p = static_cast<const uint8_t *>(data);
    while (size > 0) {
        const ssize_t sent = send(fd, p, size, MSG_NOSIGNAL);
        if (sent <= 0) return false;
        p += sent;
        size -= static_cast<size_t>(sent);
    }
    return true;
}

bool ReadAll(int fd, void *data, size_t size) {
    auto *p = static_cast<uint8_t *>(data);
    while (size > 0) {
        const ssize_t got = recv(fd, p, size, 0);
        if (got <= 0) return false;
        p += got;
        size -= static_cast<size_t>(got);
    }
    return true;
}

} // namespace

RealtimeClient::RealtimeClient() = default;
RealtimeClient::~RealtimeClient() { Stop(); }

bool RealtimeClient::Start(const std::string &system_prompt,
                           const std::string &tools_json, AudioOutput *out,
                           ToolExecutor executor, TurnDoneCb on_turn_done) {
    if (running_.load()) return true;

    const double budget_min = EnvDouble("GEMINI_LIVE_DAILY_MINUTES", 60.0);
    const double used_min = SecondsUsedToday() / 60.0;
    if (budget_min > 0.0 && used_min >= budget_min) {
        ESP_LOGW(TAG,
                 "daily realtime budget spent (%.1f of %.0f min); "
                 "falling back to the STT/LLM/TTS path until tomorrow",
                 used_min, budget_min);
        return false;
    }

    const std::string socket_path =
        EnvOr("GEMINI_LIVE_SOCKET", "/run/jetsona-live/realtime.sock");
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        ESP_LOGE(TAG, "socket() failed: %s", std::strerror(errno));
        return false;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(address.sun_path)) {
        close(fd);
        ESP_LOGE(TAG, "socket path too long");
        return false;
    }
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
    if (connect(fd, reinterpret_cast<sockaddr *>(&address),
                sizeof(address)) != 0) {
        ESP_LOGW(TAG, "sidecar not reachable at %s: %s", socket_path.c_str(),
                 std::strerror(errno));
        close(fd);
        return false;
    }

    fd_ = fd;
    out_ = out;
    out_gain_ = std::max(0.05, std::min(1.0, EnvDouble("JETSON_VOICE_OUT_GAIN", 0.35)));
    echo_tail_sec_ = std::max(
        0.1, std::min(2.0, EnvDouble("JETSON_ECHO_TAIL_MS", 650.0) / 1000.0));
    assistant_audio_active_.store(false);
    {
        std::lock_guard<std::mutex> lk(activity_mtx_);
        echo_until_ = 0.0;
    }
    executor_ = std::move(executor);
    on_turn_done_ = std::move(on_turn_done);
    playing_ = false;
    turn_muted_.store(false);
    MarkActivity();

    nlohmann::json config;
    config["system"] = system_prompt;
    auto tools = nlohmann::json::parse(tools_json, nullptr, false);
    config["tools"] = tools.is_discarded() ? nlohmann::json::array() : tools;
    config["voice"] = EnvOr("GEMINI_LIVE_VOICE", "Aoede");
    const std::string payload = config.dump();

    running_.store(true);
    if (!SendFrame(kMsgConfig, payload.data(), payload.size())) {
        ESP_LOGE(TAG, "could not send session config");
        running_.store(false);
        close(fd_);
        fd_ = -1;
        return false;
    }

    session_started_ = NowSeconds();
    reader_ = std::thread(&RealtimeClient::ReaderThread, this);
    ESP_LOGI(TAG, "session open (voice=%s, %zu tools, %.1f/%.0f min dùng hôm nay)",
             config["voice"].get<std::string>().c_str(),
             config["tools"].size(), used_min, budget_min);
    return true;
}

void RealtimeClient::Stop() {
    if (!running_.exchange(false)) return;
    // Shutting the socket down is what unblocks the reader's recv().
    if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR);
    if (reader_.joinable()) reader_.join();
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    FinishPlayback(true);

    const double elapsed = NowSeconds() - session_started_;
    RecordSeconds(elapsed);
    const double budget_min = EnvDouble("GEMINI_LIVE_DAILY_MINUTES", 60.0);
    ESP_LOGI(TAG, "session closed sau %.0f s (%.1f/%.0f phút hôm nay)", elapsed,
             SecondsUsedToday() / 60.0, budget_min);
}

void RealtimeClient::SendAudio(const int16_t *samples, size_t n) {
    if (!running_.load() || !samples || n == 0) return;

    // No acoustic echo canceller is available on this hardware. Never send
    // speaker output back to Gemini; a level-based exception is precisely what
    // caused Nova to transcribe and interrupt her own replies.
    if (EchoActive()) return;

    SendFrame(kMsgAudioIn, samples, n * sizeof(int16_t));
}

bool RealtimeClient::EchoActive() const {
    if (assistant_audio_active_.load()) return true;
    std::lock_guard<std::mutex> lk(activity_mtx_);
    return NowSeconds() < echo_until_;
}

void RealtimeClient::FinishPlayback(bool drain) {
    const bool had_playback = playing_;
    if (out_ && playing_) {
        if (drain)
            out_->EndStream();
        else
            out_->AbortStream();
        playing_ = false;
    }

    // EndStream() blocks until ALSA has played every queued frame. Only after
    // it returns does the room-tail timer begin. Keeping the active flag set
    // during the drain closes the leak that previously fed the last words of
    // every reply back into Gemini.
    if (had_playback) {
        std::lock_guard<std::mutex> lk(activity_mtx_);
        echo_until_ = NowSeconds() + echo_tail_sec_;
    }
    assistant_audio_active_.store(false);
}

double RealtimeClient::IdleSeconds() const {
    std::lock_guard<std::mutex> lk(activity_mtx_);
    return NowSeconds() - last_activity_;
}

void RealtimeClient::MarkActivity() {
    std::lock_guard<std::mutex> lk(activity_mtx_);
    last_activity_ = NowSeconds();
}

bool RealtimeClient::SendFrame(uint8_t type, const void *payload, size_t size) {
    std::lock_guard<std::mutex> lk(send_mtx_);
    if (fd_ < 0) return false;
    uint8_t header[5];
    const uint32_t length = static_cast<uint32_t>(size);
    std::memcpy(header, &length, 4);
    header[4] = type;
    if (!WriteAll(fd_, header, sizeof(header))) return false;
    if (size > 0 && !WriteAll(fd_, payload, size)) return false;
    return true;
}

void RealtimeClient::HandleToolCall(const std::string &json_payload) {
    auto call = nlohmann::json::parse(json_payload, nullptr, false);
    if (call.is_discarded() || !call.contains("name")) return;
    const std::string id = call.value("id", "");
    const std::string name = call.value("name", "");
    const std::string args =
        call.contains("args") ? call["args"].dump() : std::string("{}");

    // On its own thread: a tool that talks to the network would otherwise stall
    // the reader, and with it the audio the user is currently listening to.
    std::thread([this, id, name, args] {
        std::string result = executor_ ? executor_(name, args)
                                       : "ERROR: khong co tool registry";
        nlohmann::json response;
        response["id"] = id;
        response["name"] = name;
        response["result"] = result;
        const std::string payload = response.dump();
        SendFrame(kMsgToolResult, payload.data(), payload.size());
    }).detach();
}

void RealtimeClient::ReaderThread() {
    while (running_.load()) {
        uint8_t header[5];
        if (!ReadAll(fd_, header, sizeof(header))) break;
        uint32_t length = 0;
        std::memcpy(&length, header, 4);
        const uint8_t type = header[4];
        if (length > 8u * 1024u * 1024u) {
            ESP_LOGE(TAG, "absurd frame length %u; dropping session", length);
            break;
        }
        std::vector<uint8_t> payload(length);
        if (length > 0 && !ReadAll(fd_, payload.data(), length)) break;

        switch (type) {
        case kMsgAudioOut: {
            MarkActivity();
            // Interrupted: swallow the rest of this turn rather than resuming a
            // sentence the user has already talked over.
            if (turn_muted_.load()) break;
            if (!out_) break;
            if (!playing_) {
                const std::string device =
                    EnvOr("JETSON_VOICE_OUT", "plughw:CARD=Lite,DEV=0");
                playing_ = out_->BeginStream(kReplySampleRate, device);
                if (!playing_) break;
                assistant_audio_active_.store(true);
            }
            const auto *pcm = reinterpret_cast<const int16_t *>(payload.data());
            const size_t count = payload.size() / sizeof(int16_t);

            // Attenuate before playing so Nova remains comfortable at the
            // close listening distance this enclosure is designed for.
            gain_buffer_.resize(count);
            for (size_t i = 0; i < count; ++i) {
                const double scaled = pcm[i] * out_gain_;
                gain_buffer_[i] = static_cast<int16_t>(
                    scaled > 32767.0 ? 32767.0
                                     : (scaled < -32768.0 ? -32768.0 : scaled));
            }

            out_->WriteStream(gain_buffer_.data(), count);
            break;
        }
        case kMsgInterrupted:
            // The user spoke over the reply. Everything already handed to ALSA
            // is stale; playing it out would be the device talking over them.
            FinishPlayback(false);
            turn_muted_.store(true);
            ESP_LOGI(TAG, "barge-in: playback dropped");
            MarkActivity();
            break;
        case kMsgToolCall:
            MarkActivity();
            HandleToolCall(std::string(payload.begin(), payload.end()));
            break;
        case kMsgTurnComplete:
            turn_muted_.store(false);
            FinishPlayback(true);
            if (!user_line_.empty()) {
                ESP_LOGI(TAG, "nghe: %s", user_line_.c_str());
                user_line_.clear();
            }
            if (!assistant_line_.empty()) {
                ESP_LOGI(TAG, "nói: %s", assistant_line_.c_str());
                assistant_line_.clear();
            }
            MarkActivity();
            if (on_turn_done_) on_turn_done_();
            break;
        case kMsgTranscript: {
            auto item = nlohmann::json::parse(
                std::string(payload.begin(), payload.end()), nullptr, false);
            if (!item.is_discarded()) {
                MarkActivity();
                // Accumulate; whole sentences are logged when the turn ends.
                const std::string piece = item.value("text", "");
                if (item.value("role", "") == "user") {
                    user_line_ += piece;
                    // Do not wait for a cloud model to choose the obvious tool
                    // while a physical alarm is sounding in the room.
                    if (jetson::StopAlarmIfRequested(user_line_)) {
                        ESP_LOGI(TAG, "alarm stopped directly from transcript");
                    }
                } else {
                    assistant_line_ += piece;
                }
            }
            break;
        }
        default:
            ESP_LOGW(TAG, "unknown frame type %#x", type);
            break;
        }
    }
    if (running_.load()) ESP_LOGW(TAG, "sidecar closed the session");
}

} // namespace jetson::audio
