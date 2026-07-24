#pragma once

#include <alsa/asoundlib.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace jetson::audio {

/* ALSA playback for TTS / ringtone-beep output on the reSpeaker USB v3 speaker.
 *
 * Plays float samples in [-1, 1] at the given sample rate through `plughw:...`
 * so ALSA resamples when the device default rate differs (Piper vi outputs
 * 16 kHz; the reSpeaker may run at 48 kHz). One stream at a time; Stop()
 * interrupts the current playback (used so a new utterance / alarm can cut in). */
class AudioOutput {
public:
    AudioOutput() = default;
    ~AudioOutput();

    AudioOutput(const AudioOutput &) = delete;
    AudioOutput &operator=(const AudioOutput &) = delete;

    /* Blocking: opens the device, plays all samples, closes. Returns false on
     * open failure. Called on the speaker thread, never the main loop. */
    bool Play(const float *samples, size_t n, int sample_rate,
              const std::string &device);

    /* Interrupts the in-flight Play() if any. Thread-safe. */
    void Stop() { stop_.store(true); }

private:
    std::mutex mtx_;
    std::atomic<bool> stop_{false};
};

} // namespace jetson::audio