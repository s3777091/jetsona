#pragma once

#include <cstdint>

#include <lvgl.h>

namespace jetson::ui {

// Converts the project's 0xRRGGBB palette values to LVGL colors.
inline lv_color_t Color(uint32_t rgb) {
    return lv_color_make((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

// LVGL is shared by the handler and worker threads. Keep lock ownership scoped
// so early returns and exceptions cannot leave the runtime locked.
class LvglLockGuard final {
public:
    LvglLockGuard() { lv_lock(); }
    ~LvglLockGuard() { lv_unlock(); }

    LvglLockGuard(const LvglLockGuard &) = delete;
    LvglLockGuard &operator=(const LvglLockGuard &) = delete;
    LvglLockGuard(LvglLockGuard &&) = delete;
    LvglLockGuard &operator=(LvglLockGuard &&) = delete;
};

} // namespace jetson::ui
