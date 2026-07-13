#ifndef JETSON_SHIM_ESP_LOG_H
#define JETSON_SHIM_ESP_LOG_H
/* Drop-in replacement for esp_log.h on Linux. */
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <chrono>

static inline uint32_t esp_log_timestamp_ms() {
    static auto start = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - start)
                  .count();
    return (uint32_t)ms;
}

#ifndef ESP_LOG_TAG
#define ESP_LOG_TAG "jetson"
#endif

#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "E (%u) %s: " fmt "\n", esp_log_timestamp_ms(), tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "W (%u) %s: " fmt "\n", esp_log_timestamp_ms(), tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) fprintf(stderr, "I (%u) %s: " fmt "\n", esp_log_timestamp_ms(), tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) /* debug disabled by default */
#define ESP_LOGV(tag, fmt, ...) /* verbose disabled by default */

#define ESP_LOG_LEVEL_NONE  0
#define ESP_LOG_LEVEL_ERROR 1
#define ESP_LOG_LEVEL_WARN  2
#define ESP_LOG_LEVEL_INFO  3
#define ESP_LOG_LEVEL_DEBUG 4
#define ESP_LOG_LEVEL_VERBOSE 5

#endif