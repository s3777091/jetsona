#pragma once

/* "Pods" drawer app: rent and manage RunPod GPU cloud pods from the panel.
 *
 * Body: a toolbar (status text + "Thuê GPU" + refresh) over a scrollable list
 * of the account's pods (status dot, name, GPU / price line, and per-row
 * Studio / power / delete buttons). Tapping a row opens a detail sheet with
 * the connect info (SSH command, web-IDE proxy URL, password) and the same
 * actions. "Thuê GPU" opens a rent sheet: pick a workspace preset (VS Code
 * Studio via code-server, or PyTorch + Jupyter) and a GPU type with live
 * $/hr pricing, then create the pod.
 *
 * "Mở Studio" hands the pod's HTTP proxy URL (https://{id}-{port}.proxy.
 * runpod.net) to the home screen, which opens it in the Chromium kiosk —
 * from there the user signs into GitHub and codes against the pod's GPU.
 *
 * All RunPod calls are blocking (libcurl) and run on detached worker threads
 * holding a weak_ptr; results marshal back via Application::Schedule + the
 * LVGL lock, mirroring ChatView. */

#include "display/views/overlay_view.h"
#include "net/runpod_client.h"

#include <lvgl.h>
#include <functional>
#include <string>
#include <vector>

namespace home {

class PodsView : public OverlayView {
public:
    using NotifyCb = std::function<void(const char *)>;
    using OpenUrlCb = std::function<void(const std::string &)>;

    PodsView(lv_obj_t *parent, int width, int height, ClosedCb on_closed);
    ~PodsView() override;

    void SetNotifyCb(NotifyCb cb) { notify_cb_ = std::move(cb); }
    // Home wires this to OpenChromium(url) (kiosk hand-off).
    void SetOpenUrlCb(OpenUrlCb cb) { open_url_cb_ = std::move(cb); }

protected:
    void OnStart() override;

private:
    struct RowCtx {
        PodsView *self = nullptr;
        std::string pod_id;
    };

    void BuildBody();
    void Refresh();
    void RenderList();
    void SetStatusLine(const std::string &text);
    void Notify(const char *msg);

    const jetson::RunpodPod *FindPod(const std::string &id) const;
    void OpenStudioFor(const std::string &pod_id);
    void ToggleStartStop(const std::string &pod_id);
    void RequestTerminate(const std::string &pod_id);

    void OpenDetailSheet(const std::string &pod_id);
    void CloseSheet();
    void OpenRentSheet();
    void UpdatePresetHighlight();
    void RenderGpuList();
    void UpdateGpuSelectionHighlight();
    void DoCreatePod();

    // Fire-and-forget worker; `work` runs off-thread, `done` back on the LVGL
    // thread (skipped when the view died meanwhile).
    void RunAsync(std::function<std::string(jetson::RunpodClient &)> work,
                  std::function<void(PodsView &, const std::string &err)> done);

    static void OnRefresh(lv_event_t *e);
    static void OnRent(lv_event_t *e);
    static void OnRowClicked(lv_event_t *e);
    static void OnRowStudio(lv_event_t *e);
    static void OnRowPower(lv_event_t *e);
    static void OnRowDelete(lv_event_t *e);
    static void OnRowCtxDeleted(lv_event_t *e);
    static void OnSheetDismiss(lv_event_t *e);
    static void OnGpuRowClicked(lv_event_t *e);
    static void OnPresetClicked(lv_event_t *e);
    static void OnCreateClicked(lv_event_t *e);

    jetson::RunpodClient client_;

    lv_obj_t *status_line_ = nullptr;
    lv_obj_t *list_ = nullptr;
    lv_obj_t *sheet_ = nullptr;       // active modal (detail or rent), or null
    lv_obj_t *gpu_list_ = nullptr;    // inside the rent sheet
    lv_obj_t *create_btn_label_ = nullptr;

    std::vector<jetson::RunpodPod> pods_;
    std::vector<jetson::RunpodGpuType> gpu_types_;

    // Rent sheet state.
    int preset_index_ = 0;            // 0 = VS Code Studio, 1 = PyTorch+Jupyter
    lv_obj_t *preset_btns_[2] = {nullptr, nullptr};
    std::string selected_gpu_id_;
    bool creating_ = false;

    // Two-tap terminate confirmation.
    std::string pending_delete_id_;
    uint32_t pending_delete_tick_ = 0;

    bool busy_ = false;
    NotifyCb notify_cb_;
    OpenUrlCb open_url_cb_;
};

} // namespace home
