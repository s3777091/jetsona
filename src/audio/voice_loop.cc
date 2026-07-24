#include "audio/voice_loop.h"

#include "audio/audio_output.h"
#include "audio/mic_capture.h"
#include "audio/sherpa_engine.h"
#include "audio/voice_engine.h"
#include "app/application.h"
#include "agent/conversation.h"
#include "esp_log.h"
#include "platform/thread_affinity.h"
#include "settings.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <utility>
#include <vector>

#define TAG "VoiceLoop"

namespace jetson::audio {

namespace {
// VAD / endpointing tuned for the reSpeaker USB v3 (onboard AGC/NS keeps the
// noise floor low, so a modest RMS threshold cleanly separates speech). 512
// frames @ 16 kHz = 32 ms per chunk.
constexpr double kRmsThreshold = 250.0;      // int16 RMS above this = speech
constexpr int kMinSpeechChunks = 2;         // 64 ms of speech to start an utterance
constexpr int kSilenceToEndChunks = 20;     // ~640 ms trailing silence -> endpoint
constexpr int kMaxUtterChunks = 375;         // 12 s hard cap
constexpr int kFalseWakeChunks = 78;         // 2.5 s with no speech -> abandon

// ALSA device: env (config.yaml) wins, then Settings("voice"), then "default".
std::string AudioDevice(const char *env_key, const char *settings_key) {
    if (const char *e = std::getenv(env_key); e && *e) return e;
    return Settings("voice", false).GetString(settings_key, "default");
}
} // namespace

VoiceLoop::VoiceLoop() = default;
VoiceLoop::~VoiceLoop() { Stop(); }

VoiceLoop &VoiceLoop::Instance() {
    static VoiceLoop inst;
    return inst;
}

bool VoiceLoop::Start() {
    if (running_.load()) return true;
    engine_ = std::make_unique<SherpaVoiceEngine>();
    if (!engine_->Init())
        ESP_LOGW(TAG, "voice engine not ready (models missing?) -- loop inert until models sync");

    out_ = std::make_unique<AudioOutput>();
    running_.store(true);
    speaker_thread_ = std::thread(&VoiceLoop::SpeakerThread, this);
    jetson::platform::SetThreadName(speaker_thread_, "ekko-speak");

    mic_ = std::make_unique<MicCapture>();
    std::string mic_dev = AudioDevice("JETSON_VOICE_MIC", "mic_device");
    bool ok = mic_->Start(mic_dev, 16000, 512,
                          [this](const int16_t *s, size_t n) { OnMicChunk(s, n); });
    if (!ok) {
        ESP_LOGE(TAG, "mic open failed on '%s' -- voice loop inert (agent still usable)",
                 mic_dev.c_str());
    } else {
        ESP_LOGI(TAG, "voice loop started (mic=%s)", mic_dev.c_str());
    }
    return ok;
}

void VoiceLoop::Stop() {
    if (!running_.exchange(false)) return;
    speech_cv_.notify_all();
    if (mic_) mic_->Stop();
    if (speaker_thread_.joinable()) speaker_thread_.join();
}

void VoiceLoop::Speak(const std::string &text) {
    if (!running_.load() || text.empty()) return;
    {
        std::lock_guard<std::mutex> lk(speech_mtx_);
        speech_.push(text);
    }
    speech_cv_.notify_one();
}

void VoiceLoop::OnMicChunk(const int16_t *samples, size_t n) {
    if (!running_.load()) return;
    // While the speaker is active (or a turn is in flight) we ignore the mic:
    // the device's own TTS output must not be heard back as a wake word, and
    // we don't want to collect a second utterance mid-turn.
    if (speaking_.load()) return;

    std::lock_guard<std::mutex> lk(mtx_);
    if (state_ == kBusy) return;

    if (state_ == kIdle) {
        if (engine_ && engine_->FeedWake(samples, n)) {
            state_ = kCollecting;
            utter_.clear();
            speech_chunks_ = silence_chunks_ = total_chunks_ = 0;
            ESP_LOGI(TAG, "wake word detected, collecting utterance");
        }
        return;
    }

    // kCollecting: append + energy VAD.
    utter_.insert(utter_.end(), samples, samples + n);
    total_chunks_++;
    double sumsq = 0;
    for (size_t i = 0; i < n; ++i) sumsq += static_cast<double>(samples[i]) * samples[i];
    const double rms = std::sqrt(sumsq / std::max<size_t>(1, n));
    if (rms > kRmsThreshold) {
        speech_chunks_++;
        silence_chunks_ = 0;
    } else {
        silence_chunks_++;
    }

    // No speech shortly after the wake word -> false wake; re-arm KWS.
    if (total_chunks_ >= kFalseWakeChunks && speech_chunks_ < kMinSpeechChunks) {
        state_ = kIdle;
        utter_.clear();
        return;
    }

    const bool endpoint =
        (speech_chunks_ >= kMinSpeechChunks && silence_chunks_ >= kSilenceToEndChunks) ||
        total_chunks_ >= kMaxUtterChunks;
    if (!endpoint) return;

    state_ = kBusy;
    std::vector<int16_t> u = std::move(utter_);
    utter_.clear();
    // Recognize + send off the capture thread so mic capture never stalls.
    std::thread(&VoiceLoop::RecognizeAndSend, this, std::move(u)).detach();
}

void VoiceLoop::RecognizeAndSend(const std::vector<int16_t> &utterance) {
    std::string text;
    if (engine_) text = engine_->Recognize(utterance.data(), utterance.size());
    if (text.empty()) {
        ESP_LOGW(TAG, "empty transcription, ignoring");
        std::lock_guard<std::mutex> lk(mtx_);
        state_ = kIdle;
        return;
    }
    ESP_LOGI(TAG, "heard: %s", text.c_str());

    auto *conv = Application::GetInstance().GetConversation();
    if (!conv) {
        std::lock_guard<std::mutex> lk(mtx_);
        state_ = kIdle;
        return;
    }
    conv->Send(text, [this](std::string reply, std::string err) {
        if (!err.empty()) ESP_LOGW(TAG, "agent error: %s", err.c_str());
        if (!reply.empty()) Speak(reply);
        std::lock_guard<std::mutex> lk(mtx_);
        state_ = kIdle;
    });
}

void VoiceLoop::SpeakerThread() {
    const std::string out_dev = AudioDevice("JETSON_VOICE_OUT", "out_device");
    while (running_.load()) {
        std::unique_lock<std::mutex> lk(speech_mtx_);
        speech_cv_.wait(lk, [this] { return !running_.load() || !speech_.empty(); });
        if (!running_.load()) break;
        std::string text = speech_.front();
        speech_.pop();
        lk.unlock();

        speaking_.store(true);
        SynthResult res;
        if (engine_ && engine_->Synthesize(text, res) && !res.samples.empty()) {
            out_->Play(res.samples.data(), res.samples.size(), res.sample_rate, out_dev);
        } else {
            // No TTS available: surface the text in the log so it isn't lost.
            ESP_LOGW(TAG, "(no TTS) %s", text.c_str());
        }
        speaking_.store(false);
    }
}

} // namespace jetson::audio