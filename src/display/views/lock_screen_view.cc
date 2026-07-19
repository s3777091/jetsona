#include "display/views/lock_screen_view.h"
#include "display/common/lvgl_utils.h"
#include "display/core/app_icons.h"
#include "fonts.h"
#include "settings.h"

#include <lvgl.h>
#include <cstring>

namespace home {

using jetson::ui::Color;
using jetson::ui::LvglLockGuard;

namespace {
// Clear the status-bar strip so the weather card never sits under the clock.
constexpr int kTopBarHeight = 46;
} // namespace

LockScreenView::LockScreenView(lv_obj_t *parent, int width, int height, ClosedCb on_closed)
    : parent_(parent), width_(width), height_(height), on_closed_(std::move(on_closed)) {
    if (!parent_) parent_ = lv_screen_active();

    const std::string pin = Settings("system").GetString("pin", "");
    pin_len_ = pin.size() == 6 ? 6 : 4;

    overlay_ = lv_obj_create(parent_);
    lv_obj_remove_style_all(overlay_);
    lv_obj_set_size(overlay_, width_, height_);
    lv_obj_set_pos(overlay_, 0, 0);
    // Same vertical gradient as the standby wallpaper backdrop, so locking
    // reads as the screen settling rather than a modal appearing.
    lv_obj_set_style_bg_color(overlay_, Color(0x11151c), 0);
    lv_obj_set_style_bg_grad_color(overlay_, Color(0x232a35), 0);
    lv_obj_set_style_bg_grad_dir(overlay_, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(overlay_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(overlay_, LV_OBJ_FLAG_SCROLLABLE);

    /* --- Weather, top-right corner ------------------------------------- */
    weather_card_ = lv_obj_create(overlay_);
    lv_obj_remove_style_all(weather_card_);
    lv_obj_set_width(weather_card_, 300);
    lv_obj_set_height(weather_card_, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(weather_card_, 16, 0);
    lv_obj_set_style_bg_color(weather_card_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(weather_card_, LV_OPA_10, 0);
    lv_obj_set_style_pad_all(weather_card_, 14, 0);
    lv_obj_set_style_pad_row(weather_card_, 4, 0);
    lv_obj_set_flex_flow(weather_card_, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(weather_card_,
                      (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_align(weather_card_, LV_ALIGN_TOP_RIGHT, -18, kTopBarHeight + 12);
    // Nothing to show until the first fetch lands.
    lv_obj_add_flag(weather_card_, LV_OBJ_FLAG_HIDDEN);

    weather_place_ = lv_label_create(weather_card_);
    lv_obj_set_style_text_font(weather_place_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(weather_place_, lv_color_white(), 0);
    lv_obj_set_style_text_align(weather_place_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(weather_place_, lv_pct(100));
    lv_label_set_long_mode(weather_place_, LV_LABEL_LONG_DOT);
    lv_label_set_text(weather_place_, "");

    weather_detail_ = lv_label_create(weather_card_);
    lv_obj_set_style_text_font(weather_detail_, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(weather_detail_, Color(0xc7ccd4), 0);
    lv_obj_set_style_text_align(weather_detail_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(weather_detail_, lv_pct(100));
    lv_label_set_long_mode(weather_detail_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(weather_detail_, "");

    /* --- PIN column, centered ------------------------------------------ */
    auto *column = lv_obj_create(overlay_);
    lv_obj_remove_style_all(column);
    lv_obj_set_width(column, 320);
    lv_obj_set_height(column, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(column, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(column, 14, 0);
    lv_obj_clear_flag(column, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(column, LV_ALIGN_CENTER, 0, kTopBarHeight / 2);

    /* PNG from the shared icon cache, not a text glyph: the bundled arial.ttf
     * carries no symbol block, so a literal padlock would spam the log. */
    auto *lock_icon = jetson::ui::CreateAppIcon(column, "lock", 52);
    lv_obj_set_style_image_recolor(lock_icon, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(lock_icon, LV_OPA_COVER, 0);

    auto *title = lv_label_create(column);
    lv_obj_set_style_text_font(title, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text(title, "Màn hình đã khóa");

    input_ = new TelexInput(column, 260, 48);
    input_->SetTelex(false);
    input_->SetPassword(true);
    input_->SetMaxLen(pin_len_);
    input_->SetAcceptedChars("0123456789");
    input_->SetPlaceholder(pin_len_ == 6 ? "Nhập PIN 6 số..." : "Nhập PIN 4 số...");
    // Enter on the field submits.
    lv_obj_add_event_cb(input_->obj(), OnPinReady, LV_EVENT_READY, this);

    auto *btn = lv_button_create(column);
    lv_obj_set_size(btn, 160, 44);
    lv_obj_set_style_bg_color(btn, Color(0x2b6fd6), 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_add_event_cb(btn, OnUnlock, LV_EVENT_CLICKED, this);
    auto *bl = lv_label_create(btn);
    lv_obj_set_style_text_font(bl, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(bl, lv_color_white(), 0);
    lv_label_set_text(bl, "Mở khóa");
    lv_obj_center(bl);

    status_label_ = lv_label_create(column);
    lv_obj_set_style_text_font(status_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(status_label_, Color(0x9aa0a6), 0);
    lv_label_set_text(status_label_, " ");
}

LockScreenView::~LockScreenView() {
    closed_ = true;
    if (overlay_) {
        // May run on a worker thread; guard the LVGL deletion.
        LvglLockGuard lock;
        lv_obj_del(overlay_);
        overlay_ = nullptr;
        input_ = nullptr; // freed via its LV_EVENT_DELETE -> delete self
    }
}

void LockScreenView::Start() {
    if (input_) input_->Focus();
}

void LockScreenView::SetWeather(const std::string &line) {
    if (!weather_card_ || closed_) return;
    if (line.empty()) {
        lv_obj_add_flag(weather_card_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    /* FormatLine is "<city>: <conditions>". Split so the place name can carry
     * the larger face; a line without a city degrades to detail-only. */
    const size_t sep = line.find(": ");
    if (sep == std::string::npos) {
        lv_label_set_text(weather_place_, "");
        lv_obj_add_flag(weather_place_, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(weather_detail_, line.c_str());
    } else {
        lv_obj_clear_flag(weather_place_, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(weather_place_, line.substr(0, sep).c_str());
        lv_label_set_text(weather_detail_, line.substr(sep + 2).c_str());
    }
    lv_obj_clear_flag(weather_card_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(weather_card_, LV_ALIGN_TOP_RIGHT, -18, kTopBarHeight + 12);
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
    LvglLockGuard lock;
    auto *self = static_cast<LockScreenView *>(lv_event_get_user_data(e));
    self->CheckPin();
}

void LockScreenView::OnPinReady(lv_event_t *e) {
    LvglLockGuard lock;
    auto *self = static_cast<LockScreenView *>(lv_event_get_user_data(e));
    self->CheckPin();
}

} // namespace home
