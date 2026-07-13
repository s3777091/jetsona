#pragma once

#include "overlay_view.h"
#include "lvgl_image.h"

#include <array>
#include <functional>
#include <lvgl.h>
#include <memory>

namespace home {

/* Grid of the 10 DS-02 wallpapers as thumbnails. Tapping one saves it as the
 * selected background (Settings "display"/"ds02_background") and fires
 * on_select(index) so the home screen can apply it immediately. */
class BackgroundGalleryView : public OverlayView {
public:
    using OnSelect = std::function<void(size_t)>;

    BackgroundGalleryView(lv_obj_t *parent, int width, int height, ClosedCb on_closed);
    ~BackgroundGalleryView();

    void SetOnSelect(OnSelect cb) { on_select_ = std::move(cb); }
    void SetCurrent(size_t index) { current_ = index; }

protected:
    void OnStart() override;

private:
    struct CellCtx { BackgroundGalleryView *self; size_t index; };

    static constexpr size_t kCount = 10;
    static constexpr int kCols = 2;

    OnSelect on_select_;
    size_t current_ = 0;
    std::array<lv_obj_t *, kCount> cells_{};
    std::array<lv_obj_t *, kCount> img_objs_{};
    std::array<std::unique_ptr<LvglImage>, kCount> images_{};
    std::array<CellCtx *, kCount> ctxs_{};

    void BuildBody();
    void RefreshHighlights();
    static std::string AssetPath(size_t index);
    static void OnCellClicked(lv_event_t *e);
    static void OnCellDeleted(lv_event_t *e);
};

} // namespace home