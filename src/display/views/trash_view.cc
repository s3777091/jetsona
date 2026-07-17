#include "display/views/trash_view.h"

#include "display/common/backgrounds.h"
#include "display/common/lvgl_utils.h"
#include "display/core/app_icons.h"
#include "display/theme/ui_theme.h"
#include "fonts.h"

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace {

using jetson::ui::Color;

bool PngSize(const std::string &path, int *width, int *height) {
    *width = 0;
    *height = 0;
    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    unsigned char header[24] = {};
    const size_t read = std::fread(header, 1, sizeof(header), file);
    std::fclose(file);
    if (read < sizeof(header) || header[0] != 0x89 || header[1] != 0x50 ||
        header[2] != 0x4e || header[3] != 0x47) {
        return false;
    }
    *width = (header[16] << 24) | (header[17] << 16) |
             (header[18] << 8) | header[19];
    *height = (header[20] << 24) | (header[21] << 16) |
              (header[22] << 8) | header[23];
    return *width > 0 && *height > 0;
}

int PngScaleToFit(const std::string &path, int max_width, int max_height) {
    int width = 0;
    int height = 0;
    if (!PngSize(path, &width, &height)) return 256;
    return std::max(16, std::min(max_width * 256 / width,
                                 max_height * 256 / height));
}

std::string FormatSize(long bytes) {
    char value[48];
    if (bytes >= 1024L * 1024L) {
        std::snprintf(value, sizeof(value), "%.1f MB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024L) {
        std::snprintf(value, sizeof(value), "%.1f KB",
                      static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(value, sizeof(value), "%ld B", bytes);
    }
    return value;
}

std::string FormatDeletedTime(std::time_t value) {
    char text[48] = {};
    std::tm local {};
#if defined(_WIN32)
    localtime_s(&local, &value);
#else
    localtime_r(&value, &local);
#endif
    if (std::strftime(text, sizeof(text), "%d/%m/%Y %H:%M", &local) == 0) {
        return "—";
    }
    return text;
}

} // namespace

namespace home {

using jetson::ui::LvglLockGuard;

TrashView::TrashView(lv_obj_t *parent, int width, int height, ClosedCb on_closed)
    : OverlayView(parent, width, height, "Thùng rác", std::move(on_closed)) {
    empty_btn_ = lv_button_create(header_);
    lv_obj_set_size(empty_btn_, 28, 28);
    lv_obj_align(empty_btn_, LV_ALIGN_BOTTOM_RIGHT, -12, -1);
    lv_obj_set_style_radius(empty_btn_, 8, 0);
    lv_obj_set_style_bg_color(
        empty_btn_, Color(jetson::UiTheme::Instance().Palette().button), 0);
    lv_obj_set_style_pad_all(empty_btn_, 0, 0);
    lv_obj_add_event_cb(empty_btn_, OnEmptyClicked, LV_EVENT_CLICKED, this);
    auto *icon = jetson::ui::CreateAppIcon(empty_btn_, "empty-trash", 18);
    lv_obj_center(icon);
    BuildBody();
}

TrashView::~TrashView() {
    LvglLockGuard lock;
    if (load_timer_) { lv_timer_del(load_timer_); load_timer_ = nullptr; }
    if (popup_) { lv_obj_del(popup_); popup_ = nullptr; }
    ClearBody();
}

void TrashView::ClearBody() {
    for (auto *image : image_objs_) {
        if (image) lv_image_set_src(image, nullptr);
    }
    if (body_) lv_obj_clean(body_);
    cells_.clear();
    image_objs_.clear();
    skeletons_.clear();
    images_.clear();
}

void TrashView::BuildBody() {
    if (load_timer_) { lv_timer_del(load_timer_); load_timer_ = nullptr; }
    ClearBody();
    items_ = jetson::ui::trash::ListBackgroundTrash();
    const auto &palette = jetson::UiTheme::Instance().Palette();

    if (items_.empty()) lv_obj_add_state(empty_btn_, LV_STATE_DISABLED);
    else lv_obj_remove_state(empty_btn_, LV_STATE_DISABLED);

    lv_obj_set_flex_flow(body_, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(body_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(body_, 14, 0);
    lv_obj_set_style_pad_row(body_, 14, 0);
    lv_obj_set_style_pad_column(body_, 14, 0);
    lv_obj_add_flag(body_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body_, LV_DIR_VER);

    cells_.resize(items_.size(), nullptr);
    image_objs_.resize(items_.size(), nullptr);
    skeletons_.resize(items_.size(), nullptr);
    images_.resize(items_.size());

    for (size_t i = 0; i < items_.size(); ++i) {
        auto *cell = lv_obj_create(body_);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, 174, 144);
        lv_obj_set_style_radius(cell, 14, 0);
        lv_obj_set_style_bg_color(cell, Color(palette.row), 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        lv_obj_set_style_clip_corner(cell, true, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);

        auto *image = lv_image_create(cell);
        lv_obj_set_size(image, 166, 96);
        lv_obj_set_style_bg_color(image, Color(palette.button), 0);
        lv_obj_set_style_bg_opa(image, LV_OPA_COVER, 0);
        lv_obj_align(image, LV_ALIGN_TOP_MID, 0, 4);

        auto *skeleton = lv_label_create(cell);
        lv_obj_set_style_text_font(skeleton, &BUILTIN_ICON_FONT, 0);
        lv_obj_set_style_text_color(skeleton, Color(palette.sub_text), 0);
        lv_label_set_text(skeleton, LV_SYMBOL_IMAGE);
        lv_obj_align(skeleton, LV_ALIGN_TOP_MID, 0, 36);

        auto *name = lv_label_create(cell);
        lv_obj_set_style_text_font(name, &BUILTIN_SMALL_TEXT_FONT, 0);
        lv_obj_set_style_text_color(name, Color(palette.text), 0);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name, 158);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(name, items_[i].original_name.c_str());
        lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -8);

        auto *ctx = new CellCtx{this, i};
        lv_obj_add_event_cb(cell, OnCellClicked, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(cell, OnCellDeleted, LV_EVENT_DELETE, ctx);
        cells_[i] = cell;
        image_objs_[i] = image;
        skeletons_[i] = skeleton;
    }

    if (items_.empty()) {
        auto *empty = lv_label_create(body_);
        lv_obj_set_width(empty, width_ - 32);
        lv_obj_set_style_text_font(empty, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(empty, Color(palette.sub_text), 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(empty, "Thùng rác đang trống");
        lv_obj_set_style_margin_top(empty, 110, 0);
    } else {
        load_index_ = 0;
        load_timer_ = lv_timer_create(OnLoadTimer, 35, this);
    }
}

void TrashView::LoadNextImage() {
    if (load_index_ >= items_.size()) {
        if (load_timer_) { lv_timer_del(load_timer_); load_timer_ = nullptr; }
        return;
    }
    const size_t index = load_index_++;
    const std::string path = items_[index].has_thumbnail
                                 ? items_[index].thumbnail_path
                                 : items_[index].stored_path;
    auto image = LvglImageFromFile(path);
    if (image && image_objs_[index]) {
        lv_image_set_scale(image_objs_[index],
                           static_cast<uint16_t>(PngScaleToFit(path, 166, 96)));
        lv_image_set_src(image_objs_[index], image->image_dsc());
        images_[index] = std::move(image);
        if (skeletons_[index]) {
            lv_obj_del(skeletons_[index]);
            skeletons_[index] = nullptr;
        }
    }
}

void TrashView::OpenPopup(size_t index) {
    if (popup_) ClosePopup();
    if (index >= items_.size()) return;
    popup_index_ = index;
    const auto &item = items_[index];
    const auto &palette = jetson::UiTheme::Instance().Palette();

    popup_ = lv_obj_create(overlay_);
    lv_obj_remove_style_all(popup_);
    lv_obj_set_size(popup_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(popup_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(popup_, LV_OPA_60, 0);
    lv_obj_add_flag(popup_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(popup_, OnPopupDismiss, LV_EVENT_CLICKED, this);

    popup_card_ = lv_obj_create(popup_);
    lv_obj_remove_style_all(popup_card_);
    lv_obj_set_size(popup_card_, 390, 280);
    lv_obj_set_style_radius(popup_card_, 18, 0);
    lv_obj_set_style_bg_color(popup_card_, Color(palette.row), 0);
    lv_obj_set_style_bg_opa(popup_card_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(popup_card_, 16, 0);
    lv_obj_set_style_pad_row(popup_card_, 7, 0);
    lv_obj_set_flex_flow(popup_card_, LV_FLEX_FLOW_COLUMN);
    lv_obj_align(popup_card_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(popup_card_, LV_OBJ_FLAG_CLICKABLE);

    auto add_line = [&](const std::string &text, uint32_t color, bool title = false) {
        auto *label = lv_label_create(popup_card_);
        lv_obj_set_width(label, 358);
        lv_obj_set_style_text_font(label,
                                   title ? &BUILTIN_TEXT_FONT : &BUILTIN_SMALL_TEXT_FONT, 0);
        lv_obj_set_style_text_color(label, Color(color), 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_label_set_text(label, text.c_str());
        return label;
    };
    add_line(item.original_name, palette.text, true);
    add_line("Loại: Ảnh nền", palette.sub_text);
    add_line("Dung lượng: " + FormatSize(item.size), palette.sub_text);
    add_line("Đã xóa: " + FormatDeletedTime(item.deleted_at), palette.sub_text);
    add_line("Vị trí cũ: " + jetson::ui::backgrounds::BackgroundPath(item.original_name),
             palette.sub_text);

    popup_feedback_ = add_line("", 0xff5f57);
    lv_obj_add_flag(popup_feedback_, LV_OBJ_FLAG_HIDDEN);

    auto *actions = lv_obj_create(popup_card_);
    lv_obj_remove_style_all(actions);
    lv_obj_set_size(actions, 358, 46);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(actions, 10, 0);
    lv_obj_clear_flag(actions, LV_OBJ_FLAG_SCROLLABLE);

    auto add_action = [&](const char *text, uint32_t color, lv_event_cb_t callback) {
        auto *button = lv_button_create(actions);
        lv_obj_set_size(button, 150, 42);
        lv_obj_set_style_radius(button, 10, 0);
        lv_obj_set_style_bg_color(button, Color(color), 0);
        lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, this);
        auto *label = lv_label_create(button);
        lv_obj_set_style_text_font(label, &BUILTIN_SMALL_TEXT_FONT, 0);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_label_set_text(label, text);
        lv_obj_center(label);
    };
    add_action("Đóng", palette.button, OnPopupClose);
    add_action("Khôi phục", palette.accent, OnPopupRestore);
}

void TrashView::ClosePopup() {
    if (!popup_) return;
    lv_obj_del(popup_);
    popup_ = nullptr;
    popup_card_ = nullptr;
    popup_feedback_ = nullptr;
}

void TrashView::OnStart() {
    SetStatus(items_.empty() ? "Thùng rác đang trống"
                             : "Chạm một mục để xem thông tin hoặc khôi phục");
}

void TrashView::OnEmptyClicked(lv_event_t *event) {
    LvglLockGuard lock;
    auto *self = static_cast<TrashView *>(lv_event_get_user_data(event));
    const size_t before = self->items_.size();
    if (before == 0) return;
    self->ClosePopup();
    const size_t removed = jetson::ui::trash::EmptyBackgroundTrash();
    if (removed > 0 && self->on_changed_) self->on_changed_();
    self->BuildBody();
    self->SetStatus(removed == before ? "Đã dọn sạch Thùng rác"
                                      : "Không thể xóa một số mục");
}

void TrashView::OnCellClicked(lv_event_t *event) {
    LvglLockGuard lock;
    auto *ctx = static_cast<CellCtx *>(lv_event_get_user_data(event));
    ctx->self->OpenPopup(ctx->index);
}

void TrashView::OnCellDeleted(lv_event_t *event) {
    delete static_cast<CellCtx *>(lv_event_get_user_data(event));
}

void TrashView::OnPopupDismiss(lv_event_t *event) {
    LvglLockGuard lock;
    auto *self = static_cast<TrashView *>(lv_event_get_user_data(event));
    if (lv_event_get_target(event) == self->popup_) self->ClosePopup();
}

void TrashView::OnPopupClose(lv_event_t *event) {
    LvglLockGuard lock;
    static_cast<TrashView *>(lv_event_get_user_data(event))->ClosePopup();
}

void TrashView::OnPopupRestore(lv_event_t *event) {
    LvglLockGuard lock;
    auto *self = static_cast<TrashView *>(lv_event_get_user_data(event));
    if (self->popup_index_ >= self->items_.size()) return;
    const auto item = self->items_[self->popup_index_];
    if (!jetson::ui::trash::RestoreBackground(item)) {
        if (self->popup_feedback_) {
            lv_label_set_text(self->popup_feedback_,
                              "Không thể khôi phục: vị trí cũ đã có file cùng tên");
            lv_obj_clear_flag(self->popup_feedback_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    self->ClosePopup();
    if (self->on_changed_) self->on_changed_();
    self->BuildBody();
    const std::string status = "Đã khôi phục " + item.original_name;
    self->SetStatus(status.c_str());
}

void TrashView::OnLoadTimer(lv_timer_t *timer) {
    LvglLockGuard lock;
    static_cast<TrashView *>(lv_timer_get_user_data(timer))->LoadNextImage();
}

} // namespace home
