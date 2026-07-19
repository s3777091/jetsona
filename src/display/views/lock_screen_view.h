#pragma once

/* Full-screen lock overlay (no traffic lights -- it cannot be dismissed
 * except by entering the correct PIN). The home display owns the instance so
 * it outlives the settings view that triggered it.
 *
 * Layout: the global status bar and Dynamic Island stay visible above this
 * overlay (StatusBar::SetLocked disarms their menus while locked), the weather
 * card sits in the top-right corner -- the home screen no longer carries a
 * weather line, this is where it lives -- and the PIN column is centered.
 *
 * PIN entry uses a digit TelexInput (plain ASCII, password-masked, sized to
 * the stored 4- or 6-digit PIN) wired into the USB-keyboard keypad group;
 * Enter or the "Mở khóa" button submits. The expected PIN is read from
 * Settings("system").GetString("pin"); home does not open this view when no
 * PIN is set. Wrong PIN flashes a message and clears the field. */
#include "display/widgets/telex_ime.h"

#include <lvgl.h>
#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace home {

class LockScreenView : public std::enable_shared_from_this<LockScreenView> {
public:
    using ClosedCb = std::function<void()>;

    LockScreenView(lv_obj_t *parent, int width, int height, ClosedCb on_closed);
    ~LockScreenView();

    void Start();
    void RequestClose();

    /* Weather text for the top-right card ("Đà Nẵng: Mưa rào · 31°C ..."), fed
     * by the home display's updater thread. Call on the LVGL thread. */
    void SetWeather(const std::string &line);

private:
    lv_obj_t *parent_ = nullptr;
    lv_obj_t *overlay_ = nullptr;
    lv_obj_t *status_label_ = nullptr;
    lv_obj_t *weather_card_ = nullptr;
    lv_obj_t *weather_place_ = nullptr;
    lv_obj_t *weather_detail_ = nullptr;
    TelexInput *input_ = nullptr;  // self-deletes with its LVGL object
    int width_ = 0;
    int height_ = 0;
    int pin_len_ = 4;
    ClosedCb on_closed_;
    std::atomic<bool> closed_{false};

    void CheckPin();
    static void OnUnlock(lv_event_t *e);
    static void OnPinReady(lv_event_t *e);
    static void OnCloseTimer(lv_timer_t *t);
};

} // namespace home
