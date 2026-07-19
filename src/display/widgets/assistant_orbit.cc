#include "display/widgets/assistant_orbit.h"

#include "display/common/lvgl_utils.h"
#include "settings.h"

namespace home {

using jetson::ui::Color;

namespace {

/* Palette order matches the reference artwork left-to-right; Helios (the
 * orange sun sphere) is the default. */
constexpr OrbitPalette kPalettes[] = {
    {"selene", "Selene", "Ánh trăng bạc",
     0xf2f6f2, 0xb7c3bc, 0xffffff, 0xa3b4a9, 0xffffff, 0xdfe8e2},
    {"iris", "Iris", "Tím cực quang",
     0xc99df8, 0x7e96f2, 0xf2a9dd, 0x8fc7ff, 0xffe6f6, 0xc3a4ff},
    {"helios", "Helios", "Cam mặt trời",
     0xf7b25c, 0xd45c10, 0xffdf9e, 0xa63f0b, 0xfff0cd, 0xffb24d},
    {"nyx", "Nyx", "Nâu bóng đêm",
     0x86656d, 0x2a1e24, 0xa07a70, 0x39262f, 0xd8bfc9, 0x8f6b76},
    {"eos", "Eos", "Ngọc trai bình minh",
     0xfcf2df, 0xe3d0bf, 0xf7d8e4, 0xcfe6da, 0xfffaf0, 0xf2ddc4},
};
constexpr size_t kPaletteCount = sizeof(kPalettes) / sizeof(kPalettes[0]);
constexpr size_t kDefaultPalette = 2; // helios

// Child slots inside the orb container. The module owns the orb's whole
// subtree, so index-based lookup is safe and avoids a side allocation.
enum OrbChild : uint32_t { kBlobA = 0, kBlobB, kGlow, kOrbChildCount };

// Resting opacities; the glow additionally breathes around its value.
constexpr lv_opa_t kBlobAOpa = 150;
constexpr lv_opa_t kBlobBOpa = 135;
constexpr lv_opa_t kGlowOpa = 110;

void AnimTranslateX(void *var, int32_t v) {
    lv_obj_set_style_translate_x(static_cast<lv_obj_t *>(var), v, 0);
}
void AnimTranslateY(void *var, int32_t v) {
    lv_obj_set_style_translate_y(static_cast<lv_obj_t *>(var), v, 0);
}
void AnimBgOpa(void *var, int32_t v) {
    lv_obj_set_style_bg_opa(static_cast<lv_obj_t *>(var), (lv_opa_t)v, 0);
}

void StartDrift(lv_obj_t *obj, lv_anim_exec_xcb_t exec, int32_t amplitude,
                uint32_t period_ms) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, exec);
    lv_anim_set_values(&a, -amplitude, amplitude);
    lv_anim_set_time(&a, period_ms);
    lv_anim_set_playback_time(&a, period_ms);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

void StartBreath(lv_obj_t *obj, lv_opa_t from, lv_opa_t to, uint32_t period_ms) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, AnimBgOpa);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, period_ms);
    lv_anim_set_playback_time(&a, period_ms);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

lv_obj_t *MakeBlob(lv_obj_t *orb, int size, lv_align_t align, int ofs_x,
                   int ofs_y) {
    auto *blob = lv_obj_create(orb);
    lv_obj_remove_style_all(blob);
    lv_obj_set_size(blob, size, size);
    lv_obj_set_style_radius(blob, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(blob, align, ofs_x, ofs_y);
    lv_obj_clear_flag(blob, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE |
                                            LV_OBJ_FLAG_CLICKABLE));
    return blob;
}

} // namespace

const OrbitPalette *OrbitPalettes(size_t *count) {
    if (count) *count = kPaletteCount;
    return kPalettes;
}

const OrbitPalette &OrbitPaletteById(const std::string &id) {
    for (const auto &palette : kPalettes)
        if (id == palette.id) return palette;
    return kPalettes[kDefaultPalette];
}

std::string SelectedOrbitId() {
    return Settings("assistant").GetString("orbit_color",
                                           kPalettes[kDefaultPalette].id);
}

lv_obj_t *CreateOrbitOrb(lv_obj_t *parent, const OrbitPalette &palette,
                         int diameter) {
    auto *orb = lv_obj_create(parent);
    lv_obj_remove_style_all(orb);
    lv_obj_set_size(orb, diameter, diameter);
    lv_obj_set_style_radius(orb, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(orb, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_dir(orb, LV_GRAD_DIR_VER, 0);
    // The blobs drift partially outside the circle; the corner clip keeps the
    // spill inside the sphere so it reads as watercolor, not stray dots.
    lv_obj_set_style_clip_corner(orb, true, 0);
    lv_obj_clear_flag(orb, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE |
                                           LV_OBJ_FLAG_CLICKABLE));

    // Creation order must match OrbChild.
    auto *blob_a = MakeBlob(orb, diameter * 78 / 100, LV_ALIGN_CENTER,
                            -diameter / 10, -diameter * 8 / 100);
    lv_obj_set_style_bg_opa(blob_a, kBlobAOpa, 0);
    lv_obj_set_style_bg_grad_dir(blob_a, LV_GRAD_DIR_VER, 0);

    auto *blob_b = MakeBlob(orb, diameter * 60 / 100, LV_ALIGN_CENTER,
                            diameter * 16 / 100, diameter / 5);
    lv_obj_set_style_bg_opa(blob_b, kBlobBOpa, 0);

    auto *glow = MakeBlob(orb, diameter * 42 / 100, LV_ALIGN_CENTER,
                          -diameter * 18 / 100, -diameter * 22 / 100);
    lv_obj_set_style_bg_opa(glow, kGlowOpa, 0);

    SetOrbitPalette(orb, palette);
    return orb;
}

void SetOrbitPalette(lv_obj_t *orb, const OrbitPalette &palette) {
    if (!orb || lv_obj_get_child_count(orb) < kOrbChildCount) return;
    lv_obj_set_style_bg_color(orb, Color(palette.base_top), 0);
    lv_obj_set_style_bg_grad_color(orb, Color(palette.base_bottom), 0);

    auto *blob_a = lv_obj_get_child(orb, kBlobA);
    lv_obj_set_style_bg_color(blob_a, Color(palette.blob_a), 0);
    lv_obj_set_style_bg_grad_color(blob_a, Color(palette.base_top), 0);

    lv_obj_set_style_bg_color(lv_obj_get_child(orb, kBlobB),
                              Color(palette.blob_b), 0);
    lv_obj_set_style_bg_color(lv_obj_get_child(orb, kGlow),
                              Color(palette.glow), 0);
}

void SetOrbitAnimated(lv_obj_t *orb, bool animated) {
    if (!orb || lv_obj_get_child_count(orb) < kOrbChildCount) return;
    auto *blob_a = lv_obj_get_child(orb, kBlobA);
    auto *blob_b = lv_obj_get_child(orb, kBlobB);
    auto *glow = lv_obj_get_child(orb, kGlow);

    for (auto *blob : {blob_a, blob_b, glow}) {
        lv_anim_delete(blob, AnimTranslateX);
        lv_anim_delete(blob, AnimTranslateY);
        lv_anim_delete(blob, AnimBgOpa);
        lv_obj_set_style_translate_x(blob, 0, 0);
        lv_obj_set_style_translate_y(blob, 0, 0);
    }
    lv_obj_set_style_bg_opa(blob_a, kBlobAOpa, 0);
    lv_obj_set_style_bg_opa(blob_b, kBlobBOpa, 0);
    lv_obj_set_style_bg_opa(glow, kGlowOpa, 0);
    if (!animated) return;

    // Mismatched x/y periods trace slow Lissajous drifts instead of a loop
    // the eye can lock onto. Amplitudes scale with the orb so the same code
    // animates both the island orb and any larger future variant. Fall back
    // to the style width in case the caller animates before the first layout
    // pass resolves the object size.
    int d = lv_obj_get_width(orb);
    if (d <= 0) d = 112;
    StartDrift(blob_a, AnimTranslateX, d * 10 / 100, 3400);
    StartDrift(blob_a, AnimTranslateY, d * 8 / 100, 4600);
    StartDrift(blob_b, AnimTranslateX, d * 12 / 100, 4000);
    StartDrift(blob_b, AnimTranslateY, d * 10 / 100, 2900);
    StartDrift(glow, AnimTranslateX, d * 6 / 100, 5200);
    StartBreath(glow, kGlowOpa - 40, kGlowOpa + 60, 3600);
}

} // namespace home
