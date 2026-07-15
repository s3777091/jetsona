#include "display/widgets/status_bar.h"
#include "display/common/lvgl_utils.h"
#include "fonts.h"
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

// Short Vietnamese weekday, libc tm_wday convention (0 = Sunday).
const char *kShortWday[7] = {"CN", "T2", "T3", "T4", "T5", "T6", "T7"};

constexpr int kPillW = 520;
constexpr int kPillH = 28;
} // namespace

StatusBar::StatusBar(lv_obj_t *parent) {
    if (!parent) parent = lv_layer_top();

    // ---- Island pill (centered top) ----
    pill_ = lv_obj_create(parent);
    lv_obj_remove_style_all(pill_);
    lv_obj_set_size(pill_, kPillW, kPillH);
    lv_obj_align(pill_, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_bg_color(pill_, Color(0x000000), 0);
    lv_obj_set_style_bg_opa(pill_, LV_OPA_60, 0);
    lv_obj_set_style_radius(pill_, 14, 0);
    lv_obj_set_style_pad_left(pill_, 12, 0);
    lv_obj_set_style_pad_right(pill_, 12, 0);
    lv_obj_set_style_pad_top(pill_, 2, 0);
    lv_obj_set_style_pad_bottom(pill_, 2, 0);
    lv_obj_set_flex_flow(pill_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pill_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(pill_, LV_OBJ_FLAG_SCROLLABLE);

    auto make_cluster = [&](lv_obj_t **out) {
        auto *c = lv_obj_create(pill_);
        lv_obj_remove_style_all(c);
        lv_obj_set_size(c, LV_SIZE_CONTENT, kPillH - 4);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(c, 12, 0);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        *out = c;
    };
    make_cluster(&left_);
    make_cluster(&right_);

    auto add_icon = [&](lv_obj_t *host, lv_obj_t **out, const char *glyph,
                        lv_event_cb_t cb) {
        auto *l = lv_label_create(host);
        lv_obj_set_style_text_font(l, &BUILTIN_ICON_FONT, 0);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_label_set_text(l, glyph);
        lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
        if (cb) lv_obj_add_event_cb(l, cb, LV_EVENT_CLICKED, this);
        *out = l;
    };
    add_icon(left_, &wifi_label_, LV_SYMBOL_WIFI, OnWifiClick);
    add_icon(left_, &bt_label_, LV_SYMBOL_BLUETOOTH, OnBtClick);

    // Battery: drawn-rect icon + percent/"AC" label (display-only).
    battery_percent_label_ = lv_label_create(left_);
    lv_obj_set_style_text_font(battery_percent_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(battery_percent_label_, lv_color_white(), 0);
    lv_label_set_text(battery_percent_label_, "100%");

    battery_icon_root_ = lv_obj_create(left_);
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

    // Input language indicator (display-only, refreshed each tick).
    lang_label_ = lv_label_create(left_);
    lv_obj_set_style_text_font(lang_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(lang_label_, lv_color_white(), 0);
    lv_label_set_text(lang_label_, "EN");

    // Power/lock icon -> drops the power menu.
    add_icon(left_, &power_label_, LV_SYMBOL_POWER, OnPowerClick);

    // Right cluster: weekday + date + time.
    datetime_label_ = lv_label_create(right_);
    lv_obj_set_style_text_font(datetime_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(datetime_label_, lv_color_white(), 0);
    lv_label_set_text(datetime_label_, "--:--");

    // ---- Notification drop panel (below pill center) ----
    notif_panel_ = lv_obj_create(parent);
    lv_obj_remove_style_all(notif_panel_);
    lv_obj_set_width(notif_panel_, 440);
    lv_obj_set_height(notif_panel_, LV_SIZE_CONTENT);
    lv_obj_align_to(notif_panel_, pill_, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
    lv_obj_set_style_bg_color(notif_panel_, Color(0x000000), 0);
    lv_obj_set_style_bg_opa(notif_panel_, LV_OPA_60, 0);
    lv_obj_set_style_radius(notif_panel_, 14, 0);
    lv_obj_set_style_pad_all(notif_panel_, 10, 0);
    lv_obj_clear_flag(notif_panel_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_opa(notif_panel_, LV_OPA_0, 0);
    lv_obj_add_flag(notif_panel_, LV_OBJ_FLAG_HIDDEN);

    notif_label_ = lv_label_create(notif_panel_);
    lv_obj_set_width(notif_label_, 420);
    lv_obj_set_style_text_font(notif_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(notif_label_, lv_color_white(), 0);
    lv_label_set_long_mode(notif_label_, LV_LABEL_LONG_WRAP);
    lv_obj_center(notif_label_);
    lv_obj_add_event_cb(notif_panel_, OnNotifDeleted, LV_EVENT_DELETE, this);

    // ---- Power menu + dismiss backdrop ----
    power_backdrop_ = lv_obj_create(parent);
    lv_obj_remove_style_all(power_backdrop_);
    lv_obj_set_size(power_backdrop_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(power_backdrop_, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(power_backdrop_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(power_backdrop_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(power_backdrop_, OnPowerMenuDismiss, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(power_backdrop_, OnBackdropDeleted, LV_EVENT_DELETE, this);
    lv_obj_add_flag(power_backdrop_, LV_OBJ_FLAG_HIDDEN);

    BuildPowerMenu();

    lv_obj_add_event_cb(pill_, OnDeleted, LV_EVENT_DELETE, this);
    timer_ = lv_timer_create(OnTimer, 1000, this);
    Refresh();
}

void StatusBar::BuildPowerMenu() {
    power_menu_ = lv_obj_create(power_backdrop_);
    lv_obj_remove_style_all(power_menu_);
    lv_obj_set_size(power_menu_, 220, LV_SIZE_CONTENT);
    lv_obj_align_to(power_menu_, pill_, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
    lv_obj_set_style_bg_color(power_menu_, Color(0x000000), 0);
    lv_obj_set_style_bg_opa(power_menu_, LV_OPA_60, 0);
    lv_obj_set_style_radius(power_menu_, 14, 0);
    lv_obj_set_style_pad_all(power_menu_, 8, 0);
    lv_obj_set_style_pad_row(power_menu_, 6, 0);
    lv_obj_set_flex_flow(power_menu_, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(power_menu_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_opa(power_menu_, LV_OPA_0, 0);
    lv_obj_add_flag(power_menu_, LV_OBJ_FLAG_HIDDEN);

    auto add_item = [&](const char *label, lv_event_cb_t cb) {
        auto *row = lv_obj_create(power_menu_);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, 38);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        auto *lbl = lv_label_create(row);
        lv_obj_set_style_text_font(lbl, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_label_set_text(lbl, label);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 12, 0);
        lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, this);
    };
    add_item("Khoa", OnPowerLock);
    add_item("Khoi dong lai", OnPowerReboot);
    add_item("Tat may", OnPowerShutdown);
}

StatusBar::~StatusBar() {
    LvglLockGuard lock;
    if (timer_) { lv_timer_del(timer_); timer_ = nullptr; }
    if (notif_timer_) { lv_timer_del(notif_timer_); notif_timer_ = nullptr; }
    // notif_panel_ / power_backdrop_ are siblings of pill_ on the same parent,
    // so they are not deleted with pill_; delete them explicitly. OnDeleted
    // nulls pill_ if the layer already tore it down. power_menu_ is a child of
    // power_backdrop_, so it goes with it.
    if (notif_panel_) { lv_obj_del(notif_panel_); notif_panel_ = nullptr; }
    if (power_backdrop_) { lv_obj_del(power_backdrop_); power_backdrop_ = nullptr; }
    if (pill_) { lv_obj_del(pill_); pill_ = nullptr; }
}

void StatusBar::Hide() {
    if (pill_) lv_obj_add_flag(pill_, LV_OBJ_FLAG_HIDDEN);
    // Also hide any open drop so nothing floats above the lock screen.
    if (notif_panel_ && !lv_obj_has_flag(notif_panel_, LV_OBJ_FLAG_HIDDEN))
        lv_obj_add_flag(notif_panel_, LV_OBJ_FLAG_HIDDEN);
    if (power_backdrop_ && !lv_obj_has_flag(power_backdrop_, LV_OBJ_FLAG_HIDDEN))
        lv_obj_add_flag(power_backdrop_, LV_OBJ_FLAG_HIDDEN);
}

void StatusBar::Show() {
    if (pill_) lv_obj_clear_flag(pill_, LV_OBJ_FLAG_HIDDEN);
}

void StatusBar::Refresh() {
    RefreshClock();
    RefreshBattery();
    RefreshLang();
}

void StatusBar::RefreshClock() {
    time_t now = std::time(nullptr);
    struct tm t = *std::localtime(&now);
    if (t.tm_year < (2025 - 1900)) return; // time not set yet
    bool h24 = Settings("display").GetBool("clock_24h", true);
    char ts[16];
    std::strftime(ts, sizeof(ts), h24 ? "%H:%M" : "%I:%M", &t);
    char out[64];
    std::snprintf(out, sizeof(out), "%s  %02d/%02d/%04d  %s",
                  kShortWday[t.tm_wday], t.tm_mday, t.tm_mon + 1,
                  t.tm_year + 1900, ts);
    std::string s(out);
    if (s != cached_datetime_ && datetime_label_) {
        lv_label_set_text(datetime_label_, s.c_str());
        cached_datetime_ = s;
    }
}

void StatusBar::RefreshBattery() {
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
            lv_obj_set_size(battery_icon_fill_, 18 * level / 100, 10);
        }
        if (battery_percent_label_) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%d%%", level);
            lv_label_set_text(battery_percent_label_, buf);
        }
        // Low-battery alert (only meaningful when a real battery is present).
        if (level <= 20 && !low_warned_) {
            low_warned_ = true;
            ShowNotification("Pin yeu -- sac ngay");
        } else if (level > 25) {
            low_warned_ = false;
        }
    } else {
        if (battery_percent_label_) lv_label_set_text(battery_percent_label_, "AC");
        if (battery_icon_fill_) lv_obj_add_flag(battery_icon_fill_, LV_OBJ_FLAG_HIDDEN);
    }
}

void StatusBar::RefreshLang() {
    std::string lang = Settings("input").GetString("kbd_lang", "en");
    std::string disp = (lang == "vi") ? "VI" : "EN";
    if (disp != cached_lang_ && lang_label_) {
        lv_label_set_text(lang_label_, disp.c_str());
        cached_lang_ = disp;
    }
}

void StatusBar::AnimateDrop(lv_obj_t *obj, bool show) {
    if (!obj) return;
    lv_anim_delete(obj, OnDropOpa);
    lv_anim_delete(obj, OnDropY);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(obj, show ? LV_OPA_0 : LV_OPA_COVER, 0);
    lv_obj_set_style_translate_y(obj, show ? -10 : 0, 0);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, OnDropOpa);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_time(&a, 220);
    lv_anim_set_values(&a, show ? 0 : 255, show ? 255 : 0);
    lv_anim_start(&a);

    lv_anim_t b;
    lv_anim_init(&b);
    lv_anim_set_var(&b, obj);
    lv_anim_set_exec_cb(&b, OnDropY);
    lv_anim_set_path_cb(&b, lv_anim_path_ease_out);
    lv_anim_set_time(&b, 220);
    lv_anim_set_values(&b, show ? -10 : 0, show ? 0 : -10);
    if (!show) lv_anim_set_completed_cb(&b, OnDropHidden);
    lv_anim_start(&b);
}

void StatusBar::ShowNotification(const char *text, int duration_ms) {
    if (!notif_panel_) return;
    // The drop slot is shared with the power menu; close the menu first.
    if (power_menu_ && !lv_obj_has_flag(power_menu_, LV_OBJ_FLAG_HIDDEN)) HidePowerMenu();
    lv_label_set_text(notif_label_, text ? text : "");
    AnimateDrop(notif_panel_, true);
    if (notif_timer_) { lv_timer_del(notif_timer_); notif_timer_ = nullptr; }
    int ms = duration_ms < 800 ? 800 : duration_ms;
    notif_timer_ = lv_timer_create(OnNotifTimer, ms, this);
    lv_timer_set_repeat_count(notif_timer_, 1);
}

void StatusBar::ShowPowerMenu() {
    if (!power_menu_) return;
    if (notif_timer_) { lv_timer_del(notif_timer_); notif_timer_ = nullptr; }
    if (notif_panel_ && !lv_obj_has_flag(notif_panel_, LV_OBJ_FLAG_HIDDEN))
        AnimateDrop(notif_panel_, false);
    if (power_backdrop_) lv_obj_clear_flag(power_backdrop_, LV_OBJ_FLAG_HIDDEN);
    AnimateDrop(power_menu_, true);
}

void StatusBar::HidePowerMenu() {
    AnimateDrop(power_menu_, false);
    if (power_backdrop_) lv_obj_add_flag(power_backdrop_, LV_OBJ_FLAG_HIDDEN);
}

void StatusBar::OnTimer(lv_timer_t *t) {
    auto *self = static_cast<StatusBar *>(lv_timer_get_user_data(t));
    LvglLockGuard lock;
    self->Refresh();
}

void StatusBar::OnNotifTimer(lv_timer_t *t) {
    auto *self = static_cast<StatusBar *>(lv_timer_get_user_data(t));
    LvglLockGuard lock;
    self->notif_timer_ = nullptr;
    lv_timer_del(t);
    self->AnimateDrop(self->notif_panel_, false);
}

void StatusBar::OnDeleted(lv_event_t *e) {
    auto *self = static_cast<StatusBar *>(lv_event_get_user_data(e));
    if (!self) return;
    if (self->timer_) { lv_timer_del(self->timer_); self->timer_ = nullptr; }
    if (self->notif_timer_) { lv_timer_del(self->notif_timer_); self->notif_timer_ = nullptr; }
    self->pill_ = nullptr;
}

void StatusBar::OnNotifDeleted(lv_event_t *e) {
    auto *self = static_cast<StatusBar *>(lv_event_get_user_data(e));
    if (self) self->notif_panel_ = nullptr;
}

void StatusBar::OnBackdropDeleted(lv_event_t *e) {
    auto *self = static_cast<StatusBar *>(lv_event_get_user_data(e));
    if (!self) return;
    // power_menu_ is a child of power_backdrop_, so it goes with it.
    self->power_backdrop_ = nullptr;
    self->power_menu_ = nullptr;
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

void StatusBar::OnPowerClick(lv_event_t *e) {
    auto *self = static_cast<StatusBar *>(lv_event_get_user_data(e));
    LvglLockGuard lock;
    self->ShowPowerMenu();
}

void StatusBar::OnPowerLock(lv_event_t *e) {
    auto *self = static_cast<StatusBar *>(lv_event_get_user_data(e));
    LvglLockGuard lock;
    self->HidePowerMenu();
    if (self->lock_action_) self->lock_action_();
}

void StatusBar::OnPowerReboot(lv_event_t *e) {
    auto *self = static_cast<StatusBar *>(lv_event_get_user_data(e));
    LvglLockGuard lock;
    self->HidePowerMenu();
    if (self->reboot_action_) self->reboot_action_();
}

void StatusBar::OnPowerShutdown(lv_event_t *e) {
    auto *self = static_cast<StatusBar *>(lv_event_get_user_data(e));
    LvglLockGuard lock;
    self->HidePowerMenu();
    if (self->shutdown_action_) self->shutdown_action_();
}

void StatusBar::OnPowerMenuDismiss(lv_event_t *e) {
    auto *self = static_cast<StatusBar *>(lv_event_get_user_data(e));
    LvglLockGuard lock;
    self->HidePowerMenu();
}

void StatusBar::OnDropOpa(void *var, int32_t v) {
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(var), (lv_opa_t)v, 0);
}

void StatusBar::OnDropY(void *var, int32_t v) {
    lv_obj_set_style_translate_y(static_cast<lv_obj_t *>(var), v, 0);
}

void StatusBar::OnDropHidden(lv_anim_t *a) {
    lv_obj_add_flag(static_cast<lv_obj_t *>(lv_anim_get_var(a)), LV_OBJ_FLAG_HIDDEN);
}

} // namespace home