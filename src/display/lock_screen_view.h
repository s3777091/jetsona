#pragma once

/* Full-screen lock overlay (no traffic lights -- it cannot be dismissed
 * except by entering the correct PIN). The home display owns the instance so
 * it outlives the settings view that triggered it.
 *
 * PIN entry uses a digit TelexInput (plain ASCII, password-masked, max 4) wired
 * into the USB-keyboard keypad group; Enter or the "Mở khóa" button submits.
 * The expected PIN is read from Settings("system").GetString("pin"); home does
 * not open this view when no PIN is set. Wrong PIN flashes a message and
 * clears the field. */
#include "telex_ime.h"

#include <lvgl.h>
#include <atomic>
#include <functional>
#include <memory>

namespace home {

class LockScreenView : public std::enable_shared_from_this<LockScreenView> {
public:
    using ClosedCb = std::function<void()>;

    LockScreenView(lv_obj_t *parent, int width, int height, ClosedCb on_closed);
    ~LockScreenView();

    void Start();
    void RequestClose();

private:
    lv_obj_t *parent_ = nullptr;
    lv_obj_t *overlay_ = nullptr;
    lv_obj_t *status_label_ = nullptr;
    TelexInput *input_ = nullptr;  // self-deletes with its LVGL object
    int width_ = 0;
    int height_ = 0;
    ClosedCb on_closed_;
    std::atomic<bool> closed_{false};

    void CheckPin();
    static void OnUnlock(lv_event_t *e);
    static void OnPinReady(lv_event_t *e);
    static void OnCloseTimer(lv_timer_t *t);
};

} // namespace home