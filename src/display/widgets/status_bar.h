#pragma once

/* iPhone-like Dynamic Island, parented to `lv_layer_top()` so it renders above
 * every full-screen view. The ordinary system information is deliberately
 * outside the island: time/date/weather at the left, a compact
 * quick-settings cluster at the right, and an opaque black island
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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "display/widgets/optimize_widget.h"
#include "net/bluetooth_manager.h"
#include "net/wifi_manager.h"

class LvglImage;

namespace home {

class StatusBar {
public:
    using Action = std::function<void()>;

    explicit StatusBar(lv_obj_t *parent);
    ~StatusBar();

    void SetWifiAction(Action cb) { wifi_action_ = std::move(cb); }
    void SetBluetoothAction(Action cb) { bt_action_ = std::move(cb); }
    void SetVolumeAction(std::function<void(int, bool)> cb) {
        volume_action_ = std::move(cb);
    }
    void SetBrightnessAction(std::function<void(int)> cb) {
        brightness_action_ = std::move(cb);
    }
    void SetSleepAction(Action cb) { sleep_action_ = std::move(cb); }
    void SetLockAction(Action cb) { lock_action_ = std::move(cb); }
    void SetRebootAction(Action cb) { reboot_action_ = std::move(cb); }
    void SetShutdownAction(Action cb) { shutdown_action_ = std::move(cb); }
    // With music active, click toggles compact/expanded now-playing. Otherwise
    // it opens the app switcher; long-press always keeps the switcher reachable.
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
    // Resting island content: Bluetooth device mini icon (controller-mini /
    // headphones / unknow-device) + connection status ring. Hidden while a
    // notification blooms the pill.
    lv_obj_t *island_rest_ = nullptr;
    lv_obj_t *island_device_icon_ = nullptr;
    lv_obj_t *island_ring_ = nullptr;
    lv_obj_t *island_content_ = nullptr;
    lv_obj_t *island_icon_bg_ = nullptr;
    lv_obj_t *island_icon_ = nullptr;
    lv_obj_t *island_title_ = nullptr;
    lv_obj_t *island_message_ = nullptr;
    // Persistent media presentation. Notifications temporarily cover this
    // subtree, then the island returns to compact now-playing automatically.
    lv_obj_t *media_content_ = nullptr;
    lv_obj_t *media_compact_ = nullptr;
    lv_obj_t *media_compact_art_host_ = nullptr;
    lv_obj_t *media_compact_art_ = nullptr;
    lv_obj_t *media_compact_title_ = nullptr;
    lv_obj_t *media_compact_artist_ = nullptr;
    lv_obj_t *media_compact_more_ = nullptr;
    lv_obj_t *media_expanded_ = nullptr;
    lv_obj_t *media_expanded_art_host_ = nullptr;
    lv_obj_t *media_expanded_art_ = nullptr;
    lv_obj_t *media_expanded_title_ = nullptr;
    lv_obj_t *media_expanded_artist_ = nullptr;
    lv_obj_t *media_progress_ = nullptr;
    lv_obj_t *media_elapsed_ = nullptr;
    lv_obj_t *media_remaining_ = nullptr;
    lv_obj_t *media_toggle_label_ = nullptr;
    std::unique_ptr<LvglImage> media_artwork_;
    std::string media_artwork_path_;
    uint64_t media_revision_ = 0;
    lv_obj_t *left_cluster_ = nullptr;
    lv_obj_t *right_cluster_ = nullptr;
    lv_obj_t *airplane_icon_ = nullptr;
    lv_obj_t *vpn_icon_ = nullptr;
    // PNG icons from assets/icons/app (label fallbacks when a PNG is missing).
    lv_obj_t *wifi_icon_ = nullptr;
    lv_obj_t *bt_icon_ = nullptr;
    lv_obj_t *sound_icon_ = nullptr;
    lv_obj_t *brightness_icon_ = nullptr;
    lv_obj_t *battery_icon_root_ = nullptr;
    lv_obj_t *battery_icon_body_ = nullptr;
    lv_obj_t *battery_icon_fill_ = nullptr;
    lv_obj_t *battery_icon_nub_ = nullptr;
    lv_obj_t *battery_percent_label_ = nullptr;
    lv_obj_t *lang_label_ = nullptr;
    lv_obj_t *power_icon_ = nullptr;
    lv_obj_t *datetime_label_ = nullptr;

    // Compact quick settings. Exactly one of these popovers is visible at a
    // time; cache owns the first status icon and its own disk/RAM popover.
    std::unique_ptr<OptimizeWidget> optimize_widget_;
    lv_obj_t *wifi_menu_ = nullptr;
    lv_obj_t *bt_menu_ = nullptr;
    lv_obj_t *sound_menu_ = nullptr;
    lv_obj_t *brightness_menu_ = nullptr;
    lv_obj_t *wifi_switch_ = nullptr;
    lv_obj_t *bt_switch_ = nullptr;
    lv_obj_t *sound_slider_ = nullptr;
    lv_obj_t *sound_value_ = nullptr;
    lv_obj_t *sound_mute_icon_ = nullptr;
    lv_obj_t *brightness_slider_ = nullptr;
    lv_obj_t *brightness_value_ = nullptr;
    bool suppress_quick_events_ = false;
    lv_timer_t *quick_menu_timer_ = nullptr;

    struct QuickRowContext {
        StatusBar *self = nullptr;
        std::string id;
        bool active = false;
        bool secured = false;
        bool known = false;
    };
    std::vector<std::unique_ptr<QuickRowContext>> wifi_row_ctx_;
    std::vector<std::unique_ptr<QuickRowContext>> bt_row_ctx_;

    std::mutex quick_scan_mutex_;
    std::vector<jetson::WifiNetwork> quick_wifi_networks_;
    std::vector<jetson::BtDevice> quick_bt_devices_;
    std::thread wifi_scan_thread_;
    std::thread bt_scan_thread_;
    std::atomic<bool> wifi_scan_busy_{false};
    std::atomic<bool> bt_scan_busy_{false};
    std::atomic<uint32_t> wifi_scan_revision_{0};
    std::atomic<uint32_t> bt_scan_revision_{0};
    uint32_t applied_wifi_scan_revision_ = 0;
    uint32_t applied_bt_scan_revision_ = 0;

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
    bool notification_visible_ = false;
    bool media_available_ = false;
    bool media_expanded_open_ = false;
    bool suppress_island_click_ = false;
    std::string cached_datetime_;
    std::string cached_lang_;
    bool cached_airplane_mode_ = false;
    bool airplane_state_read_ = false;
    bool cached_vpn_enabled_ = false;
    bool vpn_state_read_ = false;

    /* WiFi/Bluetooth state polling. nmcli/bluetoothctl block for up to
     * seconds, so a worker thread polls them and publishes into atomics; the
     * 1 Hz LVGL timer only reads the atomics and swaps icon sources.
     * wifi signal: -2 not read yet, -1 radio off/disconnected, 0..100 in use. */
    std::thread conn_poll_thread_;
    std::atomic<bool> conn_poll_stop_{false};
    std::atomic<int> polled_wifi_signal_{-2};
    std::atomic<int> polled_wifi_enabled_{-1};
    // -1 unknown, 0 no cable/link, 1 LAN cable link up. Unlike the radios this
    // is a cheap sysfs read, so the worker refreshes it on every 500 ms tick
    // and the ethernet icon reacts to a plug/unplug almost immediately.
    std::atomic<int> polled_eth_connected_{-1};
    std::atomic<int> polled_bt_powered_{-1}; // -1 unknown, 0 off, 1 on
    // jetson::BtDeviceKind of the connected device (-1 = not polled yet).
    std::atomic<int> polled_bt_device_{-1};
    int cached_wifi_signal_ = -3;            // last value applied to the UI
    int cached_wifi_enabled_ = -2;
    int cached_eth_connected_ = -2;
    int cached_bt_powered_ = -2;
    int cached_bt_device_ = -2;

    Action wifi_action_, bt_action_, sleep_action_, lock_action_, reboot_action_, shutdown_action_;
    std::function<void(int, bool)> volume_action_;
    std::function<void(int)> brightness_action_;
    Action island_action_;

    void Refresh();
    void RefreshClock();
    void RefreshBattery();
    void RefreshLang();
    void RefreshConnectivity();
    void BuildPowerMenu();
    void BuildQuickMenus();
    lv_obj_t *CreateQuickMenu(int width);
    void RebuildWifiMenu();
    void RebuildBluetoothMenu();
    void StartWifiScan();
    void StartBluetoothScan();
    void ShowQuickMenu(lv_obj_t *menu, lv_obj_t *anchor);
    void HideQuickMenus(lv_obj_t *except = nullptr);
    void ArmQuickMenuTimer();
    void ShowPowerMenu();
    void HidePowerMenu();
    void ShowIslandMessage(const char *title, const char *text,
                           const char *icon, uint32_t accent,
                           int duration_ms);
    void AnimateIslandSize(int width, int height, bool collapsing);
    void CollapseIsland(bool animated = true);
    // Show island_rest_ only while the pill is a plain resting island (no
    // notification bloom, no now-playing surface).
    void SyncIslandRest();
    void BuildMediaContent();
    void RefreshMedia(bool force_layout = false);
    void ShowMediaPresentation(bool animate);
    void HideMediaContent();
    void LoadMediaArtwork(const std::string &path);
    void AnimateDrop(lv_obj_t *obj, bool show);

    static void OnTimer(lv_timer_t *t);
    static void OnDeleted(lv_event_t *e);
    static void OnPowerMenuDeleted(lv_event_t *e);
    static void OnNotifTimer(lv_timer_t *t);
    static void OnPowerMenuTimer(lv_timer_t *t);
    static void OnQuickMenuTimer(lv_timer_t *t);
    static void OnWifiClick(lv_event_t *e);
    static void OnBtClick(lv_event_t *e);
    static void OnSoundClick(lv_event_t *e);
    static void OnBrightnessClick(lv_event_t *e);
    static void OnWifiToggle(lv_event_t *e);
    static void OnBluetoothToggle(lv_event_t *e);
    static void OnWifiRow(lv_event_t *e);
    static void OnBluetoothRow(lv_event_t *e);
    static void OnWifiSettings(lv_event_t *e);
    static void OnBluetoothSettings(lv_event_t *e);
    static void OnVolumeChanged(lv_event_t *e);
    static void OnMuteClick(lv_event_t *e);
    static void OnBrightnessChanged(lv_event_t *e);
    static void OnIslandClick(lv_event_t *e);
    static void OnIslandLongPress(lv_event_t *e);
    static void OnMediaPrevious(lv_event_t *e);
    static void OnMediaToggle(lv_event_t *e);
    static void OnMediaNext(lv_event_t *e);
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
