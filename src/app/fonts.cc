#include "fonts.h"
#include "esp_log.h"

#include <cstdio>
#include <cstring>
#include <string>

// tiny_ttf is pulled in via <lvgl.h> (already included by fonts.h).

#define TAG "fonts"

namespace jetson {

lv_font_t *g_builtin_text_font = nullptr;
lv_font_t *g_builtin_small_text_font = nullptr;
lv_font_t *g_builtin_icon_font = nullptr;

static std::string joinPath(const char *dir, const char *file) {
    std::string p = dir;
    if (!p.empty() && p.back() != '/') p += "/";
    p += file;
    return p;
}

void InitBuiltinFonts(const char *assets_dir) {
    /* Prefer Noto Sans (Vietnamese diacritics + CJK) if the user drops it in,
     * otherwise fall back to the bundled arial.ttf. */
    const char *text_file = "fonts/NotoSans-Regular.ttf";
    const char *icon_file = "fonts/NotoSans-Regular.ttf";
    std::string text_path = joinPath(assets_dir, text_file);
    FILE *f = std::fopen(text_path.c_str(), "rb");
    if (!f) {
        text_path = joinPath(assets_dir, "fonts/arial.ttf");
    } else {
        std::fclose(f);
    }
    std::string icon_path = joinPath(assets_dir, icon_file);
    f = std::fopen(icon_path.c_str(), "rb");
    if (!f) {
        icon_path = joinPath(assets_dir, "fonts/arial.ttf");
    } else {
        std::fclose(f);
    }

    g_builtin_text_font = lv_tiny_ttf_create_file(text_path.c_str(), 28);
    if (!g_builtin_text_font) {
        ESP_LOGW(TAG, "tiny_ttf failed for %s, falling back to montserrat_28", text_path.c_str());
        g_builtin_text_font = (lv_font_t *)&lv_font_montserrat_28;
    }
    // Compact dialogs use a separate face instance: tiny_ttf stores the size
    // on the lv_font_t, so resizing the main font would shrink the whole UI.
    g_builtin_small_text_font = lv_tiny_ttf_create_file(text_path.c_str(), 18);
    if (!g_builtin_small_text_font) {
        ESP_LOGW(TAG, "tiny_ttf small font failed for %s, falling back to montserrat_18",
                 text_path.c_str());
        g_builtin_small_text_font = (lv_font_t *)&lv_font_montserrat_18;
    }
    /* The icon font MUST contain the LVGL symbol glyphs (LV_SYMBOL_WIFI /
     * SETTINGS / BLUETOOTH / IMAGE / LEFT / REFRESH ...). The bundled arial.ttf
     * and NotoSans do NOT carry that Font Awesome symbol block, so loading the
     * icon font from those files would render every icon as a missing-glyph box.
     * Use LVGL's built-in montserrat, which ships with the symbol range baked in.
     * (Vietnamese diacritics in labels use the text font above, not this one.) */
    g_builtin_icon_font = (lv_font_t *)&lv_font_montserrat_24;
    ESP_LOGI(TAG, "fonts ready (text=%s, compact=18, icon=montserrat_24)",
             text_path.c_str());
}

} // namespace jetson
