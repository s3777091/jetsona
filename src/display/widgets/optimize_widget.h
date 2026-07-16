#pragma once

/* Home-screen "Tối ưu" (optimize) widget for the DS-02 standby layer.
 *
 * A translucent pill showing live disk and RAM usage bars (statvfs("/") and
 * /proc/meminfo, refreshed by an lv_timer). The round button on the left
 * drops the kernel page/dentry caches (sync + /proc/sys/vm/drop_caches) on a
 * detached worker thread, then reports how much RAM was freed through the
 * notify callback (wired to the Dynamic Island toast).
 *
 * Threading follows StatusBar: the lv_timer and event callbacks run on the
 * LVGL handler thread without lv_lock, so they take jetson::ui::LvglLockGuard.
 * The widget is created once on the standby layer and lives for the whole
 * app lifetime, so the worker thread may safely capture `this`. */

#include <lvgl.h>

#include <atomic>
#include <cstdint>
#include <functional>

namespace home {

class OptimizeWidget {
public:
    using NotifyCb = std::function<void(const char *)>;

    explicit OptimizeWidget(lv_obj_t *parent);
    ~OptimizeWidget();

    void SetNotifyCb(NotifyCb cb) { notify_ = std::move(cb); }

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
    lv_obj_t *button_ = nullptr;
    lv_obj_t *disk_bar_ = nullptr;
    lv_obj_t *disk_label_ = nullptr;
    lv_obj_t *ram_bar_ = nullptr;
    lv_obj_t *ram_label_ = nullptr;
    lv_timer_t *timer_ = nullptr;
    std::atomic<bool> optimizing_{false};
    NotifyCb notify_;
};

} // namespace home
