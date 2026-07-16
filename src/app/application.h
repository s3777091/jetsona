#ifndef APPLICATION_H
#define APPLICATION_H

#include "device_state.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#define MAIN_EVENT_SCHEDULE (1 << 0)
#define MAIN_EVENT_CLOCK_TICK (1 << 6)
#define MAIN_EVENT_TOGGLE_CHAT (1 << 9)
#define MAIN_EVENT_STATE_CHANGED (1 << 12)
#define MAIN_EVENT_NETWORK_CONNECTED (1 << 7)
#define MAIN_EVENT_NETWORK_DISCONNECTED (1 << 8)
#define MAIN_EVENT_ERROR (1 << 4)

class Application {
public:
    static Application &GetInstance() {
        static Application instance;
        return instance;
    }
    Application(const Application &) = delete;
    Application &operator=(const Application &) = delete;

    void Initialize();
    void Run();

    DeviceState GetDeviceState() const { return state_; }
    bool SetDeviceState(DeviceState state);
    void Schedule(std::function<void()> &&callback);
    void ToggleChatState();
    void StartListening();
    void StopListening();
    void PlaySound(const std::string & /*sound*/) {}
    void Reboot();
    void Alert(const char *status, const char *message, const char *emotion = "");

private:
    Application();
    ~Application();
    static void OnClockTimer(void *arg);
    void HandleStateChanged();

    DeviceState state_ = kDeviceStateUnknown;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_ = nullptr;
    std::mutex tasks_mtx_;
    std::deque<std::function<void()>> main_tasks_;
};

#endif