#pragma once

#include "audio/voice_engine.h"

namespace jetson::audio {

/* sherpa-onnx (v1.10.30) backed VoiceEngine for the Jetson Nano + reSpeaker.
 *
 * All three sub-models are config-driven: paths come from Settings("voice")
 * (with sensible defaults under JETSON_ASSETS_DIR/models/{kws,stt,tts}/), so a
 * model swap is a config change, not a rebuild. The execution provider is
 * configurable per sub-model and defaults to CPU for JetPack compatibility. */
class SherpaVoiceEngine : public VoiceEngine {
public:
    SherpaVoiceEngine() = default;
    ~SherpaVoiceEngine() override;

    /* Lazily constructs the KWS / STT / TTS handles from Settings on first use,
     * so a missing/broken model only disables that one stage instead of
     * crashing boot. Returns true once STT is ready. */
    bool Ready() const override;
    bool Init() override;

    bool FeedWake(const int16_t *samples, size_t n) override;
    std::string Recognize(const int16_t *samples, size_t n) override;
    bool Synthesize(const std::string &text, SynthResult &out) override;

private:
    bool EnsureKws();
    bool EnsureStt();
    bool EnsureTts();

    // sherpa-onnx opaque handles (void* so the C API header stays out of the
    // public header; cast back in the .cc).
    void *kws_ = nullptr;     // SherpaOnnxKeywordSpotter*
    void *kws_stream_ = nullptr; // SherpaOnnxOnlineStream*
    const void *stt_ = nullptr; // SherpaOnnxOfflineRecognizer* (API returns const)
    void *tts_ = nullptr;     // SherpaOnnxOfflineTts*
    bool kws_tried_ = false;
    bool stt_tried_ = false;
    bool tts_tried_ = false;
};

} // namespace jetson::audio
