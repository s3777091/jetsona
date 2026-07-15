#pragma once

/* Dynamic-Island style status bar, parented to `lv_layer_top()` so it renders
 * above every full-screen overlay and stays visible on every screen.
 *
 * A black pill sits centered at the top. Flat children left-to-right:
 *   wifi, bluetooth, battery (drawn icon + %/"AC"), input language (EN/VI),
 *   power/lock icon  ...  [flex-grow spacer]  ...  weekday + date + time
 *
 * (Flat children -- not nested content-sized clusters -- because LVGL does
 * not reliably resolve nested LV_SIZE_CONTENT in one layout pass, which left
 * the island empty. A flex-grow spacer pushes the clock group to the right.)
 *
 * Where an iPhone puts the camera / Face ID, this firmware puts notifications:
 * `ShowNotification(text, ms)` drops a small panel straight down from the
 * island's center (slide + fade), leaving the clusters visible, then
 * auto-dismisses. Tapping the power icon drops a menu (Khoa / Khoi dong lai /
 * Tat may) the same way; the icon toggles it and it auto-closes.
 *
 * Threading: the lv_timer and all event callbacks run on the LVGL handler
 * thread, which does NOT hold lv_lock, so they take jetson::ui::LvglLockGuard. */
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
    void SetLockAction(Action cb) { lock_action_ = std::move(cb); }
    void SetRebootAction(Action cb) { reboot_action_ = std::move(cb); }
    void SetShutdownAction(Action cb) { shutdown_action_ = std::move(cb); }

    void Hide();
    void Show();

    /* Drop a notification from the island center; auto-dismisses after ms. */
    void ShowNotification(const char *text, int duration_ms = 3000);

private:
    lv_obj_t *pill_ = nullptr;
    lv_obj_t *wifi_label_ = nullptr;
    lv_obj_t *bt_label_ = nullptr;
    lv_obj_t *battery_icon_root_ = nullptr;
    lv_obj_t *battery_icon_body_ = nullptr;
    lv_obj_t *battery_icon_fill_ = nullptr;
    lv_obj_t *battery_percent_label_ = nullptr;
    lv_obj_t *lang_label_ = nullptr;
    lv_obj_t *power_label_ = nullptr;
    lv_obj_t *datetime_label_ = nullptr;

    // Notification drop panel (below pill center).
    lv_obj_t *notif_panel_ = nullptr;
    lv_obj_t *notif_label_ = nullptr;
    lv_timer_t *notif_timer_ = nullptr;

    // Power menu drop (below pill center).
    lv_obj_t *power_menu_ = nullptr;
    lv_timer_t *power_menu_timer_ = nullptr;

    lv_timer_t *timer_ = nullptr; // 1 Hz refresh

    std::chrono::steady_clock::time_point last_battery_read_{};
    int cached_battery_level_ = 100;
    bool has_battery_ = true;
    bool battery_read_done_ = false;
    bool low_warned_ = false;
    std::string cached_datetime_;
    std::string cached_lang_;

    Action wifi_action_, bt_action_, lock_action_, reboot_action_, shutdown_action_;

    void Refresh();
    void RefreshClock();
    void RefreshBattery();
    void RefreshLang();
    void BuildPowerMenu();
    void ShowPowerMenu();
    void HidePowerMenu();
    void AnimateDrop(lv_obj_t *obj, bool show);

    static void OnTimer(lv_timer_t *t);
    static void OnDeleted(lv_event_t *e);
    static void OnNotifDeleted(lv_event_t *e);
    static void OnPowerMenuDeleted(lv_event_t *e);
    static void OnNotifTimer(lv_timer_t *t);
    static void OnPowerMenuTimer(lv_timer_t *t);
    static void OnWifiClick(lv_event_t *e);
    static void OnBtClick(lv_event_t *e);
    static void OnPowerClick(lv_event_t *e);
    static void OnPowerLock(lv_event_t *e);
    static void OnPowerReboot(lv_event_t *e);
    static void OnPowerShutdown(lv_event_t *e);
    static void OnDropOpa(void *var, int32_t v);
    static void OnDropY(void *var, int32_t v);
    static void OnDropHidden(lv_anim_t *a);
};

} // namespace home