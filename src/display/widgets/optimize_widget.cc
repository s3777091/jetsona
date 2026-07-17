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
constexpr uint32_t kTextDark = 0x1c2733;
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
    lv_obj_set_size(bar, 170, 22);
    lv_bar_set_range(bar, 0, 1000);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(bar, 11, 0);
    lv_obj_set_style_bg_color(bar, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_90, 0);
    lv_obj_set_style_radius(bar, 11, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, Color(accent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_clear_flag(bar, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    auto *label = lv_label_create(bar);
    lv_obj_set_style_text_font(label, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(label, Color(kTextDark), 0);
    lv_label_set_text(label, "-- GB / -- GB");
    lv_obj_align(label, LV_ALIGN_RIGHT_MID, -8, 0);
    *label_out = label;
    return bar;
}

} // namespace

OptimizeWidget::OptimizeWidget(lv_obj_t *parent) {
    root_ = lv_obj_create(parent);
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, LV_SIZE_CONTENT, 66);
    // Tucked under the status strip's wifi/bt/battery cluster (top-right).
    lv_obj_align(root_, LV_ALIGN_TOP_RIGHT, -10, 50);
    lv_obj_set_style_radius(root_, 33, 0);
    lv_obj_set_style_bg_color(root_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_60, 0);
    lv_obj_set_style_border_width(root_, 1, 0);
    lv_obj_set_style_border_color(root_, lv_color_white(), 0);
    lv_obj_set_style_border_opa(root_, LV_OPA_50, 0);
    lv_obj_set_style_shadow_color(root_, lv_color_black(), 0);
    lv_obj_set_style_shadow_width(root_, 12, 0);
    lv_obj_set_style_shadow_offset_y(root_, 3, 0);
    lv_obj_set_style_shadow_opa(root_, LV_OPA_20, 0);
    lv_obj_set_style_pad_left(root_, 8, 0);
    lv_obj_set_style_pad_right(root_, 10, 0);
    lv_obj_set_style_pad_top(root_, 7, 0);
    lv_obj_set_style_pad_bottom(root_, 7, 0);
    lv_obj_set_style_pad_column(root_, 8, 0);
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(root_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(root_, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    // Round blue action button; the "Tối ưu" caption beside it shares the
    // same clickable hit area.
    button_ = lv_obj_create(root_);
    lv_obj_remove_style_all(button_);
    lv_obj_set_size(button_, LV_SIZE_CONTENT, 52);
    lv_obj_set_style_pad_column(button_, 8, 0);
    lv_obj_set_flex_flow(button_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(button_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(button_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(button_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(button_, OnOptimizeClicked, LV_EVENT_CLICKED, this);

    auto *circle = lv_obj_create(button_);
    lv_obj_remove_style_all(circle);
    lv_obj_set_size(circle, 44, 44);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(circle, Color(kAccentBlue), 0);
    lv_obj_set_style_bg_opa(circle, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_color(circle, Color(kAccentBlue), 0);
    lv_obj_set_style_shadow_width(circle, 10, 0);
    lv_obj_set_style_shadow_opa(circle, LV_OPA_30, 0);
    lv_obj_clear_flag(circle, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    auto *icon = jetson::ui::CreateAppIcon(circle, "clean-cache", 24);
    lv_obj_set_style_image_recolor(icon, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
    lv_obj_center(icon);

    auto *caption = lv_label_create(button_);
    lv_obj_set_style_text_font(caption, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(caption, Color(kTextDark), 0);
    lv_label_set_text(caption, "Tối ưu");
    lv_obj_clear_flag(caption, LV_OBJ_FLAG_CLICKABLE);

    auto *bars = lv_obj_create(root_);
    lv_obj_remove_style_all(bars);
    lv_obj_set_size(bars, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(bars, 6, 0);
    lv_obj_set_flex_flow(bars, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(bars, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(bars, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    disk_bar_ = MakeUsageBar(bars, kAccentCyan, &disk_label_);
    ram_bar_ = MakeUsageBar(bars, kAccentBlue, &ram_label_);

    Apply(ReadStats());
    timer_ = lv_timer_create(OnTimer, kRefreshMs, this);
}

OptimizeWidget::~OptimizeWidget() {
    LvglLockGuard lock;
    if (timer_) { lv_timer_del(timer_); timer_ = nullptr; }
    if (root_) { lv_obj_del(root_); root_ = nullptr; }
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
    lv_obj_set_style_opa(button_, LV_OPA_50, 0);

    std::thread([this]() {
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
            lv_obj_set_style_opa(button_, LV_OPA_COVER, 0);
            if (notify_) {
                char msg[80];
                if (ok) std::snprintf(msg, sizeof(msg), "Đã giải phóng %ld MB RAM", freed_mb);
                else std::snprintf(msg, sizeof(msg), "Không thể dọn RAM");
                notify_(msg);
            }
        }
        optimizing_ = false;
    }).detach();
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
    self->StartOptimize();
}

} // namespace home
