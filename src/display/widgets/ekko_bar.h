#pragma once

/* The Ekko composer -- a chat strip that sits just above the dock and blooms
 * in and out with the Dynamic Island orbit.
 *
 * The device has no microphone yet, so this is how the agent is actually
 * driven: the user types, Conversation runs the tool loop on its worker
 * thread, and the reply lands back here. It deliberately shows only the last
 * few turns; the full transcript still lives in ChatView (dock -> Ekko app),
 * which shares the same Conversation object and therefore the same history.
 *
 * Lives on lv_layer_top() so it stays reachable while an app overlay is open,
 * matching the island it belongs to. Timing (360 ms, ease-in-out) is copied
 * from the quick-menu bloom so the strip and the island read as one motion.
 *
 * Threading: Conversation invokes its callbacks on a worker thread. Those
 * callbacks marshal through Application::Schedule and then take lv_lock before
 * touching anything here. A shared "alive" token guards against the bar being
 * destroyed while a reply is still in flight. */

#include <lvgl.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace jetson {
class Conversation;
}

namespace home {

class EkkoBar {
public:
    /* `bottom_margin` is the gap from the bottom of the screen to the bar's
     * lower edge -- callers pass the dock's height plus its own margin so the
     * strip lands directly above the dock. */
    EkkoBar(lv_obj_t *parent, int screen_width, int bottom_margin,
            std::shared_ptr<jetson::Conversation> conv);
    ~EkkoBar();

    EkkoBar(const EkkoBar &) = delete;
    EkkoBar &operator=(const EkkoBar &) = delete;

    void Show();
    void Hide();
    bool visible() const { return visible_; }

    // Tint the border/send button to the active orbit palette's accent.
    void SetAccent(uint32_t accent);

private:
    void BuildUi(lv_obj_t *parent);
    void InstallToolEventHook();
    void DoSend();
    void AddBubble(const std::string &role, const std::string &text);
    void SetStatus(const std::string &text);
    void SetBusy(bool busy);
    void TrimBubbles();

    static void OnSendClicked(lv_event_t *e);
    static void OnInputReady(lv_event_t *e);
    static void OnShowAnim(void *var, int32_t v);
    static void OnHideDone(lv_anim_t *a);

    std::shared_ptr<jetson::Conversation> conv_;
    // Set to false in the destructor; queued reply callbacks check it before
    // dereferencing `this`.
    std::shared_ptr<std::atomic<bool>> alive_;

    lv_obj_t *root_ = nullptr;
    lv_obj_t *list_ = nullptr;
    lv_obj_t *status_ = nullptr;
    lv_obj_t *input_ = nullptr;
    lv_obj_t *send_btn_ = nullptr;

    int width_ = 0;
    int bottom_margin_ = 0;
    uint32_t accent_ = 0xffb24d;
    bool visible_ = false;
};

} // namespace home
