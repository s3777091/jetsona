#include "display/views/bluetooth_settings_view.h"
#include "display/common/lvgl_utils.h"
#include "display/common/signal_bars.h"
#include "display/core/app_icons.h"
#include "fonts.h"
#include "esp_log.h"

#include <lvgl.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

#define TAG "BtView"

namespace home {

using jetson::ui::Color;
using jetson::ui::LvglLockGuard;

namespace {
struct RowCtx {
    BluetoothSettingsView *self;
    std::string address;
    std::string name;
    bool connected;
};

void OnRowDeleted(lv_event_t *e) {
    auto *ctx = static_cast<RowCtx *>(lv_event_get_user_data(e));
    delete ctx;
}

} // namespace

BluetoothSettingsView::BluetoothSettingsView(lv_obj_t *parent, int width, int height,
                                             jetson::IBluetoothManager &bluetooth,
                                             ClosedCb on_closed)
    : bluetooth_(bluetooth), parent_(parent), width_(width), height_(height),
      on_closed_(std::move(on_closed)) {
    if (!parent_) parent_ = lv_screen_active();
    BuildUi();
}

BluetoothSettingsView::~BluetoothSettingsView() {
    closed_ = true;
    if (overlay_) {
        LvglLockGuard lock;
        lv_obj_del(overlay_);
        overlay_ = nullptr;
    }
}

void BluetoothSettingsView::Start() { StartScan(); }

void BluetoothSettingsView::SetStatus(const char *text) {
    if (status_label_) lv_label_set_text(status_label_, text ? text : "");
}

void BluetoothSettingsView::BuildUi() {
    overlay_ = lv_obj_create(parent_);
    lv_obj_remove_style_all(overlay_);
    lv_obj_set_size(overlay_, width_, height_);
    lv_obj_set_pos(overlay_, 0, 0);
    lv_obj_set_style_bg_color(overlay_, Color(0x121417), 0);
    lv_obj_set_style_bg_opa(overlay_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(overlay_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(overlay_, 0, 0);

    // ---- Header (48px) ----
    auto *header = lv_obj_create(overlay_);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, width_, 48);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_color(header, Color(0x1b1d22), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(header, 8, 0);
    lv_obj_set_style_pad_right(header, 8, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    back_btn_ = lv_button_create(header);
    lv_obj_set_size(back_btn_, 40, 40);
    lv_obj_align(back_btn_, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(back_btn_, Color(0x2a2d33), 0);
    lv_obj_add_event_cb(back_btn_, OnBack, LV_EVENT_CLICKED, this);
    auto *back_lbl = lv_label_create(back_btn_);
    lv_obj_set_style_text_font(back_lbl, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(back_lbl, lv_color_white(), 0);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_center(back_lbl);

    rescan_btn_ = lv_button_create(header);
    lv_obj_set_size(rescan_btn_, 40, 40);
    // Sit next to the back button on the left so the top-right corner is free
    // for the global status bar (which lives on lv_layer_top and would overlap
    // a right-aligned button here).
    lv_obj_align_to(rescan_btn_, back_btn_, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
    lv_obj_set_style_bg_color(rescan_btn_, Color(0x2a2d33), 0);
    lv_obj_add_event_cb(rescan_btn_, OnRescan, LV_EVENT_CLICKED, this);
    auto *res_ic = jetson::ui::CreateAppIcon(rescan_btn_, "reload", 22);
    lv_obj_set_style_image_recolor(res_ic, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(res_ic, LV_OPA_COVER, 0);
    lv_obj_center(res_ic);

    title_label_ = lv_label_create(header);
    lv_obj_set_style_text_font(title_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(title_label_, lv_color_white(), 0);
    lv_label_set_text(title_label_, "Bluetooth");
    // Title sits on the left (after the back + rescan buttons) so it does not
    // sit under the centered Dynamic-Island bar at the top.
    lv_obj_align_to(title_label_, rescan_btn_, LV_ALIGN_OUT_RIGHT_MID, 12, 0);

    // ---- Status (24px) ----
    status_label_ = lv_label_create(overlay_);
    lv_obj_set_style_text_font(status_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(status_label_, Color(0x9aa0a6), 0);
    lv_obj_set_pos(status_label_, 12, 54);
    lv_obj_set_width(status_label_, width_ - 24);
    lv_label_set_text(status_label_, "Đang quét Bluetooth...");

    // ---- Device list (fills remaining space) ----
    list_ = lv_obj_create(overlay_);
    lv_obj_remove_style_all(list_);
    lv_obj_set_pos(list_, 0, 80);
    lv_obj_set_size(list_, width_, height_ - 80);
    lv_obj_set_style_bg_opa(list_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(list_, 8, 0);
    lv_obj_set_style_pad_row(list_, 8, 0);
    lv_obj_set_flex_flow(list_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollbar_mode(list_, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_scroll_dir(list_, LV_DIR_VER);
    lv_obj_add_flag(list_, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *BluetoothSettingsView::CreateRow(const jetson::BtDevice &dev) {
    auto *row = lv_obj_create(list_);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 64);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_bg_color(row, dev.connected ? Color(0x1e3a5f) : Color(0x1c1c1e), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(row, 12, 0);
    lv_obj_set_style_pad_right(row, 12, 0);
    lv_obj_set_style_pad_top(row, 8, 0);
    lv_obj_set_style_pad_bottom(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Leading device-category icon from the scan (controller-mini /
    // headphones / unknow-device).
    auto *kind_icon =
        jetson::ui::CreateAppIcon(row, jetson::BtKindIconName(dev.kind), 24);
    lv_obj_set_style_image_recolor(kind_icon, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(kind_icon, LV_OPA_COVER, 0);

    // Left: name (white) over address (grey), stacked in a column.
    auto *left = lv_obj_create(row);
    lv_obj_remove_style_all(left);
    lv_obj_set_flex_grow(left, 1);
    lv_obj_set_height(left, 48);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(left, 0, 0);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 2, 0);

    auto *name_lbl = lv_label_create(left);
    lv_obj_set_style_text_font(name_lbl, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(name_lbl, lv_color_white(), 0);
    lv_label_set_text(name_lbl, dev.name.c_str());
    lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_width(name_lbl, width_ - 180, 0);

    auto *addr_lbl = lv_label_create(left);
    lv_obj_set_style_text_font(addr_lbl, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(addr_lbl, Color(0x9aa0a6), 0);
    lv_label_set_text(addr_lbl, dev.address.c_str());

    // Right: RSSI bars + state tag.
    jetson::ui::CreateSignalBars(row, jetson::ui::RssiToSignalPercent(dev.rssi));

    const char *tag_text = nullptr;
    uint32_t tag_color = 0;
    if (dev.connected) { tag_text = "Đã kết nối"; tag_color = 0x4ea8ff; }
    else if (dev.paired) { tag_text = "Đã pair"; tag_color = 0x9aa0a6; }
    if (tag_text) {
        auto *tag = lv_label_create(row);
        lv_obj_set_style_text_font(tag, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(tag, Color(tag_color), 0);
        lv_label_set_text(tag, tag_text);
    }

    auto *ctx = new RowCtx{this, dev.address, dev.name, dev.connected};
    lv_obj_add_event_cb(row, OnRowClicked, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(row, OnRowDeleted, LV_EVENT_DELETE, ctx);
    return row;
}

void BluetoothSettingsView::ClearRows() {
    if (list_) lv_obj_clean(list_);
}

void BluetoothSettingsView::RenderList(const std::vector<jetson::BtDevice> &devs) {
    ClearRows();
    for (const auto &d : devs) CreateRow(d);
    if (devs.empty()) SetStatus("Không tìm thấy thiết bị. Bật thiết bị BT + bấm quay lại.");
}

void BluetoothSettingsView::StartScan() {
    if (scanning_.exchange(true)) return;
    SetStatus("Đang quét Bluetooth...");
    ESP_LOGI(TAG, "scan requested");
    std::thread([self = shared_from_this()]() {
        const bool available = self->bluetooth_.Available();
        std::vector<jetson::BtDevice> devs;
        std::string error;
        if (available) devs = self->bluetooth_.Scan(8);
        error = self->bluetooth_.LastError();

        LvglLockGuard lock;
        self->scanning_ = false;
        if (!self->closed_.load()) {
            if (!available) {
                self->SetStatus(("Lỗi Bluetooth: " + error).c_str());
                ESP_LOGE(TAG, "scan unavailable: %s", error.c_str());
                return;
            }
            self->RenderList(devs);
            int connected = 0;
            for (const auto &d : devs) if (d.connected) ++connected;
            self->SetStatus(devs.empty() ? "Không tìm thấy thiết bị Bluetooth"
                            : connected > 0
                                ? (std::to_string(connected) + " thiết bị đang kết nối").c_str()
                                : "Chạm thiết bị để pair + kết nối");
            ESP_LOGI(TAG, "rendered %zu devices", devs.size());
        }
    }).detach();
}

void BluetoothSettingsView::DoAction(const std::string &address, bool connected) {
    if (scanning_.exchange(true)) return;
    SetStatus(connected ? ("Đang ngắt " + address + "...").c_str()
                        : ("Đang pair + kết nối " + address + "...").c_str());
    std::thread([self = shared_from_this(), address, connected]() {
        bool ok = connected ? self->bluetooth_.Disconnect(address)
                            : self->bluetooth_.PairAndConnect(address);
        std::string error = self->bluetooth_.LastError();
        std::vector<jetson::BtDevice> devs;
        if (ok) devs = self->bluetooth_.Scan(4);
        LvglLockGuard lock;
        self->scanning_ = false;
        if (!self->closed_.load()) {
            if (ok) {
                self->RenderList(devs);
                self->SetStatus(connected ? "Đã ngắt kết nối" : "Đã kết nối");
                if (self->notify_cb_)
                    self->notify_cb_(connected ? "Đã ngắt Bluetooth"
                                               : "Đã kết nối Bluetooth");
                ESP_LOGI(TAG, "device action complete: %s", address.c_str());
            } else {
                self->SetStatus(("Lỗi: " + error).c_str());
                ESP_LOGE(TAG, "device action failed for %s: %s",
                         address.c_str(), error.c_str());
            }
        }
    }).detach();
}

void BluetoothSettingsView::RequestClose() {
    if (closed_.exchange(true)) return;
    if (overlay_) lv_obj_add_flag(overlay_, LV_OBJ_FLAG_HIDDEN);
    lv_timer_t *t = lv_timer_create(OnCloseTimer, 0, this);
    lv_timer_set_repeat_count(t, 1);
}

void BluetoothSettingsView::OnCloseTimer(lv_timer_t *t) {
    auto *self = static_cast<BluetoothSettingsView *>(lv_timer_get_user_data(t));
    lv_timer_del(t);
    if (self && self->on_closed_) self->on_closed_();
}

void BluetoothSettingsView::OnBack(lv_event_t *e) {
    LvglLockGuard lock;
    auto *self = static_cast<BluetoothSettingsView *>(lv_event_get_user_data(e));
    self->RequestClose();
}

void BluetoothSettingsView::OnRescan(lv_event_t *e) {
    LvglLockGuard lock;
    auto *self = static_cast<BluetoothSettingsView *>(lv_event_get_user_data(e));
    self->StartScan();
}

void BluetoothSettingsView::OnRowClicked(lv_event_t *e) {
    LvglLockGuard lock;
    auto *ctx = static_cast<RowCtx *>(lv_event_get_user_data(e));
    ctx->self->DoAction(ctx->address, ctx->connected);
}

} // namespace home
