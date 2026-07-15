#pragma once

/* Global status bar (wifi / bluetooth / battery / volume / clock) parented to
 * `lv_layer_top()` so it renders above every full-screen overlay and stays
 * visible on every screen -- home and all app views. Self-refreshes clock +
 * battery via an lv_timer; the I2C battery read is throttled to once / 5 s and
 * falls back to "AC" when no battery is present (wall-powered Jetson). Click
 * hooks (wifi/bluetooth/volume) are wired by the home display; Hide()/Show()
 * let the lock screen mask the bar (it lives above the lock overlay otherwise).
 *
 * Threading: the lv_timer runs on the LVGL handler thread, which does NOT hold
 * lv_lock, so OnTimer and the click handlers take jetson::ui::LvglLockGuard. */

#include <lvgl.h>

#include <chrono>
#include <functional>
#include <string>

namespace home {

class StatusBar {
public:
    using Action = std::function<void()>;

    explicit StatusBar(lv_obj_t *parent);
    ~StatusBar();

    void SetWifiAction(Action cb) { wifi_action_ = std::move(cb); }
    void SetBluetoothAction(Action cb) { bt_action_ = std::move(cb); }
    void SetVolumeAction(Action cb) { vol_action_ = std::move(cb); }

    void Hide();
    void Show();

private:
    lv_obj_t *pill_ = nullptr;
    lv_obj_t *wifi_label_ = nullptr;
    lv_obj_t *bt_label_ = nullptr;
    lv_obj_t *battery_percent_label_ = nullptr;
    lv_obj_t *battery_icon_root_ = nullptr;
    lv_obj_t *battery_icon_body_ = nullptr;
    lv_obj_t *battery_icon_fill_ = nullptr;
    lv_obj_t *volume_label_ = nullptr;
    lv_obj_t *time_label_ = nullptr;

    lv_timer_t *timer_ = nullptr;

    std::chrono::steady_clock::time_point last_battery_read_{};
    int cached_battery_level_ = 100;
    bool has_battery_ = true;
    bool battery_read_done_ = false;
    std::string cached_time_;

    Action wifi_action_;
    Action bt_action_;
    Action vol_action_;

    void Refresh();
    void RefreshClock();
    void RefreshBattery();
    void RefreshVolume();

    static void OnTimer(lv_timer_t *t);
    static void OnDeleted(lv_event_t *e);
    static void OnWifiClick(lv_event_t *e);
    static void OnBtClick(lv_event_t *e);
    static void OnVolumeClick(lv_event_t *e);
};

} // namespace home