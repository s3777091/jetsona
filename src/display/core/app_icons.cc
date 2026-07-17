#include "display/core/app_icons.h"
#include "display/core/lvgl_image.h"
#include "esp_log.h"

#include <dirent.h>
#include <strings.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#define TAG "AppIcons"

namespace jetson::ui {
namespace {

constexpr char kAppIconDir[] = "assets/icons/app/";

struct CachedIcon {
    std::unique_ptr<LvglImage> image;
    int w = 0, h = 0;
};

std::mutex g_cache_mutex;
std::unordered_map<std::string, CachedIcon> &Cache() {
    static auto *cache = new std::unordered_map<std::string, CachedIcon>();
    return *cache;
}

/* PNG IHDR width/height (big-endian at offsets 16/20). LvglRawImage keeps
 * header.w/h at 0 until LVGL lazily decodes, so read the size straight from
 * the raw bytes the cache already owns. */
void PngDims(const lv_img_dsc_t *dsc, int *w, int *h) {
    *w = *h = 0;
    if (!dsc || !dsc->data || dsc->data_size < 24) return;
    const uint8_t *d = dsc->data;
    if (d[0] != 0x89 || d[1] != 0x50) return; // not a PNG (JPG/GIF): leave 0x0
    *w = (d[16] << 24) | (d[17] << 16) | (d[18] << 8) | d[19];
    *h = (d[20] << 24) | (d[21] << 16) | (d[22] << 8) | d[23];
}

// Loads on first use; a missing file is cached too so it is not re-stat'ed
// every frame refresh that asks for it.
const CachedIcon *Lookup(const char *name) {
    if (!name || !*name) return nullptr;
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    auto &cache = Cache();
    auto it = cache.find(name);
    if (it == cache.end()) {
        CachedIcon icon;
        icon.image = LvglImageFromFile(std::string(kAppIconDir) + name + ".png");
        if (icon.image) PngDims(icon.image->image_dsc(), &icon.w, &icon.h);
        it = cache.emplace(name, std::move(icon)).first;
    }
    return it->second.image ? &it->second : nullptr;
}

// Aspect-fit the icon's IHDR size into a box_px square (most app icons are
// square, but e.g. no-bluetooh.png is 26x33 and must not be distorted).
void FitBox(const CachedIcon *icon, int box_px, int *w, int *h) {
    *w = *h = box_px;
    if (!icon || icon->w <= 0 || icon->h <= 0) return;
    if (icon->w >= icon->h) *h = box_px * icon->h / icon->w;
    else *w = box_px * icon->w / icon->h;
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
}

} // namespace

const lv_img_dsc_t *AppIconDsc(const char *name) {
    const CachedIcon *icon = Lookup(name);
    return icon ? icon->image->image_dsc() : nullptr;
}

/* Size the object to the aspect-fitted box and scale the source into it with
 * pivot (0,0), so the transformed image exactly covers the object. This is the
 * IHDR-based approach the dock/drawer already uses; LV_IMAGE_ALIGN_STRETCH is
 * avoided on purpose -- it derives the scale from the layouted object size,
 * which is stale at build time and is not recomputed on LV_EVENT_SIZE_CHANGED
 * in LVGL 9.2. */
static void ApplyIcon(lv_obj_t *img, const CachedIcon *icon, int box_px) {
    lv_image_set_src(img, icon->image->image_dsc());
    int w, h;
    FitBox(icon, box_px, &w, &h);
    lv_obj_set_size(img, w, h);
    lv_image_set_pivot(img, 0, 0);
    const int longer = (icon->w > icon->h) ? icon->w : icon->h;
    if (longer > 0) {
        int scale = box_px * 256 / longer;
        if (scale < 32) scale = 32;
        if (scale > 1024) scale = 1024;
        lv_image_set_scale(img, (uint32_t)scale);
    }
}

lv_obj_t *CreateAppIcon(lv_obj_t *parent, const char *name, int box_px) {
    auto *img = lv_image_create(parent);
    if (const CachedIcon *icon = Lookup(name)) {
        ApplyIcon(img, icon, box_px);
    } else {
        // Missing asset (already logged by the loader): keep the layout slot.
        lv_obj_set_size(img, box_px, box_px);
    }
    lv_obj_clear_flag(img, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    return img;
}

void SetAppIcon(lv_obj_t *img, const char *name, int box_px) {
    const CachedIcon *icon = Lookup(name);
    if (!img || !icon) return;
    ApplyIcon(img, icon, box_px);
}

void PreloadAppIcons() {
    DIR *dir = opendir(kAppIconDir);
    if (!dir) {
        ESP_LOGW(TAG, "cannot open %s", kAppIconDir);
        return;
    }
    int loaded = 0;
    while (struct dirent *entry = readdir(dir)) {
        const char *dot = std::strrchr(entry->d_name, '.');
        if (!dot || strcasecmp(dot, ".png") != 0) continue;
        std::string name(entry->d_name, (size_t)(dot - entry->d_name));
        const lv_img_dsc_t *dsc = AppIconDsc(name.c_str());
        if (!dsc) continue;
        /* Decode once now; with LV_CACHE_DEF_SIZE > 0 the decoded pixels stay
         * in LVGL's image cache keyed by this (stable) descriptor, so the
         * first frame does not decode the whole system icon set. */
        lv_image_decoder_dsc_t dec;
        if (lv_image_decoder_open(&dec, dsc, nullptr) == LV_RESULT_OK)
            lv_image_decoder_close(&dec);
        ++loaded;
    }
    closedir(dir);
    ESP_LOGI(TAG, "preloaded %d app icons", loaded);
}

} // namespace jetson::ui
