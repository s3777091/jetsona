#include "display/widgets/ekko_bar.h"

#include "agent/conversation.h"
#include "application.h"
#include "display/common/lvgl_utils.h"
#include "display/theme/ui_theme.h"
#include "esp_log.h"
#include "fonts.h"
#include "lvgl_runtime.h"

#include <algorithm>
#include <string>
#include <utility>

#define TAG "EkkoBar"

namespace home {

using jetson::ui::Color;
using jetson::ui::LvglLockGuard;

namespace {
// Matched to StatusBar's island bloom so the transcript and pill move together.
constexpr uint32_t kBloomMs = 360;
} // namespace

EkkoBar::EkkoBar(lv_obj_t *parent,
                 std::shared_ptr<jetson::Conversation> conv)
    : conv_(std::move(conv)),
      alive_(std::make_shared<std::atomic<bool>>(true)) {
    BuildUi(parent);
}

EkkoBar::~EkkoBar() {
    alive_->store(false);
    if (conv_) conv_->SetOnToolEvent(nullptr);
    if (root_) {
        lv_anim_delete(this, OnShowAnim);
        lv_obj_delete(root_);
        root_ = nullptr;
    }
}

void EkkoBar::BuildUi(lv_obj_t *parent) {
    const auto &p = jetson::UiTheme::Instance().Palette();

    root_ = lv_obj_create(parent);
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(root_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_set_style_pad_row(root_, 6, 0);
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    // Hidden until the orbit blooms; opacity is what the show/hide anim drives.
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(root_, LV_OPA_0, 0);

    // ---- Full transcript (the only vertically scrolling region) ----
    list_ = lv_obj_create(root_);
    lv_obj_remove_style_all(list_);
    lv_obj_set_width(list_, lv_pct(100));
    lv_obj_set_flex_grow(list_, 1);
    lv_obj_set_flex_flow(list_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(list_, Color(0x111722), 0);
    lv_obj_set_style_bg_opa(list_, LV_OPA_80, 0);
    lv_obj_set_style_radius(list_, 16, 0);
    lv_obj_set_style_pad_all(list_, 10, 0);
    lv_obj_set_style_pad_row(list_, 8, 0);
    lv_obj_add_flag(list_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(list_, Color(accent_), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list_, LV_OPA_60, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(list_, 4, LV_PART_SCROLLBAR);

    // ---- Tool/status line ----
    status_ = lv_label_create(root_);
    lv_obj_set_style_text_font(status_, jetson::BuiltinTextFaceAt(13), 0);
    lv_obj_set_style_text_color(status_, Color(p.sub_text), 0);
    lv_label_set_long_mode(status_, LV_LABEL_LONG_DOT);
    lv_obj_set_width(status_, lv_pct(100));
    lv_label_set_text(status_, "Hỏi Ekko bất cứ điều gì…");

    // ---- Composer ----
    auto *row = lv_obj_create(root_);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 44);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    input_ = lv_textarea_create(row);
    lv_obj_set_flex_grow(input_, 1);
    lv_obj_set_height(input_, 42);
    lv_obj_set_style_text_font(input_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(input_, lv_color_white(), 0);
    lv_obj_set_style_bg_color(input_, Color(0x2b2d33), 0);
    lv_obj_set_style_bg_opa(input_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(input_, 0, 0);
    lv_obj_set_style_radius(input_, 12, 0);
    lv_obj_set_style_pad_all(input_, 10, 0);
    lv_textarea_set_placeholder_text(input_, "Nhắn cho Ekko…");
    lv_textarea_set_one_line(input_, true);
    lv_textarea_set_max_length(input_, 500);
    lv_obj_add_event_cb(input_, OnInputReady, LV_EVENT_READY, this);

    send_btn_ = lv_button_create(row);
    lv_obj_set_size(send_btn_, 68, 42);
    lv_obj_set_style_bg_color(send_btn_, Color(accent_), 0);
    lv_obj_set_style_radius(send_btn_, 12, 0);
    lv_obj_set_style_shadow_width(send_btn_, 0, 0);
    auto *send_lbl = lv_label_create(send_btn_);
    lv_obj_set_style_text_font(send_lbl, jetson::BuiltinTextFaceAt(14), 0);
    lv_obj_set_style_text_color(send_lbl, lv_color_black(), 0);
    lv_label_set_text(send_lbl, "Gửi");
    lv_obj_center(send_lbl);
    lv_obj_add_event_cb(send_btn_, OnSendClicked, LV_EVENT_CLICKED, this);

    InstallToolEventHook();
}

void EkkoBar::InstallToolEventHook() {
    /* Tool progress arrives on the worker thread. Marshal to the main loop and
     * re-check `alive` before touching LVGL -- the bar may be gone by then.
     *
     * Conversation holds a single tool-event callback, and ChatView installs
     * its own when the Ekko app opens. Re-installing on every Show() takes the
     * hook back so the island's status line does not go silent after a visit
     * to the legacy full-screen app. */
    if (!conv_) return;
    auto alive = alive_;
    EkkoBar *self = this;
    conv_->SetOnToolEvent([alive, self](std::string name, std::string status) {
        if (!alive->load()) return;
        Application::GetInstance().Schedule([alive, self, name, status]() {
            if (!alive->load()) return;
            LvglLockGuard lock;
            if (status == "start") self->SetStatus("Đang dùng công cụ: " + name + "…");
        });
    });
}

void EkkoBar::SetAccent(uint32_t accent) {
    accent_ = accent;
    if (send_btn_) lv_obj_set_style_bg_color(send_btn_, Color(accent_), 0);
    if (list_) lv_obj_set_style_bg_color(list_, Color(accent_), LV_PART_SCROLLBAR);
}

// ---- show / hide ---------------------------------------------------------

void EkkoBar::OnShowAnim(void *var, int32_t v) {
    auto *self = static_cast<EkkoBar *>(var);
    if (!self->root_) return;
    lv_obj_set_style_opa(self->root_, (lv_opa_t)v, 0);
}

void EkkoBar::OnHideDone(lv_anim_t *a) {
    auto *self = static_cast<EkkoBar *>(lv_anim_get_user_data(a));
    if (!self || !self->root_) return;
    // A Show() that raced this animation already made the bar visible again.
    if (self->visible_) return;
    lv_obj_add_flag(self->root_, LV_OBJ_FLAG_HIDDEN);
}

void EkkoBar::Show() {
    if (!root_ || visible_) return;
    visible_ = true;

    lv_obj_clear_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(root_);
    RebuildHistory();
    InstallToolEventHook();
    if (conv_ && conv_->busy()) SetBusy(true);

    lv_anim_delete(this, OnShowAnim);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, this);
    lv_anim_set_exec_cb(&a, OnShowAnim);
    lv_anim_set_values(&a, lv_obj_get_style_opa(root_, 0), LV_OPA_COVER);
    lv_anim_set_time(&a, kBloomMs);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);

    /* The USB keyboard types into whatever the keypad group has focused, so
     * joining the group here (and leaving on hide) is what makes the composer
     * usable without a touchscreen. Focus is set explicitly rather than from a
     * FOCUSED handler -- see the recursion note in chat_view.cc. */
    if (auto *g = jetson::LvglRuntime::Instance().keypad_group()) {
        lv_group_add_obj(g, input_);
        lv_group_focus_obj(input_);
        lv_group_set_editing(g, true);
    }
}

void EkkoBar::Hide() {
    if (!root_ || !visible_) return;
    visible_ = false;

    if (auto *g = jetson::LvglRuntime::Instance().keypad_group()) {
        lv_group_set_editing(g, false);
        lv_group_remove_obj(input_);
    }

    lv_anim_delete(this, OnShowAnim);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, this);
    lv_anim_set_exec_cb(&a, OnShowAnim);
    lv_anim_set_values(&a, lv_obj_get_style_opa(root_, 0), LV_OPA_0);
    lv_anim_set_time(&a, kBloomMs);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_user_data(&a, this);
    lv_anim_set_completed_cb(&a, OnHideDone);
    lv_anim_start(&a);
}

// ---- transcript ----------------------------------------------------------

void EkkoBar::RebuildHistory() {
    if (!list_) return;
    lv_obj_clean(list_);
    if (!conv_) return;
    for (const auto &message : conv_->History()) {
        if ((message.role != "user" && message.role != "assistant") ||
            message.content.empty()) {
            continue;
        }
        AddBubble(message.role, message.content);
    }
}

void EkkoBar::AddBubble(const std::string &role, const std::string &text) {
    // Caller holds lv_lock.
    if (!list_) return;
    const bool is_user = (role == "user");

    auto *row = lv_obj_create(list_);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, is_user ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    auto *bubble = lv_label_create(row);
    lv_obj_set_style_text_font(bubble, jetson::BuiltinTextFaceAt(14), 0);
    lv_label_set_long_mode(bubble, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(bubble, lv_pct(82));
    lv_label_set_text(bubble, text.c_str());
    lv_obj_set_style_text_color(bubble, is_user ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_style_bg_color(bubble, is_user ? Color(accent_) : Color(0x2b2d33), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bubble, 12, 0);
    lv_obj_set_style_pad_all(bubble, 8, 0);

    lv_obj_scroll_to_view_recursive(row, LV_ANIM_OFF);
}

void EkkoBar::SetStatus(const std::string &text) {
    if (status_) lv_label_set_text(status_, text.c_str());
}

void EkkoBar::SetBusy(bool busy) {
    if (send_btn_) {
        if (busy) lv_obj_add_state(send_btn_, LV_STATE_DISABLED);
        else lv_obj_clear_state(send_btn_, LV_STATE_DISABLED);
    }
    if (busy) SetStatus("Ekko đang nghĩ…");
}

// ---- send ----------------------------------------------------------------

void EkkoBar::DoSend() {
    if (!conv_ || !input_) return;

    std::string text;
    {
        LvglLockGuard lock;
        if (conv_->busy()) return;
        const char *raw = lv_textarea_get_text(input_);
        text = raw ? raw : "";
        if (text.find_first_not_of(" \t\r\n") == std::string::npos) return;
        lv_textarea_set_text(input_, "");
        AddBubble("user", text);
        SetBusy(true);
    }
    // Lock released before the request: the agent loop is a network call.

    auto alive = alive_;
    EkkoBar *self = this;
    conv_->Send(text, [alive, self](std::string reply, std::string err) {
        if (!alive->load()) return;
        Application::GetInstance().Schedule([alive, self, reply, err]() {
            if (!alive->load()) return;
            LvglLockGuard lock;
            if (!err.empty()) {
                self->AddBubble("assistant", "Lỗi: " + err);
                self->SetStatus("Gặp lỗi — thử lại hoặc kiểm tra .env");
            } else {
                self->AddBubble("assistant", reply.empty() ? "(không có nội dung)" : reply);
                self->SetStatus("Hỏi Ekko bất cứ điều gì…");
            }
            self->SetBusy(false);
        });
    });
}

void EkkoBar::OnSendClicked(lv_event_t *e) {
    static_cast<EkkoBar *>(lv_event_get_user_data(e))->DoSend();
}

void EkkoBar::OnInputReady(lv_event_t *e) {
    // Enter on the one-line textarea sends, same as the Ekko app.
    static_cast<EkkoBar *>(lv_event_get_user_data(e))->DoSend();
}

} // namespace home
