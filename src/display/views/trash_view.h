#pragma once

#include "display/common/trash_store.h"
#include "display/core/lvgl_image.h"
#include "display/views/overlay_view.h"

#include <functional>
#include <lvgl.h>
#include <memory>
#include <utility>
#include <vector>

namespace home {

/* Persistent Trash for deleted wallpapers.  Items open an information modal
 * with their original location, size and deletion time, plus a Restore action.
 * Files are only unlinked from disk through the explicit Empty Trash button. */
class TrashView : public OverlayView {
public:
    using OnChanged = std::function<void()>;

    TrashView(lv_obj_t *parent, int width, int height, ClosedCb on_closed);
    ~TrashView() override;

    void SetOnChanged(OnChanged cb) { on_changed_ = std::move(cb); }

protected:
    void OnStart() override;

private:
    struct CellCtx { TrashView *self; size_t index; };

    OnChanged on_changed_;
    std::vector<jetson::ui::trash::Item> items_;
    std::vector<lv_obj_t *> cells_;
    std::vector<lv_obj_t *> image_objs_;
    std::vector<lv_obj_t *> skeletons_;
    std::vector<std::unique_ptr<LvglImage>> images_;

    lv_obj_t *empty_btn_ = nullptr;
    lv_obj_t *popup_ = nullptr;
    lv_obj_t *popup_card_ = nullptr;
    lv_obj_t *popup_feedback_ = nullptr;
    size_t popup_index_ = 0;

    lv_timer_t *load_timer_ = nullptr;
    size_t load_index_ = 0;

    void BuildBody();
    void ClearBody();
    void LoadNextImage();
    void OpenPopup(size_t index);
    void ClosePopup();

    static void OnEmptyClicked(lv_event_t *e);
    static void OnCellClicked(lv_event_t *e);
    static void OnCellDeleted(lv_event_t *e);
    static void OnPopupDismiss(lv_event_t *e);
    static void OnPopupClose(lv_event_t *e);
    static void OnPopupRestore(lv_event_t *e);
    static void OnLoadTimer(lv_timer_t *t);
};

} // namespace home
