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

/* macOS System Settings-style hub: a sidebar of categories + a detail pane
 * that rebuilds on selection. Categories: Appearance (theme), Display
 * (brightness 15..100 via a software scrim), Sound (volume, UI-only), WiFi
 * (radio toggle + scan + per-network info/login/forget modal), Bluetooth
 * (power toggle + scan + per-device connect/disconnect/forget modal), Keyboard
 * (vi/en with a live Telex IME demo field), Date & Time (timezone via
 * timedatectl + 12h/24h), Power & Security (sleep timeout, lock screen, set
 * PIN, reboot, shutdown), About (device + storage + memory).
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
    void SetVolumeApplier(std::function<void(int, bool)> cb) { volume_cb_ = std::move(cb); }
    void SetLockRequest(std::function<void()> cb) { lock_cb_ = std::move(cb); }

protected:
    void OnStart() override;
    void OnResize(int w, int h) override;

private:
    // Dependencies are non-owning and must outlive the view. Production uses
    // the process-wide managers; tests can provide deterministic fakes.
    jetson::IWifiManager &wifi_;
    jetson::IBluetoothManager &bluetooth_;

    enum class Cat {
        Appearance, Display, Sound, Wifi, Bluetooth, Keyboard, DateTime, Power, About
    };

    struct SideCtx { SettingsView *self; Cat cat; };
    struct WifiRowCtx { SettingsView *self; std::string ssid; bool secured; bool in_use; int signal; };
    struct BtRowCtx { SettingsView *self; std::string addr; };
    struct OptCtx { SettingsView *self; std::string value; }; // timezone / sleep option

    // Layout.
    lv_obj_t *sidebar_ = nullptr;
    lv_obj_t *detail_ = nullptr;
    Cat current_ = Cat::Appearance;
    std::vector<lv_obj_t *> side_rows_;

    // WiFi pane.
    lv_obj_t *wifi_switch_ = nullptr;
    lv_obj_t *wifi_list_ = nullptr;
    std::vector<jetson::WifiNetwork> wifi_nets_;
    bool wifi_scanned_ = false;
    bool wifi_enabled_ = false;
    std::atomic<bool> wifi_busy_{false};

    // Bluetooth pane.
    lv_obj_t *bt_switch_ = nullptr;
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
    lv_obj_t *vol_slider_ = nullptr;
    lv_obj_t *mute_switch_ = nullptr;

    // Modal (built on overlay_).
    lv_obj_t *popup_ = nullptr;
    lv_obj_t *popup_card_ = nullptr;
    TelexInput *popup_input_ = nullptr;     // WiFi password / PIN entry
    TelexInput *pin_a_ = nullptr;           // PIN set: first field
    TelexInput *pin_b_ = nullptr;           // PIN set: confirm field
    std::string modal_ssid_;
    std::string modal_bt_addr_;
    bool modal_bt_connected_ = false;
    std::function<void()> modal_yes_;       // confirm-modal action

    // Home hooks.
    std::function<void(int)> brightness_cb_;
    std::function<void(int, bool)> volume_cb_;  // (volume, muted)
    std::function<void()> lock_cb_;

    // ---- layout / panes ----
    void BuildShell();
    void AddSidebarRow(Cat cat, const char *glyph, const char *label);
    void HighlightSide(Cat cat);
    void ShowCategory(Cat c);
    void ClearDetail();
    lv_obj_t *SectionTitle(const char *text);
    lv_obj_t *MakeRow(const char *title, const char *sub = nullptr);
    lv_obj_t *MakeSwitch(lv_obj_t *parent, bool on, lv_event_cb_t cb);
    lv_obj_t *MakeSlider(lv_obj_t *parent, int minv, int maxv, int val, lv_event_cb_t cb);
    lv_obj_t *MakeButton(lv_obj_t *parent, const char *text, uint32_t bg, lv_event_cb_t cb);

    void BuildAppearance();
    void BuildDisplay();
    void BuildSound();
    void BuildWifi();
    void BuildBluetooth();
    void BuildKeyboard();
    void BuildDateTime();
    void BuildPower();
    void BuildAbout();

    // ---- WiFi ----
    void WifiRefreshSwitch();
    void WifiRescan();
    void WifiRenderList();
    void WifiCreateRow(const jetson::WifiNetwork &n);
    void WifiOpenModal(const WifiRowCtx &info);
    void WifiDoConnect(const std::string &ssid, const std::string &pw);
    void WifiDoForget(const std::string &ssid);

    // ---- Bluetooth ----
    void BtRefreshSwitch();
    void BtRescan();
    void BtRenderList();
    void BtCreateRow(const jetson::BtDevice &d);
    void BtOpenModal(const std::string &addr);
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

    static void OnThemeToggle(lv_event_t *e);
    static void OnBrightChanged(lv_event_t *e);
    static void OnVolChanged(lv_event_t *e);
    static void OnMuteToggle(lv_event_t *e);

    static void OnWifiSwitch(lv_event_t *e);
    static void OnWifiRescan(lv_event_t *e);
    static void OnWifiRowClicked(lv_event_t *e);
    static void OnBtSwitch(lv_event_t *e);
    static void OnBtRescan(lv_event_t *e);
    static void OnBtRowClicked(lv_event_t *e);

    static void OnLangVi(lv_event_t *e);
    static void OnLangEn(lv_event_t *e);
    static void On24hToggle(lv_event_t *e);
    static void OnTzSelected(lv_event_t *e);
    static void OnSleepSelected(lv_event_t *e);

    static void OnLockNow(lv_event_t *e);
    static void OnSetPin(lv_event_t *e);
    static void OnPinClear(lv_event_t *e);
    static void OnReboot(lv_event_t *e);
    static void OnShutdown(lv_event_t *e);

    static void OnPopupDismiss(lv_event_t *e);
    static void OnModalClose(lv_event_t *e);
    static void OnModalConnect(lv_event_t *e);
    static void OnModalForget(lv_event_t *e);
    static void OnModalBtAction(lv_event_t *e);
    static void OnModalBtRemove(lv_event_t *e);
    static void OnModalConfirmYes(lv_event_t *e);
    static void OnPinSave(lv_event_t *e);
};

} // namespace home
