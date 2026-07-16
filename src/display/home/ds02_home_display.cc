#include "display/home/ds02_home_display.h"
#include "display/common/lvgl_utils.h"
#include "display/common/backgrounds.h"
#include "display/views/background_gallery_view.h"
#include "display/views/bluetooth_settings_view.h"
#include "display/views/calendar_view.h"
#include "display/views/chat_view.h"
#include "display/views/documents_view.h"
#include "display/views/lock_screen_view.h"
#include "display/views/pods_view.h"
#include "display/views/ps_remote_play_view.h"
#include "display/views/reminders_view.h"
#include "display/views/settings_view.h"
#include "display/views/terminal_view.h"
#include "display/views/trash_view.h"
#include "display/views/wifi_settings_view.h"
#include "agent/conversation.h"
#include "application.h"
#include "net/bluetooth_manager.h"
#include "net/weather_client.h"
#include "net/wifi_manager.h"
#include "board.h"
#include "fonts.h"
#include "lvgl_runtime.h"
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
#include <string>
#include <thread>
#include <unistd.h>

#define TAG "Ds02Home"

namespace {
int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
using jetson::ui::Color;

// Brightness (20..100) -> black-scrim opacity for the full-screen brightness
// overlay. At the requested 20% minimum this becomes 80% black; mapping the
// whole range linearly keeps every slider step visually meaningful.
lv_opa_t BrightnessToOpa(int pct) {
    pct = Clamp(pct, 20, 100);
    return (lv_opa_t)((100 - pct) * 255 / 100);
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
constexpr int kDockHeight = 74;
constexpr int kDockBottomMargin = 6;
constexpr int kDockButtonSize = 50;
constexpr int kDockItemHeight = 64;
constexpr int kDockHorizontalPadding = 10;
constexpr int kDockVerticalPadding = 4;
constexpr int kDockItemGap = 4;
constexpr int kDockBorderWidth = 1;
constexpr int kDockDividerWidth = 1;
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

    // A simulated screen-off surface above the home UI but below app overlays.
    // It consumes taps so an accidental touch cannot launch a dock app while
    // the display is black; tap-to-wake can explicitly restore the home screen.
    screen_off_overlay_ = lv_obj_create(root_);
    lv_obj_remove_style_all(screen_off_overlay_);
    lv_obj_set_size(screen_off_overlay_, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(screen_off_overlay_, 0, 0);
    lv_obj_set_style_bg_color(screen_off_overlay_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen_off_overlay_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen_off_overlay_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screen_off_overlay_,
                    (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN));
    lv_obj_add_event_cb(screen_off_overlay_, OnScreenOffClicked, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(screen_off_overlay_, OnScreenOffClicked, LV_EVENT_LONG_PRESSED, this);

    // True Tone / Night Shift color-temperature layer. It sits below the
    // brightness scrim so both effects compose across the complete UI.
    tone_overlay_ = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(tone_overlay_);
    lv_obj_set_size(tone_overlay_, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(tone_overlay_, 0, 0);
    lv_obj_set_style_bg_color(tone_overlay_, Color(0xffb05a), 0);
    lv_obj_set_style_bg_opa(tone_overlay_, 0, 0);
    lv_obj_clear_flag(tone_overlay_,
                      (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

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
        if (b < 20) b = 20;
        if (b > 100) b = 100;
        lv_obj_set_style_bg_opa(brightness_overlay_, BrightnessToOpa(b), 0);
    }

    ApplyDisplayPreferences();

    Settings s("display", true);
    background_files_ = jetson::ui::backgrounds::ListBackgroundFiles();
    background_file_ = s.GetString("ds02_background_file", "");
    sleep_background_file_ = s.GetString("ds02_sleep_bg_file", "");
    // Fall back to the first available wallpaper if the saved one is gone
    // (e.g. moved into Trash via the gallery) or none was set.
    bool bg_ok = false;
    for (const auto &f : background_files_)
        if (f == background_file_) { bg_ok = true; break; }
    if (!bg_ok) {
        background_file_ = background_files_.empty() ? "" : background_files_.front();
        s.SetString("ds02_background_file", background_file_);
    }
    if (!sleep_background_file_.empty() &&
        std::find(background_files_.begin(), background_files_.end(),
                  sleep_background_file_) == background_files_.end()) {
        sleep_background_file_.clear();
        s.SetString("ds02_sleep_bg_file", "");
    }
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

    UpdateStatusBar(true);

    // Repaint the whole home screen when the light/dark theme flips.
    // Ds02HomeDisplay lives for the whole app lifetime, so capturing `this` is safe.
    jetson::UiTheme::Instance().Subscribe([this]() { RepaintForTheme(); });

    StartWeatherUpdater();

    // Boot splash on top of the freshly-built home UI; fades out on its own.
    ShowOnboardSplash(2500);
}

void Ds02HomeDisplay::StartWeatherUpdater() {
    /* The standby weather line comes from open-meteo (free, keyless; location
     * via JETSON_WEATHER_LAT/LON). Blocking HTTP, so a detached worker loop:
     * fetch, marshal the text to the LVGL thread, sleep 30 min (5 min after a
     * failure so a flaky boot-time network recovers quickly). `this` lives for
     * the whole process, like the theme subscription above. */
    std::thread([this]() {
        for (;;) {
            jetson::WeatherInfo info;
            std::string err;
            const bool ok = jetson::WeatherClient::Fetch(info, err);
            if (ok) {
                std::string line = jetson::WeatherClient::FormatLine(info);
                Application::GetInstance().Schedule([this, line]() {
                    DisplayLockGuard lock(this);
                    if (weather_label_) lv_label_set_text(weather_label_, line.c_str());
                });
            } else {
                ESP_LOGW(TAG, "weather fetch failed: %s", err.c_str());
            }
            std::this_thread::sleep_for(std::chrono::minutes(ok ? 30 : 5));
        }
    }).detach();
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

    // The clock + weekday + date all live in the global StatusBar (Dynamic
    // Island) built by CreateSystemBarObjects, so the standby layer only needs
    // the weather line. The old big center "Wednesday, 15 July" date label was
    // redundant with the island and was removed.

    // Weather, bottom-center.
    weather_label_ = lv_label_create(standby_layer_);
    lv_obj_set_style_text_font(weather_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(weather_label_, lv_color_white(), 0);
    lv_label_set_text(weather_label_, "");
    lv_obj_align(weather_label_, LV_ALIGN_BOTTOM_MID, 0, -(kDockHeight + kDockBottomMargin + 8));

    // Chat subtitle (assistant / user messages).
    chat_label_ = lv_label_create(standby_layer_);
    lv_obj_set_style_text_font(chat_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(chat_label_, lv_color_white(), 0);
    lv_obj_set_width(chat_label_, width_ - 64);
    lv_label_set_long_mode(chat_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(chat_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(chat_label_, "");
    lv_obj_align(chat_label_, LV_ALIGN_BOTTOM_MID, 0, -(kDockHeight + kDockBottomMargin + 80));

    // "Tối ưu" system widget: live disk/RAM bars + drop-caches button. Its
    // toast reports how much RAM was freed via the Dynamic Island.
    optimize_widget_ = std::make_unique<OptimizeWidget>(standby_layer_);
    optimize_widget_->SetNotifyCb(
        [this](const char *msg) { ShowNotification(msg, 2500); });
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
        {"assets/icons/drawer/agent.png",       "Agent",      0},
        {"assets/icons/drawer/calculator.png",  "Máy tính",   8},
        {"assets/icons/drawer/chromium.png",    "Chromium",   1},
        {"assets/icons/drawer/game.png",        "Trò chơi",   9},
        {"assets/icons/drawer/github.png",      "GitHub",     2},
        {"assets/icons/drawer/minion.png",      "Minion",     3},
        {"assets/icons/drawer/nightowl.png",    "NightOwl",   4},
        {"assets/icons/drawer/photos.png",      "Ảnh",        5},
        {"assets/icons/drawer/pods.png",        "Pods",      10},
        {"assets/icons/drawer/record.png",      "Ghi âm",    11},
        {"assets/icons/drawer/studio.png",      "Studio",    12},
        {"assets/icons/drawer/translate.png",   "Dịch",       7},
    };
    constexpr int kCols = 4;
    static const int32_t kGridCols[] = {
        LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
        LV_GRID_TEMPLATE_LAST,
    };
    static const int32_t kGridRows[] = {
        LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST,
    };

    // Horizontal, snap-to-page drawer. Each child page contains a fixed 4x2
    // grid; swiping left/right moves exactly one page at a time.
    app_grid_ = lv_obj_create(launcher_layer_);
    lv_obj_remove_style_all(app_grid_);
    int grid_w = Clamp(width_ - 48, 360, 640);
    int grid_h = Clamp(height_ - 156, 250, 300);
    lv_obj_set_size(app_grid_, grid_w, grid_h);
    lv_obj_align(app_grid_, LV_ALIGN_CENTER, 0, -14);
    lv_obj_set_flex_flow(app_grid_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(app_grid_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(app_grid_, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(app_grid_, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(app_grid_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(app_grid_, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE |
                                               LV_OBJ_FLAG_SCROLL_ONE));
    lv_obj_add_event_cb(app_grid_, OnDrawerScrollEnd, LV_EVENT_SCROLL_END, this);

    for (size_t page_index = 0; page_index < kDrawerPageCount; ++page_index) {
        auto *page = lv_obj_create(app_grid_);
        drawer_pages_[page_index] = page;
        lv_obj_remove_style_all(page);
        lv_obj_set_size(page, grid_w, grid_h);
        lv_obj_set_grid_dsc_array(page, kGridCols, kGridRows);
        lv_obj_set_style_pad_all(page, 10, 0);
        lv_obj_set_style_pad_column(page, 10, 0);
        lv_obj_set_style_pad_row(page, 10, 0);
        lv_obj_clear_flag(page, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE |
                                                LV_OBJ_FLAG_CLICKABLE));
        lv_obj_add_flag(page, LV_OBJ_FLAG_SNAPPABLE);
    }

    int cellW = (grid_w - 20 - 10 * (kCols - 1)) / kCols;
    for (size_t i = 0; i < kDrawerItemCount; ++i) {
        const size_t page_index = i / kDrawerItemsPerPage;
        const size_t page_item = i % kDrawerItemsPerPage;
        auto *btn = lv_obj_create(drawer_pages_[page_index]);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, cellW, 104);
        lv_obj_set_grid_cell(btn, LV_GRID_ALIGN_STRETCH,
                             (int32_t)(page_item % kCols), 1,
                             LV_GRID_ALIGN_CENTER,
                             (int32_t)(page_item / kCols), 1);
        // No "glass" tile behind the icon -- transparent, just a tap target.
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(btn, 4, 0);

        drawer_icon_cache_[i] = LvglImageFromFile(kApps[i].icon);
        if (drawer_icon_cache_[i]) {
            auto *icon = lv_image_create(btn);
            lv_image_set_src(icon, drawer_icon_cache_[i]->image_dsc());
            lv_image_set_scale(icon, (uint16_t)PngScaleToFit(kApps[i].icon, 60)); // icon only, bigger
            lv_obj_clear_flag(icon, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
        } else {
            auto *fallback = lv_label_create(btn);
            lv_obj_set_style_text_font(fallback, &BUILTIN_ICON_FONT, 0);
            lv_obj_set_style_text_color(fallback, Color(p.text), 0);
            lv_label_set_text(fallback, LV_SYMBOL_IMAGE);
        }

        auto *label = lv_label_create(btn);
        lv_obj_set_width(label, cellW);
        lv_obj_set_style_text_font(label, &BUILTIN_SMALL_TEXT_FONT, 0);
        lv_obj_set_style_text_color(label, Color(p.text), 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_label_set_text(label, kApps[i].label);
        lv_obj_clear_flag(label, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

        auto *ctx = new AppCtx{this, kApps[i].id};
        lv_obj_add_event_cb(btn, OnAppButtonClicked, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(btn, OnAppDeleted, LV_EVENT_DELETE, ctx);
    }
    lv_obj_update_layout(app_grid_);
    lv_obj_update_snap(app_grid_, LV_ANIM_OFF);

    drawer_page_indicator_ = lv_obj_create(launcher_layer_);
    lv_obj_remove_style_all(drawer_page_indicator_);
    lv_obj_set_size(drawer_page_indicator_, (int32_t)kDrawerPageCount * 16, 12);
    lv_obj_align(drawer_page_indicator_, LV_ALIGN_BOTTOM_MID, 0,
                 -(kDockHeight + kDockBottomMargin + 10));
    lv_obj_set_flex_flow(drawer_page_indicator_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(drawer_page_indicator_, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(drawer_page_indicator_, 8, 0);
    lv_obj_clear_flag(drawer_page_indicator_,
                      (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    for (size_t i = 0; i < kDrawerPageCount; ++i) {
        auto *dot = lv_obj_create(drawer_page_indicator_);
        drawer_page_dots_[i] = dot;
        lv_obj_remove_style_all(dot);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_clear_flag(dot, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE |
                                               LV_OBJ_FLAG_CLICKABLE));
    }
    drawer_page_index_ = 0;
    UpdateDrawerPageDots();
}

void Ds02HomeDisplay::OnDrawerScrollEnd(lv_event_t *e) {
    auto *self = static_cast<Ds02HomeDisplay *>(lv_event_get_user_data(e));
    if (!self || !self->app_grid_) return;
    const int page_width = lv_obj_get_width(self->app_grid_);
    if (page_width <= 0) return;
    const int scroll_x = lv_obj_get_scroll_x(self->app_grid_);
    const int page = Clamp((scroll_x + page_width / 2) / page_width,
                           0, (int)kDrawerPageCount - 1);
    self->drawer_page_index_ = (size_t)page;
    self->UpdateDrawerPageDots();
}

void Ds02HomeDisplay::UpdateDrawerPageDots() {
    const auto &p = jetson::UiTheme::Instance().Palette();
    for (size_t i = 0; i < kDrawerPageCount; ++i) {
        auto *dot = drawer_page_dots_[i];
        if (!dot) continue;
        const bool active = i == drawer_page_index_;
        lv_obj_set_size(dot, active ? 8 : 6, active ? 8 : 6);
        lv_obj_set_style_bg_color(dot, Color(active ? p.text : p.sub_text), 0);
        lv_obj_set_style_bg_opa(dot, active ? LV_OPA_COVER : LV_OPA_50, 0);
    }
}

void Ds02HomeDisplay::OnAppButtonClicked(lv_event_t *e) {
    auto *ctx = static_cast<AppCtx *>(lv_event_get_user_data(e));
    auto *self = ctx->self;
    // Most drawer apps are the upcoming app set with no backing view yet, so a
    // tap just announces "coming soon". The "Ảnh" tile (id 5) opens the
    // wallpaper gallery, which moved here from the dock so the dock's folder
    // icon can open the drawer itself.
    switch (ctx->id) {
    case 1: self->OpenChromium(); break;        // drawer "Chromium" -> Xorg kiosk
    case 2: self->OpenChromium("https://github.com"); break; // GitHub in kiosk
    case 5: self->OpenBackgroundGallery(); break;
    case 9: self->OpenPsRemotePlay(); break;    // drawer "Trò chơi" -> PS5 Remote Play
    case 10: self->OpenPods(); break;           // RunPod GPU manager
    case 12: self->OpenStudio(); break;         // running pod's web IDE
    default: self->ShowNotification("Sắp ra mắt", 1500); break;
    }
}

void Ds02HomeDisplay::CreateSystemBarObjects() {
    /* The iPhone-like status row + Dynamic Island live on lv_layer_top() so
     * they render above every full-screen view. Time/status sit outside the
     * centered sensor pill; notifications morph that same pill. Created before
     * the brightness overlay so the software dimmer affects it too. */
    volume_muted_ = Settings("display").GetBool("muted", false);
    status_bar_ = std::make_unique<StatusBar>(lv_layer_top());
    status_bar_->SetWifiAction([this]() { OpenWifiSettings(); });
    status_bar_->SetBluetoothAction([this]() { OpenBluetoothSettings(); });
    status_bar_->SetLockAction([this]() { OpenLockScreen(); });
    // Reboot/shutdown shell out exactly like Settings > Power does (system()
    // on a detached thread). The power menu's open+tap is the 2-step confirm.
    status_bar_->SetRebootAction([]() {
        std::thread([]() { sync(); int r = std::system("reboot"); (void)r; }).detach();
    });
    status_bar_->SetShutdownAction([]() {
        std::thread([]() { sync(); int r = std::system("poweroff"); (void)r; }).detach();
    });
    // Clicking the resting island blooms the multitask switcher out of it
    // (and a second click while open collapses it back in).
    status_bar_->SetIslandAction([this]() { OpenAppSwitcher(); });

    // Reuse the base class status/notification labels (created on the screen by
    // LvglDisplay) for the device-state status line.
    const int bar_h = 42;
    // The device-state status ("Ready"/"Listening"/...) sits at the top-left
    // so it does not collide with the centered island as it expands. The old
    // base toast stays hidden because notifications now bloom inside the island.
    if (status_label_) lv_obj_align(status_label_, LV_ALIGN_TOP_LEFT, 8, bar_h + 8);
    if (notification_label_) {
        lv_obj_align(notification_label_, LV_ALIGN_TOP_LEFT, 8, bar_h + 8);
        lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    }
}

void Ds02HomeDisplay::CreateDockObjects() {
    // Size the strip from its contents so both ends have exactly the same
    // padding. A fixed centered row also avoids SPACE_EVENLY adding surplus
    // gaps that made the dock background look too wide and slightly offset.
    const int dock_items_width =
        static_cast<int>(kDockItemCount) * (kDockButtonSize + 2) +
        kDockDividerWidth +
        static_cast<int>(kDockItemCount) * kDockItemGap;
    const int preferred_dock_width = dock_items_width +
        2 * (kDockHorizontalPadding + kDockBorderWidth);
    const int dock_width = std::min(preferred_dock_width, width_ - 24);
    dock_base_width_ = dock_width; // widened while Gallery holds its temp slot
    dock_ = lv_obj_create(root_);
    lv_obj_remove_style_all(dock_);
    lv_obj_set_size(dock_, dock_width, kDockHeight);
    lv_obj_set_style_radius(dock_, 24, 0);
    lv_obj_set_style_bg_color(dock_, Color(0x202126), 0);
    lv_obj_set_style_bg_opa(dock_, LV_OPA_80, 0);
    lv_obj_set_style_border_width(dock_, kDockBorderWidth, 0);
    lv_obj_set_style_border_color(dock_, lv_color_white(), 0);
    lv_obj_set_style_border_opa(dock_, LV_OPA_30, 0);
    lv_obj_set_style_shadow_color(dock_, lv_color_black(), 0);
    lv_obj_set_style_shadow_width(dock_, 18, 0);
    lv_obj_set_style_shadow_offset_y(dock_, 5, 0);
    lv_obj_set_style_shadow_opa(dock_, LV_OPA_40, 0);
    lv_obj_set_style_pad_left(dock_, kDockHorizontalPadding, 0);
    lv_obj_set_style_pad_right(dock_, kDockHorizontalPadding, 0);
    lv_obj_set_style_pad_top(dock_, kDockVerticalPadding, 0);
    lv_obj_set_style_pad_bottom(dock_, kDockVerticalPadding, 0);
    lv_obj_set_style_pad_column(dock_, kDockItemGap, 0);
    lv_obj_set_flex_flow(dock_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dock_, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(dock_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(dock_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_align(dock_, LV_ALIGN_BOTTOM_MID, 0, -kDockBottomMargin);

    static const char *kIconFiles[kDockItemCount] = {
        "assets/icons/dock/finder.png", "assets/icons/dock/calendar.png",
        "assets/icons/dock/folder.png", "assets/icons/dock/music.png",
        "assets/icons/dock/reminders.png", "assets/icons/dock/settings.png",
        "assets/icons/dock/siri.png", "assets/icons/dock/terminal.png",
        "assets/icons/dock/trash.png",
    };
    static const char *kFallbackIcons[kDockItemCount] = {
        FONT_AWESOME_FINDER, FONT_AWESOME_BOOK_OPEN, FONT_AWESOME_BOOK,
        FONT_AWESOME_MUSIC, FONT_AWESOME_MICROPHONE_LINES, FONT_AWESOME_GEAR,
        FONT_AWESOME_MICROPHONE, FONT_AWESOME_TERMINAL, LV_SYMBOL_TRASH,
    };

    for (size_t i = 0; i < kDockItemCount; ++i) {
        // Keep Trash visually separate from the default application group,
        // matching the macOS Dock convention.
        if (i == kDockItemCount - 1) {
            auto *divider = lv_obj_create(dock_);
            lv_obj_remove_style_all(divider);
            lv_obj_set_size(divider, kDockDividerWidth, 40);
            lv_obj_set_style_bg_color(divider, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(divider, LV_OPA_30, 0);
            lv_obj_clear_flag(divider,
                              (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE |
                                              LV_OBJ_FLAG_CLICKABLE));
        }

        auto *item = lv_obj_create(dock_);
        lv_obj_remove_style_all(item);
        lv_obj_set_size(item, kDockButtonSize + 2, kDockItemHeight);
        lv_obj_clear_flag(item, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
        lv_obj_add_flag(item, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

        auto *btn = lv_obj_create(item);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, kDockButtonSize, kDockButtonSize);
        lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 0);
        /* No colored tile behind the icon -- the PNG/label sits directly on the
         * dock strip. The button is just a transparent, clickable hit area that
         * still carries the magnify transform. */
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
    Settings display("display", false);
    const bool always_on = display.GetBool("always_on", true);
    const bool show_wallpaper = display.GetBool("aod_wallpaper", true);
    const bool blur_wallpaper = display.GetBool("aod_blur", true);
    switch (standby_state_) {
    case StandbyState::Dim:
        // A sleep-screen wallpaper (if set) replaces the desktop wallpaper
        // while dim; only a light scrim is drawn over it so it stays visible.
        ApplyWallpaperForState();
        lv_obj_set_style_bg_opa(dim_overlay_,
                                show_wallpaper
                                    ? (sleep_background_file_.empty() ? LV_OPA_60 : LV_OPA_20)
                                    : LV_OPA_90,
                                0);
        if (wallpaper_image_obj_) {
            lv_obj_set_style_image_opa(wallpaper_image_obj_,
                                       show_wallpaper ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            lv_obj_set_style_image_recolor(wallpaper_image_obj_, Color(0x30343a), 0);
            lv_obj_set_style_image_recolor_opa(wallpaper_image_obj_,
                                               blur_wallpaper ? LV_OPA_30 : LV_OPA_TRANSP, 0);
        }
        lv_obj_clear_flag(launcher_layer_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(launcher_layer_, LV_OBJ_FLAG_HIDDEN);
        if (screen_off_overlay_) {
            lv_obj_set_style_bg_opa(screen_off_overlay_,
                                    always_on ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
            lv_obj_clear_flag(screen_off_overlay_, LV_OBJ_FLAG_HIDDEN);
        }
        if (status_bar_) {
            if (always_on) status_bar_->Show();
            else status_bar_->Hide();
        }
        break;
    case StandbyState::Awake:
        ApplyWallpaperForState();
        lv_obj_set_style_bg_opa(dim_overlay_, 0, 0);
        if (wallpaper_image_obj_) {
            lv_obj_set_style_image_opa(wallpaper_image_obj_, LV_OPA_COVER, 0);
            lv_obj_set_style_image_recolor_opa(wallpaper_image_obj_, LV_OPA_TRANSP, 0);
        }
        if (screen_off_overlay_) lv_obj_add_flag(screen_off_overlay_, LV_OBJ_FLAG_HIDDEN);
        if (status_bar_) status_bar_->Show();
        lv_obj_add_flag(launcher_layer_, LV_OBJ_FLAG_HIDDEN);
        break;
    case StandbyState::Launcher:
        ApplyWallpaperForState();
        lv_obj_set_style_bg_opa(dim_overlay_, 0, 0);
        if (wallpaper_image_obj_) {
            lv_obj_set_style_image_opa(wallpaper_image_obj_, LV_OPA_COVER, 0);
            lv_obj_set_style_image_recolor_opa(wallpaper_image_obj_, LV_OPA_TRANSP, 0);
        }
        if (screen_off_overlay_) lv_obj_add_flag(screen_off_overlay_, LV_OBJ_FLAG_HIDDEN);
        if (status_bar_) status_bar_->Show();
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

    // The dot under a dock icon now marks "running / in the multitask queue"
    // (see UpdateDockDots), not the last-clicked item.
    BounceDockButton(target);

    // Dock icons (in file order): finder, calendar, folder, music, reminders,
    // settings, siri, terminal, separator, trash. The finder icon toggles the app drawer
    // open/closed (no touch swipe on this panel); the folder icon opens the
    // Documents file browser. Music carries WiFi while Reminders opens its own
    // persistent task list; Bluetooth remains available from the status bar
    // and Settings. The wallpaper
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
    case 4: self->OpenReminders(); break;
    case 5: self->OpenSettings(); break;
    case 6: self->OpenChat(); break;
    case 7: self->OpenTerminal(); break;
    case 8: self->OpenTrash(); break;
    default: self->AdvanceStandbyButtonState(); break;
    }
}

void Ds02HomeDisplay::OpenWifiSettings() {
    DisplayLockGuard lock(this);
    OpenSettings();
    if (settings_view_) settings_view_->ShowWifiPage();
}

void Ds02HomeDisplay::OpenBluetoothSettings() {
    DisplayLockGuard lock(this);
    OpenSettings();
    if (settings_view_) settings_view_->ShowBluetoothPage();
}

void Ds02HomeDisplay::OpenCalendar() {
    DisplayLockGuard lock(this);
    if (calendar_view_) { RestoreApp(kAppCalendar); return; }
    if (!root_) root_ = lv_screen_active();
    calendar_view_ = std::make_shared<CalendarView>(
        root_, width_, height_,
        [this]() { calendar_view_.reset(); OnAppClosed(kAppCalendar); });
    calendar_view_->SetBackgroundRequest([this]() { BackgroundApp(kAppCalendar); });
    calendar_view_->Start();
    NoteAppOpened(kAppCalendar);
}

void Ds02HomeDisplay::OpenDocuments() {
    DisplayLockGuard lock(this);
    if (documents_view_) { RestoreApp(kAppDocuments); return; }
    if (!root_) root_ = lv_screen_active();
    documents_view_ = std::make_shared<DocumentsView>(
        root_, width_, height_,
        [this]() { documents_view_.reset(); OnAppClosed(kAppDocuments); });
    documents_view_->SetBackgroundRequest([this]() { BackgroundApp(kAppDocuments); });
    documents_view_->Start();
    NoteAppOpened(kAppDocuments);
}

void Ds02HomeDisplay::OpenReminders() {
    DisplayLockGuard lock(this);
    if (reminders_view_) { RestoreApp(kAppReminders); return; }
    if (!root_) root_ = lv_screen_active();
    reminders_view_ = std::make_shared<RemindersView>(
        root_, width_, height_,
        [this]() { reminders_view_.reset(); OnAppClosed(kAppReminders); });
    reminders_view_->SetBackgroundRequest([this]() { BackgroundApp(kAppReminders); });
    reminders_view_->Start();
    NoteAppOpened(kAppReminders);
}

void Ds02HomeDisplay::OpenBackgroundGallery() {
    DisplayLockGuard lock(this);
    if (gallery_view_) { RestoreApp(kAppGallery); return; }
    if (!root_) root_ = lv_screen_active();
    gallery_view_ = std::make_shared<BackgroundGalleryView>(
        root_, width_, height_,
        [this]() {
            // The set may have changed (items moved to Trash) while the gallery was open;
            // reload the runtime list and re-apply the current wallpaper.
            ReloadBackgrounds();
            gallery_view_.reset();
            OnAppClosed(kAppGallery);
        });
    gallery_view_->SetOnSelect([this](const std::string &file) { ApplyBackgroundFromFile(file); });
    gallery_view_->SetOnSleep([this](const std::string &file) { SetSleepBackground(file); });
    gallery_view_->SetOnChanged([this]() { ReloadBackgrounds(); });
    gallery_view_->SetBackgroundRequest([this]() { BackgroundApp(kAppGallery); });
    gallery_view_->Start();
    NoteAppOpened(kAppGallery);
}

void Ds02HomeDisplay::OpenSettings() {
    DisplayLockGuard lock(this);
    if (settings_view_) { RestoreApp(kAppSettings); return; }
    if (!root_) root_ = lv_screen_active();
    settings_view_ = std::make_shared<SettingsView>(
        root_, width_, height_, wifi_, bluetooth_,
        [this]() { settings_view_.reset(); OnAppClosed(kAppSettings); });
    // Wire the hub's controls back into the home UI: brightness dims the
    // whole panel via the scrim, volume toggles the menu-bar icon, and the
    // lock request raises the full-screen PIN lock.
    settings_view_->SetBrightnessApplier([this](int b) { SetBrightness(b); });
    settings_view_->SetDisplayPreferencesApplier([this]() { ApplyDisplayPreferences(); });
    settings_view_->SetVolumeApplier(
        [this](int v, bool muted) {
            volume_muted_ = muted;
            (void)v;
            // The global StatusBar reads the persisted "muted" flag and updates
            // its own icon, so nothing to touch here.
        });
    settings_view_->SetLockRequest([this]() { OpenLockScreen(); });
    settings_view_->SetNotificationApplier(
        [this](const char *message, int duration_ms) {
            ShowNotification(message, duration_ms);
        });
    settings_view_->SetBackgroundRequest([this]() { BackgroundApp(kAppSettings); });
    settings_view_->Start();
    NoteAppOpened(kAppSettings);
}

void Ds02HomeDisplay::SetBrightness(int pct) {
    DisplayLockGuard lock(this);
    if (pct < 20) pct = 20;
    if (pct > 100) pct = 100;
    if (brightness_overlay_) {
        lv_obj_set_style_bg_opa(brightness_overlay_, BrightnessToOpa(pct), 0);
    }
    Settings("display", true).SetInt("brightness", pct);
}

void Ds02HomeDisplay::ApplyDisplayPreferences() {
    DisplayLockGuard lock(this);
    Settings display("display", false);
    const bool night_shift = display.GetBool("night_shift", false);
    const bool true_tone = display.GetBool("true_tone", false);
    const int warmth = Clamp(display.GetInt("night_warmth", 55), 0, 100);
    if (tone_overlay_) {
        // True Tone is intentionally subtle. Night Shift uses the selected
        // warmth and overrides it with a stronger amber layer.
        const int opacity = night_shift ? (18 + warmth * 52 / 100)
                                         : (true_tone ? 16 : 0);
        lv_obj_set_style_bg_color(tone_overlay_,
                                  Color(night_shift ? 0xff9b3d : 0xffd3a0), 0);
        lv_obj_set_style_bg_opa(tone_overlay_, (lv_opa_t)opacity, 0);
    }
    ApplyStandbyState();
}

void Ds02HomeDisplay::OnScreenOffClicked(lv_event_t *e) {
    auto *self = static_cast<Ds02HomeDisplay *>(lv_event_get_user_data(e));
    if (!self) return;
    const bool touch_to_wake = Settings("display", false).GetBool("touch_to_wake", true);
    // A long press remains an escape hatch when tap-to-wake is disabled, since
    // this panel has no separate hardware wake key wired into the application.
    if (!touch_to_wake && lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    DisplayLockGuard lock(self);
    self->standby_state_ = StandbyState::Awake;
    self->ApplyStandbyState();
    lv_disp_trig_activity(nullptr);
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
    // The global bar lives above the lock overlay (it's on lv_layer_top()), so
    // hide it while locked -- otherwise its clickable wifi/bt icons would let
    // someone escape the lock screen.
    if (status_bar_) status_bar_->Hide();
    lock_screen_view_ = std::make_shared<LockScreenView>(
        root_, width_, height_, [this]() {
            lock_screen_view_.reset();
            if (status_bar_) status_bar_->Show();
        });
    lock_screen_view_->Start();
}

void Ds02HomeDisplay::OpenChat() {
    DisplayLockGuard lock(this);
    if (chat_view_) { RestoreApp(kAppChat); return; }
    if (!root_) root_ = lv_screen_active();
    if (!chat_conv_) {
        chat_conv_ = std::make_shared<jetson::Conversation>();
        chat_conv_->SetTools(jetson::BuildDefaultToolRegistry());
    }
    chat_view_ = std::make_shared<ChatView>(
        root_, width_, height_, chat_conv_,
        [this]() { chat_view_.reset(); OnAppClosed(kAppChat); });
    chat_view_->SetBackgroundRequest([this]() { BackgroundApp(kAppChat); });
    chat_view_->Start();
    NoteAppOpened(kAppChat);
}

void Ds02HomeDisplay::OpenTerminal() {
    DisplayLockGuard lock(this);
    if (terminal_view_) { RestoreApp(kAppTerminal); return; }
    if (!root_) root_ = lv_screen_active();
    terminal_view_ = std::make_shared<TerminalView>(
        root_, width_, height_,
        [this]() { terminal_view_.reset(); OnAppClosed(kAppTerminal); });
    terminal_view_->SetBackgroundRequest([this]() { BackgroundApp(kAppTerminal); });
    terminal_view_->Start();
    NoteAppOpened(kAppTerminal);
}

void Ds02HomeDisplay::OpenPsRemotePlay() {
    DisplayLockGuard lock(this);
    if (ps_remote_play_view_) { RestoreApp(kAppPsRemotePlay); return; }
    if (!root_) root_ = lv_screen_active();
    ps_remote_play_view_ = std::make_shared<PsRemotePlayView>(
        root_, width_, height_,
        [this]() {
            ps_remote_play_view_.reset();
            OnAppClosed(kAppPsRemotePlay);
        });
    ps_remote_play_view_->SetLaunchRequest(
        [this](bool configure) { LaunchPsRemotePlay(configure); });
    ps_remote_play_view_->SetBackgroundRequest(
        [this]() { BackgroundApp(kAppPsRemotePlay); });
    ps_remote_play_view_->Start();
    NoteAppOpened(kAppPsRemotePlay);
}

void Ds02HomeDisplay::LaunchPsRemotePlay(bool configure) {
    ShowNotification(configure ? "Đang mở thiết lập PS5..."
                               : "Đang kết nối PS5...", 1200);
    std::thread([configure]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        jetson::LvglRuntime::Instance().Stop();  // release /dev/fb0 completely
        sync();
        const int request_code = configure ? 43 : 44;
        if (std::getenv("JETSON_FW_SUPERVISED"))
            _exit(request_code);

        const char *launcher = "/opt/jetson-fw/scripts/launch_ps_remote_play.sh";
        if (::access(launcher, R_OK) != 0)
            launcher = "scripts/launch_ps_remote_play.sh";
        const char *mode = configure ? "configure" : "stream";
        const std::string command = std::string("bash '") + launcher + "' " + mode;
        const int launch_result = ::system(command.c_str());
        if (launch_result != 0)
            ESP_LOGW(TAG, "PS Remote Play launcher exited with status %d", launch_result);
        ::execl("/proc/self/exe", "jetson_fw", (char *)nullptr);
        _exit(1);
    }).detach();
}

void Ds02HomeDisplay::OpenPods() {
    DisplayLockGuard lock(this);
    if (pods_view_) { RestoreApp(kAppPods); return; }
    if (!root_) root_ = lv_screen_active();
    pods_view_ = std::make_shared<PodsView>(
        root_, width_, height_,
        [this]() { pods_view_.reset(); OnAppClosed(kAppPods); });
    pods_view_->SetNotifyCb([this](const char *msg) { ShowNotification(msg, 2500); });
    pods_view_->SetOpenUrlCb([this](const std::string &url) { OpenChromium(url); });
    pods_view_->SetBackgroundRequest([this]() { BackgroundApp(kAppPods); });
    pods_view_->Start();
    NoteAppOpened(kAppPods);
}

void Ds02HomeDisplay::OpenStudio() {
    /* Studio = the self-hosted code-server on the VM (JETSON_STUDIO_URL,
     * deployed once by vm/code-server/deploy.py). Always. GPU pods are an
     * explicit, paid choice: rent one in the Pods app and open its IDE from
     * there ("Mở Studio" on the pod), or SSH into it from this VM studio's
     * terminal (the SSH command is shown in the pod's detail sheet). No
     * RunPod calls here — the tile must work instantly and offline from
     * any GPU account state. */
    const char *vm_studio = std::getenv("JETSON_STUDIO_URL");
    if (!vm_studio || !vm_studio[0]) {
        ShowNotification("Chưa cấu hình JETSON_STUDIO_URL — chạy vm/code-server/deploy.py", 3500);
        return;
    }
    ShowNotification("Đang mở Studio trên VM...", 1500);
    OpenChromium(vm_studio);
}

void Ds02HomeDisplay::OpenChromium(const std::string &url) {
    /* Hand the panel to a Chromium kiosk. We can't run a browser inside the
     * firmware (it owns /dev/fb0 via FBDEV with no display server), so the
     * deal is: stop the LVGL render threads (no more writes to the framebuffer),
     * then exit the process with code 42. The jetson-fw supervisor
     * (scripts/jetson_fw_run.sh) sees 42, runs scripts/launch_chromium.sh to
     * start Xorg + chromium --kiosk on the HDMI panel, and restarts this
     * firmware once the browser closes -- re-acquiring /dev/fb0 fresh.
     *
     * Stop() joins the LVGL tick + handler threads, so it must NOT run on the
     * LVGL handler thread (we're inside an event callback here). Sleep briefly
     * so the click feedback paints, then stop + exit from a detached thread.
     *
     * The exit-42 contract only works under the supervisor (jetson-fw service /
     * jetson_fw_run.sh, which sets JETSON_FW_SUPERVISED=1). When the firmware
     * was started directly (sudo ./jetson_fw, run_fbdev.sh) exiting would just
     * kill the UI, so in that case we run the kiosk ourselves and re-exec this
     * binary afterwards to re-acquire /dev/fb0. */
    if (::system("command -v chromium-browser >/dev/null 2>&1 || "
                 "command -v chromium >/dev/null 2>&1") != 0) {
        ShowNotification("Chromium chưa được cài: sudo apt install chromium-browser", 3500);
        return;
    }
    ShowNotification("Đang mở Chromium...", 1200);
    // Hand the start URL to launch_chromium.sh through a file: the supervisor
    // path (exit 42) can't inherit our environment.
    if (!url.empty()) {
        if (FILE *f = std::fopen("/tmp/jetson_chromium_url", "w")) {
            std::fputs(url.c_str(), f);
            std::fclose(f);
        }
    } else {
        ::unlink("/tmp/jetson_chromium_url"); // stale URL from a previous run
    }
    std::thread([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        jetson::LvglRuntime::Instance().Stop();  // release /dev/fb0 writes
        sync();
        if (std::getenv("JETSON_FW_SUPERVISED"))
            _exit(42);  // 42 -> supervisor launches the kiosk, then restarts us
        const char *kiosk = "/opt/jetson-fw/scripts/launch_chromium.sh";
        if (::access(kiosk, R_OK) != 0) kiosk = "scripts/launch_chromium.sh";
        const int launch_result = ::system((std::string("bash ") + kiosk).c_str());
        if (launch_result != 0)
            ESP_LOGW(TAG, "Chromium launcher exited with status %d", launch_result);
        ::execl("/proc/self/exe", "jetson_fw", (char *)nullptr);
        _exit(1);  // execl failed; nothing left to render with
    }).detach();
}

void Ds02HomeDisplay::OpenTrash() {
    DisplayLockGuard lock(this);
    if (trash_view_) { RestoreApp(kAppTrash); return; }
    if (!root_) root_ = lv_screen_active();
    trash_view_ = std::make_shared<TrashView>(
        root_, width_, height_,
        [this]() { trash_view_.reset(); OnAppClosed(kAppTrash); });
    trash_view_->SetOnChanged([this]() { ReloadBackgrounds(); });
    trash_view_->SetBackgroundRequest([this]() { BackgroundApp(kAppTrash); });
    trash_view_->Start();
    NoteAppOpened(kAppTrash);
}

/* ---- Multitasking ------------------------------------------------------
 * LRU queue of live OverlayView apps (max kMaxRunningApps). Background apps
 * are hidden, not destroyed: LVGL skips hidden subtrees entirely, so 5 warm
 * apps cost zero render time and switching back is instant (no rebuild, no
 * reload). Opening a 6th app closes the least recently used one. */

OverlayView *Ds02HomeDisplay::GetAppView(AppId id) const {
    switch (id) {
    case kAppCalendar:  return calendar_view_.get();
    case kAppDocuments: return documents_view_.get();
    case kAppReminders: return reminders_view_.get();
    case kAppSettings:  return settings_view_.get();
    case kAppChat:      return chat_view_.get();
    case kAppTerminal:  return terminal_view_.get();
    case kAppTrash:     return trash_view_.get();
    case kAppGallery:   return gallery_view_.get();
    case kAppPsRemotePlay: return ps_remote_play_view_.get();
    case kAppPods:      return pods_view_.get();
    default:            return nullptr;
    }
}

void Ds02HomeDisplay::NoteAppOpened(AppId id) {
    // The previous foreground app steps aside but stays warm in the queue.
    if (foreground_app_ != kAppNone && foreground_app_ != id) {
        SnapshotApp(foreground_app_);
        if (auto *v = GetAppView(foreground_app_)) v->SetHidden(true);
    }
    task_queue_.erase(std::remove(task_queue_.begin(), task_queue_.end(), id),
                      task_queue_.end());
    task_queue_.push_back(id);
    foreground_app_ = id;
    // Over capacity -> evict the least recently used app (front of the queue;
    // the app just opened sits at the back, so it is never the victim).
    if (task_queue_.size() > kMaxRunningApps) CloseApp(task_queue_.front());
    if (id == kAppGallery) AddGalleryDockItem();
    UpdateDockDots();
}

void Ds02HomeDisplay::RestoreApp(AppId id) {
    auto *view = GetAppView(id);
    if (!view) return;
    if (foreground_app_ != kAppNone && foreground_app_ != id) {
        SnapshotApp(foreground_app_);
        if (auto *v = GetAppView(foreground_app_)) v->SetHidden(true);
    }
    view->SetHidden(false); // warm restore: the view was never torn down
    task_queue_.erase(std::remove(task_queue_.begin(), task_queue_.end(), id),
                      task_queue_.end());
    task_queue_.push_back(id);
    foreground_app_ = id;
    UpdateDockDots();
}

void Ds02HomeDisplay::BackgroundApp(AppId id) {
    auto *view = GetAppView(id);
    if (!view) return;
    SnapshotApp(id); // fresh thumbnail for the switcher card
    view->SetHidden(true);
    if (foreground_app_ == id) foreground_app_ = kAppNone;
    UpdateDockDots();
}

void Ds02HomeDisplay::CloseApp(AppId id) {
    // RequestClose hides immediately and fires the view's close callback on a
    // deferred one-shot timer; that callback resets the member and calls
    // OnAppClosed(id), which removes the queue entry and the dock dot.
    if (auto *view = GetAppView(id)) view->RequestClose();
}

void Ds02HomeDisplay::OnAppClosed(AppId id) {
    task_queue_.erase(std::remove(task_queue_.begin(), task_queue_.end(), id),
                      task_queue_.end());
    FreeSnapshot(id);
    if (foreground_app_ == id) foreground_app_ = kAppNone;
    if (id == kAppGallery) RemoveGalleryDockItem();
    UpdateDockDots();
}

void Ds02HomeDisplay::SnapshotApp(AppId id) {
    auto *view = GetAppView(id);
    if (!view || !view->overlay_obj()) return;
    // Only visible overlays render a meaningful shot; a hidden app keeps the
    // thumbnail captured when it went to the background.
    if (lv_obj_has_flag(view->overlay_obj(), LV_OBJ_FLAG_HIDDEN)) return;
    lv_draw_buf_t *shot = lv_snapshot_take(view->overlay_obj(), LV_COLOR_FORMAT_ARGB8888);
    if (!shot) return;
    FreeSnapshot(id);
    app_snapshots_[id] = shot;
}

void Ds02HomeDisplay::FreeSnapshot(AppId id) {
    auto it = app_snapshots_.find(id);
    if (it == app_snapshots_.end()) return;
    lv_draw_buf_destroy(it->second);
    app_snapshots_.erase(it);
}

void Ds02HomeDisplay::UpdateDockDots() {
    auto running = [this](AppId id) {
        return std::find(task_queue_.begin(), task_queue_.end(), id) != task_queue_.end();
    };
    static constexpr struct { int dock_index; AppId app; } kDockApps[] = {
        {1, kAppCalendar}, {2, kAppDocuments}, {4, kAppReminders}, {5, kAppSettings},
        {6, kAppChat},     {7, kAppTerminal},  {8, kAppTrash},
    };
    for (const auto &m : kDockApps) {
        if (!dock_indicators_[m.dock_index]) continue;
        lv_obj_set_style_bg_opa(dock_indicators_[m.dock_index],
                                running(m.app) ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
}

void Ds02HomeDisplay::OpenAppSwitcher() {
    DisplayLockGuard lock(this);
    if (app_switcher_) { app_switcher_->Dismiss(); return; } // island click toggles
    if (lock_screen_view_) return;
    if (task_queue_.empty()) {
        ShowNotification("Không có ứng dụng đang chạy", 1500);
        return;
    }
    // Park the foreground app first so its switcher card is up to date and the
    // switcher blooms over the home screen, mirroring the iPhone gesture.
    if (foreground_app_ != kAppNone) BackgroundApp(foreground_app_);

    auto icon_for = [this](AppId id, const char **path) -> const LvglImage * {
        switch (id) {
        case kAppCalendar:  *path = "assets/icons/dock/calendar.png"; return dock_icon_cache_[1].get();
        case kAppDocuments: *path = "assets/icons/dock/folder.png";   return dock_icon_cache_[2].get();
        case kAppReminders: *path = "assets/icons/dock/reminders.png"; return dock_icon_cache_[4].get();
        case kAppSettings:  *path = "assets/icons/dock/settings.png"; return dock_icon_cache_[5].get();
        case kAppChat:      *path = "assets/icons/dock/siri.png";     return dock_icon_cache_[6].get();
        case kAppTerminal:  *path = "assets/icons/dock/terminal.png"; return dock_icon_cache_[7].get();
        case kAppTrash:     *path = "assets/icons/dock/trash.png";    return dock_icon_cache_[8].get();
        case kAppGallery:   *path = "assets/icons/drawer/photos.png"; return drawer_icon_cache_[kGalleryDrawerIndex].get();
        case kAppPsRemotePlay: *path = "assets/icons/drawer/game.png"; return drawer_icon_cache_[3].get();
        case kAppPods:      *path = "assets/icons/drawer/pods.png";   return drawer_icon_cache_[kPodsDrawerIndex].get();
        default:            *path = nullptr; return nullptr;
        }
    };

    std::vector<AppSwitcher::Card> cards; // most recent first
    for (auto it = task_queue_.rbegin(); it != task_queue_.rend(); ++it) {
        AppSwitcher::Card c;
        c.app_id = *it;
        if (auto *v = GetAppView(*it)) c.title = v->title();
        auto snap = app_snapshots_.find(*it);
        if (snap != app_snapshots_.end()) c.snapshot = snap->second;
        const char *path = nullptr;
        const LvglImage *icon = icon_for(*it, &path);
        if (icon && path) {
            c.icon_src = icon->image_dsc();
            c.icon_scale = (uint16_t)PngScaleToFit(path, 22);
        }
        cards.push_back(std::move(c));
    }

    const int cx = status_bar_ ? status_bar_->IslandCenterX() : width_ / 2;
    const int cy = status_bar_ ? status_bar_->IslandCenterY() : 21;
    app_switcher_ = std::make_unique<AppSwitcher>(
        root_, width_, height_, std::move(cards), cx, cy,
        [this](int id) { RestoreApp(static_cast<AppId>(id)); },
        [this](int id) { CloseApp(static_cast<AppId>(id)); },
        [this]() { app_switcher_.reset(); }); // fired via deferred timer
}

void Ds02HomeDisplay::AddGalleryDockItem() {
    if (gallery_dock_item_ || !dock_) return;
    // One extra slot; the flex row reflows, the strip stays centered.
    if (dock_base_width_ > 0) {
        lv_obj_set_width(dock_, std::min(dock_base_width_ + kDockButtonSize + 2 + kDockItemGap,
                                         width_ - 24));
    }
    auto *item = lv_obj_create(dock_);
    lv_obj_remove_style_all(item);
    lv_obj_set_size(item, kDockButtonSize + 2, kDockItemHeight);
    lv_obj_clear_flag(item, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    // Insert into the app group, before the divider+Trash pair at the end.
    lv_obj_move_to_index(item, (int32_t)lv_obj_get_child_count(dock_) - 3);

    auto *btn = lv_obj_create(item);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, kDockButtonSize, kDockButtonSize);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_ext_click_area(btn, 4);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, OnGalleryDockClicked, LV_EVENT_CLICKED, this);

    if (drawer_icon_cache_[kGalleryDrawerIndex]) { // "Ảnh" drawer tile icon
        auto *icon = lv_image_create(btn);
        lv_image_set_src(icon, drawer_icon_cache_[kGalleryDrawerIndex]->image_dsc());
        lv_image_set_scale(icon, (uint16_t)PngScaleToFit("assets/icons/drawer/photos.png", 41));
        lv_obj_center(icon);
        lv_obj_clear_flag(icon, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    } else {
        auto *fallback = lv_label_create(btn);
        lv_obj_set_style_text_font(fallback, &BUILTIN_ICON_FONT, 0);
        lv_obj_set_style_text_color(fallback, lv_color_white(), 0);
        lv_label_set_text(fallback, LV_SYMBOL_IMAGE);
        lv_obj_center(fallback);
    }

    // Running dot -- always lit: the slot only exists while Gallery runs.
    auto *dot = lv_obj_create(item);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 5, 5);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_align(dot, LV_ALIGN_BOTTOM_MID, 0, -1);
    lv_obj_clear_flag(dot, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    gallery_dock_item_ = item;
}

void Ds02HomeDisplay::RemoveGalleryDockItem() {
    if (!gallery_dock_item_) return;
    lv_obj_del(gallery_dock_item_);
    gallery_dock_item_ = nullptr;
    if (dock_ && dock_base_width_ > 0) lv_obj_set_width(dock_, dock_base_width_);
}

void Ds02HomeDisplay::OnGalleryDockClicked(lv_event_t *e) {
    auto *self = static_cast<Ds02HomeDisplay *>(lv_event_get_user_data(e));
    if (self) self->OpenBackgroundGallery(); // running -> instant warm restore
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

    auto is_available = [this](const std::string &file) {
        return std::find(background_files_.begin(), background_files_.end(), file) !=
               background_files_.end();
    };
    const bool background_missing = !is_available(background_file_);
    const bool sleep_background_missing =
        !sleep_background_file_.empty() && !is_available(sleep_background_file_);

    // Detach LVGL before freeing image descriptors for files moved to Trash.
    if ((background_missing || sleep_background_missing) && wallpaper_image_obj_) {
        lv_image_set_src(wallpaper_image_obj_, nullptr);
    }

    // Drop cache entries whose files were moved to Trash.
    for (auto it = background_image_cache_.begin(); it != background_image_cache_.end();) {
        if (!is_available(it->first)) it = background_image_cache_.erase(it);
        else ++it;
    }

    Settings settings("display", true);
    if (background_missing) {
        background_file_ = background_files_.empty() ? "" : background_files_.front();
        settings.SetString("ds02_background_file", background_file_);
    }
    if (sleep_background_missing) {
        sleep_background_file_.clear();
        settings.SetString("ds02_sleep_bg_file", "");
    }
    ApplyWallpaperForState();
}

void Ds02HomeDisplay::OnAppDeleted(lv_event_t *e) {
    auto *ctx = static_cast<AppCtx *>(lv_event_get_user_data(e));
    delete ctx;
}

void Ds02HomeDisplay::ToggleVolume() {
    volume_muted_ = !volume_muted_;
    // Persist so the global StatusBar (which reads "muted") reflects the toggle
    // within ~1 s. The bar owns the icon now; no label to update here.
    Settings("display", true).SetBool("muted", volume_muted_);
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
        const uint32_t page_count = lv_obj_get_child_cnt(app_grid_);
        for (uint32_t page_index = 0; page_index < page_count; ++page_index) {
            auto *page = lv_obj_get_child(app_grid_, page_index);
            const uint32_t button_count = lv_obj_get_child_cnt(page);
            for (uint32_t i = 0; i < button_count; ++i) {
                auto *btn = lv_obj_get_child(page, i);
                // icon/fallback = child 0, app name = child 1
                auto *icon = lv_obj_get_child(btn, 0);
                auto *label = lv_obj_get_child(btn, 1);
                if (icon) lv_obj_set_style_text_color(icon, Color(p.text), 0);
                if (label) lv_obj_set_style_text_color(label, Color(p.text), 0);
            }
        }
        UpdateDrawerPageDots();
    }
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
    for (lv_obj_t *lbl : {weather_label_, chat_label_}) {
        if (lbl) lv_obj_set_style_text_color(lbl, c, 0);
    }
}

void Ds02HomeDisplay::UpdateStatusBar(bool /*update_all*/) {
    DisplayLockGuard lock(this);
    // The clock + date live in the global StatusBar (Dynamic Island), which
    // refreshes its own time/battery/lang on its 1s lv_timer. Nothing date-
    // related remains on the standby layer after the center date label was
    // removed, so there is no per-tick clock work to do here.
}

void Ds02HomeDisplay::OnRefreshTimer(void *arg) {
    auto *self = static_cast<Ds02HomeDisplay *>(arg);
    self->UpdateStatusBar(false);
    self->CheckIdleDim();
}

void Ds02HomeDisplay::CheckIdleDim() {
    // Auto-dim the standby screen after the configured idle timeout. 0 = never.
    // Only fires from Awake; once dimmed, input activity wakes it back up via
    // the existing standby-button handling. App overlays are ignored (their own
    // UI keeps the screen alive and they sit above the dim scrim anyway).
    int timeout = Settings("display").GetInt("sleep_timeout", 0);
    if (timeout <= 0) return;
    DisplayLockGuard lock(this);
    if (HasOpenOverlay()) return;
    if (standby_state_ != StandbyState::Awake) return;
    uint32_t idle_ms = lv_disp_get_inactive_time(nullptr);
    if (idle_ms >= (uint32_t)timeout * 1000u) {
        standby_state_ = StandbyState::Dim;
        ApplyStandbyState();
    }
}

bool Ds02HomeDisplay::HasOpenOverlay() const {
    // Background (hidden) multitask apps do not keep the screen awake -- only
    // a visible foreground app, the switcher, or the non-queue overlays do.
    return wifi_view_ || bt_view_ || lock_screen_view_ ||
           foreground_app_ != kAppNone || app_switcher_ != nullptr;
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
    if (standby_state_ == StandbyState::Dim) {
        Settings display("display", false);
        if (!display.GetBool("always_on", true) ||
            !display.GetBool("aod_notifications", true)) return;
    }
    // Toasts now bloom from the Dynamic Island instead of using the old
    // centered base-class label.
    if (status_bar_) status_bar_->ShowNotification(notification, duration_ms);
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
    // The greeting owns the whole boot moment. Reveal the status row and bloom
    // the welcome island only after the splash has gone.
    if (status_bar_) status_bar_->Hide();

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
    lv_obj_t *logo = nullptr;
    if (splash_logo_) {
        logo = lv_image_create(splash_);
        lv_image_set_src(logo, splash_logo_->image_dsc());
        lv_image_set_scale(logo, (uint16_t)PngScaleToFit(kLogoPath, 130)); // ~130 px logo.
        lv_obj_align(logo, LV_ALIGN_CENTER, 0, -30);
        lv_obj_clear_flag(logo, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    }

    // Fade the logo in gently; the welcome copy belongs exclusively to the
    // Dynamic Island after loading completes.
    auto fade_in = [](lv_obj_t *obj, uint32_t delay, uint32_t time) {
        if (!obj) return;
        lv_obj_set_style_opa(obj, LV_OPA_0, 0);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, obj);
        lv_anim_set_values(&a, 0, 255);
        lv_anim_set_delay(&a, delay);
        lv_anim_set_time(&a, time);
        lv_anim_set_exec_cb(&a, OnSplashOpa);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    };
    fade_in(logo, 100, 500);

    // Progress bar near the bottom.
    auto *bar = lv_bar_create(splash_);
    lv_obj_set_size(bar, 280, 6);
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
    if (self->status_bar_) {
        self->status_bar_->Show();
        self->status_bar_->ShowWelcome(4000);
    }
    // splash_logo_ is intentionally kept alive until the async delete runs: the
    // lv_image inside splash_ still references its dsc until the subtree is gone.
}

} // namespace home
