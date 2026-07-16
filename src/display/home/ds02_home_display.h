#pragma once

#include "display/core/lcd_display.h"
#include "display/home/app_switcher.h"
#include "display/widgets/optimize_widget.h"
#include "display/widgets/status_bar.h"

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
class OverlayView;
class SettingsView;
class TerminalView;
class TrashView;
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
    void OpenTrash();
    void OpenLockScreen();
    void SetBrightness(int pct);
    void ApplyDisplayPreferences();
    void ApplyBackgroundFromFile(const std::string &file);
    void SetSleepBackground(const std::string &file);
    void ReloadBackgrounds();

    /* The wallpaper set is scanned from disk at runtime (backgrounds::ListBackgroundFiles)
     * and cached by filename; the gallery can move files to Trash and the list updates. */

private:
    jetson::IWifiManager &wifi_;
    jetson::IBluetoothManager &bluetooth_;

    enum class StandbyState { Dim, Awake, Launcher };

    static constexpr size_t kDockItemCount = 9;
    static constexpr size_t kDrawerItemCount = 8;

    /* ---- Multitasking ----
     * Every OverlayView app that opens joins an LRU queue and stays alive when
     * sent to the background (hidden, zero render cost -- LVGL skips hidden
     * subtrees), so switching back never rebuilds or reloads the view. The
     * queue holds at most kMaxRunningApps; opening one more closes the least
     * recently used app. Dock dots mark the running set; the Dynamic Island
     * click opens the card switcher. */
    enum AppId {
        kAppNone = 0,
        kAppCalendar,
        kAppDocuments,
        kAppSettings,
        kAppChat,
        kAppTerminal,
        kAppTrash,
        kAppGallery, // drawer app: gets a temporary dock slot while running
    };
    static constexpr size_t kMaxRunningApps = 5;

    struct AppCtx {
        Ds02HomeDisplay *self;
        int id;
    };

    void CreateStandbyObjects();
    void CreateDrawerObjects();
    void CreateSystemBarObjects();
    void CreateDockObjects();
    void SetDockActive(int index);
    void CheckIdleDim();
    void ApplyStandbyState();
    bool HasOpenOverlay() const;
    void RepaintForTheme();
    bool ApplyBackgroundFile(const std::string &file);
    void ApplyWallpaperForState();
    LvglImage *GetBackgroundImage(const std::string &file);
    void SetTextColor(uint32_t color);
    static void OnRefreshTimer(void *arg);
    static void OnDockButtonEvent(lv_event_t *e);
    static void OnAppButtonClicked(lv_event_t *e);
    static void OnAppDeleted(lv_event_t *e);
    static void OnScreenOffClicked(lv_event_t *e);
    // Multitask queue plumbing.
    OverlayView *GetAppView(AppId id) const;
    void NoteAppOpened(AppId id);
    void RestoreApp(AppId id);
    void BackgroundApp(AppId id);
    void CloseApp(AppId id);
    void OnAppClosed(AppId id);
    void SnapshotApp(AppId id);
    void FreeSnapshot(AppId id);
    void UpdateDockDots();
    void OpenAppSwitcher();
    void AddGalleryDockItem();
    void RemoveGalleryDockItem();
    static void OnGalleryDockClicked(lv_event_t *e);
    void ToggleVolume();
    static void OnSplashOpa(void *var, int32_t v);
    static void OnSplashBar(void *var, int32_t v);
    static void OnSplashGone(lv_anim_t *a);

    StandbyState standby_state_ = StandbyState::Awake;
    esp_timer_handle_t refresh_timer_ = nullptr;

    lv_obj_t *root_ = nullptr;
    lv_obj_t *standby_layer_ = nullptr;
    // Full-screen software display effects. The HDMI panel does not expose a
    // controllable backlight, so brightness and color temperature are rendered
    // as non-interactive scrims. screen_off_overlay_ is a root child above the
    // home UI (but below app overlays) and receives tap-to-wake input.
    lv_obj_t *brightness_overlay_ = nullptr;
    lv_obj_t *tone_overlay_ = nullptr;
    lv_obj_t *screen_off_overlay_ = nullptr;
    lv_obj_t *wallpaper_ = nullptr;
    lv_obj_t *wallpaper_image_obj_ = nullptr;
    lv_obj_t *dim_overlay_ = nullptr;
    // Global wifi/bt/battery/volume/clock bar on lv_layer_top(), visible on
    // every screen. Self-refreshes; home wires the click hooks.
    std::unique_ptr<StatusBar> status_bar_;
    // "Tối ưu" pill on the standby layer: live disk/RAM usage bars plus a
    // drop-caches button. Hides together with the standby layer.
    std::unique_ptr<OptimizeWidget> optimize_widget_;
    bool volume_muted_ = false;
    lv_obj_t *weather_label_ = nullptr;
    lv_obj_t *chat_label_ = nullptr;
    lv_obj_t *launcher_layer_ = nullptr;
    lv_obj_t *dock_ = nullptr;
    std::array<lv_obj_t *, kDockItemCount> dock_buttons_ = {};
    std::array<lv_obj_t *, kDockItemCount> dock_indicators_ = {};
    std::array<std::unique_ptr<LvglImage>, kDockItemCount> dock_icon_cache_ = {};
    int dock_active_index_ = -1;
    std::array<std::unique_ptr<LvglImage>, kDrawerItemCount> drawer_icon_cache_ = {};

    // Multitask state: LRU order (front = oldest, back = most recent).
    std::vector<AppId> task_queue_;
    AppId foreground_app_ = kAppNone;
    std::map<int, lv_draw_buf_t *> app_snapshots_; // switcher thumbnails
    std::unique_ptr<AppSwitcher> app_switcher_;
    int dock_base_width_ = 0;
    lv_obj_t *gallery_dock_item_ = nullptr; // temp dock slot while Gallery runs

    std::map<std::string, std::unique_ptr<LvglImage>> background_image_cache_;
    std::vector<std::string> background_files_;
    std::string background_file_;       // current desktop wallpaper filename
    std::string sleep_background_file_; // wallpaper shown when the screen is dim
    uint32_t text_color_ = 0xffffff;

    std::shared_ptr<WifiSettingsView> wifi_view_;
    std::shared_ptr<BluetoothSettingsView> bt_view_;
    std::shared_ptr<CalendarView> calendar_view_;
    std::shared_ptr<DocumentsView> documents_view_;
    std::shared_ptr<BackgroundGalleryView> gallery_view_;
    std::shared_ptr<SettingsView> settings_view_;
    std::shared_ptr<ChatView> chat_view_;
    std::shared_ptr<TerminalView> terminal_view_;
    std::shared_ptr<TrashView> trash_view_;
    std::shared_ptr<LockScreenView> lock_screen_view_;
    std::shared_ptr<jetson::Conversation> chat_conv_;

    lv_obj_t *app_grid_ = nullptr;

    /* Full-screen boot greeting shown for ~duration_ms. It fades away, then
     * hands off to the welcome animation in the Dynamic Island. */
    lv_obj_t *splash_ = nullptr;
    std::unique_ptr<LvglImage> splash_logo_;
};

} // namespace home
