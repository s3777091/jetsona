#include "display/views/reminders_view.h"

#include "display/common/lvgl_utils.h"
#include "display/core/app_icons.h"
#include "display/theme/ui_theme.h"
#include "fonts.h"
#include "settings.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>
#include <utility>

namespace home {

namespace {

using jetson::ui::Color;
using jetson::ui::LvglLockGuard;

constexpr const char *kStoreKey = "items_v1";

bool IsKbdVi() {
    return Settings("input", false).GetString("kbd_lang", "en") == "vi";
}

void SetSheetTranslateY(void *obj, int32_t value) {
    lv_obj_set_style_translate_y(static_cast<lv_obj_t *>(obj), value, 0);
}

std::string HexEncode(const std::string &value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 2);
    for (unsigned char c : value) {
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 0x0f]);
    }
    return out;
}

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string HexDecode(const std::string &value) {
    if (value.size() % 2 != 0) return {};
    std::string out;
    out.reserve(value.size() / 2);
    for (size_t i = 0; i < value.size(); i += 2) {
        const int hi = HexDigit(value[i]);
        const int lo = HexDigit(value[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return out;
}

std::string CreatedNow() {
    std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char value[48];
    std::snprintf(value, sizeof(value), "Tạo lúc %02d:%02d, %02d/%02d/%04d",
                  local.tm_hour, local.tm_min, local.tm_mday,
                  local.tm_mon + 1, local.tm_year + 1900);
    return value;
}

lv_obj_t *MakeIconButton(lv_obj_t *parent, int size, uint32_t bg,
                         const char *symbol, lv_event_cb_t callback,
                         void *user_data, uint32_t icon_color,
                         const char *png_icon = nullptr) {
    auto *button = lv_button_create(parent);
    lv_obj_set_size(button, size, size);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(button, Color(bg), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);

    lv_obj_t *icon;
    if (png_icon) {
        icon = jetson::ui::CreateAppIcon(button, png_icon, size * 3 / 5);
        lv_obj_set_style_image_recolor(icon, Color(icon_color), 0);
        lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
    } else {
        icon = lv_label_create(button);
        lv_obj_set_style_text_font(icon, &BUILTIN_ICON_FONT, 0);
        lv_obj_set_style_text_color(icon, Color(icon_color), 0);
        lv_label_set_text(icon, symbol);
    }
    lv_obj_center(icon);
    return button;
}

/* Pin icon on a task row: assets/icons/app/pin.png recolored to `color`.
 * The unpinned state renders dimmed instead of the old outline-only variant. */
void DrawPin(lv_obj_t *button, uint32_t color, bool filled) {
    auto *img = jetson::ui::CreateAppIcon(button, "pin", 18);
    lv_obj_set_style_image_recolor(img, Color(color), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    if (!filled) lv_obj_set_style_opa(img, LV_OPA_50, 0);
    lv_obj_center(img);
}

} // namespace

RemindersView::RemindersView(lv_obj_t *parent, int width, int height,
                             ClosedCb on_closed)
    : OverlayView(parent, width, height, "Lời nhắc", std::move(on_closed)) {
    Load();
    BuildBody();
}

RemindersView::~RemindersView() {
    for (auto *ctx : row_ctxs_) delete ctx;
    row_ctxs_.clear();
}

void RemindersView::OnStart() {
    SetStatus("");
}

void RemindersView::BuildBody() {
    const auto &p = jetson::UiTheme::Instance().Palette();
    lv_obj_set_style_pad_all(body_, 10, 0);
    lv_obj_set_style_pad_row(body_, 8, 0);
    lv_obj_set_flex_flow(body_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    auto *composer = lv_obj_create(body_);
    lv_obj_remove_style_all(composer);
    lv_obj_set_size(composer, std::min(width_ - 28, 720), 48);
    lv_obj_set_style_bg_color(composer, Color(p.row), 0);
    lv_obj_set_style_bg_opa(composer, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(composer, 16, 0);
    lv_obj_set_style_pad_left(composer, 12, 0);
    lv_obj_set_style_pad_right(composer, 5, 0);
    lv_obj_set_style_pad_column(composer, 6, 0);
    lv_obj_set_flex_flow(composer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(composer, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(composer, LV_OBJ_FLAG_SCROLLABLE);

    input_ = new TelexInput(composer, 1, 40);
    input_->SetTelex(IsKbdVi());
    input_->SetMaxLen(96);
    input_->SetPlaceholder("Thêm việc cần làm...");
    input_->SetFont(&BUILTIN_SMALL_TEXT_FONT);
    lv_obj_set_flex_grow(input_->obj(), 1);
    lv_obj_set_style_bg_opa(input_->obj(), LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(input_->obj(), 0, 0);
    lv_obj_add_event_cb(input_->obj(), OnAdd, LV_EVENT_READY, this);

    MakeIconButton(composer, 38, p.accent, LV_SYMBOL_PLUS, OnAdd, this, 0xffffff,
                   "add");

    auto *hint = lv_label_create(body_);
    lv_obj_set_width(hint, std::min(width_ - 28, 720));
    lv_obj_set_style_text_font(hint, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(hint, Color(p.sub_text), 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(hint, "Ghim để ưu tiên • Kéo tay cầm ≡ để sắp xếp");

    list_ = lv_obj_create(body_);
    lv_obj_remove_style_all(list_);
    lv_obj_set_width(list_, std::min(width_ - 28, 720));
    lv_obj_set_flex_grow(list_, 1);
    lv_obj_set_flex_flow(list_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list_, 8, 0);
    lv_obj_set_style_pad_right(list_, 4, 0);
    lv_obj_set_scroll_dir(list_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_, LV_SCROLLBAR_MODE_AUTO);
    Render();
}

void RemindersView::Load() {
    tasks_.clear();
    next_id_ = 1;
    const std::string saved = Settings("reminders", false).GetString(kStoreKey, "");
    std::istringstream records(saved);
    std::string record;
    while (std::getline(records, record, '~')) {
        if (record.empty()) continue;
        std::array<std::string, 6> fields;
        std::istringstream parts(record);
        bool valid = true;
        for (size_t i = 0; i < fields.size(); ++i) {
            if (!std::getline(parts, fields[i], '|')) { valid = false; break; }
        }
        if (!valid) continue;
        try {
            Task task;
            task.id = std::stoi(fields[0]);
            task.pinned = fields[1] == "1";
            task.done = fields[2] == "1";
            task.color = static_cast<uint32_t>(std::stoul(fields[3], nullptr, 16));
            task.title = HexDecode(fields[4]);
            task.info = HexDecode(fields[5]);
            if (task.id <= 0 || task.title.empty()) continue;
            tasks_.push_back(std::move(task));
            next_id_ = std::max(next_id_, tasks_.back().id + 1);
        } catch (...) {
            // Ignore a malformed record and keep the rest of the saved list.
        }
    }
}

void RemindersView::ReloadFromStore() {
    Load();
    Render();
}

void RemindersView::Save() const {
    std::ostringstream out;
    for (size_t i = 0; i < tasks_.size(); ++i) {
        const auto &task = tasks_[i];
        if (i != 0) out << '~';
        out << task.id << '|' << (task.pinned ? 1 : 0) << '|'
            << (task.done ? 1 : 0) << '|' << std::uppercase << std::hex
            << std::setw(6) << std::setfill('0') << task.color << std::dec
            << '|' << HexEncode(task.title) << '|' << HexEncode(task.info);
    }
    Settings("reminders", true).SetString(kStoreKey, out.str());
}

uint32_t RemindersView::RandomBrightColor() const {
    // All choices remain readable in both themes and deliberately avoid dark
    // shades. Randomness only chooses within this safe palette.
    static constexpr std::array<uint32_t, 12> kColors = {
        0xff6b6b, 0xff9f43, 0xffd93d, 0x6bcb77,
        0x4dd0e1, 0x45b7d1, 0x4d96ff, 0x8b7cff,
        0xb980f0, 0xff8fab, 0xffb4a2, 0x72ddf7,
    };
    static std::mt19937 engine(std::random_device{}());
    std::uniform_int_distribution<size_t> pick(0, kColors.size() - 1);
    return kColors[pick(engine)];
}

void RemindersView::AddTask() {
    if (!input_) return;
    const std::string title = input_->Text();
    if (title.find_first_not_of(" \t\r\n") == std::string::npos) {
        input_->Focus();
        return;
    }
    tasks_.push_back(Task{next_id_++, false, false, RandomBrightColor(),
                          title, CreatedNow()});
    input_->Clear();
    Save();
    Render();
}

RemindersView::Task *RemindersView::FindTask(int id) {
    auto it = std::find_if(tasks_.begin(), tasks_.end(),
                           [id](const Task &task) { return task.id == id; });
    return it == tasks_.end() ? nullptr : &*it;
}

const RemindersView::Task *RemindersView::FindTask(int id) const {
    auto it = std::find_if(tasks_.begin(), tasks_.end(),
                           [id](const Task &task) { return task.id == id; });
    return it == tasks_.end() ? nullptr : &*it;
}

void RemindersView::Render() {
    if (!list_) return;
    for (auto *ctx : row_ctxs_) delete ctx;
    row_ctxs_.clear();
    lv_obj_clean(list_);

    if (tasks_.empty()) {
        const auto &p = jetson::UiTheme::Instance().Palette();
        auto *empty = lv_obj_create(list_);
        lv_obj_remove_style_all(empty);
        lv_obj_set_size(empty, lv_pct(100), 180);
        lv_obj_set_flex_flow(empty, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(empty, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(empty, LV_OBJ_FLAG_SCROLLABLE);

        auto *mark = lv_obj_create(empty);
        lv_obj_remove_style_all(mark);
        lv_obj_set_size(mark, 46, 46);
        lv_obj_set_style_radius(mark, 14, 0);
        lv_obj_set_style_bg_color(mark, Color(0xffd93d), 0);
        lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);

        auto *title = lv_label_create(empty);
        lv_obj_set_style_text_font(title, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(title, Color(p.text), 0);
        lv_label_set_text(title, "Chưa có lời nhắc");

        auto *sub = lv_label_create(empty);
        lv_obj_set_style_text_font(sub, &BUILTIN_SMALL_TEXT_FONT, 0);
        lv_obj_set_style_text_color(sub, Color(p.sub_text), 0);
        lv_label_set_text(sub, "Nhập một công việc ở phía trên để bắt đầu");
        return;
    }

    RenderSection("Đã ghim", true);
    RenderSection("Tất cả lời nhắc", false);
}

void RemindersView::RenderSection(const char *title, bool pinned) {
    const bool has_items = std::any_of(tasks_.begin(), tasks_.end(),
                                       [pinned](const Task &t) {
                                           return t.pinned == pinned;
                                       });
    if (!has_items) return;
    const auto &p = jetson::UiTheme::Instance().Palette();

    auto *label = lv_label_create(list_);
    lv_obj_set_width(label, lv_pct(100));
    lv_obj_set_style_pad_left(label, 10, 0);
    lv_obj_set_style_text_font(label, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(label, pinned ? Color(p.accent) : Color(p.sub_text), 0);
    lv_label_set_text(label, title);

    auto *section = lv_obj_create(list_);
    lv_obj_remove_style_all(section);
    lv_obj_set_width(section, lv_pct(100));
    lv_obj_set_height(section, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(section, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(section, 7, 0);
    lv_obj_clear_flag(section, LV_OBJ_FLAG_SCROLLABLE);

    int animation_delay = 0;
    for (const auto &task : tasks_) {
        if (task.pinned != pinned) continue;
        auto *row = lv_obj_create(section);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 66);
        lv_obj_set_style_bg_color(row, Color(p.row), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 17, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, Color(p.border), 0);
        lv_obj_set_style_pad_left(row, 9, 0);
        lv_obj_set_style_pad_right(row, 7, 0);
        lv_obj_set_style_pad_column(row, 8, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        auto *ctx = new RowCtx{this, task.id, row, 0, false};
        row_ctxs_.push_back(ctx);

        auto *check = lv_obj_create(row);
        lv_obj_remove_style_all(check);
        lv_obj_set_size(check, 26, 26);
        lv_obj_set_style_radius(check, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(check, 2, 0);
        lv_obj_set_style_border_color(check, Color(task.color), 0);
        lv_obj_set_style_bg_color(check, Color(task.color), 0);
        lv_obj_set_style_bg_opa(check, task.done ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_add_flag(check, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(check, OnDone, LV_EVENT_CLICKED, ctx);
        if (task.done) {
            auto *tick = lv_label_create(check);
            lv_obj_set_style_text_font(tick, &BUILTIN_ICON_FONT, 0);
            lv_obj_set_style_text_color(tick, lv_color_white(), 0);
            lv_label_set_text(tick, LV_SYMBOL_OK);
            lv_obj_center(tick);
        }

        auto *color = lv_obj_create(row);
        lv_obj_remove_style_all(color);
        lv_obj_set_size(color, 36, 36);
        lv_obj_set_style_radius(color, 11, 0);
        lv_obj_set_style_bg_color(color, Color(task.color), 0);
        lv_obj_set_style_bg_opa(color, task.done ? LV_OPA_50 : LV_OPA_COVER, 0);
        lv_obj_clear_flag(color, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

        auto *copy = lv_obj_create(row);
        lv_obj_remove_style_all(copy);
        lv_obj_set_size(copy, 1, 54);
        lv_obj_set_flex_grow(copy, 1);
        lv_obj_set_flex_flow(copy, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(copy, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        lv_obj_clear_flag(copy, LV_OBJ_FLAG_SCROLLABLE);

        auto *task_title = lv_label_create(copy);
        lv_obj_set_width(task_title, lv_pct(100));
        lv_obj_set_style_text_font(task_title, &BUILTIN_SMALL_TEXT_FONT, 0);
        lv_obj_set_style_text_color(task_title,
                                    task.done ? Color(p.sub_text) : Color(p.text), 0);
        lv_obj_set_style_text_decor(task_title,
                                    task.done ? LV_TEXT_DECOR_STRIKETHROUGH
                                              : LV_TEXT_DECOR_NONE,
                                    0);
        lv_label_set_long_mode(task_title, LV_LABEL_LONG_DOT);
        lv_label_set_text(task_title, task.title.c_str());

        auto *info_text = lv_label_create(copy);
        lv_obj_set_width(info_text, lv_pct(100));
        lv_obj_set_style_text_font(info_text, &BUILTIN_SMALL_TEXT_FONT, 0);
        lv_obj_set_style_text_color(info_text, Color(p.sub_text), 0);
        lv_label_set_long_mode(info_text, LV_LABEL_LONG_DOT);
        lv_label_set_text(info_text, task.info.c_str());

        // Information button: a plain circled "i" is guaranteed by the text
        // font and remains clear at this small touch-target size.
        auto *info = lv_button_create(row);
        lv_obj_set_size(info, 34, 34);
        lv_obj_set_style_radius(info, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(info, Color(p.button), 0);
        lv_obj_set_style_bg_opa(info, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_width(info, 0, 0);
        lv_obj_set_style_pad_all(info, 0, 0);
        lv_obj_add_event_cb(info, OnInfo, LV_EVENT_CLICKED, ctx);
        auto *i_label = lv_label_create(info);
        lv_obj_set_style_text_font(i_label, &BUILTIN_SMALL_TEXT_FONT, 0);
        lv_obj_set_style_text_color(i_label, Color(p.accent), 0);
        lv_label_set_text(i_label, "i");
        lv_obj_center(i_label);

        auto *pin = lv_button_create(row);
        lv_obj_set_size(pin, 34, 34);
        lv_obj_set_style_radius(pin, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(pin, task.pinned ? Color(p.accent) : Color(p.button), 0);
        lv_obj_set_style_bg_opa(pin, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_width(pin, 0, 0);
        lv_obj_set_style_pad_all(pin, 0, 0);
        lv_obj_add_event_cb(pin, OnPin, LV_EVENT_CLICKED, ctx);
        DrawPin(pin, task.pinned ? 0xffffff : p.sub_text, task.pinned);

        auto *grip = lv_obj_create(row);
        lv_obj_remove_style_all(grip);
        lv_obj_set_size(grip, 34, 44);
        lv_obj_set_style_radius(grip, 10, 0);
        lv_obj_set_style_bg_color(grip, Color(p.button), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(grip, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_flag(grip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(grip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(grip, OnDrag, LV_EVENT_ALL, ctx);
        auto *grip_label = lv_label_create(grip);
        lv_obj_set_style_text_font(grip_label, &BUILTIN_ICON_FONT, 0);
        lv_obj_set_style_text_color(grip_label, Color(p.sub_text), 0);
        lv_label_set_text(grip_label, LV_SYMBOL_BARS);
        lv_obj_center(grip_label);
        lv_obj_clear_flag(grip_label, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

        lv_obj_fade_in(row, 150, animation_delay);
        animation_delay = std::min(animation_delay + 18, 90);
    }
}

void RemindersView::ToggleDone(int id) {
    if (Task *task = FindTask(id)) {
        task->done = !task->done;
        Save();
        Render();
    }
}

void RemindersView::TogglePinned(int id) {
    if (Task *task = FindTask(id)) {
        task->pinned = !task->pinned;
        Save();
        Render();
    }
}

void RemindersView::MoveTask(RowCtx *ctx, int direction) {
    if (!ctx || direction == 0) return;
    auto current = std::find_if(tasks_.begin(), tasks_.end(),
                                [ctx](const Task &task) {
                                    return task.id == ctx->id;
                                });
    if (current == tasks_.end()) return;
    const bool pinned = current->pinned;
    auto target = current;
    if (direction < 0) {
        while (target != tasks_.begin()) {
            --target;
            if (target->pinned == pinned) break;
        }
        if (target == current || target->pinned != pinned) return;
    } else {
        do {
            ++target;
            if (target == tasks_.end()) return;
        } while (target->pinned != pinned);
    }

    std::iter_swap(current, target);
    const uint32_t visual_index = lv_obj_get_index(ctx->row);
    if (direction < 0 && visual_index > 0) {
        lv_obj_move_to_index(ctx->row, static_cast<int32_t>(visual_index - 1));
    } else if (direction > 0) {
        lv_obj_move_to_index(ctx->row, static_cast<int32_t>(visual_index + 1));
    }
    ctx->drag_changed = true;
}

void RemindersView::OpenInfo(int id) {
    const Task *task = FindTask(id);
    if (!task) return;
    CloseInfo();
    info_task_id_ = id;
    const auto &p = jetson::UiTheme::Instance().Palette();
    const int sheet_w = std::min(width_ - 24, 720);
    const int sheet_h = std::min(height_ - 18, 278);

    info_overlay_ = lv_obj_create(overlay_);
    lv_obj_remove_style_all(info_overlay_);
    lv_obj_set_size(info_overlay_, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(info_overlay_, 0, 0);
    lv_obj_set_style_bg_color(info_overlay_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(info_overlay_, LV_OPA_50, 0);
    lv_obj_add_flag(info_overlay_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(info_overlay_, OnInfoDismiss, LV_EVENT_CLICKED, this);

    info_card_ = lv_obj_create(info_overlay_);
    lv_obj_remove_style_all(info_card_);
    lv_obj_set_size(info_card_, sheet_w, sheet_h);
    lv_obj_align(info_card_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(info_card_, Color(p.panel), 0);
    lv_obj_set_style_bg_opa(info_card_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(info_card_, 24, 0);
    lv_obj_set_style_shadow_color(info_card_, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(info_card_, LV_OPA_30, 0);
    lv_obj_set_style_shadow_width(info_card_, 24, 0);
    lv_obj_set_style_pad_all(info_card_, 14, 0);
    lv_obj_set_style_pad_row(info_card_, 10, 0);
    lv_obj_set_flex_flow(info_card_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(info_card_, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(info_card_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(info_card_, LV_OBJ_FLAG_SCROLLABLE);

    auto *grabber = lv_obj_create(info_card_);
    lv_obj_remove_style_all(grabber);
    lv_obj_set_size(grabber, 48, 5);
    lv_obj_set_style_radius(grabber, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(grabber, Color(p.sub_text), 0);
    lv_obj_set_style_bg_opa(grabber, LV_OPA_60, 0);
    lv_obj_set_align(grabber, LV_ALIGN_TOP_MID);

    auto *header = lv_obj_create(info_card_);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, lv_pct(100), 40);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    auto *heading = lv_label_create(header);
    lv_obj_set_style_text_font(heading, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(heading, Color(p.text), 0);
    lv_label_set_text(heading, "Thông tin lời nhắc");
    MakeIconButton(header, 36, p.button, LV_SYMBOL_CLOSE, OnInfoClose, this,
                   p.text);

    auto *summary = lv_obj_create(info_card_);
    lv_obj_remove_style_all(summary);
    lv_obj_set_size(summary, lv_pct(100), 66);
    lv_obj_set_style_bg_color(summary, Color(p.row), 0);
    lv_obj_set_style_bg_opa(summary, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(summary, 16, 0);
    lv_obj_set_style_pad_hor(summary, 12, 0);
    lv_obj_set_style_pad_column(summary, 12, 0);
    lv_obj_set_flex_flow(summary, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(summary, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(summary, LV_OBJ_FLAG_SCROLLABLE);

    auto *color = lv_obj_create(summary);
    lv_obj_remove_style_all(color);
    lv_obj_set_size(color, 38, 38);
    lv_obj_set_style_radius(color, 12, 0);
    lv_obj_set_style_bg_color(color, Color(task->color), 0);
    lv_obj_set_style_bg_opa(color, LV_OPA_COVER, 0);

    auto *summary_copy = lv_obj_create(summary);
    lv_obj_remove_style_all(summary_copy);
    lv_obj_set_size(summary_copy, 1, lv_pct(100));
    lv_obj_set_flex_grow(summary_copy, 1);
    lv_obj_set_flex_flow(summary_copy, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(summary_copy, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(summary_copy, LV_OBJ_FLAG_SCROLLABLE);
    auto *name = lv_label_create(summary_copy);
    lv_obj_set_width(name, lv_pct(100));
    lv_obj_set_style_text_font(name, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(name, task->done ? Color(p.sub_text) : Color(p.text), 0);
    lv_obj_set_style_text_decor(name, task->done ? LV_TEXT_DECOR_STRIKETHROUGH
                                                 : LV_TEXT_DECOR_NONE, 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_label_set_text(name, task->title.c_str());
    auto *created = lv_label_create(summary_copy);
    lv_obj_set_style_text_font(created, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(created, Color(p.sub_text), 0);
    lv_label_set_text(created, task->info.c_str());

    auto *footer = lv_obj_create(info_card_);
    lv_obj_remove_style_all(footer);
    lv_obj_set_size(footer, lv_pct(100), 44);
    lv_obj_set_style_pad_column(footer, 8, 0);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(footer, LV_OBJ_FLAG_SCROLLABLE);

    auto make_badge = [&](const char *text, uint32_t bg, uint32_t fg) {
        auto *badge = lv_obj_create(footer);
        lv_obj_remove_style_all(badge);
        lv_obj_set_height(badge, 34);
        lv_obj_set_width(badge, LV_SIZE_CONTENT);
        lv_obj_set_style_radius(badge, 10, 0);
        lv_obj_set_style_bg_color(badge, Color(bg), 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(badge, 12, 0);
        lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
        auto *badge_label = lv_label_create(badge);
        lv_obj_set_style_text_font(badge_label, &BUILTIN_SMALL_TEXT_FONT, 0);
        lv_obj_set_style_text_color(badge_label, Color(fg), 0);
        lv_label_set_text(badge_label, text);
        lv_obj_center(badge_label);
    };
    make_badge(task->pinned ? "Đã ghim" : "Chưa ghim",
               task->pinned ? p.accent : p.button,
               task->pinned ? 0xffffff : p.sub_text);
    make_badge(task->done ? "Đã hoàn thành" : "Đang thực hiện",
               task->done ? 0x6bcb77 : p.button,
               task->done ? 0xffffff : p.sub_text);

    auto *spacer = lv_obj_create(footer);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_size(spacer, 1, 1);
    lv_obj_set_flex_grow(spacer, 1);
    MakeIconButton(footer, 38, 0xff6b6b, LV_SYMBOL_TRASH, OnDelete, this,
                   0xffffff);

    lv_anim_t slide;
    lv_anim_init(&slide);
    lv_anim_set_var(&slide, info_card_);
    lv_anim_set_values(&slide, sheet_h + 18, 0);
    lv_anim_set_time(&slide, 280);
    lv_anim_set_exec_cb(&slide, SetSheetTranslateY);
    lv_anim_set_path_cb(&slide, lv_anim_path_ease_out);
    lv_anim_start(&slide);
}

void RemindersView::CloseInfo() {
    if (info_overlay_) lv_obj_del(info_overlay_);
    info_overlay_ = nullptr;
    info_card_ = nullptr;
    info_task_id_ = 0;
}

void RemindersView::DeleteTask(int id) {
    tasks_.erase(std::remove_if(tasks_.begin(), tasks_.end(),
                                [id](const Task &task) { return task.id == id; }),
                 tasks_.end());
    Save();
    Render();
}

void RemindersView::OnAdd(lv_event_t *e) {
    LvglLockGuard lock;
    auto *self = static_cast<RemindersView *>(lv_event_get_user_data(e));
    self->AddTask();
}

void RemindersView::OnDone(lv_event_t *e) {
    LvglLockGuard lock;
    auto *ctx = static_cast<RowCtx *>(lv_event_get_user_data(e));
    ctx->self->ToggleDone(ctx->id);
}

void RemindersView::OnPin(lv_event_t *e) {
    LvglLockGuard lock;
    auto *ctx = static_cast<RowCtx *>(lv_event_get_user_data(e));
    ctx->self->TogglePinned(ctx->id);
}

void RemindersView::OnInfo(lv_event_t *e) {
    LvglLockGuard lock;
    auto *ctx = static_cast<RowCtx *>(lv_event_get_user_data(e));
    ctx->self->OpenInfo(ctx->id);
}

void RemindersView::OnDrag(lv_event_t *e) {
    const lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING &&
        code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
    auto *ctx = static_cast<RowCtx *>(lv_event_get_user_data(e));
    if (code == LV_EVENT_PRESSED) {
        ctx->drag_y = 0;
        ctx->drag_changed = false;
        lv_obj_set_style_opa(ctx->row, LV_OPA_80, 0);
        return;
    }
    if (code == LV_EVENT_PRESSING) {
        lv_indev_t *indev = lv_indev_active();
        if (!indev) return;
        lv_point_t vector{};
        lv_indev_get_vect(indev, &vector);
        ctx->drag_y += vector.y;
        lv_obj_set_style_translate_y(ctx->row,
                                     std::clamp(ctx->drag_y, -22, 22), 0);
        if (ctx->drag_y <= -30) {
            ctx->self->MoveTask(ctx, -1);
            ctx->drag_y = 0;
        } else if (ctx->drag_y >= 30) {
            ctx->self->MoveTask(ctx, 1);
            ctx->drag_y = 0;
        }
        return;
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lv_obj_set_style_translate_y(ctx->row, 0, 0);
        lv_obj_set_style_opa(ctx->row, LV_OPA_COVER, 0);
        if (ctx->drag_changed) ctx->self->Save();
    }
}

void RemindersView::OnInfoDismiss(lv_event_t *e) {
    LvglLockGuard lock;
    auto *self = static_cast<RemindersView *>(lv_event_get_user_data(e));
    if (lv_event_get_target(e) == self->info_overlay_) self->CloseInfo();
}

void RemindersView::OnInfoClose(lv_event_t *e) {
    LvglLockGuard lock;
    auto *self = static_cast<RemindersView *>(lv_event_get_user_data(e));
    self->CloseInfo();
}

void RemindersView::OnDelete(lv_event_t *e) {
    LvglLockGuard lock;
    auto *self = static_cast<RemindersView *>(lv_event_get_user_data(e));
    const int id = self->info_task_id_;
    self->CloseInfo();
    self->DeleteTask(id);
}

} // namespace home
