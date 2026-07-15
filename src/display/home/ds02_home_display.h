#pragma once

#include "display/core/lcd_display.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jetson {
class Conversation;
class IWifiManager;
class IBluetoothManager;
} // namespace jetson

namespace home {

class BackgroundGalleryView;
class BluetoothSettingsView;
class CalendarView;
class ChatView;
class DocumentsView;
class LockScreenView;
class SettingsView;
class TerminalView;
class WifiSettingsView;

/* Compact DS-02 home display for the Jetson 800x480 HDMI panel.
 * Reproduces the DS-02 standby (wallpaper + clock + system bar + dock) and
 * app drawer (swipe-up grid of app icons) with touch interaction. The dock
 * carries the backed apps (calendar / gallery / settings / wifi / bt / chat /
 * terminal); the drawer is the upcoming app set. */
class Ds02HomeDisplay : public SpiLcdDisplay {
public:
    Ds02HomeDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                    int width, int height, int offset_x, int offset_y,
                    bool mirror_x, bool mirror_y, bool swap_xy,
                    jetson::IWifiManager &wifi,
                    jetson::IBluetoothManager &bluetooth);
    ~Ds02HomeDisplay() override;

    void SetupUI() override;
    void SetStatus(const char *status) override;
    void SetPowerSaveMode(bool on) override;
    void UpdateStatusBar(bool update_all = false) override;
    void SetTheme(Theme *theme) override;
    void SetChatMessage(const char *role, const char *content) override;
    void SetEmotion(const char *emotion) override;
    void ShowNotification(const char *notification, int duration_ms = 3000) override;

    void AdvanceStandbyButtonState();
    void ShowOnboardSplash(int duration_ms = 3000);
    void OpenWifiSettings();
    void OpenBluetoothSettings();
    void OpenCalendar();
    void OpenDocuments();
    void OpenBackgroundGallery();
    void OpenSettings();
    void OpenChat();
    void OpenTerminal();
    void OpenLockScreen();
    void SetBrightness(int pct);
    void ApplyBackgroundFromFile(const std::string &file);
    void SetSleepBackground(const std::string &file);
    void ReloadBackgrounds();

    /* The wallpaper set is scanned from disk at runtime (backgrounds::ListBackgroundFiles)
     * and cached by filename; the gallery can delete files and the list updates. */

private:
    jetson::IWifiManager &wifi_;
    jetson::IBluetoothManager &bluetooth_;

    enum class StandbyState { Dim, Awake, Launcher };

    static constexpr size_t kDockItemCount = 8;
    static constexpr size_t kDrawerItemCount = 8;

    struct AppCtx {
        Ds02HomeDisplay *self;
        int id;
    };

    void CreateStandbyObjects();
    void CreateDrawerObjects();
    void CreateSystemBarObjects();
    void CreateDockObjects();
    void SetDockActive(int index);
    void RefreshClock();
    void RefreshBattery();
    void CheckIdleDim();
    void ApplyStandbyState();
    void RepaintForTheme();
    bool ApplyBackgroundFile(const std::string &file);
    void ApplyWallpaperForState();
    LvglImage *GetBackgroundImage(const std::string &file);
    void SetTextColor(uint32_t color);
    static void OnRefreshTimer(void *arg);
    static void OnDockButtonEvent(lv_event_t *e);
    static void OnAppButtonClicked(lv_event_t *e);
    static void OnAppDeleted(lv_event_t *e);
    static void OnMenuWifi(lv_event_t *e);
    static void OnMenuBluetooth(lv_event_t *e);
    static void OnMenuVolume(lv_event_t *e);
    static void OnMenuPower(lv_event_t *e);
    void ToggleVolume();
    static void OnSplashOpa(void *var, int32_t v);
    static void OnSplashBar(void *var, int32_t v);
    static void OnSplashGone(lv_anim_t *a);
    static std::string FormatTime(const struct tm &t);
    static std::string FormatDate(const struct tm &t);

    StandbyState standby_state_ = StandbyState::Awake;
    esp_timer_handle_t refresh_timer_ = nullptr;

    lv_obj_t *root_ = nullptr;
    lv_obj_t *standby_layer_ = nullptr;
    // Full-screen black scrim on LVGL's top layer used to dim the whole UI,
    // including app overlays. Non-clickable and non-scrollable.
    lv_obj_t *brightness_overlay_ = nullptr;
    lv_obj_t *wallpaper_ = nullptr;
    lv_obj_t *wallpaper_image_obj_ = nullptr;
    lv_obj_t *dim_overlay_ = nullptr;
    lv_obj_t *system_bar_ = nullptr;       // macOS-style menu bar (top-right cluster)
    lv_obj_t *wifi_label_ = nullptr;
    lv_obj_t *bluetooth_label_ = nullptr;
    lv_obj_t *volume_label_ = nullptr;
    lv_obj_t *power_label_ = nullptr;
    bool volume_muted_ = false;
    lv_obj_t *battery_icon_root_ = nullptr;
    lv_obj_t *battery_icon_body_ = nullptr;
    lv_obj_t *battery_icon_fill_ = nullptr;
    lv_obj_t *battery_percent_label_ = nullptr;
    // Battery is read from I2C (INA219); throttle to one read per ~5 s and
    // cache the result so the two 1 Hz refresh timers don't hammer the bus.
    std::chrono::steady_clock::time_point last_battery_read_{};
    int  cached_battery_level_ = 100;
    bool cached_battery_charging_ = false;
    bool cached_battery_discharging_ = false;
    bool battery_read_done_ = false;
    lv_obj_t *time_label_ = nullptr;
    lv_obj_t *date_label_ = nullptr;
    lv_obj_t *weather_label_ = nullptr;
    lv_obj_t *chat_label_ = nullptr;
    lv_obj_t *launcher_layer_ = nullptr;
    lv_obj_t *dock_ = nullptr;
    std::array<lv_obj_t *, kDockItemCount> dock_buttons_ = {};
    std::array<lv_obj_t *, kDockItemCount> dock_indicators_ = {};
    std::array<std::unique_ptr<LvglImage>, kDockItemCount> dock_icon_cache_ = {};
    int dock_active_index_ = -1;
    std::array<std::unique_ptr<LvglImage>, kDrawerItemCount> drawer_icon_cache_ = {};

    std::map<std::string, std::unique_ptr<LvglImage>> background_image_cache_;
    std::vector<std::string> background_files_;
    std::string background_file_;       // current desktop wallpaper filename
    std::string sleep_background_file_; // wallpaper shown when the screen is dim
    uint32_t text_color_ = 0xffffff;
    std::string cached_time_;
    std::string cached_date_;

    std::shared_ptr<WifiSettingsView> wifi_view_;
    std::shared_ptr<BluetoothSettingsView> bt_view_;
    std::shared_ptr<CalendarView> calendar_view_;
    std::shared_ptr<DocumentsView> documents_view_;
    std::shared_ptr<BackgroundGalleryView> gallery_view_;
    std::shared_ptr<SettingsView> settings_view_;
    std::shared_ptr<ChatView> chat_view_;
    std::shared_ptr<TerminalView> terminal_view_;
    std::shared_ptr<LockScreenView> lock_screen_view_;
    std::shared_ptr<jetson::Conversation> chat_conv_;

    lv_obj_t *app_grid_ = nullptr;

    /* Full-screen boot splash shown on top of the home UI for ~duration_ms.
     * Fades out then self-destructs (see ShowOnboardSplash). */
    lv_obj_t *splash_ = nullptr;
    std::unique_ptr<LvglImage> splash_logo_;
};

} // namespace home
