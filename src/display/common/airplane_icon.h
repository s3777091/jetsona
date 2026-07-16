#pragma once

#include <lvgl.h>

namespace jetson::ui {

// Small code-native airplane silhouette used by Settings and the status bar.
// It avoids relying on U+2708, which is absent from the bundled Arial font.
lv_obj_t *CreateAirplaneIcon(lv_obj_t *parent, lv_color_t color);

} // namespace jetson::ui
