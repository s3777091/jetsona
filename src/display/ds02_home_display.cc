#include "ds02_home_display.h"
#include "backgrounds.h"
#include "board.h"
#include "fonts.h"
#include "settings.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "font_awesome.h"
#include "ui_theme.h"

#include <lvgl.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <string>

#define TAG "Ds02Home"

namespace {
int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
lv_color_t Color(uint32_t rgb) { return lv_color_make((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff); }

// Read PNG pixel dimensions from the IHDR chunk (width@16, height@20, big-endian)
// so we can compute an lv_image scale before LVGL lazily decodes the body
// (LvglRawImage leaves image_dsc_.header.w/h at 0 until decode).
bool PngSize(const char *path, int *w, int *h) {
    *w = 0; *h = 0;
    FILE *f = std::fopen(path, "rb");
    if (!f) return false;
    uint8_t hdr[24];
    size_t n = std::fread(hdr, 1, sizeof(hdr), f);
    std::fclose(f);
    if (n < 24 || hdr[0] != 0x89 || hdr[1] != 0x50 || hdr[2] != 0x4E || hdr[3] != 0x47) return false;
    *w = (hdr[16] << 24) | (hdr[17] << 16) | (hdr[18] << 8) | hdr[19];
    *h = (hdr[20] << 24) | (hdr[21] << 16) | (hdr[22] << 8) | hdr[23];
    return *w > 0 && *h > 0;
}

// lv_image zoom (256 == 100%) that fits a PNG's longer edge to target_px.
int PngScaleToFit(const char *path, int target_px) {
    int w = 0, h = 0;
    if (PngSize(path, &w, &h) && w > 0 && h > 0) {
        int longer = std::max(w, h);
        int s = target_px * 256 / longer;
        return Clamp(s, 32, 1024);
    }
    return 256; // unknown size -> leave native (100%)
}

constexpr int kSystemBarHeight = 28;
constexpr int kDockHeight = 78;
constexpr int kDockBottomMargin = 6;
constexpr int kDockButtonSize = 50;
constexpr int kDockItemHeight = 64;
constexpr int kDockScaleNormal = 256;
constexpr int kDockScaleNeighbor = 278;
constexpr int kDockScaleFocused = 318;
constexpr uint64_t kRefreshIntervalUs = 1000000; // 1s

void DockScaleAnim(void *var, int32_t value) {
    lv_obj_set_style_transform_scale(static_cast<lv_obj_t *>(var), value, 0);
}

void DockTranslateAnim(void *var, int32_t value) {
    lv_obj_set_style_translate_y(static_cast<lv_obj_t *>(var), value, 0);
}

void AnimateDockButton(lv_obj_t *button, int32_t scale, int32_t translate_y,
                       uint32_t duration = 130) {
    if (!button) return;

    lv_anim_delete(button, DockScaleAnim);
    lv_anim_t scale_anim;
    lv_anim_init(&scale_anim);
    lv_anim_set_var(&scale_anim, button);
    lv_anim_set_values(&scale_anim,
                       lv_obj_get_style_transform_scale_x(button, 0), scale);
    lv_anim_set_time(&scale_anim, duration);
    lv_anim_set_exec_cb(&scale_anim, DockScaleAnim);
    lv_anim_set_path_cb(&scale_anim, lv_anim_path_ease_out);
    lv_anim_start(&scale_anim);

    lv_anim_delete(button, DockTranslateAnim);
    lv_anim_t translate_anim;
    lv_anim_init(&translate_anim);
    lv_anim_set_var(&translate_anim, button);
    lv_anim_set_values(&translate_anim,
                       lv_obj_get_style_translate_y(button, 0), translate_y);
    lv_anim_set_time(&translate_anim, duration);
    lv_anim_set_exec_cb(&translate_anim, DockTranslateAnim);
    lv_anim_set_path_cb(&translate_anim, lv_anim_path_ease_out);
    lv_anim_start(&translate_anim);
}

void BounceDockButton(lv_obj_t *button) {
    if (!button) return;

    lv_anim_delete(button, DockScaleAnim);
    lv_anim_t scale_anim;
    lv_anim_init(&scale_anim);
    lv_anim_set_var(&scale_anim, button);
    lv_anim_set_values(&scale_anim, kDockScaleNormal, 292);
    lv_anim_set_time(&scale_anim, 90);
    lv_anim_set_playback_time(&scale_anim, 140);
    lv_anim_set_exec_cb(&scale_anim, DockScaleAnim);
    lv_anim_set_path_cb(&scale_anim, lv_anim_path_ease_out);
    lv_anim_start(&scale_anim);

    lv_anim_delete(button, DockTranslateAnim);
    lv_anim_t translate_anim;
    lv_anim_init(&translate_anim);
    lv_anim_set_var(&translate_anim, button);
    lv_anim_set_values(&translate_anim, 0, -8);
    lv_anim_set_time(&translate_anim, 90);
    lv_anim_set_playback_time(&translate_anim, 140);
    lv_anim_set_exec_cb(&translate_anim, DockTranslateAnim);
    lv_anim_set_path_cb(&translate_anim, lv_anim_path_ease_out);
    lv_anim_start(&translate_anim);
}

// Filenames live in backgrounds.h (shared with the gallery).
} // namespace

namespace home {

Ds02HomeDisplay::Ds02HomeDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                                 int width, int height, int /*ox*/, int /*oy*/,
                                 bool /*mx*/, bool /*my*/, bool /*swap*/)
    : SpiLcdDisplay(panel_io, panel, width, height, 0, 0, false, false, false) {}

Ds02HomeDisplay::~Ds02HomeDisplay() {
    if (refresh_timer_) { esp_timer_stop(refresh_timer_); esp_timer_delete(refresh_timer_); }
    if (splash_) {
        lv_anim_delete(splash_, OnSplashOpa);
        lv_obj_del_async(splash_);
        splash_ = nullptr;
    }
}

void Ds02HomeDisplay::SetupUI() {
    if (setup_ui_called_) return;
    Display::SetupUI();
    DisplayLockGuard lock(this);

    root_ = lv_screen_active();
    lv_obj_clean(root_);
    lv_obj_set_style_bg_color(root_, Color(0x000000), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);

    CreateStandbyObjects();
    CreateDrawerObjects();
    CreateSystemBarObjects();
    CreateDockObjects();

    Settings s("display", false);
    background_index_ = (size_t)Clamp(s.GetInt("ds02_background", 0), 0, (int)kBackgroundCount - 1);
    text_color_ = (uint32_t)s.GetInt("ds02_text_color", (int32_t)0xffffff) & 0xffffff;
    ApplyBackgroundIndex(background_index_);
    SetTextColor(text_color_);

    ApplyStandbyState();

    esp_timer_create_args_t args = {
        .callback = OnRefreshTimer,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ds02_home",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&args, &refresh_timer_);
    esp_timer_start_periodic(refresh_timer_, kRefreshIntervalUs);

    RefreshClock();
    UpdateStatusBar(true);

    // Repaint the whole home screen when the light/dark theme flips.
    // Ds02HomeDisplay lives for the whole app lifetime, so capturing `this` is safe.
    jetson::UiTheme::Instance().Subscribe([this]() { RepaintForTheme(); });

    // Boot splash on top of the freshly-built home UI; fades out on its own.
    ShowOnboardSplash(2500);
}

void Ds02HomeDisplay::CreateStandbyObjects() {
    standby_layer_ = lv_obj_create(root_);
    lv_obj_remove_style_all(standby_layer_);
    lv_obj_set_size(standby_layer_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(standby_layer_, Color(0x1b2630), 0);
    lv_obj_set_style_bg_opa(standby_layer_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_color(standby_layer_, Color(0x8b7966), 0);
    lv_obj_set_style_bg_grad_dir(standby_layer_, LV_GRAD_DIR_VER, 0);
    lv_obj_clear_flag(standby_layer_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(standby_layer_, LV_OBJ_FLAG_CLICKABLE);

    wallpaper_image_obj_ = lv_image_create(standby_layer_);
    lv_obj_center(wallpaper_image_obj_);

    dim_overlay_ = lv_obj_create(standby_layer_);
    lv_obj_remove_style_all(dim_overlay_);
    lv_obj_set_size(dim_overlay_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(dim_overlay_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dim_overlay_, 0, 0); // awake by default
    lv_obj_clear_flag(dim_overlay_, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    // Big clock, top-right corner.
    time_label_ = lv_label_create(standby_layer_);
    lv_obj_set_style_text_font(time_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(time_label_, lv_color_white(), 0);
    lv_obj_align(time_label_, LV_ALIGN_TOP_RIGHT, -16, kSystemBarHeight + 12);

    // Weather + date, bottom-center.
    weather_label_ = lv_label_create(standby_layer_);
    lv_obj_set_style_text_font(weather_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(weather_label_, lv_color_white(), 0);
    lv_label_set_text(weather_label_, "");
    lv_obj_align(weather_label_, LV_ALIGN_BOTTOM_MID, 0, -(kDockHeight + kDockBottomMargin + 8));

    date_label_ = lv_label_create(standby_layer_);
    lv_obj_set_style_text_font(date_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(date_label_, lv_color_white(), 0);
    lv_obj_align(date_label_, LV_ALIGN_BOTTOM_MID, 0, -(kDockHeight + kDockBottomMargin + 40));

    // Chat subtitle (assistant / user messages).
    chat_label_ = lv_label_create(standby_layer_);
    lv_obj_set_style_text_font(chat_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(chat_label_, lv_color_white(), 0);
    lv_obj_set_width(chat_label_, width_ - 64);
    lv_label_set_long_mode(chat_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(chat_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(chat_label_, "");
    lv_obj_align(chat_label_, LV_ALIGN_BOTTOM_MID, 0, -(kDockHeight + kDockBottomMargin + 80));

    // Swipe up -> launcher, swipe down -> dim.
    lv_obj_add_event_cb(standby_layer_, OnStandbyGesture, LV_EVENT_GESTURE, this);
}

void Ds02HomeDisplay::CreateDrawerObjects() {
    const auto &p = jetson::UiTheme::Instance().Palette();
    launcher_layer_ = lv_obj_create(root_);
    lv_obj_remove_style_all(launcher_layer_);
    lv_obj_set_size(launcher_layer_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(launcher_layer_, Color(p.bg), 0);
    lv_obj_set_style_bg_opa(launcher_layer_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(launcher_layer_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(launcher_layer_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(launcher_layer_, OnStandbyGesture, LV_EVENT_GESTURE, this);

    // ---- App drawer grid (the DS-02 launcher content) ----
    struct AppDef { const char *icon; const char *label; int id; };
    static const AppDef kApps[kDrawerItemCount] = {
        {"assets/icon_2/drawer/agent.png",      "Agent",      0},
        {"assets/icon_2/drawer/chromium.png",    "Chromium",   1},
        {"assets/icon_2/drawer/git.png",         "Git",        2},
        {"assets/icon_2/drawer/minion.png",      "Minion",     3},
        {"assets/icon_2/drawer/nightowl.png",    "NightOwl",   4},
        {"assets/icon_2/drawer/photos.png",      "Ảnh",        5},
        {"assets/icon_2/drawer/teamspeak.png",   "TeamSpeak",  6},
        {"assets/icon_2/drawer/translate.png",   "Dịch",       7},
    };
    constexpr int kCols = 4;

    app_grid_ = lv_obj_create(launcher_layer_);
    lv_obj_remove_style_all(app_grid_);
    int grid_w = Clamp(width_ - 48, 360, 640);
    int grid_h = Clamp(height_ - 80, 320, 420);
    lv_obj_set_size(app_grid_, grid_w, grid_h);
    lv_obj_align(app_grid_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_pad_all(app_grid_, 12, 0);
    lv_obj_set_style_pad_row(app_grid_, 16, 0);
    lv_obj_set_style_pad_column(app_grid_, 16, 0);
    lv_obj_set_flex_flow(app_grid_, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(app_grid_, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(app_grid_, LV_OBJ_FLAG_SCROLLABLE);

    int cellW = (grid_w - 16 * (kCols - 1) - 24) / kCols;
    for (size_t i = 0; i < kDrawerItemCount; ++i) {
        auto *btn = lv_obj_create(app_grid_);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, cellW, 104);
        lv_obj_set_style_radius(btn, 16, 0);
        lv_obj_set_style_bg_color(btn, Color(p.row), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(btn, 6, 0);

        drawer_icon_cache_[i] = LvglImageFromFile(kApps[i].icon);
        if (drawer_icon_cache_[i]) {
            auto *icon = lv_image_create(btn);
            lv_image_set_src(icon, drawer_icon_cache_[i]->image_dsc());
            lv_image_set_scale(icon, (uint16_t)PngScaleToFit(kApps[i].icon, 52)); // ~52 px on the tile
            lv_obj_center(icon);
            lv_obj_clear_flag(icon, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
        } else {
            auto *fallback = lv_label_create(btn);
            lv_obj_set_style_text_font(fallback, &BUILTIN_ICON_FONT, 0);
            lv_obj_set_style_text_color(fallback, Color(p.text), 0);
            lv_label_set_text(fallback, LV_SYMBOL_IMAGE);
            lv_obj_center(fallback);
        }

        auto *lbl = lv_label_create(btn);
        lv_obj_set_style_text_font(lbl, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(lbl, Color(p.text), 0);
        lv_label_set_text(lbl, kApps[i].label);

        auto *ctx = new AppCtx{this, kApps[i].id};
        lv_obj_add_event_cb(btn, OnAppButtonClicked, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(btn, OnAppDeleted, LV_EVENT_DELETE, ctx);
    }
}

void Ds02HomeDisplay::OnAppButtonClicked(lv_event_t *e) {
    auto *ctx = static_cast<AppCtx *>(lv_event_get_user_data(e));
    auto *self = ctx->self;
    // Drawer apps are the upcoming app set; none has a backing view yet, so a
    // tap just announces "coming soon" rather than launching anything.
    self->ShowNotification("Sắp ra mắt", 1500);
}

void Ds02HomeDisplay::CreateSystemBarObjects() {
    system_bar_ = lv_obj_create(root_);
    lv_obj_remove_style_all(system_bar_);
    lv_obj_set_size(system_bar_, lv_pct(100), kSystemBarHeight);
    lv_obj_align(system_bar_, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(system_bar_, Color(0x000000), 0);
    lv_obj_set_style_bg_opa(system_bar_, LV_OPA_50, 0);
    lv_obj_set_style_pad_left(system_bar_, 12, 0);
    lv_obj_set_style_pad_right(system_bar_, 12, 0);
    lv_obj_clear_flag(system_bar_, LV_OBJ_FLAG_SCROLLABLE);

    wifi_label_ = lv_label_create(system_bar_);
    lv_obj_set_style_text_font(wifi_label_, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(wifi_label_, lv_color_white(), 0);
    lv_label_set_text(wifi_label_, FONT_AWESOME_WIFI);
    lv_obj_align(wifi_label_, LV_ALIGN_LEFT_MID, 0, 0);

    // Battery icon drawn from rectangles (DS-02 style).
    battery_icon_root_ = lv_obj_create(system_bar_);
    lv_obj_remove_style_all(battery_icon_root_);
    lv_obj_set_size(battery_icon_root_, 28, 16);
    lv_obj_clear_flag(battery_icon_root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(battery_icon_root_, LV_ALIGN_RIGHT_MID, 0, 0);

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

    // Percentage label just to the left of the battery icon (e.g. "87%").
    battery_percent_label_ = lv_label_create(system_bar_);
    lv_obj_set_style_text_font(battery_percent_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(battery_percent_label_, lv_color_white(), 0);
    lv_label_set_text(battery_percent_label_, "100%");
    lv_obj_align(battery_percent_label_, LV_ALIGN_RIGHT_MID, -32, 0);
    lv_obj_clear_flag(battery_percent_label_, LV_OBJ_FLAG_SCROLLABLE);

    // Reuse base status/notification labels in the system bar (left area).
    status_label_ = lv_label_create(system_bar_);
    lv_obj_set_style_text_font(status_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(status_label_, lv_color_white(), 0);
    lv_obj_align(status_label_, LV_ALIGN_LEFT_MID, 28, 0);
    notification_label_ = lv_label_create(system_bar_);
    lv_obj_set_style_text_font(notification_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(notification_label_, lv_color_white(), 0);
    lv_obj_align(notification_label_, LV_ALIGN_LEFT_MID, 28, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
}

void Ds02HomeDisplay::CreateDockObjects() {
    const int dock_width = Clamp(width_ - 180, 388, 520);
    dock_ = lv_obj_create(root_);
    lv_obj_remove_style_all(dock_);
    lv_obj_set_size(dock_, dock_width, kDockHeight);
    lv_obj_set_style_radius(dock_, 24, 0);
    lv_obj_set_style_bg_color(dock_, Color(0x202126), 0);
    lv_obj_set_style_bg_opa(dock_, LV_OPA_80, 0);
    lv_obj_set_style_border_width(dock_, 1, 0);
    lv_obj_set_style_border_color(dock_, lv_color_white(), 0);
    lv_obj_set_style_border_opa(dock_, LV_OPA_30, 0);
    lv_obj_set_style_shadow_color(dock_, lv_color_black(), 0);
    lv_obj_set_style_shadow_width(dock_, 18, 0);
    lv_obj_set_style_shadow_offset_y(dock_, 5, 0);
    lv_obj_set_style_shadow_opa(dock_, LV_OPA_40, 0);
    lv_obj_set_style_pad_left(dock_, 8, 0);
    lv_obj_set_style_pad_right(dock_, 8, 0);
    lv_obj_set_style_pad_top(dock_, 6, 0);
    lv_obj_set_style_pad_bottom(dock_, 6, 0);
    lv_obj_set_style_pad_column(dock_, 4, 0);
    lv_obj_set_flex_flow(dock_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dock_, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(dock_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(dock_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_align(dock_, LV_ALIGN_BOTTOM_MID, 0, -kDockBottomMargin);

    static const char *kIconFiles[kDockItemCount] = {
        "assets/icon_2/dock/calendar.png", "assets/icon_2/dock/folder.png",
        "assets/icon_2/dock/music.png", "assets/icon_2/dock/reminders.png",
        "assets/icon_2/dock/settings.png", "assets/icon_2/dock/siri.png",
        "assets/icon_2/dock/terminal.png",
    };
    static const char *kFallbackIcons[kDockItemCount] = {
        FONT_AWESOME_BOOK_OPEN, FONT_AWESOME_BOOK, FONT_AWESOME_MUSIC,
        FONT_AWESOME_MICROPHONE_LINES, FONT_AWESOME_GEAR,
        FONT_AWESOME_MICROPHONE, FONT_AWESOME_TERMINAL,
    };
    static constexpr uint32_t kTileColors[kDockItemCount] = {
        0x7357d9, 0xe04f5f, 0xe2aa36, 0x66707d, 0x29a58d, 0x3282d8, 0x3a3a3a,
    };
    static constexpr uint32_t kTileGradients[kDockItemCount] = {
        0x3e2c91, 0x8f2535, 0x8a5c14, 0x343a43, 0x126354, 0x184c91, 0x181818,
    };

    for (size_t i = 0; i < kDockItemCount; ++i) {
        auto *item = lv_obj_create(dock_);
        lv_obj_remove_style_all(item);
        lv_obj_set_size(item, kDockButtonSize + 2, kDockItemHeight);
        lv_obj_clear_flag(item, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
        lv_obj_add_flag(item, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

        auto *btn = lv_obj_create(item);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, kDockButtonSize, kDockButtonSize);
        lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_style_radius(btn, 14, 0);
        lv_obj_set_style_bg_color(btn, Color(kTileColors[i]), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_grad_color(btn, Color(kTileGradients[i]), 0);
        lv_obj_set_style_bg_grad_dir(btn, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, lv_color_white(), 0);
        lv_obj_set_style_border_opa(btn, LV_OPA_30, 0);
        lv_obj_set_style_shadow_color(btn, lv_color_black(), 0);
        lv_obj_set_style_shadow_width(btn, 8, 0);
        lv_obj_set_style_shadow_offset_y(btn, 3, 0);
        lv_obj_set_style_shadow_opa(btn, LV_OPA_40, 0);
        lv_obj_set_style_transform_pivot_x(btn, kDockButtonSize / 2, 0);
        lv_obj_set_style_transform_pivot_y(btn, kDockButtonSize, 0);
        lv_obj_set_style_transform_scale(btn, kDockScaleNormal, 0);
        lv_obj_set_ext_click_area(btn, 4);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

        dock_icon_cache_[i] = LvglImageFromFile(kIconFiles[i]);
        if (dock_icon_cache_[i]) {
            auto *icon = lv_image_create(btn);
            lv_image_set_src(icon, dock_icon_cache_[i]->image_dsc());
            lv_image_set_scale(icon, (uint16_t)PngScaleToFit(kIconFiles[i], 41)); // ~41 px on the dock.
            lv_obj_center(icon);
            lv_obj_clear_flag(icon, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
        } else {
            auto *fallback = lv_label_create(btn);
            lv_obj_set_style_text_font(fallback, &BUILTIN_ICON_FONT, 0);
            lv_obj_set_style_text_color(fallback, lv_color_white(), 0);
            lv_label_set_text(fallback, kFallbackIcons[i]);
            lv_obj_center(fallback);
        }

        auto *indicator = lv_obj_create(item);
        lv_obj_remove_style_all(indicator);
        lv_obj_set_size(indicator, 5, 5);
        lv_obj_set_style_radius(indicator, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(indicator, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(indicator, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_color(indicator, lv_color_black(), 0);
        lv_obj_set_style_shadow_width(indicator, 4, 0);
        lv_obj_set_style_shadow_opa(indicator, LV_OPA_40, 0);
        lv_obj_align(indicator, LV_ALIGN_BOTTOM_MID, 0, -1);
        lv_obj_clear_flag(indicator, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

        lv_obj_add_event_cb(btn, OnDockButtonEvent, LV_EVENT_ALL, this);
        dock_buttons_[i] = btn;
        dock_indicators_[i] = indicator;
    }
}

void Ds02HomeDisplay::SetDockActive(int index) {
    dock_active_index_ = index;
    for (size_t i = 0; i < kDockItemCount; ++i) {
        if (!dock_indicators_[i]) continue;
        lv_obj_set_style_bg_opa(dock_indicators_[i],
                                static_cast<int>(i) == index ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
}

void Ds02HomeDisplay::ApplyStandbyState() {
    if (!standby_layer_ || !launcher_layer_) return;
    switch (standby_state_) {
    case StandbyState::Dim:
        lv_obj_set_style_bg_opa(dim_overlay_, LV_OPA_60, 0);
        lv_obj_clear_flag(launcher_layer_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(launcher_layer_, LV_OBJ_FLAG_HIDDEN);
        break;
    case StandbyState::Awake:
        lv_obj_set_style_bg_opa(dim_overlay_, 0, 0);
        lv_obj_add_flag(launcher_layer_, LV_OBJ_FLAG_HIDDEN);
        break;
    case StandbyState::Launcher:
        lv_obj_set_style_bg_opa(dim_overlay_, 0, 0);
        lv_obj_clear_flag(launcher_layer_, LV_OBJ_FLAG_HIDDEN);
        break;
    }
}

void Ds02HomeDisplay::AdvanceStandbyButtonState() {
    DisplayLockGuard lock(this);
    switch (standby_state_) {
    case StandbyState::Dim: standby_state_ = StandbyState::Awake; break;
    case StandbyState::Awake: standby_state_ = StandbyState::Launcher; break;
    case StandbyState::Launcher: standby_state_ = StandbyState::Dim; break;
    }
    ApplyStandbyState();
}

void Ds02HomeDisplay::OnStandbyGesture(lv_event_t *e) {
    auto *self = static_cast<Ds02HomeDisplay *>(lv_event_get_user_data(e));
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    DisplayLockGuard lock(self);
    if (dir == LV_DIR_TOP) { self->standby_state_ = StandbyState::Launcher; }
    else if (dir == LV_DIR_BOTTOM) { self->standby_state_ = StandbyState::Awake; }
    else if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) { self->standby_state_ = StandbyState::Awake; }
    self->ApplyStandbyState();
}

void Ds02HomeDisplay::OnDockButtonEvent(lv_event_t *e) {
    auto *self = static_cast<Ds02HomeDisplay *>(lv_event_get_user_data(e));
    lv_obj_t *target = lv_event_get_current_target_obj(e);
    int focused = -1;
    for (size_t i = 0; i < kDockItemCount; ++i) {
        if (self->dock_buttons_[i] == target) {
            focused = static_cast<int>(i);
            break;
        }
    }
    if (focused < 0) return;

    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        for (size_t i = 0; i < kDockItemCount; ++i) {
            const int distance = std::abs(static_cast<int>(i) - focused);
            const int scale = distance == 0 ? kDockScaleFocused
                              : distance == 1 ? kDockScaleNeighbor
                                              : kDockScaleNormal;
            const int y = distance == 0 ? -10 : distance == 1 ? -3 : 0;
            AnimateDockButton(self->dock_buttons_[i], scale, y);
        }
        return;
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        for (auto *button : self->dock_buttons_) {
            AnimateDockButton(button, kDockScaleNormal, 0, 170);
        }
        return;
    }
    if (code != LV_EVENT_CLICKED) return;

    self->SetDockActive(focused);
    BounceDockButton(target);

    // Dock icons (in file order): calendar, folder, music, reminders, settings,
    // siri, terminal -> the 7 backed apps. music/reminders carry WiFi/BT since
    // they have no app of their own (matches the old gear/globe wiring).
    switch (focused) {
    case 0: self->OpenCalendar(); break;
    case 1: self->OpenBackgroundGallery(); break;
    case 2: self->OpenWifiSettings(); break;
    case 3: self->OpenBluetoothSettings(); break;
    case 4: self->OpenSettings(); break;
    case 5: self->OpenChat(); break;
    case 6: self->OpenTerminal(); break;
    default: self->AdvanceStandbyButtonState(); break;
    }
}

void Ds02HomeDisplay::OpenWifiSettings() {
    DisplayLockGuard lock(this);
    if (wifi_view_) return;
    if (!root_) root_ = lv_screen_active();
    wifi_view_ = std::make_shared<WifiSettingsView>(
        root_, width_, height_,
        [this]() { wifi_view_.reset(); });
    wifi_view_->Start();
}

void Ds02HomeDisplay::OpenBluetoothSettings() {
    DisplayLockGuard lock(this);
    if (bt_view_) return;
    if (!root_) root_ = lv_screen_active();
    bt_view_ = std::make_shared<BluetoothSettingsView>(
        root_, width_, height_,
        [this]() { bt_view_.reset(); });
    bt_view_->Start();
}

void Ds02HomeDisplay::OpenCalendar() {
    DisplayLockGuard lock(this);
    if (calendar_view_) return;
    if (!root_) root_ = lv_screen_active();
    calendar_view_ = std::make_shared<CalendarView>(
        root_, width_, height_,
        [this]() { calendar_view_.reset(); });
    calendar_view_->Start();
}

void Ds02HomeDisplay::OpenBackgroundGallery() {
    DisplayLockGuard lock(this);
    if (gallery_view_) return;
    if (!root_) root_ = lv_screen_active();
    gallery_view_ = std::make_shared<BackgroundGalleryView>(
        root_, width_, height_,
        [this]() { gallery_view_.reset(); });
    gallery_view_->SetCurrent(background_index_);
    gallery_view_->SetOnSelect([this](size_t index) { ApplyBackgroundIndexFromGallery(index); });
    gallery_view_->Start();
}

void Ds02HomeDisplay::OpenSettings() {
    DisplayLockGuard lock(this);
    if (settings_view_) return;
    if (!root_) root_ = lv_screen_active();
    settings_view_ = std::make_shared<SettingsView>(
        root_, width_, height_,
        [this]() { settings_view_.reset(); });
    settings_view_->Start();
}

void Ds02HomeDisplay::OpenChat() {
    DisplayLockGuard lock(this);
    if (chat_view_) return;
    if (!root_) root_ = lv_screen_active();
    if (!chat_conv_) {
        chat_conv_ = std::make_shared<jetson::Conversation>();
        chat_conv_->SetTools(jetson::BuildDefaultToolRegistry());
    }
    chat_view_ = std::make_shared<ChatView>(
        root_, width_, height_, chat_conv_,
        [this]() { chat_view_.reset(); });
    chat_view_->Start();
}

void Ds02HomeDisplay::OpenTerminal() {
    DisplayLockGuard lock(this);
    if (terminal_view_) return;
    if (!root_) root_ = lv_screen_active();
    terminal_view_ = std::make_shared<TerminalView>(
        root_, width_, height_,
        [this]() { terminal_view_.reset(); });
    terminal_view_->Start();
}

void Ds02HomeDisplay::ApplyBackgroundIndexFromGallery(size_t index) {
    DisplayLockGuard lock(this);
    if (ApplyBackgroundIndex(index)) {
        Settings s("display", false);
        s.SetInt("ds02_background", (int32_t)index);
    }
}

void Ds02HomeDisplay::OnAppDeleted(lv_event_t *e) {
    auto *ctx = static_cast<AppCtx *>(lv_event_get_user_data(e));
    delete ctx;
}

void Ds02HomeDisplay::RepaintForTheme() {
    DisplayLockGuard lock(this);
    const auto &p = jetson::UiTheme::Instance().Palette();
    if (standby_layer_) {
        lv_obj_set_style_bg_color(standby_layer_, Color(p.grad_top), 0);
        lv_obj_set_style_bg_grad_color(standby_layer_, Color(p.grad_bottom), 0);
    }
    if (launcher_layer_) lv_obj_set_style_bg_color(launcher_layer_, Color(p.bg), 0);
    if (app_grid_) {
        uint32_t n = lv_obj_get_child_cnt(app_grid_);
        for (uint32_t i = 0; i < n; ++i) {
            auto *btn = lv_obj_get_child(app_grid_, i);
            lv_obj_set_style_bg_color(btn, Color(p.row), 0);
            // icon = child 0, label = child 1
            auto *icon = lv_obj_get_child(btn, 0);
            auto *lbl = lv_obj_get_child(btn, 1);
            if (icon) lv_obj_set_style_text_color(icon, Color(p.text), 0);
            if (lbl) lv_obj_set_style_text_color(lbl, Color(p.text), 0);
        }
    }
    if (system_bar_) lv_obj_set_style_bg_color(system_bar_, Color(p.bar_bg), 0);
    if (dock_) lv_obj_set_style_bg_color(dock_, Color(p.dock_bg), 0);
}

const char *Ds02HomeDisplay::GetBackgroundFile(size_t index) const {
    return home::BackgroundFile(index);
}

LvglImage *Ds02HomeDisplay::GetBackgroundImage(size_t index) {
    if (index >= kBackgroundCount) return nullptr;
    if (background_image_cache_[index]) return background_image_cache_[index].get();
    std::string path = home::BackgroundsDir() + "/" + home::kBackgroundFiles[index];
    auto img = LvglImageFromFile(path);
    if (!img) return nullptr;
    background_image_cache_[index] = std::move(img);
    return background_image_cache_[index].get();
}

bool Ds02HomeDisplay::ApplyBackgroundIndex(size_t index) {
    background_index_ = index;
    LvglImage *img = GetBackgroundImage(index);
    if (!img || !wallpaper_image_obj_) {
        if (wallpaper_image_obj_) lv_image_set_src(wallpaper_image_obj_, nullptr);
        return false;
    }
    lv_image_set_src(wallpaper_image_obj_, img->image_dsc());
    // Cover-fit: scale the wallpaper to fill the 800x480 panel. Parse the PNG
    // IHDR directly -- image_dsc().header.w stays 0 until LVGL lazily decodes
    // the body, which left the wallpaper unscaled (shown too small).
    std::string path = home::BackgroundsDir() + "/" + home::BackgroundFile(index);
    int iw = 0, ih = 0;
    if (PngSize(path.c_str(), &iw, &ih) && iw > 0 && ih > 0) {
        int scale = std::max(width_ * 256 / iw, height_ * 256 / ih);
        lv_image_set_scale(wallpaper_image_obj_, (uint16_t)scale);
    }
    lv_obj_center(wallpaper_image_obj_);
    return true;
}

void Ds02HomeDisplay::SetTextColor(uint32_t color) {
    text_color_ = color;
    lv_color_t c = Color(color);
    for (lv_obj_t *lbl : {time_label_, date_label_, weather_label_, chat_label_}) {
        if (lbl) lv_obj_set_style_text_color(lbl, c, 0);
    }
}

std::string Ds02HomeDisplay::FormatTime(const struct tm &t) {
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M", &t);
    return buf;
}
std::string Ds02HomeDisplay::FormatDate(const struct tm &t) {
    char buf[40];
    std::strftime(buf, sizeof(buf), "%A, %d %B", &t);
    return buf;
}

void Ds02HomeDisplay::RefreshClock() {
    time_t now = std::time(nullptr);
    struct tm t = *std::localtime(&now);
    if (t.tm_year < (2025 - 1900)) return; // time not set yet
    std::string ts = FormatTime(t);
    std::string ds = FormatDate(t);
    if (ts != cached_time_ && time_label_) { lv_label_set_text(time_label_, ts.c_str()); cached_time_ = ts; }
    if (ds != cached_date_ && date_label_) { lv_label_set_text(date_label_, ds.c_str()); cached_date_ = ds; }
}

void Ds02HomeDisplay::RefreshBattery() {
    // Battery comes from the INA219 over I2C (Board::GetBatteryLevel). The two
    // 1 Hz timers call this ~2x/s; throttle the I2C read to once per ~5 s and
    // cache the result so the bus isn't hammered while the LVGL lock is held.
    constexpr auto kBatteryReadInterval = std::chrono::seconds(5);
    auto now = std::chrono::steady_clock::now();
    if (!battery_read_done_ || (now - last_battery_read_ >= kBatteryReadInterval)) {
        int level = 100; bool charging = false, discharging = false;
        Board::GetInstance().GetBatteryLevel(level, charging, discharging);
        cached_battery_level_ = Clamp(level, 0, 100);
        cached_battery_charging_ = charging;
        cached_battery_discharging_ = discharging;
        last_battery_read_ = now;
        battery_read_done_ = true;
    }

    int level = cached_battery_level_;
    if (battery_icon_fill_) {
        // Fill width 0..18 px maps to 0..100 %; the body is 24 px wide with a
        // 2 px inset on each side, so 18 px == full.
        lv_obj_set_size(battery_icon_fill_, 18 * level / 100, 10);
    }
    if (battery_percent_label_) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%d%%", level);
        lv_label_set_text(battery_percent_label_, buf);
    }
    (void)cached_battery_charging_;
    (void)cached_battery_discharging_;
}

void Ds02HomeDisplay::UpdateStatusBar(bool /*update_all*/) {
    DisplayLockGuard lock(this);
    RefreshClock();
    RefreshBattery();
}

void Ds02HomeDisplay::OnRefreshTimer(void *arg) {
    auto *self = static_cast<Ds02HomeDisplay *>(arg);
    self->UpdateStatusBar(false);
}

void Ds02HomeDisplay::SetStatus(const char *status) {
    DisplayLockGuard lock(this);
    if (status_label_) {
        lv_label_set_text(status_label_, status);
        lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        if (notification_label_) lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    }
}

void Ds02HomeDisplay::ShowNotification(const char *notification, int duration_ms) {
    LvglDisplay::ShowNotification(notification, duration_ms);
}

void Ds02HomeDisplay::SetChatMessage(const char *role, const char *content) {
    DisplayLockGuard lock(this);
    if (!chat_label_) return;
    if (role && std::strcmp(role, "system") == 0) { lv_label_set_text(chat_label_, ""); return; }
    lv_label_set_text(chat_label_, content ? content : "");
}

void Ds02HomeDisplay::SetEmotion(const char * /*emotion*/) {
    // Phase 1: no emotion GIF yet.
}

void Ds02HomeDisplay::SetTheme(Theme *theme) { Display::SetTheme(theme); }

void Ds02HomeDisplay::SetPowerSaveMode(bool on) {
    DisplayLockGuard lock(this);
    standby_state_ = on ? StandbyState::Dim : StandbyState::Awake;
    ApplyStandbyState();
}

void Ds02HomeDisplay::ShowOnboardSplash(int duration_ms) {
    // Full-screen boot splash drawn on top of the (already-built) home UI.
    // A progress bar fills over `duration_ms`, then the whole splash fades out
    // and self-destructs, revealing the standby/home screen beneath.
    // Must be called with the display lock held (as from SetupUI).
    if (duration_ms <= 0) return;
    if (splash_) lv_obj_del(splash_); // replace any prior splash

    splash_ = lv_obj_create(root_);
    lv_obj_remove_style_all(splash_);
    lv_obj_set_size(splash_, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(splash_, 0, 0);
    lv_obj_set_style_bg_color(splash_, Color(0x0b1a2f), 0);
    lv_obj_set_style_bg_opa(splash_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_color(splash_, Color(0x000000), 0);
    lv_obj_set_style_bg_grad_dir(splash_, LV_GRAD_DIR_VER, 0);
    lv_obj_clear_flag(splash_, LV_OBJ_FLAG_SCROLLABLE);
    // Swallow taps while the splash is up so the dock isn't poked mid-boot.
    lv_obj_add_flag(splash_, LV_OBJ_FLAG_CLICKABLE);

    // App logo (assets/icon_2/app/logo.png) as the boot mark.
    static const char *kLogoPath = "assets/icon_2/app/logo.png";
    splash_logo_ = LvglImageFromFile(kLogoPath);
    if (splash_logo_) {
        auto *logo = lv_image_create(splash_);
        lv_image_set_src(logo, splash_logo_->image_dsc());
        lv_image_set_scale(logo, (uint16_t)PngScaleToFit(kLogoPath, 130)); // ~130 px logo.
        lv_obj_align(logo, LV_ALIGN_CENTER, 0, -54);
        lv_obj_clear_flag(logo, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    }

    // Wordmark + accent underline.
    auto *title = lv_label_create(splash_);
    lv_obj_set_style_text_font(title, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_letter_space(title, 4, 0);
    lv_label_set_text(title, "DS-02");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 38);

    auto *underline = lv_obj_create(splash_);
    lv_obj_remove_style_all(underline);
    lv_obj_set_size(underline, 96, 3);
    lv_obj_set_style_bg_color(underline, Color(0x4aa3df), 0);
    lv_obj_set_style_bg_opa(underline, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(underline, 2, 0);
    lv_obj_align(underline, LV_ALIGN_CENTER, 0, 56);
    lv_obj_clear_flag(underline, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    auto *sub = lv_label_create(splash_);
    lv_obj_set_style_text_font(sub, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(sub, Color(0x9fb4c8), 0);
    lv_label_set_text(sub, "Jetson Nano  \xC2\xB7  AI Firmware");
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 72);

    // Progress bar near the bottom.
    auto *bar = lv_bar_create(splash_);
    lv_obj_set_size(bar, 320, 6);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -42);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_start_value(bar, 0, LV_ANIM_OFF);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, Color(0x1b3a5b), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 3, 0);
    lv_obj_set_style_bg_color(bar, Color(0x4aa3df), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);

    // Progress fill 0 -> 100 over the splash duration.
    lv_anim_t b;
    lv_anim_init(&b);
    lv_anim_set_var(&b, bar);
    lv_anim_set_values(&b, 0, 100);
    lv_anim_set_time(&b, duration_ms);
    lv_anim_set_exec_cb(&b, OnSplashBar);
    lv_anim_set_path_cb(&b, lv_anim_path_ease_in_out);
    lv_anim_set_early_apply(&b, true);
    lv_anim_start(&b);

    // Fade out after the bar finishes, then delete the splash subtree.
    lv_anim_t c;
    lv_anim_init(&c);
    lv_anim_set_var(&c, splash_);
    lv_anim_set_values(&c, 255, 0);
    lv_anim_set_delay(&c, duration_ms);
    lv_anim_set_time(&c, 500);
    lv_anim_set_exec_cb(&c, OnSplashOpa);
    lv_anim_set_path_cb(&c, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&c, OnSplashGone);
    lv_anim_set_user_data(&c, this);
    lv_anim_set_early_apply(&c, false);
    lv_anim_start(&c);
}

void Ds02HomeDisplay::OnSplashBar(void *var, int32_t v) {
    lv_bar_set_value((lv_obj_t *)var, v, LV_ANIM_OFF);
}

void Ds02HomeDisplay::OnSplashOpa(void *var, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

void Ds02HomeDisplay::OnSplashGone(lv_anim_t *a) {
    auto *self = static_cast<Ds02HomeDisplay *>(lv_anim_get_user_data(a));
    if (!self || !self->splash_) return;
    // Deferred delete: safe from within an anim completed callback.
    lv_obj_del_async(self->splash_);
    self->splash_ = nullptr;
    // splash_logo_ is intentionally kept alive until the async delete runs: the
    // lv_image inside splash_ still references its dsc until the subtree is gone.
}

} // namespace home
