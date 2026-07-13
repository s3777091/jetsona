#ifndef JETSON_SHIM_ESP_ERR_H
#define JETSON_SHIM_ESP_ERR_H
/* Minimal esp_err.h replacement for Linux. */
#include <cstdint>
#include <cstdio>
#include <cstdlib>

typedef int32_t esp_err_t;

#define ESP_OK          0
#define ESP_FAIL        -1
#define ESP_ERR_NO_MEM          0x101
#define ESP_ERR_INVALID_ARG     0x102
#define ESP_ERR_INVALID_STATE   0x103
#define ESP_ERR_NOT_FOUND       0x104
#define ESP_ERR_NOT_SUPPORTED   0x105
#define ESP_ERR_TIMEOUT         0x107
#define ESP_ERR_NVS_NOT_FOUND   0x1102
#define ESP_ERR_NVS_NO_FREE_PAGES 0x1103
#define ESP_ERR_NVS_NEW_VERSION_FOUND 0x1104

static inline const char *esp_err_to_name(esp_err_t err) {
    if (err == ESP_OK) return "ESP_OK";
    if (err == ESP_FAIL) return "ESP_FAIL";
    return "ESP_ERR_UNKNOWN";
}

#define ESP_ERROR_CHECK(x) do { \
    esp_err_t __err_rc = (x); \
    if (__err_rc != ESP_OK) { \
        fprintf(stderr, "ESP_ERROR_CHECK failed: %s (%d) at %s:%d\n", \
                esp_err_to_name(__err_rc), __err_rc, __FILE__, __LINE__); \
        abort(); \
    } \
} while (0)

#define ESP_ERROR_CHECK_WITHOUT_ABORT(x) do { \
    esp_err_t __err_rc = (x); \
    if (__err_rc != ESP_OK) { \
        fprintf(stderr, "ESP_ERROR_CHECK failed: %s (%d) at %s:%d\n", \
                esp_err_to_name(__err_rc), __err_rc, __FILE__, __LINE__); \
    } \
} while (0)

#endif