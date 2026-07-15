#include "display/views/calendar_view.h"
#include "display/common/lvgl_utils.h"
#include "fonts.h"
#include "settings.h"
#include "display/theme/ui_theme.h"
#include "esp_log.h"

#include <lvgl.h>
#include <algorithm>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace home {

#define TAG "CalendarView"

namespace {
using jetson::ui::Color;
using jetson::ui::LvglLockGuard;

const char *kWeekdays[7] = {"T2", "T3", "T4", "T5", "T6", "T7", "CN"};

// Vietnamese weekday names for the day-modal title (tm_wday: 0=Sun).
const char *kVnWeekday[7] = {
    "Chủ nhật", "Thứ hai", "Thứ ba", "Thứ tư",
    "Thứ năm", "Thứ sáu", "Thứ bảy",
};

bool IsKbdVi() {
    Settings s("input", false);
    return s.GetString("kbd_lang", "en") == "vi";
}

void SetSheetTranslateY(void *obj, int32_t value) {
    lv_obj_set_style_translate_y(static_cast<lv_obj_t *>(obj), value, 0);
}
} // namespace

CalendarView::CalendarView(lv_obj_t *parent, int width, int height, ClosedCb on_closed)
    : OverlayView(parent, width, height, "Lịch", std::move(on_closed)) {
    std::time_t now = std::time(nullptr);
    std::tm *t = std::localtime(&now);
    year_ = t->tm_year + 1900;
    month_ = t->tm_mon;
    LoadTaskDates();
    BuildBody();
}

int CalendarView::DaysInMonth(int y, int m) {
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int n = d[m];
    if (m == 1 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) n = 29;
    return n;
}

const char *CalendarView::MonthName(int m) {
    static const char *names[] = {
        "Tháng 1", "Tháng 2", "Tháng 3", "Tháng 4", "Tháng 5", "Tháng 6",
        "Tháng 7", "Tháng 8", "Tháng 9", "Tháng 10", "Tháng 11", "Tháng 12"};
    return names[m % 12];
}

std::string CalendarView::Key(int y, int m, int d) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m + 1, d);
    return buf;
}

bool CalendarView::IsToday(int y, int m, int d) const {
    std::time_t now = std::time(nullptr);
    std::tm *t = std::localtime(&now);
    return t->tm_year + 1900 == y && t->tm_mon == m && t->tm_mday == d;
}

std::string CalendarView::FormatDateTitle(int y, int m, int d) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = m;
    tm.tm_mday = d;
    tm.tm_hour = 12;
    std::mktime(&tm);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%s, %d/%d/%d",
                  kVnWeekday[tm.tm_wday], d, m + 1, y);
    return buf;
}

// ---- task store ----

void CalendarView::LoadTaskDates() {
    task_dates_.clear();
    Settings s("calendar", false);
    std::string v = s.GetString("task_dates", "");
    std::istringstream iss(v);
    std::string token;
    while (std::getline(iss, token, ',')) {
        if (!token.empty()) task_dates_.insert(token);
    }
}

void CalendarView::SaveTaskDates() {
    Settings s("calendar", true);
    std::string out;
    for (const auto &k : task_dates_) {
        if (!out.empty()) out += ",";
        out += k;
    }
    s.SetString("task_dates", out);
}

std::string CalendarView::SerializeTask(const Task &t) {
    std::string out;
    out += t.time;
    out += '|';
    out += t.done ? "1" : "0";
    out += '|';
    out += t.title;  // title is the remainder; may contain '|', never newline
    return out;
}

CalendarView::Task CalendarView::ParseTask(const std::string &line) {
    Task t;
    size_t p1 = line.find('|');
    if (p1 == std::string::npos) { t.title = line; return t; }
    t.time = line.substr(0, p1);
    size_t p2 = line.find('|', p1 + 1);
    if (p2 == std::string::npos) {
        t.done = line.substr(p1 + 1) == "1";
        return t;
    }
    t.done = line.substr(p1 + 1, p2 - p1 - 1) == "1";
    t.title = line.substr(p2 + 1);
    return t;
}

std::vector<CalendarView::Task> CalendarView::LoadTasks(const std::string &date) {
    std::vector<Task> out;
    Settings s("calendar", false);
    std::string v = s.GetString(("d_" + date).c_str(), "");
    if (v.empty()) return out;
    std::istringstream iss(v);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty()) out.push_back(ParseTask(line));
    }
    return out;
}

void CalendarView::SaveTasks(const std::string &date, const std::vector<Task> &tasks) {
    Settings s("calendar", true);
    if (tasks.empty()) {
        s.EraseKey(("d_" + date).c_str());
        // task_dates_ is updated by the caller (Toggle/Delete/Add).
    } else {
        std::string out;
        for (const auto &t : tasks) {
            if (!out.empty()) out += "\n";
            out += SerializeTask(t);
        }
        s.SetString(("d_" + date).c_str(), out);
    }
}

bool CalendarView::IsValidTime(const std::string &t) {
    if (t.empty()) return true;
    if (t.size() != 5 || t[2] != ':') return false;
    for (int i = 0; i < 5; ++i) {
        if (i == 2) continue;
        if (t[i] < '0' || t[i] > '9') return false;
    }
    int hh = (t[0] - '0') * 10 + (t[1] - '0');
    int mm = (t[3] - '0') * 10 + (t[4] - '0');
    return hh < 24 && mm < 60;
}

// ---- body / grid ----

void CalendarView::BuildBody() {
    const auto &p = jetson::UiTheme::Instance().Palette();

    lv_obj_set_flex_flow(body_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(body_, 4, 0);
    lv_obj_clear_flag(body_, LV_OBJ_FLAG_SCROLLABLE);

    // Top row: prev | month label (grow) | "Hôm nay" | next
    top_bar_ = lv_obj_create(body_);
    lv_obj_remove_style_all(top_bar_);
    lv_obj_set_size(top_bar_, width_ - 16, 44);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top_bar_, 8, 0);
    lv_obj_clear_flag(top_bar_, LV_OBJ_FLAG_SCROLLABLE);

    auto makeNav = [&](const char *sym, lv_event_cb_t cb) {
        auto *b = lv_button_create(top_bar_);
        lv_obj_set_size(b, 44, 36);
        lv_obj_set_style_bg_color(b, Color(p.button), 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, this);
        auto *l = lv_label_create(b);
        lv_label_set_text(l, sym);
        lv_obj_set_style_text_font(l, &BUILTIN_ICON_FONT, 0);
        lv_obj_set_style_text_color(l, Color(p.text), 0);
        lv_obj_center(l);
        return b;
    };
    makeNav(LV_SYMBOL_LEFT, OnPrev);

    month_label_ = lv_label_create(top_bar_);
    lv_obj_set_flex_grow(month_label_, 1);
    lv_obj_set_style_text_font(month_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(month_label_, Color(p.text), 0);

    lv_obj_set_style_text_align(month_label_, LV_TEXT_ALIGN_CENTER, 0);

    today_btn_ = lv_button_create(top_bar_);
    lv_obj_set_height(today_btn_, 36);
    lv_obj_set_style_bg_color(today_btn_, Color(p.row_active), 0);
    lv_obj_set_style_radius(today_btn_, 8, 0);
    lv_obj_set_style_pad_hor(today_btn_, 12, 0);
    lv_obj_add_event_cb(today_btn_, OnToday, LV_EVENT_CLICKED, this);
    {
        auto *tl = lv_label_create(today_btn_);
        lv_obj_set_style_text_font(tl, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(tl, Color(p.accent), 0);
        lv_label_set_text(tl, "Hôm nay");
        lv_obj_center(tl);
    }

    makeNav(LV_SYMBOL_RIGHT, OnNext);

    // Weekday header (7 columns).
    weekday_bar_ = lv_obj_create(body_);
    lv_obj_remove_style_all(weekday_bar_);
    lv_obj_set_size(weekday_bar_, width_ - 16, 28);
    lv_obj_set_flex_flow(weekday_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(weekday_bar_, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 7; ++i) {
        auto *l = lv_label_create(weekday_bar_);
        lv_obj_set_flex_grow(l, 1);
        lv_obj_set_style_text_font(l, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(l, Color(p.sub_text), 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(l, kWeekdays[i]);
    }

    // Day grid: 7 cols x 6 rows via flex-wrap.
    grid_ = lv_obj_create(body_);
    lv_obj_remove_style_all(grid_);
    lv_obj_set_flex_flow(grid_, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(grid_, 0, 0);
    lv_obj_set_style_pad_row(grid_, 4, 0);
    lv_obj_set_style_pad_column(grid_, 4, 0);
    lv_obj_clear_flag(grid_, LV_OBJ_FLAG_SCROLLABLE);

    const int calendar_w = width_ - 16;
    const int grid_h = (height_ - 48 - 16) - 44 - 28 - 8;
    const int cell_w = (calendar_w - 6 * 4) / 7;
    const int cell_h = (grid_h - 5 * 4) / 6;
    lv_obj_set_size(grid_, calendar_w, grid_h);

    for (int i = 0; i < 42; ++i) {
        auto *cell = lv_obj_create(grid_);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, cell_w, cell_h);
        lv_obj_set_style_pad_all(cell, 0, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);

        // A fixed 42px badge keeps each day aligned in a true 7 x 6 grid.
        // Previously all cells only had flex_grow and zero base width, so LVGL
        // put all 42 day numbers on one long row.
        auto *badge = lv_obj_create(cell);
        lv_obj_remove_style_all(badge);
        lv_obj_set_size(badge, 42, 42);
        lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_TRANSP, 0);
        lv_obj_align(badge, LV_ALIGN_CENTER, 0, -2);
        lv_obj_clear_flag(badge, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

        auto *num = lv_label_create(badge);
        lv_obj_set_style_text_font(num, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(num, Color(p.text), 0);
        lv_obj_center(num);
        auto *dot = lv_obj_create(cell);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 6, 6);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, Color(p.accent), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_TRANSP, 0);
        lv_obj_align(dot, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_clear_flag(dot, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
        auto *ctx = new DayCtx{this, 0};
        day_ctxs_[i] = ctx;
        lv_obj_add_event_cb(cell, OnDayClicked, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(cell, OnDayDeleted, LV_EVENT_DELETE, ctx);
        cells_[i] = cell;
    }

    UpdateGrid();
    ESP_LOGI(TAG, "calendar grid ready: %04d-%02d, cell=%dx%d",
             year_, month_ + 1, cell_w, cell_h);
}

void CalendarView::LayoutCalendar(int body_width, int body_height) {
    const int calendar_w = std::max(320, body_width - 16);
    const int grid_h = std::max(240, body_height - 16 - 44 - 28 - 8);
    const int cell_w = (calendar_w - 6 * 4) / 7;
    const int cell_h = (grid_h - 5 * 4) / 6;
    if (top_bar_) lv_obj_set_width(top_bar_, calendar_w);
    if (weekday_bar_) lv_obj_set_width(weekday_bar_, calendar_w);
    if (grid_) lv_obj_set_size(grid_, calendar_w, grid_h);
    for (auto *cell : cells_) {
        if (cell) lv_obj_set_size(cell, cell_w, cell_h);
    }
}

void CalendarView::UpdateGrid() {
    const auto &p = jetson::UiTheme::Instance().Palette();
    if (month_label_) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%s %d", MonthName(month_), year_);
        lv_label_set_text(month_label_, buf);
    }
    int firstWday = 0; // 0=Sun
    {
        std::tm tm{};
        tm.tm_year = year_ - 1900;
        tm.tm_mon = month_;
        tm.tm_mday = 1;
        tm.tm_hour = 12;
        std::mktime(&tm);
        firstWday = tm.tm_wday;
    }
    int mondayOffset = (firstWday + 6) % 7; // Monday-first index of the 1st
    int dim = DaysInMonth(year_, month_);

    for (int i = 0; i < 42; ++i) {
        int day = i - mondayOffset + 1;
        lv_obj_t *cell = cells_[i];
        lv_obj_t *badge = lv_obj_get_child(cell, 0);
        lv_obj_t *num = badge ? lv_obj_get_child(badge, 0) : nullptr;
        lv_obj_t *dot = lv_obj_get_child(cell, 1);
        day_ctxs_[i]->day = 0;
        if (day >= 1 && day <= dim) {
            day_ctxs_[i]->day = day;
            char b[8];
            std::snprintf(b, sizeof(b), "%d", day);
            lv_label_set_text(num, b);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_HIDDEN);
            bool today = IsToday(year_, month_, day);
            bool has_tasks = task_dates_.count(Key(year_, month_, day)) > 0;
            lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
            // macOS today: a small filled accent circle, not the entire cell.
            lv_obj_set_style_bg_color(badge, Color(p.accent), 0);
            lv_obj_set_style_bg_opa(badge, today ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            lv_obj_set_style_text_color(num, today ? lv_color_white() : Color(p.text), 0);
            // Task dot below the number (accent on normal days, white on today).
            lv_obj_set_style_bg_color(dot, today ? lv_color_white() : Color(p.accent), 0);
            lv_obj_set_style_bg_opa(dot, has_tasks ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        } else {
            lv_label_set_text(num, "");
            lv_obj_set_style_bg_opa(badge, LV_OPA_TRANSP, 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_TRANSP, 0);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        }
    }
}

void CalendarView::ChangeMonth(int delta) {
    month_ += delta;
    while (month_ < 0) { month_ += 12; year_ -= 1; }
    while (month_ > 11) { month_ -= 12; year_ += 1; }
    UpdateGrid();
    ESP_LOGI(TAG, "showing %04d-%02d", year_, month_ + 1);
}

void CalendarView::GoToday() {
    std::time_t now = std::time(nullptr);
    std::tm *t = std::localtime(&now);
    year_ = t->tm_year + 1900;
    month_ = t->tm_mon;
    UpdateGrid();
}

// ---- day modal ----

void CalendarView::OpenDayModal(int day) {
    if (day <= 0) return;
    if (popup_) CloseDayModal();
    const auto &p = jetson::UiTheme::Instance().Palette();
    popup_date_ = Key(year_, month_, day);

    const int overlay_w = lv_obj_get_width(overlay_);
    const int overlay_h = lv_obj_get_height(overlay_);
    const int sheet_w = std::min(744, overlay_w - 24);
    const int sheet_h = std::min(430, overlay_h - 16);
    char date_text[24];
    std::snprintf(date_text, sizeof(date_text), "%02d/%02d/%04d", day, month_ + 1, year_);

    popup_ = lv_obj_create(overlay_);
    lv_obj_remove_style_all(popup_);
    lv_obj_set_size(popup_, overlay_w, overlay_h);
    lv_obj_set_pos(popup_, 0, 0);
    lv_obj_set_style_bg_color(popup_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(popup_, LV_OPA_50, 0);
    lv_obj_add_flag(popup_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(popup_, OnPopupDismiss, LV_EVENT_CLICKED, this);

    popup_card_ = lv_obj_create(popup_);
    lv_obj_remove_style_all(popup_card_);
    lv_obj_set_size(popup_card_, sheet_w, sheet_h);
    lv_obj_set_style_bg_color(popup_card_, Color(p.panel), 0);
    lv_obj_set_style_bg_opa(popup_card_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(popup_card_, 24, 0);
    lv_obj_set_style_shadow_color(popup_card_, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(popup_card_, LV_OPA_30, 0);
    lv_obj_set_style_shadow_width(popup_card_, 24, 0);
    lv_obj_set_style_pad_all(popup_card_, 14, 0);
    lv_obj_set_style_pad_row(popup_card_, 8, 0);
    lv_obj_set_flex_flow(popup_card_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(popup_card_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_align(popup_card_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(popup_card_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(popup_card_, LV_OBJ_FLAG_SCROLLABLE);

    // Small grabber makes the bottom-sheet motion and affordance obvious.
    auto *grabber = lv_obj_create(popup_card_);
    lv_obj_remove_style_all(grabber);
    lv_obj_set_size(grabber, 48, 5);
    lv_obj_set_style_radius(grabber, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(grabber, Color(p.sub_text), 0);
    lv_obj_set_style_bg_opa(grabber, LV_OPA_60, 0);
    lv_obj_clear_flag(grabber, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    // Header: cancel | "Mới" | save.
    auto *sheet_header = lv_obj_create(popup_card_);
    lv_obj_remove_style_all(sheet_header);
    lv_obj_set_size(sheet_header, lv_pct(100), 40);
    lv_obj_set_flex_flow(sheet_header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sheet_header, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(sheet_header, LV_OBJ_FLAG_SCROLLABLE);

    auto make_header_button = [&](const char *symbol, lv_event_cb_t cb, uint32_t bg) {
        auto *button = lv_button_create(sheet_header);
        lv_obj_set_size(button, 40, 40);
        lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(button, Color(bg), 0);
        lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, this);
        auto *label = lv_label_create(button);
        lv_obj_set_style_text_font(label, &BUILTIN_ICON_FONT, 0);
        lv_obj_set_style_text_color(label, cb == OnAddTask ? lv_color_white() : Color(p.text), 0);
        lv_label_set_text(label, symbol);
        lv_obj_center(label);
        return button;
    };
    make_header_button(LV_SYMBOL_CLOSE, OnPopupClose, p.button);

    popup_title_ = lv_label_create(sheet_header);
    lv_obj_set_style_text_font(popup_title_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(popup_title_, Color(p.text), 0);
    lv_label_set_text(popup_title_, "Mới");
    make_header_button(LV_SYMBOL_OK, OnAddTask, p.accent);

    // Segmented control from the reference. The calendar stores both choices
    // as reminders; the labels communicate the familiar event-creation flow.
    auto *segments = lv_obj_create(popup_card_);
    lv_obj_remove_style_all(segments);
    lv_obj_set_size(segments, 300, 34);
    lv_obj_set_style_bg_color(segments, Color(p.button), 0);
    lv_obj_set_style_bg_opa(segments, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(segments, 10, 0);
    lv_obj_set_style_pad_all(segments, 3, 0);
    lv_obj_set_style_pad_column(segments, 3, 0);
    lv_obj_set_flex_flow(segments, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(segments, LV_OBJ_FLAG_SCROLLABLE);
    auto make_segment = [&](const char *text, bool selected) {
        auto *seg = lv_obj_create(segments);
        lv_obj_remove_style_all(seg);
        lv_obj_set_height(seg, lv_pct(100));
        lv_obj_set_flex_grow(seg, 1);
        lv_obj_set_style_radius(seg, 8, 0);
        lv_obj_set_style_bg_color(seg, Color(selected ? p.row : p.button), 0);
        lv_obj_set_style_bg_opa(seg, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        auto *label = lv_label_create(seg);
        lv_obj_set_style_text_font(label, &BUILTIN_SMALL_TEXT_FONT, 0);
        lv_obj_set_style_text_color(label, Color(p.text), 0);
        lv_label_set_text(label, text);
        lv_obj_center(label);
    };
    make_segment("Sự kiện", true);
    make_segment("Lời nhắc", false);

    // The phone reference is vertical. On this landscape panel the same fields
    // are arranged in two compact columns so no important control is clipped.
    auto *content = lv_obj_create(popup_card_);
    lv_obj_remove_style_all(content);
    lv_obj_set_width(content, lv_pct(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(content, 12, 0);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    auto make_column = [&]() {
        auto *column = lv_obj_create(content);
        lv_obj_remove_style_all(column);
        lv_obj_set_width(column, 1);
        lv_obj_set_height(column, lv_pct(100));
        lv_obj_set_flex_grow(column, 1);
        lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(column, 8, 0);
        lv_obj_clear_flag(column, LV_OBJ_FLAG_SCROLLABLE);
        return column;
    };
    auto *left = make_column();
    auto *right = make_column();

    auto make_group = [&](lv_obj_t *parent, int h) {
        auto *group = lv_obj_create(parent);
        lv_obj_remove_style_all(group);
        lv_obj_set_size(group, lv_pct(100), h);
        lv_obj_set_style_bg_color(group, Color(p.row), 0);
        lv_obj_set_style_bg_opa(group, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(group, 14, 0);
        lv_obj_set_style_pad_all(group, 0, 0);
        lv_obj_set_flex_flow(group, LV_FLEX_FLOW_COLUMN);
        lv_obj_clear_flag(group, LV_OBJ_FLAG_SCROLLABLE);
        return group;
    };
    auto make_divider = [&](lv_obj_t *parent) {
        auto *line = lv_obj_create(parent);
        lv_obj_remove_style_all(line);
        lv_obj_set_size(line, lv_pct(100), 1);
        lv_obj_set_style_bg_color(line, Color(p.border), 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
        return line;
    };
    auto style_input = [&](TelexInput *input) {
        input->SetFont(&BUILTIN_SMALL_TEXT_FONT);
        lv_obj_set_style_bg_opa(input->obj(), LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(input->obj(), 0, 0);
        lv_obj_set_style_radius(input->obj(), 0, 0);
    };
    auto make_text_label = [&](lv_obj_t *parent, const char *text, uint32_t color) {
        auto *label = lv_label_create(parent);
        lv_obj_set_style_text_font(label, &BUILTIN_SMALL_TEXT_FONT, 0);
        lv_obj_set_style_text_color(label, Color(color), 0);
        lv_label_set_text(label, text);
        return label;
    };

    // Title and optional place/video call.
    auto *identity = make_group(left, 86);
    popup_input_ = new TelexInput(identity, lv_pct(100), 42);
    popup_input_->SetTelex(IsKbdVi());
    popup_input_->SetMaxLen(64);
    popup_input_->SetPlaceholder("Tiêu đề");
    style_input(popup_input_);
    make_divider(identity);
    popup_location_ = new TelexInput(identity, lv_pct(100), 42);
    popup_location_->SetTelex(IsKbdVi());
    popup_location_->SetMaxLen(96);
    popup_location_->SetPlaceholder("Vị trí hoặc cuộc gọi video");
    style_input(popup_location_);

    auto make_info_row = [&](lv_obj_t *parent, const char *title, int h) {
        auto *row = lv_obj_create(parent);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), h);
        lv_obj_set_style_pad_hor(row, 10, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        auto *label = make_text_label(row, title, p.text);
        lv_obj_set_flex_grow(label, 1);
        return row;
    };

    // All day, start, end and time zone. Calendar selection supplies the date.
    auto *timing = make_group(left, 188);
    auto *all_day_row = make_info_row(timing, "Cả ngày", 42);
    popup_all_day_ = lv_switch_create(all_day_row);
    lv_obj_set_size(popup_all_day_, 48, 26);
    lv_obj_set_style_bg_color(popup_all_day_, Color(p.button), LV_PART_MAIN);
    lv_obj_set_style_bg_color(popup_all_day_, Color(p.accent),
                              (lv_style_selector_t)(LV_PART_MAIN | LV_STATE_CHECKED));
    lv_obj_add_event_cb(popup_all_day_, OnAllDayChanged, LV_EVENT_VALUE_CHANGED, this);
    make_divider(timing);

    auto make_time_row = [&](const char *title, TelexInput **out) {
        auto *row = make_info_row(timing, title, 48);
        auto *date = make_text_label(row, date_text, p.sub_text);
        lv_obj_set_width(date, 98);
        *out = new TelexInput(row, 74, 34);
        (*out)->SetTelex(false);
        (*out)->SetMaxLen(5);
        (*out)->SetPlaceholder("HH:MM");
        style_input(*out);
        lv_obj_set_style_bg_color((*out)->obj(), Color(p.button), 0);
        lv_obj_set_style_bg_opa((*out)->obj(), LV_OPA_COVER, 0);
        lv_obj_set_style_radius((*out)->obj(), 9, 0);
    };
    make_time_row("Bắt đầu", &popup_time_);
    make_divider(timing);
    make_time_row("Kết thúc", &popup_end_time_);
    make_divider(timing);
    auto *timezone_row = make_info_row(timing, "Múi giờ", 46);
    make_text_label(timezone_row, "Theo hệ thống", p.sub_text);

    auto make_dropdown_row = [&](lv_obj_t *parent, const char *title,
                                 const char *options, lv_obj_t **out) {
        auto *row = make_info_row(parent, title, 44);
        auto *dropdown = lv_dropdown_create(row);
        lv_obj_set_size(dropdown, 150, 34);
        lv_dropdown_set_options(dropdown, options);
        lv_obj_set_style_text_font(dropdown, &BUILTIN_SMALL_TEXT_FONT, 0);
        lv_obj_set_style_text_color(dropdown, Color(p.sub_text), 0);
        lv_obj_set_style_bg_color(dropdown, Color(p.button), 0);
        lv_obj_set_style_bg_opa(dropdown, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dropdown, 0, 0);
        lv_obj_set_style_radius(dropdown, 9, 0);
        *out = dropdown;
    };

    auto *options_group = make_group(right, 89);
    make_dropdown_row(options_group, "Lặp lại",
                      "Không\nMỗi ngày\nMỗi tuần\nMỗi 2 tuần\nMỗi tháng\nMỗi năm\nTùy chỉnh",
                      &popup_repeat_);
    make_divider(options_group);
    make_dropdown_row(options_group, "Cảnh báo",
                      "Không\nKhi bắt đầu\nTrước 5 phút\nTrước 15 phút\nTrước 1 giờ",
                      &popup_alert_);

    // URL remains available, but unlike the removed "Hiển thị dưới dạng" row
    // its optional nature is explicit in the field itself.
    auto *url_group = make_group(right, 42);
    popup_url_ = new TelexInput(url_group, lv_pct(100), 42);
    popup_url_->SetTelex(false);
    popup_url_->SetMaxLen(256);
    popup_url_->SetPlaceholder("URL (không bắt buộc)");
    style_input(popup_url_);

    auto *tasks_group = make_group(right, 1);
    lv_obj_set_flex_grow(tasks_group, 1);
    lv_obj_set_style_pad_all(tasks_group, 8, 0);
    auto *tasks_title = make_text_label(tasks_group, "Nhắc việc ngày này", p.sub_text);
    lv_obj_set_height(tasks_title, 22);
    popup_list_ = lv_obj_create(tasks_group);
    lv_obj_remove_style_all(popup_list_);
    lv_obj_set_width(popup_list_, lv_pct(100));
    lv_obj_set_flex_grow(popup_list_, 1);
    lv_obj_set_flex_flow(popup_list_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(popup_list_, 4, 0);
    lv_obj_set_style_pad_all(popup_list_, 0, 0);
    lv_obj_set_style_bg_opa(popup_list_, LV_OPA_TRANSP, 0);

    RenderTaskList();

    // Enter from below with a short ease-out animation.
    lv_anim_t slide;
    lv_anim_init(&slide);
    lv_anim_set_var(&slide, popup_card_);
    lv_anim_set_values(&slide, sheet_h + 16, 0);
    lv_anim_set_time(&slide, 280);
    lv_anim_set_exec_cb(&slide, SetSheetTranslateY);
    lv_anim_set_path_cb(&slide, lv_anim_path_ease_out);
    lv_anim_start(&slide);
}

void CalendarView::CloseDayModal() {
    // TelexInput self-deletes on LV_EVENT_DELETE of its root; lv_obj_del(popup_)
    // cascades to children including both inputs' roots.
    for (auto *ctx : popup_rows_) delete ctx;
    popup_rows_.clear();
    if (popup_) {
        lv_obj_del(popup_);
        popup_ = nullptr;
        popup_card_ = nullptr;
        popup_title_ = nullptr;
        popup_list_ = nullptr;
    }
    // The TelexInput C++ objects were deleted via their OnDeleted handler; drop
    // our dangling pointers.
    popup_input_ = nullptr;
    popup_location_ = nullptr;
    popup_time_ = nullptr;
    popup_end_time_ = nullptr;
    popup_url_ = nullptr;
    popup_all_day_ = nullptr;
    popup_repeat_ = nullptr;
    popup_alert_ = nullptr;
    popup_date_.clear();
    UpdateGrid();
}

void CalendarView::RenderTaskList() {
    if (!popup_list_) return;
    const auto &p = jetson::UiTheme::Instance().Palette();
    // Clear existing rows.
    for (auto *ctx : popup_rows_) delete ctx;
    popup_rows_.clear();
    lv_obj_clean(popup_list_);

    auto tasks = LoadTasks(popup_date_);
    if (tasks.empty()) {
        auto *empty = lv_label_create(popup_list_);
        lv_obj_set_style_text_font(empty, &BUILTIN_SMALL_TEXT_FONT, 0);
        lv_obj_set_style_text_color(empty, Color(p.sub_text), 0);
        lv_label_set_text(empty, "Chưa có nhắc việc nào");
        return;
    }

    for (int i = 0; i < (int)tasks.size(); ++i) {
        const auto &t = tasks[i];
        auto *row = lv_obj_create(popup_list_);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, 36);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 6, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        // Checkbox (toggle done).
        auto *chk = lv_obj_create(row);
        lv_obj_remove_style_all(chk);
        lv_obj_set_size(chk, 22, 22);
        lv_obj_set_style_radius(chk, 5, 0);
        lv_obj_set_style_border_width(chk, 2, 0);
        lv_obj_set_style_border_color(chk, Color(p.accent), 0);
        lv_obj_set_style_bg_color(chk, t.done ? Color(p.accent) : lv_color_black(), 0);
        lv_obj_set_style_bg_opa(chk, t.done ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_add_flag(chk, LV_OBJ_FLAG_CLICKABLE);
        if (t.done) {
            auto *tick = lv_label_create(chk);
            lv_obj_set_style_text_font(tick, &BUILTIN_ICON_FONT, 0);
            lv_obj_set_style_text_color(tick, lv_color_white(), 0);
            lv_label_set_text(tick, LV_SYMBOL_OK);
            lv_obj_center(tick);
        }
        auto *rc = new RowCtx{this, popup_date_, i};
        popup_rows_.push_back(rc);
        lv_obj_add_event_cb(chk, OnRowToggle, LV_EVENT_CLICKED, rc);

        // Time label (if set).
        if (!t.time.empty()) {
            auto *tl = lv_label_create(row);
            lv_obj_set_style_text_font(tl, &BUILTIN_SMALL_TEXT_FONT, 0);
            lv_obj_set_style_text_color(tl, Color(p.accent), 0);
            lv_obj_set_width(tl, 54);
            lv_label_set_text(tl, t.time.c_str());
        }

        // Title (dim if done).
        auto *ttl = lv_label_create(row);
        lv_obj_set_flex_grow(ttl, 1);
        lv_obj_set_style_text_font(ttl, &BUILTIN_SMALL_TEXT_FONT, 0);
        lv_obj_set_style_text_color(ttl, t.done ? Color(p.sub_text) : Color(p.text), 0);
        lv_label_set_long_mode(ttl, LV_LABEL_LONG_DOT);
        lv_label_set_text(ttl, t.title.c_str());

        // Delete button.
        auto *del = lv_button_create(row);
        lv_obj_set_size(del, 32, 30);
        lv_obj_set_style_bg_color(del, Color(p.row_active), 0);
        lv_obj_set_style_radius(del, 8, 0);
        lv_obj_add_event_cb(del, OnRowDelete, LV_EVENT_CLICKED, rc);
        auto *dl = lv_label_create(del);
        lv_obj_set_style_text_font(dl, &BUILTIN_ICON_FONT, 0);
        lv_obj_set_style_text_color(dl, Color(p.sub_text), 0);
        lv_label_set_text(dl, LV_SYMBOL_TRASH);
        lv_obj_center(dl);
    }
}

bool CalendarView::AddTask() {
    if (!popup_input_ || popup_date_.empty()) return false;
    std::string title = popup_input_->Text();
    const bool all_day = popup_all_day_ &&
                         lv_obj_has_state(popup_all_day_, LV_STATE_CHECKED);
    std::string time = (!all_day && popup_time_) ? popup_time_->Text() : "";
    std::string end_time = (!all_day && popup_end_time_) ? popup_end_time_->Text() : "";
    if (title.empty()) {
        SetStatus("Nhập tên việc trước");
        lv_obj_set_style_border_width(popup_input_->obj(), 2, 0);
        lv_obj_set_style_border_color(popup_input_->obj(), lv_palette_main(LV_PALETTE_RED), 0);
        popup_input_->Focus();
        return false;
    }
    if (!IsValidTime(time) || !IsValidTime(end_time)) {
        SetStatus("Giờ không hợp lệ (HH:MM)");
        TelexInput *bad = !IsValidTime(time) ? popup_time_ : popup_end_time_;
        if (bad) {
            lv_obj_set_style_border_width(bad->obj(), 2, 0);
            lv_obj_set_style_border_color(bad->obj(), lv_palette_main(LV_PALETTE_RED), 0);
            bad->Focus();
        }
        return false;
    }
    if (!time.empty() && !end_time.empty() && end_time < time) {
        SetStatus("Giờ kết thúc phải sau giờ bắt đầu");
        lv_obj_set_style_border_width(popup_end_time_->obj(), 2, 0);
        lv_obj_set_style_border_color(popup_end_time_->obj(), lv_palette_main(LV_PALETTE_RED), 0);
        popup_end_time_->Focus();
        return false;
    }
    auto tasks = LoadTasks(popup_date_);
    tasks.push_back(Task{time, false, title});
    SaveTasks(popup_date_, tasks);
    task_dates_.insert(popup_date_);
    SaveTaskDates();
    UpdateGrid();
    SetStatus("Đã thêm nhắc việc");
    return true;
}

void CalendarView::ToggleTask(const std::string &date, int idx) {
    auto tasks = LoadTasks(date);
    if (idx < 0 || idx >= (int)tasks.size()) return;
    tasks[idx].done = !tasks[idx].done;
    SaveTasks(date, tasks);
    RenderTaskList();
}

void CalendarView::DeleteTask(const std::string &date, int idx) {
    auto tasks = LoadTasks(date);
    if (idx < 0 || idx >= (int)tasks.size()) return;
    tasks.erase(tasks.begin() + idx);
    SaveTasks(date, tasks);
    if (tasks.empty()) {
        task_dates_.erase(date);
        SaveTaskDates();
    }
    RenderTaskList();
    UpdateGrid();
}

void CalendarView::OnStart() {
    SetStatus("");
}

void CalendarView::OnResize(int w, int h) {
    LayoutCalendar(w, h);
}

// ---- event handlers (run on handler_thread; take the LVGL lock) ----

void CalendarView::OnPrev(lv_event_t *e) {
    LvglLockGuard lock;
    auto *self = static_cast<CalendarView *>(lv_event_get_user_data(e));
    self->ChangeMonth(-1);
}
void CalendarView::OnNext(lv_event_t *e) {
    LvglLockGuard lock;
    auto *self = static_cast<CalendarView *>(lv_event_get_user_data(e));
    self->ChangeMonth(1);
}
void CalendarView::OnToday(lv_event_t *e) {
    LvglLockGuard lock;
    auto *self = static_cast<CalendarView *>(lv_event_get_user_data(e));
    self->GoToday();
}
void CalendarView::OnDayClicked(lv_event_t *e) {
    LvglLockGuard lock;
    auto *ctx = static_cast<DayCtx *>(lv_event_get_user_data(e));
    if (ctx->day > 0) ctx->self->OpenDayModal(ctx->day);
}
void CalendarView::OnDayDeleted(lv_event_t *e) {
    auto *ctx = static_cast<DayCtx *>(lv_event_get_user_data(e));
    delete ctx;
}
void CalendarView::OnPopupDismiss(lv_event_t *e) {
    LvglLockGuard lock;
    auto *self = static_cast<CalendarView *>(lv_event_get_user_data(e));
    if (lv_event_get_target(e) == self->popup_) self->CloseDayModal();
}
void CalendarView::OnPopupClose(lv_event_t *e) {
    LvglLockGuard lock;
    auto *self = static_cast<CalendarView *>(lv_event_get_user_data(e));
    self->CloseDayModal();
}
void CalendarView::OnAddTask(lv_event_t *e) {
    LvglLockGuard lock;
    auto *self = static_cast<CalendarView *>(lv_event_get_user_data(e));
    if (self->AddTask()) self->CloseDayModal();
}
void CalendarView::OnAllDayChanged(lv_event_t *e) {
    LvglLockGuard lock;
    auto *self = static_cast<CalendarView *>(lv_event_get_user_data(e));
    const bool all_day = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e),
                                          LV_STATE_CHECKED);
    auto set_enabled = [&](TelexInput *input) {
        if (!input || !input->obj()) return;
        if (all_day) {
            lv_obj_add_state(input->obj(), LV_STATE_DISABLED);
            lv_obj_set_style_opa(input->obj(), LV_OPA_40, 0);
        } else {
            lv_obj_remove_state(input->obj(), LV_STATE_DISABLED);
            lv_obj_set_style_opa(input->obj(), LV_OPA_COVER, 0);
        }
    };
    set_enabled(self->popup_time_);
    set_enabled(self->popup_end_time_);
}
void CalendarView::OnRowToggle(lv_event_t *e) {
    LvglLockGuard lock;
    auto *rc = static_cast<RowCtx *>(lv_event_get_user_data(e));
    rc->self->ToggleTask(rc->date, rc->idx);
}
void CalendarView::OnRowDelete(lv_event_t *e) {
    LvglLockGuard lock;
    auto *rc = static_cast<RowCtx *>(lv_event_get_user_data(e));
    rc->self->DeleteTask(rc->date, rc->idx);
}

} // namespace home
