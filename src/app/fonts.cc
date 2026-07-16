#include "fonts.h"
#include "esp_log.h"
#include "settings.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <utility>

// tiny_ttf is pulled in via <lvgl.h> (already included by fonts.h).

#define TAG "fonts"

namespace jetson {

lv_font_t *g_builtin_text_font = nullptr;
lv_font_t *g_builtin_small_text_font = nullptr;
lv_font_t *g_builtin_icon_font = nullptr;

namespace {

std::string g_regular_path;
std::string g_bold_path;
std::string g_font_name = "Arial";
std::string g_assets_dir = "assets";
int g_text_size = 28;
bool g_text_bold = false;
std::map<std::tuple<std::string, int, bool>, lv_font_t *> g_font_cache;

bool FileExists(const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

lv_font_t *LoadTextFace(int size, bool bold) {
    size = std::clamp(size, 16, 34);
    const std::string &path = (bold && !g_bold_path.empty()) ? g_bold_path : g_regular_path;
    const auto key = std::make_tuple(path, size, bold);
    auto found = g_font_cache.find(key);
    if (found != g_font_cache.end()) return found->second;

    lv_font_t *font = path.empty() ? nullptr : lv_tiny_ttf_create_file(path.c_str(), size);
    if (!font) {
        ESP_LOGW(TAG, "tiny_ttf failed for %s at %d px", path.c_str(), size);
        font = size <= 24 ? (lv_font_t *)&lv_font_montserrat_24
                          : (lv_font_t *)&lv_font_montserrat_28;
    }
    g_font_cache[key] = font;
    return font;
}

void ReplaceTextFaces(lv_obj_t *obj, const lv_font_t *old_main,
                      const lv_font_t *old_small, const lv_font_t *new_main,
                      const lv_font_t *new_small) {
    if (!obj) return;
    const lv_font_t *current = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
    if (current == old_main) lv_obj_set_style_text_font(obj, new_main, LV_PART_MAIN);
    else if (current == old_small) lv_obj_set_style_text_font(obj, new_small, LV_PART_MAIN);

    const uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; ++i) {
        ReplaceTextFaces(lv_obj_get_child(obj, (int32_t)i), old_main, old_small,
                         new_main, new_small);
    }
}

} // namespace

static std::string joinPath(const char *dir, const char *file) {
    std::string p = dir;
    if (!p.empty() && p.back() != '/') p += "/";
    p += file;
    return p;
}

void InitBuiltinFonts(const char *assets_dir) {
    g_assets_dir = (assets_dir && assets_dir[0]) ? assets_dir : "assets";
    /* Prefer Noto Sans (Vietnamese diacritics + broad Unicode coverage). Font
     * packs may be copied either directly into assets/fonts or into one nested
     * folder, as with the bundled assets/fonts/fonts pack. */
    const std::string regular_candidates[] = {
        joinPath(g_assets_dir.c_str(), "fonts/NotoSans-Regular.ttf"),
        joinPath(g_assets_dir.c_str(), "fonts/fonts/NotoSans-Regular.ttf"),
        joinPath(g_assets_dir.c_str(), "fonts/arial.ttf"),
    };
    std::string text_path = regular_candidates[2];
    for (const auto &candidate : regular_candidates) {
        if (FileExists(candidate)) { text_path = candidate; break; }
    }
    g_regular_path = text_path;
    g_bold_path.clear();
    const std::string bold_candidates[] = {
        joinPath(g_assets_dir.c_str(), "fonts/NotoSans-Bold.ttf"),
        joinPath(g_assets_dir.c_str(), "fonts/fonts/NotoSans-Bold.ttf"),
        joinPath(g_assets_dir.c_str(), "fonts/arialbd.ttf"),
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Bold.ttf",
        "C:/Windows/Fonts/arialbd.ttf",
    };
    for (const auto &candidate : bold_candidates) {
        if (FileExists(candidate)) { g_bold_path = candidate; break; }
    }

    Settings display("display", false);
    const std::string saved_regular = display.GetString("font_regular_path", "");
    const std::string saved_bold = display.GetString("font_bold_path", "");
    if (!saved_regular.empty() && FileExists(saved_regular)) {
        g_regular_path = saved_regular;
        g_bold_path = FileExists(saved_bold) ? saved_bold : "";
        g_font_name = display.GetString("font_name", "Phông chữ của tôi");
    } else {
        g_font_name = text_path.find("NotoSans") != std::string::npos ? "Noto Sans" : "Arial";
    }
    g_text_size = std::clamp(display.GetInt("font_size", 28), 22, 34);
    g_text_bold = display.GetBool("bold_text", false);
    g_builtin_text_font = LoadTextFace(g_text_size, g_text_bold);
    const int small_size = std::clamp(g_text_size * 18 / 28, 16, 22);
    g_builtin_small_text_font = LoadTextFace(small_size, g_text_bold);
    /* The icon font MUST contain the LVGL symbol glyphs (LV_SYMBOL_WIFI /
     * SETTINGS / BLUETOOTH / IMAGE / LEFT / REFRESH ...). The bundled arial.ttf
     * and NotoSans do NOT carry that Font Awesome symbol block, so loading the
     * icon font from those files would render every icon as a missing-glyph box.
     * Use LVGL's built-in montserrat, which ships with the symbol range baked in.
     * (Vietnamese diacritics in labels use the text font above, not this one.) */
    g_builtin_icon_font = (lv_font_t *)&lv_font_montserrat_24;
    ESP_LOGI(TAG, "fonts ready (name=%s, text=%s, size=%d, bold=%s, icon=montserrat_24)",
             g_font_name.c_str(), g_regular_path.c_str(), g_text_size,
             g_text_bold ? "yes" : "no");
}

void ApplyBuiltinTypography(int size_px, bool bold) {
    size_px = std::clamp(size_px, 22, 34);
    lv_font_t *old_main = g_builtin_text_font;
    lv_font_t *old_small = g_builtin_small_text_font;
    lv_font_t *new_main = LoadTextFace(size_px, bold);
    const int small_size = std::clamp(size_px * 18 / 28, 16, 22);
    lv_font_t *new_small = LoadTextFace(small_size, bold);
    if (!new_main || !new_small) return;

    g_builtin_text_font = new_main;
    g_builtin_small_text_font = new_small;
    g_text_size = size_px;
    g_text_bold = bold;

    Settings s("display", true);
    s.SetInt("font_size", size_px);
    s.SetBool("bold_text", bold);

    ReplaceTextFaces(lv_screen_active(), old_main, old_small, new_main, new_small);
    ReplaceTextFaces(lv_layer_top(), old_main, old_small, new_main, new_small);
    ReplaceTextFaces(lv_layer_sys(), old_main, old_small, new_main, new_small);
    lv_obj_report_style_change(nullptr);
    ESP_LOGI(TAG, "typography -> %d px, bold=%s", size_px, bold ? "yes" : "no");
}

int BuiltinTextSize() { return g_text_size; }
bool BuiltinTextBold() { return g_text_bold; }

bool ApplyBuiltinFontFamily(const std::string &display_name,
                            const std::string &regular_path,
                            const std::string &bold_path) {
    if (regular_path.empty() || !FileExists(regular_path)) {
        ESP_LOGW(TAG, "font file is missing: %s", regular_path.c_str());
        return false;
    }
    lv_font_t *probe = lv_tiny_ttf_create_file(regular_path.c_str(), 16);
    if (!probe) {
        ESP_LOGW(TAG, "invalid or unsupported TTF: %s", regular_path.c_str());
        return false;
    }
    lv_tiny_ttf_destroy(probe);

    lv_font_t *old_main = g_builtin_text_font;
    lv_font_t *old_small = g_builtin_small_text_font;
    const std::string old_regular = g_regular_path;
    const std::string old_bold = g_bold_path;

    g_regular_path = regular_path;
    g_bold_path = (!bold_path.empty() && FileExists(bold_path)) ? bold_path : "";
    lv_font_t *new_main = LoadTextFace(g_text_size, g_text_bold);
    const int small_size = std::clamp(g_text_size * 18 / 28, 16, 22);
    lv_font_t *new_small = LoadTextFace(small_size, g_text_bold);
    if (!new_main || !new_small) {
        g_regular_path = old_regular;
        g_bold_path = old_bold;
        return false;
    }

    g_builtin_text_font = new_main;
    g_builtin_small_text_font = new_small;
    g_font_name = display_name.empty() ? "Phông chữ của tôi" : display_name;

    Settings s("display", true);
    s.SetString("font_name", g_font_name);
    s.SetString("font_regular_path", g_regular_path);
    s.SetString("font_bold_path", g_bold_path);

    ReplaceTextFaces(lv_screen_active(), old_main, old_small, new_main, new_small);
    ReplaceTextFaces(lv_layer_top(), old_main, old_small, new_main, new_small);
    ReplaceTextFaces(lv_layer_sys(), old_main, old_small, new_main, new_small);
    lv_obj_report_style_change(nullptr);
    ESP_LOGI(TAG, "font family -> %s (%s)", g_font_name.c_str(), g_regular_path.c_str());
    return true;
}

const std::string &BuiltinFontName() { return g_font_name; }
const std::string &BuiltinFontRegularPath() { return g_regular_path; }
const std::string &BuiltinAssetsDir() { return g_assets_dir; }

} // namespace jetson
