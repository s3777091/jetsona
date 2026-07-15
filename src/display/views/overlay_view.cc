#include "display/views/overlay_view.h"
#include "display/common/lvgl_utils.h"
#include "fonts.h"
#include "display/theme/ui_theme.h"
#include "esp_log.h"

#include <lvgl.h>
#include <cstring>

namespace home {

#define TAG "OverlayView"

using jetson::ui::Color;
using jetson::ui::LvglLockGuard;

OverlayView::OverlayView(lv_obj_t *parent, int width, int height, const char *title, ClosedCb on_closed)
    : width_(width), height_(height), parent_(parent), title_(title ? title : ""),
      on_closed_(std::move(on_closed)) {
    if (!parent_) parent_ = lv_screen_active();
    BuildShell(title);
}

OverlayView::~OverlayView() {
    closed_ = true;
    LvglLockGuard lock;
    if (restore_btn_) { lv_obj_del(restore_btn_); restore_btn_ = nullptr; }
    if (overlay_) { lv_obj_del(overlay_); overlay_ = nullptr; }
}

void OverlayView::BuildShell(const char *title) {
    const auto &p = jetson::UiTheme::Instance().Palette();

    overlay_ = lv_obj_create(parent_);
    lv_obj_remove_style_all(overlay_);
    lv_obj_set_size(overlay_, width_, height_);
    lv_obj_set_pos(overlay_, 0, 0);
    lv_obj_set_style_bg_color(overlay_, Color(p.bg), 0);
    lv_obj_set_style_bg_opa(overlay_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(overlay_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(overlay_, 0, 0);

    // ---- Header ----
    // The first 42 px are visually owned by the global StatusBar. App/window
    // controls sit in the remaining lower strip instead of competing with it.
    header_ = lv_obj_create(overlay_);
    lv_obj_remove_style_all(header_);
    lv_obj_set_size(header_, width_, kHeaderHeight);
    lv_obj_set_pos(header_, 0, 0);
    lv_obj_set_style_bg_color(header_, Color(p.header), 0);
    lv_obj_set_style_bg_opa(header_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(header_, 8, 0);
    lv_obj_set_style_pad_right(header_, 8, 0);
    lv_obj_clear_flag(header_, LV_OBJ_FLAG_SCROLLABLE);

    // macOS-style traffic lights: red close, yellow minimize, green zoom.
    auto make_light = [&](lv_obj_t **out, uint32_t rgb, const char *glyph,
                          lv_event_cb_t cb) {
        auto *b = lv_obj_create(header_);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 14, 14);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(b, Color(rgb), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        auto *g = lv_label_create(b);
        // Keep the controls on a small, fixed font. BUILTIN_TEXT_FONT is the
        // 28 px application font, which gets clipped inside a 14 px light and
        // makes x/-/+ look as if they sit at different heights.
        lv_obj_set_style_text_font(g, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(g, Color(0x000000), 0);
        lv_obj_set_style_text_opa(g, (lv_opa_t)0xA0, 0);
        lv_label_set_text(g, glyph);
        lv_obj_center(g);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, this);
        *out = b;
    };
    make_light(&close_btn_, 0xFF5F57, "x", OnCloseBtn);
    lv_obj_align(close_btn_, LV_ALIGN_BOTTOM_LEFT, 12, -8);
    make_light(&min_btn_, 0xFEBC2E, "-", OnMinBtn);
    lv_obj_align_to(min_btn_, close_btn_, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
    make_light(&zoom_btn_, 0x28C840, "+", OnZoomBtn);
    lv_obj_align_to(zoom_btn_, min_btn_, LV_ALIGN_OUT_RIGHT_MID, 8, 0);

    title_label_ = lv_label_create(header_);
    lv_obj_set_style_text_font(title_label_, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(title_label_, Color(p.text), 0);
    lv_label_set_text(title_label_, title ? title : "");
    // Title sits on the left, just after the traffic-light controls, so it
    // does not sit under the centered Dynamic-Island bar at the top.
    lv_obj_align_to(title_label_, zoom_btn_, LV_ALIGN_OUT_RIGHT_MID, 16, 0);

    // ---- Body (fills everything below the compact title bar) ----
    // Normal app content starts immediately below the title bar. The old shell
    // permanently reserved a strip for instructional text; on a 480 px panel
    // that was both redundant and expensive. A later transient bottom "status
    // pill" was also removed -- it duplicated info already on screen (e.g.
    // "Đã kết nối: <ssid>", "Shell san sang") and cluttered the footer. Status
    // messages now go only to the log (see SetStatus).
    body_ = lv_obj_create(overlay_);
    lv_obj_remove_style_all(body_);
    lv_obj_set_pos(body_, 0, kHeaderHeight);
    lv_obj_set_size(body_, width_, height_ - kHeaderHeight);
    lv_obj_set_style_bg_opa(body_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(body_, 8, 0);
}

void OverlayView::SetStatus(const char *text) {
    // No on-screen status pill anymore -- it duplicated visible info and
    // cluttered the footer. Keep the message in the log for diagnostics so
    // worker threads can still report progress (scans, connects, etc.).
    ESP_LOGI(TAG, "%s: %s", title_.c_str(), text ? text : "");
}

void OverlayView::SetRightButton(const char *icon_symbol, RightCb cb) {
    if (right_btn_) return; // only one
    right_cb_ = std::move(cb);
    const auto &p = jetson::UiTheme::Instance().Palette();
    right_btn_ = lv_button_create(header_);
    lv_obj_set_size(right_btn_, 40, 40);
    lv_obj_align(right_btn_, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(right_btn_, Color(p.button), 0);
    lv_obj_add_event_cb(right_btn_, OnRight, LV_EVENT_CLICKED, this);
    auto *lbl = lv_label_create(right_btn_);
    lv_obj_set_style_text_font(lbl, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(lbl, Color(p.text), 0);
    lv_label_set_text(lbl, icon_symbol ? icon_symbol : "");
    lv_obj_center(lbl);
}

void OverlayView::Start() {
    OnStart();
}

void OverlayView::RequestClose() {
    if (closed_.exchange(true)) return;
    if (overlay_) lv_obj_add_flag(overlay_, LV_OBJ_FLAG_HIDDEN);
    lv_timer_t *t = lv_timer_create(OnCloseTimer, 0, this);
    lv_timer_set_repeat_count(t, 1);
}

void OverlayView::OnCloseTimer(lv_timer_t *t) {
    auto *self = static_cast<OverlayView *>(lv_timer_get_user_data(t));
    lv_timer_del(t);
    if (self && self->on_closed_) self->on_closed_();
}

void OverlayView::OnCloseBtn(lv_event_t *e) {
    LvglLockGuard lock;
    auto *self = static_cast<OverlayView *>(lv_event_get_user_data(e));
    self->RequestClose();
}

void OverlayView::OnMinBtn(lv_event_t *e) {
    LvglLockGuard lock;
    auto *self = static_cast<OverlayView *>(lv_event_get_user_data(e));
    self->Minimize();
}

void OverlayView::OnZoomBtn(lv_event_t *e) {
    LvglLockGuard lock;
    auto *self = static_cast<OverlayView *>(lv_event_get_user_data(e));
    self->ToggleZoom();
}

void OverlayView::OnRestore(lv_event_t *e) {
    LvglLockGuard lock;
    auto *self = static_cast<OverlayView *>(lv_event_get_user_data(e));
    self->Restore();
}

void OverlayView::Minimize() {
    if (!overlay_ || restore_btn_) return; // already minimized / no overlay
    lv_obj_add_flag(overlay_, LV_OBJ_FLAG_HIDDEN);
    const auto &p = jetson::UiTheme::Instance().Palette();
    restore_btn_ = lv_obj_create(parent_);
    lv_obj_remove_style_all(restore_btn_);
    lv_obj_set_size(restore_btn_, 140, 36);
    lv_obj_set_style_radius(restore_btn_, 18, 0);
    lv_obj_set_style_bg_color(restore_btn_, Color(p.accent), 0);
    lv_obj_set_style_bg_opa(restore_btn_, LV_OPA_COVER, 0);
    lv_obj_add_flag(restore_btn_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(restore_btn_, LV_ALIGN_BOTTOM_MID, 0, -8);
    auto *lbl = lv_label_create(restore_btn_);
    lv_obj_set_style_text_font(lbl, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_label_set_text(lbl, title_.c_str());
    lv_obj_center(lbl);
    lv_obj_add_event_cb(restore_btn_, OnRestore, LV_EVENT_CLICKED, this);
}

void OverlayView::Restore() {
    if (!restore_btn_) return;
    lv_obj_del(restore_btn_);
    restore_btn_ = nullptr;
    if (overlay_) lv_obj_clear_flag(overlay_, LV_OBJ_FLAG_HIDDEN);
}

void OverlayView::ToggleZoom() {
    if (!overlay_) return;
    const auto &p = jetson::UiTheme::Instance().Palette();
    zoomed_ = !zoomed_;
    int w, h;
    if (zoomed_) {
        w = width_ * 96 / 100;   // 768
        h = height_ * 90 / 100;  // 432
        lv_obj_set_pos(overlay_, (width_ - w) / 2, (height_ - h) / 2);
        lv_obj_set_style_radius(overlay_, 16, 0);
        lv_obj_set_style_border_width(overlay_, 1, 0);
        lv_obj_set_style_border_color(overlay_, Color(p.row), 0);
        lv_obj_set_style_shadow_width(overlay_, 24, 0);
        lv_obj_set_style_clip_corner(overlay_, true, 0);
    } else {
        w = width_;
        h = height_;
        lv_obj_set_pos(overlay_, 0, 0);
        lv_obj_set_style_radius(overlay_, 0, 0);
        lv_obj_set_style_border_width(overlay_, 0, 0);
        lv_obj_set_style_shadow_width(overlay_, 0, 0);
        lv_obj_set_style_clip_corner(overlay_, false, 0);
    }
    lv_obj_set_size(overlay_, w, h);
    lv_obj_set_width(header_, w);
    lv_obj_set_size(body_, w, h - kHeaderHeight);
    OnResize(w, h - kHeaderHeight);
}

void OverlayView::OnRight(lv_event_t *e) {
    LvglLockGuard lock;
    auto *self = static_cast<OverlayView *>(lv_event_get_user_data(e));
    if (self->right_cb_) self->right_cb_(self);
}

} // namespace home
