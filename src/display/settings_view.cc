#include "settings_view.h"
#include "fonts.h"
#include "ui_theme.h"
#include "lvgl_runtime.h"
#include "settings.h"
#include "esp_log.h"

#include <lvgl.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <unistd.h>
#include <vector>

#include <sys/statvfs.h>
#include <sys/sysinfo.h>

#define TAG "SettingsView"

namespace home {

namespace {

lv_color_t Color(uint32_t rgb) {
    return lv_color_make((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

struct LvLockGuard {
    LvLockGuard() { lv_lock(); }
    ~LvLockGuard() { lv_unlock(); }
};

// Run `cmd` via /bin/sh, capture combined stdout+stderr (trimmed).
std::string RunCapture(const std::string &cmd) {
    std::string out;
    FILE *p = popen((cmd + " 2>&1").c_str(), "r");
    if (!p) return "";
    char buf[256];
    while (fgets(buf, sizeof(buf), p)) out += buf;
    pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' ||
                            out.back() == ' ' || out.back() == '\t')) {
        out.pop_back();
    }
    return out;
}

// Sidebar glyphs (FONT_AWESOME / LV_SYMBOL).
struct SleepOpt { int seconds; const char *label; };
const SleepOpt kSleepOpts[] = {
    {15, "15 giây"}, {30, "30 giây"}, {60, "1 phút"}, {300, "5 phút"}, {0, "Không"},
};
const char *kTimezones[] = {
    "Asia/Ho_Chi_Minh", "Asia/Bangkok", "Asia/Tokyo", "Asia/Shanghai",
    "Asia/Singapore", "Asia/Hong_Kong", "Asia/Kolkata", "Asia/Dubai",
    "Europe/London", "Europe/Paris", "Europe/Berlin", "Europe/Moscow",
    "America/New_York", "America/Chicago", "America/Los_Angeles",
    "America/Sao_Paulo", "Australia/Sydney", "UTC",
};

std::string CurrentTimezone() {
    std::string tz = RunCapture("timedatectl show -p Timezone --value");
    if (tz.empty()) {
        // Fallback: read the symlink target of /etc/localtime.
        tz = RunCapture("readlink /etc/localtime");
        auto p = tz.find("zoneinfo/");
        if (p != std::string::npos) tz = tz.substr(p + 8);
    }
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

} // namespace

// =========================================================================
// Construction + layout
// =========================================================================

std::shared_ptr<SettingsView> SettingsView::Self() {
    // shared_from_this() yields shared_ptr<OverlayView>; cast down to this type
    // so worker lambdas can call SettingsView members.
    return std::static_pointer_cast<SettingsView>(shared_from_this());
}

SettingsView::SettingsView(lv_obj_t *parent, int width, int height, ClosedCb on_closed)
    : OverlayView(parent, width, height, "Cài đặt", std::move(on_closed)) {
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

    int body_h = (height_ - 80) - 16;

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
        {Cat::Appearance, LV_SYMBOL_IMAGE, "Giao diện"},
        {Cat::Display, LV_SYMBOL_BRIGHTNESS, "Hiển thị"},
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

    ShowCategory(Cat::Appearance);
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
        case Cat::Appearance: BuildAppearance(); break;
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
    lv_obj_set_size(sw, 60, 28);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, this);
    return sw;
}

lv_obj_t *SettingsView::MakeSlider(lv_obj_t *parent, int minv, int maxv, int val,
                                   lv_event_cb_t cb) {
    auto *sl = lv_slider_create(parent);
    lv_obj_set_width(sl, 220);
    lv_slider_set_range(sl, minv, maxv);
    lv_slider_set_value(sl, val, LV_ANIM_OFF);
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

// =========================================================================
// Panes
// =========================================================================

void SettingsView::BuildAppearance() {
    SectionTitle("Giao diện");
    const auto &p = jetson::UiTheme::Instance().Palette();
    auto *row = MakeRow("Chủ đề sáng/tối",
                        jetson::UiTheme::Instance().Mode() == jetson::UiMode::Light ? "Sáng" : "Tối");
    auto *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 60, 28);
    if (jetson::UiTheme::Instance().Mode() == jetson::UiMode::Light)
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, OnThemeToggle, LV_EVENT_VALUE_CHANGED, this);
    (void)p;
}

void SettingsView::BuildDisplay() {
    SectionTitle("Độ sáng");
    int v = Settings("display", true).GetInt("brightness", 100);
    if (v < 15) v = 15;
    if (v > 100) v = 100;
    char sub[32];
    std::snprintf(sub, sizeof(sub), "%d%%", v);
    auto *row = MakeRow("Độ sáng màn hình", sub);
    auto *sl = lv_slider_create(row);
    lv_obj_set_width(sl, 220);
    lv_slider_set_range(sl, 15, 100);
    lv_slider_set_value(sl, v, LV_ANIM_OFF);
    lv_obj_add_event_cb(sl, OnBrightChanged, LV_EVENT_VALUE_CHANGED, this);
    bright_slider_ = sl;
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
    // Toggle + rescan row.
    auto *top = MakeRow("Bật WiFi", nullptr);
    wifi_switch_ = MakeSwitch(top, false, OnWifiSwitch);
    WifiRefreshSwitch();

    auto *btns = lv_obj_create(detail_);
    lv_obj_remove_style_all(btns);
    lv_obj_set_width(btns, lv_pct(100));
    lv_obj_set_height(btns, 44);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btns, 8, 0);
    lv_obj_clear_flag(btns, LV_OBJ_FLAG_SCROLLABLE);
    MakeButton(btns, "Quét lại", 0x2b6fd6, OnWifiRescan);

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

    // Sleep timeout.
    int cur = Settings("display", true).GetInt("sleep_timeout", 0);
    const char *curLabel = "Không";
    for (const auto &o : kSleepOpts) if (o.seconds == cur) { curLabel = o.label; break; }
    auto *sleepRow = MakeRow("Tự tắt màn hình", curLabel);
    (void)sleepRow;
    auto *sopts = lv_obj_create(detail_);
    lv_obj_remove_style_all(sopts);
    lv_obj_set_width(sopts, lv_pct(100));
    lv_obj_set_flex_flow(sopts, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(sopts, 6, 0);
    lv_obj_set_style_pad_row(sopts, 6, 0);
    lv_obj_clear_flag(sopts, LV_OBJ_FLAG_SCROLLABLE);
    for (const auto &o : kSleepOpts) {
        const auto &p = jetson::UiTheme::Instance().Palette();
        bool sel = (o.seconds == cur);
        auto *b = lv_button_create(sopts);
        lv_obj_set_height(b, 34);
        lv_obj_set_style_bg_color(b, sel ? Color(p.accent) : Color(p.bg), 0);
        lv_obj_set_style_radius(b, 8, 0);
        auto *l = lv_label_create(b);
        lv_obj_set_style_text_font(l, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(l, sel ? lv_color_white() : Color(p.text), 0);
        lv_label_set_text(l, o.label);
        lv_obj_center(l);
        auto *ctx = new OptCtx{this, std::to_string(o.seconds)};
        lv_obj_add_event_cb(b, OnSleepSelected, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(b, OnOptDeleted, LV_EVENT_DELETE, ctx);
    }

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
        storage = "Lưu trữ: " + FormatBytes(freeb) + " / " + FormatBytes(total) + " trống";
    }
    auto *s = lv_label_create(detail_);
    lv_obj_set_style_text_font(s, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(s, Color(p.sub_text), 0);
    lv_obj_set_width(s, lv_pct(100));
    lv_label_set_long_mode(s, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s, storage.c_str());

    // Memory (sysinfo).
    std::string mem = "RAM: —";
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        unsigned long long total = (unsigned long long)si.totalram * si.mem_unit;
        unsigned long long freeb = (unsigned long long)si.freeram * si.mem_unit;
        mem = "RAM: " + FormatBytes(freeb) + " / " + FormatBytes(total) + " trống";
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
    bool on = jetson::WifiManager::Instance().IsEnabled();
    if (on) lv_obj_add_state(wifi_switch_, LV_STATE_CHECKED);
    else lv_obj_clear_state(wifi_switch_, LV_STATE_CHECKED);
}

void SettingsView::WifiRescan() {
    if (!wifi_list_) return;
    if (!jetson::WifiManager::Instance().IsEnabled()) {
        SetStatus("WiFi đang tắt");
        wifi_nets_.clear();
        WifiRenderList();
        return;
    }
    SetStatus("Đang quét WiFi...");
    std::thread([self = Self()]() {
        auto nets = jetson::WifiManager::Instance().Scan();
        lv_lock();
        if (self) {
            self->wifi_nets_ = std::move(nets);
            self->wifi_scanned_ = true;
            self->WifiRenderList();
            auto active = jetson::WifiManager::Instance().ActiveSsid();
            self->SetStatus(active.empty() ? "Chạm mạng để xem/kết nối"
                                           : ("Đã kết nối: " + active).c_str());
        }
        lv_unlock();
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
        lv_label_set_text(e, "Không có mạng. Bấm \"Quét lại\".");
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

    auto *ssid = lv_label_create(row);
    lv_obj_set_style_text_font(ssid, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(ssid, Color(p.text), 0);
    lv_label_set_text(ssid, n.ssid.c_str());
    lv_label_set_long_mode(ssid, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(ssid, 1);
    lv_obj_set_style_max_width(ssid, 260, 0);

    auto *right = lv_obj_create(row);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, 90, 22);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(right, 6, 0);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    if (n.secured) {
        auto *lock = lv_label_create(right);
        lv_obj_set_style_text_font(lock, &BUILTIN_ICON_FONT, 0);
        lv_obj_set_style_text_color(lock, Color(p.sub_text), 0);
        lv_label_set_text(lock, LV_SYMBOL_EYE_CLOSE); // "secured"
    }
    DrawSignalBars(right, n.signal);
    if (n.in_use) {
        auto *tag = lv_label_create(right);
        lv_obj_set_style_text_font(tag, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(tag, Color(p.accent), 0);
        lv_label_set_text(tag, "•");
    }

    auto *ctx = new WifiRowCtx{this, n.ssid, n.secured, n.in_use, n.signal};
    lv_obj_add_event_cb(row, OnWifiRowClicked, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(row, OnWifiRowDeleted, LV_EVENT_DELETE, ctx);
}

void SettingsView::WifiOpenModal(const WifiRowCtx &info) {
    CloseModal();
    modal_ssid_ = info.ssid;
    const auto &p = jetson::UiTheme::Instance().Palette();

    popup_ = lv_obj_create(overlay_);
    lv_obj_remove_style_all(popup_);
    lv_obj_set_size(popup_, width_, height_);
    lv_obj_set_style_bg_color(popup_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(popup_, LV_OPA_50, 0);
    lv_obj_add_flag(popup_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(popup_, OnPopupDismiss, LV_EVENT_CLICKED, this);

    int cardW = 320, cardH = info.secured && !info.in_use ? 230 : 190;
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
    lv_label_set_text(title, info.ssid.c_str());

    char line[96];
    std::snprintf(line, sizeof(line), "Tín hiệu: %d%%", info.signal);
    auto *sig = lv_label_create(popup_card_);
    lv_obj_set_style_text_font(sig, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(sig, Color(p.sub_text), 0);
    lv_label_set_text(sig, line);

    auto *sec = lv_label_create(popup_card_);
    lv_obj_set_style_text_font(sec, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(sec, Color(p.sub_text), 0);
    lv_label_set_text(sec, info.secured ? "Bảo mật: WPA/WEP" : "Bảo mật: Mở");

    auto *st = lv_label_create(popup_card_);
    lv_obj_set_style_text_font(st, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(st, Color(p.sub_text), 0);
    lv_label_set_text(st, info.in_use ? "Trạng thái: Đã kết nối" : "Trạng thái: Chưa kết nối");

    if (info.secured && !info.in_use) {
        popup_input_ = new TelexInput(popup_card_, cardW - 28, 44);
        popup_input_->SetPassword(true);
        popup_input_->SetMaxLen(63);
        popup_input_->SetPlaceholder("Mật khẩu...");
        popup_input_->Focus();
    } else {
        popup_input_ = nullptr;
    }

    auto *btns = lv_obj_create(popup_card_);
    lv_obj_remove_style_all(btns);
    lv_obj_set_width(btns, lv_pct(100));
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btns, 8, 0);
    lv_obj_clear_flag(btns, LV_OBJ_FLAG_SCROLLABLE);
    if (!info.in_use) MakeButton(btns, "Kết nối", 0x2b6fd6, OnModalConnect);
    MakeButton(btns, "Quên", 0xb03a3a, OnModalForget);
    MakeButton(btns, "Đóng", 0x3a3a3a, OnModalClose);
}

void SettingsView::WifiDoConnect(const std::string &ssid, const std::string &pw) {
    SetStatus(("Đang kết nối " + ssid + "...").c_str());
    std::thread([self = Self(), ssid, pw]() {
        bool ok = jetson::WifiManager::Instance().Connect(ssid, pw);
        lv_lock();
        if (self) {
            if (ok) { self->SetStatus(("Đã kết nối: " + ssid).c_str()); self->WifiRescan(); }
            else self->SetStatus(("Lỗi: " + jetson::WifiManager::Instance().LastError()).c_str());
        }
        lv_unlock();
    }).detach();
}

void SettingsView::WifiDoForget(const std::string &ssid) {
    std::thread([self = Self(), ssid]() {
        bool ok = jetson::WifiManager::Instance().Forget(ssid);
        lv_lock();
        if (self) {
            self->SetStatus(ok ? ("Đã quên: " + ssid).c_str()
                               : ("Lỗi: " + jetson::WifiManager::Instance().LastError()).c_str());
            self->WifiRescan();
        }
        lv_unlock();
    }).detach();
}

// =========================================================================
// Bluetooth
// =========================================================================

void SettingsView::BtRefreshSwitch() {
    if (!bt_switch_) return;
    bool on = jetson::BluetoothManager::Instance().IsPowered();
    if (on) lv_obj_add_state(bt_switch_, LV_STATE_CHECKED);
    else lv_obj_clear_state(bt_switch_, LV_STATE_CHECKED);
}

void SettingsView::BtRescan() {
    if (!bt_list_) return;
    if (!jetson::BluetoothManager::Instance().IsPowered()) {
        SetStatus("Bluetooth đang tắt");
        bt_devs_.clear();
        BtRenderList();
        return;
    }
    SetStatus("Đang quét Bluetooth...");
    std::thread([self = Self()]() {
        auto devs = jetson::BluetoothManager::Instance().Scan(8);
        lv_lock();
        if (self) {
            self->bt_devs_ = std::move(devs);
            self->bt_scanned_ = true;
            self->BtRenderList();
            self->SetStatus("Chạm thiết bị để kết nối/ngắt/quên");
        }
        lv_unlock();
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
    if (lvl < 0) lvl = 0; if (lvl > 100) lvl = 100;
    DrawSignalBars(right, lvl);
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
    SetStatus(connected ? "Đang ngắt..." : "Đang kết nối...");
    std::thread([self = Self(), addr, connected]() {
        bool ok = connected ? jetson::BluetoothManager::Instance().Disconnect(addr)
                            : jetson::BluetoothManager::Instance().PairAndConnect(addr);
        lv_lock();
        if (self) {
            self->SetStatus(ok ? "Xong" : ("Lỗi: " + jetson::BluetoothManager::Instance().LastError()).c_str());
            self->BtRescan();
        }
        lv_unlock();
    }).detach();
}

void SettingsView::BtDoRemove(const std::string &addr) {
    std::thread([self = Self(), addr]() {
        bool ok = jetson::BluetoothManager::Instance().Remove(addr);
        lv_lock();
        if (self) {
            self->SetStatus(ok ? "Đã quên thiết bị" : ("Lỗi: " + jetson::BluetoothManager::Instance().LastError()).c_str());
            self->BtRescan();
        }
        lv_unlock();
    }).detach();
}

// =========================================================================
// Modal helpers
// =========================================================================

void SettingsView::CloseModal() {
    if (popup_) { lv_obj_del(popup_); popup_ = nullptr; popup_card_ = nullptr; }
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

void SettingsView::DrawSignalBars(lv_obj_t *parent, int level01, int bars) {
    auto *cont = lv_obj_create(parent);
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, bars * 8, 22);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(cont, 2, 0);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    int filled = 0;
    if (level01 >= 75) filled = 4;
    else if (level01 >= 50) filled = 3;
    else if (level01 >= 25) filled = 2;
    else if (level01 > 0) filled = 1;
    const int heights[4] = {8, 12, 16, 20};
    for (int i = 0; i < bars; ++i) {
        auto *bar = lv_obj_create(cont);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 6, heights[i]);
        lv_obj_set_style_radius(bar, 1, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(bar, i < filled ? lv_color_white() : Color(0x555555), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    }
}

// =========================================================================
// OnStart / OnResize
// =========================================================================

void SettingsView::OnStart() {
    SetStatus("Chọn mục bên trái");
}

void SettingsView::OnResize(int /*w*/, int /*h*/) {
    // Runs under the base class lock. Rebuild the current pane into the (already
    // resized) detail container. Sidebar/detail heights are % / flex-grown so
    // they reflow; only the pane content needs rebuilding.
    ShowCategory(current_);
}

// =========================================================================
// Event handlers
// =========================================================================

void SettingsView::OnSideClicked(lv_event_t *e) {
    LvLockGuard lock;
    auto *ctx = static_cast<SideCtx *>(lv_event_get_user_data(e));
    if (ctx) ctx->self->ShowCategory(ctx->cat);
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

void SettingsView::OnThemeToggle(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    jetson::UiTheme::Instance().Toggle();
    self->ShowCategory(Cat::Appearance); // rebuild to refresh the "Sáng/Tối" sub
    self->SetStatus("Đã đổi giao diện");
}

void SettingsView::OnBrightChanged(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    int v = lv_slider_get_value(self->bright_slider_);
    if (v < 15) { v = 15; lv_slider_set_value(self->bright_slider_, 15, LV_ANIM_OFF); }
    Settings("display", true).SetInt("brightness", v);
    if (self->brightness_cb_) self->brightness_cb_(v);
    self->SetStatus(("Độ sáng: " + std::to_string(v) + "%").c_str());
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
    std::thread([self = self->Self(), on]() {
        jetson::WifiManager::Instance().Enable(on);
        lv_lock();
        if (self) {
            self->wifi_scanned_ = false;
            self->SetStatus(on ? "Đang bật WiFi..." : "Đã tắt WiFi");
            if (self->wifi_list_) lv_obj_clean(self->wifi_list_);
            if (on) self->WifiRescan();
            else { self->wifi_nets_.clear(); }
        }
        lv_unlock();
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
    ctx->self->WifiOpenModal(*ctx);
}

void SettingsView::OnBtSwitch(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    bool on = lv_obj_has_state(self->bt_switch_, LV_STATE_CHECKED);
    std::thread([self = self->Self(), on]() {
        if (on) jetson::BluetoothManager::Instance().PowerOn();
        else jetson::BluetoothManager::Instance().PowerOff();
        lv_lock();
        if (self) {
            self->bt_scanned_ = false;
            self->SetStatus(on ? "Đang bật Bluetooth..." : "Đã tắt Bluetooth");
            if (self->bt_list_) lv_obj_clean(self->bt_list_);
            if (on) self->BtRescan();
            else self->bt_devs_.clear();
        }
        lv_unlock();
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
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
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
        RunCapture("timedatectl set-timezone " + tz);
        Settings("system", true).SetString("timezone", tz);
        lv_lock();
        if (self) {
            self->SetStatus(("Múi giờ: " + tz).c_str());
            self->ShowCategory(Cat::DateTime);
        }
        lv_unlock();
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
    ctx->self->ShowCategory(Cat::Power);
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
        std::thread([]() { sync(); system("reboot"); }).detach();
    });
}

void SettingsView::OnShutdown(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<SettingsView *>(lv_event_get_user_data(e));
    self->OpenConfirmModal("Tắt máy?", "Thiết bị sẽ tắt ngay.", []() {
        std::thread([]() { sync(); system("poweroff"); }).detach();
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
    self->CloseModal();
    self->WifiDoConnect(ssid, pw);
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