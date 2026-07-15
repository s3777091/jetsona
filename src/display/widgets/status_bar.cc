#include "display/widgets/status_bar.h"
#include "display/common/lvgl_utils.h"
#include "fonts.h"
#include "font_awesome.h"
#include "settings.h"
#include "board.h"

#include <lvgl.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

#define TAG "StatusBar"

namespace home {

using jetson::ui::Color;
using jetson::ui::LvglLockGuard;

namespace {
int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
} // namespace

StatusBar::StatusBar(lv_obj_t *parent) {
    if (!parent) parent = lv_layer_top();
    const int bar_h = 28;

    pill_ = lv_obj_create(parent);
    lv_obj_remove_style_all(pill_);
    lv_obj_set_size(pill_, LV_SIZE_CONTENT, bar_h);
    lv_obj_align(pill_, LV_ALIGN_TOP_RIGHT, -8, 6);
    lv_obj_set_style_bg_color(pill_, Color(0x000000), 0);
    lv_obj_set_style_bg_opa(pill_, LV_OPA_50, 0);
    lv_obj_set_style_radius(pill_, 14, 0);
    lv_obj_set_style_pad_left(pill_, 10, 0);
    lv_obj_set_style_pad_right(pill_, 10, 0);
    lv_obj_set_style_pad_top(pill_, 2, 0);
    lv_obj_set_style_pad_bottom(pill_, 2, 0);
    lv_obj_set_style_pad_column(pill_, 12, 0);
    lv_obj_set_flex_flow(pill_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pill_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(pill_, LV_OBJ_FLAG_SCROLLABLE);

    auto add_icon = [&](lv_obj_t **out, const char *glyph, lv_event_cb_t cb) {
        auto *l = lv_label_create(pill_);
        lv_obj_set_style_text_font(l, &BUILTIN_ICON_FONT, 0);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_label_set_text(l, glyph);
        lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
        if (cb) lv_obj_add_event_cb(l, cb, LV_EVENT_CLICKED, this);
        *out = l;
    };
    add_icon(&wifi_label_, FONT_AWESOME_WIFI, OnWifiClick);
    add_icon(&bt_label_, FONT_AWESOME_BLUETOOTH, OnBtClick);

    // Battery percent + drawn-rect icon (DS-02 style).
    battery_percent_label_ = lv_label_create(pill_);
    lv_obj_set_style_text_font(battery_percent_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(battery_percent_label_, lv_color_white(), 0);
    lv_label_set_text(battery_percent_label_, "100%");

    battery_icon_root_ = lv_obj_create(pill_);
    lv_obj_remove_style_all(battery_icon_root_);
    lv_obj_set_size(battery_icon_root_, 28, 16);
    lv_obj_clear_flag(battery_icon_root_, LV_OBJ_FLAG_SCROLLABLE);

    battery_icon_body_ = lv_obj_create(battery_icon_root_);
    lv_obj_remove_style_all(battery_icon_body_);
    lv_obj_set_size(battery_icon_body_, 24, 14);
    lv_obj_set_pos(battery_icon_body_, 0, 1);
    lv_obj_set_style_radius(battery_icon_body_, 2, 0);
    lv_obj_set_style_border_width(battery_icon_body_, 1, 0);
    lv_obj_set_style_border_color(battery_icon_body_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(battery_icon_body_, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(battery_icon_body_, LV_OBJ_FLAG_SCROLLABLE);

    battery_icon_fill_ = lv_obj_create(battery_icon_body_);
    lv_obj_remove_style_all(battery_icon_fill_);
    lv_obj_set_size(battery_icon_fill_, 18, 10);
    lv_obj_set_pos(battery_icon_fill_, 2, 1);
    lv_obj_set_style_radius(battery_icon_fill_, 1, 0);
    lv_obj_set_style_bg_color(battery_icon_fill_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(battery_icon_fill_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(battery_icon_fill_, LV_OBJ_FLAG_SCROLLABLE);

    auto *nub = lv_obj_create(battery_icon_root_);
    lv_obj_remove_style_all(nub);
    lv_obj_set_size(nub, 2, 6);
    lv_obj_set_pos(nub, 25, 5);
    lv_obj_set_style_bg_color(nub, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, 0);
    lv_obj_clear_flag(nub, LV_OBJ_FLAG_SCROLLABLE);

    add_icon(&volume_label_, FONT_AWESOME_VOLUME_HIGH, OnVolumeClick);

    // Clock (rightmost item).
    time_label_ = lv_label_create(pill_);
    lv_obj_set_style_text_font(time_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(time_label_, lv_color_white(), 0);
    lv_label_set_text(time_label_, "--:--");

    lv_obj_add_event_cb(pill_, OnDeleted, LV_EVENT_DELETE, this);
    timer_ = lv_timer_create(OnTimer, 1000, this);
    Refresh();
}

StatusBar::~StatusBar() {
    LvglLockGuard lock;
    if (timer_) { lv_timer_del(timer_); timer_ = nullptr; }
    // pill_ is parented to lv_layer_top(); delete it so a destroyed bar does
    // not leave a dangling widget on the top layer. OnDeleted nulls pill_ if
    // the layer already tore it down.
    if (pill_) { lv_obj_del(pill_); pill_ = nullptr; }
}

void StatusBar::Hide() {
    if (pill_) lv_obj_add_flag(pill_, LV_OBJ_FLAG_HIDDEN);
}

void StatusBar::Show() {
    if (pill_) lv_obj_clear_flag(pill_, LV_OBJ_FLAG_HIDDEN);
}

void StatusBar::Refresh() {
    RefreshClock();
    RefreshBattery();
    RefreshVolume();
}

void StatusBar::RefreshClock() {
    time_t now = std::time(nullptr);
    struct tm t = *std::localtime(&now);
    if (t.tm_year < (2025 - 1900)) return; // time not set yet
    char buf[16];
    bool h24 = Settings("display").GetBool("clock_24h", true);
    std::strftime(buf, sizeof(buf), h24 ? "%H:%M" : "%I:%M", &t);
    std::string ts(buf);
    if (ts != cached_time_ && time_label_) {
        lv_label_set_text(time_label_, ts.c_str());
        cached_time_ = ts;
    }
}

void StatusBar::RefreshBattery() {
    // Throttle the I2C (INA219) read to once / ~5 s; cache the result.
    constexpr auto kBatteryReadInterval = std::chrono::seconds(5);
    auto now = std::chrono::steady_clock::now();
    if (!battery_read_done_ || (now - last_battery_read_ >= kBatteryReadInterval)) {
        int level = 100;
        bool charging = false, discharging = false;
        has_battery_ = Board::GetInstance().GetBatteryLevel(level, charging, discharging);
        cached_battery_level_ = Clamp(level, 0, 100);
        last_battery_read_ = now;
        battery_read_done_ = true;
        (void)charging;
        (void)discharging;
    }

    if (has_battery_) {
        int level = cached_battery_level_;
        if (battery_icon_fill_) {
            lv_obj_clear_flag(battery_icon_fill_, LV_OBJ_FLAG_HIDDEN);
            // Fill width 0..18 px maps to 0..100 % (24 px body, 2 px inset each side).
            lv_obj_set_size(battery_icon_fill_, 18 * level / 100, 10);
        }
        if (battery_percent_label_) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%d%%", level);
            lv_label_set_text(battery_percent_label_, buf);
        }
    } else {
        // No battery detected (wall-powered Jetson, INA219 read failed): show
        // "AC" + an empty outline instead of the stale fallback "100%".
        if (battery_percent_label_) lv_label_set_text(battery_percent_label_, "AC");
        if (battery_icon_fill_) lv_obj_add_flag(battery_icon_fill_, LV_OBJ_FLAG_HIDDEN);
    }
}

void StatusBar::RefreshVolume() {
    // Read the persisted mute flag so a toggle from anywhere (home menu or the
    // Settings > Sound pane) is reflected here within ~1 s.
    bool muted = Settings("display").GetBool("muted", false);
    if (volume_label_) {
        lv_label_set_text(volume_label_,
                          muted ? FONT_AWESOME_VOLUME_XMARK : FONT_AWESOME_VOLUME_HIGH);
    }
}

void StatusBar::OnTimer(lv_timer_t *t) {
    auto *self = static_cast<StatusBar *>(lv_timer_get_user_data(t));
    LvglLockGuard lock;
    self->Refresh();
}

void StatusBar::OnDeleted(lv_event_t *e) {
    auto *self = static_cast<StatusBar *>(lv_event_get_user_data(e));
    if (!self) return;
    if (self->timer_) { lv_timer_del(self->timer_); self->timer_ = nullptr; }
    self->pill_ = nullptr; // the pill is being destroyed
}

void StatusBar::OnWifiClick(lv_event_t *e) {
    auto *self = static_cast<StatusBar *>(lv_event_get_user_data(e));
    LvglLockGuard lock;
    if (self->wifi_action_) self->wifi_action_();
}

void StatusBar::OnBtClick(lv_event_t *e) {
    auto *self = static_cast<StatusBar *>(lv_event_get_user_data(e));
    LvglLockGuard lock;
    if (self->bt_action_) self->bt_action_();
}

void StatusBar::OnVolumeClick(lv_event_t *e) {
    auto *self = static_cast<StatusBar *>(lv_event_get_user_data(e));
    LvglLockGuard lock;
    if (self->vol_action_) self->vol_action_();
}

} // namespace home