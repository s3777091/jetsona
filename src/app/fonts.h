#ifndef JETSON_APP_FONTS_H
#define JETSON_APP_FONTS_H

#include <lvgl.h>

namespace jetson {
extern lv_font_t *g_builtin_text_font;
extern lv_font_t *g_builtin_small_text_font;
extern lv_font_t *g_builtin_icon_font;

/* Load scalable TTF fonts from <assets_dir>/fonts/. Falls back to LVGL's
 * built-in montserrat font if the TTF is missing, so the firmware always
 * renders text. */
void InitBuiltinFonts(const char *assets_dir);
} // namespace jetson

/* The DS-02 UI references &BUILTIN_TEXT_FONT / &BUILTIN_ICON_FONT as
 * lv_font_t pointers. These macros resolve to the runtime tiny_ttf fonts. */
#define BUILTIN_TEXT_FONT (*::jetson::g_builtin_text_font)
#define BUILTIN_SMALL_TEXT_FONT (*::jetson::g_builtin_small_text_font)
#define BUILTIN_ICON_FONT (*::jetson::g_builtin_icon_font)

#endif
