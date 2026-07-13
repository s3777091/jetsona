#include "settings_view.h"
#include "fonts.h"
#include "ui_theme.h"
#include "esp_log.h"

#include <lvgl.h>
#include <cstdio>
#include <cstring>

namespace home {

namespace {
lv_color_t Color(uint32_t rgb) {
    return lv_color_make((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}
} // namespace

SettingsView::SettingsView(lv_obj_t *parent, int width, int height, ClosedCb on_closed)
    : OverlayView(parent, width, height, "Cài đặt", std::move(on_closed)) {
    BuildBody();
}

void SettingsView::BuildBody() {
    const auto &p = jetson::UiTheme::Instance().Palette();

    lv_obj_set_flex_flow(body_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body_, 12, 0);
    lv_obj_clear_flag(body_, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Theme row ----
    theme_row_ = lv_obj_create(body_);
    lv_obj_remove_style_all(theme_row_);
    lv_obj_set_size(theme_row_, width_ - 16, 64);
    lv_obj_set_style_radius(theme_row_, 12, 0);
    lv_obj_set_style_bg_color(theme_row_, Color(p.row), 0);
    lv_obj_set_style_bg_opa(theme_row_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(theme_row_, 16, 0);
    lv_obj_set_style_pad_right(theme_row_, 16, 0);
    lv_obj_set_flex_flow(theme_row_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(theme_row_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(theme_row_, LV_OBJ_FLAG_SCROLLABLE);

    auto *left = lv_obj_create(theme_row_);
    lv_obj_remove_style_all(left);
    lv_obj_set_flex_grow(left, 1);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 2, 0);

    theme_label_ = lv_label_create(left);
    lv_obj_set_style_text_font(theme_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(theme_label_, Color(p.text), 0);
    lv_label_set_text(theme_label_, "Giao diện");

    mode_label_ = lv_label_create(left);
    lv_obj_set_style_text_font(mode_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(mode_label_, Color(p.sub_text), 0);

    theme_switch_ = lv_switch_create(theme_row_);
    lv_obj_set_size(theme_switch_, 70, 32);
    // On = light, off = dark.
    if (jetson::UiTheme::Instance().Mode() == jetson::UiMode::Light)
        lv_obj_add_state(theme_switch_, LV_STATE_CHECKED);
    lv_obj_add_event_cb(theme_switch_, OnSwitchChanged, LV_EVENT_VALUE_CHANGED, this);

    // ---- About row ----
    about_label_ = lv_label_create(body_);
    lv_obj_set_style_text_font(about_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(about_label_, Color(p.sub_text), 0);
    lv_obj_set_width(about_label_, width_ - 32);
    lv_label_set_long_mode(about_label_, LV_LABEL_LONG_WRAP);
    Recolor();
}

void SettingsView::Recolor() {
    const auto &p = jetson::UiTheme::Instance().Palette();
    if (theme_row_) lv_obj_set_style_bg_color(theme_row_, Color(p.row), 0);
    if (theme_label_) lv_obj_set_style_text_color(theme_label_, Color(p.text), 0);
    if (mode_label_) {
        lv_obj_set_style_text_color(mode_label_, Color(p.sub_text), 0);
        lv_label_set_text(mode_label_,
                          jetson::UiTheme::Instance().Mode() == jetson::UiMode::Light
                              ? "Sáng" : "Tối");
    }
    if (about_label_) {
        lv_obj_set_style_text_color(about_label_, Color(p.sub_text), 0);
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "Thiết bị: %s\nMàn hình: %dx%d\nFirmware: jetson-fw 0.1",
                      BOARD_NAME, width_, height_);
        lv_label_set_text(about_label_, buf);
    }
}

void SettingsView::OnToggle() {
    jetson::UiTheme::Instance().Toggle();
    Recolor();
    SetStatus("Đã đổi giao diện");
}

void SettingsView::OnStart() {
    SetStatus("Bật/tắt giao diện sáng tối");
}

void SettingsView::OnSwitchChanged(lv_event_t *e) {
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    self->OnToggle();
}

} // namespace home