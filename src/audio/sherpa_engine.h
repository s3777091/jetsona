#pragma once

#include "audio/voice_engine.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace jetson::audio {

/* sherpa-onnx (v1.10.30) backed VoiceEngine for the Jetson Nano + reSpeaker.
 *
 * Only Silero VAD still runs in-process here. Wake detection moved to a
 * network-disabled openWakeWord sidecar, transcription to OpenAI, and synthesis
 * to Edge TTS. What remains is config-driven: paths come from Settings("voice")
 * with defaults under JETSON_ASSETS_DIR/models/, so a model swap is a config
 * change, not a rebuild, and the execution provider defaults to CPU for JetPack
 * compatibility. */
class SherpaVoiceEngine : public VoiceEngine {
public:
    SherpaVoiceEngine() = default;
    ~SherpaVoiceEngine() override;

    /* Lazily constructs the remaining model handles from Settings on first use,
     * so a missing/broken model only disables that one stage instead of
     * crashing boot. Returns true once STT is ready. */
    bool Ready() const override;
    bool Init() override;

    bool FeedWake(const int16_t *samples, size_t n) override;
    bool VadReady() const override;
    bool FeedVad(const int16_t *samples, size_t n) override;
    void ResetVad() override;
    std::string Recognize(const int16_t *samples, size_t n) override;
    bool Synthesize(const std::string &text, SynthResult &out) override;

private:
    bool EnsureKws();
    bool EnsureVad();
    bool EnsureStt();

    // sherpa-onnx opaque handles (void* so the C API header stays out of the
    // public header; cast back in the .cc).
    void *kws_ = nullptr;     // SherpaOnnxKeywordSpotter*
    void *kws_stream_ = nullptr; // SherpaOnnxOnlineStream*
    void *vad_ = nullptr;     // SherpaOnnxVoiceActivityDetector*
    const void *stt_ = nullptr; // SherpaOnnxOfflineRecognizer* (API returns const)

    // openWakeWord runs in a network-disabled sidecar. Audio is accumulated
    // into its native 80 ms / 1280-sample frames and exchanged over a local
    // Unix socket. The old sherpa KWS handles above remain only so older model
    // settings can still be destroyed safely during a rolling upgrade.
    int oww_fd_ = -1;
    std::vector<int16_t> oww_audio_;
    int oww_score_frames_ = 0;
    bool oww_connected_logged_ = false;

    // Rendered audio for short fixed phrases (wake acknowledgements, "let me
    // check", the fallback line). Edge TTS costs about three seconds a call,
    // which these repeat-heavy lines cannot afford. Guarded because Synthesize
    // runs on the speaker thread while Speak can be called from anywhere.
    std::unordered_map<std::string, SynthResult> tts_cache_;
    std::mutex tts_cache_mutex_;

    bool kws_tried_ = false;
    bool vad_tried_ = false;
    bool stt_tried_ = false;
};

} // namespace jetson::audio
