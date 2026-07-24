#include "audio/audio_output.h"

#include "esp_log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#define TAG "AudioOutput"

namespace jetson::audio {

AudioOutput::~AudioOutput() { Stop(); }

bool AudioOutput::Play(const float *samples, size_t n, int sample_rate,
                       const std::string &device) {
    std::lock_guard<std::mutex> lk(mtx_);
    stop_.store(false);

    snd_pcm_t *pcm = nullptr;
    int err = snd_pcm_open(&pcm, device.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        ESP_LOGE(TAG, "cannot open playback device '%s': %s", device.c_str(),
                 snd_strerror(err));
        return false;
    }

    snd_pcm_hw_params_t *hw = nullptr;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);
    snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
    unsigned rate = static_cast<unsigned>(sample_rate > 0 ? sample_rate : 16000);
    snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, nullptr);
    unsigned channels = 1;
    snd_pcm_hw_params_set_channels(pcm, hw, channels);
    unsigned latency_us = 200000;
    snd_pcm_hw_params_set_buffer_time_near(pcm, hw, &latency_us, nullptr);
    err = snd_pcm_hw_params(pcm, hw);
    if (err < 0) {
        ESP_LOGE(TAG, "playback hw_params failed: %s", snd_strerror(err));
        snd_pcm_close(pcm);
        return false;
    }
    snd_pcm_prepare(pcm);

    constexpr size_t kFrames = 512;
    std::vector<int16_t> out(kFrames);
    size_t i = 0;
    bool interrupted = false;
    while (i < n) {
        if (stop_.load()) { interrupted = true; break; }
        size_t chunk = std::min<size_t>(kFrames, n - i);
        for (size_t k = 0; k < chunk; ++k) {
            float s = samples[i + k];
            if (s > 1.0f) s = 1.0f;
            if (s < -1.0f) s = -1.0f;
            out[k] = static_cast<int16_t>(s * 32767.0f);
        }
        snd_pcm_sframes_t w = snd_pcm_writei(pcm, out.data(),
                                             static_cast<snd_pcm_uframes_t>(chunk));
        if (w < 0) {
            if (w == -EPIPE) { snd_pcm_prepare(pcm); continue; }
            snd_pcm_recover(pcm, static_cast<int>(w), 1);
            continue;
        }
        i += static_cast<size_t>(w);
    }
    snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
    if (interrupted) ESP_LOGI(TAG, "playback interrupted");
    return !interrupted;
}

} // namespace jetson::audio