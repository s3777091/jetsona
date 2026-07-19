#pragma once

/* Ekko Bot "orbit" -- the watercolor sphere shown inside the Dynamic Island
 * when the assistant is summoned from the dock. Five fixed palettes named
 * after Greek deities; the user picks one in Settings > Ứng dụng > Ekko Bot
 * (persisted as assistant/orbit_color).
 *
 * The orb is a circular, corner-clipped container holding two drifting color
 * blobs plus a soft highlight. LVGL's software renderer has no blur and (in
 * this build) no radial gradients, so the watercolor look comes from layered
 * translucent circles with linear gradients; the "life" comes from slow
 * translate/opacity lv_anims with mismatched periods. Animations must only
 * run while the orb is actually visible: a live lv_anim keeps the LVGL
 * refresh timer hot every frame, so callers pair SetOrbitAnimated(true) on
 * show with SetOrbitAnimated(false) on hide. */
#include <lvgl.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace home {

struct OrbitPalette {
    const char *id;       // settings value (assistant/orbit_color)
    const char *name;     // Greek deity display name
    const char *subtitle; // Vietnamese flavor line for the settings row
    uint32_t base_top;    // orb background gradient, top
    uint32_t base_bottom; // orb background gradient, bottom
    uint32_t blob_a;      // large drifting blob
    uint32_t blob_b;      // small drifting blob
    uint32_t glow;        // highlight circle
    uint32_t accent;      // island border tint while the orbit is open
};

const OrbitPalette *OrbitPalettes(size_t *count);
// Unknown/empty ids fall back to the default palette (Helios).
const OrbitPalette &OrbitPaletteById(const std::string &id);
std::string SelectedOrbitId();

lv_obj_t *CreateOrbitOrb(lv_obj_t *parent, const OrbitPalette &palette,
                         int diameter);
void SetOrbitPalette(lv_obj_t *orb, const OrbitPalette &palette);
void SetOrbitAnimated(lv_obj_t *orb, bool animated);

} // namespace home
