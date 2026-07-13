#ifndef JETSON_SHIM_ESP_SYSTEM_H
#define JETSON_SHIM_ESP_SYSTEM_H
#include <cstdlib>
#include "esp_err.h"

static inline void esp_restart(void) { abort(); }
static inline void esp_system_abort(const char */*msg*/) { abort(); }

#endif