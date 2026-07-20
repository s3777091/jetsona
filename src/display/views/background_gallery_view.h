#pragma once

#include "display/core/lvgl_image.h"
#include "display/views/overlay_view.h"

#include <functional>
#include <lvgl.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace home {

/* Wallpaper carousel (the "Ảnh" app). A horizontal, center-snapping row of
 * thumbnails fills progressively so opening never stalls. The card nearest
 * the viewport center is the active selection and scales up gently while the
 * user scrolls. A fixed action rail keeps Share on the left and Trash on the
 * right. Share opens a small inward-facing popover with desktop/sleep choices;
 * Trash moves the centered file (full + thumb) to Trash.
 *
 * The wallpaper set is read from disk at runtime
 * (backgrounds::ListBackgroundFiles), so moved items disappear immediately
 * and can later be recovered from Trash. The view is shared_ptr-owned (see
 * OverlayView) so the deferred loader remains safe while the app is open. */
class BackgroundGalleryView : public OverlayView {
public:
    using OnSelectBg = std::function<void(const std::string &file)>;
    using OnChanged = std::function<void()>;

    BackgroundGalleryView(lv_obj_t *parent, int width, int height,
                          ClosedCb on_closed);
    ~BackgroundGalleryView() override;

    void SetOnSelect(OnSelectBg cb) { on_select_ = std::move(cb); }
    void SetOnSleep(OnSelectBg cb) { on_sleep_ = std::move(cb); }
    void SetOnChanged(OnChanged cb) { on_changed_ = std::move(cb); }

protected:
    void OnStart() override;

private:
    struct CellCtx {
        BackgroundGalleryView *self;
        size_t index;
    };

    // Home-wired callbacks (set before Start()).
    OnSelectBg on_select_;
    OnSelectBg on_sleep_;
    OnChanged on_changed_;

    std::vector<std::string> files_;
    std::string current_file_;   // selected desktop wallpaper
    std::string sleep_file_;     // selected sleep-screen wallpaper

    std::vector<lv_obj_t *> cells_{};
    std::vector<lv_obj_t *> img_objs_{};
    std::vector<lv_obj_t *> skeletons_{};
    std::vector<std::unique_ptr<LvglImage>> images_{};

    lv_obj_t *carousel_ = nullptr;
    lv_obj_t *action_row_ = nullptr;
    lv_obj_t *share_btn_ = nullptr;
    lv_obj_t *trash_btn_ = nullptr;
    lv_obj_t *share_menu_ = nullptr;
    size_t selected_index_ = 0;
    bool selection_initialized_ = false;
    bool share_menu_visible_ = false;
    bool rebuilding_ = false;

    lv_timer_t *load_timer_ = nullptr;
    size_t load_idx_ = 0;

    void BuildBody();
    void ClearGrid();
    void RebuildGrid();
    void LoadNextImage();
    void RefreshHighlights();
    void UpdateSelectedFromScroll(bool animated);
    void SetSelectedIndex(size_t index, bool animated);
    void AnimateCellScale(lv_obj_t *cell, int target, bool animated);
    void CenterSelected(lv_anim_enable_t animated);
    void ShowShareMenu();
    void HideShareMenu(bool animated = true);
    void SetActionsEnabled(bool enabled);
    void MoveImageToTrash(size_t index);

    static void OnCellClicked(lv_event_t *e);
    static void OnCellDeleted(lv_event_t *e);
    static void OnCarouselScroll(lv_event_t *e);
    static void OnShareClicked(lv_event_t *e);
    static void OnTrashClicked(lv_event_t *e);
    static void OnShareDesktop(lv_event_t *e);
    static void OnShareSleep(lv_event_t *e);
    static void OnLoadTimer(lv_timer_t *t);
    static void OnCellScale(void *var, int32_t value);
    static void OnShareMenuOpa(void *var, int32_t value);
    static void OnShareMenuY(void *var, int32_t value);
    static void OnShareMenuHidden(lv_anim_t *a);
};

} // namespace home
