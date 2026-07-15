#pragma once

#include "overlay_view.h"

#include <lvgl.h>
#include <string>
#include <vector>

namespace home {

/* macOS-Finder-style Documents browser (the dock "folder" app). A minimal,
 * embedded-tailored take on the React file-system reference: an icon grid of
 * the entries in a real directory (~/Documents by default), folder/file
 * glyphs, back/forward navigation, and a small info popup when a file is
 * tapped (name + size + type). Folders are opened by tapping them.
 *
 * Threading + lifetime mirror OverlayView / BackgroundGalleryView: shared_ptr
 * ownership so the synchronous directory scan and callbacks outlive the
 * on-screen overlay, and close is deferred through the base class. */
class DocumentsView : public OverlayView {
public:
    DocumentsView(lv_obj_t *parent, int width, int height, ClosedCb on_closed);
    ~DocumentsView() override;

protected:
    void OnStart() override;
    void OnResize(int w, int h) override;

private:
    struct Entry {
        std::string name;
        bool is_dir = false;
        long size = 0;       // bytes (0 for directories)
    };
    struct CellCtx { DocumentsView *self; size_t index; };

    std::string root_path_;
    std::string current_path_;
    std::vector<std::string> history_;
    size_t hist_idx_ = 0;

    std::vector<Entry> entries_;
    std::vector<lv_obj_t *> cells_{};
    std::vector<CellCtx *> ctxs_{};

    lv_obj_t *toolbar_ = nullptr;
    lv_obj_t *back_btn_ = nullptr;
    lv_obj_t *fwd_btn_ = nullptr;
    lv_obj_t *path_label_ = nullptr;
    lv_obj_t *grid_ = nullptr;

    lv_obj_t *popup_ = nullptr;        // modal backdrop over the app
    lv_obj_t *popup_card_ = nullptr;
    size_t popup_index_ = 0;

    // Folder navigation is deferred to a one-shot timer so the clicked cell's
    // CLICKED event finishes before BuildGrid() deletes it (LVGL dislikes the
    // event target being freed mid-delivery).
    lv_timer_t *nav_timer_ = nullptr;
    std::string nav_pending_;

    int body_w_ = 0;   // current body content width (for grid reflow on zoom)
    int body_h_ = 0;

    void BuildBody();
    void BuildToolbar();
    void BuildGrid();
    void ClearGrid();
    void Rescan();
    void NavigateTo(const std::string &path, bool push_history);
    void ScheduleNavigate(const std::string &path);
    void GoBack();
    void GoForward();
    void UpdateNavButtons();
    void UpdatePathLabel();
    void OpenPopup(size_t index);
    void ClosePopup();

    static std::string BaseName(const std::string &path);
    static std::string FormatSize(long bytes);
    static std::string ExtUpper(const std::string &name);

    static void OnBack(lv_event_t *e);
    static void OnForward(lv_event_t *e);
    static void OnEntryClicked(lv_event_t *e);
    static void OnEntryDeleted(lv_event_t *e);
    static void OnPopupDismiss(lv_event_t *e);
    static void OnPopupClose(lv_event_t *e);
    static void OnNavTimer(lv_timer_t *t);
};

} // namespace home