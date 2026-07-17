#pragma once

/* Minimal Remote Play welcome screen for the 800x480 DS-02 display.
 *
 * The screen intentionally stays close to Sony's lightweight welcome view:
 * one live controller-state icon, one sign-in action, and a compact settings
 * sheet.  Runtime/install diagnostics belong outside this user-facing view.
 */

#include "display/views/overlay_view.h"
#include "display/widgets/telex_ime.h"

#include <lvgl.h>

#include <functional>
#include <string>
#include <utility>

namespace home {

class PsRemotePlayView : public OverlayView {
public:
    using LaunchCb = std::function<void(bool configure)>;
    using NotifyCb = std::function<void(const char *)>;
    using OpenBluetoothCb = std::function<void()>;

    PsRemotePlayView(lv_obj_t *parent, int width, int height, ClosedCb on_closed);
    ~PsRemotePlayView() override;

    // Kept for the existing launcher hand-off; this minimal form does not
    // trigger an external client by itself.
    void SetLaunchRequest(LaunchCb cb) { launch_cb_ = std::move(cb); }
    void SetNotifyCb(NotifyCb cb) { notify_cb_ = std::move(cb); }
    void SetOpenBluetoothCb(OpenBluetoothCb cb) {
        open_bluetooth_cb_ = std::move(cb);
    }

protected:
    void OnStart() override;

private:
    enum class Preset { Performance, Quality };

    // Minimal welcome content.
    lv_obj_t *controller_icon_ = nullptr;
    lv_obj_t *controller_state_label_ = nullptr;
    lv_obj_t *sign_in_btn_ = nullptr;

    // PIN bottom sheet.
    lv_obj_t *pin_modal_ = nullptr;
    TelexInput *pin_input_ = nullptr;
    lv_obj_t *pin_error_ = nullptr;

    // Settings bottom sheet.
    lv_obj_t *settings_modal_ = nullptr;
    lv_obj_t *settings_card_ = nullptr;
    lv_obj_t *settings_controller_label_ = nullptr;
    lv_obj_t *settings_ps5_name_label_ = nullptr;
    lv_obj_t *settings_ip_input_ = nullptr;
    lv_obj_t *settings_ip_error_ = nullptr;
    lv_obj_t *performance_card_ = nullptr;
    lv_obj_t *quality_card_ = nullptr;
    lv_obj_t *performance_radio_ = nullptr;
    lv_obj_t *quality_radio_ = nullptr;

    LaunchCb launch_cb_;
    NotifyCb notify_cb_;
    OpenBluetoothCb open_bluetooth_cb_;
    lv_timer_t *controller_timer_ = nullptr;

    Preset preset_ = Preset::Performance;
    Preset draft_preset_ = Preset::Performance;
    std::string host_;
    std::string ps5_name_;
    std::string controller_name_;
    bool controller_connected_ = false;

    void BuildBody();
    void LoadState();
    void RefreshControllerState();
    void UpdateWelcomeUi();
    void UpdateSettingsUi();
    void UpdatePresetCards();
    void Notify(const char *message);

    void OpenPinModal();
    void ClosePinModal();
    void AcceptPin();

    void OpenSettingsModal();
    void CloseSettingsModal();
    void SaveSettingsModal();
    void OpenBluetoothSettings();

    static void OnSignIn(lv_event_t *e);
    static void OnPinDismiss(lv_event_t *e);
    static void OnPinCancel(lv_event_t *e);
    static void OnPinSave(lv_event_t *e);
    static void OnSettingsDismiss(lv_event_t *e);
    static void OnSettingsCancel(lv_event_t *e);
    static void OnSettingsSave(lv_event_t *e);
    static void OnConnectController(lv_event_t *e);
    static void OnPerformance(lv_event_t *e);
    static void OnQuality(lv_event_t *e);
    static void OnControllerPoll(lv_timer_t *timer);
};

} // namespace home
