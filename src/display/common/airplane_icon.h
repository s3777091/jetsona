#pragma once

#include <lvgl.h>

namespace jetson::ui {

// Airplane silhouette used by Settings and the status bar. It prefers the
// reviewed `airplans` app asset and falls back to a code-native glyph when an
// older deployed asset bundle does not contain it yet.
lv_obj_t *CreateAirplaneIcon(lv_obj_t *parent, lv_color_t color);

} // namespace jetson::ui
