#pragma once

/* Base for full-screen overlay views (calendar / background gallery / settings)
 * sharing the same shell: an overlay covering the screen, a 48px header with a
 * back button + centered title (+ optional right button), a status line, and a
 * body container the subclass fills.
 *
 * Threading + lifetime mirror WifiSettingsView: the subclass is shared_ptr-owned
 * so worker threads (scans, etc.) can outlive the on-screen overlay; the back
 * button hides the overlay and closes via a deferred one-shot lv_timer so we
 * never destroy `*this` from inside one of its own callbacks. Subclasses use
 * shared_from_this() only from Start() onward (never from the constructor).
 *
 * Colors come from jetson::UiTheme (read at build time). */

#include <lvgl.h>
#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace home {

class OverlayView : public std::enable_shared_from_this<OverlayView> {
public:
    using ClosedCb = std::function<void()>;
    using RightCb = std::function<void(OverlayView *)>;

    OverlayView(lv_obj_t *parent, int width, int height, const char *title, ClosedCb on_closed);
    virtual ~OverlayView();

    void Start();          // calls OnStart() — invoke after make_shared.
    void RequestClose();
    void SetStatus(const char *text);

    // Optional right-header button (e.g. a toggle). Pass a symbol + callback.
    void SetRightButton(const char *icon_symbol, RightCb cb);

protected:
    int width_ = 0;
    int height_ = 0;
    lv_obj_t *parent_ = nullptr;
    lv_obj_t *overlay_ = nullptr;
    lv_obj_t *header_ = nullptr;
    lv_obj_t *back_btn_ = nullptr;
    lv_obj_t *title_label_ = nullptr;
    lv_obj_t *status_label_ = nullptr;
    lv_obj_t *body_ = nullptr;        // subclass fills this (below header+status)
    lv_obj_t *right_btn_ = nullptr;

    virtual void OnStart() {}

private:
    void BuildShell(const char *title);

    static void OnBack(lv_event_t *e);
    static void OnRight(lv_event_t *e);
    static void OnCloseTimer(lv_timer_t *t);

    ClosedCb on_closed_;
    RightCb right_cb_;
    std::atomic<bool> closed_{false};
};

} // namespace home