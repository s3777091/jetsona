#pragma once

#include "display/views/overlay_view.h"
#include "display/widgets/telex_ime.h"
#include "net/bluetooth_manager.h"
#include "net/wifi_manager.h"

#include <lvgl.h>
#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace home {

/* Firmware settings hub: a compact category rail + an iPhone-inspired detail
 * pane that rebuilds on selection. Categories: Display
 * (brightness 20..100 via a software scrim), Sound (volume, UI-only), WiFi
 * (radio toggle + scan + per-network info/login/forget modal), Bluetooth
 * (power toggle + scan + the same connect-sheet/details/forget flow), and one
 * General category. General owns the Keyboard, Language & Region, Date & Time,
 * Fonts, Power & Lock, and About sub-pages instead of exposing those as six
 * unrelated top-level categories.
 *
 * Connectivity actions depend on IWifiManager/IBluetoothManager rather than
 * concrete singletons, while common LVGL locking and signal indicators are
 * shared components. Modal popups use the DocumentsView backdrop-dismiss
 * pattern. Home wires brightness/volume/lock through the setter hooks below. */
class SettingsView : public OverlayView {
public:
    SettingsView(lv_obj_t *parent, int width, int height,
                 jetson::IWifiManager &wifi, jetson::IBluetoothManager &bluetooth,
                 ClosedCb on_closed);

    void SetBrightnessApplier(std::function<void(int)> cb) { brightness_cb_ = std::move(cb); }
    void SetDisplayPreferencesApplier(std::function<void()> cb) {
        display_preferences_cb_ = std::move(cb);
    }
    void SetVolumeApplier(std::function<void(int, bool)> cb) { volume_cb_ = std::move(cb); }
    void SetLockRequest(std::function<void()> cb) { lock_cb_ = std::move(cb); }
    void SetNotificationApplier(std::function<void(const char *, int)> cb) {
        notification_cb_ = std::move(cb);
    }

    // Status-bar connectivity shortcuts reuse the Settings window instead of
    // opening a second full-screen overlay with a different header.  Keeping
    // both pages here gives them the normal traffic-light controls and the
    // same top inset as every other multitasked app.
    void ShowWifiPage();
    void ShowBluetoothPage();

protected:
    void OnStart() override;
    void OnResize(int w, int h) override;

private:
    // Dependencies are non-owning and must outlive the view. Production uses
    // the process-wide managers; tests can provide deterministic fakes.
    jetson::IWifiManager &wifi_;
    jetson::IBluetoothManager &bluetooth_;

    enum class Cat { Display, Sound, Wifi, Bluetooth, General };

    enum class DisplayPage { Main, TextSize, NightShift, AutoLock, AlwaysOn };
    enum class GeneralPage {
        Main,
        Keyboard,
        LanguageRegion,
        LanguagePicker,
        RegionPicker,
        Calendar,
        DateTime,
        Fonts,
        SystemFonts,
        MyFonts,
        CloudFonts,
        Power,
        LockTimeout,
        About,
    };

    struct SideCtx { SettingsView *self; Cat cat; };
    struct WifiRowCtx { SettingsView *self; jetson::WifiNetwork network; };
    struct BtRowCtx { SettingsView *self; jetson::BtDevice device; };
    struct OptCtx { SettingsView *self; std::string value; }; // timezone / sleep option
    struct FontCtx {
        SettingsView *self;
        std::string name;
        std::string regular_path;
        std::string bold_path;
        std::string regular_object;
        std::string bold_object;
    };

    // Layout.
    lv_obj_t *sidebar_ = nullptr;
    lv_obj_t *detail_ = nullptr;
    Cat current_ = Cat::Display;
    DisplayPage display_page_ = DisplayPage::Main;
    GeneralPage general_page_ = GeneralPage::Main;
    std::vector<lv_obj_t *> side_rows_;

    // Global airplane/VPN rows at the top of the sidebar.
    lv_obj_t *airplane_row_ = nullptr;
    lv_obj_t *airplane_icon_bg_ = nullptr;
    lv_obj_t *airplane_switch_ = nullptr;
    bool airplane_enabled_ = false;
    std::atomic<bool> airplane_busy_{false};
    lv_obj_t *vpn_row_ = nullptr;
    lv_obj_t *vpn_icon_bg_ = nullptr;
    lv_obj_t *vpn_switch_ = nullptr;
    bool vpn_enabled_ = false;
    std::atomic<bool> vpn_busy_{false};

    // WiFi pane.
    lv_obj_t *wifi_switch_ = nullptr;
    lv_obj_t *wifi_reload_btn_ = nullptr;
    lv_obj_t *wifi_list_ = nullptr;
    std::vector<jetson::WifiNetwork> wifi_nets_;
    bool wifi_scanned_ = false;
    bool wifi_enabled_ = false;
    std::atomic<bool> wifi_busy_{false};

    // Bluetooth pane.
    lv_obj_t *bt_switch_ = nullptr;
    lv_obj_t *bt_reload_btn_ = nullptr;
    lv_obj_t *bt_list_ = nullptr;
    std::vector<jetson::BtDevice> bt_devs_;
    bool bt_scanned_ = false;
    bool bt_powered_ = false;
    std::atomic<bool> bt_busy_{false};

    // Keyboard pane.
    TelexInput *kbd_demo_ = nullptr;
    lv_obj_t *lang_vi_btn_ = nullptr;
    lv_obj_t *lang_en_btn_ = nullptr;

    // Display / Sound pane widgets.
    lv_obj_t *bright_slider_ = nullptr;
    lv_obj_t *bright_value_label_ = nullptr;
    lv_obj_t *text_size_slider_ = nullptr;
    lv_obj_t *text_size_value_label_ = nullptr;
    lv_obj_t *night_warmth_slider_ = nullptr;
    lv_obj_t *vol_slider_ = nullptr;
    lv_obj_t *mute_switch_ = nullptr;
    lv_obj_t *vol_icon_ = nullptr; // speaker / speaker-mute PNG beside the slider

    // General / cloud-font pane state.
    lv_obj_t *font_status_label_ = nullptr;
    std::atomic<bool> font_busy_{false};
    bool font_catalog_requested_ = false;
    std::string font_status_;

    // Modal (built on overlay_).
    lv_obj_t *popup_ = nullptr;
    lv_obj_t *popup_card_ = nullptr;
    lv_obj_t *popup_confirm_btn_ = nullptr;
    TelexInput *popup_input_ = nullptr;     // WiFi password / PIN entry
    TelexInput *pin_a_ = nullptr;           // PIN set: first field
    TelexInput *pin_b_ = nullptr;           // PIN set: confirm field
    std::string modal_ssid_;
    jetson::WifiNetwork modal_wifi_;
    std::string modal_bt_addr_;
    bool modal_bt_connected_ = false;
    std::function<void()> modal_yes_;       // confirm-modal action

    // Home hooks.
    std::function<void(int)> brightness_cb_;
    std::function<void()> display_preferences_cb_;
    std::function<void(int, bool)> volume_cb_;  // (volume, muted)
    std::function<void()> lock_cb_;
    std::function<void(const char *, int)> notification_cb_;
    void Notify(const char *message, int duration_ms = 2800) {
        if (notification_cb_) notification_cb_(message, duration_ms);
    }

    // ---- layout / panes ----
    void BuildShell();
    void AddAirplaneRow();
    void AirplaneRefreshUi();
    void AddVpnRow();
    void VpnRefreshUi();
    void RefreshVpnStatus();
    void AddSidebarRow(Cat cat, const char *icon, const char *label);
    void HighlightSide(Cat cat);
    void ShowCategory(Cat c);
    void ClearDetail();
    lv_obj_t *SectionTitle(const char *text);
    lv_obj_t *MakeRow(const char *title, const char *sub = nullptr);
    lv_obj_t *MakeSwitch(lv_obj_t *parent, bool on, lv_event_cb_t cb);
    lv_obj_t *MakeSlider(lv_obj_t *parent, int minv, int maxv, int val, lv_event_cb_t cb);
    lv_obj_t *MakeButton(lv_obj_t *parent, const char *text, uint32_t bg, lv_event_cb_t cb);
    lv_obj_t *MakeReloadButton(lv_obj_t *parent, lv_event_cb_t cb);
    lv_obj_t *DisplayCard();
    lv_obj_t *DisplayRow(lv_obj_t *card, const char *title, const char *sub = nullptr,
                         int height = 48);
    void DisplayDivider(lv_obj_t *card);
    void DisplayPageHeader(const char *title, bool show_back);
    void DisplayCaption(const char *text);
    void MakeDisplayNavigationRow(lv_obj_t *card, const char *title, const char *value,
                                  lv_event_cb_t cb);

    void BuildDisplay();
    void BuildDisplayMain();
    void BuildTextSizePage();
    void BuildNightShiftPage();
    void BuildAutoLockPage();
    void BuildAlwaysOnPage();
    void BuildSound();
    void BuildWifi();
    void BuildBluetooth();
    void BuildGeneral();
    void BuildGeneralMain();
    void BuildGeneralKeyboard();
    void BuildLanguageRegion();
    void BuildLanguagePicker();
    void BuildRegionPicker();
    void BuildCalendarPicker();
    void BuildGeneralDateTime();
    void BuildFonts();
    void BuildLocalFonts(bool personal);
    void BuildCloudFonts();
    void BuildGeneralPower();
    void BuildGeneralLockTimeout();
    void BuildGeneralAbout();
    void GeneralPageHeader(const char *title);
    void MakeOptionRow(lv_obj_t *card, const char *title, const char *sub,
                       bool selected, lv_event_cb_t cb, const std::string &value);
    void MakeFontRow(lv_obj_t *card, const std::string &name,
                     const std::string &regular_path, const std::string &bold_path,
                     const std::string &regular_object = "",
                     const std::string &bold_object = "");
    void SetFontStatus(const std::string &text);
    void RefreshCloudFontCatalog();
    void DownloadAndApplyFont(const FontCtx &font);

    // ---- WiFi ----
    void WifiRefreshSwitch();
    void WifiLoadState();
    void WifiRescan();
    void WifiRenderList();
    void WifiCreateRow(const jetson::WifiNetwork &n);
    void WifiOpenConnectSheet(const jetson::WifiNetwork &network);
    void WifiLoadDetails(const jetson::WifiNetwork &network);
    void WifiOpenDetails(const jetson::WifiDetails &details);
    void WifiDoConnect(const std::string &ssid, const std::string &pw);
    void WifiDoForget(const std::string &ssid);

    // ---- Bluetooth ----
    void BtRefreshSwitch();
    void BtLoadState();
    void BtRescan();
    void BtRenderList();
    void BtCreateRow(const jetson::BtDevice &d);
    void BtOpenConnectSheet(const jetson::BtDevice &d);
    void BtOpenDetails(const jetson::BtDevice &d);
    void BtDoAction(const std::string &addr, bool connected);
    void BtDoRemove(const std::string &addr);

    // ---- modal ----
    void CloseModal();
    void OpenConfirmModal(const char *title, const char *msg, std::function<void()> on_yes);
    void OpenPinModal();

    // ---- helpers ----
    std::shared_ptr<SettingsView> Self();
    static void OnSideClicked(lv_event_t *e);
    static void OnSideDeleted(lv_event_t *e);
    static void OnWifiRowDeleted(lv_event_t *e);
    static void OnBtRowDeleted(lv_event_t *e);
    static void OnOptDeleted(lv_event_t *e);
    static void OnFontDeleted(lv_event_t *e);

    static void OnBrightChanged(lv_event_t *e);
    static void OnDisplayBack(lv_event_t *e);
    static void OnOpenTextSize(lv_event_t *e);
    static void OnOpenNightShift(lv_event_t *e);
    static void OnOpenAutoLock(lv_event_t *e);
    static void OnOpenAlwaysOn(lv_event_t *e);
    static void OnTextSizeChanged(lv_event_t *e);
    static void OnBoldToggle(lv_event_t *e);
    static void OnTrueToneToggle(lv_event_t *e);
    static void OnNightShiftToggle(lv_event_t *e);
    static void OnNightWarmthChanged(lv_event_t *e);
    static void OnTouchWakeToggle(lv_event_t *e);
    static void OnAlwaysOnToggle(lv_event_t *e);
    static void OnAlwaysOnWallpaperToggle(lv_event_t *e);
    static void OnAlwaysOnBlurToggle(lv_event_t *e);
    static void OnAlwaysOnNotificationsToggle(lv_event_t *e);
    static void OnVolChanged(lv_event_t *e);
    static void OnMuteToggle(lv_event_t *e);
    static void OnAirplaneSwitch(lv_event_t *e);
    static void OnVpnSwitch(lv_event_t *e);

    static void OnWifiSwitch(lv_event_t *e);
    static void OnWifiRescan(lv_event_t *e);
    static void OnWifiRowClicked(lv_event_t *e);
    static void OnWifiInfoClicked(lv_event_t *e);
    static void OnBtSwitch(lv_event_t *e);
    static void OnBtRescan(lv_event_t *e);
    static void OnBtRowClicked(lv_event_t *e);
    static void OnBtInfoClicked(lv_event_t *e);

    static void OnLangVi(lv_event_t *e);
    static void OnLangEn(lv_event_t *e);
    static void OnGeneralBack(lv_event_t *e);
    static void OnOpenGeneralKeyboard(lv_event_t *e);
    static void OnOpenLanguageRegion(lv_event_t *e);
    static void OnOpenLanguagePicker(lv_event_t *e);
    static void OnOpenRegionPicker(lv_event_t *e);
    static void OnOpenCalendarPicker(lv_event_t *e);
    static void OnOpenGeneralDateTime(lv_event_t *e);
    static void OnOpenFonts(lv_event_t *e);
    static void OnOpenSystemFonts(lv_event_t *e);
    static void OnOpenMyFonts(lv_event_t *e);
    static void OnOpenCloudFonts(lv_event_t *e);
    static void OnOpenGeneralPower(lv_event_t *e);
    static void OnOpenGeneralAbout(lv_event_t *e);
    static void OnLanguageSelected(lv_event_t *e);
    static void OnRegionSelected(lv_event_t *e);
    static void OnCalendarSelected(lv_event_t *e);
    static void On24hToggle(lv_event_t *e);
    static void OnAutoTimeToggle(lv_event_t *e);
    static void OnTzSelected(lv_event_t *e);
    static void OnSleepSelected(lv_event_t *e);
    static void OnOpenGeneralLockTimeout(lv_event_t *e);
    static void OnFontSelected(lv_event_t *e);
    static void OnRefreshFontCatalog(lv_event_t *e);

    static void OnLockNow(lv_event_t *e);
    static void OnSetPin(lv_event_t *e);
    static void OnPinClear(lv_event_t *e);
    static void OnReboot(lv_event_t *e);
    static void OnShutdown(lv_event_t *e);

    static void OnPopupDismiss(lv_event_t *e);
    static void OnModalClose(lv_event_t *e);
    static void OnModalConnect(lv_event_t *e);
    static void OnModalPasswordChanged(lv_event_t *e);
    static void OnModalForget(lv_event_t *e);
    static void OnModalBtAction(lv_event_t *e);
    static void OnModalBtRemove(lv_event_t *e);
    static void OnModalConfirmYes(lv_event_t *e);
    static void OnPinSave(lv_event_t *e);
};

} // namespace home
