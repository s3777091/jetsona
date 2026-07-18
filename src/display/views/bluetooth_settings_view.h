#pragma once

/* On-screen Bluetooth settings view for the Jetson DS-02 UI.
 *
 * Full-screen overlay: header (back + "Bluetooth" + scan), a status line, and a
 * scrollable list of scanned devices (kind icon + name + RSSI bars + state tag).
 * Tapping a connected device disconnects it; tapping any other device pairs +
 * connects it. Pairing is driven by bluetoothctl's default-agent (no on-screen
 * password entry needed).
 *
 * Same threading model as WifiSettingsView: scan/connect run on a worker thread;
 * the view is shared_ptr-owned so the worker can outlive the on-screen overlay;
 * the back button closes via a deferred one-shot lv_timer. */

#include "net/bluetooth_manager.h"

#include <lvgl.h>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace home {

class BluetoothSettingsView : public std::enable_shared_from_this<BluetoothSettingsView> {
public:
    using ClosedCb = std::function<void()>;
    using NotifyCb = std::function<void(const char *)>;

    BluetoothSettingsView(lv_obj_t *parent, int width, int height,
                          jetson::IBluetoothManager &bluetooth, ClosedCb on_closed);
    ~BluetoothSettingsView();

    void Start();
    void RequestClose();

    // Surface a short user-facing message (e.g. "Đã kết nối Bluetooth") on the
    // home Dynamic-Island notification. Home wires this to ShowNotification.
    void SetNotifyCb(NotifyCb cb) { notify_cb_ = std::move(cb); }

private:
    jetson::IBluetoothManager &bluetooth_;

    void BuildUi();
    void StartScan();
    void RenderList(const std::vector<jetson::BtDevice> &devs);
    void ClearRows();
    lv_obj_t *CreateRow(const jetson::BtDevice &dev);
    void DoAction(const std::string &address, const std::string &name,
                  bool connected);
    void SetStatus(const char *text);

    static void OnBack(lv_event_t *e);
    static void OnRescan(lv_event_t *e);
    static void OnRowClicked(lv_event_t *e);
    static void OnCloseTimer(lv_timer_t *t);

    lv_obj_t *parent_ = nullptr;
    lv_obj_t *overlay_ = nullptr;
    lv_obj_t *back_btn_ = nullptr;
    lv_obj_t *title_label_ = nullptr;
    lv_obj_t *rescan_btn_ = nullptr;
    lv_obj_t *status_label_ = nullptr;
    lv_obj_t *list_ = nullptr;

    int width_ = 0;
    int height_ = 0;
    ClosedCb on_closed_;
    NotifyCb notify_cb_;

    std::atomic<bool> scanning_{false};
    std::atomic<bool> closed_{false};
};

} // namespace home
