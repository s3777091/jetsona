#pragma once

#include "lcd_display.h"
#include "lvgl_image.h"
#include "wifi_settings_view.h"
#include "bluetooth_settings_view.h"
#include "calendar_view.h"
#include "background_gallery_view.h"
#include "settings_view.h"
#include "chat_view.h"
#include "terminal_view.h"
#include "conversation.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <functional>
#include <memory>
#include <string>

namespace home {

/* Compact DS-02 home display for the Jetson 800x480 HDMI panel.
 * Reproduces the DS-02 standby (wallpaper + clock + system bar + dock) and
 * launcher (avatar sphere) look with touch interaction. This is the phase-1
 * UI; the full upstream ds02_home_display (calendar / settings / app drawer /
 * background gallery) drops in here later. */
class Ds02HomeDisplay : public SpiLcdDisplay {
public:
    Ds02HomeDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                    int width, int height, int offset_x, int offset_y,
                    bool mirror_x, bool mirror_y, bool swap_xy);
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
    void OpenBackgroundGallery();
    void OpenSettings();
    void OpenChat();
    void OpenTerminal();
    void ApplyBackgroundIndexFromGallery(size_t index);

    static constexpr size_t kBackgroundCount = 10;

private:
    enum class StandbyState { Dim, Awake, Launcher };

    static constexpr size_t kDockItemCount = 6;

    struct AppCtx {
        Ds02HomeDisplay *self;
        int id;
    };

    void CreateStandbyObjects();
    void CreateLauncherObjects();
    void CreateSystemBarObjects();
    void CreateDockObjects();
    void CreateSphere(lv_obj_t *parent, int size);
    void RefreshClock();
    void RefreshBattery();
    void ApplyStandbyState();
    void RepaintForTheme();
    bool ApplyBackgroundIndex(size_t index);
    LvglImage *GetBackgroundImage(size_t index);
    const char *GetBackgroundFile(size_t index) const;
    void SetTextColor(uint32_t color);
    static void OnRefreshTimer(void *arg);
    static void OnDockButtonClicked(lv_event_t *e);
    static void OnAppButtonClicked(lv_event_t *e);
    static void OnAppDeleted(lv_event_t *e);
    static void OnStandbyGesture(lv_event_t *e);
    static void OnSphereRotate(lv_timer_t *t);
    static std::string FormatTime(const struct tm &t);
    static std::string FormatDate(const struct tm &t);

    StandbyState standby_state_ = StandbyState::Awake;
    esp_timer_handle_t refresh_timer_ = nullptr;

    lv_obj_t *root_ = nullptr;
    lv_obj_t *standby_layer_ = nullptr;
    lv_obj_t *wallpaper_ = nullptr;
    lv_obj_t *wallpaper_image_obj_ = nullptr;
    lv_obj_t *dim_overlay_ = nullptr;
    lv_obj_t *system_bar_ = nullptr;
    lv_obj_t *wifi_label_ = nullptr;
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
    lv_obj_t *avatar_sphere_ = nullptr;
    lv_obj_t *dock_ = nullptr;
    std::array<lv_obj_t *, kDockItemCount> dock_buttons_ = {};

    std::array<std::unique_ptr<LvglImage>, kBackgroundCount> background_image_cache_ = {};
    size_t background_index_ = 0;
    uint32_t text_color_ = 0xffffff;
    std::string cached_time_;
    std::string cached_date_;

    std::shared_ptr<WifiSettingsView> wifi_view_;
    std::shared_ptr<BluetoothSettingsView> bt_view_;
    std::shared_ptr<CalendarView> calendar_view_;
    std::shared_ptr<BackgroundGalleryView> gallery_view_;
    std::shared_ptr<SettingsView> settings_view_;
    std::shared_ptr<ChatView> chat_view_;
    std::shared_ptr<TerminalView> terminal_view_;
    std::shared_ptr<jetson::Conversation> chat_conv_;

    lv_timer_t *sphere_timer_ = nullptr;
    int sphere_angle_ = 0;
    lv_obj_t *app_grid_ = nullptr;
};

} // namespace home