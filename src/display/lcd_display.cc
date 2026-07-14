#include "lcd_display.h"
#include "lvgl_theme.h"
#include "lvgl_runtime.h"
#include "fonts.h"
#include "esp_log.h"

#include <cstring>

#define TAG "LcdDisplay"

LcdDisplay::LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                       int width, int height)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;
    display_ = jetson::LvglRuntime::Instance().display();
    /* Adopt the panel's actual resolution if the backend chose one. */
    if (display_) {
        int32_t w = lv_display_get_horizontal_resolution(display_);
        int32_t h = lv_display_get_vertical_resolution(display_);
        if (w > 0) width_ = (int)w;
        if (h > 0) height_ = (int)h;
    }
    InitializeLcdThemes();
}

LcdDisplay::~LcdDisplay() {}

void LcdDisplay::InitializeLcdThemes() {
    auto &mgr = LvglThemeManager::GetInstance();
    if (!current_theme_ && mgr.GetTheme("dark")) {
        SetTheme(mgr.GetTheme("dark"));
    }
}

bool LcdDisplay::Lock(int /*timeout_ms*/) {
    lv_lock();
    return true;
}

void LcdDisplay::Unlock() {
    lv_unlock();
}

void LcdDisplay::SetupUI() {
    Display::SetupUI();
    DisplayLockGuard lock(this);
    auto screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    /* Minimal status/notification labels so the base LvglDisplay setters work. */
    status_label_ = lv_label_create(screen);
    lv_obj_set_style_text_font(status_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(status_label_, lv_color_white(), 0);
    lv_obj_align(status_label_, LV_ALIGN_TOP_LEFT, 8, 4);

    notification_label_ = lv_label_create(screen);
    lv_obj_set_style_text_font(notification_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(notification_label_, lv_color_white(), 0);
    lv_obj_align(notification_label_, LV_ALIGN_TOP_LEFT, 8, 4);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
}

void LcdDisplay::SetEmotion(const char * /*emotion*/) {}
void LcdDisplay::SetChatMessage(const char * /*role*/, const char * /*content*/) {}
void LcdDisplay::ClearChatMessages() {}
void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> /*image*/) {}
void LcdDisplay::SetTheme(Theme *theme) { Display::SetTheme(theme); }
void LcdDisplay::SetHideSubtitle(bool hide) { hide_subtitle_ = hide; }

SpiLcdDisplay::SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                             int width, int height, int /*offset_x*/, int /*offset_y*/,
                             bool /*mirror_x*/, bool /*mirror_y*/, bool /*swap_xy*/)
    : LcdDisplay(panel_io, panel, width, height) {}