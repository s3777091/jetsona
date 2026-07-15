#include "calendar_view.h"
#include "fonts.h"
#include "settings.h"
#include "ui_theme.h"

#include <lvgl.h>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <set>
#include <sstream>
#include <string>

namespace home {

namespace {
lv_color_t Color(uint32_t rgb) {
    return lv_color_make((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}
const char *kWeekdays[7] = {"T2", "T3", "T4", "T5", "T6", "T7", "CN"};

struct MarkSet { std::set<std::string> s; };
} // namespace

CalendarView::CalendarView(lv_obj_t *parent, int width, int height, ClosedCb on_closed)
    : OverlayView(parent, width, height, "Lịch", std::move(on_closed)) {
    std::time_t now = std::time(nullptr);
    std::tm *t = std::localtime(&now);
    year_ = t->tm_year + 1900;
    month_ = t->tm_mon;
    BuildBody();
    LoadMarks();
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

void CalendarView::LoadMarks() {
    Settings s("calendar", false);
    std::string v = s.GetString("marks", "");
    // Stored comma-separated "YYYY-MM-DD".
    std::istringstream iss(v);
    std::string token;
    while (std::getline(iss, token, ',')) {
        if (!token.empty()) marks_.insert(token);
    }
}
void CalendarView::SaveMarks() {
    Settings s("calendar", true);
    std::string out;
    for (const auto &k : marks_) {
        if (!out.empty()) out += ",";
        out += k;
    }
    s.SetString("marks", out);
}

void CalendarView::BuildBody() {
    const auto &p = jetson::UiTheme::Instance().Palette();

    // Top row: prev | month label | next
    auto *top = lv_obj_create(body_);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, width_ - 16, 44);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);

    auto *prev = lv_button_create(top);
    lv_obj_set_size(prev, 44, 36);
    lv_obj_set_style_bg_color(prev, Color(p.button), 0);
    lv_obj_add_event_cb(prev, OnPrev, LV_EVENT_CLICKED, this);
    auto *pl = lv_label_create(prev); lv_label_set_text(pl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(pl, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(pl, Color(p.text), 0); lv_obj_center(pl);

    month_label_ = lv_label_create(top);
    lv_obj_set_style_text_font(month_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(month_label_, Color(p.text), 0);

    auto *next = lv_button_create(top);
    lv_obj_set_size(next, 44, 36);
    lv_obj_set_style_bg_color(next, Color(p.button), 0);
    lv_obj_add_event_cb(next, OnNext, LV_EVENT_CLICKED, this);
    auto *nl = lv_label_create(next); lv_label_set_text(nl, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(nl, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(nl, Color(p.text), 0); lv_obj_center(nl);

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

    // Day grid: 7 cols x 6 rows via flex-wrap (simplest portable layout).
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
        lv_obj_set_height(cell, 44);
        lv_obj_set_style_radius(cell, 10, 0);
        lv_obj_set_style_bg_color(cell, Color(p.row), 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        auto *num = lv_label_create(cell);
        lv_obj_set_style_text_font(num, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(num, Color(p.text), 0);
        lv_obj_center(num);
        auto *dot = lv_obj_create(cell);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 5, 5);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, Color(p.accent), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_TRANSP, 0);
        lv_obj_align(dot, LV_ALIGN_BOTTOM_MID, 0, -2);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
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
    int mondayOffset = (firstWday + 6) % 7; // index of the 1st in a Monday-first grid
    int dim = DaysInMonth(year_, month_);

    for (int i = 0; i < 42; ++i) {
        int day = i - mondayOffset + 1;
        lv_obj_t *cell = cells_[i];
        lv_obj_t *num = lv_obj_get_child(cell, 0);
        lv_obj_t *dot = lv_obj_get_child(cell, 1);
        day_ctxs_[i]->day = 0;
        if (day >= 1 && day <= dim) {
            day_ctxs_[i]->day = day;
            char b[8]; std::snprintf(b, sizeof(b), "%d", day);
            lv_label_set_text(num, b);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_HIDDEN);
            bool today = IsToday(year_, month_, day);
            bool marked = marks_.count(Key(year_, month_, day)) > 0;
            lv_obj_set_style_bg_color(cell, today ? Color(p.row_active) : Color(p.row), 0);
            lv_obj_set_style_text_color(num, today ? Color(p.accent) : Color(p.text), 0);
            lv_obj_set_style_bg_opa(dot, marked ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        } else {
            lv_label_set_text(num, "");
            lv_obj_set_style_bg_opa(dot, LV_OPA_TRANSP, 0);
            lv_obj_set_style_bg_color(cell, Color(p.row), 0);
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

void CalendarView::ToggleMark(int day) {
    if (day <= 0) return;
    std::string k = Key(year_, month_, day);
    if (marks_.count(k)) marks_.erase(k);
    else marks_.insert(k);
    SaveMarks();
    UpdateGrid();
    SetStatus(("Đánh dấu: " + k).c_str());
}

void CalendarView::OnStart() {
    SetStatus("Chạm ngày để đánh dấu");
}

void CalendarView::OnPrev(lv_event_t *e) {
    auto *self = static_cast<CalendarView *>(lv_event_get_user_data(e));
    self->ChangeMonth(-1);
}
void CalendarView::OnNext(lv_event_t *e) {
    auto *self = static_cast<CalendarView *>(lv_event_get_user_data(e));
    self->ChangeMonth(1);
}
void CalendarView::OnDayClicked(lv_event_t *e) {
    auto *ctx = static_cast<DayCtx *>(lv_event_get_user_data(e));
    ctx->self->ToggleMark(ctx->day);
}
void CalendarView::OnDayDeleted(lv_event_t *e) {
    auto *ctx = static_cast<DayCtx *>(lv_event_get_user_data(e));
    delete ctx;
}

} // namespace home