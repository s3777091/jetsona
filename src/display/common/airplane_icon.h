#pragma once

#include <lvgl.h>

namespace jetson::ui {

// Small code-native airplane silhouette used by Settings and the status bar.
// It avoids depending on a font glyph or the runtime PNG decoder.
lv_obj_t *CreateAirplaneIcon(lv_obj_t *parent, lv_color_t color);

} // namespace jetson::ui
