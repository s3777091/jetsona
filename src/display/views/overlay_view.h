#pragma once

/* Base for full-screen overlay views (calendar / background gallery / settings)
 * sharing the same shell: an overlay covering the screen, a compact header with
 * a macOS-style traffic-light cluster below the global system-status row, and a
 * body container that fills the rest. SetStatus() exists for worker-thread progress
 * but no longer renders anything on screen -- it only logs.
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

    /* Multitasking: the yellow (-) and green (+) lights both send the app to
     * the background instead of drawing a restore pill over the dock. The home
     * screen wires this to its task queue (snapshot + hide + dock dot). */
    void SetBackgroundRequest(std::function<void()> cb) { background_request_ = std::move(cb); }
    // Hide/show the whole overlay without destroying it (kept alive in the
    // multitask queue, so restoring never rebuilds the view).
    void SetHidden(bool hidden);
    lv_obj_t *overlay_obj() const { return overlay_; }
    const std::string &title() const { return title_; }

protected:
    // The global StatusBar occupies y=0..41 on the top layer. Keeping the app
    // controls in the lower part of this header prevents the traffic lights and
    // title from being painted underneath the clock/date/status clusters.
    static constexpr int kHeaderHeight = 72;

    int width_ = 0;
    int height_ = 0;
    lv_obj_t *parent_ = nullptr;
    lv_obj_t *overlay_ = nullptr;
    lv_obj_t *header_ = nullptr;
    lv_obj_t *close_btn_ = nullptr;   // macOS traffic-light: red (close)
    lv_obj_t *min_btn_ = nullptr;     // yellow (minimize)
    lv_obj_t *zoom_btn_ = nullptr;    // green (zoom / windowed)
    lv_obj_t *title_label_ = nullptr;
    lv_obj_t *status_label_ = nullptr;
    lv_obj_t *body_ = nullptr;        // subclass fills this (below header+status)
    lv_obj_t *right_btn_ = nullptr;
    std::string title_;

    virtual void OnStart() {}
    // Called after the overlay is resized by the green zoom button so subclasses
    // can reflow their content into the new body width/height (default: no-op).
    virtual void OnResize(int /*w*/, int /*h*/) {}

private:
    void BuildShell(const char *title);
    void ToBackground();

    static void OnCloseBtn(lv_event_t *e);
    static void OnMinBtn(lv_event_t *e);
    static void OnZoomBtn(lv_event_t *e);
    static void OnRight(lv_event_t *e);
    static void OnCloseTimer(lv_timer_t *t);

    ClosedCb on_closed_;
    RightCb right_cb_;
    std::function<void()> background_request_;
    std::atomic<bool> closed_{false};
};

} // namespace home
