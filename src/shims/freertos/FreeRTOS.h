#ifndef JETSON_SHIM_FREERTOS_H
#define JETSON_SHIM_FREERTOS_H
/* Minimal FreeRTOS shim for Linux (pthread-backed). */
#include <cstdint>

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;

#define pdTRUE   1
#define pdFALSE  0
#define pdPASS   1
#define pdFAIL   0

#define portMAX_DELAY UINT32_MAX

static inline TickType_t pdMS_TO_TICKS(uint32_t ms) { return ms; }

#endif