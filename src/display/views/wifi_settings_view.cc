#include "wifi_settings_view.h"
#include "fonts.h"
#include "esp_log.h"
#include "lvgl_runtime.h"

#include <lvgl.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

#define TAG "WifiView"

namespace home {

namespace {
lv_color_t Color(uint32_t rgb) {
    return lv_color_make((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

struct RowCtx {
    WifiSettingsView *self;
    std::string ssid;
    bool secured;
};

void OnRowDeleted(lv_event_t *e) {
    auto *ctx = static_cast<RowCtx *>(lv_event_get_user_data(e));
    delete ctx;
}

// LVGL is not thread-safe; every event callback touches objects and must hold
// lv_lock (the worker threads lock separately). The handler loop does not hold
// it for us, so we acquire it here.
struct LvLockGuard {
    LvLockGuard() { lv_lock(); }
    ~LvLockGuard() { lv_unlock(); }
};
} // namespace

WifiSettingsView::WifiSettingsView(lv_obj_t *parent, int width, int height, ClosedCb on_closed)
    : parent_(parent), width_(width), height_(height), on_closed_(std::move(on_closed)) {
    if (!parent_) parent_ = lv_screen_active();
    BuildUi();
}

void WifiSettingsView::Start() {
    StartScan();
}

WifiSettingsView::~WifiSettingsView() {
    closed_ = true;
    if (overlay_) {
        // May run on a worker thread; guard the LVGL deletion.
        lv_lock();
        lv_obj_del(overlay_);
        lv_unlock();
        overlay_ = nullptr;
    }
}

void WifiSettingsView::SetStatus(const char *text) {
    if (status_label_) lv_label_set_text(status_label_, text ? text : "");
}

void WifiSettingsView::BuildUi() {
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

    title_label_ = lv_label_create(header);
    lv_obj_set_style_text_font(title_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(title_label_, lv_color_white(), 0);
    lv_label_set_text(title_label_, "WiFi");
    lv_obj_center(title_label_);

    rescan_btn_ = lv_button_create(header);
    lv_obj_set_size(rescan_btn_, 40, 40);
    lv_obj_align(rescan_btn_, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(rescan_btn_, Color(0x2a2d33), 0);
    lv_obj_add_event_cb(rescan_btn_, OnRescan, LV_EVENT_CLICKED, this);
    auto *res_lbl = lv_label_create(rescan_btn_);
    lv_obj_set_style_text_font(res_lbl, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(res_lbl, lv_color_white(), 0);
    lv_label_set_text(res_lbl, LV_SYMBOL_REFRESH);
    lv_obj_center(res_lbl);

    // ---- Status (24px) ----
    status_label_ = lv_label_create(overlay_);
    lv_obj_set_style_text_font(status_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(status_label_, Color(0x9aa0a6), 0);
    lv_obj_set_pos(status_label_, 12, 54);
    lv_obj_set_width(status_label_, width_ - 24);
    lv_label_set_text(status_label_, "Đang quét WiFi...");

    // ---- Network list (fills remaining space) ----
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

    // ---- Keyboard panel (hidden until a secured network is tapped) ----
    kb_panel_ = lv_obj_create(overlay_);
    lv_obj_remove_style_all(kb_panel_);
    lv_obj_set_pos(kb_panel_, 0, height_ - 260);
    lv_obj_set_size(kb_panel_, width_, 260);
    lv_obj_set_style_bg_color(kb_panel_, Color(0x1b1d22), 0);
    lv_obj_set_style_bg_opa(kb_panel_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(kb_panel_, 8, 0);
    lv_obj_set_style_radius(kb_panel_, 16, 0);
    lv_obj_set_style_border_side(kb_panel_, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(kb_panel_, 1, 0);
    lv_obj_set_style_border_color(kb_panel_, Color(0x2a2d33), 0);
    lv_obj_clear_flag(kb_panel_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(kb_panel_, LV_OBJ_FLAG_HIDDEN);

    kb_ssid_label_ = lv_label_create(kb_panel_);
    lv_obj_set_style_text_font(kb_ssid_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(kb_ssid_label_, lv_color_white(), 0);
    lv_label_set_text(kb_ssid_label_, "");
    lv_obj_set_pos(kb_ssid_label_, 8, 6);
    lv_obj_set_width(kb_ssid_label_, width_ - 16);

    kb_textarea_ = lv_textarea_create(kb_panel_);
    lv_obj_set_pos(kb_textarea_, 8, 30);
    lv_obj_set_size(kb_textarea_, width_ - 16 - 176, 40);
    lv_obj_set_style_text_font(kb_textarea_, &BUILTIN_TEXT_FONT, 0);
    lv_textarea_set_placeholder_text(kb_textarea_, "Mật khẩu...");
    lv_textarea_set_password_mode(kb_textarea_, true);
    lv_textarea_set_one_line(kb_textarea_, true);
    lv_textarea_set_max_length(kb_textarea_, 63);
    /* Let the USB keyboard type the WiFi password via the keypad group. */
    if (auto *g = jetson::LvglRuntime::Instance().keypad_group()) lv_group_add_obj(g, kb_textarea_);

    kb_connect_btn_ = lv_button_create(kb_panel_);
    lv_obj_set_size(kb_connect_btn_, 80, 40);
    lv_obj_align(kb_connect_btn_, LV_ALIGN_TOP_RIGHT, -96, 30);
    lv_obj_set_style_bg_color(kb_connect_btn_, Color(0x2b6fd6), 0);
    lv_obj_add_event_cb(kb_connect_btn_, OnConnect, LV_EVENT_CLICKED, this);
    auto *conn_lbl = lv_label_create(kb_connect_btn_);
    lv_obj_set_style_text_font(conn_lbl, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(conn_lbl, lv_color_white(), 0);
    lv_label_set_text(conn_lbl, "Kết nối");
    lv_obj_center(conn_lbl);

    kb_cancel_btn_ = lv_button_create(kb_panel_);
    lv_obj_set_size(kb_cancel_btn_, 80, 40);
    lv_obj_align(kb_cancel_btn_, LV_ALIGN_TOP_RIGHT, -8, 30);
    lv_obj_set_style_bg_color(kb_cancel_btn_, Color(0x2a2d33), 0);
    lv_obj_add_event_cb(kb_cancel_btn_, OnCancel, LV_EVENT_CLICKED, this);
    auto *can_lbl = lv_label_create(kb_cancel_btn_);
    lv_obj_set_style_text_font(can_lbl, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(can_lbl, lv_color_white(), 0);
    lv_label_set_text(can_lbl, "Hủy");
    lv_obj_center(can_lbl);

    kb_keyboard_ = lv_keyboard_create(kb_panel_);
    lv_obj_set_pos(kb_keyboard_, 0, 80);
    lv_obj_set_size(kb_keyboard_, width_, 260 - 80);
    lv_keyboard_set_textarea(kb_keyboard_, kb_textarea_);
    lv_obj_add_event_cb(kb_keyboard_, OnConnect, LV_EVENT_READY, this);
    lv_obj_add_event_cb(kb_keyboard_, OnCancel, LV_EVENT_CANCEL, this);
}

void WifiSettingsView::DrawSignalBars(lv_obj_t *parent, int signal) {
    auto *bars = lv_obj_create(parent);
    lv_obj_remove_style_all(bars);
    lv_obj_set_size(bars, 36, 22);
    lv_obj_clear_flag(bars, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(bars, 0, 0);
    lv_obj_set_flex_flow(bars, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bars, 2, 0);
    lv_obj_set_flex_align(bars, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

    // 4 bars; fill count based on signal (0-100).
    int filled = 0;
    if (signal >= 75) filled = 4;
    else if (signal >= 50) filled = 3;
    else if (signal >= 25) filled = 2;
    else if (signal > 0) filled = 1;
    const int heights[4] = {8, 12, 16, 20};
    for (int i = 0; i < 4; ++i) {
        auto *bar = lv_obj_create(bars);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 6, heights[i]);
        lv_obj_set_style_radius(bar, 1, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        if (i < filled) {
            lv_obj_set_style_bg_color(bar, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_color(bar, Color(0x555555), 0);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        }
    }
}

void WifiSettingsView::ClearRows() {
    if (!list_) return;
    lv_obj_clean(list_);
}

lv_obj_t *WifiSettingsView::CreateRow(const jetson::WifiNetwork &net, int index) {
    auto *row = lv_obj_create(list_);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 56);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_bg_color(row, net.in_use ? Color(0x1e3a5f) : Color(0x1c1c1e), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(row, 12, 0);
    lv_obj_set_style_pad_right(row, 12, 0);
    lv_obj_set_style_pad_top(row, 0, 0);
    lv_obj_set_style_pad_bottom(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto *ssid_lbl = lv_label_create(row);
    lv_obj_set_style_text_font(ssid_lbl, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(ssid_lbl, lv_color_white(), 0);
    lv_label_set_text(ssid_lbl, net.ssid.c_str());
    lv_label_set_long_mode(ssid_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(ssid_lbl, 1);
    lv_obj_set_style_max_width(ssid_lbl, width_ - 160, 0);

    auto *right = lv_obj_create(row);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, 80, 22);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(right, 0, 0);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(right, 6, 0);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    if (net.secured) {
        auto *lock = lv_label_create(right);
        lv_obj_set_style_text_font(lock, &BUILTIN_ICON_FONT, 0);
        lv_obj_set_style_text_color(lock, Color(0x9aa0a6), 0);
        lv_label_set_text(lock, LV_SYMBOL_KEYBOARD); // "needs password"
    }
    DrawSignalBars(right, net.signal);

    if (net.in_use) {
        auto *tag = lv_label_create(row);
        lv_obj_set_style_text_font(tag, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(tag, Color(0x4ea8ff), 0);
        lv_label_set_text(tag, "Đã kết nối");
    }

    auto *ctx = new RowCtx{this, net.ssid, net.secured};
    lv_obj_add_event_cb(row, OnRowClicked, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(row, OnRowDeleted, LV_EVENT_DELETE, ctx);
    return row;
}

void WifiSettingsView::RenderList(const std::vector<jetson::WifiNetwork> &nets) {
    ClearRows();
    for (size_t i = 0; i < nets.size(); ++i) CreateRow(nets[i], (int)i);
    if (nets.empty()) SetStatus("Không tìm thấy mạng WiFi. Bấm quay lại thử lại.");
}

void WifiSettingsView::StartScan() {
    if (!jetson::WifiManager::Instance().Available()) {
        SetStatus("NetworkManager không sẵn sàng (cắm USB WiFi + kiểm tra nmcli).");
        return;
    }
    if (scanning_.exchange(true)) return;
    SetStatus("Đang quét WiFi...");
    std::thread([self = shared_from_this()]() {
        auto nets = jetson::WifiManager::Instance().Scan();
        lv_lock();
        self->scanning_ = false;
        if (!self->closed_.load()) {
            self->last_networks_ = std::move(nets);
            self->RenderList(self->last_networks_);
            auto active = jetson::WifiManager::Instance().ActiveSsid();
            self->SetStatus(active.empty() ? "Chạm vào mạng để kết nối"
                                           : ("Đã kết nối: " + active).c_str());
        }
        lv_unlock();
    }).detach();
}

void WifiSettingsView::ShowKeyboardFor(const std::string &ssid) {
    pending_ssid_ = ssid;
    if (kb_ssid_label_) lv_label_set_text(kb_ssid_label_, ("Mật khẩu cho: " + ssid).c_str());
    if (kb_textarea_) lv_textarea_set_text(kb_textarea_, "");
    if (kb_panel_) lv_obj_clear_flag(kb_panel_, LV_OBJ_FLAG_HIDDEN);
    if (kb_keyboard_) lv_keyboard_set_textarea(kb_keyboard_, kb_textarea_);
}

void WifiSettingsView::HideKeyboard() {
    pending_ssid_.clear();
    if (kb_panel_) lv_obj_add_flag(kb_panel_, LV_OBJ_FLAG_HIDDEN);
    if (kb_keyboard_) lv_keyboard_set_textarea(kb_keyboard_, nullptr);
}

void WifiSettingsView::DoConnect(const std::string &ssid, const std::string &password) {
    HideKeyboard();
    SetStatus(("Đang kết nối " + ssid + "...").c_str());
    std::thread([self = shared_from_this(), ssid, password]() {
        bool ok = jetson::WifiManager::Instance().Connect(ssid, password);
        lv_lock();
        if (!self->closed_.load()) {
            if (ok) {
                self->SetStatus(("Đã kết nối: " + ssid).c_str());
                self->StartScan();
            } else {
                self->SetStatus(("Lỗi: " + jetson::WifiManager::Instance().LastError()).c_str());
            }
        }
        lv_unlock();
    }).detach();
}

void WifiSettingsView::RequestClose() {
    if (closed_.exchange(true)) return;
    if (overlay_) lv_obj_add_flag(overlay_, LV_OBJ_FLAG_HIDDEN);
    lv_timer_t *t = lv_timer_create(OnCloseTimer, 0, this);
    lv_timer_set_repeat_count(t, 1);
}

void WifiSettingsView::OnCloseTimer(lv_timer_t *t) {
    auto *self = static_cast<WifiSettingsView *>(lv_timer_get_user_data(t));
    lv_timer_del(t);
    if (self && self->on_closed_) self->on_closed_();
}

void WifiSettingsView::OnBack(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<WifiSettingsView *>(lv_event_get_user_data(e));
    self->RequestClose();
}

void WifiSettingsView::OnRescan(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<WifiSettingsView *>(lv_event_get_user_data(e));
    self->StartScan();
}

void WifiSettingsView::OnRowClicked(lv_event_t *e) {
    LvLockGuard lock;
    auto *ctx = static_cast<RowCtx *>(lv_event_get_user_data(e));
    auto *self = ctx->self;
    if (ctx->secured) self->ShowKeyboardFor(ctx->ssid);
    else self->DoConnect(ctx->ssid, "");
}

void WifiSettingsView::OnConnect(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<WifiSettingsView *>(lv_event_get_user_data(e));
    if (self->pending_ssid_.empty()) return;
    const char *pass = self->kb_textarea_ ? lv_textarea_get_text(self->kb_textarea_) : "";
    std::string ssid = self->pending_ssid_;
    self->DoConnect(ssid, pass ? pass : "");
}

void WifiSettingsView::OnCancel(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<WifiSettingsView *>(lv_event_get_user_data(e));
    self->HideKeyboard();
}

} // namespace home