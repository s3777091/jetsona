#pragma once

#include "display/core/lcd_display.h"
#include "display/home/app_switcher.h"
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
class MusicView;
class OverlayView;
class PodsView;
class PsRemotePlayView;
class RemindersView;
class SettingsView;
class TerminalView;
class TrashView;
class WifiSettingsView;

/* Compact DS-02 home display for the Jetson 800x480 HDMI panel.
 * Reproduces the DS-02 standby (wallpaper + clock + system bar + dock) and
 * app drawer (swipe-up grid of app icons) with touch interaction. The dock
 * carries the backed apps (calendar / documents / reminders / settings / chat /
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
    void OpenMusic();
    void OpenReminders();
    void OpenBackgroundGallery();
    void OpenSettings();
    void OpenChat();
    void OpenTerminal();
    void OpenTrash();
    void OpenPsRemotePlay();
    /* RunPod GPU pod manager (drawer "Pods"): rent, start/stop, delete pods
     * and jump into a pod's web IDE. Needs RUNPOD_API_KEY in .env. */
    void OpenPods();
    /* Drawer "Studio": open the self-hosted code-server on the VM
     * (JETSON_STUDIO_URL, vm/code-server/deploy.py) in the Chromium kiosk.
     * GPU pods are opened explicitly from the Pods app instead. */
    void OpenStudio();
    /* Hand the HDMI panel to a Chromium kiosk: stops the FBDEV render loop,
     * then either exits with code 42 (under the jetson-fw supervisor, which
     * runs launch_chromium.sh and restarts us) or -- when started without a
     * supervisor -- runs the kiosk in-process and re-execs this binary when
     * the browser closes. Not an OverlayView app; the whole UI is suspended
     * while the kiosk owns the panel. A non-empty `url` is handed to the
     * kiosk via /tmp/jetson_chromium_url (read by launch_chromium.sh).
     * `captive_portal` enables the launcher's touch keyboard and automatic
     * return once unrestricted Internet access has been confirmed. */
    void OpenChromium(const std::string &url = std::string(),
                      bool captive_portal = false);
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

    // Keep Dock order in one place: click routing, running dots and switcher
    // artwork all use these names instead of duplicating fragile integers.
    enum DockIndex : size_t {
        kDockFinder = 0,
        kDockCalendar,
        kDockDocuments,
        kDockMusic,
        kDockReminders,
        kDockSettings,
        kDockEkkoBot,
        kDockTerminal,
        kDockNightOwl,
        kDockTranslate,
        kDockTrash,
        kDockItemCount,
    };
    static constexpr size_t kDrawerItemCount = 15;
    static constexpr size_t kDrawerItemsPerPage = 8;
    static constexpr size_t kDrawerPageCount =
        (kDrawerItemCount + kDrawerItemsPerPage - 1) / kDrawerItemsPerPage;
    static constexpr size_t kGalleryDrawerIndex = 5;
    static constexpr size_t kPodsDrawerIndex = 6;
    static constexpr size_t kPsRemotePlayDrawerIndex = 2;

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
        kAppMusic,
        kAppReminders,
        kAppSettings,
        kAppChat,
        kAppTerminal,
        kAppTrash,
        kAppGallery, // drawer app: gets a temporary dock slot while running
        kAppPsRemotePlay,
        kAppPods,    // drawer app: RunPod GPU manager
    };
    static constexpr size_t kMaxRunningApps = 5;

    struct AppCtx {
        Ds02HomeDisplay *self;
        int id;
    };

    void CreateStandbyObjects();
    void StartWeatherUpdater();
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
    static void OnDrawerScrollEnd(lv_event_t *e);
    static void OnScreenOffClicked(lv_event_t *e);
    void UpdateDrawerPageDots();
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
    /* Release the framebuffer and hand the panel to chiaki-ng. `configure`
     * opens chiaki-ng's official registration UI; stream mode starts the
     * selected PS5 directly. The supervisor maps these to exit 43/44. */
    void LaunchPsRemotePlay(bool configure);
    static void OnGalleryDockClicked(lv_event_t *e);
    void ToggleVolume();
    static void OnSplashOpa(void *var, int32_t v);
    static void OnSplashBar(void *var, int32_t v);
    static void OnSplashGone(lv_anim_t *a);

    /* First half of the Chromium hand-off zoom: the app card collapses into
     * the island and leaves the panel black, which is the frame the
     * framebuffer keeps while Xorg starts. jetson_kiosk_bar then blooms the
     * same card back out, so the two processes read as one animation.
     * Returns the milliseconds to wait before tearing LVGL down. */
    int PlayChromiumHandoff(const std::string &url);
    static void OnHandoffCollapse(void *var, int32_t v);

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
    // Global quick-settings/clock bar on lv_layer_top(), visible on every
    // screen. Self-refreshes; home wires the functional controls.
    std::unique_ptr<StatusBar> status_bar_;
    bool volume_muted_ = false;
    // Latest formatted weather line; shown by the lock screen, which is usually
    // closed when the updater thread delivers one.
    std::string weather_line_;
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
    std::shared_ptr<MusicView> music_view_;
    std::shared_ptr<RemindersView> reminders_view_;
    std::shared_ptr<BackgroundGalleryView> gallery_view_;
    std::shared_ptr<SettingsView> settings_view_;
    std::shared_ptr<ChatView> chat_view_;
    std::shared_ptr<TerminalView> terminal_view_;
    std::shared_ptr<TrashView> trash_view_;
    std::shared_ptr<PsRemotePlayView> ps_remote_play_view_;
    std::shared_ptr<PodsView> pods_view_;
    std::shared_ptr<LockScreenView> lock_screen_view_;
    std::shared_ptr<jetson::Conversation> chat_conv_;

    lv_obj_t *app_grid_ = nullptr;
    lv_obj_t *drawer_page_indicator_ = nullptr;
    std::array<lv_obj_t *, kDrawerPageCount> drawer_pages_ = {};
    std::array<lv_obj_t *, kDrawerPageCount> drawer_page_dots_ = {};
    size_t drawer_page_index_ = 0;

    /* Full-screen boot greeting shown for ~duration_ms. It fades away, then
     * hands off to the welcome animation in the Dynamic Island. */
    lv_obj_t *splash_ = nullptr;
    std::unique_ptr<LvglImage> splash_logo_;
    // Chromium hand-off card. Lives until the process exits, so nothing
    // tears it down again.
    lv_obj_t *handoff_cover_ = nullptr;
    lv_obj_t *handoff_card_ = nullptr;
    lv_obj_t *handoff_icon_ = nullptr;
    std::unique_ptr<LvglImage> handoff_icon_image_;
    int handoff_icon_w_ = 0, handoff_icon_h_ = 0;
};

} // namespace home
