#pragma once

#include "display/views/overlay_view.h"
#include "display/widgets/telex_ime.h"

#include <array>
#include <lvgl.h>
#include <set>
#include <string>
#include <vector>

namespace home {

/* macOS-inspired month calendar with reminders. Days that have at least one
 * task are marked with a colored dot; clicking any day opens an iOS-inspired
 * bottom sheet for creating a reminder and managing that day's saved tasks.
 *
 * Tasks are persisted in Settings("calendar"):
 *   - "task_dates": comma-separated "YYYY-MM-DD" index of dates that have >=1
 *     task (so the grid can highlight them without enumerating every key).
 *   - "d_YYYY-MM-DD": newline-separated tasks, each "time|done|title"
 *     (time = "" or "HH:MM", done = "0"/"1", title = remainder, may contain '|').
 *
 * The shell (close/min/zoom traffic lights, title, status) comes from OverlayView. */
class CalendarView : public OverlayView {
public:
    CalendarView(lv_obj_t *parent, int width, int height, ClosedCb on_closed);

protected:
    void OnStart() override;
    void OnResize(int w, int h) override;

private:
    enum class EntryKind {
        kEvent,
        kReminder,
    };

    struct Task {
        std::string time;   // "" or "HH:MM"
        bool done = false;
        std::string title;
    };
    struct DayCtx {
        CalendarView *self;
        int day;            // 0 = empty cell
    };
    struct RowCtx {
        CalendarView *self;
        std::string date;   // "YYYY-MM-DD"
        int idx;            // index within that date's task list
    };

    int year_ = 0;    // e.g. 2026
    int month_ = 0;   // 0..11
    lv_obj_t *month_label_ = nullptr;
    lv_obj_t *today_btn_ = nullptr;
    lv_obj_t *top_bar_ = nullptr;
    lv_obj_t *weekday_bar_ = nullptr;
    lv_obj_t *grid_ = nullptr;
    std::array<lv_obj_t *, 42> cells_{};
    std::array<DayCtx *, 42> day_ctxs_{};
    std::set<std::string> task_dates_;  // dates with >=1 task (for highlighting)

    // Day-detail bottom sheet (built on overlay_).
    lv_obj_t *popup_ = nullptr;          // full-screen backdrop
    lv_obj_t *popup_card_ = nullptr;
    lv_obj_t *popup_title_ = nullptr;
    lv_obj_t *popup_event_tab_ = nullptr;
    lv_obj_t *popup_reminder_tab_ = nullptr;
    EntryKind popup_entry_kind_ = EntryKind::kEvent;
    lv_obj_t *popup_list_ = nullptr;    // scrollable column of task rows
    TelexInput *popup_input_ = nullptr;     // event title (Telex when vi)
    TelexInput *popup_location_ = nullptr;  // optional place / video call
    TelexInput *popup_url_ = nullptr;       // explicitly optional URL
    lv_obj_t *popup_all_day_ = nullptr;
    lv_obj_t *popup_start_time_ = nullptr;
    lv_obj_t *popup_start_time_label_ = nullptr;
    lv_obj_t *popup_end_time_ = nullptr;
    lv_obj_t *popup_end_time_label_ = nullptr;
    lv_obj_t *popup_repeat_ = nullptr;
    lv_obj_t *popup_alert_ = nullptr;
    int popup_start_minutes_ = -1;          // local time, minutes since midnight
    int popup_end_minutes_ = -1;
    std::string popup_date_;            // "YYYY-MM-DD"
    std::vector<RowCtx *> popup_rows_;  // RowCtx owned by the current modal

    // Compact hour/minute wheel shown above the day-detail sheet.
    lv_obj_t *time_picker_ = nullptr;
    lv_obj_t *time_hour_roller_ = nullptr;
    lv_obj_t *time_minute_roller_ = nullptr;
    bool time_picker_for_end_ = false;
    int time_picker_min_minutes_ = 0;
    int time_picker_hour_base_ = 0;
    int time_picker_minute_base_ = 0;

    void BuildBody();
    void LayoutCalendar(int body_width, int body_height);
    void UpdateGrid();
    void ChangeMonth(int delta);
    void GoToday();

    // ---- task store ----
    static std::string Key(int y, int m, int d);
    void LoadTaskDates();
    void SaveTaskDates();
    static std::vector<Task> LoadTasks(const std::string &date);
    static void SaveTasks(const std::string &date, const std::vector<Task> &tasks);
    static std::string SerializeTask(const Task &t);
    static Task ParseTask(const std::string &line);
    static bool IsValidTime(const std::string &t);
    static std::string FormatTime(int minutes);

    // ---- day modal ----
    void OpenDayModal(int day);
    void CloseDayModal();
    void SetEntryKind(EntryKind kind);
    void OpenTimePicker(bool for_end);
    void CloseTimePicker();
    void RefreshTimeLabels();
    void RefreshMinuteRoller(int preferred_minute);
    void ApplyTimePicker();
    int MinimumSelectableTime(bool for_end) const;
    bool IsPopupDateToday() const;
    static bool IsPastDate(const std::string &date);
    void RenderTaskList();
    bool AddTask();
    void ToggleTask(const std::string &date, int idx);
    void DeleteTask(const std::string &date, int idx);

    bool IsToday(int y, int m, int d) const;
    static int DaysInMonth(int y, int m);
    static const char *MonthName(int m);
    static std::string FormatDateTitle(int y, int m, int d);

    static void OnPrev(lv_event_t *e);
    static void OnNext(lv_event_t *e);
    static void OnToday(lv_event_t *e);
    static void OnDayClicked(lv_event_t *e);
    static void OnDayDeleted(lv_event_t *e);
    static void OnPopupDismiss(lv_event_t *e);
    static void OnPopupClose(lv_event_t *e);
    static void OnEventTab(lv_event_t *e);
    static void OnReminderTab(lv_event_t *e);
    static void OnAddTask(lv_event_t *e);
    static void OnAllDayChanged(lv_event_t *e);
    static void OnStartTimeClicked(lv_event_t *e);
    static void OnEndTimeClicked(lv_event_t *e);
    static void OnTimePickerDismiss(lv_event_t *e);
    static void OnTimePickerCancel(lv_event_t *e);
    static void OnTimePickerConfirm(lv_event_t *e);
    static void OnTimeHourChanged(lv_event_t *e);
    static void OnRowToggle(lv_event_t *e);
    static void OnRowDelete(lv_event_t *e);
};

} // namespace home
