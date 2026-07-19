#pragma once

/* Minimal Remote Play welcome screen for the 800x480 DS-02 display.
 *
 * The screen intentionally stays close to Sony's lightweight welcome view:
 * one live controller-state icon, one play action, and a compact settings
 * sheet. Runtime/install diagnostics belong outside this user-facing view.
 */

#include "display/views/overlay_view.h"

#include <lvgl.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace home {

struct ControllerTestSession;

class PsRemotePlayView : public OverlayView {
public:
    using LaunchCb = std::function<void(bool configure)>;
    using NotifyCb = std::function<void(const char *)>;
    using OpenBluetoothCb = std::function<void()>;

    PsRemotePlayView(lv_obj_t *parent, int width, int height, ClosedCb on_closed);
    ~PsRemotePlayView() override;

    // Launcher hand-off used by both the PS5 setup row and "Chơi ngay".
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
    lv_obj_t *controller_hint_label_ = nullptr;
    lv_obj_t *play_btn_ = nullptr;

    // Live gamepad test overlay. The session owns the non-blocking Linux
    // input fd(s), the current input snapshot and its LVGL control objects.
    lv_obj_t *controller_test_modal_ = nullptr;
    lv_timer_t *controller_input_timer_ = nullptr;
    std::unique_ptr<ControllerTestSession> controller_test_;

    // Settings bottom sheet.
    lv_obj_t *settings_modal_ = nullptr;
    lv_obj_t *settings_card_ = nullptr;
    lv_obj_t *settings_controller_label_ = nullptr;
    lv_obj_t *settings_ps5_name_label_ = nullptr;
    lv_obj_t *settings_ps5_status_icon_ = nullptr;
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
    std::string controller_path_;
    bool controller_connected_ = false;
    bool controller_readable_ = false;
    bool controller_uses_evdev_ = true;
    bool ps5_registered_ = false;

    void BuildBody();
    void LoadState();
    void RefreshPs5State();
    void RefreshControllerState();
    void UpdateWelcomeUi();
    void UpdateSettingsUi();
    void UpdatePresetCards();
    void Notify(const char *message);

    void OpenControllerTest();
    void CloseControllerTest();
    void PollControllerInput();
    void UpdateControllerTestUi();

    void OpenPs5Setup();
    void StartStream();

    void OpenSettingsModal();
    void CloseSettingsModal();
    void SaveSettingsModal();
    void OpenBluetoothSettings();

    /* Launcher-facing state. The stream/configure hand-off scripts read
     * /var/lib/jetson-fw/ps-remote-play.conf, not the firmware Settings
     * store; this keeps the two in sync. */
    void WriteLauncherState() const;

    static void OnPlay(lv_event_t *e);
    static void OnSettingsDismiss(lv_event_t *e);
    static void OnSettingsCancel(lv_event_t *e);
    static void OnSettingsSave(lv_event_t *e);
    static void OnControllerIcon(lv_event_t *e);
    static void OnControllerTestDismiss(lv_event_t *e);
    static void OnControllerTestClose(lv_event_t *e);
    static void OnConnectController(lv_event_t *e);
    static void OnSetupPs5(lv_event_t *e);
    static void OnPerformance(lv_event_t *e);
    static void OnQuality(lv_event_t *e);
    static void OnControllerPoll(lv_timer_t *timer);
    static void OnControllerInputPoll(lv_timer_t *timer);
};

} // namespace home
