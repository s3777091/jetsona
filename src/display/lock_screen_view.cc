#include "lock_screen_view.h"
#include "fonts.h"
#include "settings.h"

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

LockScreenView::LockScreenView(lv_obj_t *parent, int width, int height, ClosedCb on_closed)
    : parent_(parent), width_(width), height_(height), on_closed_(std::move(on_closed)) {
    if (!parent_) parent_ = lv_screen_active();

    overlay_ = lv_obj_create(parent_);
    lv_obj_remove_style_all(overlay_);
    lv_obj_set_size(overlay_, width_, height_);
    lv_obj_set_pos(overlay_, 0, 0);
    lv_obj_set_style_bg_color(overlay_, Color(0x101216), 0);
    lv_obj_set_style_bg_opa(overlay_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(overlay_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(overlay_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(overlay_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(overlay_, 14, 0);
    lv_obj_set_style_pad_all(overlay_, 24, 0);

    auto *lock = lv_label_create(overlay_);
    lv_obj_set_style_text_font(lock, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(lock, lv_color_white(), 0);
    lv_label_set_text(lock, LV_SYMBOL_EYE_CLOSE); // a "lock-ish" glyph

    auto *title = lv_label_create(overlay_);
    lv_obj_set_style_text_font(title, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text(title, "Màn hình đã khóa");

    input_ = new TelexInput(overlay_, 260, 48);
    input_->SetPassword(true);
    input_->SetMaxLen(4);
    input_->SetPlaceholder("Nhập PIN...");
    // Enter on the field submits.
    lv_obj_add_event_cb(input_->obj(), OnPinReady, LV_EVENT_READY, this);

    auto *btn = lv_button_create(overlay_);
    lv_obj_set_size(btn, 160, 44);
    lv_obj_set_style_bg_color(btn, Color(0x2b6fd6), 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_add_event_cb(btn, OnUnlock, LV_EVENT_CLICKED, this);
    auto *bl = lv_label_create(btn);
    lv_obj_set_style_text_font(bl, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(bl, lv_color_white(), 0);
    lv_label_set_text(bl, "Mở khóa");
    lv_obj_center(bl);

    status_label_ = lv_label_create(overlay_);
    lv_obj_set_style_text_font(status_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(status_label_, Color(0x9aa0a6), 0);
    lv_label_set_text(status_label_, " ");
}

LockScreenView::~LockScreenView() {
    closed_ = true;
    if (overlay_) {
        // May run on a worker thread; guard the LVGL deletion.
        lv_lock();
        lv_obj_del(overlay_);
        lv_unlock();
        overlay_ = nullptr;
        input_ = nullptr; // freed via its LV_EVENT_DELETE -> delete self
    }
}

void LockScreenView::Start() {
    if (input_) input_->Focus();
}

void LockScreenView::CheckPin() {
    if (!input_) return;
    std::string entered = input_->Text();
    std::string expected = Settings("system").GetString("pin", "");
    if (!expected.empty() && entered == expected) {
        RequestClose();
        return;
    }
    input_->Clear();
    if (input_) input_->Focus();
    if (status_label_) lv_label_set_text(status_label_, "Sai PIN, thử lại");
}

void LockScreenView::RequestClose() {
    if (closed_.exchange(true)) return;
    if (overlay_) lv_obj_add_flag(overlay_, LV_OBJ_FLAG_HIDDEN);
    lv_timer_t *t = lv_timer_create(OnCloseTimer, 0, this);
    lv_timer_set_repeat_count(t, 1);
}

void LockScreenView::OnCloseTimer(lv_timer_t *t) {
    auto *self = static_cast<LockScreenView *>(lv_timer_get_user_data(t));
    lv_timer_del(t);
    if (self && self->on_closed_) self->on_closed_();
}

void LockScreenView::OnUnlock(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<LockScreenView *>(lv_event_get_user_data(e));
    self->CheckPin();
}

void LockScreenView::OnPinReady(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<LockScreenView *>(lv_event_get_user_data(e));
    self->CheckPin();
}

} // namespace home