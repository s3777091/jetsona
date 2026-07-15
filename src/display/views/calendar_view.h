#pragma once

#include "overlay_view.h"

#include <array>
#include <lvgl.h>
#include <set>
#include <string>

namespace home {

/* Month calendar with day marks (dots). Marks are "YYYY-MM-DD" persisted in
 * Settings("calendar","marks"). Tapping a day toggles its mark. */
class CalendarView : public OverlayView {
public:
    CalendarView(lv_obj_t *parent, int width, int height, ClosedCb on_closed);

protected:
    void OnStart() override;

private:
    struct DayCtx {
        CalendarView *self;
        int day; // 0 = empty cell
    };

    int year_ = 0;   // e.g. 2026
    int month_ = 0;  // 0..11
    lv_obj_t *month_label_ = nullptr;
    lv_obj_t *grid_ = nullptr;
    std::array<lv_obj_t *, 42> cells_{};
    std::array<DayCtx *, 42> day_ctxs_{};
    std::set<std::string> marks_;

    void BuildBody();
    void LoadMarks();
    void SaveMarks();
    void UpdateGrid();
    void ChangeMonth(int delta);
    void ToggleMark(int day);
    static std::string Key(int y, int m, int d);
    bool IsToday(int y, int m, int d) const;
    static int DaysInMonth(int y, int m);
    static const char *MonthName(int m);

    static void OnPrev(lv_event_t *e);
    static void OnNext(lv_event_t *e);
    static void OnDayClicked(lv_event_t *e);
    static void OnDayDeleted(lv_event_t *e);
};

} // namespace home