#include "display/widgets/optimize_widget.h"
#include "display/common/lvgl_utils.h"
#include "display/core/app_icons.h"
#include "platform/shell_command.h"
#include "fonts.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <sys/statvfs.h>
#include <thread>
#include <unistd.h>

namespace home {

using jetson::ui::Color;
using jetson::ui::LvglLockGuard;

namespace {

constexpr uint32_t kAccentBlue = 0x0a84ff;  // RAM bar + optimize button
constexpr uint32_t kAccentCyan = 0x00c3d7;  // disk bar
constexpr uint32_t kTextLight = 0xf4f7fb;
constexpr uint32_t kRefreshMs = 3000;

// "155,1" — Vietnamese decimal comma. Whole totals print without decimals.
void FormatGb(char *buf, size_t n, double gb, bool decimals) {
    std::snprintf(buf, n, decimals ? "%.1f" : "%.0f", gb);
    for (char *p = buf; *p; ++p)
        if (*p == '.') *p = ',';
}

void SetUsageText(lv_obj_t *label, uint64_t used_kb, uint64_t total_kb) {
    char used[16], total[16], text[48];
    FormatGb(used, sizeof(used), used_kb / 1048576.0, true);
    FormatGb(total, sizeof(total), total_kb / 1048576.0, false);
    std::snprintf(text, sizeof(text), "%s GB / %s GB", used, total);
    lv_label_set_text(label, text);
}

// One compact pill-shaped usage bar (0..1000 range) with a right-aligned
// "used / total" caption drawn on top of the indicator.
lv_obj_t *MakeUsageBar(lv_obj_t *parent, uint32_t accent, lv_obj_t **label_out) {
    auto *bar = lv_bar_create(parent);
    lv_obj_set_size(bar, 262, 22);
    lv_bar_set_range(bar, 0, 1000);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(bar, 11, 0);
    lv_obj_set_style_bg_color(bar, Color(0x202938), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 11, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, Color(accent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_clear_flag(bar, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    auto *label = lv_label_create(bar);
    lv_obj_set_style_text_font(label, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(label, Color(kTextLight), 0);
    lv_label_set_text(label, "-- GB / -- GB");
    lv_obj_align(label, LV_ALIGN_RIGHT_MID, -8, 0);
    *label_out = label;
    return bar;
}

} // namespace

OptimizeWidget::OptimizeWidget(lv_obj_t *icon_parent, lv_obj_t *popup_parent) {
    if (!icon_parent) return;
    if (!popup_parent) popup_parent = lv_layer_top();

    // The action itself participates in StatusBar's flex row. The usage bars
    // are parented to the shared Dynamic Island content host.
    button_ = jetson::ui::CreateAppIcon(icon_parent, "clean-cache", 20);
    lv_obj_set_style_image_recolor(button_, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(button_, LV_OPA_COVER, 0);
    lv_obj_add_flag(button_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(button_, 8);
    lv_obj_add_event_cb(button_, OnOptimizeClicked, LV_EVENT_CLICKED, this);

    root_ = lv_obj_create(popup_parent);
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, 300, 104);
    lv_obj_center(root_);
    lv_obj_set_style_bg_opa(root_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(root_, 8, 0);
    lv_obj_set_style_pad_row(root_, 6, 0);
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(root_, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(status_label_, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(status_label_, Color(0x69b7ff), 0);
    lv_label_set_text(status_label_, "BỘ NHỚ • NHẤN ĐỂ DỌN");

    disk_bar_ = MakeUsageBar(root_, kAccentCyan, &disk_label_);
    ram_bar_ = MakeUsageBar(root_, kAccentBlue, &ram_label_);

    Apply(ReadStats());
    timer_ = lv_timer_create(OnTimer, kRefreshMs, this);
}

OptimizeWidget::~OptimizeWidget() {
    if (optimize_thread_.joinable()) optimize_thread_.join();
    LvglLockGuard lock;
    if (timer_) { lv_timer_del(timer_); timer_ = nullptr; }
    if (root_) { lv_obj_del(root_); root_ = nullptr; }
}

void OptimizeWidget::HidePopup() {
    if (root_) {
        lv_obj_set_style_opa(root_, LV_OPA_0, 0);
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    }
}

OptimizeWidget::Stats OptimizeWidget::ReadStats() {
    Stats s;
    if (FILE *f = std::fopen("/proc/meminfo", "r")) {
        char line[128];
        while (std::fgets(line, sizeof(line), f)) {
            unsigned long long v = 0;
            if (std::sscanf(line, "MemTotal: %llu kB", &v) == 1) s.mem_total_kb = v;
            else if (std::sscanf(line, "MemAvailable: %llu kB", &v) == 1) s.mem_avail_kb = v;
            if (s.mem_total_kb && s.mem_avail_kb) break;
        }
        std::fclose(f);
    }
    struct statvfs vfs;
    if (statvfs("/", &vfs) == 0) {
        const uint64_t frsize = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
        s.disk_total_kb = (uint64_t)vfs.f_blocks * frsize / 1024;
        // f_bavail matches `df`'s notion of free space (minus the root reserve).
        s.disk_used_kb = ((uint64_t)vfs.f_blocks - (uint64_t)vfs.f_bavail) * frsize / 1024;
    }
    return s;
}

void OptimizeWidget::Apply(const Stats &s) {
    if (disk_label_ && s.disk_total_kb) {
        SetUsageText(disk_label_, s.disk_used_kb, s.disk_total_kb);
        lv_bar_set_value(disk_bar_, (int32_t)(s.disk_used_kb * 1000 / s.disk_total_kb), LV_ANIM_ON);
    }
    if (ram_label_ && s.mem_total_kb) {
        const uint64_t used = s.mem_total_kb - s.mem_avail_kb;
        SetUsageText(ram_label_, used, s.mem_total_kb);
        lv_bar_set_value(ram_bar_, (int32_t)(used * 1000 / s.mem_total_kb), LV_ANIM_ON);
    }
}

void OptimizeWidget::StartOptimize() {
    bool expected = false;
    if (!optimizing_.compare_exchange_strong(expected, true)) return;
    if (optimize_thread_.joinable()) optimize_thread_.join();
    lv_obj_set_style_image_recolor(button_, Color(0x69b7ff), 0);
    if (status_label_) lv_label_set_text(status_label_, "ĐANG DỌN CACHE…");

    optimize_thread_ = std::thread([this]() {
        const Stats before = ReadStats();
        sync();
        // Drop clean page/dentry/inode caches. Direct write first (we run as
        // root on the Jetson); shell fallback in case /proc is namespaced.
        bool ok = false;
        if (FILE *f = std::fopen("/proc/sys/vm/drop_caches", "w")) {
            ok = std::fputs("3", f) >= 0;
            std::fclose(f);
        }
        if (!ok) {
            std::string out;
            ok = jetson::platform::RunShellCommand(
                     "sync; echo 3 > /proc/sys/vm/drop_caches", out) == 0;
        }
        usleep(400 * 1000); // let the meminfo counters settle
        const Stats after = ReadStats();
        long freed_mb = ((long long)after.mem_avail_kb - (long long)before.mem_avail_kb) / 1024;
        if (freed_mb < 0) freed_mb = 0;

        {
            LvglLockGuard lock;
            Apply(after);
            lv_obj_set_style_image_recolor(button_, lv_color_white(), 0);
            if (status_label_)
                lv_label_set_text(status_label_, ok ? "BỘ NHỚ • ĐÃ TỐI ƯU"
                                                     : "KHÔNG THỂ DỌN CACHE");
            if (notify_) {
                char msg[80];
                if (ok) std::snprintf(msg, sizeof(msg), "Đã giải phóng %ld MB RAM", freed_mb);
                else std::snprintf(msg, sizeof(msg), "Không thể dọn RAM");
                notify_(msg);
            }
        }
        optimizing_ = false;
    });
}

void OptimizeWidget::OnTimer(lv_timer_t *t) {
    auto *self = static_cast<OptimizeWidget *>(lv_timer_get_user_data(t));
    const Stats s = ReadStats();
    LvglLockGuard lock;
    self->Apply(s);
}

void OptimizeWidget::OnOptimizeClicked(lv_event_t *e) {
    auto *self = static_cast<OptimizeWidget *>(lv_event_get_user_data(e));
    LvglLockGuard lock;
    if (!self->root_) return;
    const bool opening = lv_obj_has_flag(self->root_, LV_OBJ_FLAG_HIDDEN);
    if (self->before_open_) {
        self->before_open_(self->root_, self->button_);
    } else if (opening) {
        lv_obj_clear_flag(self->root_, LV_OBJ_FLAG_HIDDEN);
    } else {
        self->HidePopup();
    }
    if (opening) {
        self->Apply(ReadStats());
        // No separate "clean" button: opening the real status action performs
        // the cache drop immediately and then refreshes both usage bars.
        self->StartOptimize();
    }
}

} // namespace home
