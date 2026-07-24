#ifndef APPLICATION_H
#define APPLICATION_H

#include "device_state.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace jetson {
class Conversation;
}

#define MAIN_EVENT_SCHEDULE         (1 << 0)
#define MAIN_EVENT_CLOCK_TICK       (1 << 6)
#define MAIN_EVENT_STATE_CHANGED    (1 << 12)
#define MAIN_EVENT_ERROR            (1 << 4)

/* Headless "Ekko Lite" application.
 *
 * No display, no LVGL. The main loop is a FreeRTOS-style event group wait that
 * drains Scheduled tasks and ticks a 1 s clock used by the alarm + battery
 * watchers. The AI Conversation and its tool registry are constructed here and
 * exposed so the voice loop (mic/STT/TTS) can drive them. */
class Application {
public:
    static Application &GetInstance() {
        static Application instance;
        return instance;
    }
    Application(const Application &) = delete;
    Application &operator=(const Application &) = delete;

    bool Initialize();
    void Run();
    void RequestStop();

    DeviceState GetDeviceState() const { return state_; }
    bool SetDeviceState(DeviceState state);
    void Schedule(std::function<void()> &&callback);

    /* Speak a short message to the user. With no display this is the only
     * feedback channel, so the battery/alarm watchers route through here. Until
     * the TTS path lands it logs at NOTICE level so nothing fails silently. */
    void Speak(const std::string &text);
    void Alert(const char *status, const char *message, const char *emotion = "");

    jetson::Conversation *GetConversation() const { return conversation_.get(); }

    void Reboot();

private:
    Application();
    ~Application();
    static void OnClockTimer(void *arg);

    void HandleStateChanged();
    void TickClock();
    void CheckAlarm();
    void FireAlarm();

    DeviceState state_ = kDeviceStateUnknown;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_ = nullptr;
    std::mutex tasks_mtx_;
    std::deque<std::function<void()>> main_tasks_;
    std::atomic<bool> running_{true};

    std::shared_ptr<jetson::Conversation> conversation_;

    // Battery low-state alert bookkeeping (see TickClock).
    int last_battery_level_ = -1;
    bool battery_50_said_ = false;
    bool battery_30_said_ = false;
    bool battery_10_said_ = false;
};

#endif