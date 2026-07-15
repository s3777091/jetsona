#include "display/home/ds02_home_display.h"
#include "display/common/lvgl_utils.h"
#include "display/common/backgrounds.h"
#include "display/views/background_gallery_view.h"
#include "display/views/bluetooth_settings_view.h"
#include "display/views/calendar_view.h"
#include "display/views/chat_view.h"
#include "display/views/documents_view.h"
#include "display/views/lock_screen_view.h"
#include "display/views/settings_view.h"
#include "display/views/terminal_view.h"
#include "display/views/wifi_settings_view.h"
#include "agent/conversation.h"
#include "net/bluetooth_manager.h"
#include "net/wifi_manager.h"
#include "board.h"
#include "fonts.h"
#include "settings.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "font_awesome.h"
#include "display/theme/ui_theme.h"

#include <lvgl.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <fstream>
#include <string>

#define TAG "Ds02Home"

namespace {
int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
using jetson::ui::Color;

// Brightness (15..100) -> black-scrim opacity for the full-screen brightness
// overlay. The scrim is CAPPED at LV_OPA_70 (~70% black) so the lowest setting
// never makes the home screen unreadable -- otherwise a persisted-low brightness
// darkens the dock/menu bar so the user can't see to open Settings and raise it
// back (the pointer still works, but there's nothing visible to click).
lv_opa_t BrightnessToOpa(int pct) {
    int o = (100 - pct) * 255 / 100;
    if (o > LV_OPA_70) o = LV_OPA_70;
    return (lv_opa_t)o;
}

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
                                 bool /*mx*/, bool /*my*/, bool /*swap*/,
                                 jetson::IWifiManager &wifi,
                                 jetson::IBluetoothManager &bluetooth)
    : SpiLcdDisplay(panel_io, panel, width, height, 0, 0, false, false, false),
      wifi_(wifi), bluetooth_(bluetooth) {}

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

    // Brightness is emulated with a scrim because the HDMI panel has no
    // controllable backlight. Put it on LVGL's top layer so it also covers
    // full-screen app views (including Settings itself); a root_ child would
    // sit behind every app opened later and make the slider appear broken.
    // The system layer remains above it, keeping the mouse cursor visible.
    brightness_overlay_ = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(brightness_overlay_);
    lv_obj_set_size(brightness_overlay_, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(brightness_overlay_, 0, 0);
    lv_obj_set_style_bg_color(brightness_overlay_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(brightness_overlay_, 0, 0);
    lv_obj_clear_flag(brightness_overlay_,
                      (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    {
        int b = Settings("display").GetInt("brightness", 100);
        if (b < 15) b = 15;
        if (b > 100) b = 100;
        lv_obj_set_style_bg_opa(brightness_overlay_, BrightnessToOpa(b), 0);
    }

    Settings s("display", false);
    background_files_ = jetson::ui::backgrounds::ListBackgroundFiles();
    background_file_ = s.GetString("ds02_background_file", "");
    sleep_background_file_ = s.GetString("ds02_sleep_bg_file", "");
    // Fall back to the first available wallpaper if the saved one is gone
    // (e.g. deleted via the gallery) or none was set.
    bool bg_ok = !background_file_.empty();
    for (const auto &f : background_files_)
        if (f == background_file_) { bg_ok = true; break; }
    if (!bg_ok) background_file_ = background_files_.empty() ? "" : background_files_.front();
    text_color_ = (uint32_t)s.GetInt("ds02_text_color", (int32_t)0xffffff) & 0xffffff;
    ApplyBackgroundFile(background_file_);
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

    // The clock lives in the macOS-style menu bar (top-right cluster) built by
    // CreateSystemBarObjects; weather + date stay bottom-center.

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
    // No swipe gestures: the panel has no working touch, so standby state
    // changes only via the dock (finder icon toggles the app drawer).
}

void Ds02HomeDisplay::CreateDrawerObjects() {
    const auto &p = jetson::UiTheme::Instance().Palette();
    launcher_layer_ = lv_obj_create(root_);
    lv_obj_remove_style_all(launcher_layer_);
    lv_obj_set_size(launcher_layer_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(launcher_layer_, Color(p.bg), 0);
    lv_obj_set_style_bg_opa(launcher_layer_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(launcher_layer_, LV_OBJ_FLAG_SCROLLABLE);
    // Clickable so the drawer absorbs clicks on empty space (acts modal) and
    // doesn't pass them through to the standby layer behind it.
    lv_obj_add_flag(launcher_layer_, LV_OBJ_FLAG_CLICKABLE);

    // ---- App drawer grid (the DS-02 launcher content) ----
    struct AppDef { const char *icon; const char *label; int id; };
    static const AppDef kApps[kDrawerItemCount] = {
        {"assets/icons/drawer/agent.png",      "Agent",      0},
        {"assets/icons/drawer/chromium.png",    "Chromium",   1},
        {"assets/icons/drawer/git.png",         "Git",        2},
        {"assets/icons/drawer/minion.png",      "Minion",     3},
        {"assets/icons/drawer/nightowl.png",    "NightOwl",   4},
        {"assets/icons/drawer/photos.png",      "Ảnh",        5},
        {"assets/icons/drawer/teamspeak.png",   "TeamSpeak",  6},
        {"assets/icons/drawer/translate.png",   "Dịch",       7},
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
        lv_obj_set_size(btn, cellW, 84);
        // No "glass" tile behind the icon -- transparent, just a tap target.
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        drawer_icon_cache_[i] = LvglImageFromFile(kApps[i].icon);
        if (drawer_icon_cache_[i]) {
            auto *icon = lv_image_create(btn);
            lv_image_set_src(icon, drawer_icon_cache_[i]->image_dsc());
            lv_image_set_scale(icon, (uint16_t)PngScaleToFit(kApps[i].icon, 60)); // icon only, bigger
            lv_obj_center(icon);
            lv_obj_clear_flag(icon, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
        } else {
            auto *fallback = lv_label_create(btn);
            lv_obj_set_style_text_font(fallback, &BUILTIN_ICON_FONT, 0);
            lv_obj_set_style_text_color(fallback, Color(p.text), 0);
            lv_label_set_text(fallback, LV_SYMBOL_IMAGE);
            lv_obj_center(fallback);
        }

        auto *ctx = new AppCtx{this, kApps[i].id};
        lv_obj_add_event_cb(btn, OnAppButtonClicked, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(btn, OnAppDeleted, LV_EVENT_DELETE, ctx);
    }
}

void Ds02HomeDisplay::OnAppButtonClicked(lv_event_t *e) {
    auto *ctx = static_cast<AppCtx *>(lv_event_get_user_data(e));
    auto *self = ctx->self;
    ESP_LOGI(TAG, "drawer click: app_id=%d", ctx->id);
    // Most drawer apps are the upcoming app set with no backing view yet, so a
    // tap just announces "coming soon". The "Ảnh" tile (id 5) opens the
    // wallpaper gallery, which moved here from the dock so the dock's folder
    // icon can open the drawer itself.
    switch (ctx->id) {
    case 5: self->OpenBackgroundGallery(); break;
    default: self->ShowNotification("Sắp ra mắt", 1500); break;
    }
}

void Ds02HomeDisplay::CreateSystemBarObjects() {
    /* macOS-style menu bar: a compact pill pinned to the top-right corner
     * holding the status icons (wifi, bluetooth, battery, volume, power) and
     * the clock -- all in one corner, like the right side of the macOS menu
     * bar. Icons are clickable (wifi/bt open their settings, volume toggles
     * mute, power is a placeholder). Notifications/status show as a transient
     * toast centered just below the pill. */
    const int bar_h = 28;
    system_bar_ = lv_obj_create(root_);
    lv_obj_remove_style_all(system_bar_);
    lv_obj_set_size(system_bar_, LV_SIZE_CONTENT, bar_h);
    lv_obj_align(system_bar_, LV_ALIGN_TOP_RIGHT, -8, 6);
    lv_obj_set_style_bg_color(system_bar_, Color(0x000000), 0);
    lv_obj_set_style_bg_opa(system_bar_, LV_OPA_50, 0);
    lv_obj_set_style_radius(system_bar_, 14, 0);
    lv_obj_set_style_pad_left(system_bar_, 10, 0);
    lv_obj_set_style_pad_right(system_bar_, 10, 0);
    lv_obj_set_style_pad_top(system_bar_, 2, 0);
    lv_obj_set_style_pad_bottom(system_bar_, 2, 0);
    lv_obj_set_style_pad_column(system_bar_, 12, 0);
    lv_obj_set_flex_flow(system_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(system_bar_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(system_bar_, LV_OBJ_FLAG_SCROLLABLE);

    auto add_icon = [&](lv_obj_t **out, const char *glyph, lv_event_cb_t cb) {
        auto *l = lv_label_create(system_bar_);
        lv_obj_set_style_text_font(l, &BUILTIN_ICON_FONT, 0);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_label_set_text(l, glyph);
        lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
        if (cb) lv_obj_add_event_cb(l, cb, LV_EVENT_CLICKED, this);
        *out = l;
    };
    add_icon(&wifi_label_, FONT_AWESOME_WIFI, OnMenuWifi);
    add_icon(&bluetooth_label_, FONT_AWESOME_BLUETOOTH, OnMenuBluetooth);

    // Battery percent + drawn icon (DS-02 style, rectangles).
    battery_percent_label_ = lv_label_create(system_bar_);
    lv_obj_set_style_text_font(battery_percent_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(battery_percent_label_, lv_color_white(), 0);
    lv_label_set_text(battery_percent_label_, "100%");

    battery_icon_root_ = lv_obj_create(system_bar_);
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

    add_icon(&volume_label_, FONT_AWESOME_VOLUME_HIGH, OnMenuVolume);
    // The power/settings button that used to sit here was removed: it opened
    // Settings, which is already reachable from the dock's settings icon, so it
    // was redundant next to the clock. Reboot/shutdown live in Settings > Nguồn.

    // Clock (rightmost item in the pill).
    time_label_ = lv_label_create(system_bar_);
    lv_obj_set_style_text_font(time_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(time_label_, lv_color_white(), 0);
    lv_label_set_text(time_label_, "--:--");

    // Reuse the base class status/notification labels (created on the screen by
    // LvglDisplay) as a centered toast just below the menu bar.
    if (status_label_) lv_obj_align(status_label_, LV_ALIGN_TOP_MID, 0, bar_h + 8);
    if (notification_label_) {
        lv_obj_align(notification_label_, LV_ALIGN_TOP_MID, 0, bar_h + 8);
        lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    }
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
        "assets/icons/dock/finder.png", "assets/icons/dock/calendar.png",
        "assets/icons/dock/folder.png", "assets/icons/dock/music.png",
        "assets/icons/dock/reminders.png", "assets/icons/dock/settings.png",
        "assets/icons/dock/siri.png", "assets/icons/dock/terminal.png",
    };
    static const char *kFallbackIcons[kDockItemCount] = {
        FONT_AWESOME_FINDER, FONT_AWESOME_BOOK_OPEN, FONT_AWESOME_BOOK,
        FONT_AWESOME_MUSIC, FONT_AWESOME_MICROPHONE_LINES, FONT_AWESOME_GEAR,
        FONT_AWESOME_MICROPHONE, FONT_AWESOME_TERMINAL,
    };
    static constexpr uint32_t kTileColors[kDockItemCount] = {
        0x2b8cff, 0x7357d9, 0xe04f5f, 0xe2aa36, 0x66707d, 0x29a58d, 0x3282d8, 0x3a3a3a,
    };
    static constexpr uint32_t kTileGradients[kDockItemCount] = {
        0x125a9c, 0x3e2c91, 0x8f2535, 0x8a5c14, 0x343a43, 0x126354, 0x184c91, 0x181818,
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
        // A sleep-screen wallpaper (if set) replaces the desktop wallpaper
        // while dim; only a light scrim is drawn over it so it stays visible.
        ApplyWallpaperForState();
        lv_obj_set_style_bg_opa(dim_overlay_,
                                sleep_background_file_.empty() ? LV_OPA_60 : LV_OPA_20, 0);
        lv_obj_clear_flag(launcher_layer_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(launcher_layer_, LV_OBJ_FLAG_HIDDEN);
        break;
    case StandbyState::Awake:
        ApplyWallpaperForState();
        lv_obj_set_style_bg_opa(dim_overlay_, 0, 0);
        lv_obj_add_flag(launcher_layer_, LV_OBJ_FLAG_HIDDEN);
        break;
    case StandbyState::Launcher:
        ApplyWallpaperForState();
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

    static const char *kDockNames[] = {
        "finder", "calendar", "files", "wifi", "bluetooth", "settings", "chat", "terminal"
    };
    ESP_LOGI(TAG, "dock click: %s (index=%d)", kDockNames[focused], focused);

    // Dock icons (in file order): finder, calendar, folder, music, reminders,
    // settings, siri, terminal. The finder icon toggles the app drawer
    // open/closed (no touch swipe on this panel); the folder icon opens the
    // Documents file browser. music/reminders carry WiFi/BT; the wallpaper
    // gallery lives in the drawer's "Ảnh" tile.
    switch (focused) {
    case 0: // finder -> toggle app drawer
        self->standby_state_ = (self->standby_state_ == StandbyState::Launcher)
                                   ? StandbyState::Awake
                                   : StandbyState::Launcher;
        self->ApplyStandbyState();
        break;
    case 1: self->OpenCalendar(); break;
    case 2: self->OpenDocuments(); break;
    case 3: self->OpenWifiSettings(); break;
    case 4: self->OpenBluetoothSettings(); break;
    case 5: self->OpenSettings(); break;
    case 6: self->OpenChat(); break;
    case 7: self->OpenTerminal(); break;
    default: self->AdvanceStandbyButtonState(); break;
    }
}

void Ds02HomeDisplay::OpenWifiSettings() {
    DisplayLockGuard lock(this);
    if (wifi_view_) return;
    if (!root_) root_ = lv_screen_active();
    wifi_view_ = std::make_shared<WifiSettingsView>(
        root_, width_, height_, wifi_,
        [this]() { wifi_view_.reset(); });
    wifi_view_->Start();
}

void Ds02HomeDisplay::OpenBluetoothSettings() {
    DisplayLockGuard lock(this);
    if (bt_view_) return;
    if (!root_) root_ = lv_screen_active();
    bt_view_ = std::make_shared<BluetoothSettingsView>(
        root_, width_, height_, bluetooth_,
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

void Ds02HomeDisplay::OpenDocuments() {
    DisplayLockGuard lock(this);
    if (documents_view_) return;
    if (!root_) root_ = lv_screen_active();
    documents_view_ = std::make_shared<DocumentsView>(
        root_, width_, height_,
        [this]() { documents_view_.reset(); });
    documents_view_->Start();
}

void Ds02HomeDisplay::OpenBackgroundGallery() {
    DisplayLockGuard lock(this);
    if (gallery_view_) return;
    if (!root_) root_ = lv_screen_active();
    gallery_view_ = std::make_shared<BackgroundGalleryView>(
        root_, width_, height_,
        [this]() {
            // The set may have changed (deletes) while the gallery was open;
            // reload the runtime list and re-apply the current wallpaper.
            ReloadBackgrounds();
            gallery_view_.reset();
        });
    gallery_view_->SetOnSelect([this](const std::string &file) { ApplyBackgroundFromFile(file); });
    gallery_view_->SetOnSleep([this](const std::string &file) { SetSleepBackground(file); });
    gallery_view_->SetOnChanged([this]() { ReloadBackgrounds(); });
    gallery_view_->Start();
}

void Ds02HomeDisplay::OpenSettings() {
    DisplayLockGuard lock(this);
    if (settings_view_) return;
    if (!root_) root_ = lv_screen_active();
    settings_view_ = std::make_shared<SettingsView>(
        root_, width_, height_, wifi_, bluetooth_,
        [this]() { settings_view_.reset(); });
    // Wire the hub's controls back into the home UI: brightness dims the
    // whole panel via the scrim, volume toggles the menu-bar icon, and the
    // lock request raises the full-screen PIN lock.
    settings_view_->SetBrightnessApplier([this](int b) { SetBrightness(b); });
    settings_view_->SetVolumeApplier(
        [this](int v, bool muted) {
            volume_muted_ = muted;
            (void)v;
            if (volume_label_) {
                lv_label_set_text(volume_label_,
                                  muted ? FONT_AWESOME_VOLUME_XMARK
                                        : FONT_AWESOME_VOLUME_HIGH);
            }
        });
    settings_view_->SetLockRequest([this]() { OpenLockScreen(); });
    settings_view_->Start();
}

void Ds02HomeDisplay::SetBrightness(int pct) {
    DisplayLockGuard lock(this);
    if (pct < 15) pct = 15;
    if (pct > 100) pct = 100;
    if (brightness_overlay_) {
        lv_obj_set_style_bg_opa(brightness_overlay_, BrightnessToOpa(pct), 0);
    }
    Settings("display", true).SetInt("brightness", pct);
}

void Ds02HomeDisplay::OpenLockScreen() {
    DisplayLockGuard lock(this);
    if (lock_screen_view_) return;
    // No PIN set -> nothing to unlock against; nudge the user instead.
    if (Settings("system").GetString("pin", "").empty()) {
        ShowNotification("Chưa đặt PIN — mở Cài đặt > Nguồn & Bảo mật", 2500);
        return;
    }
    if (!root_) root_ = lv_screen_active();
    lock_screen_view_ = std::make_shared<LockScreenView>(
        root_, width_, height_, [this]() { lock_screen_view_.reset(); });
    lock_screen_view_->Start();
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

void Ds02HomeDisplay::ApplyBackgroundFromFile(const std::string &file) {
    DisplayLockGuard lock(this);
    if (ApplyBackgroundFile(file)) {
        Settings s("display", true);
        s.SetString("ds02_background_file", background_file_);
    }
}

void Ds02HomeDisplay::SetSleepBackground(const std::string &file) {
    DisplayLockGuard lock(this);
    sleep_background_file_ = file;
    Settings s("display", true);
    s.SetString("ds02_sleep_bg_file", file);
    if (standby_state_ == StandbyState::Dim) ApplyWallpaperForState();
}

void Ds02HomeDisplay::ReloadBackgrounds() {
    DisplayLockGuard lock(this);
    background_files_ = jetson::ui::backgrounds::ListBackgroundFiles();
    // Drop cache entries whose files were deleted.
    for (auto it = background_image_cache_.begin(); it != background_image_cache_.end();) {
        bool exists = false;
        for (const auto &f : background_files_) if (f == it->first) { exists = true; break; }
        if (!exists) it = background_image_cache_.erase(it);
        else ++it;
    }
    // If the current desktop wallpaper was deleted, fall back to the first.
    bool bg_ok = !background_file_.empty();
    for (const auto &f : background_files_) if (f == background_file_) { bg_ok = true; break; }
    if (!bg_ok) background_file_ = background_files_.empty() ? "" : background_files_.front();
    ApplyWallpaperForState();
}

void Ds02HomeDisplay::OnAppDeleted(lv_event_t *e) {
    auto *ctx = static_cast<AppCtx *>(lv_event_get_user_data(e));
    delete ctx;
}

void Ds02HomeDisplay::OnMenuWifi(lv_event_t *e) {
    auto *self = static_cast<Ds02HomeDisplay *>(lv_event_get_user_data(e));
    DisplayLockGuard lock(self);
    self->OpenWifiSettings();
}

void Ds02HomeDisplay::OnMenuBluetooth(lv_event_t *e) {
    auto *self = static_cast<Ds02HomeDisplay *>(lv_event_get_user_data(e));
    DisplayLockGuard lock(self);
    self->OpenBluetoothSettings();
}

void Ds02HomeDisplay::OnMenuVolume(lv_event_t *e) {
    auto *self = static_cast<Ds02HomeDisplay *>(lv_event_get_user_data(e));
    DisplayLockGuard lock(self);
    self->ToggleVolume();
}

void Ds02HomeDisplay::OnMenuPower(lv_event_t *e) {
    auto *self = static_cast<Ds02HomeDisplay *>(lv_event_get_user_data(e));
    DisplayLockGuard lock(self);
    self->OpenSettings();
}

void Ds02HomeDisplay::ToggleVolume() {
    volume_muted_ = !volume_muted_;
    if (volume_label_) {
        lv_label_set_text(volume_label_,
                          volume_muted_ ? FONT_AWESOME_VOLUME_XMARK
                                        : FONT_AWESOME_VOLUME_HIGH);
    }
    ShowNotification(volume_muted_ ? "Tắt tiếng" : "Bật tiếng", 1200);
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

LvglImage *Ds02HomeDisplay::GetBackgroundImage(const std::string &file) {
    if (file.empty()) return nullptr;
    auto it = background_image_cache_.find(file);
    if (it != background_image_cache_.end() && it->second) return it->second.get();
    std::string path = jetson::ui::backgrounds::BackgroundPath(file);
    auto img = LvglImageFromFile(path);
    if (!img) return nullptr;
    LvglImage *raw = img.get();
    background_image_cache_[file] = std::move(img);
    return raw;
}

bool Ds02HomeDisplay::ApplyBackgroundFile(const std::string &file) {
    if (!file.empty()) background_file_ = file;
    LvglImage *img = GetBackgroundImage(background_file_);
    if (!img || !wallpaper_image_obj_) {
        if (wallpaper_image_obj_) lv_image_set_src(wallpaper_image_obj_, nullptr);
        return false;
    }
    lv_image_set_src(wallpaper_image_obj_, img->image_dsc());
    // Backgrounds are pre-resized to the panel size on disk (800x480), so
    // display them 1:1 -- no runtime cover-fit scaling needed.
    lv_obj_center(wallpaper_image_obj_);
    return true;
}

// Choose which wallpaper to show based on standby state: the sleep-screen
// wallpaper (if set) when dim, otherwise the desktop wallpaper.
void Ds02HomeDisplay::ApplyWallpaperForState() {
    if (!wallpaper_image_obj_) return;
    std::string file = background_file_;
    if (standby_state_ == StandbyState::Dim && !sleep_background_file_.empty()) {
        // Only swap if the sleep file still exists on disk.
        bool exists = false;
        for (const auto &f : background_files_) if (f == sleep_background_file_) { exists = true; break; }
        if (exists) file = sleep_background_file_;
    }
    if (LvglImage *img = GetBackgroundImage(file)) {
        lv_image_set_src(wallpaper_image_obj_, img->image_dsc());
        lv_obj_center(wallpaper_image_obj_);
    } else {
        lv_image_set_src(wallpaper_image_obj_, nullptr);
    }
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
    bool h24 = Settings("display").GetBool("clock_24h", true);
    std::strftime(buf, sizeof(buf), h24 ? "%H:%M" : "%I:%M", &t);
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
    self->CheckIdleDim();

    // Memory leak watch: log our RSS every ~5 s so we can see when it grows
    // (calendar open? wifi scan? idle-dim swap?). Read /proc/self/status only
    // -- no image decoding, no allocations.
    static int tick = 0;
    if (++tick % 5 == 0) {
        std::ifstream f("/proc/self/status");
        std::string line;
        while (std::getline(f, line)) {
            if (line.rfind("VmRSS:", 0) == 0) {
                ESP_LOGI("Mem", "%s", line.c_str());
                break;
            }
        }
    }
}

void Ds02HomeDisplay::CheckIdleDim() {
    // Auto-dim the standby screen after the configured idle timeout. 0 = never.
    // Only fires from Awake; once dimmed, input activity wakes it back up via
    // the existing standby-button handling. App overlays are ignored (their own
    // UI keeps the screen alive and they sit above the dim scrim anyway).
    int timeout = Settings("display").GetInt("sleep_timeout", 0);
    if (timeout <= 0) return;
    DisplayLockGuard lock(this);
    if (standby_state_ != StandbyState::Awake) return;
    uint32_t idle_ms = lv_disp_get_inactive_time(nullptr);
    if (idle_ms >= (uint32_t)timeout * 1000u) {
        standby_state_ = StandbyState::Dim;
        ApplyStandbyState();
    }
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

    // App logo (assets/icons/app/logo.png) as the boot mark.
    static const char *kLogoPath = "assets/icons/app/logo.png";
    splash_logo_ = LvglImageFromFile(kLogoPath);
    if (splash_logo_) {
        auto *logo = lv_image_create(splash_);
        lv_image_set_src(logo, splash_logo_->image_dsc());
        lv_image_set_scale(logo, (uint16_t)PngScaleToFit(kLogoPath, 130)); // ~130 px logo.
        lv_obj_align(logo, LV_ALIGN_CENTER, 0, -30);
        lv_obj_clear_flag(logo, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    }

    // Splash is logo + progress bar only (no wordmark/subtitle text).

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
