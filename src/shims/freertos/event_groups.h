#ifndef JETSON_SHIM_FREERTOS_EVENT_GROUPS_H
#define JETSON_SHIM_FREERTOS_EVENT_GROUPS_H
#include "FreeRTOS.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

typedef uint32_t EventBits_t;

class EventGroup {
public:
    EventBits_t set(EventBits_t bits) {
        std::lock_guard<std::mutex> lk(mtx_);
        bits_ |= bits;
        cv_.notify_all();
        return bits_;
    }
    EventBits_t clear(EventBits_t bits) {
        std::lock_guard<std::mutex> lk(mtx_);
        bits_ &= ~bits;
        return bits_;
    }
    EventBits_t get() {
        std::lock_guard<std::mutex> lk(mtx_);
        return bits_;
    }
    /* waitAll true => all bits; false => any bit. timeout in ticks (portMAX_DELAY=infinite). */
    EventBits_t wait(EventBits_t bits, bool clear_on_exit, bool wait_all, TickType_t timeout) {
        std::unique_lock<std::mutex> lk(mtx_);
        auto pred = [&] {
            return (wait_all ? ((bits_ & bits) == bits) : ((bits_ & bits) != 0));
        };
        if (timeout == portMAX_DELAY) {
            cv_.wait(lk, pred);
        } else {
            cv_.wait_for(lk, std::chrono::milliseconds(timeout), pred);
        }
        EventBits_t result = bits_;
        if (clear_on_exit) bits_ &= ~bits;
        return result;
    }

private:
    std::mutex mtx_;
    std::condition_variable cv_;
    EventBits_t bits_ = 0;
};

typedef EventGroup *EventGroupHandle_t;

static inline EventGroupHandle_t xEventGroupCreate() { return new EventGroup(); }
static inline EventBits_t xEventGroupSetBits(EventGroupHandle_t g, EventBits_t b) {
    return g ? g->set(b) : 0;
}
static inline EventBits_t xEventGroupClearBits(EventGroupHandle_t g, EventBits_t b) {
    return g ? g->clear(b) : 0;
}
static inline EventBits_t xEventGroupGetBits(EventGroupHandle_t g) { return g ? g->get() : 0; }
static inline EventBits_t xEventGroupWaitBits(EventGroupHandle_t g, EventBits_t bits,
                                              BaseType_t clear_on_exit, BaseType_t wait_all,
                                              TickType_t timeout) {
    return g ? g->wait(bits, clear_on_exit == pdTRUE, wait_all == pdTRUE, timeout) : 0;
}
static inline void xEventGroupDelete(EventGroupHandle_t g) { delete g; }

#endif