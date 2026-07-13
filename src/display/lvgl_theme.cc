#include "lvgl_theme.h"
#include <cstdlib>

LvglTheme::LvglTheme(const std::string &name) : Theme(name) {}

lv_color_t LvglTheme::ParseColor(const std::string &color) {
    if (color.find("#") == 0 && color.size() >= 7) {
        uint8_t r = (uint8_t)std::strtol(color.substr(1, 2).c_str(), nullptr, 16);
        uint8_t g = (uint8_t)std::strtol(color.substr(3, 2).c_str(), nullptr, 16);
        uint8_t b = (uint8_t)std::strtol(color.substr(5, 2).c_str(), nullptr, 16);
        return lv_color_make(r, g, b);
    }
    return lv_color_black();
}

LvglThemeManager::LvglThemeManager() { InitializeDefaultThemes(); }

LvglTheme *LvglThemeManager::GetTheme(const std::string &name) {
    auto it = themes_.find(name);
    return it != themes_.end() ? it->second : nullptr;
}

void LvglThemeManager::RegisterTheme(const std::string &name, LvglTheme *theme) {
    themes_[name] = theme;
}

void LvglThemeManager::InitializeDefaultThemes() {
    /* A single dark theme for the DS-02 look. lcd_display may register more. */
    auto *dark = new LvglTheme("dark");
    dark->set_background_color(lv_color_make(0x1b, 0x26, 0x30));
    dark->set_text_color(lv_color_white());
    dark->set_chat_background_color(lv_color_make(0x12, 0x12, 0x12));
    dark->set_user_bubble_color(lv_color_make(0x1a, 0x73, 0xe8));
    dark->set_assistant_bubble_color(lv_color_make(0x30, 0x30, 0x30));
    dark->set_system_bubble_color(lv_color_make(0x20, 0x20, 0x20));
    dark->set_system_text_color(lv_color_white());
    dark->set_border_color(lv_color_make(0x40, 0x40, 0x40));
    dark->set_low_battery_color(lv_color_make(0xe0, 0x4a, 0x4a));
    RegisterTheme("dark", dark);
}