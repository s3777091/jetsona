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
    current_file_ = s.GetString("ds02_background_file", "");
    sleep_file_ = s.GetString("ds02_sleep_bg_file", "");
    BuildBody();
}

BackgroundGalleryView::~BackgroundGalleryView() {
    // Tear down LVGL resources that reference our buffers before the base class
    // deletes the overlay. The load timer and popup live on the overlay; the
    // image dscs point into images_, which is freed when the member destructs.
    lv_lock();
    if (load_timer_) { lv_timer_del(load_timer_); load_timer_ = nullptr; }
    if (popup_) { lv_obj_del(popup_); popup_ = nullptr; popup_card_ = nullptr; }
    for (auto *o : img_objs_) if (o) lv_image_set_src(o, nullptr);
    lv_unlock();
    // cells_ + CellCtx objects are freed when the base class deletes overlay_
    // (OnCellDeleted runs for each cell and deletes its ctx).
}

void BackgroundGalleryView::ClearGrid() {
    for (auto *o : img_objs_) if (o) lv_image_set_src(o, nullptr);
    for (auto *c : cells_) if (c) lv_obj_del(c);
    cells_.clear();
    img_objs_.clear();
    skeletons_.clear();
    images_.clear();
}

void BackgroundGalleryView::RebuildGrid() {
    BuildBody();
}

void BackgroundGalleryView::BuildBody() {
    const auto &p = jetson::UiTheme::Instance().Palette();

    lv_obj_set_flex_flow(body_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(body_, 12, 0);
    lv_obj_add_flag(body_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(body_, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_scroll_dir(body_, LV_DIR_HOR);

    ClearGrid();
    files_ = home::ListBackgroundFiles();
    images_.resize(files_.size());
    cells_.resize(files_.size(), nullptr);
    img_objs_.resize(files_.size(), nullptr);
    skeletons_.resize(files_.size(), nullptr);

    for (size_t i = 0; i < files_.size(); ++i) {
        int cellW = (i % 4 == 0) ? 260 : 200;     // occasional bento "feature" tile
        int thumbH = cellW * 3 / 5;                // thumbs are 400x240 (3:5 -> 0.6 wide)
        int cellH = thumbH + 34;

        auto *cell = lv_obj_create(body_);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, cellW, cellH);
        lv_obj_set_style_radius(cell, 14, 0);
        lv_obj_set_style_bg_color(cell, Color(p.row), 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        lv_obj_set_style_clip_corner(cell, true, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);

        // Thumbnail (filled later by the progressive loader).
        auto *img_obj = lv_image_create(cell);
        lv_obj_set_size(img_obj, cellW, thumbH);
        lv_obj_set_style_bg_color(img_obj, Color(p.button), 0);
        lv_obj_set_style_bg_opa(img_obj, LV_OPA_COVER, 0);
        lv_obj_align(img_obj, LV_ALIGN_TOP_MID, 0, 0);

        // Skeleton placeholder shown until the image loads.
        auto *skel = lv_label_create(cell);
        lv_obj_set_style_text_font(skel, &BUILTIN_ICON_FONT, 0);
        lv_obj_set_style_text_color(skel, Color(p.sub_text), 0);
        lv_label_set_text(skel, LV_SYMBOL_IMAGE);
        lv_obj_align(skel, LV_ALIGN_TOP_MID, 0, thumbH / 2 - 12);

        // Caption (filename without extension).
        auto *name = lv_label_create(cell);
        lv_obj_set_style_text_font(name, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(name, Color(p.sub_text), 0);
        std::string fn = files_[i];
        size_t dot = fn.rfind('.');
        if (dot != std::string::npos) fn = fn.substr(0, dot);
        lv_label_set_text(name, fn.c_str());
        lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -6);

        auto *ctx = new CellCtx{this, i};
        lv_obj_add_event_cb(cell, OnCellClicked, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(cell, OnCellDeleted, LV_EVENT_DELETE, ctx);
        cells_[i] = cell;
        img_objs_[i] = img_obj;
        skeletons_[i] = skel;
    }
    RefreshHighlights();

    // Progressive load: one thumbnail per tick so the gallery opens instantly
    // and skeletons fill in smoothly (no 10-image decode stall on open).
    load_idx_ = 0;
    if (load_timer_) { lv_timer_del(load_timer_); load_timer_ = nullptr; }
    if (!files_.empty()) {
        load_timer_ = lv_timer_create(OnLoadTimer, 40, this);
    } else {
        SetStatus("Không có ảnh nào trong assets/backgrounds");
    }
}

void BackgroundGalleryView::LoadNextImage() {
    if (load_idx_ >= files_.size()) {
        if (load_timer_) { lv_timer_del(load_timer_); load_timer_ = nullptr; }
        return;
    }
    size_t i = load_idx_++;
    auto img = LvglImageFromFile(home::ThumbPath(files_[i]));
    if (img && img_objs_[i]) {
        int cellW = lv_obj_get_width(img_objs_[i]);
        if (cellW <= 0) cellW = 200;
        // Thumbs are 400x240; fit to cell width, crop vertical overflow via clip.
        lv_image_set_scale(img_objs_[i], (uint16_t)(cellW * 256 / 400));
        lv_image_set_src(img_objs_[i], img->image_dsc());
        images_[i] = std::move(img);
        if (skeletons_[i]) { lv_obj_del(skeletons_[i]); skeletons_[i] = nullptr; }
    }
}

void BackgroundGalleryView::RefreshHighlights() {
    const auto &p = jetson::UiTheme::Instance().Palette();
    for (size_t i = 0; i < cells_.size(); ++i) {
        if (!cells_[i]) continue;
        bool desktop = (!current_file_.empty() && files_[i] == current_file_);
        bool sleep = (!sleep_file_.empty() && files_[i] == sleep_file_);
        if (desktop) {
            lv_obj_set_style_border_width(cells_[i], 3, 0);
            lv_obj_set_style_border_color(cells_[i], Color(p.accent), 0);
        } else if (sleep) {
            lv_obj_set_style_border_width(cells_[i], 3, 0);
            lv_obj_set_style_border_color(cells_[i], lv_color_white(), 0);
        } else {
            lv_obj_set_style_border_width(cells_[i], 0, 0);
        }
    }
}

void BackgroundGalleryView::OpenPopup(size_t index) {
    if (popup_) ClosePopup();
    if (index >= files_.size()) return;
    popup_index_ = index;
    const auto &p = jetson::UiTheme::Instance().Palette();

    popup_ = lv_obj_create(overlay_);
    lv_obj_remove_style_all(popup_);
    lv_obj_set_size(popup_, width_, height_);
    lv_obj_set_style_bg_color(popup_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(popup_, LV_OPA_50, 0);
    lv_obj_add_flag(popup_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(popup_, OnPopupDismiss, LV_EVENT_CLICKED, this);

    popup_card_ = lv_obj_create(popup_);
    lv_obj_remove_style_all(popup_card_);
    lv_obj_set_size(popup_card_, 280, 176);
    lv_obj_set_style_bg_color(popup_card_, Color(p.row), 0);
    lv_obj_set_style_bg_opa(popup_card_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(popup_card_, 16, 0);
    lv_obj_set_style_pad_all(popup_card_, 12, 0);
    lv_obj_set_style_pad_row(popup_card_, 8, 0);
    lv_obj_set_flex_flow(popup_card_, LV_FLEX_FLOW_COLUMN);
    lv_obj_align(popup_card_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(popup_card_, LV_OBJ_FLAG_CLICKABLE);

    auto add_btn = [&](const char *text, lv_event_cb_t cb, uint32_t fg) {
        auto *b = lv_button_create(popup_card_);
        lv_obj_set_width(b, 256);
        lv_obj_set_style_bg_color(b, Color(p.button), 0);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, this);
        auto *lbl = lv_label_create(b);
        lv_obj_set_style_text_font(lbl, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(lbl, Color(fg), 0);
        lv_label_set_text(lbl, text);
        lv_obj_center(lbl);
    };
    add_btn("Đặt hình nền desktop", OnPopupDesktop, p.text);
    add_btn("Đặt hình nền sleep screen", OnPopupSleep, p.text);
    add_btn("Xóa ảnh", OnPopupDelete, 0xFF5F57);
}

void BackgroundGalleryView::ClosePopup() {
    if (popup_) { lv_obj_del(popup_); popup_ = nullptr; popup_card_ = nullptr; }
}

void BackgroundGalleryView::DeleteImage(size_t index) {
    if (index >= files_.size()) return;
    std::string file = files_[index];
    ClosePopup();
    std::remove(home::BackgroundPath(file).c_str());
    std::remove(home::ThumbPath(file).c_str());
    if (current_file_ == file) current_file_.clear();
    if (sleep_file_ == file) sleep_file_.clear();
    if (on_changed_) on_changed_();
    RebuildGrid();
    SetStatus("Đã xóa ảnh");
}

void BackgroundGalleryView::OnStart() {
    SetStatus("Chạm ảnh để tùy chỉnh nền");
}

void BackgroundGalleryView::OnCellClicked(lv_event_t *e) {
    LvLockGuard lock;
    auto *ctx = static_cast<CellCtx *>(lv_event_get_user_data(e));
    ctx->self->OpenPopup(ctx->index);
}

void BackgroundGalleryView::OnCellDeleted(lv_event_t *e) {
    auto *ctx = static_cast<CellCtx *>(lv_event_get_user_data(e));
    delete ctx;
}

void BackgroundGalleryView::OnPopupDismiss(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<BackgroundGalleryView *>(lv_event_get_user_data(e));
    // Only dismiss when the click landed on the backdrop itself, not on the
    // card or a button (those have their own handlers).
    if (lv_event_get_target(e) == self->popup_) self->ClosePopup();
}

void BackgroundGalleryView::OnPopupDesktop(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<BackgroundGalleryView *>(lv_event_get_user_data(e));
    if (self->popup_index_ < self->files_.size()) {
        const std::string &file = self->files_[self->popup_index_];
        if (self->on_select_) self->on_select_(file);
        self->current_file_ = file;
        self->RefreshHighlights();
    }
    self->ClosePopup();
    self->SetStatus("Đã đặt hình nền desktop");
}

void BackgroundGalleryView::OnPopupSleep(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<BackgroundGalleryView *>(lv_event_get_user_data(e));
    if (self->popup_index_ < self->files_.size()) {
        const std::string &file = self->files_[self->popup_index_];
        if (self->on_sleep_) self->on_sleep_(file);
        self->sleep_file_ = file;
        self->RefreshHighlights();
    }
    self->ClosePopup();
    self->SetStatus("Đã đặt hình nền sleep screen");
}

void BackgroundGalleryView::OnPopupDelete(lv_event_t *e) {
    LvLockGuard lock;
    auto *self = static_cast<BackgroundGalleryView *>(lv_event_get_user_data(e));
    self->DeleteImage(self->popup_index_);
}

void BackgroundGalleryView::OnLoadTimer(lv_timer_t *t) {
    auto *self = static_cast<BackgroundGalleryView *>(lv_timer_get_user_data(t));
    // Runs on the LVGL handler thread, not the home refresh thread, so take the
    // LVGL lock to avoid racing the 1 Hz status refresh.
    LvLockGuard lock;
    self->LoadNextImage();
}

} // namespace home