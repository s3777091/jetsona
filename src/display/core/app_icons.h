#pragma once

/* Shared decode-once cache for the small system PNGs in assets/icons/app/.
 *
 * Views used to call LvglImageFromFile() for private copies of the same icon,
 * which duplicated the raw PNG bytes and — because LVGL's image cache
 * (LV_CACHE_DEF_SIZE) keys decoded pixels by descriptor pointer — also
 * duplicated the decoded ARGB. Loading each icon exactly once keeps a single
 * stable lv_img_dsc_t per file, so the FULL-render-mode frame loop always hits
 * the same cache entry.
 *
 * Call these with the LVGL lock held; the returned descriptors stay valid for
 * the process lifetime. */

#include <lvgl.h>

namespace jetson::ui {

// Descriptor for assets/icons/app/<name>.png ("wifi", "cellular-3", ...),
// loaded once per process. nullptr when the file is missing/unreadable.
const lv_img_dsc_t *AppIconDsc(const char *name);

// lv_image showing the icon aspect-fitted into a box_px square. The object is
// sized from the PNG's IHDR, so callers never depend on the source pixel size
// (the icon set can be re-exported at any resolution). Always returns a valid
// lv_image; a missing PNG renders as an empty box_px box (and is logged by the
// loader), so call sites need no fallback path.
lv_obj_t *CreateAppIcon(lv_obj_t *parent, const char *name, int box_px);

// Swap the source of an image created by CreateAppIcon — for stateful icons
// (wifi/no-wifi, speaker/speaker-mute, cellular-2..5). Recomputes the
// aspect-fitted size for the new source. No-op when the PNG is missing.
void SetAppIcon(lv_obj_t *img, const char *name, int box_px);

// Load every PNG under assets/icons/app into the cache and run the decoder
// once per icon, so LVGL's global image cache is warm before the first frame
// instead of decoding the whole system set during it.
void PreloadAppIcons();

} // namespace jetson::ui
