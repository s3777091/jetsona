#pragma once

#include <alsa/asoundlib.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace jetson::audio {

/* ALSA microphone capture for the reSpeaker USB v3 (UAC2 sound card).
 *
 * Opens a capture stream at 16 kHz / mono / S16LE -- the exact format sherpa-onnx
 * expects (it resamples internally only if needed, but feeding it native 16k
 * avoids a resample stage on the A57). Captures `frames_per_chunk` frames per
 * read and hands each chunk to the callback on the capture thread.
 *
 * The callback runs on the capture thread, so callers must not block in it
 * for long; the voice loop copies the chunk into its own buffer and returns. */
class MicCapture {
public:
    using ChunkCb = std::function<void(const int16_t *samples, size_t n)>;

    MicCapture() = default;
    ~MicCapture();

    MicCapture(const MicCapture &) = delete;
    MicCapture &operator=(const MicCapture &) = delete;

    /* device = ALSA pcm name ("default", "plughw:CARD=ReSpeaker,DEV=0", ...).
     * Returns false (and logs) on open/param failure so the caller can fall
     * back to a different device name. */
    bool Start(const std::string &device, int sample_rate, int frames_per_chunk,
               ChunkCb cb);
    void Stop();

    bool running() const { return running_.load(); }

private:
    void Run();

    snd_pcm_t *pcm_ = nullptr;
    int sample_rate_ = 16000;
    int frames_per_chunk_ = 512;
    ChunkCb cb_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

} // namespace jetson::audio