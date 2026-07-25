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
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#define TAG "VoiceLoop"

namespace jetson::audio {

namespace {
// 512 frames @ 16 kHz = 32 ms per chunk.
constexpr int kCalibrationChunks = 313;     // ~10 s robust startup calibration
constexpr double kCalibrationRejectRms = 500.0;
constexpr int kMinOnsetChunks = 3;          // reject short fan/mechanical spikes
constexpr int kMinSpeechChunks = 2;         // 64 ms of speech to start an utterance
constexpr int kSilenceToEndChunks = 28;     // ~900 ms trailing silence -> endpoint
constexpr int kMaxUtterChunks = 375;         // 12 s hard cap
constexpr int kFalseWakeChunks = 78;         // 2.5 s with no speech -> abandon
constexpr int kVadLogChunks = 313;           // ~10 s between idle diagnostics

// Seconds to keep listening for a follow-up (no wake word needed) after a turn.
constexpr int kAwakeWindowSec = 12;

// Wake phrases matched case-insensitively in the STT transcript. "hey lewa" is
// not a guess: it is the exact output observed from the previously deployed
// multilingual model for a spoken "Hey Nova". Keep it during the Vietnamese
// model migration so the wake path remains tolerant of pronunciation.
const char *const kWakePhrases[] = {
    "hey nova", "hey no va", "hey nô va", "hey nowa",
    "hey lewa", "hey leva", "hây nô va", "hãy nô va",
    "ê nova", "ê nô va", "nova", "no va", "nô va"
};

// ALSA device: env (config.yaml) wins, then Settings("voice"), then "default".
std::string AudioDevice(const char *env_key, const char *settings_key) {
    if (const char *e = std::getenv(env_key); e && *e) return e;
    return Settings("voice", false).GetString(settings_key, "default");
}

// Lower-case ASCII A-Z only; multi-byte UTF-8 (Vietnamese diacritics) is left
// untouched, so byte offsets stay aligned with the original string.
std::string LowerAscii(const std::string &s) {
    std::string o = s;
    for (char &c : o)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return o;
}

std::string TrimStr(const std::string &s) {
    auto sp = s.find_first_not_of(" \t\r\n");
    if (sp == std::string::npos) return "";
    auto ep = s.find_last_not_of(" \t\r\n");
    return s.substr(sp, ep - sp + 1);
}

bool IsAsciiWordByte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

bool HasWordBoundaries(const std::string &text, size_t pos, size_t len) {
    const bool left_ok =
        pos == 0 || !IsAsciiWordByte(static_cast<unsigned char>(text[pos - 1]));
    const size_t end = pos + len;
    const bool right_ok =
        end >= text.size() ||
        !IsAsciiWordByte(static_cast<unsigned char>(text[end]));
    return left_ok && right_ok;
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

    Settings voice("voice", false);
    vad_min_rms_ = std::max(
        10.0, std::min(2000.0, static_cast<double>(
            voice.GetFloat("vad_min_rms", 38.0f))));
    vad_noise_multiplier_ = std::max(
        1.1, std::min(8.0, static_cast<double>(
            voice.GetFloat("vad_noise_multiplier", 1.6f))));
    vad_noise_margin_ = std::max(
        0.0, std::min(1000.0, static_cast<double>(
            voice.GetFloat("vad_noise_margin", 8.0f))));
    const int pre_roll_ms = std::max(
        0, std::min(1000, voice.GetInt("vad_pre_roll_ms", 640)));
    pre_roll_samples_ = static_cast<size_t>(pre_roll_ms) * 16;
    calibration_rms_.clear();
    calibration_rms_.reserve(kCalibrationChunks);
    ESP_LOGI(TAG,
             "adaptive VAD (min=%.1f, noise x%.2f +%.1f, calibrate=%d ms, pre-roll=%d ms)",
             vad_min_rms_, vad_noise_multiplier_, vad_noise_margin_,
             kCalibrationChunks * 32, pre_roll_ms);

    running_.store(true);
    speaker_thread_ = std::thread(&VoiceLoop::SpeakerThread, this);
    jetson::platform::SetThreadName(speaker_thread_, "ekko-speak");

    mic_ = std::make_unique<MicCapture>();
    std::string mic_dev = AudioDevice("JETSON_VOICE_MIC", "mic_device");
    const bool is_respeaker_lite =
        mic_dev.find("CARD=Lite") != std::string::npos ||
        mic_dev.find("ReSpeaker Lite") != std::string::npos;
    const int mic_channels = std::max(
        1, std::min(8, voice.GetInt("mic_channels",
                                    is_respeaker_lite ? 2 : 1)));
    const int mic_channel = std::max(
        0, std::min(mic_channels - 1,
                    voice.GetInt("mic_channel", 0)));
    bool ok = mic_->Start(mic_dev, 16000, 512, mic_channels, mic_channel,
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
    // the device's own TTS output must not be transcribed back as a command, and
    // we don't want to collect a second utterance mid-turn.
    if (speaking_.load()) return;

    std::lock_guard<std::mutex> lk(mtx_);
    if (state_ == kBusy) return;

    // Energy of this chunk.
    double sumsq = 0;
    for (size_t i = 0; i < n; ++i) sumsq += static_cast<double>(samples[i]) * samples[i];
    const double rms = std::sqrt(sumsq / std::max<size_t>(1, n));

    // The tiny English KWS model listens continuously and fires before the
    // slower utterance recognizers. It is the primary path for "Hey Nova".
    // Clear any VAD capture already in progress because the wake phrase itself
    // is not a command; the next utterance is handled inside the awake window.
    if (engine_ && engine_->FeedWake(samples, n)) {
        ESP_LOGI(TAG, "wake matched (KWS)");
        engine_->ResetVad();
        utter_.clear();
        pre_roll_.clear();
        speech_chunks_ = silence_chunks_ = total_chunks_ = 0;
        onset_chunks_ = 0;
        state_ = kIdle;
        awake_until_ = std::chrono::steady_clock::now() +
                       std::chrono::seconds(kAwakeWindowSec);
        // Close the capture/speaker hand-off race: without this early flag, a
        // new utterance can open while the speaker thread is waking up.
        speaking_.store(true);
        Speak("Dạ, Nova nghe đây.");
        return;
    }

    const bool neural_vad = engine_ && engine_->VadReady();
    const bool neural_speech =
        neural_vad && engine_->FeedVad(samples, n);

    if (state_ == kIdle) {
        // Preserve audio before onset so a quiet initial "Hey" is not lost.
        pre_roll_.insert(pre_roll_.end(), samples, samples + n);
        while (pre_roll_.size() > pre_roll_samples_) pre_roll_.pop_front();

        if (neural_vad) {
            idle_peak_rms_ = std::max(idle_peak_rms_, rms);
            idle_log_chunks_++;
            if (neural_speech) {
                onset_chunks_++;
                if (onset_chunks_ >= 2) {
                    state_ = kCollecting;
                    utter_.assign(pre_roll_.begin(), pre_roll_.end());
                    pre_roll_.clear();
                    speech_chunks_ = onset_chunks_;
                    silence_chunks_ = 0;
                    total_chunks_ = onset_chunks_;
                    onset_chunks_ = 0;
                    ESP_LOGI(TAG,
                             "speech onset (Silero VAD, rms=%.1f)",
                             rms);
                }
            } else {
                onset_chunks_ = 0;
                if (idle_log_chunks_ >= kVadLogChunks) {
                    ESP_LOGI(TAG,
                             "VAD idle (Silero, peak-rms=%.1f)",
                             idle_peak_rms_);
                    idle_log_chunks_ = 0;
                    idle_peak_rms_ = 0.0;
                }
            }
            return;
        }

        // Keep several seconds of samples and use their lower percentile. This
        // measures continuous fan noise correctly, while ignoring chunks where
        // somebody starts talking before startup calibration has finished.
        if (calibration_chunks_ < kCalibrationChunks) {
            calibration_rms_.push_back(rms);
            calibration_chunks_++;
            if (calibration_chunks_ == kCalibrationChunks) {
                std::sort(calibration_rms_.begin(), calibration_rms_.end());
                const size_t low_index = std::min(
                    calibration_rms_.size() - 1,
                    static_cast<size_t>(
                        (calibration_rms_.size() - 1) * 0.20));
                const size_t median_index = calibration_rms_.size() / 2;
                const size_t high_index = std::min(
                    calibration_rms_.size() - 1,
                    static_cast<size_t>(
                        (calibration_rms_.size() - 1) * 0.60));
                const double low = calibration_rms_[low_index];
                const double median =
                    calibration_rms_[calibration_rms_.size() / 2];
                const double high = calibration_rms_[high_index];
                // Reject a window with no quiet portion at all (for example a
                // startup sound or somebody talking through all 10 seconds).
                // KWS keeps listening while the next window is collected.
                if (low > kCalibrationRejectRms) {
                    ESP_LOGW(TAG,
                             "VAD calibration contaminated (p20=%.1f); retrying",
                             low);
                    calibration_rms_.clear();
                    calibration_chunks_ = 0;
                    pre_roll_.clear();
                    return;
                }
                // p60 is high enough to include the continuous fan but remains
                // below a short spoken phrase in a ten-second window.
                noise_rms_ = calibration_rms_[median_index];
                noise_high_rms_ = high;
                const double fan_guard = high * 1.90 + 20.0;
                ESP_LOGI(TAG,
                         "VAD calibrated (p20=%.1f, median=%.1f, p60=%.1f, threshold=%.1f)",
                         low, median, high,
                         std::max(
                             fan_guard,
                             std::max(vad_min_rms_,
                                      noise_rms_ * vad_noise_multiplier_ +
                                          vad_noise_margin_)));
                calibration_rms_.clear();
                calibration_rms_.shrink_to_fit();
                pre_roll_.clear();
            }
            return;
        }

        const bool awake =
            std::chrono::steady_clock::now() < awake_until_;
        // The upper quiet-band statistic captures the fan's broadband bursts,
        // which its median alone misses. Keep a stronger guard while asleep;
        // after a real KWS wake, relax it slightly for a soft follow-up.
        const double fan_guard =
            noise_high_rms_ * (awake ? 1.70 : 1.90) +
            (awake ? 15.0 : 20.0);
        const double threshold = std::max(
            fan_guard,
            std::max(vad_min_rms_,
                     noise_rms_ * vad_noise_multiplier_ + vad_noise_margin_));
        idle_peak_rms_ = std::max(idle_peak_rms_, rms);
        idle_log_chunks_++;

        // A fan can produce isolated 2-10 ms mechanical spikes far above its
        // steady RMS. Require sustained energy for ~96 ms before opening the
        // command utterance; pre-roll still preserves its first syllable.
        if (rms > threshold) {
            onset_chunks_++;
            if (onset_chunks_ >= kMinOnsetChunks) {
                state_ = kCollecting;
                utter_.assign(pre_roll_.begin(), pre_roll_.end());
                pre_roll_.clear();
                speech_chunks_ = onset_chunks_;
                silence_chunks_ = 0;
                total_chunks_ = onset_chunks_;
                onset_chunks_ = 0;
                active_threshold_ = threshold;
                ESP_LOGI(TAG,
                         "speech onset sustained (rms=%.1f, threshold=%.1f, noise=%.1f)",
                         rms, threshold, noise_rms_);
            }
        } else {
            onset_chunks_ = 0;
            // Track the stationary noise floor slowly without letting a speech
            // onset immediately drag the threshold upward.
            noise_rms_ = noise_rms_ * 0.99 + rms * 0.01;
            if (idle_log_chunks_ >= kVadLogChunks) {
                ESP_LOGI(TAG, "VAD idle (noise=%.1f, threshold=%.1f, peak=%.1f)",
                         noise_rms_, threshold, idle_peak_rms_);
                idle_log_chunks_ = 0;
                idle_peak_rms_ = 0.0;
            }
        }
        return;
    }

    // kCollecting: append + energy VAD.
    utter_.insert(utter_.end(), samples, samples + n);
    total_chunks_++;
    // A little hysteresis keeps soft word endings inside the utterance.
    // Once the utterance has opened, compare against the calibrated room
    // floor instead of the higher onset threshold. This keeps quiet syllables
    // such as the middle of "Hey Nova" from being mistaken for silence.
    const double continue_threshold =
        std::max(noise_rms_ * 1.35 + 6.0, active_threshold_ * 0.80);
    if (neural_vad ? neural_speech : rms > continue_threshold) {
        speech_chunks_++;
        silence_chunks_ = 0;
    } else {
        silence_chunks_++;
    }

    // Onset turned out to be a transient (no sustained speech) -> abandon.
    if (total_chunks_ >= kFalseWakeChunks && speech_chunks_ < kMinSpeechChunks) {
        state_ = kIdle;
        onset_chunks_ = 0;
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

bool VoiceLoop::MatchWake(const std::string &text, std::string &cmd) {
    const std::string low = LowerAscii(text);
    size_t pos = std::string::npos, wlen = 0;
    for (const char *w : kWakePhrases) {
        const size_t len = std::string(w).size();
        size_t search_from = low.size();
        while (search_from != std::string::npos) {
            const size_t p = low.rfind(w, search_from);
            if (p == std::string::npos) break;
            if (HasWordBoundaries(low, p, len)) {
                if (pos == std::string::npos || p > pos ||
                    (p == pos && len > wlen)) {
                    pos = p;
                    wlen = len;
                }
                break;
            }
            if (p == 0) break;
            search_from = p - 1;
        }
    }
    if (pos != std::string::npos) {
        cmd = TrimStr(text.substr(pos + wlen));   // command follows the wake word
        return true;
    }
    // No wake word: act only if we're still awake from a recent turn.
    std::lock_guard<std::mutex> lk(mtx_);
    if (std::chrono::steady_clock::now() < awake_until_) {
        cmd = TrimStr(text);
        return true;
    }
    return false;
}

void VoiceLoop::RecognizeAndSend(const std::vector<int16_t> &utterance) {
    auto go_idle = [this] {
        std::lock_guard<std::mutex> lk(mtx_);
        onset_chunks_ = 0;
        state_ = kIdle;
    };
    // The tiny English KWS is the fast path, but a Vietnamese accent can miss
    // its English acoustic model. Decode sustained speech with the Vietnamese
    // recognizer as a fallback and require an explicit wake phrase unless KWS
    // already opened the awake window. Ambient speech therefore cannot send a
    // command merely because VAD fired.
    std::string text;
    if (engine_) text = engine_->Recognize(utterance.data(), utterance.size());
    if (text.empty()) { go_idle(); return; }
    ESP_LOGI(TAG, "heard: %s", text.c_str());

    std::string cmd;
    if (!MatchWake(text, cmd)) {
        go_idle();
        return;
    }

    if (cmd.empty()) {
        // Bare "nova": greet and stay awake for the follow-up command.
        speaking_.store(true);
        Speak("Dạ, Nova nghe đây.");
        std::lock_guard<std::mutex> lk(mtx_);
        awake_until_ = std::chrono::steady_clock::now() +
                       std::chrono::seconds(kAwakeWindowSec);
        onset_chunks_ = 0;
        state_ = kIdle;
        return;
    }

    auto *conv = Application::GetInstance().GetConversation();
    if (!conv) { go_idle(); return; }
    conv->Send(cmd, [this](std::string reply, std::string err) {
        if (!err.empty()) ESP_LOGW(TAG, "agent error: %s", err.c_str());
        if (!reply.empty()) {
            speaking_.store(true);
            Speak(reply);
        }
        std::lock_guard<std::mutex> lk(mtx_);
        awake_until_ = std::chrono::steady_clock::now() +
                       std::chrono::seconds(kAwakeWindowSec);
        onset_chunks_ = 0;
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
            if (out_->Play(res.samples.data(), res.samples.size(), res.sample_rate,
                           out_dev)) {
                ESP_LOGI(TAG, "spoke: %s", text.c_str());
            } else {
                ESP_LOGE(TAG, "speaker playback failed");
            }
        } else {
            // No TTS available: surface the text in the log so it isn't lost.
            ESP_LOGW(TAG, "(no TTS) %s", text.c_str());
        }
        speaking_.store(false);
    }
}

} // namespace jetson::audio
