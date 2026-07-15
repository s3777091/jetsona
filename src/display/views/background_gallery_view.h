#pragma once

#include "display/core/lvgl_image.h"
#include "display/views/overlay_view.h"

#include <functional>
#include <lvgl.h>
#include <memory>
#include <string>
#include <vector>

namespace home {

/* Bento-style wallpaper gallery (the "Ảnh" app). A horizontal, drag-to-pan
 * row of thumbnails with skeleton placeholders that fill in progressively so
 * opening never stalls. Tapping a thumbnail opens a small popup with three
 * actions:
 *   - "Đặt hình nền desktop"  -> set as the home wallpaper
 *   - "Đặt hình nền sleep screen" -> set as the sleep/dim wallpaper
 *   - "Xóa ảnh"               -> delete the file (full + thumb) from disk
 * The wallpaper set is read from disk at runtime (backgrounds::ListBackgroundFiles),
 * so deletions take effect immediately and the app stays capped by what's on
 * disk. The view is shared_ptr-owned (see OverlayView) so the deferred load
 * timer and worker callbacks outlive the on-screen overlay. */
class BackgroundGalleryView : public OverlayView {
public:
    using OnSelectBg = std::function<void(const std::string &file)>;
    using OnChanged = std::function<void()>;

    BackgroundGalleryView(lv_obj_t *parent, int width, int height, ClosedCb on_closed);
    ~BackgroundGalleryView() override;

    void SetOnSelect(OnSelectBg cb) { on_select_ = std::move(cb); }
    void SetOnSleep(OnSelectBg cb) { on_sleep_ = std::move(cb); }
    void SetOnChanged(OnChanged cb) { on_changed_ = std::move(cb); }

protected:
    void OnStart() override;

private:
    struct CellCtx { BackgroundGalleryView *self; size_t index; };

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
    std::vector<CellCtx *> ctxs_{};

    lv_obj_t *popup_ = nullptr;        // modal overlay over the app
    lv_obj_t *popup_card_ = nullptr;
    size_t popup_index_ = 0;

    lv_timer_t *load_timer_ = nullptr;
    size_t load_idx_ = 0;

    void BuildBody();
    void ClearGrid();
    void RebuildGrid();
    void LoadNextImage();
    void RefreshHighlights();
    void OpenPopup(size_t index);
    void ClosePopup();
    void DeleteImage(size_t index);

    static void OnCellClicked(lv_event_t *e);
    static void OnCellDeleted(lv_event_t *e);
    static void OnPopupDismiss(lv_event_t *e);
    static void OnPopupDesktop(lv_event_t *e);
    static void OnPopupSleep(lv_event_t *e);
    static void OnPopupDelete(lv_event_t *e);
    static void OnLoadTimer(lv_timer_t *t);
};

} // namespace home
