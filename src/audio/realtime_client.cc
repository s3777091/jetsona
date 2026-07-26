#include "audio/realtime_client.h"

#include "audio/audio_output.h"
#include "esp_log.h"
#include "settings.h"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
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

/* How long after the last write the speaker is still audible: the ALSA buffer
 * plus the room. Slightly generous, because letting one echo frame through is
 * worse than swallowing one frame of the user. */
constexpr double kEchoTailSec = 0.45;

// Consecutive loud chunks (32 ms each) before the reply is cut short. Long
// enough that a cough or a door does not stop Nova mid-sentence.
constexpr int kBargeInChunks = 5;

double RmsOf(const int16_t *samples, size_t n) {
    if (!samples || n == 0) return 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i)
        sum += static_cast<double>(samples[i]) * samples[i];
    return std::sqrt(sum / static_cast<double>(n));
}

double NowSeconds() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
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
    echo_coupling_ = std::max(0.1, EnvDouble("JETSON_ECHO_COUPLING", 1.2));
    echo_margin_ = std::max(1.1, EnvDouble("JETSON_ECHO_MARGIN", 1.6));
    echo_rms_.store(0.0);
    executor_ = std::move(executor);
    on_turn_done_ = std::move(on_turn_done);
    playing_ = false;
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

    reader_ = std::thread(&RealtimeClient::ReaderThread, this);
    ESP_LOGI(TAG, "session open (voice=%s, %zu tools)",
             config["voice"].get<std::string>().c_str(),
             config["tools"].size());
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
    if (out_ && playing_) {
        out_->EndStream();
        playing_ = false;
    }
    ESP_LOGI(TAG, "session closed");
}

void RealtimeClient::SendAudio(const int16_t *samples, size_t n) {
    if (!running_.load() || !samples || n == 0) return;

    /* While Nova is audible the microphone mostly hears Nova, and this board
     * has no canceller to remove her. What must not happen is forwarding some
     * frames and dropping others: an earlier attempt at that fed the model a
     * chopped stream, which it transcribed as fragments of Thai and then acted
     * on -- it reached for pc_power because a burst of echo looked like a
     * request. Send a whole stream or none of it.
     *
     * So: stay silent while she speaks, and watch the level. Speech close to
     * the microphone beats the echo, and when it does for long enough this
     * stops the playback outright -- which both answers the interruption and
     * removes the echo, so everything forwarded from then on is clean. */
    if (EchoActive()) {
        if (RmsOf(samples, n) < echo_rms_.load() * echo_margin_) {
            barge_chunks_ = 0;
            return;
        }
        if (++barge_chunks_ < kBargeInChunks) return;
        barge_chunks_ = 0;
        ESP_LOGI(TAG, "barge-in: user spoke over the reply; stopping playback");
        if (out_) out_->AbortStream();
        playing_ = false;
        echo_rms_.store(0.0);
        // Fall through: from here the room is quiet and the user has the floor.
    }

    SendFrame(kMsgAudioIn, samples, n * sizeof(int16_t));
}

bool RealtimeClient::EchoActive() const {
    std::lock_guard<std::mutex> lk(activity_mtx_);
    return NowSeconds() < echo_until_ && echo_rms_.load() > 1.0;
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
            if (!out_) break;
            if (!playing_) {
                const std::string device =
                    EnvOr("JETSON_VOICE_OUT", "plughw:CARD=Lite,DEV=0");
                playing_ = out_->BeginStream(kReplySampleRate, device);
                if (!playing_) break;
            }
            const auto *pcm = reinterpret_cast<const int16_t *>(payload.data());
            const size_t count = payload.size() / sizeof(int16_t);

            // Attenuate before playing: at full level the echo swamps anything
            // the user could say over it, and barge-in stops being possible.
            gain_buffer_.resize(count);
            for (size_t i = 0; i < count; ++i) {
                const double scaled = pcm[i] * out_gain_;
                gain_buffer_[i] = static_cast<int16_t>(
                    scaled > 32767.0 ? 32767.0
                                     : (scaled < -32768.0 ? -32768.0 : scaled));
            }

            /* What the microphone is about to hear. Peak-hold with a deadline
             * rather than a per-write decay: the audio handed to ALSA does not
             * reach the speaker for another buffer's worth of time, so a decay
             * applied per chunk had collapsed the estimate to near zero by the
             * moment the echo actually arrived -- which is how the gate came to
             * pass everything it was meant to block. */
            const double expected = RmsOf(gain_buffer_.data(), count) *
                                    echo_coupling_;
            echo_rms_.store(std::max(expected, echo_rms_.load()));
            {
                std::lock_guard<std::mutex> lk(activity_mtx_);
                echo_until_ = NowSeconds() + kEchoTailSec;
            }

            out_->WriteStream(gain_buffer_.data(), count);
            break;
        }
        case kMsgInterrupted:
            echo_rms_.store(0.0);
            // The user spoke over the reply. Everything already handed to ALSA
            // is stale; playing it out would be the device talking over them.
            if (out_ && playing_) {
                out_->AbortStream();
                playing_ = false;
            }
            ESP_LOGI(TAG, "barge-in: playback dropped");
            MarkActivity();
            break;
        case kMsgToolCall:
            MarkActivity();
            HandleToolCall(std::string(payload.begin(), payload.end()));
            break;
        case kMsgTurnComplete:
            echo_rms_.store(0.0);
            if (out_ && playing_) {
                out_->EndStream();
                playing_ = false;
            }
            MarkActivity();
            if (on_turn_done_) on_turn_done_();
            break;
        case kMsgTranscript: {
            auto item = nlohmann::json::parse(
                std::string(payload.begin(), payload.end()), nullptr, false);
            if (!item.is_discarded()) {
                MarkActivity();
                ESP_LOGI(TAG, "%s: %s", item.value("role", "?").c_str(),
                         item.value("text", "").c_str());
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
