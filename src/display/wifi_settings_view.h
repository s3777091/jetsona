#pragma once

/* On-screen WiFi provisioning view for the Jetson DS-02 UI.
 *
 * Full-screen overlay (drawn above the standby/launcher/dock): a header with a
 * back button + rescan, a scrollable list of scanned networks (SSID + signal
 * bars + lock marker + "Connected" tag), and an LVGL keyboard for entering the
 * password when a secured network is tapped. Open networks connect directly.
 *
 * Scanning/connecting shells out to nmcli (blocking), so those run on a worker
 * thread; the view is reference-counted (shared_ptr) so the worker can safely
 * outlive the on-screen overlay. The back button closes via a deferred one-shot
 * lv_timer so we never destroy `*this` from inside one of its own event
 * callbacks. */

#include "wifi_manager.h"

#include <lvgl.h>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace home {

class WifiSettingsView : public std::enable_shared_from_this<WifiSettingsView> {
public:
    using ClosedCb = std::function<void()>;

    WifiSettingsView(lv_obj_t *parent, int width, int height, ClosedCb on_closed);
    ~WifiSettingsView();

    // Kick off the first scan. Must be called AFTER the object is owned by a
    // shared_ptr (i.e. after make_shared returns), because StartScan uses
    // shared_from_this(); calling it from the constructor would throw.
    void Start();

    // Hide + mark closed; the actual destruction is deferred (see file header).
    void RequestClose();

private:
    void BuildUi();
    void StartScan();
    void RenderList(const std::vector<jetson::WifiNetwork> &nets);
    void ClearRows();
    lv_obj_t *CreateRow(const jetson::WifiNetwork &net, int index);
    void DrawSignalBars(lv_obj_t *parent, int signal);
    void ShowKeyboardFor(const std::string &ssid);
    void HideKeyboard();
    void DoConnect(const std::string &ssid, const std::string &password);
    void SetStatus(const char *text);

    static void OnBack(lv_event_t *e);
    static void OnRescan(lv_event_t *e);
    static void OnRowClicked(lv_event_t *e);
    static void OnConnect(lv_event_t *e);
    static void OnCancel(lv_event_t *e);
    static void OnCloseTimer(lv_timer_t *t);

    lv_obj_t *parent_ = nullptr;
    lv_obj_t *overlay_ = nullptr;
    lv_obj_t *back_btn_ = nullptr;
    lv_obj_t *title_label_ = nullptr;
    lv_obj_t *rescan_btn_ = nullptr;
    lv_obj_t *status_label_ = nullptr;
    lv_obj_t *list_ = nullptr;
    lv_obj_t *kb_panel_ = nullptr;
    lv_obj_t *kb_ssid_label_ = nullptr;
    lv_obj_t *kb_textarea_ = nullptr;
    lv_obj_t *kb_keyboard_ = nullptr;
    lv_obj_t *kb_connect_btn_ = nullptr;
    lv_obj_t *kb_cancel_btn_ = nullptr;

    int width_ = 0;
    int height_ = 0;
    ClosedCb on_closed_;

    std::vector<jetson::WifiNetwork> last_networks_;
    std::string pending_ssid_;          // network awaiting password entry
    std::atomic<bool> scanning_{false};
    std::atomic<bool> closed_{false};
};

} // namespace home