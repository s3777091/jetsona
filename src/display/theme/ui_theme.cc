#include "display/theme/ui_theme.h"
#include "settings.h"
#include "esp_log.h"

#include <lvgl.h>

#include <algorithm>

#define TAG "UiTheme"

namespace jetson {

namespace {

UiPalette DarkPalette() {
    UiPalette p;
    p.bg = 0x121417;
    p.panel = 0x1b1d22;
    p.row = 0x1c1c1e;
    p.row_active = 0x1e3a5f;
    p.header = 0x1b1d22;
    p.bar_bg = 0x000000;
    p.dock_bg = 0x1c1c1e;
    p.button = 0x2a2d33;
    p.text = 0xffffff;
    p.sub_text = 0x9aa0a6;
    p.accent = 0x4ea8ff;
    p.border = 0x2a2d33;
    p.scrim = 0x000000;
    p.grad_top = 0x1b2630;
    p.grad_bottom = 0x8b7966;
    return p;
}

UiPalette LightPalette() {
    UiPalette p;
    p.bg = 0xf5f6f8;
    p.panel = 0xffffff;
    p.row = 0xffffff;
    p.row_active = 0xd6e6ff;
    p.header = 0xffffff;
    p.bar_bg = 0xffffff;
    p.dock_bg = 0xe8eaed;
    p.button = 0xe8eaed;
    p.text = 0x1b1d22;
    p.sub_text = 0x5f6368;
    p.accent = 0x1a73e8;
    p.border = 0xdadce0;
    p.scrim = 0x101418;
    p.grad_top = 0xdfe7ef;
    p.grad_bottom = 0xb0a695;
    return p;
}

} // namespace

UiTheme &UiTheme::Instance() {
    static UiTheme inst;
    return inst;
}

UiTheme::UiTheme() {
    Load();
    Apply();
}

void UiTheme::Load() {
    // Light mode was removed -- the UI is dark-only. Ignore any "light" value
    // persisted from an older build so a stale setting can't flip the UI back.
    mode_ = UiMode::Dark;
}

void UiTheme::Save() {
    Settings s("ui", true);
    s.SetString("theme_mode", ModeName(mode_));
}

void UiTheme::Apply() {
    palette_ = (mode_ == UiMode::Dark) ? DarkPalette() : LightPalette();
}

void UiTheme::SetMode(UiMode m) {
    if (m == mode_) return;
    mode_ = m;
    Apply();
    Save();
    ESP_LOGI(TAG, "theme -> %s", ModeName(mode_));
    for (auto &cb : subs_) cb();
}

void UiTheme::Toggle() {
    SetMode(mode_ == UiMode::Dark ? UiMode::Light : UiMode::Dark);
}

void UiTheme::Subscribe(std::function<void()> cb) {
    subs_.push_back(std::move(cb));
}

} // namespace jetson
