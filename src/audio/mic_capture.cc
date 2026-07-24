#include "audio/mic_capture.h"

#include "esp_log.h"
#include "platform/thread_affinity.h"

#include <cstring>
#include <vector>

#define TAG "MicCapture"

namespace jetson::audio {

MicCapture::~MicCapture() { Stop(); }

bool MicCapture::Start(const std::string &device, int sample_rate,
                       int frames_per_chunk, ChunkCb cb) {
    if (running_.load()) return true;
    sample_rate_ = sample_rate > 0 ? sample_rate : 16000;
    frames_per_chunk_ = frames_per_chunk > 0 ? frames_per_chunk : 512;
    cb_ = std::move(cb);

    int err = snd_pcm_open(&pcm_, device.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        ESP_LOGE(TAG, "cannot open capture device '%s': %s", device.c_str(),
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
    unsigned channels = 1;
    snd_pcm_hw_params_set_channels(pcm_, hw, channels);
    // ~200 ms latency buffer is plenty for a wake-word + utterance loop and
    // keeps underruns rare on a busy A57.
    unsigned latency_us = 200000;
    snd_pcm_hw_params_set_buffer_time_near(pcm_, hw, &latency_us, nullptr);
    err = snd_pcm_hw_params(pcm_, hw);
    if (err < 0) {
        ESP_LOGE(TAG, "hw_params failed on '%s': %s", device.c_str(),
                 snd_strerror(err));
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
        return false;
    }

    snd_pcm_prepare(pcm_);
    running_.store(true);
    thread_ = std::thread(&MicCapture::Run, this);
    // Pin capture to core 0: KWS runs here and must keep low jitter while the
    // agent / STT occupy the other A57 cores. Best-effort on Linux only.
    jetson::platform::SetThreadName(thread_, "ekko-mic");
    if (!jetson::platform::PinToCore(thread_, 0))
        ESP_LOGW(TAG, "could not pin capture to core 0");
    ESP_LOGI(TAG, "capturing %u Hz mono S16LE from '%s' (%d frames/chunk)",
             rate, device.c_str(), frames_per_chunk_);
    return true;
}

void MicCapture::Stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    if (pcm_) {
        snd_pcm_drain(pcm_);
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
    }
}

void MicCapture::Run() {
    std::vector<int16_t> buf(frames_per_chunk_);
    while (running_.load()) {
        snd_pcm_sframes_t n = snd_pcm_readi(pcm_, buf.data(), frames_per_chunk_);
        if (n < 0) {
            // EPIPE = overrun; recover and keep going rather than dropping the
            // whole capture session on a transient scheduler hiccup.
            if (n == -EPIPE) {
                snd_pcm_prepare(pcm_);
                continue;
            }
            ESP_LOGW(TAG, "readi error: %s", snd_strerror((int)n));
            snd_pcm_recover(pcm_, (int)n, 1);
            continue;
        }
        if (cb_) cb_(buf.data(), static_cast<size_t>(n));
    }
}

} // namespace jetson::audio