#pragma once

/* iPhone-like Dynamic Island, parented to `lv_layer_top()` so it renders above
 * every full-screen view. The ordinary system information is deliberately
 * outside the island: time/date/weather at the left, a compact
 * connectivity/battery/action cluster at the right, and an opaque black island
 * pill in the center.
 *
 * A notification "blooms" the *same* pill into a larger rounded surface,
 * reveals a compact title/message, then smoothly returns to the sensor size.
 * This mirrors the compact/expanded relationship of a real Dynamic Island;
 * it is not a second toast hanging below a menu bar.
 *
 * Threading: the lv_timer and all event callbacks run on the LVGL handler
 * thread, which does NOT hold lv_lock, so they take jetson::ui::LvglLockGuard. */
#include <lvgl.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>

class LvglImage;

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
    // Clicking the resting island toggles the app switcher (multitasking).
    void SetIslandAction(Action cb) { island_action_ = std::move(cb); }

    // Island center point in screen coords -- the app switcher blooms from here.
    int IslandCenterX() const;
    int IslandCenterY() const;

    void Hide();
    void Show();

    /* Bloom a notification from the island; auto-collapses after ms. */
    void ShowNotification(const char *text, int duration_ms = 3000);
    void ShowWelcome(int duration_ms = 3800);

private:
    lv_obj_t *status_strip_ = nullptr;
    lv_obj_t *pill_ = nullptr;
    lv_obj_t *island_content_ = nullptr;
    lv_obj_t *island_icon_bg_ = nullptr;
    lv_obj_t *island_icon_ = nullptr;
    lv_obj_t *island_title_ = nullptr;
    lv_obj_t *island_message_ = nullptr;
    lv_obj_t *left_cluster_ = nullptr;
    lv_obj_t *right_cluster_ = nullptr;
    lv_obj_t *wifi_label_ = nullptr;
    lv_obj_t *bt_label_ = nullptr;
    lv_obj_t *battery_icon_root_ = nullptr;
    lv_obj_t *battery_icon_body_ = nullptr;
    lv_obj_t *battery_icon_fill_ = nullptr;
    lv_obj_t *battery_icon_nub_ = nullptr;
    lv_obj_t *battery_percent_label_ = nullptr;
    lv_obj_t *lang_label_ = nullptr;
    lv_obj_t *power_label_ = nullptr;
    lv_obj_t *datetime_label_ = nullptr;
    lv_obj_t *weather_icon_ = nullptr;
    std::unique_ptr<LvglImage> weather_icon_image_;

    lv_timer_t *notif_timer_ = nullptr;

    // Power menu drop (below pill center).
    lv_obj_t *power_menu_ = nullptr;
    lv_timer_t *power_menu_timer_ = nullptr;

    lv_timer_t *timer_ = nullptr; // 1 Hz refresh

    std::chrono::steady_clock::time_point last_battery_read_{};
    int cached_battery_level_ = 100;
    bool cached_battery_charging_ = false;
    bool has_battery_ = true;
    bool battery_read_done_ = false;
    bool low_warned_ = false;
    bool visible_ = true;
    bool island_expanded_ = false;
    std::string cached_datetime_;
    std::string cached_lang_;

    Action wifi_action_, bt_action_, lock_action_, reboot_action_, shutdown_action_;
    Action island_action_;

    void Refresh();
    void RefreshClock();
    void RefreshBattery();
    void RefreshLang();
    void BuildPowerMenu();
    void ShowPowerMenu();
    void HidePowerMenu();
    void ShowIslandMessage(const char *title, const char *text,
                           const char *icon, uint32_t accent,
                           int duration_ms);
    void AnimateIslandSize(int width, int height, bool collapsing);
    void CollapseIsland(bool animated = true);
    void AnimateDrop(lv_obj_t *obj, bool show);

    static void OnTimer(lv_timer_t *t);
    static void OnDeleted(lv_event_t *e);
    static void OnPowerMenuDeleted(lv_event_t *e);
    static void OnNotifTimer(lv_timer_t *t);
    static void OnPowerMenuTimer(lv_timer_t *t);
    static void OnWifiClick(lv_event_t *e);
    static void OnBtClick(lv_event_t *e);
    static void OnIslandClick(lv_event_t *e);
    static void OnPowerClick(lv_event_t *e);
    static void OnPowerLock(lv_event_t *e);
    static void OnPowerReboot(lv_event_t *e);
    static void OnPowerShutdown(lv_event_t *e);
    static void OnIslandWidth(void *var, int32_t v);
    static void OnIslandHeight(void *var, int32_t v);
    static void OnIslandContentOpa(void *var, int32_t v);
    static void OnIslandCollapsed(lv_anim_t *a);
    static void OnDropOpa(void *var, int32_t v);
    static void OnDropY(void *var, int32_t v);
    static void OnDropHidden(lv_anim_t *a);
};

} // namespace home
