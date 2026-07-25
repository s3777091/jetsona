#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace jetson::audio {

/* Abstract voice front-end: neural VAD, wake-word detection, speech-to-text,
 * and text-to-speech. The voice loop depends only on this interface so the
 * sherpa dependency can be swapped without touching the loop. */
struct SynthResult {
    std::vector<float> samples;  // mono, [-1, 1]
    int sample_rate = 16000;
};

class VoiceEngine {
public:
    virtual ~VoiceEngine() = default;

    /* Load whatever the wake-word stage needs so FeedWake can fire. Returns true
     * if at least wake-word detection is usable; false leaves the loop inert. */
    virtual bool Init() = 0;

    virtual bool Ready() const = 0;

    /* Feed a 16 kHz mono S16LE chunk to the wake-word detector. Returns true if
     * the wake word ("Nova") fired in this chunk; the engine re-arms itself. */
    virtual bool FeedWake(const int16_t *samples, size_t n) = 0;

    /* Neural speech/no-speech classification for one 16 kHz chunk. */
    virtual bool VadReady() const = 0;
    virtual bool FeedVad(const int16_t *samples, size_t n) = 0;
    virtual void ResetVad() = 0;

    /* Recognize one complete utterance (16 kHz mono) -> Vietnamese text. */
    virtual std::string Recognize(const int16_t *samples, size_t n) = 0;

    /* Synthesize text -> audio. Returns false on failure. */
    virtual bool Synthesize(const std::string &text, SynthResult &out) = 0;
};

} // namespace jetson::audio
