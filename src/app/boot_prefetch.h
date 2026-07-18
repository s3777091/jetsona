#pragma once

/* Post-boot warm-up + RAM budget guard.
 *
 * Called once after the home UI is on screen. It never delays the first
 * paint: everything runs on the TaskPool prefetch lane (nice +10) behind the
 * boot splash.
 *
 *  - Grows LVGL's global image cache from the conservative lv_conf default to
 *    JETSON_IMG_CACHE_MB (default 128 MB), so wallpapers, app icons and
 *    artwork decoded once stay decoded.
 *  - Refreshes the Zing discover snapshot (net/zing_discover_cache.h) and its
 *    artwork under a short CPU boost, so the first Music open is warm.
 *  - Starts the memory-budget monitor: when /proc/meminfo MemAvailable drops
 *    under JETSON_MIN_FREE_MB (default 500 MB) the LVGL image cache is
 *    dropped, trading redecodes for headroom — the Nano shares its 4 GB with
 *    the GPU, so the firmware must never be the reason the OOM killer wakes
 *    up. */

namespace jetson {

void StartBootPrefetch();

} // namespace jetson
