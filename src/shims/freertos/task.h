#ifndef JETSON_SHIM_FREERTOS_TASK_H
#define JETSON_SHIM_FREERTOS_TASK_H
#include "FreeRTOS.h"
#include <thread>
#include <chrono>
#include <functional>
#include <memory>
#include <string>

typedef std::shared_ptr<std::thread> TaskHandle_t;

static inline void vTaskDelay(TickType_t ticks) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ticks));
}
static inline void vTaskDelayUntil(TickType_t *prev, TickType_t inc) {
    (void)prev; std::this_thread::sleep_for(std::chrono::milliseconds(inc));
}

/* xTaskCreate: stack_depth/priority/core ignored on Linux. */
static inline BaseType_t xTaskCreate(
    std::function<void(void *)> task_code, const char *name,
    uint32_t /*stack_depth*/, void *arg, UBaseType_t /*priority*/,
    TaskHandle_t *out_handle) {
    auto t = std::make_shared<std::thread>([task_code, arg]() { task_code(arg); });
    t->detach();
    if (out_handle) *out_handle = t;
    if (name) {} /* name unused */
    return pdPASS;
}

static inline BaseType_t xTaskCreatePinnedToCore(
    std::function<void(void *)> task_code, const char *name,
    uint32_t stack_depth, void *arg, UBaseType_t priority,
    TaskHandle_t *out_handle, BaseType_t /*core*/) {
    return xTaskCreate(task_code, name, stack_depth, arg, priority, out_handle);
}

static inline void vTaskDelete(TaskHandle_t /*handle*/) {
    /* threads are detached; nothing to do */
}

static inline UBaseType_t uxTaskPriorityGet(TaskHandle_t) { return 1; }
static inline void vTaskPrioritySet(TaskHandle_t, UBaseType_t) {}
static inline UBaseType_t uxTaskGetNumberOfTasks(void) { return 1; }

#endif