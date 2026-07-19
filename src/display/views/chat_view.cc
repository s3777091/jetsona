#include "display/views/chat_view.h"
#include "display/common/lvgl_utils.h"
#include "fonts.h"
#include "display/theme/ui_theme.h"
#include "application.h"
#include "lvgl_runtime.h"
#include "esp_log.h"

#include <lvgl.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#define TAG "ChatView"

namespace home {

using jetson::ui::Color;
using jetson::ui::LvglLockGuard;

ChatView::ChatView(lv_obj_t *parent, int width, int height,
                   std::shared_ptr<jetson::Conversation> conv, ClosedCb on_closed)
    : OverlayView(parent, width, height, "Tro chuyen", std::move(on_closed)),
      conv_(std::move(conv)) {
    // body_ is created by OverlayView::BuildShell; fill it here.
    const auto &p = jetson::UiTheme::Instance().Palette();

    lv_obj_set_flex_flow(body_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body_, 8, 0);
    lv_obj_set_style_pad_row(body_, 6, 0);
    lv_obj_clear_flag(body_, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Message list (scrollable) ----
    list_ = lv_obj_create(body_);
    lv_obj_remove_style_all(list_);
    lv_obj_set_flex_grow(list_, 1);
    lv_obj_set_width(list_, lv_pct(100));
    lv_obj_set_flex_flow(list_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list_, 8, 0);
    lv_obj_set_style_pad_all(list_, 4, 0);
    lv_obj_add_flag(list_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_, LV_SCROLLBAR_MODE_AUTO);

    // ---- Input row (textarea + send) ----
    auto *input_row = lv_obj_create(body_);
    lv_obj_remove_style_all(input_row);
    lv_obj_set_width(input_row, lv_pct(100));
    lv_obj_set_height(input_row, 52);
    lv_obj_set_style_pad_all(input_row, 0, 0);
    lv_obj_set_style_pad_column(input_row, 6, 0);
    lv_obj_set_flex_flow(input_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(input_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(input_row, LV_OBJ_FLAG_SCROLLABLE);

    input_ = lv_textarea_create(input_row);
    lv_obj_set_flex_grow(input_, 1);
    lv_obj_set_height(input_, 48);
    lv_obj_set_style_text_font(input_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(input_, Color(p.text), 0);
    lv_obj_set_style_bg_color(input_, Color(p.row), 0);
    lv_obj_set_style_bg_opa(input_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(input_, Color(p.border), 0);
    lv_obj_set_style_border_width(input_, 1, 0);
    lv_obj_set_style_radius(input_, 10, 0);
    lv_textarea_set_placeholder_text(input_, "Nhap tin nhan...");
    lv_textarea_set_one_line(input_, true);
    lv_textarea_set_max_length(input_, 500);
    lv_obj_add_event_cb(input_, OnInputReady, LV_EVENT_READY, this);
    lv_obj_add_event_cb(input_, OnInputFocused, LV_EVENT_FOCUSED, this);
    /* Let the USB keyboard type into this textarea via the keypad group. */
    if (auto *g = jetson::LvglRuntime::Instance().keypad_group()) lv_group_add_obj(g, input_);

    send_btn_ = lv_button_create(input_row);
    lv_obj_set_size(send_btn_, 80, 48);
    lv_obj_set_style_bg_color(send_btn_, Color(p.accent), 0);
    lv_obj_set_style_radius(send_btn_, 10, 0);
    auto *send_lbl = lv_label_create(send_btn_);
    lv_obj_set_style_text_font(send_lbl, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(send_lbl, lv_color_white(), 0);
    lv_label_set_text(send_lbl, "Gui");
    lv_obj_center(send_lbl);
    lv_obj_add_event_cb(send_btn_, OnSendClicked, LV_EVENT_CLICKED, this);

    // ---- On-screen keyboard ----
    keyboard_ = lv_keyboard_create(body_);
    lv_obj_set_width(keyboard_, lv_pct(100));
    lv_obj_set_height(keyboard_, 180);
    lv_keyboard_set_textarea(keyboard_, input_);
    // lv_keyboard uses LV_FONT_DEFAULT (montserrat_14) which carries the LVGL
    // symbol glyphs for the backspace/enter buttons.
    lv_obj_set_style_bg_color(keyboard_, Color(p.panel), 0);
    // Hide keyboard until the input is focused.
    lv_obj_add_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
}

void ChatView::OnStart() {
    SetStatus("Nhap cau hoi va nhan Gui");
    // Replay any existing history (e.g. chat reopened mid-conversation).
    if (conv_) {
        for (const auto &m : conv_->History()) {
            if (m.role == "system") continue;
            AddBubble(m.role, m.content);
        }
    }
    if (conv_ && conv_->busy()) SetBusy(true);

    // Surface tool activity in the status line (fires on the worker thread ->
    // marshal to the main loop, then lock LVGL).
    if (conv_) {
        std::weak_ptr<ChatView> weak = std::static_pointer_cast<ChatView>(shared_from_this());
        conv_->SetOnToolEvent([weak](std::string name, std::string status) {
            Application::GetInstance().Schedule([weak, name = std::move(name),
                                                 status = std::move(status)]() {
                auto sp = weak.lock();
                if (!sp) return;
                LvglLockGuard lock;
                if (status == "start") {
                    sp->SetStatus((std::string("Dang go tool: ") + name + "...").c_str());
                }
            });
        });
    }
}

void ChatView::AddBubble(const std::string &role, const std::string &content) {
    // Caller must hold lv_lock.
    const auto &p = jetson::UiTheme::Instance().Palette();
    bool is_user = (role == "user");

    auto *row = lv_obj_create(list_);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
                          is_user ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    auto *bubble = lv_label_create(row);
    lv_obj_set_style_text_font(bubble, &BUILTIN_TEXT_FONT, 0);
    lv_label_set_long_mode(bubble, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(bubble, width_ * 3 / 4); // wrap at 75% width
    lv_label_set_text(bubble, content.c_str());
    lv_obj_set_style_text_color(bubble, is_user ? lv_color_white() : Color(p.text), 0);
    lv_obj_set_style_bg_color(bubble, is_user ? Color(p.accent) : Color(p.row), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bubble, 12, 0);
    lv_obj_set_style_pad_all(bubble, 10, 0);
    lv_obj_set_style_border_width(bubble, 0, 0);

    lv_obj_scroll_to_view_recursive(row, LV_ANIM_OFF);
}

void ChatView::AppendMessage(const std::string &role, const std::string &content) {
    LvglLockGuard lock;
    AddBubble(role, content);
}

void ChatView::SetBusy(bool busy) {
    LvglLockGuard lock;
    if (send_btn_) {
        if (busy) lv_obj_add_state(send_btn_, LV_STATE_DISABLED);
        else      lv_obj_clear_state(send_btn_, LV_STATE_DISABLED);
    }
    SetStatus(busy ? "Dang tra loi..." : "Nhap cau hoi va nhan Gui");
}

void ChatView::DoSend() {
    if (!conv_) return;
    std::string text;
    {
        LvglLockGuard lock;
        if (conv_->busy()) return;
        const char *txt = lv_textarea_get_text(input_);
        text = txt ? txt : "";
        if (text.empty()) return;
        lv_textarea_set_text(input_, "");
        AddBubble("user", text);
    }
    // Lock released here — the network call runs off the UI thread.

    auto self = std::static_pointer_cast<ChatView>(shared_from_this());
    std::weak_ptr<ChatView> weak = self;
    SetBusy(true);

    conv_->Send(text, [weak](std::string reply, std::string err) {
        // On worker thread. Marshal to the main loop, then lock LVGL.
        Application::GetInstance().Schedule([weak, reply = std::move(reply),
                                             err = std::move(err)]() {
            auto sp = weak.lock();
            if (!sp) return; // view closed -> drop
            if (!err.empty()) {
                sp->AppendMessage("assistant", std::string("Loi: ") + err);
            } else if (!reply.empty()) {
                sp->AppendMessage("assistant", reply);
            }
            sp->SetBusy(false);
        });
    });
}

void ChatView::OnSendClicked(lv_event_t *e) {
    auto *self = static_cast<ChatView *>(lv_event_get_user_data(e));
    self->DoSend();
}

void ChatView::OnInputReady(lv_event_t *e) {
    // Enter pressed on the one-line textarea -> send.
    auto *self = static_cast<ChatView *>(lv_event_get_user_data(e));
    self->DoSend();
}

void ChatView::OnInputFocused(lv_event_t *e) {
    auto *self = static_cast<ChatView *>(lv_event_get_user_data(e));
    LvglLockGuard lock;
    if (self->input_) lv_group_focus_obj(self->input_);
    if (self->keyboard_) lv_obj_clear_flag(self->keyboard_, LV_OBJ_FLAG_HIDDEN);
}

} // namespace home
