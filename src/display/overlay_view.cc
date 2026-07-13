#include "overlay_view.h"
#include "fonts.h"
#include "ui_theme.h"

#include <lvgl.h>
#include <cstring>

namespace home {

namespace {
lv_color_t Color(uint32_t rgb) {
    return lv_color_make((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}
struct LvLockGuard {
    LvLockGuard() { lv_lock(); }
    ~LvLockGuard() { lv_unlock(); }
};
} // namespace

OverlayView::OverlayView(lv_obj_t *parent, int width, int height, const char *title, ClosedCb on_closed)
    : width_(width), height_(height), parent_(parent), on_closed_(std::move(on_closed)) {
    if (!parent_) parent_ = lv_screen_active();
    BuildShell(title);
}

OverlayView::~OverlayView() {
    closed_ = true;
    if (overlay_) {
        lv_lock();
        lv_obj_del(overlay_);
        lv_unlock();
        overlay_ = nullptr;
    }
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

    // ---- Header (48px) ----
    header_ = lv_obj_create(overlay_);
    lv_obj_remove_style_all(header_);
    lv_obj_set_size(header_, width_, 48);
    lv_obj_set_pos(header_, 0, 0);
    lv_obj_set_style_bg_color(header_, Color(p.header), 0);
    lv_obj_set_style_bg_opa(header_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(header_, 8, 0);
    lv_obj_set_style_pad_right(header_, 8, 0);
    lv_obj_clear_flag(header_, LV_OBJ_FLAG_SCROLLABLE);

    back_btn_ = lv_button_create(header_);
    lv_obj_set_size(back_btn_, 40, 40);
    lv_obj_align(back_btn_, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(back_btn_, Color(p.button), 0);
    lv_obj_add_event_cb(back_btn_, OnBack, LV_EVENT_CLICKED, this);
    auto *back_lbl = lv_label_create(back_btn_);
    lv_obj_set_style_text_font(back_lbl, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(back_lbl, Color(p.text), 0);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_center(back_lbl);

    title_label_ = lv_label_create(header_);
    lv_obj_set_style_text_font(title_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(title_label_, Color(p.text), 0);
    lv_label_set_text(title_label_, title ? title : "");
    lv_obj_center(title_label_);

    // ---- Status (24px) ----
    status_label_ = lv_label_create(overlay_);
    lv_obj_set_style_text_font(status_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(status_label_, Color(p.sub_text), 0);
    lv_obj_set_pos(status_label_, 12, 54);
    lv_obj_set_width(status_label_, width_ - 24);
    lv_label_set_text(status_label_, "");

    // ---- Body (fills remaining space) ----
    body_ = lv_obj_create(overlay_);
    lv_obj_remove_style_all(body_);
    lv_obj_set_pos(body_, 0, 80);
    lv_obj_set_size(body_, width_, height_ - 80);
    lv_obj_set_style_bg_opa(body_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(body_, 8, 0);
}

void OverlayView::SetStatus(const char *text) {
    if (status_label_) lv_label_set_text(status_label_, text ? text : "");
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

void OverlayView::OnBack(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<OverlayView *>(lv_event_get_user_data(e));
    self->RequestClose();
}

void OverlayView::OnRight(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<OverlayView *>(lv_event_get_user_data(e));
    if (self->right_cb_) self->right_cb_(self);
}

} // namespace home