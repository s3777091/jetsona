#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace jetson::audio {

/* Abstract voice front-end: wake-word detection, speech-to-text, and
 * text-to-speech. The voice loop depends only on this interface so the sherpa
 * dep can be swapped or stubbed without touching the loop. */
struct SynthResult {
    std::vector<float> samples;  // mono, [-1, 1]
    int sample_rate = 16000;
};

class VoiceEngine {
public:
    virtual ~VoiceEngine() = default;

    virtual bool Ready() const = 0;

    /* Feed a 16 kHz mono S16LE chunk to the wake-word detector. Returns true if
     * the wake word ("Ekko") fired in this chunk; the engine re-arms itself. */
    virtual bool FeedWake(const int16_t *samples, size_t n) = 0;

    /* Recognize one complete utterance (16 kHz mono) -> Vietnamese text. */
    virtual std::string Recognize(const int16_t *samples, size_t n) = 0;

    /* Synthesize text -> audio. Returns false on failure. */
    virtual bool Synthesize(const std::string &text, SynthResult &out) = 0;
};

} // namespace jetson::audio