#ifndef JETSON_SHIM_ESP_TIMER_H
#define JETSON_SHIM_ESP_TIMER_H
/* esp_timer replacement backed by std::thread + condition_variable.
 * API-compatible with the ESP-IDF esp_timer used by the ported sources. */
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*esp_timer_cb_t)(void *arg);

typedef enum { ESP_TIMER_TASK, ESP_TIMER_ISR } esp_timer_dispatch_t;

typedef struct {
    esp_timer_cb_t callback;
    void *arg;
    esp_timer_dispatch_t dispatch_method;
    const char *name;
    bool skip_unhandled_events;
} esp_timer_create_args_t;

struct esp_timer;
typedef struct esp_timer *esp_timer_handle_t;

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

struct esp_timer {
    esp_timer_cb_t callback = nullptr;
    void *arg = nullptr;
    bool periodic = false;
    uint64_t period_us = 0;
    std::atomic<bool> alive{true};
    std::atomic<bool> active{false};
    std::thread thread;
    std::mutex mtx;
    std::condition_variable cv;

    esp_timer() = default;

    ~esp_timer() {
        alive = false;
        cv.notify_all();
        if (thread.joinable()) thread.join();
    }

    void run() {
        auto next = std::chrono::steady_clock::now() +
                    std::chrono::microseconds(period_us);
        while (alive.load()) {
            std::unique_lock<std::mutex> lock(mtx);
            if (cv.wait_until(lock, next) == std::cv_status::timeout) {
                if (!alive.load()) break;
                if (callback) callback(arg);
                if (!periodic) break;
                next = std::chrono::steady_clock::now() +
                       std::chrono::microseconds(period_us);
            }
            if (!alive.load()) break;
        }
        active = false;
    }
};

static inline esp_err_t esp_timer_create(const esp_timer_create_args_t *args,
                                         esp_timer_handle_t *out_handle) {
    auto *t = new esp_timer();
    t->callback = args->callback;
    t->arg = args->arg;
    *out_handle = t;
    return ESP_OK;
}

static inline esp_err_t esp_timer_stop(esp_timer_handle_t t);  // fwd decl

static inline esp_err_t esp_timer_start_periodic(esp_timer_handle_t t, uint64_t us) {
    if (!t) return ESP_ERR_INVALID_ARG;
    if (t->active.load()) esp_timer_stop(t);
    t->periodic = true;
    t->period_us = us;
    t->active = true;
    if (t->thread.joinable()) t->thread.join();
    t->thread = std::thread([t]() { t->run(); });
    return ESP_OK;
}

static inline esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t us) {
    if (!t) return ESP_ERR_INVALID_ARG;
    if (t->active.load()) esp_timer_stop(t);
    t->periodic = false;
    t->period_us = us;
    t->active = true;
    if (t->thread.joinable()) t->thread.join();
    t->thread = std::thread([t]() { t->run(); });
    return ESP_OK;
}

static inline esp_err_t esp_timer_stop(esp_timer_handle_t t) {
    if (!t) return ESP_ERR_INVALID_ARG;
    t->alive = false;
    t->cv.notify_all();
    return ESP_OK;
}

static inline esp_err_t esp_timer_delete(esp_timer_handle_t t) {
    if (!t) return ESP_ERR_INVALID_ARG;
    t->alive = false;
    t->cv.notify_all();
    if (t->thread.joinable()) t->thread.join();
    delete t;
    return ESP_OK;
}

static inline int64_t esp_timer_get_time(void) {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

static inline uint64_t esp_timer_next_expiry_time(void) { return 0; }
static inline esp_err_t esp_timer_dump(FILE *f) { (void)f; return ESP_OK; }

#endif // __cplusplus
#endif