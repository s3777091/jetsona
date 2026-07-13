#ifndef JETSON_SHIM_ESP_PM_H
#define JETSON_SHIM_ESP_PM_H
/* Power-management locks are no-ops on Linux. */
#include "esp_err.h"

typedef enum {
    ESP_PM_CPU_FREQ_MAX,
    ESP_PM_APB_FREQ_MAX,
    ESP_PM_NO_LIGHT_SLEEP,
} esp_pm_lock_type_t;

struct esp_pm_lock { int dummy; };
typedef struct esp_pm_lock *esp_pm_lock_handle_t;

static inline esp_err_t esp_pm_lock_create(esp_pm_lock_type_t /*type*/,
                                           uint32_t /*timeout_us*/, const char */*name*/,
                                           esp_pm_lock_handle_t *handle) {
    *handle = new esp_pm_lock{};
    return ESP_OK;
}
static inline esp_err_t esp_pm_lock_acquire(esp_pm_lock_handle_t) { return ESP_OK; }
static inline esp_err_t esp_pm_lock_release(esp_pm_lock_handle_t) { return ESP_OK; }
static inline esp_err_t esp_pm_lock_delete(esp_pm_lock_handle_t h) { delete h; return ESP_OK; }
static inline void esp_pm_dump_locks(FILE */*f*/) {}

#endif