#pragma once

/* Compact PS5 Remote Play control panel for the 800x480 DS-02 display.
 *
 * The view only prepares and validates the persistent Remote Play settings.
 * The home display owns the expensive hand-off to chiaki-ng: LaunchCb(true)
 * opens registration/configuration, while LaunchCb(false) starts streaming.
 * Both status/probe/save helper calls run off the LVGL thread and return through
 * Application::Schedule. No registration secret is collected or logged here.
 */

#include "display/views/overlay_view.h"

#include <lvgl.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace home {

class PsRemotePlayView : public OverlayView {
public:
    using LaunchCb = std::function<void(bool configure)>;

    PsRemotePlayView(lv_obj_t *parent, int width, int height, ClosedCb on_closed);
    ~PsRemotePlayView() override = default;

    // true = registration/configuration; false = stream the saved profile.
    void SetLaunchRequest(LaunchCb cb) { launch_cb_ = std::move(cb); }

protected:
    void OnStart() override;

private:
    enum class Preset { Smooth, Quality };

    // Status banner.
    lv_obj_t *status_dot_ = nullptr;
    lv_obj_t *status_summary_ = nullptr;
    lv_obj_t *status_detail_ = nullptr;
    lv_obj_t *install_badge_ = nullptr;
    lv_obj_t *register_badge_ = nullptr;
    lv_obj_t *refresh_btn_ = nullptr;

    // Host/configuration controls.
    lv_obj_t *host_label_ = nullptr;
    lv_obj_t *edit_host_btn_ = nullptr;
    lv_obj_t *probe_btn_ = nullptr;
    lv_obj_t *smooth_card_ = nullptr;
    lv_obj_t *quality_card_ = nullptr;
    lv_obj_t *runtime_detail_ = nullptr;
    lv_obj_t *action_status_ = nullptr;
    lv_obj_t *configure_btn_ = nullptr;
    lv_obj_t *configure_btn_label_ = nullptr;
    lv_obj_t *play_btn_ = nullptr;

    // Numeric IPv4 entry modal.
    lv_obj_t *host_modal_ = nullptr;
    lv_obj_t *host_input_ = nullptr;
    lv_obj_t *host_keyboard_ = nullptr;
    lv_obj_t *host_error_ = nullptr;

    LaunchCb launch_cb_;
    Preset preset_ = Preset::Smooth;
    std::string host_;
    std::string nickname_;
    std::string controller_;
    std::string network_;
    std::string remote_state_;
    std::string helper_message_;
    bool installed_ = false;
    bool registered_ = false;
    bool status_loaded_ = false;
    bool busy_ = false; // accessed only while the LVGL lock is held

    void BuildBody();
    void UpdateUi();
    void UpdatePresetCards();
    void SetBusy(bool busy, const std::string &message = "");
    void SetActionStatus(const std::string &message, bool error = false);

    void RefreshStatus();
    void ProbeHost();
    void SaveThenLaunch(bool configure);

    void OpenHostModal();
    void CloseHostModal();
    void AcceptHostModal();

    std::shared_ptr<PsRemotePlayView> Self();

    static void OnRefresh(lv_event_t *e);
    static void OnEditHost(lv_event_t *e);
    static void OnProbe(lv_event_t *e);
    static void OnSmooth(lv_event_t *e);
    static void OnQuality(lv_event_t *e);
    static void OnConfigure(lv_event_t *e);
    static void OnPlay(lv_event_t *e);
    static void OnModalDismiss(lv_event_t *e);
    static void OnKeyboardReady(lv_event_t *e);
    static void OnKeyboardCancel(lv_event_t *e);
};

} // namespace home
