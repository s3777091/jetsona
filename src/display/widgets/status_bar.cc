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

// The screen is 800x480, so these keep the resting island clearly separated
// from the left/right status clusters while leaving enough room for Vietnamese
// notification text when it expands.
constexpr int kPillW = 132;
constexpr int kPillH = 36;
constexpr int kExpandedW = 470;
constexpr int kExpandedH = 82;
constexpr int kTopInset = 3;
constexpr int kAutoCloseMs = 6000;
} // namespace

StatusBar::StatusBar(lv_obj_t *parent) {
    if (!parent) parent = lv_layer_top();

    // ---- Transparent iPhone-like status row ----
    // Status information belongs beside the sensor cutout, never inside it.
    status_strip_ = lv_obj_create(parent);
    lv_obj_remove_style_all(status_strip_);
    lv_obj_set_size(status_strip_, lv_pct(100), 42);
    lv_obj_set_pos(status_strip_, 0, 0);
    lv_obj_clear_flag(status_strip_,
                      (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    auto add_icon = [&](lv_obj_t **out, const char *glyph, lv_event_cb_t cb) {
        auto *l = lv_label_create(status_strip_);
        lv_obj_set_style_text_font(l, &BUILTIN_ICON_FONT, 0);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_label_set_text(l, glyph);
        lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(l, 8);
        if (cb) lv_obj_add_event_cb(l, cb, LV_EVENT_CLICKED, this);
        *out = l;
    };

    // A real iPhone status row shows only the time here; the long date that
    // previously made this look like a desktop menu bar is intentionally gone.
    datetime_label_ = lv_label_create(status_strip_);
    lv_obj_set_style_text_font(datetime_label_, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(datetime_label_, lv_color_white(), 0);
    lv_label_set_text(datetime_label_, "--:--");
    lv_obj_align(datetime_label_, LV_ALIGN_LEFT_MID, 14, 0);

    add_icon(&wifi_label_, LV_SYMBOL_WIFI, OnWifiClick);
    lv_obj_align(wifi_label_, LV_ALIGN_RIGHT_MID, -190, 0);
    add_icon(&bt_label_, LV_SYMBOL_BLUETOOTH, OnBtClick);
    lv_obj_align(bt_label_, LV_ALIGN_RIGHT_MID, -154, 0);

    battery_icon_root_ = lv_obj_create(status_strip_);
    lv_obj_remove_style_all(battery_icon_root_);
    lv_obj_set_size(battery_icon_root_, 52, 20);
    lv_obj_align(battery_icon_root_, LV_ALIGN_RIGHT_MID, -73, 0);
    lv_obj_clear_flag(battery_icon_root_,
                      (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    battery_icon_body_ = lv_obj_create(battery_icon_root_);
    lv_obj_remove_style_all(battery_icon_body_);
    lv_obj_set_size(battery_icon_body_, 47, 20);
    lv_obj_set_pos(battery_icon_body_, 0, 0);
    lv_obj_set_style_radius(battery_icon_body_, 6, 0);
    lv_obj_set_style_border_width(battery_icon_body_, 1, 0);
    lv_obj_set_style_border_color(battery_icon_body_, lv_color_white(), 0);
    lv_obj_set_style_bg_color(battery_icon_body_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(battery_icon_body_, LV_OPA_70, 0);
    lv_obj_clear_flag(battery_icon_body_,
                      (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    battery_icon_fill_ = lv_obj_create(battery_icon_body_);
    lv_obj_remove_style_all(battery_icon_fill_);
    lv_obj_set_size(battery_icon_fill_, 41, 16);
    lv_obj_set_pos(battery_icon_fill_, 2, 1);
    lv_obj_set_style_radius(battery_icon_fill_, 4, 0);
    lv_obj_set_style_bg_color(battery_icon_fill_, Color(0x34c759), 0);
    lv_obj_set_style_bg_opa(battery_icon_fill_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(battery_icon_fill_,
                      (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    // Percentage is drawn inside the battery body, above the colored fill.
    battery_percent_label_ = lv_label_create(battery_icon_body_);
    lv_obj_set_style_text_font(battery_percent_label_, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(battery_percent_label_, lv_color_black(), 0);
    lv_label_set_text(battery_percent_label_, "100%");
    lv_obj_center(battery_percent_label_);

    auto *nub = lv_obj_create(battery_icon_root_);
    lv_obj_remove_style_all(nub);
    lv_obj_set_size(nub, 3, 8);
    lv_obj_set_pos(nub, 48, 6);
    lv_obj_set_style_radius(nub, 1, 0);
    lv_obj_set_style_bg_color(nub, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, 0);
    lv_obj_clear_flag(nub,
                      (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    lang_label_ = lv_label_create(status_strip_);
    lv_obj_set_style_text_font(lang_label_, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(lang_label_, lv_color_white(), 0);
    lv_label_set_text(lang_label_, "EN");
    lv_obj_align(lang_label_, LV_ALIGN_RIGHT_MID, -43, 0);

    add_icon(&power_label_, LV_SYMBOL_POWER, OnPowerClick);
    lv_obj_align(power_label_, LV_ALIGN_RIGHT_MID, -12, 0);

    // ---- Resting Dynamic Island (centered, solid black) ----
    pill_ = lv_obj_create(parent);
    lv_obj_remove_style_all(pill_);
    lv_obj_set_size(pill_, kPillW, kPillH);
    lv_obj_align(pill_, LV_ALIGN_TOP_MID, 0, kTopInset);
    lv_obj_set_style_bg_color(pill_, Color(0x000000), 0);
    lv_obj_set_style_bg_opa(pill_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pill_, 44, 0);
    lv_obj_set_style_border_width(pill_, 1, 0);
    lv_obj_set_style_border_color(pill_, Color(0x253142), 0);
    lv_obj_set_style_border_opa(pill_, LV_OPA_40, 0);
    lv_obj_set_style_shadow_color(pill_, lv_color_black(), 0);
    lv_obj_set_style_shadow_width(pill_, 12, 0);
    lv_obj_set_style_shadow_opa(pill_, LV_OPA_30, 0);
    lv_obj_clear_flag(pill_, LV_OBJ_FLAG_SCROLLABLE);

    // Expanded content is already laid out inside the pill, but kept hidden
    // until the pill has started to bloom. This avoids a separate toast panel.
    island_content_ = lv_obj_create(pill_);
    lv_obj_remove_style_all(island_content_);
    lv_obj_set_size(island_content_, lv_pct(100), lv_pct(100));
    lv_obj_clear_flag(island_content_,
                      (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_style_opa(island_content_, LV_OPA_0, 0);
    lv_obj_add_flag(island_content_, LV_OBJ_FLAG_HIDDEN);

    island_icon_bg_ = lv_obj_create(island_content_);
    lv_obj_remove_style_all(island_icon_bg_);
    lv_obj_set_size(island_icon_bg_, 44, 44);
    lv_obj_align(island_icon_bg_, LV_ALIGN_LEFT_MID, 15, 0);
    lv_obj_set_style_radius(island_icon_bg_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(island_icon_bg_, Color(0x1677ff), 0);
    lv_obj_set_style_bg_opa(island_icon_bg_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(island_icon_bg_,
                      (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    island_icon_ = lv_label_create(island_icon_bg_);
    lv_obj_set_style_text_font(island_icon_, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(island_icon_, lv_color_white(), 0);
    lv_label_set_text(island_icon_, LV_SYMBOL_BELL);
    lv_obj_center(island_icon_);

    island_title_ = lv_label_create(island_content_);
    lv_obj_set_style_text_font(island_title_, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(island_title_, Color(0x69b7ff), 0);
    lv_obj_set_pos(island_title_, 72, 10);
    lv_label_set_text(island_title_, "THÔNG BÁO");

    island_message_ = lv_label_create(island_content_);
    lv_obj_set_size(island_message_, kExpandedW - 92, 28);
    lv_obj_set_style_text_font(island_message_, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(island_message_, lv_color_white(), 0);
    lv_obj_set_pos(island_message_, 72, 39);
    lv_label_set_long_mode(island_message_, LV_LABEL_LONG_DOT);
    lv_label_set_text(island_message_, "");

    // ---- Power menu drop (below pill center, on layer_top; no full-screen
    // backdrop -- a full-screen clickable on layer_top intercepted mouse input,
    // so the menu now dismisses via the power icon toggle + an auto-close
    // timer + tapping an action). ----
    BuildPowerMenu();

    lv_obj_add_event_cb(pill_, OnDeleted, LV_EVENT_DELETE, this);
    timer_ = lv_timer_create(OnTimer, 1000, this);
    Refresh();
}

void StatusBar::BuildPowerMenu() {
    power_menu_ = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(power_menu_);
    lv_obj_set_size(power_menu_, 220, LV_SIZE_CONTENT);
    lv_obj_align_to(power_menu_, pill_, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
    lv_obj_set_style_bg_color(power_menu_, Color(0x000000), 0);
    lv_obj_set_style_bg_opa(power_menu_, LV_OPA_90, 0);
    lv_obj_set_style_radius(power_menu_, 14, 0);
    lv_obj_set_style_pad_all(power_menu_, 8, 0);
    lv_obj_set_style_pad_row(power_menu_, 6, 0);
    lv_obj_set_flex_flow(power_menu_, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(power_menu_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_opa(power_menu_, LV_OPA_0, 0);
    lv_obj_add_flag(power_menu_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(power_menu_, OnPowerMenuDeleted, LV_EVENT_DELETE, this);

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
    add_item("Khóa", OnPowerLock);
    add_item("Khởi động lại", OnPowerReboot);
    add_item("Tắt máy", OnPowerShutdown);
}

StatusBar::~StatusBar() {
    LvglLockGuard lock;
    if (timer_) { lv_timer_del(timer_); timer_ = nullptr; }
    if (notif_timer_) { lv_timer_del(notif_timer_); notif_timer_ = nullptr; }
    if (power_menu_timer_) { lv_timer_del(power_menu_timer_); power_menu_timer_ = nullptr; }
    // Siblings on the same top layer are not deleted with pill_; delete them
    // explicitly. Delete callbacks null pointers if the layer tore down first.
    if (power_menu_) { lv_obj_del(power_menu_); power_menu_ = nullptr; }
    if (pill_) { lv_obj_del(pill_); pill_ = nullptr; }
    if (status_strip_) { lv_obj_del(status_strip_); status_strip_ = nullptr; }
}

void StatusBar::Hide() {
    visible_ = false;
    if (notif_timer_) { lv_timer_del(notif_timer_); notif_timer_ = nullptr; }
    if (pill_) {
        lv_anim_delete(pill_, OnIslandWidth);
        lv_anim_delete(pill_, OnIslandHeight);
        lv_obj_set_size(pill_, kPillW, kPillH);
        lv_obj_align(pill_, LV_ALIGN_TOP_MID, 0, kTopInset);
        lv_obj_add_flag(pill_, LV_OBJ_FLAG_HIDDEN);
    }
    if (island_content_) {
        lv_anim_delete(island_content_, OnIslandContentOpa);
        lv_obj_set_style_opa(island_content_, LV_OPA_0, 0);
        lv_obj_add_flag(island_content_, LV_OBJ_FLAG_HIDDEN);
    }
    if (status_strip_) lv_obj_add_flag(status_strip_, LV_OBJ_FLAG_HIDDEN);
    island_expanded_ = false;
    if (power_menu_ && !lv_obj_has_flag(power_menu_, LV_OBJ_FLAG_HIDDEN))
        HidePowerMenu();
}

void StatusBar::Show() {
    visible_ = true;
    if (status_strip_) lv_obj_clear_flag(status_strip_, LV_OBJ_FLAG_HIDDEN);
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
    std::string s(ts);
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
        cached_battery_charging_ = charging;
        (void)discharging;
    }

    // Some Jetson installations have no readable battery sensor. Keep the
    // battery visual in that case (100%) instead of leaking the old "AC" text.
    const int level = has_battery_ ? cached_battery_level_ : 100;
    if (battery_icon_fill_) {
        if (level <= 0) {
            lv_obj_add_flag(battery_icon_fill_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(battery_icon_fill_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(battery_icon_fill_, std::max(1, 41 * level / 100), 16);
        }
        const uint32_t fill_color = cached_battery_charging_ || level > 50
                                        ? 0x34c759
                                        : (level > 20 ? 0xffcc00 : 0xff3b30);
        lv_obj_set_style_bg_color(battery_icon_fill_, Color(fill_color), 0);
    }
    if (battery_percent_label_) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%d%%", level);
        lv_label_set_text(battery_percent_label_, buf);
        // Dark digits remain crisp over a mostly filled bright battery; white
        // is clearer once the fill drops behind most of the percentage.
        lv_obj_set_style_text_color(battery_percent_label_,
                                    level >= 45 ? lv_color_black()
                                                : lv_color_white(), 0);
    }
    if (has_battery_) {
        if (level <= 20 && !low_warned_) {
            low_warned_ = true;
            ShowNotification("Pin yếu — hãy sạc thiết bị");
        } else if (level > 25) {
            low_warned_ = false;
        }
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

void StatusBar::AnimateIslandSize(int width, int height, bool collapsing) {
    if (!pill_) return;
    lv_obj_update_layout(pill_);
    const int from_w = lv_obj_get_width(pill_);
    const int from_h = lv_obj_get_height(pill_);
    lv_anim_delete(pill_, OnIslandWidth);
    lv_anim_delete(pill_, OnIslandHeight);

    lv_anim_t w;
    lv_anim_init(&w);
    lv_anim_set_var(&w, pill_);
    lv_anim_set_exec_cb(&w, OnIslandWidth);
    lv_anim_set_values(&w, from_w, width);
    lv_anim_set_time(&w, collapsing ? 320 : 420);
    lv_anim_set_delay(&w, collapsing ? 40 : 0);
    lv_anim_set_path_cb(&w, lv_anim_path_ease_in_out);
    lv_anim_start(&w);

    lv_anim_t h;
    lv_anim_init(&h);
    lv_anim_set_var(&h, pill_);
    lv_anim_set_exec_cb(&h, OnIslandHeight);
    lv_anim_set_values(&h, from_h, height);
    lv_anim_set_time(&h, collapsing ? 320 : 420);
    lv_anim_set_delay(&h, collapsing ? 40 : 0);
    lv_anim_set_path_cb(&h, lv_anim_path_ease_in_out);
    if (collapsing) {
        lv_anim_set_completed_cb(&h, OnIslandCollapsed);
        lv_anim_set_user_data(&h, this);
    }
    lv_anim_start(&h);
}

void StatusBar::ShowIslandMessage(const char *title, const char *text,
                                  const char *icon, uint32_t accent,
                                  int duration_ms) {
    if (!visible_ || !pill_ || !island_content_) return;
    if (power_menu_ && !lv_obj_has_flag(power_menu_, LV_OBJ_FLAG_HIDDEN))
        HidePowerMenu();

    if (notif_timer_) { lv_timer_del(notif_timer_); notif_timer_ = nullptr; }
    lv_label_set_text(island_title_, title ? title : "");
    lv_label_set_text(island_message_, text ? text : "");
    lv_label_set_text(island_icon_, icon ? icon : LV_SYMBOL_BELL);
    lv_obj_set_style_bg_color(island_icon_bg_, Color(accent), 0);
    lv_obj_set_style_text_color(island_title_, Color(accent), 0);
    lv_obj_set_style_border_color(pill_, Color(accent), 0);
    lv_obj_set_style_border_opa(pill_, LV_OPA_40, 0);

    lv_obj_clear_flag(island_content_, LV_OBJ_FLAG_HIDDEN);
    lv_anim_delete(island_content_, OnIslandContentOpa);
    lv_obj_set_style_opa(island_content_, LV_OPA_0, 0);
    AnimateIslandSize(kExpandedW, kExpandedH, false);

    lv_anim_t content;
    lv_anim_init(&content);
    lv_anim_set_var(&content, island_content_);
    lv_anim_set_exec_cb(&content, OnIslandContentOpa);
    lv_anim_set_values(&content, 0, 255);
    lv_anim_set_delay(&content, 140);
    lv_anim_set_time(&content, 220);
    lv_anim_set_path_cb(&content, lv_anim_path_ease_in_out);
    lv_anim_start(&content);

    island_expanded_ = true;
    const int ms = std::max(1200, duration_ms);
    notif_timer_ = lv_timer_create(OnNotifTimer, ms, this);
    lv_timer_set_repeat_count(notif_timer_, 1);
}

void StatusBar::CollapseIsland(bool animated) {
    if (!pill_ || !island_content_) return;
    if (notif_timer_) { lv_timer_del(notif_timer_); notif_timer_ = nullptr; }

    lv_anim_delete(island_content_, OnIslandContentOpa);
    if (!animated) {
        lv_anim_delete(pill_, OnIslandWidth);
        lv_anim_delete(pill_, OnIslandHeight);
        lv_obj_set_size(pill_, kPillW, kPillH);
        lv_obj_align(pill_, LV_ALIGN_TOP_MID, 0, kTopInset);
        lv_obj_set_style_opa(island_content_, LV_OPA_0, 0);
        lv_obj_add_flag(island_content_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_border_color(pill_, Color(0x253142), 0);
        island_expanded_ = false;
        return;
    }
    if (!island_expanded_) return;

    lv_anim_t content;
    lv_anim_init(&content);
    lv_anim_set_var(&content, island_content_);
    lv_anim_set_exec_cb(&content, OnIslandContentOpa);
    lv_anim_set_values(&content, lv_obj_get_style_opa(island_content_, 0), 0);
    lv_anim_set_time(&content, 120);
    lv_anim_set_path_cb(&content, lv_anim_path_ease_in);
    lv_anim_start(&content);
    AnimateIslandSize(kPillW, kPillH, true);
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
    ShowIslandMessage("THÔNG BÁO", text, LV_SYMBOL_BELL, 0x69b7ff,
                      duration_ms);
}

void StatusBar::ShowWelcome(int duration_ms) {
    ShowIslandMessage("XIN CHÀO", "Chào mừng bạn đến với Ekko Land",
                      LV_SYMBOL_HOME, 0x62e6a7, duration_ms);
}

void StatusBar::ShowPowerMenu() {
    if (!power_menu_) return;
    CollapseIsland(false);
    lv_obj_align_to(power_menu_, pill_, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
    AnimateDrop(power_menu_, true);
    if (power_menu_timer_) { lv_timer_del(power_menu_timer_); power_menu_timer_ = nullptr; }
    power_menu_timer_ = lv_timer_create(OnPowerMenuTimer, kAutoCloseMs, this);
    lv_timer_set_repeat_count(power_menu_timer_, 1);
}

void StatusBar::HidePowerMenu() {
    if (power_menu_timer_) { lv_timer_del(power_menu_timer_); power_menu_timer_ = nullptr; }
    if (power_menu_) AnimateDrop(power_menu_, false);
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
    self->CollapseIsland(true);
}

void StatusBar::OnPowerMenuTimer(lv_timer_t *t) {
    auto *self = static_cast<StatusBar *>(lv_timer_get_user_data(t));
    LvglLockGuard lock;
    self->power_menu_timer_ = nullptr;
    lv_timer_del(t);
    self->HidePowerMenu();
}

void StatusBar::OnDeleted(lv_event_t *e) {
    auto *self = static_cast<StatusBar *>(lv_event_get_user_data(e));
    if (!self) return;
    if (self->timer_) { lv_timer_del(self->timer_); self->timer_ = nullptr; }
    if (self->notif_timer_) { lv_timer_del(self->notif_timer_); self->notif_timer_ = nullptr; }
    if (self->power_menu_timer_) { lv_timer_del(self->power_menu_timer_); self->power_menu_timer_ = nullptr; }
    self->pill_ = nullptr;
}

void StatusBar::OnPowerMenuDeleted(lv_event_t *e) {
    auto *self = static_cast<StatusBar *>(lv_event_get_user_data(e));
    if (self) self->power_menu_ = nullptr;
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
    // Toggle: open if closed, close if open.
    if (self->power_menu_ && !lv_obj_has_flag(self->power_menu_, LV_OBJ_FLAG_HIDDEN))
        self->HidePowerMenu();
    else
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

void StatusBar::OnIslandWidth(void *var, int32_t v) {
    auto *obj = static_cast<lv_obj_t *>(var);
    lv_obj_set_width(obj, v);
    lv_obj_align(obj, LV_ALIGN_TOP_MID, 0, kTopInset);
}

void StatusBar::OnIslandHeight(void *var, int32_t v) {
    auto *obj = static_cast<lv_obj_t *>(var);
    lv_obj_set_height(obj, v);
    lv_obj_align(obj, LV_ALIGN_TOP_MID, 0, kTopInset);
}

void StatusBar::OnIslandContentOpa(void *var, int32_t v) {
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(var), (lv_opa_t)v, 0);
}

void StatusBar::OnIslandCollapsed(lv_anim_t *a) {
    auto *self = static_cast<StatusBar *>(lv_anim_get_user_data(a));
    if (!self || !self->pill_) return;
    lv_obj_set_size(self->pill_, kPillW, kPillH);
    lv_obj_align(self->pill_, LV_ALIGN_TOP_MID, 0, kTopInset);
    if (self->island_content_) {
        lv_obj_set_style_opa(self->island_content_, LV_OPA_0, 0);
        lv_obj_add_flag(self->island_content_, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_border_color(self->pill_, Color(0x253142), 0);
    self->island_expanded_ = false;
}

void StatusBar::OnDropOpa(void *var, int32_t v) {
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(var), (lv_opa_t)v, 0);
}

void StatusBar::OnDropY(void *var, int32_t v) {
    lv_obj_set_style_translate_y(static_cast<lv_obj_t *>(var), v, 0);
}

void StatusBar::OnDropHidden(lv_anim_t *a) {
    // lv_anim_t::var is a public field (there is no lv_anim_get_var accessor).
    if (a && a->var) lv_obj_add_flag(static_cast<lv_obj_t *>(a->var), LV_OBJ_FLAG_HIDDEN);
}

} // namespace home
