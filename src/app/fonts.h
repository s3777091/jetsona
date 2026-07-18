#ifndef JETSON_APP_FONTS_H
#define JETSON_APP_FONTS_H

#include <lvgl.h>
#include <string>

namespace jetson {
extern lv_font_t *g_builtin_text_font;
extern lv_font_t *g_builtin_small_text_font;
extern lv_font_t *g_builtin_icon_font;

/* Load scalable TTF fonts from <assets_dir>/fonts/. Falls back to LVGL's
 * built-in montserrat font if the TTF is missing, so the firmware always
 * renders text. */
void InitBuiltinFonts(const char *assets_dir);

/* Change the application typography at runtime. The selected size/bold state
 * is persisted in the display settings and applied to every live LVGL object
 * that currently uses one of the application text faces. */
void ApplyBuiltinTypography(int size_px, bool bold);
int BuiltinTextSize();
bool BuiltinTextBold();

/* Switch the live application font to a TTF already present on disk. The
 * paths and display name are persisted, so a font downloaded from the
 * firmware's own S3 bucket remains selected after reboot. No network access
 * happens here; the settings UI owns the explicit download step. */
bool ApplyBuiltinFontFamily(const std::string &display_name,
                            const std::string &regular_path,
                            const std::string &bold_path = "");
const std::string &BuiltinFontName();
const std::string &BuiltinFontRegularPath();
const std::string &BuiltinAssetsDir();

/* A cached text face (current family, regular weight) at an explicit pixel
 * size. Use for compact secondary labels that still need Vietnamese diacritics
 * but want a smaller size than the body text -- e.g. the music view's artist /
 * album / subtitle rows, which must NOT use lv_font_montserrat_* (those carry
 * no Vietnamese glyphs). Falls back to a built-in font if the TTF is
 * unavailable. The returned pointer is stable for the process lifetime. */
const lv_font_t *BuiltinTextFaceAt(int size_px);

/* Fixed-width face for terminal/code surfaces at an explicit pixel size.
 * Source Code Pro is preferred from the firmware assets, then common system
 * monospace families are tried. Faces are cached for the process lifetime. */
const lv_font_t *BuiltinTerminalFontAt(int size_px);
const lv_font_t *BuiltinTerminalFont();
const std::string &BuiltinTerminalFontName();
} // namespace jetson

/* The DS-02 UI references &BUILTIN_TEXT_FONT / &BUILTIN_ICON_FONT as
 * lv_font_t pointers. These macros resolve to the runtime tiny_ttf fonts. */
#define BUILTIN_TEXT_FONT (*::jetson::g_builtin_text_font)
#define BUILTIN_SMALL_TEXT_FONT (*::jetson::g_builtin_small_text_font)
#define BUILTIN_ICON_FONT (*::jetson::g_builtin_icon_font)

#endif
