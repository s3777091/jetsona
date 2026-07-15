#include "display/views/calendar_view.h"
#include "display/common/lvgl_utils.h"
#include "fonts.h"
#include "settings.h"
#include "display/theme/ui_theme.h"

#include <lvgl.h>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace home {

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
} // namespace

CalendarView::CalendarView(lv_obj_t *parent, int width, int height, ClosedCb on_closed)
    : OverlayView(parent, width, height, "Lịch", std::move(on_closed)) {
    std::time_t now = std::time(nullptr);
    std::tm *t = std::localtime(&now);
    year_ = t->tm_year + 1900;
    month_ = t->tm_mon;
    BuildBody();
    LoadTaskDates();
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

    // Top row: prev | month label (grow) | "Hôm nay" | next
    auto *top = lv_obj_create(body_);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, width_ - 16, 44);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top, 8, 0);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);

    auto makeNav = [&](const char *sym, lv_event_cb_t cb) {
        auto *b = lv_button_create(top);
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

    month_label_ = lv_label_create(top);
    lv_obj_set_flex_grow(month_label_, 1);
    lv_obj_set_style_text_font(month_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(month_label_, Color(p.text), 0);

    today_btn_ = lv_button_create(top);
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
    auto *wd = lv_obj_create(body_);
    lv_obj_remove_style_all(wd);
    lv_obj_set_size(wd, width_ - 16, 24);
    lv_obj_set_flex_flow(wd, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(wd, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 7; ++i) {
        auto *l = lv_label_create(wd);
        lv_obj_set_flex_grow(l, 1);
        lv_obj_set_style_text_font(l, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(l, Color(p.sub_text), 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(l, kWeekdays[i]);
    }

    // Day grid: 7 cols x 6 rows via flex-wrap.
    grid_ = lv_obj_create(body_);
    lv_obj_remove_style_all(grid_);
    lv_obj_set_size(grid_, width_ - 16, height_ - 80 - 44 - 24 - 16);
    lv_obj_set_flex_flow(grid_, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid_, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(grid_, 4, 0);
    lv_obj_set_style_pad_column(grid_, 4, 0);
    lv_obj_clear_flag(grid_, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 42; ++i) {
        auto *cell = lv_obj_create(grid_);
        lv_obj_remove_style_all(cell);
        lv_obj_set_flex_grow(cell, 1);
        lv_obj_set_height(cell, 48);
        lv_obj_set_style_radius(cell, 12, 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(cell, 0, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        auto *num = lv_label_create(cell);
        lv_obj_set_style_text_font(num, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(num, Color(p.text), 0);
        lv_obj_align(num, LV_ALIGN_TOP_MID, 0, 6);
        auto *dot = lv_obj_create(cell);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 6, 6);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, Color(p.accent), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_TRANSP, 0);
        lv_obj_align(dot, LV_ALIGN_BOTTOM_MID, 0, -4);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        auto *ctx = new DayCtx{this, 0};
        day_ctxs_[i] = ctx;
        lv_obj_add_event_cb(cell, OnDayClicked, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(cell, OnDayDeleted, LV_EVENT_DELETE, ctx);
        cells_[i] = cell;
    }

    UpdateGrid();
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
        lv_obj_t *num = lv_obj_get_child(cell, 0);
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
            // macOS today: filled accent circle, white number.
            lv_obj_set_style_bg_color(cell, Color(p.accent), 0);
            lv_obj_set_style_bg_opa(cell, today ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            lv_obj_set_style_text_color(num, today ? lv_color_white() : Color(p.text), 0);
            // Task dot below the number (accent on normal days, white on today).
            lv_obj_set_style_bg_color(dot, today ? lv_color_white() : Color(p.accent), 0);
            lv_obj_set_style_bg_opa(dot, has_tasks ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        } else {
            lv_label_set_text(num, "");
            lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_TRANSP, 0);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void CalendarView::ChangeMonth(int delta) {
    month_ += delta;
    while (month_ < 0) { month_ += 12; year_ -= 1; }
    while (month_ > 11) { month_ -= 12; year_ += 1; }
    UpdateGrid();
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

    popup_ = lv_obj_create(overlay_);
    lv_obj_remove_style_all(popup_);
    lv_obj_set_size(popup_, width_, height_);
    lv_obj_set_style_bg_color(popup_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(popup_, LV_OPA_50, 0);
    lv_obj_add_flag(popup_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(popup_, OnPopupDismiss, LV_EVENT_CLICKED, this);

    popup_card_ = lv_obj_create(popup_);
    lv_obj_remove_style_all(popup_card_);
    lv_obj_set_size(popup_card_, 340, 380);
    lv_obj_set_style_bg_color(popup_card_, Color(p.row), 0);
    lv_obj_set_style_bg_opa(popup_card_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(popup_card_, 16, 0);
    lv_obj_set_style_pad_all(popup_card_, 16, 0);
    lv_obj_set_style_pad_row(popup_card_, 10, 0);
    lv_obj_set_flex_flow(popup_card_, LV_FLEX_FLOW_COLUMN);
    lv_obj_align(popup_card_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(popup_card_, LV_OBJ_FLAG_CLICKABLE);

    popup_title_ = lv_label_create(popup_card_);
    lv_obj_set_style_text_font(popup_title_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(popup_title_, Color(p.text), 0);
    lv_label_set_text(popup_title_, FormatDateTitle(year_, month_, day).c_str());

    // Scrollable task list.
    popup_list_ = lv_obj_create(popup_card_);
    lv_obj_remove_style_all(popup_list_);
    lv_obj_set_width(popup_list_, 308);
    lv_obj_set_flex_grow(popup_list_, 1);
    lv_obj_set_flex_flow(popup_list_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(popup_list_, 6, 0);
    lv_obj_set_style_pad_all(popup_list_, 0, 0);
    lv_obj_set_style_bg_opa(popup_list_, LV_OPA_TRANSP, 0);

    // Add-task row: [time][title][+]
    auto *addrow = lv_obj_create(popup_card_);
    lv_obj_remove_style_all(addrow);
    lv_obj_set_width(addrow, 308);
    lv_obj_set_height(addrow, 44);
    lv_obj_set_flex_flow(addrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(addrow, 6, 0);
    lv_obj_clear_flag(addrow, LV_OBJ_FLAG_SCROLLABLE);

    popup_time_ = new TelexInput(addrow, 70, 40);
    popup_time_->SetTelex(false);
    popup_time_->SetMaxLen(5);
    popup_time_->SetPlaceholder("HH:MM");

    popup_input_ = new TelexInput(addrow, 190, 40);
    popup_input_->SetTelex(IsKbdVi());
    popup_input_->SetMaxLen(64);
    popup_input_->SetPlaceholder("Nhắc việc...");

    auto *addbtn = lv_button_create(addrow);
    lv_obj_set_size(addbtn, 44, 40);
    lv_obj_set_style_bg_color(addbtn, Color(p.accent), 0);
    lv_obj_set_style_radius(addbtn, 8, 0);
    lv_obj_add_event_cb(addbtn, OnAddTask, LV_EVENT_CLICKED, this);
    auto *al = lv_label_create(addbtn);
    lv_obj_set_style_text_font(al, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(al, lv_color_white(), 0);
    lv_label_set_text(al, LV_SYMBOL_PLUS);
    lv_obj_center(al);

    // Close button.
    auto *close = lv_button_create(popup_card_);
    lv_obj_set_width(close, 120);
    lv_obj_set_height(close, 40);
    lv_obj_set_style_bg_color(close, Color(p.button), 0);
    lv_obj_set_style_radius(close, 10, 0);
    lv_obj_add_event_cb(close, OnPopupClose, LV_EVENT_CLICKED, this);
    auto *cl = lv_label_create(close);
    lv_obj_set_style_text_font(cl, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(cl, Color(p.text), 0);
    lv_label_set_text(cl, "Đóng");
    lv_obj_center(cl);

    RenderTaskList();
    if (popup_input_) popup_input_->Focus();
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
    popup_time_ = nullptr;
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
        lv_obj_set_style_text_font(empty, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(empty, Color(p.sub_text), 0);
        lv_label_set_text(empty, "Chưa có nhắc việc nào");
        return;
    }

    for (int i = 0; i < (int)tasks.size(); ++i) {
        const auto &t = tasks[i];
        auto *row = lv_obj_create(popup_list_);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, 308);
        lv_obj_set_height(row, 44);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 8, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        // Checkbox (toggle done).
        auto *chk = lv_obj_create(row);
        lv_obj_remove_style_all(chk);
        lv_obj_set_size(chk, 24, 24);
        lv_obj_set_style_radius(chk, 6, 0);
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
            lv_obj_set_style_text_font(tl, &BUILTIN_TEXT_FONT, 0);
            lv_obj_set_style_text_color(tl, Color(p.accent), 0);
            lv_obj_set_width(tl, 70);
            lv_label_set_text(tl, t.time.c_str());
        }

        // Title (dim if done).
        auto *ttl = lv_label_create(row);
        lv_obj_set_flex_grow(ttl, 1);
        lv_obj_set_style_text_font(ttl, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(ttl, t.done ? Color(p.sub_text) : Color(p.text), 0);
        lv_label_set_long_mode(ttl, LV_LABEL_LONG_DOT);
        lv_label_set_text(ttl, t.title.c_str());

        // Delete button.
        auto *del = lv_button_create(row);
        lv_obj_set_size(del, 36, 32);
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

void CalendarView::AddTask() {
    if (!popup_input_ || popup_date_.empty()) return;
    std::string title = popup_input_->Text();
    std::string time = popup_time_ ? popup_time_->Text() : "";
    if (title.empty()) {
        SetStatus("Nhập tên việc trước");
        return;
    }
    if (!IsValidTime(time)) {
        SetStatus("Giờ không hợp lệ (HH:MM)");
        return;
    }
    auto tasks = LoadTasks(popup_date_);
    tasks.push_back(Task{time, false, title});
    SaveTasks(popup_date_, tasks);
    task_dates_.insert(popup_date_);
    SaveTaskDates();
    popup_input_->Clear();
    if (popup_time_) popup_time_->Clear();
    RenderTaskList();
    UpdateGrid();
    if (popup_input_) popup_input_->Focus();
    SetStatus("Đã thêm nhắc việc");
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
    SetStatus("Chạm ngày để xem / thêm nhắc việc");
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
    self->AddTask();
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