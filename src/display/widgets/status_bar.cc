#include "display/widgets/status_bar.h"
#include "display/common/airplane_icon.h"
#include "display/common/lvgl_utils.h"
#include "display/core/app_icons.h"
#include "display/core/lvgl_image.h"
#include "fonts.h"
#include "media/player_controller.h"
#include "net/airplane_mode.h"
#include "net/bluetooth_manager.h"
#include "net/ethernet_status.h"
#include "net/vpn_manager.h"
#include "net/wifi_manager.h"
#include "settings.h"
#include "board.h"

#include <lvgl.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>

#define TAG "StatusBar"

namespace home {

using jetson::ui::Color;
using jetson::ui::LvglLockGuard;

namespace {
int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// The screen is 800x480, so these keep the resting island clearly separated
// from the left/right status clusters while leaving enough room for Vietnamese
// notification text when it expands.
constexpr int kPillW = 132;
constexpr int kPillH = 36;
constexpr int kExpandedW = 430;
constexpr int kExpandedH = 72;
constexpr int kMediaCompactW = 224;
constexpr int kMediaCompactH = 40;
constexpr int kMediaExpandedW = 430;
constexpr int kMediaExpandedH = 126;
constexpr int kTopInset = 3;
constexpr int kAutoCloseMs = 6000;

// Box size for the PNG status icons (assets/icons/app, 28x28 sources).
constexpr int kStatusIconPx = 20;
// Radio state poll period. nmcli/bluetoothctl shell-outs run on the worker
// thread, so this only bounds how stale the wifi/bt icons can be.
constexpr auto kConnPollInterval = std::chrono::seconds(10);
constexpr auto kConnPollTick = std::chrono::milliseconds(500);

void RemoveInteraction(lv_obj_t *obj) {
    if (!obj) return;
    lv_obj_clear_flag(obj,
        (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
}

std::string MediaTime(int64_t milliseconds) {
    const int seconds = static_cast<int>(std::max<int64_t>(0, milliseconds) / 1000);
    char out[16];
    std::snprintf(out, sizeof(out), "%d:%02d", seconds / 60, seconds % 60);
    return out;
}

void ImageDimensions(const lv_img_dsc_t *dsc, int &w, int &h) {
    w = h = 240;
    if (!dsc || !dsc->data || dsc->data_size < 24) return;
    const uint8_t *d = dsc->data;
    const size_t n = dsc->data_size;
    if (d[0] == 0x89 && d[1] == 0x50) {
        w = (d[16] << 24) | (d[17] << 16) | (d[18] << 8) | d[19];
        h = (d[20] << 24) | (d[21] << 16) | (d[22] << 8) | d[23];
        return;
    }
    if (d[0] != 0xff || d[1] != 0xd8) return;
    size_t offset = 2;
    while (offset + 8 < n) {
        if (d[offset] != 0xff) { ++offset; continue; }
        while (offset < n && d[offset] == 0xff) ++offset;
        if (offset >= n) break;
        const uint8_t marker = d[offset++];
        if (marker == 0xd8 || marker == 0xd9) continue;
        if (offset + 2 > n) break;
        const size_t length = (static_cast<size_t>(d[offset]) << 8) |
                              d[offset + 1];
        if (length < 2 || offset + length > n) break;
        const bool start_of_frame =
            (marker >= 0xc0 && marker <= 0xc3) ||
            (marker >= 0xc5 && marker <= 0xc7) ||
            (marker >= 0xc9 && marker <= 0xcb) ||
            (marker >= 0xcd && marker <= 0xcf);
        if (start_of_frame && length >= 7) {
            h = (d[offset + 3] << 8) | d[offset + 4];
            w = (d[offset + 5] << 8) | d[offset + 6];
            return;
        }
        offset += length;
    }
}
} // namespace

StatusBar::StatusBar(lv_obj_t *parent) {
    if (!parent) parent = lv_layer_top();

    // ---- Transparent iPhone-like status row ----
    // Status information belongs beside the sensor cutout, never inside it.
    status_strip_ = lv_obj_create(parent);
    lv_obj_remove_style_all(status_strip_);
    lv_obj_set_size(status_strip_, lv_pct(100), 42);
    lv_obj_set_pos(status_strip_, 0, 0);
    lv_obj_clear_flag(status_strip_,
                      (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    left_cluster_ = lv_obj_create(status_strip_);
    lv_obj_remove_style_all(left_cluster_);
    lv_obj_set_size(left_cluster_, 278, lv_pct(100));
    lv_obj_align(left_cluster_, LV_ALIGN_LEFT_MID, 14, 0);
    lv_obj_set_flex_flow(left_cluster_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left_cluster_, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left_cluster_, 8, 0);
    lv_obj_clear_flag(left_cluster_,
                      (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    // Six quick-setting icons, in the requested left-to-right order:
    // cache, Bluetooth, Wi-Fi, sound, display brightness, power.
    right_cluster_ = lv_obj_create(status_strip_);
    lv_obj_remove_style_all(right_cluster_);
    lv_obj_set_size(right_cluster_, 184, lv_pct(100));
    lv_obj_align(right_cluster_, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_set_flex_flow(right_cluster_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_cluster_, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right_cluster_, 8, 0);
    lv_obj_clear_flag(right_cluster_,
                      (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    // PNG icon from assets/icons/app, recolored white like the old glyphs so
    // it stays readable on any wallpaper.
    auto add_icon = [&](lv_obj_t **out, const char *png, lv_event_cb_t cb) {
        lv_obj_t *o = jetson::ui::CreateAppIcon(right_cluster_, png, kStatusIconPx);
        lv_obj_set_style_image_recolor(o, lv_color_white(), 0);
        lv_obj_set_style_image_recolor_opa(o, LV_OPA_COVER, 0);
        if (cb) {
            lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_ext_click_area(o, 8);
            lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, this);
        }
        *out = o;
    };

    // One short line fits before the resting island: time and numeric
    // day/month. The full weekday/date stays out of the center of the
    // wallpaper so it does not duplicate this status information.
    datetime_label_ = lv_label_create(left_cluster_);
    lv_obj_set_style_text_font(datetime_label_, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(datetime_label_, lv_color_white(), 0);
    lv_label_set_text(datetime_label_, "--:--  --/--");

    // ---- Resting Dynamic Island (centered, solid black) ----
    pill_ = lv_obj_create(parent);
    lv_obj_remove_style_all(pill_);
    lv_obj_set_size(pill_, kPillW, kPillH);
    lv_obj_align(pill_, LV_ALIGN_TOP_MID, 0, kTopInset);
    lv_obj_set_style_bg_color(pill_, Color(0x000000), 0);
    lv_obj_set_style_bg_grad_color(pill_, Color(0x0b1220), 0);
    lv_obj_set_style_bg_grad_dir(pill_, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(pill_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pill_, 44, 0);
    lv_obj_set_style_border_width(pill_, 1, 0);
    lv_obj_set_style_border_color(pill_, Color(0x253142), 0);
    lv_obj_set_style_border_opa(pill_, LV_OPA_40, 0);
    lv_obj_set_style_shadow_color(pill_, lv_color_black(), 0);
    lv_obj_set_style_shadow_width(pill_, 12, 0);
    lv_obj_set_style_shadow_opa(pill_, LV_OPA_30, 0);
    // Children (notification icon circle, media rows) are laid out for the
    // expanded pill; without corner clipping they poke out of the resting
    // 132x36 pill and read as stray icons floating over whatever screen is
    // open (the bar is global on lv_layer_top()).
    lv_obj_set_style_clip_corner(pill_, true, 0);
    lv_obj_clear_flag(pill_, LV_OBJ_FLAG_SCROLLABLE);
    // With music active, a click expands/collapses now-playing. Long-press
    // retains the app-switcher gesture; without music, a normal click opens it.
    lv_obj_add_flag(pill_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(pill_, 6);
    lv_obj_add_event_cb(pill_, OnIslandClick, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(pill_, OnIslandLongPress, LV_EVENT_LONG_PRESSED, this);

    // Resting island content: Bluetooth device mini icon on the left and a
    // connection-status ring on the right (green while a device is connected,
    // dim gray otherwise). The icon swaps per polled device kind:
    // controller-mini / headphones / unknow-device. Non-clickable so the
    // island click-to-switcher keeps working.
    island_rest_ = lv_obj_create(pill_);
    lv_obj_remove_style_all(island_rest_);
    lv_obj_set_size(island_rest_, lv_pct(100), lv_pct(100));
    lv_obj_clear_flag(island_rest_,
                      (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    island_device_icon_ = jetson::ui::CreateAppIcon(island_rest_, "unknow-device", 18);
    lv_obj_set_style_image_recolor(island_device_icon_, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(island_device_icon_, LV_OPA_COVER, 0);
    lv_obj_align(island_device_icon_, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_add_flag(island_device_icon_, LV_OBJ_FLAG_HIDDEN);

    island_ring_ = lv_obj_create(island_rest_);
    lv_obj_remove_style_all(island_ring_);
    lv_obj_set_size(island_ring_, 18, 18);
    lv_obj_align(island_ring_, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_set_style_radius(island_ring_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(island_ring_, 3, 0);
    lv_obj_set_style_border_color(island_ring_, Color(0x3a3a3c), 0);
    lv_obj_set_style_bg_opa(island_ring_, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(island_ring_,
                      (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    // Expanded content is already laid out inside the pill, but kept hidden
    // until the pill has started to bloom. This avoids a separate toast panel.
    island_content_ = lv_obj_create(pill_);
    lv_obj_remove_style_all(island_content_);
    lv_obj_set_size(island_content_, lv_pct(100), lv_pct(100));
    lv_obj_clear_flag(island_content_,
                      (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_style_opa(island_content_, LV_OPA_0, 0);
    lv_obj_add_flag(island_content_, LV_OBJ_FLAG_HIDDEN);

    island_icon_bg_ = lv_obj_create(island_content_);
    lv_obj_remove_style_all(island_icon_bg_);
    lv_obj_set_size(island_icon_bg_, 38, 38);
    lv_obj_align(island_icon_bg_, LV_ALIGN_LEFT_MID, 14, 0);
    lv_obj_set_style_radius(island_icon_bg_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(island_icon_bg_, Color(0x1677ff), 0);
    lv_obj_set_style_bg_opa(island_icon_bg_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(island_icon_bg_,
                      (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    island_icon_ = lv_label_create(island_icon_bg_);
    lv_obj_set_style_text_font(island_icon_, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(island_icon_, lv_color_white(), 0);
    lv_label_set_text(island_icon_, LV_SYMBOL_BELL);
    lv_obj_center(island_icon_);

    island_title_ = lv_label_create(island_content_);
    lv_obj_set_style_text_font(island_title_, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(island_title_, Color(0x69b7ff), 0);
    lv_obj_set_pos(island_title_, 64, 7);
    lv_label_set_text(island_title_, "THÔNG BÁO");

    island_message_ = lv_label_create(island_content_);
    lv_obj_set_size(island_message_, kExpandedW - 82, 26);
    lv_obj_set_style_text_font(island_message_, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(island_message_, lv_color_white(), 0);
    lv_obj_set_pos(island_message_, 64, 34);
    lv_label_set_long_mode(island_message_, LV_LABEL_LONG_DOT);
    lv_label_set_text(island_message_, "");

    BuildMediaContent();

    // Quick settings live *inside* the Dynamic Island. The host always tracks
    // the pill's animated size, so its children are naturally revealed by the
    // pill's rounded clipping instead of appearing as detached menu boxes.
    quick_host_ = lv_obj_create(pill_);
    lv_obj_remove_style_all(quick_host_);
    lv_obj_set_size(quick_host_, lv_pct(100), lv_pct(100));
    lv_obj_center(quick_host_);
    lv_obj_set_style_opa(quick_host_, LV_OPA_0, 0);
    lv_obj_clear_flag(quick_host_,
                      (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_add_flag(quick_host_, LV_OBJ_FLAG_HIDDEN);

    optimize_widget_ = std::make_unique<OptimizeWidget>(right_cluster_, quick_host_);
    optimize_widget_->SetBeforeOpenCb([this](lv_obj_t *content, lv_obj_t *icon) {
        ShowQuickMenu(content, icon);
    });

    add_icon(&bt_icon_, "bluetooth", OnBtClick);
    add_icon(&wifi_icon_, "wifi", OnWifiClick);
    add_icon(&sound_icon_, "speaker", OnSoundClick);
    add_icon(&brightness_icon_, "sun", OnBrightnessClick);
    add_icon(&power_icon_, "start", OnPowerClick);

    // Build every quick surface only after the shared island host exists.
    BuildQuickMenus();
    BuildPowerMenu();

    lv_obj_add_event_cb(pill_, OnDeleted, LV_EVENT_DELETE, this);
    timer_ = lv_timer_create(OnTimer, 1000, this);
    Refresh();

    // Radio-state poller. Publishes into atomics only; the LVGL-side icon
    // updates happen in RefreshConnectivity() on the 1 Hz timer.
    conn_poll_thread_ = std::thread([this]() {
        auto next_poll = std::chrono::steady_clock::now(); // first poll now
        while (!conn_poll_stop_.load()) {
            // Wired link is a sysfs read, not a shell-out: refresh it every
            // tick so plugging/unplugging the LAN cable shows within ~1 s.
            polled_eth_connected_.store(jetson::IsEthernetConnected() ? 1 : 0);
            if (std::chrono::steady_clock::now() >= next_poll) {
                next_poll = std::chrono::steady_clock::now() + kConnPollInterval;
                if (jetson::IsAirplaneModeEnabled()) {
                    polled_wifi_signal_.store(-1);
                    polled_wifi_enabled_.store(0);
                    polled_bt_powered_.store(0);
                    polled_bt_device_.store(
                        static_cast<int>(jetson::BtDeviceKind::None));
                } else {
                    auto &wifi = jetson::WifiManager::Instance();
                    polled_wifi_enabled_.store(wifi.IsEnabled() ? 1 : 0);
                    polled_wifi_signal_.store(wifi.ActiveSignal());
                    auto &bt = jetson::BluetoothManager::Instance();
                    const bool bt_on = bt.IsPowered();
                    polled_bt_powered_.store(bt_on ? 1 : 0);
                    // Device kind drives the island mini icon + status ring.
                    polled_bt_device_.store(static_cast<int>(
                        bt_on ? bt.ConnectedDeviceKind()
                              : jetson::BtDeviceKind::None));
                }
            }
            std::this_thread::sleep_for(kConnPollTick);
        }
    });
}

void StatusBar::BuildMediaContent() {
    media_content_ = lv_obj_create(pill_);
    lv_obj_remove_style_all(media_content_);
    lv_obj_set_size(media_content_, lv_pct(100), lv_pct(100));
    RemoveInteraction(media_content_);
    lv_obj_add_flag(media_content_, LV_OBJ_FLAG_HIDDEN);

    auto create_art_host = [](lv_obj_t *parent, int size, int x, int y) {
        auto *host = lv_obj_create(parent);
        lv_obj_remove_style_all(host);
        lv_obj_set_size(host, size, size);
        lv_obj_set_pos(host, x, y);
        lv_obj_set_style_radius(host, size >= 40 ? 9 : 7, 0);
        lv_obj_set_style_clip_corner(host, true, 0);
        lv_obj_set_style_bg_color(host, Color(0x27213b), 0);
        lv_obj_set_style_bg_grad_color(host, Color(0x101827), 0);
        lv_obj_set_style_bg_grad_dir(host, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(host, LV_OPA_COVER, 0);
        RemoveInteraction(host);
        auto *fallback = lv_label_create(host);
        lv_obj_set_style_text_font(fallback, &BUILTIN_ICON_FONT, 0);
        lv_obj_set_style_text_color(fallback, Color(0x22d3ee), 0);
        lv_label_set_text(fallback, LV_SYMBOL_AUDIO);
        lv_obj_center(fallback);
        RemoveInteraction(fallback);
        return host;
    };

    // Compact now-playing state (reference image 5).
    media_compact_ = lv_obj_create(media_content_);
    lv_obj_remove_style_all(media_compact_);
    lv_obj_set_size(media_compact_, kMediaCompactW, kMediaCompactH);
    RemoveInteraction(media_compact_);
    media_compact_art_host_ = create_art_host(media_compact_, 30, 6, 5);

    media_compact_title_ = lv_label_create(media_compact_);
    lv_obj_set_pos(media_compact_title_, 44, 3);
    lv_obj_set_size(media_compact_title_, 142, 19);
    lv_obj_set_style_text_font(media_compact_title_, jetson::BuiltinTextFaceAt(14), 0);
    lv_obj_set_style_text_color(media_compact_title_, lv_color_white(), 0);
    lv_label_set_long_mode(media_compact_title_, LV_LABEL_LONG_DOT);
    RemoveInteraction(media_compact_title_);

    media_compact_artist_ = lv_label_create(media_compact_);
    lv_obj_set_pos(media_compact_artist_, 44, 20);
    lv_obj_set_size(media_compact_artist_, 142, 17);
    lv_obj_set_style_text_font(media_compact_artist_, jetson::BuiltinTextFaceAt(13), 0);
    lv_obj_set_style_text_color(media_compact_artist_, Color(0x9ca3af), 0);
    lv_label_set_long_mode(media_compact_artist_, LV_LABEL_LONG_DOT);
    RemoveInteraction(media_compact_artist_);

    media_compact_more_ = lv_label_create(media_compact_);
    lv_obj_set_pos(media_compact_more_, 191, 7);
    lv_obj_set_size(media_compact_more_, 28, 22);
    lv_obj_set_style_text_font(media_compact_more_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(media_compact_more_, Color(0x777b87), 0);
    lv_label_set_text(media_compact_more_, "•••");
    RemoveInteraction(media_compact_more_);

    // Expanded controls (reference image 4; waveform intentionally omitted).
    media_expanded_ = lv_obj_create(media_content_);
    lv_obj_remove_style_all(media_expanded_);
    lv_obj_set_size(media_expanded_, kMediaExpandedW, kMediaExpandedH);
    RemoveInteraction(media_expanded_);
    lv_obj_add_flag(media_expanded_, LV_OBJ_FLAG_HIDDEN);
    media_expanded_art_host_ = create_art_host(media_expanded_, 48, 14, 12);

    media_expanded_title_ = lv_label_create(media_expanded_);
    lv_obj_set_pos(media_expanded_title_, 74, 10);
    lv_obj_set_size(media_expanded_title_, 328, 24);
    lv_obj_set_style_text_font(media_expanded_title_, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(media_expanded_title_, lv_color_white(), 0);
    lv_label_set_long_mode(media_expanded_title_, LV_LABEL_LONG_DOT);
    RemoveInteraction(media_expanded_title_);

    media_expanded_artist_ = lv_label_create(media_expanded_);
    lv_obj_set_pos(media_expanded_artist_, 74, 35);
    lv_obj_set_size(media_expanded_artist_, 328, 20);
    lv_obj_set_style_text_font(media_expanded_artist_, jetson::BuiltinTextFaceAt(14), 0);
    lv_obj_set_style_text_color(media_expanded_artist_, Color(0x9ca3af), 0);
    lv_label_set_long_mode(media_expanded_artist_, LV_LABEL_LONG_DOT);
    RemoveInteraction(media_expanded_artist_);

    media_progress_ = lv_bar_create(media_expanded_);
    lv_obj_set_size(media_progress_, kMediaExpandedW - 28, 4);
    lv_obj_set_pos(media_progress_, 14, 64);
    lv_bar_set_range(media_progress_, 0, 1000);
    lv_bar_set_value(media_progress_, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(media_progress_, Color(0x30313a), LV_PART_MAIN);
    lv_obj_set_style_bg_color(media_progress_, Color(0x22d3ee), LV_PART_INDICATOR);
    lv_obj_set_style_radius(media_progress_, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(media_progress_, 2, LV_PART_INDICATOR);
    RemoveInteraction(media_progress_);

    media_elapsed_ = lv_label_create(media_expanded_);
    lv_obj_set_pos(media_elapsed_, 14, 71);
    lv_obj_set_size(media_elapsed_, 58, 18);
    lv_obj_set_style_text_font(media_elapsed_, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(media_elapsed_, Color(0x9ca3af), 0);
    RemoveInteraction(media_elapsed_);
    media_remaining_ = lv_label_create(media_expanded_);
    lv_obj_set_pos(media_remaining_, kMediaExpandedW - 72, 71);
    lv_obj_set_size(media_remaining_, 58, 18);
    lv_obj_set_style_text_align(media_remaining_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(media_remaining_, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(media_remaining_, Color(0x9ca3af), 0);
    RemoveInteraction(media_remaining_);

    auto create_control = [this](const char *symbol, int x, int size,
                                 lv_event_cb_t callback, lv_obj_t **label_out) {
        auto *button = lv_obj_create(media_expanded_);
        lv_obj_remove_style_all(button);
        lv_obj_set_size(button, size, size);
        lv_obj_set_pos(button, x, 86);
        lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(button, Color(0x25262d), 0);
        lv_obj_set_style_bg_color(button, Color(0x22d3ee), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, this);
        auto *label = lv_label_create(button);
        lv_obj_set_style_text_font(label, &BUILTIN_ICON_FONT, 0);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_label_set_text(label, symbol);
        lv_obj_center(label);
        RemoveInteraction(label);
        if (label_out) *label_out = label;
    };
    create_control(LV_SYMBOL_PREV, 137, 34, OnMediaPrevious, nullptr);
    create_control(LV_SYMBOL_PLAY, 190, 40, OnMediaToggle, &media_toggle_label_);
    create_control(LV_SYMBOL_NEXT, 249, 34, OnMediaNext, nullptr);
}

void StatusBar::SyncIslandRest() {
    if (!island_rest_) return;
    if (!notification_visible_ && !media_available_ && !quick_island_open_)
        lv_obj_clear_flag(island_rest_, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(island_rest_, LV_OBJ_FLAG_HIDDEN);
}

void StatusBar::HideMediaContent() {
    if (media_content_) lv_obj_add_flag(media_content_, LV_OBJ_FLAG_HIDDEN);
}

void StatusBar::LoadMediaArtwork(const std::string &path) {
    if (path == media_artwork_path_) return;
    media_artwork_path_ = path;
    if (media_compact_art_) {
        lv_image_set_src(media_compact_art_, nullptr);
        lv_obj_del(media_compact_art_);
        media_compact_art_ = nullptr;
    }
    if (media_expanded_art_) {
        lv_image_set_src(media_expanded_art_, nullptr);
        lv_obj_del(media_expanded_art_);
        media_expanded_art_ = nullptr;
    }
    if (media_artwork_)
        lv_image_cache_drop(media_artwork_->image_dsc());
    media_artwork_.reset();
    if (path.empty()) return;
    media_artwork_ = LvglImageFromFile(path);
    if (!media_artwork_) return;

    int w, h;
    ImageDimensions(media_artwork_->image_dsc(), w, h);
    auto create_image = [&](lv_obj_t *host, int size) {
        auto *image = lv_image_create(host);
        lv_image_set_src(image, media_artwork_->image_dsc());
        lv_obj_set_size(image, w, h);
        lv_image_set_scale(image,
            static_cast<uint32_t>(size * 256 / std::max(1, std::min(w, h))));
        lv_image_set_pivot(image, w / 2, h / 2);
        lv_obj_center(image);
        RemoveInteraction(image);
        return image;
    };
    media_compact_art_ = create_image(media_compact_art_host_, 30);
    media_expanded_art_ = create_image(media_expanded_art_host_, 48);
}

void StatusBar::ShowMediaPresentation(bool animate) {
    if (!visible_ || !media_available_ || notification_visible_ || !media_content_)
        return;
    if (quick_island_open_) return;
    if (island_content_) lv_obj_add_flag(island_content_, LV_OBJ_FLAG_HIDDEN);
    SyncIslandRest();
    lv_obj_clear_flag(media_content_, LV_OBJ_FLAG_HIDDEN);
    if (media_expanded_open_) {
        lv_obj_add_flag(media_compact_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(media_expanded_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(media_compact_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(media_expanded_, LV_OBJ_FLAG_HIDDEN);
    }
    const int target_w = media_expanded_open_ ? kMediaExpandedW : kMediaCompactW;
    const int target_h = media_expanded_open_ ? kMediaExpandedH : kMediaCompactH;
    lv_obj_set_style_border_color(pill_, Color(0x22d3ee), 0);
    lv_obj_set_style_border_opa(pill_, LV_OPA_40, 0);
    lv_obj_update_layout(pill_);
    if (animate && (lv_obj_get_width(pill_) != target_w ||
                    lv_obj_get_height(pill_) != target_h)) {
        AnimateIslandSize(target_w, target_h, false);
    } else {
        lv_obj_set_size(pill_, target_w, target_h);
        lv_obj_align(pill_, LV_ALIGN_TOP_MID, 0, kTopInset);
    }
    island_expanded_ = media_expanded_open_;
}

void StatusBar::RefreshMedia(bool force_layout) {
    const auto snapshot = jetson::music::PlayerController::Instance().Snapshot();
    const bool available = snapshot.has_current &&
                           snapshot.status != jetson::music::PlaybackStatus::Idle;
    if (!available) {
        const bool was_available = media_available_;
        media_available_ = false;
        media_expanded_open_ = false;
        HideMediaContent();
        SyncIslandRest();
        if (was_available && !notification_visible_ && !quick_island_open_ && pill_) {
            lv_anim_delete(pill_, OnIslandWidth);
            lv_anim_delete(pill_, OnIslandHeight);
            lv_obj_set_size(pill_, kPillW, kPillH);
            lv_obj_align(pill_, LV_ALIGN_TOP_MID, 0, kTopInset);
            lv_obj_set_style_border_color(pill_, Color(0x253142), 0);
        }
        return;
    }

    media_available_ = true;
    lv_label_set_text(media_compact_title_, snapshot.current.title.c_str());
    lv_label_set_text(media_compact_artist_, snapshot.current.artist.c_str());
    lv_label_set_text(media_expanded_title_, snapshot.current.title.c_str());
    lv_label_set_text(media_expanded_artist_,
        snapshot.status == jetson::music::PlaybackStatus::Error &&
                !snapshot.error.empty()
            ? snapshot.error.c_str()
            : snapshot.current.artist.c_str());
    const bool playing = snapshot.status == jetson::music::PlaybackStatus::Playing ||
                         snapshot.status == jetson::music::PlaybackStatus::Resolving ||
                         snapshot.status == jetson::music::PlaybackStatus::Buffering;
    lv_label_set_text(media_toggle_label_, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    const int progress = snapshot.duration_ms > 0
        ? static_cast<int>(std::clamp<int64_t>(
              snapshot.position_ms * 1000 / snapshot.duration_ms, 0, 1000))
        : 0;
    lv_bar_set_value(media_progress_, progress, LV_ANIM_OFF);
    const std::string elapsed = MediaTime(snapshot.position_ms);
    const std::string remaining = "-" +
        MediaTime(std::max<int64_t>(0, snapshot.duration_ms - snapshot.position_ms));
    lv_label_set_text(media_elapsed_, elapsed.c_str());
    lv_label_set_text(media_remaining_, remaining.c_str());
    if (force_layout || snapshot.revision != media_revision_ ||
        snapshot.current.artwork_path != media_artwork_path_) {
        media_revision_ = snapshot.revision;
        LoadMediaArtwork(snapshot.current.artwork_path);
    }
    if (!notification_visible_) ShowMediaPresentation(force_layout);
}

lv_obj_t *StatusBar::CreateQuickMenu(int width) {
    auto *menu = lv_obj_create(quick_host_ ? quick_host_ : pill_);
    lv_obj_remove_style_all(menu);
    lv_obj_set_size(menu, width, LV_SIZE_CONTENT);
    lv_obj_center(menu);
    lv_obj_set_style_bg_opa(menu, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(menu, 12, 0);
    lv_obj_set_style_pad_row(menu, 5, 0);
    lv_obj_set_flex_flow(menu, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(menu,
                      (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_style_opa(menu, LV_OPA_0, 0);
    lv_obj_add_flag(menu, LV_OBJ_FLAG_HIDDEN);
    return menu;
}

void StatusBar::RebuildWifiMenu() {
    if (!wifi_menu_) return;
    lv_obj_clean(wifi_menu_);
    wifi_row_ctx_.clear();

    auto *header = lv_obj_create(wifi_menu_);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, lv_pct(100), 34);
    auto *title = lv_label_create(header);
    lv_obj_set_style_text_font(title, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text(title, "Wi-Fi");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 2, 0);
    wifi_switch_ = lv_switch_create(header);
    lv_obj_set_size(wifi_switch_, 42, 24);
    lv_obj_align(wifi_switch_, LV_ALIGN_RIGHT_MID, -2, 0);
    if (polled_wifi_enabled_.load() == 1)
        lv_obj_add_state(wifi_switch_, LV_STATE_CHECKED);
    lv_obj_add_event_cb(wifi_switch_, OnWifiToggle, LV_EVENT_VALUE_CHANGED, this);

    std::vector<jetson::WifiNetwork> networks;
    {
        std::lock_guard<std::mutex> lock(quick_scan_mutex_);
        networks = quick_wifi_networks_;
    }
    if (networks.size() > 4) networks.resize(4);

    auto add_text_row = [&](const char *text, bool clickable, lv_event_cb_t cb,
                            void *data) {
        auto *row = lv_obj_create(wifi_menu_);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 36);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_bg_color(row, Color(0x121a29), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_70, 0);
        lv_obj_set_style_bg_color(row, Color(0x17365f), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        if (clickable) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, data);
        }
        auto *label = lv_label_create(row);
        lv_obj_set_width(label, 178);
        lv_obj_set_style_text_font(label, &BUILTIN_SMALL_TEXT_FONT, 0);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_label_set_text(label, text);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 4, 0);
        return row;
    };

    if (networks.empty()) {
        add_text_row(wifi_scan_busy_.load() ? "Đang quét mạng…" : "Không tìm thấy mạng",
                     false, nullptr, nullptr);
    } else {
        for (const auto &network : networks) {
            auto ctx = std::make_unique<QuickRowContext>();
            ctx->self = this;
            ctx->id = network.ssid;
            ctx->active = network.in_use;
            ctx->secured = network.secured;
            ctx->known = network.known;
            QuickRowContext *raw = ctx.get();
            wifi_row_ctx_.push_back(std::move(ctx));
            auto *row = add_text_row(network.ssid.c_str(), true, OnWifiRow, raw);
            auto *right = lv_label_create(row);
            lv_obj_set_style_text_font(right, &BUILTIN_SMALL_TEXT_FONT, 0);
            lv_obj_set_style_text_color(right,
                Color(network.in_use ? 0x56b4ff : 0x9ca3af), 0);
            char state[32];
            if (network.in_use) {
                std::snprintf(state, sizeof(state), "Nối %d%%", network.signal);
            } else {
                std::snprintf(state, sizeof(state), "%s%d%%",
                              network.secured ? "Khóa " : "", network.signal);
            }
            lv_label_set_text(right, state);
            lv_obj_align(right, LV_ALIGN_RIGHT_MID, -4, 0);
        }
    }
    auto *settings = add_text_row("Cài đặt Wi-Fi…", true, OnWifiSettings, this);
    lv_obj_set_style_border_width(settings, 1, 0);
    lv_obj_set_style_border_side(settings, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(settings, Color(0x344154), 0);
    applied_wifi_scan_revision_ = wifi_scan_revision_.load();
    if (quick_island_open_ && active_quick_menu_ == wifi_menu_) {
        AnimateDrop(wifi_menu_, true);
        ResizeQuickIsland(wifi_menu_);
    }
}

void StatusBar::RebuildBluetoothMenu() {
    if (!bt_menu_) return;
    lv_obj_clean(bt_menu_);
    bt_row_ctx_.clear();

    auto *header = lv_obj_create(bt_menu_);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, lv_pct(100), 34);
    auto *title = lv_label_create(header);
    lv_obj_set_style_text_font(title, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text(title, "Bluetooth");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 2, 0);
    bt_switch_ = lv_switch_create(header);
    lv_obj_set_size(bt_switch_, 42, 24);
    lv_obj_align(bt_switch_, LV_ALIGN_RIGHT_MID, -2, 0);
    if (polled_bt_powered_.load() == 1)
        lv_obj_add_state(bt_switch_, LV_STATE_CHECKED);
    lv_obj_add_event_cb(bt_switch_, OnBluetoothToggle, LV_EVENT_VALUE_CHANGED, this);

    std::vector<jetson::BtDevice> devices;
    {
        std::lock_guard<std::mutex> lock(quick_scan_mutex_);
        devices = quick_bt_devices_;
    }
    // Keyboards are intentionally absent from this compact device menu; they
    // remain available in the full Bluetooth settings screen.
    devices.erase(std::remove_if(devices.begin(), devices.end(), [](const auto &d) {
        std::string name = d.name;
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return name.find("keyboard") != std::string::npos ||
               name.find("off-key") != std::string::npos;
    }), devices.end());
    if (devices.size() > 4) devices.resize(4);

    auto add_row = [&](const char *text, bool clickable, lv_event_cb_t cb, void *data) {
        auto *row = lv_obj_create(bt_menu_);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 36);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_bg_color(row, Color(0x121a29), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_70, 0);
        lv_obj_set_style_bg_color(row, Color(0x17365f), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        if (clickable) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, data);
        }
        auto *label = lv_label_create(row);
        lv_obj_set_width(label, 184);
        lv_obj_set_style_text_font(label, &BUILTIN_SMALL_TEXT_FONT, 0);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_label_set_text(label, text);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 4, 0);
        return row;
    };

    if (devices.empty()) {
        add_row(bt_scan_busy_.load() ? "Đang tìm thiết bị…" : "Không có thiết bị gần đây",
                false, nullptr, nullptr);
    } else {
        for (const auto &device : devices) {
            auto ctx = std::make_unique<QuickRowContext>();
            ctx->self = this;
            ctx->id = device.address;
            ctx->active = device.connected;
            ctx->known = device.paired;
            QuickRowContext *raw = ctx.get();
            bt_row_ctx_.push_back(std::move(ctx));
            auto *row = add_row(device.name.c_str(), true, OnBluetoothRow, raw);
            auto *state = lv_label_create(row);
            lv_obj_set_style_text_font(state, &BUILTIN_SMALL_TEXT_FONT, 0);
            lv_obj_set_style_text_color(state,
                Color(device.connected ? 0x56b4ff : 0x9ca3af), 0);
            lv_label_set_text(state, device.connected ? "Đã nối" : (device.paired ? "Đã ghép" : ""));
            lv_obj_align(state, LV_ALIGN_RIGHT_MID, -4, 0);
        }
    }
    auto *settings = add_row("Cài đặt Bluetooth…", true, OnBluetoothSettings, this);
    lv_obj_set_style_border_width(settings, 1, 0);
    lv_obj_set_style_border_side(settings, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(settings, Color(0x344154), 0);
    applied_bt_scan_revision_ = bt_scan_revision_.load();
    if (quick_island_open_ && active_quick_menu_ == bt_menu_) {
        AnimateDrop(bt_menu_, true);
        ResizeQuickIsland(bt_menu_);
    }
}

void StatusBar::BuildQuickMenus() {
    wifi_menu_ = CreateQuickMenu(318);
    bt_menu_ = CreateQuickMenu(328);
    RebuildWifiMenu();
    RebuildBluetoothMenu();

    sound_menu_ = CreateQuickMenu(310);
    auto *sound_title = lv_label_create(sound_menu_);
    lv_obj_set_style_text_font(sound_title, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(sound_title, lv_color_white(), 0);
    lv_label_set_text(sound_title, "Âm thanh");
    auto *sound_row = lv_obj_create(sound_menu_);
    lv_obj_remove_style_all(sound_row);
    lv_obj_set_size(sound_row, lv_pct(100), 38);
    lv_obj_set_flex_flow(sound_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sound_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(sound_row, 8, 0);
    lv_obj_set_style_bg_color(sound_row, Color(0x121a29), 0);
    lv_obj_set_style_bg_opa(sound_row, LV_OPA_70, 0);
    lv_obj_set_style_radius(sound_row, 12, 0);
    lv_obj_set_style_pad_left(sound_row, 10, 0);
    lv_obj_set_style_pad_right(sound_row, 10, 0);
    const bool sound_muted = Settings("display").GetBool("muted", false);
    sound_mute_icon_ = jetson::ui::CreateAppIcon(
        sound_row, sound_muted ? "speaker-mute" : "speaker", 20);
    lv_obj_add_flag(sound_mute_icon_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(sound_mute_icon_, 6);
    lv_obj_add_event_cb(sound_mute_icon_, OnMuteClick, LV_EVENT_CLICKED, this);
    sound_slider_ = lv_slider_create(sound_row);
    lv_obj_set_width(sound_slider_, 184);
    lv_obj_set_style_bg_color(sound_slider_, Color(0x323b4a), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sound_slider_, Color(0x0a84ff), LV_PART_INDICATOR);
    lv_slider_set_range(sound_slider_, 0, 100);
    const int volume = Clamp(Settings("display").GetInt("volume", 50), 0, 100);
    lv_slider_set_value(sound_slider_, volume, LV_ANIM_OFF);
    lv_obj_add_event_cb(sound_slider_, OnVolumeChanged, LV_EVENT_VALUE_CHANGED, this);
    sound_value_ = lv_label_create(sound_row);
    lv_obj_set_style_text_font(sound_value_, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(sound_value_, lv_color_white(), 0);
    char volume_text[12];
    std::snprintf(volume_text, sizeof(volume_text), "%d%%", volume);
    lv_label_set_text(sound_value_, volume_text);
    auto *output = lv_label_create(sound_menu_);
    lv_obj_set_style_text_font(output, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(output, Color(0xb8c0cc), 0);
    lv_label_set_text(output, "Đầu ra\n✓  Speaker Jetson");

    brightness_menu_ = CreateQuickMenu(310);
    auto *bright_title = lv_label_create(brightness_menu_);
    lv_obj_set_style_text_font(bright_title, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(bright_title, lv_color_white(), 0);
    lv_label_set_text(bright_title, "Màn hình");
    auto *bright_row = lv_obj_create(brightness_menu_);
    lv_obj_remove_style_all(bright_row);
    lv_obj_set_size(bright_row, lv_pct(100), 38);
    lv_obj_set_flex_flow(bright_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bright_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bright_row, 8, 0);
    lv_obj_set_style_bg_color(bright_row, Color(0x121a29), 0);
    lv_obj_set_style_bg_opa(bright_row, LV_OPA_70, 0);
    lv_obj_set_style_radius(bright_row, 12, 0);
    lv_obj_set_style_pad_left(bright_row, 10, 0);
    lv_obj_set_style_pad_right(bright_row, 10, 0);
    auto *sun = jetson::ui::CreateAppIcon(bright_row, "sun", 20);
    lv_obj_set_style_image_recolor(sun, Color(0xff9f0a), 0);
    lv_obj_set_style_image_recolor_opa(sun, LV_OPA_COVER, 0);
    brightness_slider_ = lv_slider_create(bright_row);
    lv_obj_set_width(brightness_slider_, 184);
    lv_obj_set_style_bg_color(brightness_slider_, Color(0x323b4a), LV_PART_MAIN);
    lv_obj_set_style_bg_color(brightness_slider_, Color(0xff9f0a), LV_PART_INDICATOR);
    lv_slider_set_range(brightness_slider_, 20, 100);
    const int brightness = Clamp(Settings("display").GetInt("brightness", 100), 20, 100);
    lv_slider_set_value(brightness_slider_, brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(brightness_slider_, OnBrightnessChanged,
                        LV_EVENT_VALUE_CHANGED, this);
    brightness_value_ = lv_label_create(bright_row);
    lv_obj_set_style_text_font(brightness_value_, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(brightness_value_, lv_color_white(), 0);
    char brightness_text[12];
    std::snprintf(brightness_text, sizeof(brightness_text), "%d%%", brightness);
    lv_label_set_text(brightness_value_, brightness_text);
}

void StatusBar::StartWifiScan() {
    bool expected = false;
    if (!wifi_scan_busy_.compare_exchange_strong(expected, true)) return;
    if (wifi_scan_thread_.joinable()) wifi_scan_thread_.join();
    wifi_scan_thread_ = std::thread([this]() {
        auto networks = jetson::WifiManager::Instance().Scan();
        if (networks.size() > 4) networks.resize(4);
        {
            std::lock_guard<std::mutex> lock(quick_scan_mutex_);
            quick_wifi_networks_ = std::move(networks);
        }
        wifi_scan_busy_.store(false);
        wifi_scan_revision_.fetch_add(1);
    });
}

void StatusBar::StartBluetoothScan() {
    bool expected = false;
    if (!bt_scan_busy_.compare_exchange_strong(expected, true)) return;
    if (bt_scan_thread_.joinable()) bt_scan_thread_.join();
    bt_scan_thread_ = std::thread([this]() {
        auto devices = jetson::BluetoothManager::Instance().Scan(5);
        {
            std::lock_guard<std::mutex> lock(quick_scan_mutex_);
            quick_bt_devices_ = std::move(devices);
        }
        bt_scan_busy_.store(false);
        bt_scan_revision_.fetch_add(1);
    });
}

void StatusBar::HideQuickMenus(lv_obj_t *except) {
    if (!except && quick_island_open_) {
        CloseQuickIsland(true);
        return;
    }
    for (auto *menu : {wifi_menu_, bt_menu_, sound_menu_, brightness_menu_, power_menu_})
        if (menu && menu != except) lv_obj_add_flag(menu, LV_OBJ_FLAG_HIDDEN);
    if (optimize_widget_ && optimize_widget_->Content() != except)
        optimize_widget_->HidePopup();
}

void StatusBar::ShowQuickMenu(lv_obj_t *menu, lv_obj_t *anchor) {
    (void)anchor;
    if (!menu || !pill_ || !quick_host_) return;
    if (quick_island_open_ && !quick_island_closing_ && active_quick_menu_ == menu) {
        CloseQuickIsland(true);
        return;
    }

    if (!quick_island_open_ && notification_visible_)
        CollapseIsland(false);
    if (notif_timer_) { lv_timer_del(notif_timer_); notif_timer_ = nullptr; }

    HideQuickMenus(menu);
    HideMediaContent();
    media_expanded_open_ = false;
    notification_visible_ = false;
    if (island_content_) lv_obj_add_flag(island_content_, LV_OBJ_FLAG_HIDDEN);
    if (island_rest_) lv_obj_add_flag(island_rest_, LV_OBJ_FLAG_HIDDEN);

    active_quick_menu_ = menu;
    quick_island_open_ = true;
    quick_island_closing_ = false;
    island_expanded_ = true;
    lv_obj_clear_flag(quick_host_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(quick_host_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(menu, LV_OBJ_FLAG_HIDDEN);
    lv_obj_center(menu);
    AnimateDrop(menu, true);
    uint32_t accent = 0x0a84ff;
    if (menu == brightness_menu_) accent = 0xff9f0a;
    else if (menu == power_menu_) accent = 0xff5364;
    else if (optimize_widget_ && menu == optimize_widget_->Content()) accent = 0x00c3d7;
    lv_obj_set_style_border_color(pill_, Color(accent), 0);
    lv_obj_set_style_border_opa(pill_, LV_OPA_50, 0);
    ResizeQuickIsland(menu, true);
    ArmQuickMenuTimer();
}

void StatusBar::ResizeQuickIsland(lv_obj_t *menu, bool animated) {
    if (!menu || !pill_) return;
    lv_obj_update_layout(menu);
    const int target_w = std::min(460, std::max(kPillW, lv_obj_get_width(menu) + 20));
    const int target_h = std::min(350, std::max(kPillH, lv_obj_get_height(menu) + 16));
    lv_obj_center(menu);
    if (animated) {
        AnimateIslandSize(target_w, target_h, false);
    } else {
        lv_obj_set_size(pill_, target_w, target_h);
        lv_obj_align(pill_, LV_ALIGN_TOP_MID, 0, kTopInset);
    }
}

void StatusBar::CloseQuickIsland(bool animated) {
    if (!pill_ || !quick_host_) return;
    if (quick_menu_timer_) { lv_timer_del(quick_menu_timer_); quick_menu_timer_ = nullptr; }

    if (active_quick_menu_) {
        lv_anim_delete(active_quick_menu_, OnDropOpa);
        lv_anim_delete(active_quick_menu_, OnDropY);
        if (animated) AnimateDrop(active_quick_menu_, false);
        else lv_obj_add_flag(active_quick_menu_, LV_OBJ_FLAG_HIDDEN);
    }
    if (!animated) {
        quick_island_open_ = false;
        quick_island_closing_ = false;
        for (auto *menu : {wifi_menu_, bt_menu_, sound_menu_, brightness_menu_, power_menu_})
            if (menu) lv_obj_add_flag(menu, LV_OBJ_FLAG_HIDDEN);
        if (optimize_widget_) optimize_widget_->HidePopup();
        lv_obj_set_style_opa(quick_host_, LV_OPA_0, 0);
        lv_obj_add_flag(quick_host_, LV_OBJ_FLAG_HIDDEN);
        active_quick_menu_ = nullptr;
        island_expanded_ = false;
        if (media_available_) {
            RefreshMedia(true);
        } else {
            lv_anim_delete(pill_, OnIslandWidth);
            lv_anim_delete(pill_, OnIslandHeight);
            lv_obj_set_size(pill_, kPillW, kPillH);
            lv_obj_align(pill_, LV_ALIGN_TOP_MID, 0, kTopInset);
            lv_obj_set_style_border_color(pill_, Color(0x253142), 0);
            SyncIslandRest();
        }
        return;
    }

    quick_island_closing_ = true;
    AnimateIslandSize(media_available_ ? kMediaCompactW : kPillW,
                      media_available_ ? kMediaCompactH : kPillH, true);
}

void StatusBar::ArmQuickMenuTimer() {
    if (quick_menu_timer_) { lv_timer_del(quick_menu_timer_); quick_menu_timer_ = nullptr; }
    quick_menu_timer_ = lv_timer_create(OnQuickMenuTimer, kAutoCloseMs, this);
    lv_timer_set_repeat_count(quick_menu_timer_, 1);
}

void StatusBar::BuildPowerMenu() {
    power_menu_ = CreateQuickMenu(250);
    lv_obj_add_event_cb(power_menu_, OnPowerMenuDeleted, LV_EVENT_DELETE, this);

    auto add_item = [&](const char *label, lv_event_cb_t cb, bool destructive) {
        auto *row = lv_obj_create(power_menu_);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, 40);
        lv_obj_set_style_radius(row, 11, 0);
        lv_obj_set_style_bg_color(row, Color(destructive ? 0x35151b : 0x121a29), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_80, 0);
        lv_obj_set_style_bg_color(row, Color(destructive ? 0x67202d : 0x17365f),
                                  LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        auto *lbl = lv_label_create(row);
        lv_obj_set_style_text_font(lbl, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(lbl,
            destructive ? Color(0xff6b7d) : lv_color_white(), 0);
        lv_label_set_text(lbl, label);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 12, 0);
        lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, this);
    };
    add_item("Ngủ", OnPowerLock, false);
    add_item("Khởi động lại", OnPowerReboot, false);
    add_item("Tắt máy", OnPowerShutdown, true);
}

StatusBar::~StatusBar() {
    // Stop the poller before touching LVGL state. The worker never takes the
    // LVGL lock, so joining here (worst case one in-flight nmcli/bluetoothctl
    // call) cannot deadlock.
    conn_poll_stop_.store(true);
    if (conn_poll_thread_.joinable()) conn_poll_thread_.join();
    if (wifi_scan_thread_.joinable()) wifi_scan_thread_.join();
    if (bt_scan_thread_.joinable()) bt_scan_thread_.join();
    // OptimizeWidget joins its cache worker before taking the LVGL lock. Do
    // that before this destructor locks LVGL, otherwise a worker finishing its
    // notification could wait on the same lock while we wait on its join.
    optimize_widget_.reset();

    LvglLockGuard lock;
    if (timer_) { lv_timer_del(timer_); timer_ = nullptr; }
    if (notif_timer_) { lv_timer_del(notif_timer_); notif_timer_ = nullptr; }
    if (power_menu_timer_) { lv_timer_del(power_menu_timer_); power_menu_timer_ = nullptr; }
    if (quick_menu_timer_) { lv_timer_del(quick_menu_timer_); quick_menu_timer_ = nullptr; }
    // Remove quick-menu children explicitly so their row contexts/callbacks
    // disappear before the shared island host is destroyed.
    for (lv_obj_t **menu : {&wifi_menu_, &bt_menu_, &sound_menu_,
                            &brightness_menu_, &power_menu_}) {
        if (*menu) { lv_obj_del(*menu); *menu = nullptr; }
    }
    if (pill_) { lv_obj_del(pill_); pill_ = nullptr; }
    if (status_strip_) { lv_obj_del(status_strip_); status_strip_ = nullptr; }
    if (media_artwork_) {
        lv_image_cache_drop(media_artwork_->image_dsc());
        media_artwork_.reset();
    }
}

void StatusBar::Hide() {
    visible_ = false;
    if (notif_timer_) { lv_timer_del(notif_timer_); notif_timer_ = nullptr; }
    if (quick_island_open_) CloseQuickIsland(false);
    if (pill_) {
        lv_anim_delete(pill_, OnIslandWidth);
        lv_anim_delete(pill_, OnIslandHeight);
        lv_obj_set_size(pill_, kPillW, kPillH);
        lv_obj_align(pill_, LV_ALIGN_TOP_MID, 0, kTopInset);
        lv_obj_add_flag(pill_, LV_OBJ_FLAG_HIDDEN);
    }
    if (island_content_) {
        lv_anim_delete(island_content_, OnIslandContentOpa);
        lv_obj_set_style_opa(island_content_, LV_OPA_0, 0);
        lv_obj_add_flag(island_content_, LV_OBJ_FLAG_HIDDEN);
    }
    HideMediaContent();
    if (status_strip_) lv_obj_add_flag(status_strip_, LV_OBJ_FLAG_HIDDEN);
    island_expanded_ = false;
    notification_visible_ = false;
    media_expanded_open_ = false;
    SyncIslandRest();
}

void StatusBar::Show() {
    visible_ = true;
    if (status_strip_) lv_obj_clear_flag(status_strip_, LV_OBJ_FLAG_HIDDEN);
    if (pill_) lv_obj_clear_flag(pill_, LV_OBJ_FLAG_HIDDEN);
    RefreshMedia(true);
}

void StatusBar::Refresh() {
    RefreshClock();
    RefreshConnectivity();
    if (sound_icon_) {
        const bool muted = Settings("display").GetBool("muted", false);
        jetson::ui::SetAppIcon(sound_icon_, muted ? "speaker-mute" : "speaker",
                               kStatusIconPx);
    }
    if (wifi_scan_revision_.load() != applied_wifi_scan_revision_ && wifi_menu_)
        RebuildWifiMenu();
    if (bt_scan_revision_.load() != applied_bt_scan_revision_ && bt_menu_)
        RebuildBluetoothMenu();
    RefreshMedia();
}

void StatusBar::RefreshConnectivity() {
    const bool airplane = jetson::IsAirplaneModeEnabled();
    const bool vpn = jetson::VpnManager::Instance().CachedEnabled();
    const int wifi_signal = polled_wifi_signal_.load();
    const int wifi_enabled = polled_wifi_enabled_.load();
    const int eth_connected = polled_eth_connected_.load();
    const int bt_powered = polled_bt_powered_.load();
    const int bt_device_polled = polled_bt_device_.load();
    if (airplane_state_read_ && airplane == cached_airplane_mode_ &&
        vpn_state_read_ && vpn == cached_vpn_enabled_ &&
        wifi_signal == cached_wifi_signal_ &&
        wifi_enabled == cached_wifi_enabled_ &&
        eth_connected == cached_eth_connected_ &&
        bt_powered == cached_bt_powered_ &&
        bt_device_polled == cached_bt_device_)
        return;
    airplane_state_read_ = true;
    cached_airplane_mode_ = airplane;
    vpn_state_read_ = true;
    cached_vpn_enabled_ = vpn;
    cached_wifi_signal_ = wifi_signal;
    cached_wifi_enabled_ = wifi_enabled;
    cached_eth_connected_ = eth_connected;
    cached_bt_powered_ = bt_powered;
    cached_bt_device_ = bt_device_polled;

    // The six quick settings never disappear or reorder. Radio-off state is
    // represented by the crossed-out asset, so the click target remains where
    // the user expects it.
    jetson::ui::SetAppIcon(wifi_icon_,
        (airplane || wifi_enabled == 0 || wifi_signal == -1) ? "no-wifi" : "wifi",
        kStatusIconPx);
    const int bt_device = airplane
        ? static_cast<int>(jetson::BtDeviceKind::None) : cached_bt_device_;
    {
        jetson::ui::SetAppIcon(bt_icon_,
                               (airplane || bt_powered == 0) ? "no-bluetooh" : "bluetooth",
                               kStatusIconPx);
        // Powered and idle is not the same state as powered with a device on
        // it, and the glyph alone cannot tell them apart -- tint the icon blue
        // once something is actually connected.
        if (bt_icon_)
            lv_obj_set_style_image_recolor(
                bt_icon_, bt_device > 0 ? Color(0x0a84ff) : lv_color_white(), 0);
    }

    // Resting island: device-type mini icon + status ring. The ring turns
    // green while any Bluetooth device is connected; without one the icon
    // hides and the ring dims (-1 = not polled yet behaves like "none").
    if (island_device_icon_) {
        const auto kind = static_cast<jetson::BtDeviceKind>(
            bt_device > 0 ? bt_device
                          : static_cast<int>(jetson::BtDeviceKind::None));
        if (kind == jetson::BtDeviceKind::None) {
            lv_obj_add_flag(island_device_icon_, LV_OBJ_FLAG_HIDDEN);
        } else {
            jetson::ui::SetAppIcon(island_device_icon_,
                                   jetson::BtKindIconName(kind), 18);
            lv_obj_clear_flag(island_device_icon_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (island_ring_)
        lv_obj_set_style_border_color(
            island_ring_, Color(bt_device > 0 ? 0x30d158 : 0x3a3a3c), 0);
}

void StatusBar::RefreshClock() {
    time_t now = std::time(nullptr);
    struct tm t = *std::localtime(&now);
    if (t.tm_year < (2025 - 1900)) return; // time not set yet
    bool h24 = Settings("display").GetBool("clock_24h", true);
    const std::string region = Settings("system").GetString("region", "VN");
    const char *date = (region == "US") ? "%m/%d" :
                       (region == "JP" || region == "CN") ? "%Y/%m/%d" : "%d/%m";
    std::string format = h24 ? "%H:%M  " : "%I:%M  ";
    format += date;
    char ts[32];
    std::strftime(ts, sizeof(ts), format.c_str(), &t);
    std::string s(ts);
    if (s != cached_datetime_ && datetime_label_) {
        lv_label_set_text(datetime_label_, s.c_str());
        cached_datetime_ = s;
    }
}

void StatusBar::RefreshBattery() {
    constexpr auto kBatteryReadInterval = std::chrono::seconds(5);
    auto now = std::chrono::steady_clock::now();
    if (!battery_read_done_ || (now - last_battery_read_ >= kBatteryReadInterval)) {
        int level = 100;
        bool charging = false, discharging = false;
        has_battery_ = Board::GetInstance().GetBatteryLevel(level, charging, discharging);
        cached_battery_level_ = Clamp(level, 0, 100);
        last_battery_read_ = now;
        battery_read_done_ = true;
        cached_battery_charging_ = charging;
        (void)discharging;
    }

    // Keep a percentage-style battery visible even on installations without a
    // readable battery sensor; those retain the neutral 100% fallback.
    const int level = has_battery_ ? cached_battery_level_ : 100;
    if (battery_icon_fill_) {
        if (level <= 0) {
            lv_obj_add_flag(battery_icon_fill_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(battery_icon_fill_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(battery_icon_fill_, std::max(1, 41 * level / 100), 16);
        }
        // Colour tracks the charge only, never the plug: a pack sitting at 30%
        // is still low while it charges, so it stays yellow and only turns
        // green past 70.
        const uint32_t fill_color = level >= 70
                                        ? 0x34c759
                                        : (level > 20 ? 0xffcc00 : 0xff3b30);
        lv_obj_set_style_bg_color(battery_icon_fill_, Color(fill_color), 0);
        lv_obj_set_style_border_color(battery_icon_body_, Color(fill_color), 0);
        if (battery_icon_nub_)
            lv_obj_set_style_bg_color(battery_icon_nub_, Color(fill_color), 0);
    }
    if (battery_percent_label_) {
        char buf[16];
        if (has_battery_ && cached_battery_charging_)
            std::snprintf(buf, sizeof(buf), LV_SYMBOL_CHARGE "%d", level);
        else
            std::snprintf(buf, sizeof(buf), "%d", level);
        lv_label_set_text(battery_percent_label_, buf);
        lv_obj_set_style_text_color(battery_percent_label_, lv_color_white(), 0);
    }
    if (has_battery_) {
        if (level <= 20 && !low_warned_) {
            low_warned_ = true;
            ShowNotification("Pin yếu — hãy sạc thiết bị");
        } else if (level > 25) {
            low_warned_ = false;
        }
    }
}

void StatusBar::RefreshLang() {
    std::string lang = Settings("input").GetString("kbd_lang", "en");
    std::string disp = (lang == "vi") ? "VI" : "EN";
    if (disp != cached_lang_ && lang_label_) {
        lv_label_set_text(lang_label_, disp.c_str());
        cached_lang_ = disp;
    }
}

void StatusBar::AnimateIslandSize(int width, int height, bool collapsing) {
    if (!pill_) return;
    lv_obj_update_layout(pill_);
    const int from_w = lv_obj_get_width(pill_);
    const int from_h = lv_obj_get_height(pill_);
    lv_anim_delete(pill_, OnIslandWidth);
    lv_anim_delete(pill_, OnIslandHeight);

    lv_anim_t w;
    lv_anim_init(&w);
    lv_anim_set_var(&w, pill_);
    lv_anim_set_exec_cb(&w, OnIslandWidth);
    lv_anim_set_values(&w, from_w, width);
    lv_anim_set_time(&w, collapsing ? 320 : 360);
    lv_anim_set_delay(&w, 0);
    lv_anim_set_path_cb(&w, lv_anim_path_ease_in_out);
    lv_anim_start(&w);

    lv_anim_t h;
    lv_anim_init(&h);
    lv_anim_set_var(&h, pill_);
    lv_anim_set_exec_cb(&h, OnIslandHeight);
    lv_anim_set_values(&h, from_h, height);
    lv_anim_set_time(&h, collapsing ? 320 : 360);
    lv_anim_set_delay(&h, 0);
    lv_anim_set_path_cb(&h, lv_anim_path_ease_in_out);
    if (collapsing) {
        lv_anim_set_completed_cb(&h, OnIslandCollapsed);
        lv_anim_set_user_data(&h, this);
    }
    lv_anim_start(&h);
}

void StatusBar::ShowIslandMessage(const char *title, const char *text,
                                  const char *icon, uint32_t accent,
                                  int duration_ms) {
    if (!visible_ || !pill_ || !island_content_) return;
    if (quick_island_open_) CloseQuickIsland(false);

    if (notif_timer_) { lv_timer_del(notif_timer_); notif_timer_ = nullptr; }
    lv_label_set_text(island_title_, title ? title : "");
    lv_label_set_text(island_message_, text ? text : "");
    lv_label_set_text(island_icon_, icon ? icon : LV_SYMBOL_BELL);
    lv_obj_set_style_bg_color(island_icon_bg_, Color(accent), 0);
    lv_obj_set_style_text_color(island_title_, Color(accent), 0);
    lv_obj_set_style_border_color(pill_, Color(accent), 0);
    lv_obj_set_style_border_opa(pill_, LV_OPA_40, 0);

    HideMediaContent();
    media_expanded_open_ = false;
    notification_visible_ = true;
    SyncIslandRest();
    lv_obj_clear_flag(island_content_, LV_OBJ_FLAG_HIDDEN);
    lv_anim_delete(island_content_, OnIslandContentOpa);
    lv_obj_set_style_opa(island_content_, LV_OPA_0, 0);
    AnimateIslandSize(kExpandedW, kExpandedH, false);

    lv_anim_t content;
    lv_anim_init(&content);
    lv_anim_set_var(&content, island_content_);
    lv_anim_set_exec_cb(&content, OnIslandContentOpa);
    lv_anim_set_values(&content, 0, 255);
    lv_anim_set_delay(&content, 140);
    lv_anim_set_time(&content, 220);
    lv_anim_set_path_cb(&content, lv_anim_path_ease_in_out);
    lv_anim_start(&content);

    island_expanded_ = true;
    const int ms = std::max(1200, duration_ms);
    notif_timer_ = lv_timer_create(OnNotifTimer, ms, this);
    lv_timer_set_repeat_count(notif_timer_, 1);
}

void StatusBar::CollapseIsland(bool animated) {
    if (!pill_ || !island_content_) return;
    if (notif_timer_) { lv_timer_del(notif_timer_); notif_timer_ = nullptr; }

    lv_anim_delete(island_content_, OnIslandContentOpa);
    if (!animated) {
        lv_anim_delete(pill_, OnIslandWidth);
        lv_anim_delete(pill_, OnIslandHeight);
        lv_obj_set_size(pill_, kPillW, kPillH);
        lv_obj_align(pill_, LV_ALIGN_TOP_MID, 0, kTopInset);
        lv_obj_set_style_opa(island_content_, LV_OPA_0, 0);
        lv_obj_add_flag(island_content_, LV_OBJ_FLAG_HIDDEN);
        HideMediaContent();
        lv_obj_set_style_border_color(pill_, Color(0x253142), 0);
        island_expanded_ = false;
        notification_visible_ = false;
        media_expanded_open_ = false;
        SyncIslandRest();
        return;
    }
    if (!island_expanded_ && !notification_visible_) return;

    lv_anim_t content;
    lv_anim_init(&content);
    lv_anim_set_var(&content, island_content_);
    lv_anim_set_exec_cb(&content, OnIslandContentOpa);
    lv_anim_set_values(&content, lv_obj_get_style_opa(island_content_, 0), 0);
    lv_anim_set_time(&content, 180);
    lv_anim_set_path_cb(&content, lv_anim_path_ease_in);
    lv_anim_start(&content);
    media_expanded_open_ = false;
    AnimateIslandSize(media_available_ ? kMediaCompactW : kPillW,
                      media_available_ ? kMediaCompactH : kPillH, true);
}

void StatusBar::AnimateDrop(lv_obj_t *obj, bool show) {
    if (!obj) return;
    lv_anim_delete(obj, OnDropOpa);
    lv_anim_delete(obj, OnDropY);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(obj, show ? LV_OPA_0 : LV_OPA_COVER, 0);
    lv_obj_set_style_translate_y(obj, show ? -10 : 0, 0);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, OnDropOpa);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_time(&a, 220);
    lv_anim_set_delay(&a, show ? 80 : 0);
    lv_anim_set_values(&a, show ? 0 : 255, show ? 255 : 0);
    lv_anim_start(&a);

    lv_anim_t b;
    lv_anim_init(&b);
    lv_anim_set_var(&b, obj);
    lv_anim_set_exec_cb(&b, OnDropY);
    lv_anim_set_path_cb(&b, lv_anim_path_ease_out);
    lv_anim_set_time(&b, 220);
    lv_anim_set_delay(&b, show ? 80 : 0);
    lv_anim_set_values(&b, show ? -10 : 0, show ? 0 : -10);
    if (!show) lv_anim_set_completed_cb(&b, OnDropHidden);
    lv_anim_start(&b);
}

void StatusBar::ShowNotification(const char *text, int duration_ms) {
    ShowIslandMessage("THÔNG BÁO", text, LV_SYMBOL_BELL, 0x69b7ff,
                      duration_ms);
}

void StatusBar::ShowWelcome(int duration_ms) {
    ShowIslandMessage("XIN CHÀO", "Chào mừng bạn đến với Ekko Land",
                      LV_SYMBOL_HOME, 0x62e6a7, duration_ms);
}

void StatusBar::ShowPowerMenu() {
    if (!power_menu_) return;
    ShowQuickMenu(power_menu_, power_icon_);
}

void StatusBar::HidePowerMenu() {
    if (power_menu_timer_) { lv_timer_del(power_menu_timer_); power_menu_timer_ = nullptr; }
    if (quick_island_open_ && active_quick_menu_ == power_menu_)
        CloseQuickIsland(true);
    else if (power_menu_)
        lv_obj_add_flag(power_menu_, LV_OBJ_FLAG_HIDDEN);
}

void StatusBar::OnTimer(lv_timer_t *t) {
    auto *self = static_cast<StatusBar *>(lv_timer_get_user_data(t));
    LvglLockGuard lock;
    self->Refresh();
}

void StatusBar::OnNotifTimer(lv_timer_t *t) {
    auto *self = static_cast<StatusBar *>(lv_timer_get_user_data(t));
    LvglLockGuard lock;
    self->notif_timer_ = nullptr;
    lv_timer_del(t);
    self->CollapseIsland(true);
}

void StatusBar::OnPowerMenuTimer(lv_timer_t *t) {
    auto *self = static_cast<StatusBar *>(lv_timer_get_user_data(t));
    LvglLockGuard lock;
    self->power_menu_timer_ = nullptr;
    lv_timer_del(t);
    self->HidePowerMenu();
}

void StatusBar::OnQuickMenuTimer(lv_timer_t *t) {
    auto *self = static_cast<StatusBar *>(lv_timer_get_user_data(t));
    LvglLockGuard lock;
    self->quick_menu_timer_ = nullptr;
    lv_timer_del(t);
    self->HideQuickMenus();
}

void StatusBar::OnDeleted(lv_event_t *e) {
    auto *self = static_cast<StatusBar *>(lv_event_get_user_data(e));
    if (!self) return;
    if (self->timer_) { lv_timer_del(self->timer_); self->timer_ = nullptr; }
    if (self->notif_timer_) { lv_timer_del(self->notif_timer_); self->notif_timer_ = nullptr; }
    if (self->power_menu_timer_) { lv_timer_del(self->power_menu_timer_); self->power_menu_timer_ = nullptr; }
    if (self->quick_menu_timer_) { lv_timer_del(self->quick_menu_timer_); self->quick_menu_timer_ = nullptr; }
    self->pill_ = nullptr;
}

void StatusBar::OnPowerMenuDeleted(lv_event_t *e) {
    auto *self = static_cast<StatusBar *>(lv_event_get_user_data(e));
    if (self) self->power_menu_ = nullptr;
}

void StatusBar::OnWifiClick(lv_event_t *e) {
    auto *self = static_cast<StatusBar *>(lv_event_get_user_data(e));
    LvglLockGuard lock;
    self->StartWifiScan();
    self->RebuildWifiMenu();
    self->ShowQuickMenu(self->wifi_menu_, self->wifi_icon_);
}

void StatusBar::OnSoundClick(lv_event_t *e) {
    auto *self = static_cast<StatusBar *>(lv_event_get_user_data(e));
    LvglLockGuard lock;
    const int volume = Clamp(Settings("display").GetInt("volume", 50), 0, 100);
    const bool muted = Settings("display").GetBool("muted", false);
    self->suppress_quick_events_ = true;
    if (self->sound_slider_) lv_slider_set_value(self->sound_slider_, volume, LV_ANIM_OFF);
    self->suppress_quick_events_ = false;
    if (self->sound_mute_icon_)
        jetson::ui::SetAppIcon(self->sound_mute_icon_, muted ? "speaker-mute" : "speaker", 20);
    if (self->sound_value_) {
        char text[12];
        std::snprintf(text, sizeof(text), "%d%%", volume);
        lv_label_set_text(self->sound_value_, text);
    }
    self->ShowQuickMenu(self->sound_menu_, self->sound_icon_);
}

void StatusBar::OnBrightnessClick(lv_event_t *e) {
    auto *self = static_cast<StatusBar *>(lv_event_get_user_data(e));
    LvglLockGuard lock;
    const int brightness = Clamp(Settings("display").GetInt("brightness", 100), 20, 100);
    self->suppress_quick_events_ = true;
    if (self->brightness_slider_)
        lv_slider_set_value(self->brightness_slider_, brightness, LV_ANIM_OFF);
    self->suppress_quick_events_ = false;
    if (self->brightness_value_) {
        char text[12];
        std::snprintf(text, sizeof(text), "%d%%", brightness);
        lv_label_set_text(self->brightness_value_, text);
    }
    self->ShowQuickMenu(self->brightness_menu_, self->brightness_icon_);
}

void StatusBar::OnWifiToggle(lv_event_t *e) {
    LvglLockGuard lock;
    auto *self = static_cast<StatusBar *>(lv_event_get_user_data(e));
    const bool enabled = lv_obj_has_state(
        static_cast<lv_obj_t *>(lv_event_get_target(e)), LV_STATE_CHECKED);
    std::thread([self, enabled]() {
        const bool changed = jetson::WifiManager::Instance().Enable(enabled);
        if (enabled && changed) self->StartWifiScan();
    }).detach();
    self->polled_wifi_enabled_.store(enabled ? 1 : 0);
    self->ArmQuickMenuTimer();
}

void StatusBar::OnBluetoothToggle(lv_event_t *e) {
    LvglLockGuard lock;
    auto *self = static_cast<StatusBar *>(lv_event_get_user_data(e));
    const bool enabled = lv_obj_has_state(
        static_cast<lv_obj_t *>(lv_event_get_target(e)), LV_STATE_CHECKED);
    std::thread([self, enabled]() {
        auto &bt = jetson::BluetoothManager::Instance();
        if (enabled) {
            if (bt.PowerOn()) self->StartBluetoothScan();
        } else {
            (void)bt.PowerOff();
        }
    }).detach();
    self->polled_bt_powered_.store(enabled ? 1 : 0);
    self->ArmQuickMenuTimer();
}

void StatusBar::OnWifiRow(lv_event_t *e) {
    LvglLockGuard lock;
    auto *ctx = static_cast<QuickRowContext *>(lv_event_get_user_data(e));
    if (!ctx || !ctx->self) return;
    auto *self = ctx->self;
    const std::string ssid = ctx->id;
    const bool active = ctx->active;
    if (!active && ctx->secured && !ctx->known) {
        self->HideQuickMenus();
        // Password entry belongs to the full Wi-Fi sheet; the compact list
        // still handles saved/open networks directly.
        if (self->wifi_action_) self->wifi_action_();
        return;
    }
    self->HideQuickMenus();
    std::thread([ssid, active]() {
        auto &wifi = jetson::WifiManager::Instance();
        if (active) (void)wifi.Disconnect(); else (void)wifi.Connect(ssid, "");
    }).detach();
}

void StatusBar::OnBluetoothRow(lv_event_t *e) {
    LvglLockGuard lock;
    auto *ctx = static_cast<QuickRowContext *>(lv_event_get_user_data(e));
    if (!ctx || !ctx->self) return;
    const std::string address = ctx->id;
    const bool connected = ctx->active;
    ctx->self->HideQuickMenus();
    std::thread([address, connected]() {
        auto &bt = jetson::BluetoothManager::Instance();
        if (connected) (void)bt.Disconnect(address);
        else (void)bt.PairAndConnect(address);
    }).detach();
}

void StatusBar::OnWifiSettings(lv_event_t *e) {
    LvglLockGuard lock;
    auto *self = static_cast<StatusBar *>(lv_event_get_user_data(e));
    self->HideQuickMenus();
    if (self->wifi_action_) self->wifi_action_();
}

void StatusBar::OnBluetoothSettings(lv_event_t *e) {
    LvglLockGuard lock;
    auto *self = static_cast<StatusBar *>(lv_event_get_user_data(e));
    self->HideQuickMenus();
    if (self->bt_action_) self->bt_action_();
}

void StatusBar::OnVolumeChanged(lv_event_t *e) {
    LvglLockGuard lock;
    auto *self = static_cast<StatusBar *>(lv_event_get_user_data(e));
    if (self->suppress_quick_events_) return;
    const int value = lv_slider_get_value(self->sound_slider_);
    const bool muted = value == 0;
    Settings settings("display", true);
    settings.SetInt("volume", value);
    settings.SetBool("muted", muted);
    if (self->sound_value_) {
        char text[12];
        std::snprintf(text, sizeof(text), "%d%%", value);
        lv_label_set_text(self->sound_value_, text);
    }
    if (self->sound_mute_icon_)
        jetson::ui::SetAppIcon(self->sound_mute_icon_, muted ? "speaker-mute" : "speaker", 20);
    if (self->volume_action_) self->volume_action_(value, muted);
    self->ArmQuickMenuTimer();
}

void StatusBar::OnMuteClick(lv_event_t *e) {
    LvglLockGuard lock;
    auto *self = static_cast<StatusBar *>(lv_event_get_user_data(e));
    Settings settings("display", true);
    const bool muted = !settings.GetBool("muted", false);
    settings.SetBool("muted", muted);
    const int volume = Clamp(settings.GetInt("volume", 50), 0, 100);
    jetson::ui::SetAppIcon(self->sound_mute_icon_, muted ? "speaker-mute" : "speaker", 20);
    if (self->volume_action_) self->volume_action_(volume, muted);
    self->ArmQuickMenuTimer();
}

void StatusBar::OnBrightnessChanged(lv_event_t *e) {
    LvglLockGuard lock;
    auto *self = static_cast<StatusBar *>(lv_event_get_user_data(e));
    if (self->suppress_quick_events_) return;
    const int value = lv_slider_get_value(self->brightness_slider_);
    Settings("display", true).SetInt("brightness", value);
    if (self->brightness_value_) {
        char text[12];
        std::snprintf(text, sizeof(text), "%d%%", value);
        lv_label_set_text(self->brightness_value_, text);
    }
    if (self->brightness_action_) self->brightness_action_(value);
    self->ArmQuickMenuTimer();
}

void StatusBar::OnIslandClick(lv_event_t *e) {
    auto *self = static_cast<StatusBar *>(lv_event_get_user_data(e));
    LvglLockGuard lock;
    if (self->suppress_island_click_) {
        self->suppress_island_click_ = false;
        return;
    }
    if (self->quick_island_open_) {
        self->CloseQuickIsland(true);
        return;
    }
    const auto snapshot = jetson::music::PlayerController::Instance().Snapshot();
    if (snapshot.has_current &&
        snapshot.status != jetson::music::PlaybackStatus::Idle) {
        if (self->notif_timer_) {
            lv_timer_del(self->notif_timer_);
            self->notif_timer_ = nullptr;
        }
        if (self->island_content_) {
            lv_anim_delete(self->island_content_, OnIslandContentOpa);
            lv_obj_set_style_opa(self->island_content_, LV_OPA_0, 0);
            lv_obj_add_flag(self->island_content_, LV_OBJ_FLAG_HIDDEN);
        }
        self->notification_visible_ = false;
        self->media_expanded_open_ = !self->media_expanded_open_;
        self->RefreshMedia(true);
        return;
    }
    if (self->island_action_) self->island_action_();
}

void StatusBar::OnIslandLongPress(lv_event_t *e) {
    auto *self = static_cast<StatusBar *>(lv_event_get_user_data(e));
    LvglLockGuard lock;
    self->suppress_island_click_ = true;
    if (self->quick_island_open_) self->CloseQuickIsland(false);
    self->media_expanded_open_ = false;
    self->RefreshMedia(true);
    if (self->island_action_) self->island_action_();
}

void StatusBar::OnMediaPrevious(lv_event_t *e) {
    jetson::music::PlayerController::Instance().Previous();
}

void StatusBar::OnMediaToggle(lv_event_t *e) {
    jetson::music::PlayerController::Instance().Toggle();
}

void StatusBar::OnMediaNext(lv_event_t *e) {
    jetson::music::PlayerController::Instance().Next();
}

int StatusBar::IslandCenterX() const {
    if (!pill_) return 0;
    lv_obj_update_layout(pill_);
    lv_area_t a;
    lv_obj_get_coords(pill_, &a);
    return (a.x1 + a.x2) / 2;
}

int StatusBar::IslandCenterY() const {
    if (!pill_) return 0;
    lv_obj_update_layout(pill_);
    lv_area_t a;
    lv_obj_get_coords(pill_, &a);
    return (a.y1 + a.y2) / 2;
}

void StatusBar::OnBtClick(lv_event_t *e) {
    auto *self = static_cast<StatusBar *>(lv_event_get_user_data(e));
    LvglLockGuard lock;
    if (self->polled_bt_powered_.load() == 1) self->StartBluetoothScan();
    self->RebuildBluetoothMenu();
    self->ShowQuickMenu(self->bt_menu_, self->bt_icon_);
}

void StatusBar::OnPowerClick(lv_event_t *e) {
    auto *self = static_cast<StatusBar *>(lv_event_get_user_data(e));
    LvglLockGuard lock;
    // Toggle: open if closed, close if open.
    if (self->power_menu_ && !lv_obj_has_flag(self->power_menu_, LV_OBJ_FLAG_HIDDEN))
        self->HidePowerMenu();
    else
        self->ShowPowerMenu();
}

void StatusBar::OnPowerLock(lv_event_t *e) {
    auto *self = static_cast<StatusBar *>(lv_event_get_user_data(e));
    LvglLockGuard lock;
    self->HidePowerMenu();
    if (self->sleep_action_) self->sleep_action_();
    else if (self->lock_action_) self->lock_action_();
}

void StatusBar::OnPowerReboot(lv_event_t *e) {
    auto *self = static_cast<StatusBar *>(lv_event_get_user_data(e));
    LvglLockGuard lock;
    self->HidePowerMenu();
    if (self->reboot_action_) self->reboot_action_();
}

void StatusBar::OnPowerShutdown(lv_event_t *e) {
    auto *self = static_cast<StatusBar *>(lv_event_get_user_data(e));
    LvglLockGuard lock;
    self->HidePowerMenu();
    if (self->shutdown_action_) self->shutdown_action_();
}

void StatusBar::OnIslandWidth(void *var, int32_t v) {
    auto *obj = static_cast<lv_obj_t *>(var);
    lv_obj_set_width(obj, v);
    lv_obj_align(obj, LV_ALIGN_TOP_MID, 0, kTopInset);
}

void StatusBar::OnIslandHeight(void *var, int32_t v) {
    auto *obj = static_cast<lv_obj_t *>(var);
    lv_obj_set_height(obj, v);
    lv_obj_align(obj, LV_ALIGN_TOP_MID, 0, kTopInset);
}

void StatusBar::OnIslandContentOpa(void *var, int32_t v) {
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(var), (lv_opa_t)v, 0);
}

void StatusBar::OnIslandCollapsed(lv_anim_t *a) {
    auto *self = static_cast<StatusBar *>(lv_anim_get_user_data(a));
    if (!self || !self->pill_) return;
    if (self->island_content_) {
        lv_obj_set_style_opa(self->island_content_, LV_OPA_0, 0);
        lv_obj_add_flag(self->island_content_, LV_OBJ_FLAG_HIDDEN);
    }
    for (auto *menu : {self->wifi_menu_, self->bt_menu_, self->sound_menu_,
                       self->brightness_menu_, self->power_menu_}) {
        if (menu) lv_obj_add_flag(menu, LV_OBJ_FLAG_HIDDEN);
    }
    if (self->optimize_widget_) self->optimize_widget_->HidePopup();
    if (self->quick_host_) {
        lv_obj_set_style_opa(self->quick_host_, LV_OPA_0, 0);
        lv_obj_add_flag(self->quick_host_, LV_OBJ_FLAG_HIDDEN);
    }
    self->active_quick_menu_ = nullptr;
    self->quick_island_open_ = false;
    self->quick_island_closing_ = false;
    self->notification_visible_ = false;
    self->media_expanded_open_ = false;
    self->island_expanded_ = false;
    self->SyncIslandRest();
    if (self->media_available_) {
        self->RefreshMedia(true);
    } else {
        lv_obj_set_size(self->pill_, kPillW, kPillH);
        lv_obj_align(self->pill_, LV_ALIGN_TOP_MID, 0, kTopInset);
        lv_obj_set_style_border_color(self->pill_, Color(0x253142), 0);
    }
}

void StatusBar::OnDropOpa(void *var, int32_t v) {
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(var), (lv_opa_t)v, 0);
}

void StatusBar::OnDropY(void *var, int32_t v) {
    lv_obj_set_style_translate_y(static_cast<lv_obj_t *>(var), v, 0);
}

void StatusBar::OnDropHidden(lv_anim_t *a) {
    // lv_anim_t::var is a public field (there is no lv_anim_get_var accessor).
    if (a && a->var) lv_obj_add_flag(static_cast<lv_obj_t *>(a->var), LV_OBJ_FLAG_HIDDEN);
}

} // namespace home
