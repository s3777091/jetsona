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

    /* Streaming playback for the realtime path, where the reply arrives in
     * pieces and has to start playing before the rest of it exists. Begin once,
     * Write each piece as it lands, End to drain what is left.
     *
     * Abort is barge-in: the user talked over the reply, so everything still
     * sitting in the ALSA buffer is stale and must be dropped rather than
     * played after they have stopped. Samples are S16LE mono, matching what
     * the model streams back. */
    bool BeginStream(int sample_rate, const std::string &device);
    bool WriteStream(const int16_t *samples, size_t n);
    void EndStream();
    void AbortStream();
    bool streaming() const;

private:
    void CloseStreamLocked(bool drain);

    std::mutex mtx_;
    std::atomic<bool> stop_{false};

    mutable std::mutex stream_mtx_;
    snd_pcm_t *stream_pcm_ = nullptr;
};

} // namespace jetson::audio