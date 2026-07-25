#include "audio/mic_capture.h"

#include "esp_log.h"
#include "platform/thread_affinity.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#define TAG "MicCapture"

namespace jetson::audio {

MicCapture::~MicCapture() { Stop(); }

bool MicCapture::Start(const std::string &device, int sample_rate,
                       int frames_per_chunk, int capture_channels,
                       int selected_channel, ChunkCb cb) {
    if (running_.load()) return true;
    sample_rate_ = sample_rate > 0 ? sample_rate : 16000;
    frames_per_chunk_ = frames_per_chunk > 0 ? frames_per_chunk : 512;
    capture_channels_ = std::max(1, std::min(8, capture_channels));
    selected_channel_ =
        std::max(0, std::min(capture_channels_ - 1, selected_channel));
    cb_ = std::move(cb);
    device_ = device;

    if (!OpenDevice(true)) return false;

    running_.store(true);
    thread_ = std::thread(&MicCapture::Run, this);
    // Pin capture to core 0: KWS runs here and must keep low jitter while the
    // agent / STT occupy the other A57 cores. Best-effort on Linux only.
    jetson::platform::SetThreadName(thread_, "ekko-mic");
    if (!jetson::platform::PinToCore(thread_, 0))
        ESP_LOGW(TAG, "could not pin capture to core 0");
    return true;
}

bool MicCapture::OpenDevice(bool log_success) {
    CloseDevice();

    int err = snd_pcm_open(&pcm_, device_.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        ESP_LOGE(TAG, "cannot open capture device '%s': %s", device_.c_str(),
                 snd_strerror(err));
        pcm_ = nullptr;
        return false;
    }

    snd_pcm_hw_params_t *hw = nullptr;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm_, hw);
    snd_pcm_hw_params_set_access(pcm_, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm_, hw, SND_PCM_FORMAT_S16_LE);
    unsigned rate = static_cast<unsigned>(sample_rate_);
    snd_pcm_hw_params_set_rate_near(pcm_, hw, &rate, nullptr);
    unsigned channels = static_cast<unsigned>(capture_channels_);
    snd_pcm_hw_params_set_channels(pcm_, hw, channels);
    // ~200 ms latency buffer is plenty for a wake-word + utterance loop and
    // keeps underruns rare on a busy A57.
    unsigned latency_us = 200000;
    snd_pcm_hw_params_set_buffer_time_near(pcm_, hw, &latency_us, nullptr);
    err = snd_pcm_hw_params(pcm_, hw);
    if (err < 0) {
        ESP_LOGE(TAG, "hw_params failed on '%s': %s", device_.c_str(),
                 snd_strerror(err));
        CloseDevice();
        return false;
    }

    snd_pcm_prepare(pcm_);
    if (log_success) {
        ESP_LOGI(TAG,
                 "capturing %u Hz S16LE from '%s' (%d channels, selecting channel %d, %d frames/chunk)",
                 rate, device_.c_str(), capture_channels_, selected_channel_,
                 frames_per_chunk_);
    }
    return true;
}

void MicCapture::CloseDevice() {
    if (!pcm_) return;
    snd_pcm_close(pcm_);
    pcm_ = nullptr;
}

void MicCapture::Stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    if (pcm_) snd_pcm_drain(pcm_);
    CloseDevice();
}

void MicCapture::Run() {
    std::vector<int16_t> interleaved(
        static_cast<size_t>(frames_per_chunk_) * capture_channels_);
    std::vector<int16_t> mono(frames_per_chunk_);
    int reopen_attempts = 0;
    while (running_.load()) {
        if (!pcm_) {
            // Back off between reopen attempts: a re-enumerating USB mic needs
            // a moment before its new device node accepts an open.
            const int delay_ms = std::min(5000, 250 * (1 << std::min(4, reopen_attempts)));
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            if (!running_.load()) break;
            if (OpenDevice(reopen_attempts == 0)) {
                ESP_LOGI(TAG, "capture device reopened after %d attempt(s)",
                         reopen_attempts + 1);
                reopen_attempts = 0;
            } else {
                ++reopen_attempts;
            }
            continue;
        }

        snd_pcm_sframes_t n =
            snd_pcm_readi(pcm_, interleaved.data(), frames_per_chunk_);
        if (n < 0) {
            // EPIPE = overrun; recover and keep going rather than dropping the
            // whole capture session on a transient scheduler hiccup.
            if (n == -EPIPE) {
                snd_pcm_prepare(pcm_);
                continue;
            }
            const int rc = snd_pcm_recover(pcm_, (int)n, 1);
            if (rc == 0) continue;
            // -EBADFD / -ENODEV survive snd_pcm_recover: the handle refers to a
            // device that is gone (USB re-enumeration). Retrying the read would
            // spin at full speed forever, so drop the handle and reopen.
            ESP_LOGW(TAG, "readi failed unrecoverably (%s); reopening '%s'",
                     snd_strerror((int)n), device_.c_str());
            CloseDevice();
            continue;
        }
        reopen_attempts = 0;
        if (!cb_) continue;
        if (capture_channels_ == 1) {
            cb_(interleaved.data(), static_cast<size_t>(n));
            continue;
        }
        for (snd_pcm_sframes_t i = 0; i < n; ++i) {
            mono[static_cast<size_t>(i)] =
                interleaved[static_cast<size_t>(i) * capture_channels_ +
                            selected_channel_];
        }
        cb_(mono.data(), static_cast<size_t>(n));
    }
}

} // namespace jetson::audio
