#ifndef JETSON_SHIM_FREERTOS_SEMPHR_H
#define JETSON_SHIM_FREERTOS_SEMPHR_H
#include "FreeRTOS.h"
#include <mutex>
#include <chrono>

/* Binary/mutex semaphore backed by std::mutex (recursive). */
typedef std::recursive_mutex *SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateMutex() {
    return new std::recursive_mutex();
}
static inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() {
    return new std::recursive_mutex();
}
static inline void vSemaphoreDelete(SemaphoreHandle_t s) { delete s; }

static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks) {
    if (!s) return pdFALSE;
    if (ticks == portMAX_DELAY) { s->lock(); return pdTRUE; }
    return s->try_lock_for(std::chrono::milliseconds(ticks)) ? pdTRUE : pdFALSE;
}
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
    if (!s) return pdFALSE;
    s->unlock(); return pdTRUE;
}

#endif