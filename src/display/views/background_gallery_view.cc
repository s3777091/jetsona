#include "display/views/background_gallery_view.h"

#include "display/common/backgrounds.h"
#include "display/common/lvgl_utils.h"
#include "display/common/trash_store.h"
#include "display/theme/ui_theme.h"
#include "fonts.h"
#include "settings.h"

#include <lvgl.h>

#include <algorithm>
#include <climits>
#include <cstdlib>

namespace home {

using jetson::ui::Color;
using jetson::ui::LvglLockGuard;

namespace {
constexpr int kCellWidth = 238;
constexpr int kThumbHeight = kCellWidth * 3 / 5;
constexpr int kCellHeight = kThumbHeight + 34;
constexpr int kCellGap = 14;
constexpr int kNormalScale = 256;
constexpr int kSelectedScale = 274; // +7%, enough to read without crowding
constexpr uint32_t kSelectionAnimMs = 170;
constexpr uint32_t kShareAnimMs = 180;
} // namespace

BackgroundGalleryView::BackgroundGalleryView(lv_obj_t *parent, int width,
                                               int height, ClosedCb on_closed)
    : OverlayView(parent, width, height, "Ảnh nền", std::move(on_closed)) {
    Settings settings("display", false);
    current_file_ = settings.GetString("ds02_background_file", "");
    sleep_file_ = settings.GetString("ds02_sleep_bg_file", "");
    BuildBody();
}

BackgroundGalleryView::~BackgroundGalleryView() {
    // Detach every descriptor before images_ is destroyed; the base class
    // deletes the overlay only after this derived destructor has returned.
    LvglLockGuard lock;
    if (load_timer_) {
        lv_timer_del(load_timer_);
        load_timer_ = nullptr;
    }
    if (share_menu_) {
        lv_anim_delete(share_menu_, OnShareMenuOpa);
        lv_anim_delete(share_menu_, OnShareMenuY);
    }
    for (auto *cell : cells_)
        if (cell) lv_anim_delete(cell, OnCellScale);
    for (auto *image : img_objs_)
        if (image) lv_image_set_src(image, nullptr);
    // CellCtx instances are released by OnCellDeleted when the base class
    // deletes overlay_ (or earlier when ClearGrid rebuilds the carousel).
}

void BackgroundGalleryView::BuildBody() {
    const auto &palette = jetson::UiTheme::Instance().Palette();
    const bool had_cells = !cells_.empty();
    const size_t preferred_index = selected_index_;
    rebuilding_ = true;

    if (!carousel_) {
        lv_obj_set_flex_flow(body_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(body_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(body_, 8, 0);
        lv_obj_set_style_pad_row(body_, 6, 0);
        lv_obj_clear_flag(body_, LV_OBJ_FLAG_SCROLLABLE);

        carousel_ = lv_obj_create(body_);
        lv_obj_remove_style_all(carousel_);
        lv_obj_set_width(carousel_, lv_pct(100));
        lv_obj_set_flex_grow(carousel_, 1);
        lv_obj_set_flex_flow(carousel_, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(carousel_, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(carousel_, kCellGap, 0);
        lv_obj_set_scroll_dir(carousel_, LV_DIR_HOR);
        lv_obj_set_scroll_snap_x(carousel_, LV_SCROLL_SNAP_CENTER);
        lv_obj_set_scrollbar_mode(carousel_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_add_flag(carousel_,
                        (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE |
                                        LV_OBJ_FLAG_SCROLL_ONE));
        lv_obj_add_event_cb(carousel_, OnCarouselScroll, LV_EVENT_SCROLL, this);
        lv_obj_add_event_cb(carousel_, OnCarouselScroll, LV_EVENT_SCROLL_END,
                            this);

        action_row_ = lv_obj_create(body_);
        lv_obj_remove_style_all(action_row_);
        lv_obj_set_size(action_row_, lv_pct(100), 56);
        lv_obj_set_style_pad_left(action_row_, 12, 0);
        lv_obj_set_style_pad_right(action_row_, 12, 0);
        lv_obj_set_flex_flow(action_row_, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(action_row_, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(action_row_, LV_OBJ_FLAG_SCROLLABLE);

        auto make_action = [&](const char *symbol, uint32_t icon_color,
                               lv_event_cb_t callback) {
            auto *button = lv_button_create(action_row_);
            lv_obj_set_size(button, 50, 50);
            lv_obj_set_style_radius(button, 14, 0);
            lv_obj_set_style_bg_color(button, Color(palette.row), 0);
            lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(button, 1, 0);
            lv_obj_set_style_border_color(button, Color(palette.border), 0);
            lv_obj_set_style_shadow_color(button, lv_color_black(), 0);
            lv_obj_set_style_shadow_width(button, 10, 0);
            lv_obj_set_style_shadow_opa(button, LV_OPA_30, 0);
            lv_obj_set_style_bg_color(button, Color(palette.button),
                                      LV_STATE_PRESSED);
            auto *icon = lv_label_create(button);
            lv_obj_set_style_text_font(icon, &BUILTIN_ICON_FONT, 0);
            lv_obj_set_style_text_color(icon, Color(icon_color), 0);
            lv_label_set_text(icon, symbol);
            lv_obj_center(icon);
            lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, this);
            return button;
        };

        // Mirror the supplied reference: share at the far left, trash at the
        // far right, both outside the moving carousel.
        share_btn_ = make_action(LV_SYMBOL_UPLOAD, palette.text,
                                 OnShareClicked);
        trash_btn_ = make_action(LV_SYMBOL_TRASH, 0xff5f57,
                                 OnTrashClicked);

        // A non-modal popover. Because Share sits at the left edge, this is the
        // reverse of the reference menu: it grows upward and inward (right),
        // never off the screen or over the Trash button.
        share_menu_ = lv_obj_create(body_);
        lv_obj_remove_style_all(share_menu_);
        lv_obj_set_size(share_menu_, 318, 110);
        lv_obj_align(share_menu_, LV_ALIGN_BOTTOM_LEFT, 12, -64);
        lv_obj_set_style_bg_color(share_menu_, Color(palette.row), 0);
        lv_obj_set_style_bg_opa(share_menu_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(share_menu_, 18, 0);
        lv_obj_set_style_border_width(share_menu_, 1, 0);
        lv_obj_set_style_border_color(share_menu_, Color(palette.border), 0);
        lv_obj_set_style_shadow_color(share_menu_, lv_color_black(), 0);
        lv_obj_set_style_shadow_width(share_menu_, 20, 0);
        lv_obj_set_style_shadow_opa(share_menu_, LV_OPA_50, 0);
        lv_obj_set_style_pad_all(share_menu_, 8, 0);
        lv_obj_set_style_pad_row(share_menu_, 6, 0);
        lv_obj_set_flex_flow(share_menu_, LV_FLEX_FLOW_COLUMN);
        lv_obj_clear_flag(share_menu_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(share_menu_,
                        (lv_obj_flag_t)(LV_OBJ_FLAG_FLOATING |
                                        LV_OBJ_FLAG_HIDDEN));
        lv_obj_set_style_opa(share_menu_, LV_OPA_0, 0);
        lv_obj_set_style_translate_y(share_menu_, 14, 0);

        auto make_share_choice = [&](const char *symbol, const char *text,
                                     lv_event_cb_t callback) {
            auto *button = lv_button_create(share_menu_);
            lv_obj_set_size(button, lv_pct(100), 44);
            lv_obj_set_style_radius(button, 11, 0);
            lv_obj_set_style_bg_color(button, Color(palette.button), 0);
            lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
            lv_obj_set_style_shadow_width(button, 0, 0);
            auto *icon = lv_label_create(button);
            lv_obj_set_style_text_font(icon, &BUILTIN_ICON_FONT, 0);
            lv_obj_set_style_text_color(icon, Color(palette.accent), 0);
            lv_label_set_text(icon, symbol);
            lv_obj_align(icon, LV_ALIGN_LEFT_MID, 12, 0);
            auto *label = lv_label_create(button);
            lv_obj_set_style_text_font(label, &BUILTIN_SMALL_TEXT_FONT, 0);
            lv_obj_set_style_text_color(label, Color(palette.text), 0);
            lv_label_set_text(label, text);
            lv_obj_align(label, LV_ALIGN_LEFT_MID, 48, 0);
            lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, this);
        };
        make_share_choice(LV_SYMBOL_HOME, "Đặt hình nền desktop",
                          OnShareDesktop);
        make_share_choice(LV_SYMBOL_EYE_CLOSE, "Đặt hình nền sleep mode",
                          OnShareSleep);
    }

    HideShareMenu(false);
    ClearGrid();
    files_ = jetson::ui::backgrounds::ListBackgroundFiles();
    images_.resize(files_.size());
    cells_.resize(files_.size(), nullptr);
    img_objs_.resize(files_.size(), nullptr);
    skeletons_.resize(files_.size(), nullptr);
    selection_initialized_ = false;

    // End padding allows even the first and last card to snap exactly to the
    // viewport center. Every card is the same size, so both sides match.
    const int viewport_width = std::max(1, width_ - 16);
    const int end_padding = std::max(8, (viewport_width - kCellWidth) / 2);
    lv_obj_set_style_pad_left(carousel_, end_padding, 0);
    lv_obj_set_style_pad_right(carousel_, end_padding, 0);

    for (size_t index = 0; index < files_.size(); ++index) {
        auto *cell = lv_obj_create(carousel_);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, kCellWidth, kCellHeight);
        lv_obj_set_style_radius(cell, 14, 0);
        lv_obj_set_style_bg_color(cell, Color(palette.row), 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        lv_obj_set_style_clip_corner(cell, true, 0);
        lv_obj_set_style_transform_pivot_x(cell, lv_pct(50), 0);
        lv_obj_set_style_transform_pivot_y(cell, lv_pct(50), 0);
        lv_obj_set_style_transform_scale(cell, kNormalScale, 0);
        lv_obj_set_style_shadow_color(cell, Color(palette.accent), 0);
        lv_obj_set_style_shadow_width(cell, 0, 0);
        lv_obj_set_style_shadow_opa(cell, LV_OPA_0, 0);
        lv_obj_set_style_opa(cell, LV_OPA_80, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);

        // Thumbnail (filled later by the progressive loader).
        auto *image = lv_image_create(cell);
        lv_obj_set_size(image, kCellWidth, kThumbHeight);
        lv_obj_set_style_bg_color(image, Color(palette.button), 0);
        lv_obj_set_style_bg_opa(image, LV_OPA_COVER, 0);
        lv_obj_align(image, LV_ALIGN_TOP_MID, 0, 0);

        auto *skeleton = lv_label_create(cell);
        lv_obj_set_style_text_font(skeleton, &BUILTIN_ICON_FONT, 0);
        lv_obj_set_style_text_color(skeleton, Color(palette.sub_text), 0);
        lv_label_set_text(skeleton, LV_SYMBOL_IMAGE);
        lv_obj_align(skeleton, LV_ALIGN_TOP_MID, 0,
                     kThumbHeight / 2 - 12);

        auto *name = lv_label_create(cell);
        lv_obj_set_style_text_font(name, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(name, Color(palette.sub_text), 0);
        lv_obj_set_width(name, kCellWidth - 16);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        std::string filename = files_[index];
        const size_t dot = filename.rfind('.');
        if (dot != std::string::npos) filename.resize(dot);
        lv_label_set_text(name, filename.c_str());
        lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -6);

        auto *context = new CellCtx{this, index};
        lv_obj_add_event_cb(cell, OnCellClicked, LV_EVENT_CLICKED, context);
        lv_obj_add_event_cb(cell, OnCellDeleted, LV_EVENT_DELETE, context);
        cells_[index] = cell;
        img_objs_[index] = image;
        skeletons_[index] = skeleton;
    }

    if (files_.empty()) {
        selected_index_ = 0;
        SetActionsEnabled(false);
        SetStatus("Không có ảnh nào trong assets/backgrounds");
    } else {
        if (had_cells) {
            selected_index_ = std::min(preferred_index, files_.size() - 1);
        } else {
            auto current = std::find(files_.begin(), files_.end(), current_file_);
            selected_index_ = current == files_.end()
                                  ? 0
                                  : static_cast<size_t>(current - files_.begin());
        }
        SetActionsEnabled(true);
        lv_obj_update_layout(overlay_);
        SetSelectedIndex(selected_index_, false);
        CenterSelected(LV_ANIM_OFF);
    }
    RefreshHighlights();
    rebuilding_ = false;

    // Progressive load: one thumbnail per tick so the gallery opens instantly
    // and skeletons fill in smoothly (no multi-image decode stall on open).
    load_idx_ = 0;
    if (load_timer_) {
        lv_timer_del(load_timer_);
        load_timer_ = nullptr;
    }
    if (!files_.empty()) load_timer_ = lv_timer_create(OnLoadTimer, 40, this);
}

void BackgroundGalleryView::ClearGrid() {
    if (load_timer_) {
        lv_timer_del(load_timer_);
        load_timer_ = nullptr;
    }
    for (auto *image : img_objs_)
        if (image) lv_image_set_src(image, nullptr);
    for (auto *cell : cells_) {
        if (!cell) continue;
        lv_anim_delete(cell, OnCellScale);
        lv_obj_del(cell);
    }
    cells_.clear();
    img_objs_.clear();
    skeletons_.clear();
    images_.clear();
}

void BackgroundGalleryView::RebuildGrid() {
    BuildBody();
}

void BackgroundGalleryView::LoadNextImage() {
    if (load_idx_ >= files_.size()) {
        if (load_timer_) {
            lv_timer_del(load_timer_);
            load_timer_ = nullptr;
        }
        return;
    }
    const size_t index = load_idx_++;
    auto image = LvglImageFromFile(
        jetson::ui::backgrounds::ThumbPath(files_[index]));
    if (image && img_objs_[index]) {
        // Thumbs are 400x240; fit to the fixed carousel card width.
        lv_image_set_scale(img_objs_[index],
                           static_cast<uint16_t>(kCellWidth * 256 / 400));
        lv_image_set_src(img_objs_[index], image->image_dsc());
        images_[index] = std::move(image);
        if (skeletons_[index]) {
            lv_obj_del(skeletons_[index]);
            skeletons_[index] = nullptr;
        }
    }
}

void BackgroundGalleryView::RefreshHighlights() {
    const auto &palette = jetson::UiTheme::Instance().Palette();
    for (size_t index = 0; index < cells_.size(); ++index) {
        if (!cells_[index]) continue;
        const bool desktop = !current_file_.empty() &&
                             files_[index] == current_file_;
        const bool sleep = !sleep_file_.empty() &&
                           files_[index] == sleep_file_;
        if (desktop) {
            lv_obj_set_style_border_width(cells_[index], 3, 0);
            lv_obj_set_style_border_color(cells_[index],
                                          Color(palette.accent), 0);
        } else if (sleep) {
            lv_obj_set_style_border_width(cells_[index], 3, 0);
            lv_obj_set_style_border_color(cells_[index], lv_color_white(), 0);
        } else {
            lv_obj_set_style_border_width(cells_[index], 0, 0);
        }
    }
}

void BackgroundGalleryView::AnimateCellScale(lv_obj_t *cell, int target,
                                               bool animated) {
    if (!cell) return;
    lv_anim_delete(cell, OnCellScale);
    if (!animated) {
        lv_obj_set_style_transform_scale(cell, target, 0);
        return;
    }
    const int current = lv_obj_get_style_transform_scale_x(cell, 0);
    if (current == target) return;
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, cell);
    lv_anim_set_exec_cb(&animation, OnCellScale);
    lv_anim_set_values(&animation, current, target);
    lv_anim_set_time(&animation, kSelectionAnimMs);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
    lv_anim_start(&animation);
}

void BackgroundGalleryView::SetSelectedIndex(size_t index, bool animated) {
    if (index >= cells_.size()) return;
    if (selection_initialized_ && selected_index_ == index) return;

    const size_t old_index = selected_index_;
    const bool had_selection = selection_initialized_ &&
                               old_index < cells_.size();
    selected_index_ = index;
    selection_initialized_ = true;

    if (had_selection) {
        AnimateCellScale(cells_[old_index], kNormalScale, animated);
        lv_obj_set_style_shadow_width(cells_[old_index], 0, 0);
        lv_obj_set_style_shadow_opa(cells_[old_index], LV_OPA_0, 0);
        lv_obj_set_style_opa(cells_[old_index], LV_OPA_80, 0);
    }

    AnimateCellScale(cells_[selected_index_], kSelectedScale, animated);
    lv_obj_set_style_shadow_width(cells_[selected_index_], 16, 0);
    lv_obj_set_style_shadow_opa(cells_[selected_index_], LV_OPA_40, 0);
    lv_obj_set_style_opa(cells_[selected_index_], LV_OPA_COVER, 0);
}

void BackgroundGalleryView::UpdateSelectedFromScroll(bool animated) {
    if (!carousel_ || cells_.empty()) return;
    lv_area_t viewport{};
    lv_obj_get_coords(carousel_, &viewport);
    const int viewport_center = (viewport.x1 + viewport.x2) / 2;
    int best_distance = INT_MAX;
    size_t nearest = selected_index_ < cells_.size() ? selected_index_ : 0;

    for (size_t index = 0; index < cells_.size(); ++index) {
        if (!cells_[index]) continue;
        lv_area_t area{};
        lv_obj_get_coords(cells_[index], &area);
        const int center = (area.x1 + area.x2) / 2;
        const int distance = std::abs(center - viewport_center);
        if (distance < best_distance) {
            best_distance = distance;
            nearest = index;
        }
    }
    SetSelectedIndex(nearest, animated);
}

void BackgroundGalleryView::CenterSelected(lv_anim_enable_t animated) {
    if (selected_index_ >= cells_.size() || !cells_[selected_index_]) return;
    // The carousel's CENTER snap mode makes scroll_to_view align the card's
    // center rather than merely bringing its nearest edge on-screen.
    lv_obj_scroll_to_view(cells_[selected_index_], animated);
}

void BackgroundGalleryView::SetActionsEnabled(bool enabled) {
    for (auto *button : {share_btn_, trash_btn_}) {
        if (!button) continue;
        if (enabled) lv_obj_clear_state(button, LV_STATE_DISABLED);
        else lv_obj_add_state(button, LV_STATE_DISABLED);
    }
    if (!enabled) HideShareMenu(false);
}

void BackgroundGalleryView::ShowShareMenu() {
    if (!share_menu_ || files_.empty()) return;
    share_menu_visible_ = true;
    lv_anim_delete(share_menu_, OnShareMenuOpa);
    lv_anim_delete(share_menu_, OnShareMenuY);
    lv_obj_clear_flag(share_menu_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(share_menu_);
    lv_obj_set_style_opa(share_menu_, LV_OPA_0, 0);
    lv_obj_set_style_translate_y(share_menu_, 14, 0);

    lv_anim_t opacity;
    lv_anim_init(&opacity);
    lv_anim_set_var(&opacity, share_menu_);
    lv_anim_set_exec_cb(&opacity, OnShareMenuOpa);
    lv_anim_set_values(&opacity, LV_OPA_0, LV_OPA_COVER);
    lv_anim_set_time(&opacity, kShareAnimMs);
    lv_anim_set_path_cb(&opacity, lv_anim_path_ease_out);
    lv_anim_start(&opacity);

    lv_anim_t translate;
    lv_anim_init(&translate);
    lv_anim_set_var(&translate, share_menu_);
    lv_anim_set_exec_cb(&translate, OnShareMenuY);
    lv_anim_set_values(&translate, 14, 0);
    lv_anim_set_time(&translate, kShareAnimMs);
    lv_anim_set_path_cb(&translate, lv_anim_path_ease_out);
    lv_anim_start(&translate);
}

void BackgroundGalleryView::HideShareMenu(bool animated) {
    if (!share_menu_) return;
    lv_anim_delete(share_menu_, OnShareMenuOpa);
    lv_anim_delete(share_menu_, OnShareMenuY);
    share_menu_visible_ = false;
    if (!animated || lv_obj_has_flag(share_menu_, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_set_style_opa(share_menu_, LV_OPA_0, 0);
        lv_obj_set_style_translate_y(share_menu_, 14, 0);
        lv_obj_add_flag(share_menu_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_anim_t opacity;
    lv_anim_init(&opacity);
    lv_anim_set_var(&opacity, share_menu_);
    lv_anim_set_exec_cb(&opacity, OnShareMenuOpa);
    lv_anim_set_values(&opacity, lv_obj_get_style_opa(share_menu_, 0),
                       LV_OPA_0);
    lv_anim_set_time(&opacity, kShareAnimMs - 40);
    lv_anim_set_path_cb(&opacity, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&opacity, OnShareMenuHidden);
    lv_anim_start(&opacity);

    lv_anim_t translate;
    lv_anim_init(&translate);
    lv_anim_set_var(&translate, share_menu_);
    lv_anim_set_exec_cb(&translate, OnShareMenuY);
    lv_anim_set_values(&translate,
                       lv_obj_get_style_translate_y(share_menu_, 0), 10);
    lv_anim_set_time(&translate, kShareAnimMs - 40);
    lv_anim_set_path_cb(&translate, lv_anim_path_ease_in);
    lv_anim_start(&translate);
}

void BackgroundGalleryView::MoveImageToTrash(size_t index) {
    if (index >= files_.size()) return;
    const std::string file = files_[index];
    HideShareMenu(false);
    if (!jetson::ui::trash::MoveBackgroundToTrash(file)) {
        SetStatus("Không thể chuyển ảnh vào Thùng rác");
        return;
    }

    if (current_file_ == file) current_file_.clear();
    if (sleep_file_ == file) sleep_file_.clear();
    selected_index_ = index; // same slot selects the next card after rebuild
    if (on_changed_) {
        on_changed_();
        // Reload fallback selections written by
        // Ds02HomeDisplay::ReloadBackgrounds.
        Settings settings("display", false);
        current_file_ = settings.GetString("ds02_background_file", "");
        sleep_file_ = settings.GetString("ds02_sleep_bg_file", "");
    }
    RebuildGrid();
    SetStatus("Đã chuyển ảnh vào Thùng rác");
}

void BackgroundGalleryView::OnStart() {
    lv_obj_update_layout(overlay_);
    CenterSelected(LV_ANIM_OFF);
    UpdateSelectedFromScroll(false);
    SetStatus("Vuốt để chọn ảnh giữa, sau đó dùng Share hoặc Trash");
}

void BackgroundGalleryView::OnCellClicked(lv_event_t *event) {
    LvglLockGuard lock;
    auto *context = static_cast<CellCtx *>(lv_event_get_user_data(event));
    if (!context || !context->self ||
        context->index >= context->self->cells_.size()) {
        return;
    }
    context->self->HideShareMenu(false);
    // A tap no longer opens a modal; it simply glides that card into the
    // selected center position.
    lv_obj_scroll_to_view(context->self->cells_[context->index], LV_ANIM_ON);
}

void BackgroundGalleryView::OnCellDeleted(lv_event_t *event) {
    delete static_cast<CellCtx *>(lv_event_get_user_data(event));
}

void BackgroundGalleryView::OnCarouselScroll(lv_event_t *event) {
    LvglLockGuard lock;
    auto *self = static_cast<BackgroundGalleryView *>(
        lv_event_get_user_data(event));
    if (!self || self->rebuilding_) return;
    if (self->share_menu_visible_) self->HideShareMenu(false);
    self->UpdateSelectedFromScroll(true);
}

void BackgroundGalleryView::OnShareClicked(lv_event_t *event) {
    LvglLockGuard lock;
    auto *self = static_cast<BackgroundGalleryView *>(
        lv_event_get_user_data(event));
    if (!self || self->files_.empty()) return;
    self->UpdateSelectedFromScroll(false);
    if (self->share_menu_visible_) self->HideShareMenu(true);
    else self->ShowShareMenu();
}

void BackgroundGalleryView::OnTrashClicked(lv_event_t *event) {
    LvglLockGuard lock;
    auto *self = static_cast<BackgroundGalleryView *>(
        lv_event_get_user_data(event));
    if (!self || self->files_.empty()) return;
    self->UpdateSelectedFromScroll(false);
    self->MoveImageToTrash(self->selected_index_);
}

void BackgroundGalleryView::OnShareDesktop(lv_event_t *event) {
    LvglLockGuard lock;
    auto *self = static_cast<BackgroundGalleryView *>(
        lv_event_get_user_data(event));
    if (!self || self->files_.empty()) return;
    self->UpdateSelectedFromScroll(false);
    const std::string file = self->files_[self->selected_index_];
    if (self->on_select_) self->on_select_(file);
    self->current_file_ = file;
    self->RefreshHighlights();
    self->HideShareMenu(true);
    self->SetStatus("Đã đặt hình nền desktop");
}

void BackgroundGalleryView::OnShareSleep(lv_event_t *event) {
    LvglLockGuard lock;
    auto *self = static_cast<BackgroundGalleryView *>(
        lv_event_get_user_data(event));
    if (!self || self->files_.empty()) return;
    self->UpdateSelectedFromScroll(false);
    const std::string file = self->files_[self->selected_index_];
    if (self->on_sleep_) self->on_sleep_(file);
    self->sleep_file_ = file;
    self->RefreshHighlights();
    self->HideShareMenu(true);
    self->SetStatus("Đã đặt hình nền sleep mode");
}

void BackgroundGalleryView::OnLoadTimer(lv_timer_t *timer) {
    auto *self = static_cast<BackgroundGalleryView *>(
        lv_timer_get_user_data(timer));
    LvglLockGuard lock;
    if (self) self->LoadNextImage();
}

void BackgroundGalleryView::OnCellScale(void *var, int32_t value) {
    lv_obj_set_style_transform_scale(static_cast<lv_obj_t *>(var), value, 0);
}

void BackgroundGalleryView::OnShareMenuOpa(void *var, int32_t value) {
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(var),
                         static_cast<lv_opa_t>(value), 0);
}

void BackgroundGalleryView::OnShareMenuY(void *var, int32_t value) {
    lv_obj_set_style_translate_y(static_cast<lv_obj_t *>(var), value, 0);
}

void BackgroundGalleryView::OnShareMenuHidden(lv_anim_t *animation) {
    if (animation && animation->var)
        lv_obj_add_flag(static_cast<lv_obj_t *>(animation->var),
                        LV_OBJ_FLAG_HIDDEN);
}

} // namespace home
