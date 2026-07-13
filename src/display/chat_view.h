#pragma once

#include "overlay_view.h"
#include "conversation.h"

#include <lvgl.h>
#include <memory>
#include <string>

namespace home {

/* Full-screen chat overlay: a scrollable message list (user bubbles right,
 * assistant bubbles left), a one-line text input + Send button, and an
 * on-screen keyboard for the touch panel.
 *
 * The view holds a shared_ptr to a jetson::Conversation so chat history
 * persists across open/close. Send() runs the LLM call on a worker thread;
 * the reply is marshaled back via Application::Schedule + a weak_ptr to this
 * view, so a reply that arrives after the user closed the chat is dropped
 * safely (the Conversation keeps the history, the view just doesn't render it).
 *
 * Threading: AppendMessage() takes lv_lock so it is safe to call from the
 * worker-marshaled task. Event callbacks run under lv_lock already. */
class ChatView : public OverlayView {
public:
    ChatView(lv_obj_t *parent, int width, int height,
             std::shared_ptr<jetson::Conversation> conv, ClosedCb on_closed);

    // Thread-safe: appends a bubble and scrolls to bottom. role: "user"|"assistant"|"system".
    void AppendMessage(const std::string &role, const std::string &content);

protected:
    void OnStart() override;

private:
    std::shared_ptr<jetson::Conversation> conv_;

    lv_obj_t *list_ = nullptr;        // scrollable message column
    lv_obj_t *input_ = nullptr;       // lv_textarea (one line)
    lv_obj_t *send_btn_ = nullptr;
    lv_obj_t *keyboard_ = nullptr;

    void DoSend();
    void SetBusy(bool busy);
    void AddBubble(const std::string &role, const std::string &content); // expects lv_lock held

    static void OnSendClicked(lv_event_t *e);
    static void OnInputReady(lv_event_t *e);
    static void OnInputFocused(lv_event_t *e);
};

} // namespace home