#pragma once

#include "display/views/overlay_view.h"
#include "display/widgets/telex_ime.h"

#include <cstdint>
#include <lvgl.h>
#include <string>
#include <vector>

namespace home {

/* A compact, persistent reminders list for the Reminders dock icon.
 *
 * Each task has a bright color marker instead of an application icon. Tasks
 * can be completed, pinned between the two sections, inspected in a bottom
 * sheet, and reordered by dragging the handle at the right edge. */
class RemindersView : public OverlayView {
public:
    RemindersView(lv_obj_t *parent, int width, int height, ClosedCb on_closed);
    ~RemindersView() override;

    /* Re-read the store and repaint. The list is held in memory and saved
     * wholesale, so a row written behind this view's back (the agent's
     * reminder_add tool) would be erased by the next save without this.
     * Call on the LVGL thread. */
    void ReloadFromStore();

protected:
    void OnStart() override;

private:
    struct Task {
        int id = 0;
        bool pinned = false;
        bool done = false;
        uint32_t color = 0;
        std::string title;
        std::string info;
    };

    struct RowCtx {
        RemindersView *self = nullptr;
        int id = 0;
        lv_obj_t *row = nullptr;
        int drag_y = 0;
        bool drag_changed = false;
    };

    TelexInput *input_ = nullptr;
    lv_obj_t *list_ = nullptr;
    lv_obj_t *info_overlay_ = nullptr;
    lv_obj_t *info_card_ = nullptr;
    int info_task_id_ = 0;
    int next_id_ = 1;
    std::vector<Task> tasks_;
    std::vector<RowCtx *> row_ctxs_;

    void BuildBody();
    void Load();
    void Save() const;
    void AddTask();
    void Render();
    void RenderSection(const char *title, bool pinned);
    void ToggleDone(int id);
    void TogglePinned(int id);
    void MoveTask(RowCtx *ctx, int direction);
    void OpenInfo(int id);
    void CloseInfo();
    void DeleteTask(int id);
    Task *FindTask(int id);
    const Task *FindTask(int id) const;
    uint32_t RandomBrightColor() const;

    static void OnAdd(lv_event_t *e);
    static void OnDone(lv_event_t *e);
    static void OnPin(lv_event_t *e);
    static void OnInfo(lv_event_t *e);
    static void OnDrag(lv_event_t *e);
    static void OnInfoDismiss(lv_event_t *e);
    static void OnInfoClose(lv_event_t *e);
    static void OnDelete(lv_event_t *e);
};

} // namespace home
