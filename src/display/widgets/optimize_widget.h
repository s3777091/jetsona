#pragma once

/* Status-bar cache cleaner + disk/RAM Dynamic Island surface.
 *
 * A compact island view showing live disk and RAM usage bars (statvfs("/") and
 * /proc/meminfo, refreshed by an lv_timer). The status icon drops the kernel
 * page/dentry caches (sync + /proc/sys/vm/drop_caches) on a
 * worker thread, then reports how much RAM was freed through the
 * notify callback (wired to the Dynamic Island toast).
 *
 * Threading follows StatusBar: the lv_timer and event callbacks run on the
 * LVGL handler thread without lv_lock, so they take jetson::ui::LvglLockGuard.
 * The worker is joined during destruction before any LVGL objects disappear. */

#include <lvgl.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <utility>

namespace home {

class OptimizeWidget {
public:
    using NotifyCb = std::function<void(const char *)>;
    using OpenCb = std::function<void(lv_obj_t *content, lv_obj_t *icon)>;

    OptimizeWidget(lv_obj_t *icon_parent, lv_obj_t *popup_parent);
    ~OptimizeWidget();

    void SetNotifyCb(NotifyCb cb) { notify_ = std::move(cb); }
    void SetBeforeOpenCb(OpenCb cb) { before_open_ = std::move(cb); }
    void HidePopup();
    lv_obj_t *Content() const { return root_; }

private:
    struct Stats {
        uint64_t mem_total_kb = 0;
        uint64_t mem_avail_kb = 0;
        uint64_t disk_total_kb = 0;
        uint64_t disk_used_kb = 0;
    };

    static Stats ReadStats();
    void Apply(const Stats &s);
    void StartOptimize();

    static void OnTimer(lv_timer_t *t);
    static void OnOptimizeClicked(lv_event_t *e);

    lv_obj_t *root_ = nullptr;
    lv_obj_t *button_ = nullptr; // 20 px clean icon in the status row
    lv_obj_t *disk_bar_ = nullptr;
    lv_obj_t *disk_label_ = nullptr;
    lv_obj_t *ram_bar_ = nullptr;
    lv_obj_t *ram_label_ = nullptr;
    lv_obj_t *status_label_ = nullptr;
    lv_timer_t *timer_ = nullptr;
    std::atomic<bool> optimizing_{false};
    std::thread optimize_thread_;
    NotifyCb notify_;
    OpenCb before_open_;
};

} // namespace home
