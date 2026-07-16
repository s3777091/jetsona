#pragma once

/* iPhone-style app switcher for the DS-02 home screen (800x480).
 *
 * A full-screen dim layer with a horizontally scrollable row of cards, one per
 * running app (most recent first). Each card shows the app icon + title above
 * a live thumbnail (lv_snapshot draw buffer taken when the app last went to
 * the background) and carries a small "x" badge that kills the app. Tapping a
 * card switches to that app; tapping the dim background dismisses.
 *
 * The whole switcher blooms out of the Dynamic Island: the content container's
 * transform pivot sits at the island's screen position and its scale animates
 * 10% -> 100% with an ease-out path (ease-in on dismiss), so opening/closing
 * visually originates from the island.
 *
 * Lifetime: owned by Ds02HomeDisplay. Dismissing runs the close animation,
 * deletes the LVGL subtree, then reports on_dismissed via a one-shot lv_timer
 * so the owner can safely reset its unique_ptr (never from inside our own
 * callback stack). Snapshot/icon buffers are borrowed from the owner and must
 * outlive the switcher. */

#include <lvgl.h>

#include <functional>
#include <string>
#include <vector>

namespace home {

class AppSwitcher {
public:
    struct Card {
        int app_id = 0;
        std::string title;
        const void *snapshot = nullptr;  // lv_snapshot draw buf (may be null)
        const void *icon_src = nullptr;  // dock/drawer icon dsc (may be null)
        uint16_t icon_scale = 256;       // lv_image zoom for a ~22 px icon
    };
    using AppCb = std::function<void(int app_id)>;
    using VoidCb = std::function<void()>;

    AppSwitcher(lv_obj_t *parent, int width, int height,
                std::vector<Card> cards, int island_cx, int island_cy,
                AppCb on_activate, AppCb on_kill, VoidCb on_dismissed);
    ~AppSwitcher();

    void Dismiss(); // animated close (safe to call repeatedly)

private:
    struct CardCtx {
        AppSwitcher *self;
        int app_id;
    };

    void BuildCard(lv_obj_t *row, const Card &card);
    void AnimateOpen();

    static void OnScaleAnim(void *var, int32_t v);
    static void OnOpaAnim(void *var, int32_t v);
    static void OnBgOpaAnim(void *var, int32_t v);
    static void OnDismissDone(lv_anim_t *a);
    static void OnDismissTimer(lv_timer_t *t);
    static void OnBackdropClicked(lv_event_t *e);
    static void OnCardClicked(lv_event_t *e);
    static void OnKillClicked(lv_event_t *e);
    static void OnCardCtxDeleted(lv_event_t *e);

    int width_ = 0;
    int height_ = 0;
    lv_obj_t *layer_ = nullptr;   // dim backdrop, click = dismiss
    lv_obj_t *content_ = nullptr; // scaled container (pivot at the island)
    lv_obj_t *row_ = nullptr;     // horizontal scroll row of cards
    size_t card_count_ = 0;
    bool closing_ = false;

    AppCb on_activate_, on_kill_;
    VoidCb on_dismissed_;
};

} // namespace home
