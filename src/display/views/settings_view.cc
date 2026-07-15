#include "display/views/settings_view.h"
#include "display/common/lvgl_utils.h"
#include "display/common/signal_bars.h"
#include "fonts.h"
#include "display/theme/ui_theme.h"
#include "lvgl_runtime.h"
#include "platform/shell_command.h"
#include "settings.h"
#include "esp_log.h"

#include <lvgl.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <unistd.h>
#include <vector>

#include <sys/statvfs.h>
#include <sys/sysinfo.h>

#define TAG "SettingsView"

namespace home {

using jetson::ui::Color;
using LvLockGuard = jetson::ui::LvglLockGuard;

namespace {
// Run `cmd` via /bin/sh, capture combined stdout+stderr (trimmed).
std::string RunCapture(const std::string &cmd) {
    auto result = jetson::platform::RunShellCommand(cmd);
    jetson::platform::TrimTrailingWhitespace(result.output);
    return std::move(result.output);
}

// Sidebar glyphs (FONT_AWESOME / LV_SYMBOL).
struct SleepOpt { int seconds; const char *label; };
const SleepOpt kSleepOpts[] = {
    {30, "30 giây"}, {60, "1 phút"}, {120, "2 phút"}, {180, "3 phút"},
    {240, "4 phút"}, {300, "5 phút"}, {0, "Không"},
};
constexpr int kFontSizes[] = {22, 24, 26, 28, 30, 32, 34};

int FontStepForSize(int size) {
    int best = 0;
    for (int i = 1; i < (int)(sizeof(kFontSizes) / sizeof(kFontSizes[0])); ++i) {
        if (std::abs(kFontSizes[i] - size) < std::abs(kFontSizes[best] - size)) best = i;
    }
    return best;
}

const char *SleepLabel(int seconds) {
    for (const auto &option : kSleepOpts)
        if (option.seconds == seconds) return option.label;
    return "Không";
}
const char *kTimezones[] = {
    "Asia/Ho_Chi_Minh", "Asia/Bangkok", "Asia/Tokyo", "Asia/Shanghai",
    "Asia/Singapore", "Asia/Hong_Kong", "Asia/Kolkata", "Asia/Dubai",
    "Europe/London", "Europe/Paris", "Europe/Berlin", "Europe/Moscow",
    "America/New_York", "America/Chicago", "America/Los_Angeles",
    "America/Sao_Paulo", "Australia/Sydney", "UTC",
};

std::string CurrentTimezone() {
    // Keep category switching non-blocking: do not shell out to timedatectl
    // while the LVGL event mutex is held.  Prefer our persisted value, then
    // resolve /etc/localtime directly with readlink(2).
    std::string tz = Settings("system", false).GetString("timezone", "");
    if (!tz.empty()) return tz;
    char target[512]{};
    const ssize_t n = ::readlink("/etc/localtime", target, sizeof(target) - 1);
    if (n > 0) {
        target[n] = '\0';
        tz.assign(target);
        auto p = tz.find("zoneinfo/");
        if (p != std::string::npos) tz = tz.substr(p + 9);
    }
    if (tz.empty()) tz = "Asia/Ho_Chi_Minh";
    return tz;
}

std::string FormatBytes(unsigned long long bytes) {
    char buf[32];
    double b = (double)bytes;
    if (bytes < 1024ULL) std::snprintf(buf, sizeof(buf), "%llu B", bytes);
    else if (bytes < 1024ULL * 1024) std::snprintf(buf, sizeof(buf), "%.1f KB", b / 1024);
    else if (bytes < 1024ULL * 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.1f MB", b / (1024 * 1024));
    else std::snprintf(buf, sizeof(buf), "%.1f GB", b / (1024.0 * 1024 * 1024));
    return buf;
}

void SetWifiSheetTranslateY(void *obj, int32_t value) {
    lv_obj_set_style_translate_y(static_cast<lv_obj_t *>(obj), value, 0);
}

} // namespace

// =========================================================================
// Construction + layout
// =========================================================================

std::shared_ptr<SettingsView> SettingsView::Self() {
    // shared_from_this() yields shared_ptr<OverlayView>; cast down to this type
    // so worker lambdas can call SettingsView members.
    return std::static_pointer_cast<SettingsView>(shared_from_this());
}

SettingsView::SettingsView(lv_obj_t *parent, int width, int height,
                           jetson::IWifiManager &wifi,
                           jetson::IBluetoothManager &bluetooth,
                           ClosedCb on_closed)
    : OverlayView(parent, width, height, u8"Cài đặt", std::move(on_closed)),
      wifi_(wifi), bluetooth_(bluetooth) {
    BuildShell();
}

void SettingsView::BuildShell() {
    const auto &p = jetson::UiTheme::Instance().Palette();

    // body_: two columns (sidebar + detail).
    lv_obj_set_flex_flow(body_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(body_, 8, 0);
    lv_obj_set_style_pad_column(body_, 8, 0);
    lv_obj_clear_flag(body_, LV_OBJ_FLAG_SCROLLABLE);

    int body_h = (height_ - 48) - 16;

    // ---- Sidebar ----
    sidebar_ = lv_obj_create(body_);
    lv_obj_remove_style_all(sidebar_);
    lv_obj_set_size(sidebar_, 196, body_h);
    lv_obj_set_style_bg_color(sidebar_, Color(p.row), 0);
    lv_obj_set_style_bg_opa(sidebar_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(sidebar_, 12, 0);
    lv_obj_set_style_pad_all(sidebar_, 6, 0);
    lv_obj_set_style_pad_row(sidebar_, 4, 0);
    lv_obj_set_flex_flow(sidebar_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sidebar_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_add_flag(sidebar_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(sidebar_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(sidebar_, LV_SCROLLBAR_MODE_ACTIVE);

    struct Entry { Cat cat; const char *glyph; const char *label; };
    const Entry cats[] = {
        {Cat::Display, LV_SYMBOL_EYE_OPEN, "Màn hình"},
        {Cat::Sound, LV_SYMBOL_VOLUME_MAX, "Âm thanh"},
        {Cat::Wifi, LV_SYMBOL_WIFI, "WiFi"},
        {Cat::Bluetooth, LV_SYMBOL_BLUETOOTH, "Bluetooth"},
        {Cat::Keyboard, LV_SYMBOL_KEYBOARD, "Bàn phím"},
        {Cat::DateTime, LV_SYMBOL_BELL, "Ngày giờ"},
        {Cat::Power, LV_SYMBOL_POWER, "Nguồn & Khóa"},
        {Cat::About, LV_SYMBOL_LIST, "Giới thiệu"},
    };
    for (const auto &e : cats) AddSidebarRow(e.cat, e.glyph, e.label);

    // ---- Detail pane ----
    detail_ = lv_obj_create(body_);
    lv_obj_remove_style_all(detail_);
    lv_obj_set_flex_grow(detail_, 1);
    lv_obj_set_height(detail_, body_h);
    lv_obj_set_style_bg_color(detail_, Color(p.row), 0);
    lv_obj_set_style_bg_opa(detail_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(detail_, 12, 0);
    lv_obj_set_style_pad_all(detail_, 10, 0);
    lv_obj_set_style_pad_row(detail_, 10, 0);
    lv_obj_set_flex_flow(detail_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(detail_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_add_flag(detail_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(detail_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(detail_, LV_SCROLLBAR_MODE_ACTIVE);

    ShowCategory(Cat::Display);
}

void SettingsView::AddSidebarRow(Cat cat, const char *glyph, const char *label) {
    const auto &p = jetson::UiTheme::Instance().Palette();
    auto *row = lv_obj_create(sidebar_);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 44);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_left(row, 8, 0);
    lv_obj_set_style_pad_right(row, 8, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

    auto *ic = lv_label_create(row);
    lv_obj_set_style_text_font(ic, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(ic, Color(p.sub_text), 0);
    lv_label_set_text(ic, glyph);

    auto *lbl = lv_label_create(row);
    lv_obj_set_style_text_font(lbl, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(lbl, Color(p.text), 0);
    lv_label_set_text(lbl, label);

    auto *ctx = new SideCtx{this, cat};
    lv_obj_set_user_data(row, ctx); // so HighlightSide can read the category
    lv_obj_add_event_cb(row, OnSideClicked, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(row, OnSideDeleted, LV_EVENT_DELETE, ctx);
    side_rows_.push_back(row);
}

void SettingsView::HighlightSide(Cat cat) {
    const auto &p = jetson::UiTheme::Instance().Palette();
    for (auto *r : side_rows_) {
        auto *sd = static_cast<SideCtx *>(lv_obj_get_user_data(r));
        bool sel = sd && sd->cat == cat;
        lv_obj_set_style_bg_color(r, sel ? Color(p.accent) : Color(p.row), 0);
        lv_obj_set_style_bg_opa(r, sel ? LV_OPA_20 : LV_OPA_TRANSP, 0);
        auto *ic = lv_obj_get_child(r, 0);  // icon label
        auto *lbl = lv_obj_get_child(r, 1); // text label
        if (ic) lv_obj_set_style_text_color(ic, sel ? Color(p.accent) : Color(p.sub_text), 0);
        if (lbl) lv_obj_set_style_text_color(lbl, sel ? Color(p.accent) : Color(p.text), 0);
    }
}

void SettingsView::ShowCategory(Cat c) {
    current_ = c;
    ClearDetail();
    switch (c) {
        case Cat::Display: BuildDisplay(); break;
        case Cat::Sound: BuildSound(); break;
        case Cat::Wifi: BuildWifi(); break;
        case Cat::Bluetooth: BuildBluetooth(); break;
        case Cat::Keyboard: BuildKeyboard(); break;
        case Cat::DateTime: BuildDateTime(); break;
        case Cat::Power: BuildPower(); break;
        case Cat::About: BuildAbout(); break;
    }
    HighlightSide(c);
}

void SettingsView::ClearDetail() {
    if (detail_) lv_obj_clean(detail_);
    kbd_demo_ = nullptr;
    lang_vi_btn_ = nullptr;
    lang_en_btn_ = nullptr;
    wifi_switch_ = nullptr;
    wifi_list_ = nullptr;
    bt_switch_ = nullptr;
    bt_list_ = nullptr;
    bright_slider_ = nullptr; // (declared via locals; reset modal refs below)
    bright_value_label_ = nullptr;
    text_size_slider_ = nullptr;
    text_size_value_label_ = nullptr;
    night_warmth_slider_ = nullptr;
    vol_slider_ = nullptr;
    mute_switch_ = nullptr;
    // Any open modal belongs to overlay_, not detail_; leave it.
}

lv_obj_t *SettingsView::SectionTitle(const char *text) {
    const auto &p = jetson::UiTheme::Instance().Palette();
    auto *lbl = lv_label_create(detail_);
    lv_obj_set_style_text_font(lbl, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(lbl, Color(p.text), 0);
    lv_label_set_text(lbl, text);
    return lbl;
}

lv_obj_t *SettingsView::MakeRow(const char *title, const char *sub) {
    const auto &p = jetson::UiTheme::Instance().Palette();
    auto *row = lv_obj_create(detail_);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, sub ? 64 : 52);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_bg_color(row, Color(p.bg), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(row, 14, 0);
    lv_obj_set_style_pad_right(row, 14, 0);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    auto *left = lv_obj_create(row);
    lv_obj_remove_style_all(left);
    lv_obj_set_flex_grow(left, 1);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 2, 0);
    auto *t = lv_label_create(left);
    lv_obj_set_style_text_font(t, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(t, Color(p.text), 0);
    lv_label_set_text(t, title);
    if (sub) {
        auto *s = lv_label_create(left);
        lv_obj_set_style_text_font(s, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(s, Color(p.sub_text), 0);
        lv_label_set_text(s, sub);
    }
    return row;
}

lv_obj_t *SettingsView::MakeSwitch(lv_obj_t *parent, bool on, lv_event_cb_t cb) {
    auto *sw = lv_switch_create(parent);
    lv_obj_set_size(sw, 52, 28);
    lv_obj_set_style_bg_color(sw, Color(0x55565a), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, Color(0x30c967), LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_shadow_color(sw, lv_color_black(), LV_PART_KNOB);
    lv_obj_set_style_shadow_width(sw, 5, LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(sw, LV_OPA_30, LV_PART_KNOB);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, this);
    return sw;
}

lv_obj_t *SettingsView::MakeSlider(lv_obj_t *parent, int minv, int maxv, int val,
                                   lv_event_cb_t cb) {
    auto *sl = lv_slider_create(parent);
    lv_obj_set_width(sl, 220);
    lv_obj_set_height(sl, 6);
    lv_slider_set_range(sl, minv, maxv);
    lv_slider_set_value(sl, val, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, Color(0x68696d), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sl, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, Color(0x1597f4), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_shadow_color(sl, lv_color_black(), LV_PART_KNOB);
    lv_obj_set_style_shadow_width(sl, 8, LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(sl, LV_OPA_30, LV_PART_KNOB);
    lv_obj_add_event_cb(sl, cb, LV_EVENT_VALUE_CHANGED, this);
    return sl;
}

lv_obj_t *SettingsView::MakeButton(lv_obj_t *parent, const char *text, uint32_t bg,
                                   lv_event_cb_t cb) {
    auto *b = lv_button_create(parent);
    lv_obj_set_height(b, 40);
    lv_obj_set_style_min_width(b, 96, 0);
    lv_obj_set_style_bg_color(b, Color(bg), 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, this);
    auto *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

lv_obj_t *SettingsView::DisplayCard() {
    const auto &p = jetson::UiTheme::Instance().Palette();
    auto *card = lv_obj_create(detail_);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_bg_color(card, Color(p.bg), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, Color(p.border), 0);
    lv_obj_set_style_border_opa(card, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

lv_obj_t *SettingsView::DisplayRow(lv_obj_t *card, const char *title,
                                   const char *sub, int height) {
    const auto &p = jetson::UiTheme::Instance().Palette();
    auto *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, height);
    lv_obj_set_style_pad_left(row, 14, 0);
    lv_obj_set_style_pad_right(row, 14, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    if (title && *title) {
        auto *left = lv_obj_create(row);
        lv_obj_remove_style_all(left);
        lv_obj_set_flex_grow(left, 1);
        lv_obj_set_height(left, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(left, 1, 0);
        lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);

        auto *label = lv_label_create(left);
        lv_obj_set_style_text_font(label, &BUILTIN_SMALL_TEXT_FONT, 0);
        lv_obj_set_style_text_color(label, Color(p.text), 0);
        lv_label_set_text(label, title);
        if (sub) {
            auto *caption = lv_label_create(left);
            lv_obj_set_style_text_font(caption, &BUILTIN_SMALL_TEXT_FONT, 0);
            lv_obj_set_style_text_color(caption, Color(p.sub_text), 0);
            lv_label_set_text(caption, sub);
        }
    }
    return row;
}

void SettingsView::DisplayDivider(lv_obj_t *card) {
    const auto &p = jetson::UiTheme::Instance().Palette();
    auto *line = lv_obj_create(card);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, lv_pct(100), 1);
    lv_obj_set_style_bg_color(line, Color(p.border), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_70, 0);
    lv_obj_clear_flag(line, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
}

void SettingsView::DisplayPageHeader(const char *title, bool show_back) {
    const auto &p = jetson::UiTheme::Instance().Palette();
    auto *header = lv_obj_create(detail_);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, lv_pct(100), 42);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    if (show_back) {
        auto *back = lv_obj_create(header);
        lv_obj_remove_style_all(back);
        lv_obj_set_size(back, 36, 36);
        lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(back, Color(p.button), 0);
        lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
        lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_add_event_cb(back, OnDisplayBack, LV_EVENT_CLICKED, this);
        auto *arrow = lv_label_create(back);
        lv_obj_set_style_text_font(arrow, &BUILTIN_ICON_FONT, 0);
        lv_obj_set_style_text_color(arrow, Color(p.text), 0);
        lv_label_set_text(arrow, LV_SYMBOL_LEFT);
        lv_obj_center(arrow);
    }

    auto *label = lv_label_create(header);
    lv_obj_set_style_text_font(label, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(label, Color(p.text), 0);
    lv_label_set_text(label, title);
    lv_obj_align(label, show_back ? LV_ALIGN_LEFT_MID : LV_ALIGN_CENTER, show_back ? 48 : 0, 0);
}

void SettingsView::DisplayCaption(const char *text) {
    const auto &p = jetson::UiTheme::Instance().Palette();
    auto *caption = lv_label_create(detail_);
    lv_obj_set_width(caption, lv_pct(100));
    lv_obj_set_style_text_font(caption, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(caption, Color(p.sub_text), 0);
    lv_label_set_long_mode(caption, LV_LABEL_LONG_WRAP);
    lv_label_set_text(caption, text);
}

void SettingsView::MakeDisplayNavigationRow(lv_obj_t *card, const char *title,
                                            const char *value, lv_event_cb_t cb) {
    const auto &p = jetson::UiTheme::Instance().Palette();
    auto *row = DisplayRow(card, title, nullptr);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, this);

    auto *right = lv_obj_create(row);
    lv_obj_remove_style_all(right);
    lv_obj_set_height(right, LV_SIZE_CONTENT);
    lv_obj_set_width(right, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right, 8, 0);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);
    if (value && *value) {
        auto *status = lv_label_create(right);
        lv_obj_set_style_text_font(status, &BUILTIN_SMALL_TEXT_FONT, 0);
        lv_obj_set_style_text_color(status, Color(p.sub_text), 0);
        lv_label_set_text(status, value);
    }
    auto *chevron = lv_label_create(right);
    lv_obj_set_style_text_font(chevron, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(chevron, Color(p.sub_text), 0);
    lv_label_set_text(chevron, LV_SYMBOL_RIGHT);
}

// =========================================================================
// Panes
// =========================================================================

void SettingsView::BuildDisplay() {
    switch (display_page_) {
        case DisplayPage::Main: BuildDisplayMain(); break;
        case DisplayPage::TextSize: BuildTextSizePage(); break;
        case DisplayPage::NightShift: BuildNightShiftPage(); break;
        case DisplayPage::AutoLock: BuildAutoLockPage(); break;
        case DisplayPage::AlwaysOn: BuildAlwaysOnPage(); break;
    }
}

void SettingsView::BuildDisplayMain() {
    const auto &p = jetson::UiTheme::Instance().Palette();
    DisplayPageHeader("Màn hình & Độ sáng", false);

    auto *text_card = DisplayCard();
    const int font_size = Settings("display", false).GetInt("font_size", 28);
    char font_value[24];
    if (font_size == 28) std::snprintf(font_value, sizeof(font_value), "Mặc định");
    else std::snprintf(font_value, sizeof(font_value), "%d%%", font_size * 100 / 28);
    MakeDisplayNavigationRow(text_card, "Cỡ chữ", font_value, OnOpenTextSize);
    DisplayDivider(text_card);
    auto *bold_row = DisplayRow(text_card, "Chữ đậm", nullptr);
    MakeSwitch(bold_row, Settings("display", false).GetBool("bold_text", false), OnBoldToggle);

    auto *brightness_title = lv_label_create(detail_);
    lv_obj_set_style_text_font(brightness_title, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(brightness_title, Color(p.sub_text), 0);
    lv_label_set_text(brightness_title, "ĐỘ SÁNG");

    int v = Settings("display", true).GetInt("brightness", 100);
    if (v < 20) v = 20;
    if (v > 100) v = 100;
    auto *brightness_card = DisplayCard();
    auto *slider_row = DisplayRow(brightness_card, "", nullptr, 58);
    auto *sun_small = lv_label_create(slider_row);
    lv_obj_set_style_text_font(sun_small, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(sun_small, Color(p.sub_text), 0);
    lv_label_set_text(sun_small, "☀");
    bright_slider_ = MakeSlider(slider_row, 20, 100, v, OnBrightChanged);
    lv_obj_set_flex_grow(bright_slider_, 1);
    lv_obj_set_width(bright_slider_, 1);
    char value[16];
    std::snprintf(value, sizeof(value), "%d%%", v);
    bright_value_label_ = lv_label_create(slider_row);
    lv_obj_set_style_text_font(bright_value_label_, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(bright_value_label_, Color(p.text), 0);
    lv_label_set_text(bright_value_label_, value);
    DisplayDivider(brightness_card);
    auto *tone_row = DisplayRow(brightness_card, "True Tone", nullptr);
    MakeSwitch(tone_row, Settings("display", false).GetBool("true_tone", false),
               OnTrueToneToggle);
    DisplayCaption("Tự động làm dịu tông màu hiển thị để nội dung dễ nhìn và nhất quán hơn.");

    auto *night_card = DisplayCard();
    MakeDisplayNavigationRow(night_card, "Night Shift",
                             Settings("display", false).GetBool("night_shift", false)
                                 ? "Bật" : "Tắt",
                             OnOpenNightShift);

    auto *lock_card = DisplayCard();
    const int sleep = Settings("display", false).GetInt("sleep_timeout", 0);
    MakeDisplayNavigationRow(lock_card, "Tự động khóa", SleepLabel(sleep), OnOpenAutoLock);
    DisplayDivider(lock_card);
    auto *wake_row = DisplayRow(lock_card, "Chạm để bật", nullptr);
    MakeSwitch(wake_row, Settings("display", false).GetBool("touch_to_wake", true),
               OnTouchWakeToggle);

    auto *always_card = DisplayCard();
    MakeDisplayNavigationRow(always_card, "Màn hình luôn bật",
                             Settings("display", false).GetBool("always_on", true)
                                 ? "Bật" : "Tắt",
                             OnOpenAlwaysOn);
    DisplayCaption("Khi bật, màn hình chờ vẫn hiển thị thông tin bằng độ sáng thấp.");
}

void SettingsView::BuildTextSizePage() {
    const auto &p = jetson::UiTheme::Instance().Palette();
    DisplayPageHeader("Cỡ chữ", true);
    DisplayCaption("Các màn hình trong hệ thống sẽ điều chỉnh theo kích cỡ đọc ưa thích của bạn.");

    auto *preview = DisplayCard();
    auto *preview_row = DisplayRow(preview, "Văn bản mẫu", "Jetson DS-02 • Dễ đọc, rõ ràng", 64);
    (void)preview_row;

    auto *size_card = DisplayCard();
    auto *size_row = DisplayRow(size_card, "", nullptr, 70);
    auto *small_a = lv_label_create(size_row);
    lv_obj_set_style_text_font(small_a, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(small_a, Color(p.sub_text), 0);
    lv_label_set_text(small_a, "A");
    const int size = Settings("display", false).GetInt("font_size", 28);
    text_size_slider_ = MakeSlider(size_row, 0, 6, FontStepForSize(size), OnTextSizeChanged);
    lv_obj_set_flex_grow(text_size_slider_, 1);
    lv_obj_set_width(text_size_slider_, 1);
    auto *large_a = lv_label_create(size_row);
    lv_obj_set_style_text_font(large_a, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(large_a, Color(p.text), 0);
    lv_label_set_text(large_a, "A");

    char value[20];
    std::snprintf(value, sizeof(value), "%d%%", size * 100 / 28);
    text_size_value_label_ = lv_label_create(detail_);
    lv_obj_set_width(text_size_value_label_, lv_pct(100));
    lv_obj_set_style_text_align(text_size_value_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(text_size_value_label_, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(text_size_value_label_, Color(p.sub_text), 0);
    lv_label_set_text(text_size_value_label_, value);
}

void SettingsView::BuildNightShiftPage() {
    const auto &p = jetson::UiTheme::Instance().Palette();
    DisplayPageHeader("Night Shift", true);
    DisplayCaption("Night Shift phủ một tông màu ấm lên màn hình để dịu mắt hơn trong môi trường tối.");

    auto *toggle_card = DisplayCard();
    auto *toggle_row = DisplayRow(toggle_card, "Bật thủ công", nullptr);
    MakeSwitch(toggle_row, Settings("display", false).GetBool("night_shift", false),
               OnNightShiftToggle);

    auto *warmth_title = lv_label_create(detail_);
    lv_obj_set_style_text_font(warmth_title, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(warmth_title, Color(p.sub_text), 0);
    lv_label_set_text(warmth_title, "NHIỆT ĐỘ MÀU");
    auto *warmth_card = DisplayCard();
    auto *warmth_row = DisplayRow(warmth_card, "", nullptr, 62);
    auto *less = lv_label_create(warmth_row);
    lv_obj_set_style_text_font(less, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(less, Color(p.sub_text), 0);
    lv_label_set_text(less, "Ít ấm");
    const int warmth = std::clamp(Settings("display", false).GetInt("night_warmth", 55), 0, 100);
    night_warmth_slider_ = MakeSlider(warmth_row, 0, 100, warmth, OnNightWarmthChanged);
    lv_obj_set_flex_grow(night_warmth_slider_, 1);
    lv_obj_set_width(night_warmth_slider_, 1);
    auto *more = lv_label_create(warmth_row);
    lv_obj_set_style_text_font(more, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(more, Color(0xffa33a), 0);
    lv_label_set_text(more, "Ấm hơn");
}

void SettingsView::BuildAutoLockPage() {
    const auto &p = jetson::UiTheme::Instance().Palette();
    DisplayPageHeader("Tự động khóa", true);
    const int current = Settings("display", false).GetInt("sleep_timeout", 0);
    auto *card = DisplayCard();
    for (size_t i = 0; i < sizeof(kSleepOpts) / sizeof(kSleepOpts[0]); ++i) {
        const auto &option = kSleepOpts[i];
        auto *row = DisplayRow(card, option.label, nullptr, 44);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        auto *ctx = new OptCtx{this, std::to_string(option.seconds)};
        lv_obj_add_event_cb(row, OnSleepSelected, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(row, OnOptDeleted, LV_EVENT_DELETE, ctx);
        if (current == option.seconds) {
            auto *check = lv_label_create(row);
            lv_obj_set_style_text_font(check, &BUILTIN_ICON_FONT, 0);
            lv_obj_set_style_text_color(check, Color(p.accent), 0);
            lv_label_set_text(check, LV_SYMBOL_OK);
        }
        if (i + 1 < sizeof(kSleepOpts) / sizeof(kSleepOpts[0])) DisplayDivider(card);
    }
}

void SettingsView::BuildAlwaysOnPage() {
    DisplayPageHeader("Màn hình luôn bật", true);
    DisplayCaption("Màn hình chờ giảm độ sáng để hiển thị thời gian và thông tin với mức tiêu thụ điện thấp.");
    DisplayCaption("Màn hình sẽ tự chuyển sang trạng thái chờ sau khoảng thời gian Tự động khóa.");

    auto *options = DisplayCard();
    auto *wallpaper = DisplayRow(options, "Hiển thị hình nền", nullptr);
    MakeSwitch(wallpaper, Settings("display", false).GetBool("aod_wallpaper", true),
               OnAlwaysOnWallpaperToggle);
    DisplayDivider(options);
    auto *blur = DisplayRow(options, "Làm mờ hình nền", nullptr);
    MakeSwitch(blur, Settings("display", false).GetBool("aod_blur", true),
               OnAlwaysOnBlurToggle);
    DisplayDivider(options);
    auto *notifications = DisplayRow(options, "Hiển thị thông báo", nullptr);
    MakeSwitch(notifications,
               Settings("display", false).GetBool("aod_notifications", true),
               OnAlwaysOnNotificationsToggle);

    auto *master = DisplayCard();
    auto *master_row = DisplayRow(master, "Màn hình luôn bật", nullptr);
    MakeSwitch(master_row, Settings("display", false).GetBool("always_on", true),
               OnAlwaysOnToggle);
    DisplayCaption("Khi tắt, màn hình sẽ chuyển sang màu đen sau khi tự động khóa.");
}

void SettingsView::BuildSound() {
    SectionTitle("Âm thanh");
    int vol = Settings("display", true).GetInt("volume", 50);
    bool muted = Settings("display", true).GetBool("muted", false);
    char sub[32];
    std::snprintf(sub, sizeof(sub), "%d%%%s", vol, muted ? " (tắt tiếng)" : "");
    auto *row = MakeRow("Âm lượng", sub);
    auto *sl = lv_slider_create(row);
    lv_obj_set_width(sl, 200);
    lv_slider_set_range(sl, 0, 100);
    lv_slider_set_value(sl, vol, LV_ANIM_OFF);
    lv_obj_add_event_cb(sl, OnVolChanged, LV_EVENT_VALUE_CHANGED, this);
    vol_slider_ = sl;
    mute_switch_ = MakeSwitch(row, !muted, OnMuteToggle); // on = audible
}

void SettingsView::BuildWifi() {
    SectionTitle("WiFi");
    // Toggling the switch on enables the radio and automatically rescans, so a
    // separate "Quét lại" button is unnecessary.
    auto *top = MakeRow("Bật WiFi", nullptr);
    wifi_switch_ = MakeSwitch(top, false, OnWifiSwitch);
    WifiRefreshSwitch();

    // Network list.
    wifi_list_ = lv_obj_create(detail_);
    lv_obj_remove_style_all(wifi_list_);
    lv_obj_set_width(wifi_list_, lv_pct(100));
    lv_obj_set_flex_grow(wifi_list_, 1);
    lv_obj_set_style_bg_opa(wifi_list_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(wifi_list_, 0, 0);
    lv_obj_set_style_pad_row(wifi_list_, 6, 0);
    lv_obj_set_flex_flow(wifi_list_, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(wifi_list_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(wifi_list_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(wifi_list_, LV_SCROLLBAR_MODE_ACTIVE);

    if (!wifi_scanned_) WifiRescan();
    else WifiRenderList();
}

void SettingsView::BuildBluetooth() {
    SectionTitle("Bluetooth");
    auto *top = MakeRow("Bật Bluetooth", nullptr);
    bt_switch_ = MakeSwitch(top, false, OnBtSwitch);
    BtRefreshSwitch();

    auto *btns = lv_obj_create(detail_);
    lv_obj_remove_style_all(btns);
    lv_obj_set_width(btns, lv_pct(100));
    lv_obj_set_height(btns, 44);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btns, 8, 0);
    lv_obj_clear_flag(btns, LV_OBJ_FLAG_SCROLLABLE);
    MakeButton(btns, "Quét lại", 0x2b6fd6, OnBtRescan);

    bt_list_ = lv_obj_create(detail_);
    lv_obj_remove_style_all(bt_list_);
    lv_obj_set_width(bt_list_, lv_pct(100));
    lv_obj_set_flex_grow(bt_list_, 1);
    lv_obj_set_style_bg_opa(bt_list_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(bt_list_, 0, 0);
    lv_obj_set_style_pad_row(bt_list_, 6, 0);
    lv_obj_set_flex_flow(bt_list_, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(bt_list_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(bt_list_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(bt_list_, LV_SCROLLBAR_MODE_ACTIVE);

    if (!bt_scanned_) BtRescan();
    else BtRenderList();
}

void SettingsView::BuildKeyboard() {
    SectionTitle("Bàn phím");
    std::string lang = Settings("input", true).GetString("kbd_lang", "en");
    auto *row = MakeRow("Ngôn ngữ gõ", lang == "vi" ? "Tiếng Việt (Telex)" : "English");
    auto *btns = lv_obj_create(row);
    lv_obj_remove_style_all(btns);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btns, 6, 0);
    lv_obj_clear_flag(btns, LV_OBJ_FLAG_SCROLLABLE);
    lang_vi_btn_ = MakeButton(btns, "VI", lang == "vi" ? 0x2b6fd6 : 0x3a3a3a, OnLangVi);
    lang_en_btn_ = MakeButton(btns, "EN", lang == "en" ? 0x2b6fd6 : 0x3a3a3a, OnLangEn);

    auto *demoRow = MakeRow("Thử gõ tiếng Việt", nullptr);
    (void)demoRow;
    kbd_demo_ = new TelexInput(detail_, lv_pct(100), 44);
    kbd_demo_->SetTelex(lang == "vi");
    kbd_demo_->SetPlaceholder("as -> á, aw -> ă, dd -> đ ...");
}

void SettingsView::BuildDateTime() {
    SectionTitle("Ngày giờ");

    bool h24 = Settings("display", true).GetBool("clock_24h", true);
    auto *row = MakeRow("Định dạng 24h", h24 ? "24 giờ" : "12 giờ");
    MakeSwitch(row, h24, On24hToggle);

    SectionTitle("Múi giờ");
    std::string cur = CurrentTimezone();
    for (const char *tz : kTimezones) {
        const auto &p = jetson::UiTheme::Instance().Palette();
        auto *opt = lv_obj_create(detail_);
        lv_obj_remove_style_all(opt);
        lv_obj_set_width(opt, lv_pct(100));
        lv_obj_set_height(opt, 40);
        lv_obj_set_style_radius(opt, 10, 0);
        bool sel = (cur == tz);
        lv_obj_set_style_bg_color(opt, sel ? Color(p.accent) : Color(p.bg), 0);
        lv_obj_set_style_bg_opa(opt, sel ? LV_OPA_30 : LV_OPA_COVER, 0);
        lv_obj_set_style_pad_left(opt, 12, 0);
        lv_obj_clear_flag(opt, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(opt, LV_OBJ_FLAG_CLICKABLE);
        auto *l = lv_label_create(opt);
        lv_obj_set_style_text_font(l, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(l, Color(p.text), 0);
        lv_label_set_text(l, tz);
        auto *ctx = new OptCtx{this, tz};
        lv_obj_add_event_cb(opt, OnTzSelected, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(opt, OnOptDeleted, LV_EVENT_DELETE, ctx);
    }
}

void SettingsView::BuildPower() {
    SectionTitle("Nguồn & Khóa");

    // Lock + PIN.
    MakeButton(MakeRow("Khóa màn hình ngay", nullptr), "Khóa", 0x2b6fd6, OnLockNow);
    MakeButton(MakeRow("Đặt / xóa PIN khóa", nullptr), "Đặt PIN", 0x3a3a3a, OnSetPin);

    // Power actions.
    MakeButton(MakeRow("Khởi động lại thiết bị", nullptr), "Khởi động lại", 0xb03a3a, OnReboot);
    MakeButton(MakeRow("Tắt thiết bị", nullptr), "Tắt máy", 0xb03a3a, OnShutdown);
}

void SettingsView::BuildAbout() {
    SectionTitle("Giới thiệu");
    const auto &p = jetson::UiTheme::Instance().Palette();

    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "Thiết bị: %s\nMàn hình: %dx%d\nFirmware: jetson-fw 0.1",
                  BOARD_NAME, width_, height_);
    auto *a = lv_label_create(detail_);
    lv_obj_set_style_text_font(a, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(a, Color(p.text), 0);
    lv_obj_set_width(a, lv_pct(100));
    lv_label_set_long_mode(a, LV_LABEL_LONG_WRAP);
    lv_label_set_text(a, buf);

    // Storage (statvfs on "/").
    struct statvfs st;
    std::string storage = "Lưu trữ: —";
    if (statvfs("/", &st) == 0) {
        unsigned long long total = (unsigned long long)st.f_blocks * st.f_frsize;
        unsigned long long freeb = (unsigned long long)st.f_bavail * st.f_frsize;
        unsigned long long used = (total > freeb) ? (total - freeb) : 0;
        storage = "Lưu trữ: " + FormatBytes(used) + " / " + FormatBytes(total) +
                 " (" + FormatBytes(freeb) + " trống)";
    }
    auto *s = lv_label_create(detail_);
    lv_obj_set_style_text_font(s, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(s, Color(p.sub_text), 0);
    lv_obj_set_width(s, lv_pct(100));
    lv_label_set_long_mode(s, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s, storage.c_str());

    // Memory. sysinfo()'s freeram excludes buffer/cache, so on Linux it reports a
    // tiny "free" number that looks wrong (e.g. 150 MB on a 4 GB board with ~3 GB
    // actually usable). /proc/meminfo's MemAvailable is the real figure the kernel
    // considers reclaimable, so read that instead. Values are in kB.
    std::string mem = "RAM: —";
    unsigned long long mem_total_kb = 0, mem_avail_kb = 0;
    {
        std::ifstream mf("/proc/meminfo");
        std::string line;
        while (std::getline(mf, line)) {
            if (line.rfind("MemTotal:", 0) == 0)
                mem_total_kb = std::strtoull(line.c_str() + 9, nullptr, 10);
            else if (line.rfind("MemAvailable:", 0) == 0)
                mem_avail_kb = std::strtoull(line.c_str() + 13, nullptr, 10);
            if (mem_total_kb && mem_avail_kb) break;
        }
    }
    if (mem_total_kb > 0) {
        unsigned long long total = mem_total_kb * 1024ULL;
        unsigned long long avail = mem_avail_kb * 1024ULL;
        unsigned long long used = (total > avail) ? (total - avail) : 0;
        mem = "RAM: " + FormatBytes(used) + " / " + FormatBytes(total) +
              " (" + FormatBytes(avail) + " trống)";
    }
    auto *m = lv_label_create(detail_);
    lv_obj_set_style_text_font(m, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(m, Color(p.sub_text), 0);
    lv_obj_set_width(m, lv_pct(100));
    lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
    lv_label_set_text(m, mem.c_str());
}

// =========================================================================
// WiFi
// =========================================================================

void SettingsView::WifiRefreshSwitch() {
    if (!wifi_switch_) return;
    if (wifi_enabled_) lv_obj_add_state(wifi_switch_, LV_STATE_CHECKED);
    else lv_obj_clear_state(wifi_switch_, LV_STATE_CHECKED);
}

void SettingsView::WifiRescan() {
    if (!wifi_list_) return;
    if (wifi_busy_.exchange(true)) return;
    SetStatus("Đang quét WiFi...");
    ESP_LOGI(TAG, "WiFi scan requested from Settings");
    std::thread([self = Self()]() {
        const bool enabled = self->wifi_.IsEnabled();
        std::vector<jetson::WifiNetwork> nets;
        std::string active;
        if (enabled) {
            nets = self->wifi_.Scan();
            active = self->wifi_.ActiveSsid();
        }
        const std::string error = self->wifi_.LastError();
        LvLockGuard lock;
        self->wifi_busy_ = false;
        self->wifi_enabled_ = enabled;
        self->wifi_nets_ = std::move(nets);
        self->wifi_scanned_ = enabled;
        self->WifiRefreshSwitch();
        if (self->wifi_list_) self->WifiRenderList();
        if (!enabled) {
            self->SetStatus("WiFi đang tắt");
        } else if (self->wifi_nets_.empty() && !error.empty()) {
            self->SetStatus(("Lỗi quét WiFi: " + error).c_str());
            ESP_LOGE(TAG, "WiFi scan failed: %s", error.c_str());
        } else {
            self->SetStatus(active.empty() ? "Chạm mạng để kết nối"
                                           : ("Đã kết nối: " + active).c_str());
            ESP_LOGI(TAG, "WiFi scan rendered %zu networks", self->wifi_nets_.size());
        }
    }).detach();
}

void SettingsView::WifiRenderList() {
    if (!wifi_list_) return;
    lv_obj_clean(wifi_list_);
    const auto &p = jetson::UiTheme::Instance().Palette();
    if (wifi_nets_.empty()) {
        auto *e = lv_label_create(wifi_list_);
        lv_obj_set_style_text_font(e, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(e, Color(p.sub_text), 0);
        lv_label_set_text(e, "Không có mạng. Bật/tắt WiFi để quét lại.");
        return;
    }
    for (const auto &n : wifi_nets_) WifiCreateRow(n);
}

void SettingsView::WifiCreateRow(const jetson::WifiNetwork &n) {
    const auto &p = jetson::UiTheme::Instance().Palette();
    auto *row = lv_obj_create(wifi_list_);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 52);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_bg_color(row, n.in_use ? Color(0x1e3a5f) : Color(p.bg), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(row, 12, 0);
    lv_obj_set_style_pad_right(row, 12, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    auto *left = lv_obj_create(row);
    lv_obj_remove_style_all(left);
    lv_obj_set_height(left, 30);
    lv_obj_set_flex_grow(left, 1);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left, 8, 0);

    auto *check = lv_label_create(left);
    lv_obj_set_width(check, 22);
    lv_obj_set_style_text_font(check, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(check, Color(p.accent), 0);
    lv_label_set_text(check, n.in_use ? LV_SYMBOL_OK : "");

    auto *ssid = lv_label_create(left);
    lv_obj_set_style_text_font(ssid, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(ssid, Color(p.text), 0);
    lv_label_set_text(ssid, n.ssid.c_str());
    lv_label_set_long_mode(ssid, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(ssid, 1);

    auto *right = lv_obj_create(row);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, 132, 32);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(right, 8, 0);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    if (n.secured) {
        auto *lock = lv_obj_create(right);
        lv_obj_remove_style_all(lock);
        lv_obj_set_size(lock, 16, 20);
        lv_obj_clear_flag(lock, LV_OBJ_FLAG_SCROLLABLE);
        auto *shackle = lv_obj_create(lock);
        lv_obj_remove_style_all(shackle);
        lv_obj_set_size(shackle, 10, 10);
        lv_obj_set_style_radius(shackle, 6, 0);
        lv_obj_set_style_border_width(shackle, 2, 0);
        lv_obj_set_style_border_color(shackle, Color(p.sub_text), 0);
        lv_obj_set_style_bg_opa(shackle, LV_OPA_TRANSP, 0);
        lv_obj_align(shackle, LV_ALIGN_TOP_MID, 0, 0);
        auto *lockBody = lv_obj_create(lock);
        lv_obj_remove_style_all(lockBody);
        lv_obj_set_size(lockBody, 14, 11);
        lv_obj_set_style_radius(lockBody, 2, 0);
        lv_obj_set_style_bg_color(lockBody, Color(p.sub_text), 0);
        lv_obj_set_style_bg_opa(lockBody, LV_OPA_COVER, 0);
        lv_obj_align(lockBody, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
    jetson::ui::CreateSignalBars(right, n.signal);

    auto *ctx = new WifiRowCtx{this, n};

    // The information affordance is separate from the row action, matching
    // iOS: tapping the row connects, tapping the circled i opens properties.
    auto *info = lv_obj_create(right);
    lv_obj_remove_style_all(info);
    lv_obj_set_size(info, 30, 30);
    lv_obj_set_style_radius(info, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(info, 2, 0);
    lv_obj_set_style_border_color(info, Color(p.accent), 0);
    lv_obj_set_style_bg_opa(info, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(info, LV_OBJ_FLAG_CLICKABLE);
    auto *infoLabel = lv_label_create(info);
    lv_obj_set_style_text_font(infoLabel, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(infoLabel, Color(p.accent), 0);
    lv_label_set_text(infoLabel, "i");
    lv_obj_center(infoLabel);
    lv_obj_add_event_cb(info, OnWifiInfoClicked, LV_EVENT_CLICKED, ctx);

    lv_obj_add_event_cb(row, OnWifiRowClicked, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(row, OnWifiRowDeleted, LV_EVENT_DELETE, ctx);
}

void SettingsView::WifiOpenConnectSheet(const jetson::WifiNetwork &network) {
    CloseModal();
    modal_ssid_ = network.ssid;
    modal_wifi_ = network;
    const auto &p = jetson::UiTheme::Instance().Palette();

    popup_ = lv_obj_create(overlay_);
    lv_obj_remove_style_all(popup_);
    lv_obj_set_size(popup_, width_, height_);
    lv_obj_set_style_bg_color(popup_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(popup_, LV_OPA_50, 0);
    lv_obj_add_flag(popup_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(popup_, OnPopupDismiss, LV_EVENT_CLICKED, this);

    const int cardW = std::min(width_ - 32, 620);
    constexpr int sheetH = 286;
    popup_card_ = lv_obj_create(popup_);
    lv_obj_remove_style_all(popup_card_);
    lv_obj_set_size(popup_card_, cardW, sheetH);
    lv_obj_set_style_bg_color(popup_card_, Color(p.row), 0);
    lv_obj_set_style_bg_opa(popup_card_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(popup_card_, 22, 0);
    lv_obj_set_style_clip_corner(popup_card_, true, 0);
    lv_obj_set_style_pad_all(popup_card_, 16, 0);
    lv_obj_set_style_pad_row(popup_card_, 8, 0);
    lv_obj_set_flex_flow(popup_card_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(popup_card_, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(popup_card_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(popup_card_, LV_OBJ_FLAG_CLICKABLE);

    auto *header = lv_obj_create(popup_card_);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, lv_pct(100), 40);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto makeCircleAction = [&](const char *glyph, uint32_t bg, uint32_t fg,
                                lv_event_cb_t cb) {
        auto *button = lv_obj_create(header);
        lv_obj_remove_style_all(button);
        lv_obj_set_size(button, 40, 40);
        lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(button, Color(bg), 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
        auto *label = lv_label_create(button);
        lv_obj_set_style_text_font(label, &BUILTIN_ICON_FONT, 0);
        lv_obj_set_style_text_color(label, Color(fg), 0);
        lv_label_set_text(label, glyph);
        lv_obj_center(label);
        lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, this);
        return button;
    };
    makeCircleAction(LV_SYMBOL_CLOSE, p.button, p.text, OnModalClose);
    popup_confirm_btn_ = makeCircleAction(LV_SYMBOL_OK, p.button, 0xffffff,
                                          OnModalConnect);
    lv_obj_set_style_bg_opa(popup_confirm_btn_, LV_OPA_60, 0);
    lv_obj_add_state(popup_confirm_btn_, LV_STATE_DISABLED);

    auto *wifiIcon = lv_label_create(popup_card_);
    lv_obj_set_style_text_font(wifiIcon, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(wifiIcon, Color(p.accent), 0);
    lv_label_set_text(wifiIcon, LV_SYMBOL_WIFI);

    auto *title = lv_label_create(popup_card_);
    lv_obj_set_width(title, lv_pct(100));
    lv_obj_set_style_text_font(title, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(title, Color(p.text), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    const std::string titleText = "Kết nối “" + network.ssid + "”";
    lv_label_set_text(title, titleText.c_str());

    auto *subtitle = lv_label_create(popup_card_);
    lv_obj_set_width(subtitle, lv_pct(100));
    lv_obj_set_style_text_font(subtitle, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(subtitle, Color(p.sub_text), 0);
    lv_obj_set_style_text_align(subtitle, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_WRAP);
    lv_label_set_text(subtitle, "Nhập mật khẩu để kết nối vào mạng Wi-Fi này.");

    popup_input_ = new TelexInput(popup_card_, cardW - 32, 50);
    popup_input_->SetPassword(true);
    popup_input_->SetMaxLen(63);
    popup_input_->SetPlaceholder("Mật khẩu");
    popup_input_->SetFont(&BUILTIN_SMALL_TEXT_FONT);
    lv_obj_add_event_cb(popup_input_->obj(), OnModalConnect, LV_EVENT_READY, this);
    lv_obj_add_event_cb(popup_input_->obj(), OnModalPasswordChanged,
                        LV_EVENT_VALUE_CHANGED, this);
    popup_input_->Focus();

    // iOS-style bottom sheet: start below the viewport and ease into place.
    lv_anim_t slide;
    lv_anim_init(&slide);
    lv_anim_set_var(&slide, popup_card_);
    lv_anim_set_values(&slide, sheetH + 16, 0);
    lv_anim_set_time(&slide, 280);
    lv_anim_set_exec_cb(&slide, SetWifiSheetTranslateY);
    lv_anim_set_path_cb(&slide, lv_anim_path_ease_out);
    lv_anim_start(&slide);
}

void SettingsView::WifiLoadDetails(const jetson::WifiNetwork &network) {
    modal_wifi_ = network;
    modal_ssid_ = network.ssid;
    const jetson::WifiNetwork fallback = network;
    const std::string ssid = fallback.ssid;
    CloseModal();
    SetStatus(("Đang đọc thông tin " + ssid + "...").c_str());
    std::thread([self = Self(), fallback, ssid]() {
        auto details = self->wifi_.Details(ssid);
        if (details.ssid.empty()) details.ssid = ssid;
        if (details.signal == 0) details.signal = fallback.signal;
        details.connected = details.connected || fallback.in_use;
        details.known = details.known || fallback.known || fallback.in_use;
        if (details.security.empty()) {
            details.security = !fallback.security.empty() ? fallback.security
                              : (fallback.secured ? "WPA/WEP" : "Mở");
        }
        if (details.bssid.empty()) details.bssid = fallback.bssid;

        LvLockGuard lock;
        if (self->current_ == Cat::Wifi && self->overlay_ && self->modal_ssid_ == ssid)
            self->WifiOpenDetails(details);
    }).detach();
}

void SettingsView::WifiOpenDetails(const jetson::WifiDetails &details) {
    CloseModal();
    modal_ssid_ = details.ssid;
    const auto &p = jetson::UiTheme::Instance().Palette();

    popup_ = lv_obj_create(overlay_);
    lv_obj_remove_style_all(popup_);
    lv_obj_set_size(popup_, width_, height_);
    lv_obj_set_style_bg_color(popup_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(popup_, LV_OPA_50, 0);
    lv_obj_add_flag(popup_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(popup_, OnPopupDismiss, LV_EVENT_CLICKED, this);

    const int cardW = std::min(width_ - 32, 650);
    const int cardH = height_ - 24;
    popup_card_ = lv_obj_create(popup_);
    lv_obj_remove_style_all(popup_card_);
    lv_obj_set_size(popup_card_, cardW, cardH);
    lv_obj_set_style_bg_color(popup_card_, Color(p.row), 0);
    lv_obj_set_style_bg_opa(popup_card_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(popup_card_, 22, 0);
    lv_obj_set_style_clip_corner(popup_card_, true, 0);
    lv_obj_set_style_pad_all(popup_card_, 12, 0);
    lv_obj_set_style_pad_row(popup_card_, 8, 0);
    lv_obj_set_flex_flow(popup_card_, LV_FLEX_FLOW_COLUMN);
    lv_obj_align(popup_card_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(popup_card_, LV_OBJ_FLAG_CLICKABLE);

    auto *header = lv_obj_create(popup_card_);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, lv_pct(100), 42);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto *back = lv_obj_create(header);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, 40, 40);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(back, Color(p.button), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    auto *backLabel = lv_label_create(back);
    lv_obj_set_style_text_font(backLabel, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(backLabel, Color(p.text), 0);
    lv_label_set_text(backLabel, LV_SYMBOL_LEFT);
    lv_obj_center(backLabel);
    lv_obj_add_event_cb(back, OnModalClose, LV_EVENT_CLICKED, this);

    auto *title = lv_label_create(header);
    lv_obj_set_flex_grow(title, 1);
    lv_obj_set_style_text_font(title, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(title, Color(p.text), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_label_set_text(title, details.ssid.c_str());

    auto *headerSpacer = lv_obj_create(header);
    lv_obj_remove_style_all(headerSpacer);
    lv_obj_set_size(headerSpacer, 40, 40);
    lv_obj_clear_flag(headerSpacer, LV_OBJ_FLAG_SCROLLABLE);

    auto *rows = lv_obj_create(popup_card_);
    lv_obj_remove_style_all(rows);
    lv_obj_set_width(rows, lv_pct(100));
    lv_obj_set_flex_grow(rows, 1);
    lv_obj_set_style_bg_opa(rows, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(rows, 4, 0);
    lv_obj_set_style_pad_row(rows, 10, 0);
    lv_obj_set_flex_flow(rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(rows, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(rows, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(rows, LV_SCROLLBAR_MODE_ACTIVE);

    using DetailRows = std::vector<std::pair<std::string, std::string>>;
    auto makeGroup = [&](const DetailRows &items) {
        if (items.empty()) return;
        auto *group = lv_obj_create(rows);
        lv_obj_remove_style_all(group);
        lv_obj_set_size(group, lv_pct(100), static_cast<int>(items.size()) * 42);
        lv_obj_set_style_bg_color(group, Color(p.bg), 0);
        lv_obj_set_style_bg_opa(group, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(group, 14, 0);
        lv_obj_set_style_clip_corner(group, true, 0);
        lv_obj_set_style_pad_all(group, 0, 0);
        lv_obj_set_flex_flow(group, LV_FLEX_FLOW_COLUMN);
        lv_obj_clear_flag(group, LV_OBJ_FLAG_SCROLLABLE);

        for (size_t i = 0; i < items.size(); ++i) {
            auto *row = lv_obj_create(group);
            lv_obj_remove_style_all(row);
            lv_obj_set_size(row, lv_pct(100), 42);
            lv_obj_set_style_pad_left(row, 12, 0);
            lv_obj_set_style_pad_right(row, 12, 0);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                  LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            if (i + 1 < items.size()) {
                lv_obj_set_style_border_width(row, 1, 0);
                lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
                lv_obj_set_style_border_color(row, Color(p.border), 0);
            }

            auto *key = lv_label_create(row);
            lv_obj_set_width(key, 170);
            lv_obj_set_style_text_font(key, &BUILTIN_SMALL_TEXT_FONT, 0);
            lv_obj_set_style_text_color(key, Color(p.text), 0);
            lv_label_set_long_mode(key, LV_LABEL_LONG_DOT);
            lv_label_set_text(key, items[i].first.c_str());

            auto *value = lv_label_create(row);
            lv_obj_set_flex_grow(value, 1);
            lv_obj_set_style_text_font(value, &BUILTIN_SMALL_TEXT_FONT, 0);
            lv_obj_set_style_text_color(value, Color(p.sub_text), 0);
            lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
            lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
            lv_label_set_text(value, items[i].second.c_str());
        }
    };

    if (details.known || details.connected) {
        auto *forget = lv_obj_create(rows);
        lv_obj_remove_style_all(forget);
        lv_obj_set_size(forget, lv_pct(100), 44);
        lv_obj_set_style_bg_color(forget, Color(p.bg), 0);
        lv_obj_set_style_bg_opa(forget, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(forget, 14, 0);
        lv_obj_set_style_pad_left(forget, 12, 0);
        lv_obj_add_flag(forget, LV_OBJ_FLAG_CLICKABLE);
        auto *forgetLabel = lv_label_create(forget);
        lv_obj_set_style_text_font(forgetLabel, &BUILTIN_SMALL_TEXT_FONT, 0);
        lv_obj_set_style_text_color(forgetLabel, Color(p.accent), 0);
        lv_label_set_text(forgetLabel, "Quên mạng này");
        lv_obj_align(forgetLabel, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_add_event_cb(forget, OnModalForget, LV_EVENT_CLICKED, this);
    }

    DetailRows credentials;
    credentials.emplace_back("Bảo mật",
                             details.security.empty() ? "Mở" : details.security);
    if (details.known)
        credentials.emplace_back("Mật khẩu",
                                 details.password.empty() ? "Không đọc được" : details.password);
    makeGroup(credentials);

    const auto shown = [](const std::string &value) {
        return value.empty() ? std::string("—") : value;
    };
    std::string ipv4 = details.ip_address;
    const auto slash = ipv4.find('/');
    if (slash != std::string::npos) ipv4.erase(slash);

    makeGroup({
        {"Địa chỉ Wi-Fi", shown(details.adapter_address)},
        {"Điểm truy cập", shown(details.bssid)},
    });
    makeGroup({
        {"Địa chỉ IPv4", shown(ipv4)},
        {"Gateway", shown(details.gateway)},
        {"DNS", shown(details.dns)},
    });
    makeGroup({
        {"Trạng thái", details.connected ? "Đã kết nối"
                       : (details.known ? "Đã lưu" : "Khả dụng")},
        {"Tín hiệu", std::to_string(details.signal) + "%"},
        {"Kênh", shown(details.channel)},
        {"Tần số", shown(details.frequency)},
        {"Tốc độ", shown(details.rate)},
    });

    lv_anim_t slide;
    lv_anim_init(&slide);
    lv_anim_set_var(&slide, popup_card_);
    lv_anim_set_values(&slide, cardH + 16, 0);
    lv_anim_set_time(&slide, 240);
    lv_anim_set_exec_cb(&slide, SetWifiSheetTranslateY);
    lv_anim_set_path_cb(&slide, lv_anim_path_ease_out);
    lv_anim_start(&slide);
}

void SettingsView::WifiDoConnect(const std::string &ssid, const std::string &pw) {
    if (wifi_busy_.exchange(true)) return;
    SetStatus(("Đang kết nối " + ssid + "...").c_str());
    std::thread([self = Self(), ssid, pw]() {
        bool ok = self->wifi_.Connect(ssid, pw);
        std::vector<jetson::WifiNetwork> nets;
        std::string active;
        if (ok) {
            nets = self->wifi_.Scan();
            active = self->wifi_.ActiveSsid();
        }
        const std::string error = self->wifi_.LastError();
        LvLockGuard lock;
        self->wifi_busy_ = false;
        if (ok) {
            self->wifi_enabled_ = true;
            self->wifi_nets_ = std::move(nets);
            self->wifi_scanned_ = true;
            if (self->wifi_list_) self->WifiRenderList();
            self->SetStatus(("Đã kết nối: " + (active.empty() ? ssid : active)).c_str());
            ESP_LOGI(TAG, "WiFi connected: %s", ssid.c_str());
        } else {
            self->SetStatus(("Lỗi: " + error).c_str());
            ESP_LOGE(TAG, "WiFi connection failed for %s: %s", ssid.c_str(), error.c_str());
        }
    }).detach();
}

void SettingsView::WifiDoForget(const std::string &ssid) {
    if (wifi_busy_.exchange(true)) return;
    std::thread([self = Self(), ssid]() {
        bool ok = self->wifi_.Forget(ssid);
        std::vector<jetson::WifiNetwork> nets;
        std::string active;
        if (ok && self->wifi_.IsEnabled()) {
            nets = self->wifi_.Scan();
            active = self->wifi_.ActiveSsid();
        }
        const std::string error = self->wifi_.LastError();
        LvLockGuard lock;
        self->wifi_busy_ = false;
        if (ok) {
            self->wifi_nets_ = std::move(nets);
            if (self->wifi_list_) self->WifiRenderList();
            self->SetStatus(active.empty() ? ("Đã quên: " + ssid).c_str()
                                           : ("Đã kết nối: " + active).c_str());
        } else {
            self->SetStatus(("Lỗi: " + error).c_str());
            ESP_LOGE(TAG, "forget WiFi failed for %s: %s", ssid.c_str(), error.c_str());
        }
    }).detach();
}

// =========================================================================
// Bluetooth
// =========================================================================

void SettingsView::BtRefreshSwitch() {
    if (!bt_switch_) return;
    if (bt_powered_) lv_obj_add_state(bt_switch_, LV_STATE_CHECKED);
    else lv_obj_clear_state(bt_switch_, LV_STATE_CHECKED);
}

void SettingsView::BtRescan() {
    if (!bt_list_) return;
    if (bt_busy_.exchange(true)) return;
    SetStatus("Đang quét Bluetooth...");
    ESP_LOGI(TAG, "Bluetooth scan requested from Settings");
    std::thread([self = Self()]() {
        const bool powered = self->bluetooth_.IsPowered();
        std::vector<jetson::BtDevice> devs;
        if (powered) devs = self->bluetooth_.Scan(8);
        const std::string error = self->bluetooth_.LastError();
        LvLockGuard lock;
        self->bt_busy_ = false;
        self->bt_powered_ = powered;
        self->bt_devs_ = std::move(devs);
        self->bt_scanned_ = powered;
        self->BtRefreshSwitch();
        if (self->bt_list_) self->BtRenderList();
        if (!powered) {
            self->SetStatus("Bluetooth đang tắt hoặc không có adapter");
        } else if (self->bt_devs_.empty() && !error.empty()) {
            self->SetStatus(("Lỗi quét Bluetooth: " + error).c_str());
            ESP_LOGE(TAG, "Bluetooth scan failed: %s", error.c_str());
        } else {
            self->SetStatus("Chạm thiết bị để kết nối/ngắt/quên");
            ESP_LOGI(TAG, "Bluetooth scan rendered %zu devices", self->bt_devs_.size());
        }
    }).detach();
}

void SettingsView::BtRenderList() {
    if (!bt_list_) return;
    lv_obj_clean(bt_list_);
    const auto &p = jetson::UiTheme::Instance().Palette();
    if (bt_devs_.empty()) {
        auto *e = lv_label_create(bt_list_);
        lv_obj_set_style_text_font(e, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(e, Color(p.sub_text), 0);
        lv_label_set_text(e, "Không có thiết bị. Bấm \"Quét lại\".");
        return;
    }
    for (const auto &d : bt_devs_) BtCreateRow(d);
}

void SettingsView::BtCreateRow(const jetson::BtDevice &d) {
    const auto &p = jetson::UiTheme::Instance().Palette();
    auto *row = lv_obj_create(bt_list_);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 58);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_bg_color(row, d.connected ? Color(0x1e3a5f) : Color(p.bg), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(row, 12, 0);
    lv_obj_set_style_pad_right(row, 12, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    auto *left = lv_obj_create(row);
    lv_obj_remove_style_all(left);
    lv_obj_set_flex_grow(left, 1);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 2, 0);
    auto *name = lv_label_create(left);
    lv_obj_set_style_text_font(name, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(name, Color(p.text), 0);
    lv_label_set_text(name, d.name.c_str());
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    auto *addr = lv_label_create(left);
    lv_obj_set_style_text_font(addr, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(addr, Color(p.sub_text), 0);
    lv_label_set_text(addr, d.address.c_str());

    auto *right = lv_obj_create(row);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, 110, 22);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(right, 6, 0);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    int lvl = d.rssi + 100; // dBm (-100..0) -> 0..100
    if (lvl < 0) lvl = 0;
    if (lvl > 100) lvl = 100;
    jetson::ui::CreateSignalBars(right, lvl);
    auto *tag = lv_label_create(right);
    lv_obj_set_style_text_font(tag, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(tag, d.connected ? Color(p.accent) : Color(p.sub_text), 0);
    lv_label_set_text(tag, d.connected ? "Đã kết nối" : (d.paired ? "Đã pair" : ""));

    auto *ctx = new BtRowCtx{this, d.address};
    lv_obj_add_event_cb(row, OnBtRowClicked, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(row, OnBtRowDeleted, LV_EVENT_DELETE, ctx);
}

void SettingsView::BtOpenModal(const std::string &addr) {
    CloseModal();
    modal_bt_addr_ = addr;
    // Find the device in the cached list for state + info.
    const jetson::BtDevice *d = nullptr;
    for (const auto &x : bt_devs_) if (x.address == addr) { d = &x; break; }
    modal_bt_connected_ = d ? d->connected : false;
    const auto &p = jetson::UiTheme::Instance().Palette();

    popup_ = lv_obj_create(overlay_);
    lv_obj_remove_style_all(popup_);
    lv_obj_set_size(popup_, width_, height_);
    lv_obj_set_style_bg_color(popup_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(popup_, LV_OPA_50, 0);
    lv_obj_add_flag(popup_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(popup_, OnPopupDismiss, LV_EVENT_CLICKED, this);

    int cardW = 320, cardH = 200;
    popup_card_ = lv_obj_create(popup_);
    lv_obj_remove_style_all(popup_card_);
    lv_obj_set_size(popup_card_, cardW, cardH);
    lv_obj_set_style_bg_color(popup_card_, Color(p.row), 0);
    lv_obj_set_style_bg_opa(popup_card_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(popup_card_, 16, 0);
    lv_obj_set_style_pad_all(popup_card_, 14, 0);
    lv_obj_set_style_pad_row(popup_card_, 8, 0);
    lv_obj_set_flex_flow(popup_card_, LV_FLEX_FLOW_COLUMN);
    lv_obj_align(popup_card_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(popup_card_, LV_OBJ_FLAG_CLICKABLE);

    auto *title = lv_label_create(popup_card_);
    lv_obj_set_style_text_font(title, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(title, Color(p.text), 0);
    lv_label_set_text(title, d ? d->name.c_str() : addr.c_str());

    auto *a = lv_label_create(popup_card_);
    lv_obj_set_style_text_font(a, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(a, Color(p.sub_text), 0);
    lv_label_set_text(a, ("Địa chỉ: " + addr).c_str());

    auto *st = lv_label_create(popup_card_);
    lv_obj_set_style_text_font(st, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(st, Color(p.sub_text), 0);
    const char *state = modal_bt_connected_ ? "Đã kết nối" : (d && d->paired ? "Đã pair" : "Chưa pair");
    lv_label_set_text(st, ("Trạng thái: " + std::string(state)).c_str());

    auto *btns = lv_obj_create(popup_card_);
    lv_obj_remove_style_all(btns);
    lv_obj_set_width(btns, lv_pct(100));
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btns, 8, 0);
    lv_obj_clear_flag(btns, LV_OBJ_FLAG_SCROLLABLE);
    MakeButton(btns, modal_bt_connected_ ? "Ngắt" : "Kết nối", 0x2b6fd6, OnModalBtAction);
    MakeButton(btns, "Quên", 0xb03a3a, OnModalBtRemove);
    MakeButton(btns, "Đóng", 0x3a3a3a, OnModalClose);
}

void SettingsView::BtDoAction(const std::string &addr, bool connected) {
    if (bt_busy_.exchange(true)) return;
    SetStatus(connected ? "Đang ngắt..." : "Đang kết nối...");
    std::thread([self = Self(), addr, connected]() {
        bool ok = connected ? self->bluetooth_.Disconnect(addr)
                            : self->bluetooth_.PairAndConnect(addr);
        std::vector<jetson::BtDevice> devs;
        if (ok) devs = self->bluetooth_.Scan(4);
        const std::string error = self->bluetooth_.LastError();
        LvLockGuard lock;
        self->bt_busy_ = false;
        if (ok) {
            self->bt_powered_ = true;
            self->bt_devs_ = std::move(devs);
            self->bt_scanned_ = true;
            if (self->bt_list_) self->BtRenderList();
            self->SetStatus(connected ? "Đã ngắt kết nối" : "Đã kết nối");
        } else {
            self->SetStatus(("Lỗi: " + error).c_str());
            ESP_LOGE(TAG, "Bluetooth action failed for %s: %s", addr.c_str(), error.c_str());
        }
    }).detach();
}

void SettingsView::BtDoRemove(const std::string &addr) {
    if (bt_busy_.exchange(true)) return;
    std::thread([self = Self(), addr]() {
        bool ok = self->bluetooth_.Remove(addr);
        std::vector<jetson::BtDevice> devs;
        if (ok && self->bluetooth_.IsPowered()) devs = self->bluetooth_.Scan(4);
        const std::string error = self->bluetooth_.LastError();
        LvLockGuard lock;
        self->bt_busy_ = false;
        if (ok) {
            self->bt_devs_ = std::move(devs);
            if (self->bt_list_) self->BtRenderList();
            self->SetStatus("Đã quên thiết bị");
        } else {
            self->SetStatus(("Lỗi: " + error).c_str());
            ESP_LOGE(TAG, "Bluetooth remove failed for %s: %s", addr.c_str(), error.c_str());
        }
    }).detach();
}

// =========================================================================
// Modal helpers
// =========================================================================

void SettingsView::CloseModal() {
    if (popup_) { lv_obj_del(popup_); popup_ = nullptr; popup_card_ = nullptr; }
    popup_confirm_btn_ = nullptr;
    popup_input_ = nullptr; // freed via its LV_EVENT_DELETE -> delete self
    pin_a_ = nullptr; pin_b_ = nullptr;
    modal_yes_ = nullptr;
}

void SettingsView::OpenConfirmModal(const char *title, const char *msg,
                                    std::function<void()> on_yes) {
    CloseModal();
    modal_yes_ = std::move(on_yes);
    const auto &p = jetson::UiTheme::Instance().Palette();

    popup_ = lv_obj_create(overlay_);
    lv_obj_remove_style_all(popup_);
    lv_obj_set_size(popup_, width_, height_);
    lv_obj_set_style_bg_color(popup_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(popup_, LV_OPA_50, 0);
    lv_obj_add_flag(popup_, LV_OBJ_FLAG_CLICKABLE);

    popup_card_ = lv_obj_create(popup_);
    lv_obj_remove_style_all(popup_card_);
    lv_obj_set_size(popup_card_, 300, 160);
    lv_obj_set_style_bg_color(popup_card_, Color(p.row), 0);
    lv_obj_set_style_bg_opa(popup_card_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(popup_card_, 16, 0);
    lv_obj_set_style_pad_all(popup_card_, 16, 0);
    lv_obj_set_style_pad_row(popup_card_, 10, 0);
    lv_obj_set_flex_flow(popup_card_, LV_FLEX_FLOW_COLUMN);
    lv_obj_align(popup_card_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(popup_card_, LV_OBJ_FLAG_CLICKABLE);

    auto *t = lv_label_create(popup_card_);
    lv_obj_set_style_text_font(t, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(t, Color(p.text), 0);
    lv_label_set_text(t, title);
    auto *m = lv_label_create(popup_card_);
    lv_obj_set_style_text_font(m, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(m, Color(p.sub_text), 0);
    lv_obj_set_width(m, 268);
    lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
    lv_label_set_text(m, msg);

    auto *btns = lv_obj_create(popup_card_);
    lv_obj_remove_style_all(btns);
    lv_obj_set_width(btns, lv_pct(100));
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btns, 8, 0);
    lv_obj_clear_flag(btns, LV_OBJ_FLAG_SCROLLABLE);
    MakeButton(btns, "Xác nhận", 0xb03a3a, OnModalConfirmYes);
    MakeButton(btns, "Hủy", 0x3a3a3a, OnModalClose);
}

void SettingsView::OpenPinModal() {
    CloseModal();
    const auto &p = jetson::UiTheme::Instance().Palette();
    std::string cur = Settings("system", true).GetString("pin", "");

    popup_ = lv_obj_create(overlay_);
    lv_obj_remove_style_all(popup_);
    lv_obj_set_size(popup_, width_, height_);
    lv_obj_set_style_bg_color(popup_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(popup_, LV_OPA_50, 0);
    lv_obj_add_flag(popup_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(popup_, OnPopupDismiss, LV_EVENT_CLICKED, this);

    popup_card_ = lv_obj_create(popup_);
    lv_obj_remove_style_all(popup_card_);
    lv_obj_set_size(popup_card_, 300, 230);
    lv_obj_set_style_bg_color(popup_card_, Color(p.row), 0);
    lv_obj_set_style_bg_opa(popup_card_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(popup_card_, 16, 0);
    lv_obj_set_style_pad_all(popup_card_, 14, 0);
    lv_obj_set_style_pad_row(popup_card_, 8, 0);
    lv_obj_set_flex_flow(popup_card_, LV_FLEX_FLOW_COLUMN);
    lv_obj_align(popup_card_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(popup_card_, LV_OBJ_FLAG_CLICKABLE);

    auto *t = lv_label_create(popup_card_);
    lv_obj_set_style_text_font(t, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(t, Color(p.text), 0);
    lv_label_set_text(t, cur.empty() ? "Đặt PIN (4 số)" : "Đổi PIN (4 số)");

    auto *lbl1 = lv_label_create(popup_card_);
    lv_obj_set_style_text_font(lbl1, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(lbl1, Color(p.sub_text), 0);
    lv_label_set_text(lbl1, "PIN mới");
    pin_a_ = new TelexInput(popup_card_, 272, 44);
    pin_a_->SetPassword(true);
    pin_a_->SetMaxLen(4);
    pin_a_->Focus();

    auto *lbl2 = lv_label_create(popup_card_);
    lv_obj_set_style_text_font(lbl2, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(lbl2, Color(p.sub_text), 0);
    lv_label_set_text(lbl2, "Nhập lại");
    pin_b_ = new TelexInput(popup_card_, 272, 44);
    pin_b_->SetPassword(true);
    pin_b_->SetMaxLen(4);

    auto *btns = lv_obj_create(popup_card_);
    lv_obj_remove_style_all(btns);
    lv_obj_set_width(btns, lv_pct(100));
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btns, 8, 0);
    lv_obj_clear_flag(btns, LV_OBJ_FLAG_SCROLLABLE);
    MakeButton(btns, "Lưu", 0x2b6fd6, OnPinSave);
    if (!cur.empty()) MakeButton(btns, "Xóa PIN", 0xb03a3a, OnPinClear);
    MakeButton(btns, "Hủy", 0x3a3a3a, OnModalClose);
}

// =========================================================================
// OnStart / OnResize
// =========================================================================

void SettingsView::OnStart() {
    SetStatus("");
}

void SettingsView::OnResize(int /*w*/, int h) {
    // Runs under the base class lock. Rebuild the current pane into the (already
    // resized) detail container. Sidebar/detail heights are % / flex-grown so
    // they reflow; only the pane content needs rebuilding.
    const int pane_h = std::max(240, h - 16);
    if (sidebar_) lv_obj_set_height(sidebar_, pane_h);
    if (detail_) lv_obj_set_height(detail_, pane_h);
    ShowCategory(current_);
}

// =========================================================================
// Event handlers
// =========================================================================

void SettingsView::OnSideClicked(lv_event_t *e) {
    LvLockGuard lock;
    auto *ctx = static_cast<SideCtx *>(lv_event_get_user_data(e));
    if (ctx) {
        if (ctx->cat == Cat::Display) ctx->self->display_page_ = DisplayPage::Main;
        ctx->self->ShowCategory(ctx->cat);
    }
}
void SettingsView::OnSideDeleted(lv_event_t *e) {
    delete static_cast<SideCtx *>(lv_event_get_user_data(e));
}
void SettingsView::OnWifiRowDeleted(lv_event_t *e) {
    delete static_cast<WifiRowCtx *>(lv_event_get_user_data(e));
}
void SettingsView::OnBtRowDeleted(lv_event_t *e) {
    delete static_cast<BtRowCtx *>(lv_event_get_user_data(e));
}
void SettingsView::OnOptDeleted(lv_event_t *e) {
    delete static_cast<OptCtx *>(lv_event_get_user_data(e));
}

void SettingsView::OnBrightChanged(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    int v = lv_slider_get_value(self->bright_slider_);
    if (v < 20) { v = 20; lv_slider_set_value(self->bright_slider_, 20, LV_ANIM_OFF); }
    Settings("display", true).SetInt("brightness", v);
    if (self->brightness_cb_) self->brightness_cb_(v);
    if (self->bright_value_label_) {
        char value[16];
        std::snprintf(value, sizeof(value), "%d%%", v);
        lv_label_set_text(self->bright_value_label_, value);
    }
    self->SetStatus(("Độ sáng: " + std::to_string(v) + "%").c_str());
}

void SettingsView::OnDisplayBack(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    self->display_page_ = DisplayPage::Main;
    self->ShowCategory(Cat::Display);
}

void SettingsView::OnOpenTextSize(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    self->display_page_ = DisplayPage::TextSize;
    self->ShowCategory(Cat::Display);
}

void SettingsView::OnOpenNightShift(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    self->display_page_ = DisplayPage::NightShift;
    self->ShowCategory(Cat::Display);
}

void SettingsView::OnOpenAutoLock(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    self->display_page_ = DisplayPage::AutoLock;
    self->ShowCategory(Cat::Display);
}

void SettingsView::OnOpenAlwaysOn(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    self->display_page_ = DisplayPage::AlwaysOn;
    self->ShowCategory(Cat::Display);
}

void SettingsView::OnTextSizeChanged(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    const int step = std::clamp((int)lv_slider_get_value(self->text_size_slider_), 0, 6);
    const int size = kFontSizes[step];
    const bool bold = Settings("display", false).GetBool("bold_text", false);
    jetson::ApplyBuiltinTypography(size, bold);
    if (self->text_size_value_label_) {
        char value[20];
        std::snprintf(value, sizeof(value), "%d%%", size * 100 / 28);
        lv_label_set_text(self->text_size_value_label_, value);
    }
    self->SetStatus(("Cỡ chữ: " + std::to_string(size) + " px").c_str());
}

void SettingsView::OnBoldToggle(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    const bool on = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
    const int size = Settings("display", false).GetInt("font_size", 28);
    jetson::ApplyBuiltinTypography(size, on);
    self->SetStatus(on ? "Đã bật chữ đậm" : "Đã tắt chữ đậm");
}

void SettingsView::OnTrueToneToggle(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    const bool on = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
    Settings("display", true).SetBool("true_tone", on);
    if (self->display_preferences_cb_) self->display_preferences_cb_();
    self->SetStatus(on ? "True Tone: Bật" : "True Tone: Tắt");
}

void SettingsView::OnNightShiftToggle(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    const bool on = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
    Settings("display", true).SetBool("night_shift", on);
    if (self->display_preferences_cb_) self->display_preferences_cb_();
    self->SetStatus(on ? "Night Shift: Bật" : "Night Shift: Tắt");
}

void SettingsView::OnNightWarmthChanged(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    const int warmth = lv_slider_get_value(self->night_warmth_slider_);
    Settings("display", true).SetInt("night_warmth", warmth);
    if (self->display_preferences_cb_) self->display_preferences_cb_();
    self->SetStatus(("Độ ấm Night Shift: " + std::to_string(warmth) + "%").c_str());
}

void SettingsView::OnTouchWakeToggle(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    const bool on = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
    Settings("display", true).SetBool("touch_to_wake", on);
    if (self->display_preferences_cb_) self->display_preferences_cb_();
    self->SetStatus(on ? "Chạm để bật: Bật" : "Chạm để bật: Tắt");
}

void SettingsView::OnAlwaysOnToggle(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    const bool on = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
    Settings("display", true).SetBool("always_on", on);
    if (self->display_preferences_cb_) self->display_preferences_cb_();
    self->SetStatus(on ? "Màn hình luôn bật: Bật" : "Màn hình luôn bật: Tắt");
}

void SettingsView::OnAlwaysOnWallpaperToggle(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    const bool on = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
    Settings("display", true).SetBool("aod_wallpaper", on);
    if (self->display_preferences_cb_) self->display_preferences_cb_();
}

void SettingsView::OnAlwaysOnBlurToggle(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    const bool on = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
    Settings("display", true).SetBool("aod_blur", on);
    if (self->display_preferences_cb_) self->display_preferences_cb_();
}

void SettingsView::OnAlwaysOnNotificationsToggle(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    const bool on = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
    Settings("display", true).SetBool("aod_notifications", on);
    if (self->display_preferences_cb_) self->display_preferences_cb_();
}

void SettingsView::OnVolChanged(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    int v = lv_slider_get_value(self->vol_slider_);
    Settings("display", true).SetInt("volume", v);
    if (self->volume_cb_) self->volume_cb_(v, !lv_obj_has_state(self->mute_switch_, LV_STATE_CHECKED));
    self->SetStatus(("Âm lượng: " + std::to_string(v) + "%").c_str());
}

void SettingsView::OnMuteToggle(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    bool audible = lv_obj_has_state(self->mute_switch_, LV_STATE_CHECKED);
    Settings("display", true).SetBool("muted", !audible);
    int v = self->vol_slider_ ? lv_slider_get_value(self->vol_slider_) : 0;
    if (self->volume_cb_) self->volume_cb_(v, !audible);
    self->SetStatus(audible ? "Bật tiếng" : "Tắt tiếng");
}

void SettingsView::OnWifiSwitch(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    bool on = lv_obj_has_state(self->wifi_switch_, LV_STATE_CHECKED);
    if (self->wifi_busy_.exchange(true)) {
        self->WifiRefreshSwitch();
        return;
    }
    std::thread([self = self->Self(), on]() {
        const bool ok = self->wifi_.Enable(on);
        std::vector<jetson::WifiNetwork> nets;
        std::string active;
        if (ok && on) {
            nets = self->wifi_.Scan();
            active = self->wifi_.ActiveSsid();
        }
        const std::string error = self->wifi_.LastError();
        LvLockGuard lock;
        self->wifi_busy_ = false;
        if (ok) {
            self->wifi_enabled_ = on;
            self->wifi_scanned_ = false;
            self->wifi_nets_ = std::move(nets);
            self->wifi_scanned_ = on;
            if (self->wifi_list_) self->WifiRenderList();
            self->SetStatus(on ? (active.empty() ? "Đã bật WiFi"
                                                 : ("Đã kết nối: " + active).c_str())
                               : "Đã tắt WiFi");
        } else {
            self->WifiRefreshSwitch();
            self->SetStatus(("Lỗi WiFi: " + error).c_str());
            ESP_LOGE(TAG, "WiFi power change failed: %s", error.c_str());
        }
    }).detach();
}

void SettingsView::OnWifiRescan(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    self->WifiRescan();
}

void SettingsView::OnWifiRowClicked(lv_event_t *e) {
    LvLockGuard lock;
    auto *ctx = static_cast<WifiRowCtx *>(lv_event_get_user_data(e));
    if (!ctx) return;
    const auto &network = ctx->network;
    if (network.in_use) return;
    if (network.secured && !network.known)
        ctx->self->WifiOpenConnectSheet(network);
    else
        ctx->self->WifiDoConnect(network.ssid, "");
}

void SettingsView::OnWifiInfoClicked(lv_event_t *e) {
    LvLockGuard lock;
    lv_event_stop_bubbling(e);
    auto *ctx = static_cast<WifiRowCtx *>(lv_event_get_user_data(e));
    if (!ctx) return;
    ctx->self->WifiLoadDetails(ctx->network);
}

void SettingsView::OnBtSwitch(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    bool on = lv_obj_has_state(self->bt_switch_, LV_STATE_CHECKED);
    if (self->bt_busy_.exchange(true)) {
        self->BtRefreshSwitch();
        return;
    }
    std::thread([self = self->Self(), on]() {
        const bool ok = on ? self->bluetooth_.PowerOn() : self->bluetooth_.PowerOff();
        std::vector<jetson::BtDevice> devs;
        if (ok && on) devs = self->bluetooth_.Scan(8);
        const std::string error = self->bluetooth_.LastError();
        LvLockGuard lock;
        self->bt_busy_ = false;
        if (ok) {
            self->bt_powered_ = on;
            self->bt_scanned_ = false;
            self->bt_devs_ = std::move(devs);
            self->bt_scanned_ = on;
            if (self->bt_list_) self->BtRenderList();
            self->SetStatus(on ? "Đã bật Bluetooth" : "Đã tắt Bluetooth");
        } else {
            self->BtRefreshSwitch();
            self->SetStatus(("Lỗi Bluetooth: " + error).c_str());
            ESP_LOGE(TAG, "Bluetooth power change failed: %s", error.c_str());
        }
    }).detach();
}

void SettingsView::OnBtRescan(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    self->BtRescan();
}

void SettingsView::OnBtRowClicked(lv_event_t *e) {
    LvLockGuard lock;
    auto *ctx = static_cast<BtRowCtx *>(lv_event_get_user_data(e));
    if (!ctx) return;
    ctx->self->BtOpenModal(ctx->addr);
}

void SettingsView::OnLangVi(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    Settings("input", true).SetString("kbd_lang", "vi");
    self->ShowCategory(Cat::Keyboard);
}
void SettingsView::OnLangEn(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    Settings("input", true).SetString("kbd_lang", "en");
    self->ShowCategory(Cat::Keyboard);
}

void SettingsView::On24hToggle(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    bool on = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
    Settings("display", true).SetBool("clock_24h", on);
    self->SetStatus(on ? "Định dạng 24h" : "Định dạng 12h");
    self->ShowCategory(Cat::DateTime);
}

void SettingsView::OnTzSelected(lv_event_t *e) {
    LvLockGuard lock;
    auto *ctx = static_cast<OptCtx *>(lv_event_get_user_data(e));
    if (!ctx) return;
    std::string tz = ctx->value;
    ctx->self->SetStatus(("Đang đặt múi giờ " + tz + "...").c_str());
    std::thread([self = ctx->self->Self(), tz]() {
        RunCapture("timedatectl set-timezone " + jetson::platform::QuoteShellArgument(tz));
        Settings("system", true).SetString("timezone", tz);
        LvLockGuard lock;
        if (self) {
            self->SetStatus(("Múi giờ: " + tz).c_str());
            self->ShowCategory(Cat::DateTime);
        }
    }).detach();
}

void SettingsView::OnSleepSelected(lv_event_t *e) {
    LvLockGuard lock;
    auto *ctx = static_cast<OptCtx *>(lv_event_get_user_data(e));
    if (!ctx) return;
    int secs = 0;
    try { secs = std::stoi(ctx->value); } catch (...) { secs = 0; }
    Settings("display", true).SetInt("sleep_timeout", secs);
    ctx->self->SetStatus(secs == 0 ? "Tự tắt: Không"
                                   : ("Tự tắt sau " + std::to_string(secs) + "s").c_str());
    if (ctx->self->current_ == Cat::Display &&
        ctx->self->display_page_ == DisplayPage::AutoLock) {
        ctx->self->ShowCategory(Cat::Display);
    } else {
        ctx->self->ShowCategory(Cat::Power);
    }
}

void SettingsView::OnLockNow(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    if (self->lock_cb_) self->lock_cb_();
}

void SettingsView::OnSetPin(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    self->OpenPinModal();
}

void SettingsView::OnReboot(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    self->OpenConfirmModal("Khởi động lại?", "Thiết bị sẽ khởi động lại ngay.", []() {
        std::thread([]() { sync(); int r = system("reboot"); (void)r; }).detach();
    });
}

void SettingsView::OnShutdown(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    self->OpenConfirmModal("Tắt máy?", "Thiết bị sẽ tắt ngay.", []() {
        std::thread([]() { sync(); int r = system("poweroff"); (void)r; }).detach();
    });
}

void SettingsView::OnPopupDismiss(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    if (lv_event_get_target(e) == self->popup_) self->CloseModal();
}

void SettingsView::OnModalClose(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    self->CloseModal();
}

void SettingsView::OnModalConnect(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    std::string ssid = self->modal_ssid_;
    std::string pw = self->popup_input_ ? self->popup_input_->Text() : "";
    if (self->popup_input_ && pw.empty()) return;
    self->CloseModal();
    self->WifiDoConnect(ssid, pw);
}

void SettingsView::OnModalPasswordChanged(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    if (!self->popup_confirm_btn_ || !self->popup_input_) return;
    const bool ready = !self->popup_input_->Text().empty();
    const auto &p = jetson::UiTheme::Instance().Palette();
    lv_obj_set_style_bg_color(self->popup_confirm_btn_,
                              Color(ready ? p.accent : p.button), 0);
    lv_obj_set_style_bg_opa(self->popup_confirm_btn_,
                            ready ? LV_OPA_COVER : LV_OPA_60, 0);
    if (ready) lv_obj_clear_state(self->popup_confirm_btn_, LV_STATE_DISABLED);
    else lv_obj_add_state(self->popup_confirm_btn_, LV_STATE_DISABLED);
}

void SettingsView::OnModalForget(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    std::string ssid = self->modal_ssid_;
    self->CloseModal();
    self->WifiDoForget(ssid);
}

void SettingsView::OnModalBtAction(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    std::string addr = self->modal_bt_addr_;
    bool connected = self->modal_bt_connected_;
    self->CloseModal();
    self->BtDoAction(addr, connected);
}

void SettingsView::OnModalBtRemove(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    std::string addr = self->modal_bt_addr_;
    self->CloseModal();
    self->BtDoRemove(addr);
}

void SettingsView::OnModalConfirmYes(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    auto yes = std::move(self->modal_yes_);
    self->CloseModal();
    if (yes) yes();
}

void SettingsView::OnPinSave(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    if (!self->pin_a_ || !self->pin_b_) return;
    std::string a = self->pin_a_->Text();
    std::string b = self->pin_b_->Text();
    if (a.size() != 4 || a != b) {
        self->SetStatus("PIN không hợp lệ hoặc không khớp (cần 4 ký tự)");
        return;
    }
    Settings("system", true).SetString("pin", a);
    self->SetStatus("Đã lưu PIN");
    self->CloseModal();
}

void SettingsView::OnPinClear(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    Settings("system", true).SetString("pin", "");
    self->SetStatus("Đã xóa PIN");
    self->CloseModal();
}

} // namespace home
