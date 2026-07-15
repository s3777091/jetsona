#include "background_gallery_view.h"
#include "backgrounds.h"
#include "fonts.h"
#include "settings.h"
#include "ui_theme.h"
#include "esp_log.h"

#include <lvgl.h>
#include <cstdio>
#include <cstring>

namespace home {

namespace {
lv_color_t Color(uint32_t rgb) {
    return lv_color_make((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}
struct LvLockGuard {
    LvLockGuard() { lv_lock(); }
    ~LvLockGuard() { lv_unlock(); }
};
} // namespace

BackgroundGalleryView::BackgroundGalleryView(lv_obj_t *parent, int width, int height, ClosedCb on_closed)
    : OverlayView(parent, width, height, "Ảnh nền", std::move(on_closed)) {
    Settings s("display", false);
    current_ = (size_t)s.GetInt("ds02_background", 0);
    if (current_ >= kCount) current_ = 0;
    BuildBody();
}

BackgroundGalleryView::~BackgroundGalleryView() {
    // Detach LVGL image sources from the lv_image objects BEFORE the LvglImage
    // buffers (images_) are freed, so LVGL never dereferences a dangling dsc
    // when the overlay is deleted afterwards.
    if (overlay_) {
        lv_lock();
        for (size_t i = 0; i < kCount; ++i) {
            if (img_objs_[i]) lv_image_set_src(img_objs_[i], nullptr);
        }
        lv_unlock();
    }
}

std::string BackgroundGalleryView::AssetPath(size_t index) {
    // Load the small pre-rendered thumbnail (400x240) instead of the full
    // 800x480 wallpaper. Decoding 10 full-res PNGs at gallery-open made the
    // Jetson stall for seconds; the thumbnails decode near-instantly. The
    // selected wallpaper is still applied from the full-size file by the home
    // screen, so visual quality on the actual background is unchanged.
    return BackgroundsDir() + "/thumbs/" + kBackgroundFiles[index];
}

void BackgroundGalleryView::BuildBody() {
    const auto &p = jetson::UiTheme::Instance().Palette();

    lv_obj_set_flex_flow(body_, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(body_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(body_, 8, 0);
    lv_obj_set_style_pad_column(body_, 8, 0);
    lv_obj_add_flag(body_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(body_, LV_SCROLLBAR_MODE_ACTIVE);

    int cellW = (width_ - 16 - 8) / kCols; // body pad 8 + gap 8
    int imgH = 110;
    int cellH = imgH + 28;

    for (size_t i = 0; i < kCount; ++i) {
        auto *cell = lv_obj_create(body_);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, cellW, cellH);
        lv_obj_set_style_radius(cell, 12, 0);
        lv_obj_set_style_bg_color(cell, Color(p.row), 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(cell, (i == current_) ? 3 : 0, 0);
        lv_obj_set_style_border_color(cell, Color(p.accent), 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);

        auto *img_obj = lv_image_create(cell);
        lv_obj_set_pos(img_obj, 0, 0);
        lv_obj_set_size(img_obj, cellW, imgH);
        lv_obj_set_style_clip_corner(img_obj, true, 0);
        lv_obj_set_style_radius(img_obj, 12, 0);
        lv_obj_set_style_bg_color(img_obj, Color(p.button), 0);
        lv_obj_set_style_bg_opa(img_obj, LV_OPA_COVER, 0);

        auto img = LvglImageFromFile(AssetPath(i));
        if (img) {
            // Thumbnails are a fixed 400x240 on disk. The LvglRawImage header
            // leaves w/h at 0 (lazy decode), so we can't read dims from the dsc
            // -- use the known thumbnail size and fit-to-width so the strip
            // fills the cell, with clip_corner cropping the vertical overflow.
            constexpr int kThumbW = 400;
            int scale = cellW * 256 / kThumbW;
            lv_image_set_scale(img_obj, (uint16_t)scale);
            lv_image_set_src(img_obj, img->image_dsc());
            images_[i] = std::move(img);
        }
        img_objs_[i] = img_obj;
        lv_obj_align(img_obj, LV_ALIGN_TOP_MID, 0, 0);

        auto *name = lv_label_create(cell);
        lv_obj_set_style_text_font(name, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(name, Color(p.sub_text), 0);
        // Strip ".png" for a cleaner caption.
        std::string fn = kBackgroundFiles[i];
        size_t dot = fn.rfind('.');
        if (dot != std::string::npos) fn = fn.substr(0, dot);
        lv_label_set_text(name, fn.c_str());
        lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -4);

        auto *ctx = new CellCtx{this, i};
        ctxs_[i] = ctx;
        lv_obj_add_event_cb(cell, OnCellClicked, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(cell, OnCellDeleted, LV_EVENT_DELETE, ctx);
        cells_[i] = cell;
    }
}

void BackgroundGalleryView::RefreshHighlights() {
    const auto &p = jetson::UiTheme::Instance().Palette();
    for (size_t i = 0; i < kCount; ++i) {
        if (!cells_[i]) continue;
        lv_obj_set_style_border_width(cells_[i], (i == current_) ? 3 : 0, 0);
        lv_obj_set_style_border_color(cells_[i], Color(p.accent), 0);
    }
}

void BackgroundGalleryView::OnStart() {
    SetStatus("Chạm ảnh để đặt làm nền");
}

void BackgroundGalleryView::OnCellClicked(lv_event_t *e) {
    LvLockGuard lock;
    auto *ctx = static_cast<CellCtx *>(lv_event_get_user_data(e));
    auto *self = ctx->self;
    self->current_ = ctx->index;
    Settings s("display", true);
    s.SetInt("ds02_background", (int)self->current_);
    self->RefreshHighlights();
    if (self->on_select_) self->on_select_(self->current_);
    self->SetStatus("Đã đặt ảnh nền");
}

void BackgroundGalleryView::OnCellDeleted(lv_event_t *e) {
    auto *ctx = static_cast<CellCtx *>(lv_event_get_user_data(e));
    delete ctx;
}

} // namespace home