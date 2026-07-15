#include "display/common/signal_bars.h"

#include "display/common/lvgl_utils.h"

#include <algorithm>

namespace jetson::ui {

namespace {
constexpr int kBarCount = 4;
constexpr int kBarHeights[kBarCount] = {8, 12, 16, 20};

constexpr int SignalBarCount(int signal_percent) {
    const int strength = signal_percent < 0 ? 0 : (signal_percent > 100 ? 100 : signal_percent);
    return strength == 0 ? 0 : (strength >= 75 ? 4 : strength / 25 + 1);
}

static_assert(SignalBarCount(0) == 0);
static_assert(SignalBarCount(1) == 1);
static_assert(SignalBarCount(25) == 2);
static_assert(SignalBarCount(50) == 3);
static_assert(SignalBarCount(75) == 4);
static_assert(SignalBarCount(100) == 4);
} // namespace

int RssiToSignalPercent(int rssi_dbm) {
    return std::clamp(rssi_dbm + 100, 0, 100);
}

lv_obj_t *CreateSignalBars(lv_obj_t *parent, int signal_percent) {
    const int strength = std::clamp(signal_percent, 0, 100);
    const int filled = SignalBarCount(strength);

    auto *container = lv_obj_create(parent);
    lv_obj_remove_style_all(container);
    lv_obj_set_size(container, 36, 22);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(container, 2, 0);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);

    for (int i = 0; i < kBarCount; ++i) {
        auto *bar = lv_obj_create(container);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 6, kBarHeights[i]);
        lv_obj_set_style_radius(bar, 1, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(bar, i < filled ? lv_color_white() : Color(0x555555), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    }

    return container;
}

} // namespace jetson::ui
