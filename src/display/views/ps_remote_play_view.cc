#include "display/views/ps_remote_play_view.h"

#include "display/common/lvgl_utils.h"
#include "display/core/app_icons.h"
#include "display/theme/ui_theme.h"
#include "esp_log.h"
#include "fonts.h"
#include "input/gamepad_device.h"
#include "lvgl_runtime.h"
#include "settings.h"

#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace home {

using jetson::ui::Color;
using jetson::ui::LvglLockGuard;

struct ControllerTestSession {
    jetson::input::GamepadDevice device;
    jetson::input::GamepadSnapshot state;

    lv_obj_t *card = nullptr;
    lv_obj_t *device_label = nullptr;
    lv_obj_t *last_input_label = nullptr;

    lv_obj_t *dpad_up = nullptr;
    lv_obj_t *dpad_down = nullptr;
    lv_obj_t *dpad_left = nullptr;
    lv_obj_t *dpad_right = nullptr;
    lv_obj_t *cross = nullptr;
    lv_obj_t *circle = nullptr;
    lv_obj_t *triangle = nullptr;
    lv_obj_t *square = nullptr;
    lv_obj_t *l1 = nullptr;
    lv_obj_t *l2 = nullptr;
    lv_obj_t *r1 = nullptr;
    lv_obj_t *r2 = nullptr;
    lv_obj_t *l2_fill = nullptr;
    lv_obj_t *r2_fill = nullptr;
    lv_obj_t *l3 = nullptr;
    lv_obj_t *r3 = nullptr;
    lv_obj_t *left_knob = nullptr;
    lv_obj_t *right_knob = nullptr;
    lv_obj_t *create = nullptr;
    lv_obj_t *options = nullptr;
    lv_obj_t *ps = nullptr;
    lv_obj_t *touchpad = nullptr;
};

namespace {

constexpr uint32_t kBlue = 0x1677ff;
constexpr uint32_t kGreen = 0x30d158;
constexpr uint32_t kOrange = 0xff9f0a;
constexpr uint32_t kRed = 0xff453a;

std::string Trim(std::string value) {
    auto blank = [](unsigned char c) { return std::isspace(c) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(),
                                             [&](char c) { return !blank(c); }));
    value.erase(std::find_if(value.rbegin(), value.rend(),
                             [&](char c) { return !blank(c); }).base(),
                value.end());
    return value;
}

bool CanonicalIpv4(const std::string &input, std::string &canonical) {
    const std::string value = Trim(input);
    if (value.empty()) {
        canonical.clear();
        return true;
    }

    in_addr address{};
    if (inet_pton(AF_INET, value.c_str(), &address) != 1) return false;
    const uint32_t host_order = ntohl(address.s_addr);
    if (host_order == 0 || host_order == 0xffffffffU) return false;

    char output[INET_ADDRSTRLEN]{};
    if (!inet_ntop(AF_INET, &address, output, sizeof(output))) return false;
    canonical = output;
    return true;
}

void RemoveInteraction(lv_obj_t *obj) {
    lv_obj_clear_flag(obj, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE |
                                           LV_OBJ_FLAG_CLICKABLE));
}

lv_obj_t *MakeLabel(lv_obj_t *parent, const char *text, const lv_font_t *font,
                    uint32_t color);

lv_obj_t *MakeTestControl(lv_obj_t *parent, const char *text,
                          const lv_font_t *font, int x, int y, int width,
                          int height, int radius, uint32_t idle,
                          uint32_t accent) {
    auto *control = lv_obj_create(parent);
    lv_obj_remove_style_all(control);
    lv_obj_set_pos(control, x, y);
    lv_obj_set_size(control, width, height);
    lv_obj_set_style_bg_color(control, Color(idle), 0);
    lv_obj_set_style_bg_color(control, Color(accent), LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(control, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(control, Color(0x6f7482), 0);
    lv_obj_set_style_border_color(control, Color(0xffffff), LV_STATE_CHECKED);
    lv_obj_set_style_border_width(control, 1, 0);
    lv_obj_set_style_border_width(control, 2, LV_STATE_CHECKED);
    lv_obj_set_style_radius(control, radius, 0);
    lv_obj_set_style_shadow_width(control, 0, 0);
    RemoveInteraction(control);

    if (text && text[0]) {
        auto *label = MakeLabel(control, text, font, 0xffffff);
        lv_obj_center(label);
    }
    return control;
}

void SetControlPressed(lv_obj_t *control, bool pressed) {
    if (!control) return;
    if (pressed) lv_obj_add_state(control, LV_STATE_CHECKED);
    else lv_obj_remove_state(control, LV_STATE_CHECKED);
}

void SetTriggerLevel(lv_obj_t *trigger, lv_obj_t *fill, float level,
                     bool digital) {
    level = std::max(0.0f, std::min(1.0f, level));
    SetControlPressed(trigger, digital || level > 0.08f);
    if (!fill) return;
    const int width = std::max(0, static_cast<int>(74.0f * level));
    lv_obj_set_width(fill, width);
    if (width > 0) lv_obj_clear_flag(fill, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(fill, LV_OBJ_FLAG_HIDDEN);
}

void SetStickPosition(lv_obj_t *ring, lv_obj_t *knob, float x, float y,
                      bool pressed) {
    x = std::max(-1.0f, std::min(1.0f, x));
    y = std::max(-1.0f, std::min(1.0f, y));
    const bool moved = std::fabs(x) > 0.10f || std::fabs(y) > 0.10f;
    SetControlPressed(ring, pressed || moved);
    if (!knob) return;
    lv_obj_set_style_translate_x(knob, static_cast<int>(x * 13.0f), 0);
    lv_obj_set_style_translate_y(knob, static_cast<int>(y * 13.0f), 0);
}

void StyleSurface(lv_obj_t *obj, uint32_t background, uint32_t border,
                  int radius) {
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_bg_color(obj, Color(background), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, Color(border), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *MakeLabel(lv_obj_t *parent, const char *text, const lv_font_t *font,
                    uint32_t color) {
    auto *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, Color(color), 0);
    lv_label_set_text(label, text ? text : "");
    return label;
}

lv_obj_t *MakeRoundButton(lv_obj_t *parent, const char *symbol,
                          uint32_t background, uint32_t foreground,
                          lv_event_cb_t callback, void *user_data) {
    auto *button = lv_button_create(parent);
    lv_obj_set_size(button, 40, 40);
    lv_obj_set_style_bg_color(button, Color(background), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);

    auto *icon = MakeLabel(button, symbol, &BUILTIN_ICON_FONT, foreground);
    lv_obj_center(icon);
    return button;
}

lv_obj_t *MakeModalBackdrop(lv_obj_t *parent, int width, int height,
                            uint32_t scrim, lv_event_cb_t dismiss,
                            void *user_data) {
    auto *modal = lv_obj_create(parent);
    lv_obj_remove_style_all(modal);
    lv_obj_set_size(modal, width, height);
    lv_obj_set_pos(modal, 0, 0);
    lv_obj_set_style_bg_color(modal, Color(scrim), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_70, 0);
    lv_obj_add_flag(modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(modal, dismiss, LV_EVENT_CLICKED, user_data);
    return modal;
}

lv_obj_t *MakeBottomSheet(lv_obj_t *backdrop, int width, int height,
                          uint32_t panel) {
    auto *card = lv_obj_create(backdrop);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, width, height);
    lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(card, Color(panel), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 24, 0);
    lv_obj_set_style_shadow_color(card, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_30, 0);
    lv_obj_set_style_shadow_width(card, 24, 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_style_pad_row(card, 7, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

void AddGrabber(lv_obj_t *card, uint32_t color) {
    auto *grabber = lv_obj_create(card);
    lv_obj_remove_style_all(grabber);
    lv_obj_set_size(grabber, 48, 5);
    lv_obj_set_style_bg_color(grabber, Color(color), 0);
    lv_obj_set_style_bg_opa(grabber, LV_OPA_60, 0);
    lv_obj_set_style_radius(grabber, LV_RADIUS_CIRCLE, 0);
    RemoveInteraction(grabber);
}

lv_obj_t *MakeSheetHeader(lv_obj_t *card, const char *title,
                          lv_event_cb_t cancel, lv_event_cb_t save,
                          void *user_data) {
    const auto &palette = jetson::UiTheme::Instance().Palette();
    auto *header = lv_obj_create(card);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, lv_pct(100), 40);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    RemoveInteraction(header);

    MakeRoundButton(header, LV_SYMBOL_CLOSE, palette.button, palette.text,
                    cancel, user_data);
    auto *heading = MakeLabel(header, title, &BUILTIN_TEXT_FONT, palette.text);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    MakeRoundButton(header, LV_SYMBOL_OK, palette.accent, 0xffffff,
                    save, user_data);
    return header;
}

void AnimateSheetIn(lv_obj_t *card, int height) {
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, card);
    lv_anim_set_values(&animation, height + 16, 0);
    lv_anim_set_time(&animation, 260);
    lv_anim_set_exec_cb(&animation, [](void *object, int32_t value) {
        lv_obj_set_style_translate_y(static_cast<lv_obj_t *>(object), value, 0);
    });
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
    lv_anim_start(&animation);
}

void DrawController(lv_obj_t *root, bool connected) {
    if (!root) return;
    lv_obj_clean(root);
    const uint32_t shell = connected ? 0xdce8ff : 0x686b74;
    const uint32_t detail = connected ? kBlue : 0x9699a2;

    // Small code-native controller glyph. Keeping this independent from the
    // runtime asset bucket means controller discovery/test is still usable on
    // a fresh install before optional images have been fetched from S3.
    auto make_part = [&](int x, int y, int w, int h, int radius,
                         uint32_t color) {
        auto *part = lv_obj_create(root);
        lv_obj_remove_style_all(part);
        lv_obj_set_pos(part, x, y);
        lv_obj_set_size(part, w, h);
        lv_obj_set_style_bg_color(part, Color(color), 0);
        lv_obj_set_style_bg_opa(part, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(part, radius, 0);
        RemoveInteraction(part);
        return part;
    };
    make_part(18, 18, 76, 42, 19, shell);
    make_part(18, 40, 26, 28, 13, shell);
    make_part(68, 40, 26, 28, 13, shell);
    make_part(31, 31, 20, 6, 2, detail);
    make_part(38, 24, 6, 20, 2, detail);
    make_part(70, 27, 7, 7, LV_RADIUS_CIRCLE, detail);
    make_part(80, 36, 7, 7, LV_RADIUS_CIRCLE, detail);
    make_part(51, 48, 10, 6, 3, detail);
    if (!connected) {
        auto *slash = MakeLabel(root, "/", &lv_font_montserrat_28, kRed);
        lv_obj_set_style_transform_rotation(slash, 250, 0);
        lv_obj_center(slash);
    }
}

} // namespace

PsRemotePlayView::PsRemotePlayView(lv_obj_t *parent, int width, int height,
                                   ClosedCb on_closed)
    : OverlayView(parent, width, height, "Remote Play", std::move(on_closed)) {
    LoadState();
    BuildBody();
    SetRightButton(LV_SYMBOL_SETTINGS, [](OverlayView *view) {
        static_cast<PsRemotePlayView *>(view)->OpenSettingsModal();
    });
}

PsRemotePlayView::~PsRemotePlayView() {
    CloseControllerTest();
    if (controller_timer_) {
        lv_timer_del(controller_timer_);
        controller_timer_ = nullptr;
    }
}

void PsRemotePlayView::OnStart() {
    RefreshControllerState();
    if (!controller_timer_) {
        controller_timer_ = lv_timer_create(OnControllerPoll, 2000, this);
    }
}

void PsRemotePlayView::LoadState() {
    Settings settings("remote_play", false);
    host_ = settings.GetString("ps5_ip", "");
    ps5_name_ = settings.GetString("ps5_name", "");
    preset_ = settings.GetString("preset", "performance") == "quality"
                  ? Preset::Quality
                  : Preset::Performance;
    draft_preset_ = preset_;
}

void PsRemotePlayView::BuildBody() {
    const auto &palette = jetson::UiTheme::Instance().Palette();
    lv_obj_set_style_pad_all(body_, 0, 0);
    lv_obj_clear_flag(body_, LV_OBJ_FLAG_SCROLLABLE);

    auto *title = MakeLabel(body_, "Chào mừng đến với Remote Play",
                            &BUILTIN_TEXT_FONT, palette.text);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 34);

    auto *subtitle = MakeLabel(
        body_, "Chơi game PS5 của bạn từ thiết bị này.",
        &BUILTIN_SMALL_TEXT_FONT, palette.sub_text);
    lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 76);

    controller_icon_ = lv_obj_create(body_);
    lv_obj_remove_style_all(controller_icon_);
    lv_obj_set_size(controller_icon_, 112, 76);
    lv_obj_align(controller_icon_, LV_ALIGN_TOP_MID, 0, 116);
    lv_obj_clear_flag(controller_icon_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(controller_icon_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(controller_icon_, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_event_cb(controller_icon_, OnControllerIcon, LV_EVENT_CLICKED, this);
    DrawController(controller_icon_, false);

    controller_state_label_ = MakeLabel(
        body_, "Chưa kết nối tay cầm", &BUILTIN_SMALL_TEXT_FONT,
        palette.sub_text);
    lv_obj_set_width(controller_state_label_, 360);
    lv_obj_set_style_text_align(controller_state_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(controller_state_label_, LV_ALIGN_TOP_MID, 0, 208);

    controller_hint_label_ = MakeLabel(
        body_, "Chạm biểu tượng để cập nhật và kiểm tra các nút",
        jetson::BuiltinTextFaceAt(14), palette.sub_text);
    lv_obj_set_width(controller_hint_label_, 460);
    lv_obj_set_style_text_align(controller_hint_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_opa(controller_hint_label_, LV_OPA_70, 0);
    lv_obj_align(controller_hint_label_, LV_ALIGN_TOP_MID, 0, 240);

    auto make_action_button = [&](int x_offset, uint32_t color, const char *text,
                                  lv_event_cb_t cb) {
        auto *button = lv_button_create(body_);
        lv_obj_set_size(button, 190, 48);
        lv_obj_align(button, LV_ALIGN_BOTTOM_MID, x_offset, -38);
        lv_obj_set_style_bg_color(button, Color(color), 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(button, 0, 0);
        lv_obj_set_style_radius(button, 24, 0);
        lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, this);
        auto *label = MakeLabel(button, text, &BUILTIN_SMALL_TEXT_FONT, 0xffffff);
        lv_obj_center(label);
        return button;
    };
    register_btn_ = make_action_button(-105, kBlue, "Đăng ký PS5", OnRegister);
    play_btn_ = make_action_button(105, kGreen, "Chơi ngay", OnPlay);
}

void PsRemotePlayView::RefreshControllerState() {
    const jetson::input::GamepadDeviceInfo state = jetson::input::DetectGamepad();
    const bool changed = state.connected != controller_connected_ ||
                         state.readable != controller_readable_ ||
                         state.path != controller_path_ ||
                         state.name != controller_name_;
    controller_connected_ = state.connected;
    controller_readable_ = state.readable;
    controller_uses_evdev_ = state.backend == jetson::input::GamepadBackend::Evdev;
    controller_path_ = state.path;
    controller_name_ = state.name;
    if (changed) UpdateWelcomeUi();
    UpdateSettingsUi();
}

void PsRemotePlayView::UpdateWelcomeUi() {
    const auto &palette = jetson::UiTheme::Instance().Palette();
    DrawController(controller_icon_, controller_connected_);
    if (!controller_state_label_) return;

    std::string text = "Chưa kết nối tay cầm";
    if (controller_connected_) {
        text = "Đã kết nối · " + controller_name_;
        if (!controller_readable_) text += " · không có quyền input";
    }
    lv_label_set_text(controller_state_label_, text.c_str());
    lv_obj_set_style_text_color(
        controller_state_label_,
        Color(!controller_connected_ ? palette.sub_text
                                     : controller_readable_ ? kGreen : kOrange),
        0);

    if (controller_hint_label_) {
        lv_label_set_text(
            controller_hint_label_,
            controller_connected_
                ? "Chạm biểu tượng để cập nhật và mở màn kiểm tra nút"
                : "Kết nối Bluetooth, rồi chạm biểu tượng để dò lại");
    }
}

void PsRemotePlayView::UpdateSettingsUi() {
    if (settings_controller_label_) {
        std::string state = controller_connected_
                                ? "Kết nối: " + controller_name_
                                : "Kết nối: Chưa kết nối";
        if (controller_connected_ && !controller_readable_)
            state += " (thiếu quyền input)";
        lv_label_set_text(settings_controller_label_, state.c_str());
    }
    if (settings_ps5_name_label_) {
        const std::string name = ps5_name_.empty()
                                     ? "Tên PS5: Chưa kết nối PS5 nào"
                                     : "Tên PS5: " + ps5_name_;
        lv_label_set_text(settings_ps5_name_label_, name.c_str());
    }
}

void PsRemotePlayView::Notify(const char *message) {
    if (notify_cb_) notify_cb_(message ? message : "");
}

void PsRemotePlayView::OpenControllerTest() {
    if (controller_test_modal_) return;
    RefreshControllerState();
    if (!controller_connected_) {
        Notify("Chưa phát hiện tay cầm. Kết nối Bluetooth rồi chạm để thử lại");
        return;
    }

    const auto &palette = jetson::UiTheme::Instance().Palette();
    controller_test_ = std::make_unique<ControllerTestSession>();
    ControllerTestSession &test = *controller_test_;

    controller_test_modal_ = MakeModalBackdrop(
        overlay_, width_, height_, palette.scrim, OnControllerTestDismiss, this);
    test.card = lv_obj_create(controller_test_modal_);
    StyleSurface(test.card, palette.panel, palette.border, 24);
    lv_obj_set_size(test.card, 760, 414);
    lv_obj_align(test.card, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(test.card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_shadow_color(test.card, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(test.card, LV_OPA_30, 0);
    lv_obj_set_style_shadow_width(test.card, 24, 0);

    auto *title = MakeLabel(test.card, "Kiểm tra tay cầm PS5",
                            jetson::BuiltinTextFaceAt(20), palette.text);
    lv_obj_set_pos(title, 18, 8);
    test.device_label = MakeLabel(test.card, controller_name_.c_str(),
                                  jetson::BuiltinTextFaceAt(12), palette.sub_text);
    lv_obj_set_pos(test.device_label, 18, 34);
    lv_obj_set_width(test.device_label, 650);
    lv_label_set_long_mode(test.device_label, LV_LABEL_LONG_DOT);
    auto *close = MakeRoundButton(test.card, LV_SYMBOL_CLOSE, palette.button,
                                  palette.text, OnControllerTestClose, this);
    lv_obj_set_pos(close, 704, 8);

    const uint32_t idle = palette.button;
    const uint32_t accent = palette.accent;
    const lv_font_t *compact = jetson::BuiltinTextFaceAt(13);
    const lv_font_t *symbol = jetson::BuiltinTextFaceAt(20);

    // Shoulder row. Analog trigger travel is shown by the white bar along the
    // bottom edge; digital-only mappings still illuminate the whole control.
    test.l2 = MakeTestControl(test.card, "L2", compact, 120, 54, 80, 32, 10,
                              idle, accent);
    test.l1 = MakeTestControl(test.card, "L1", compact, 212, 54, 80, 32, 10,
                              idle, accent);
    test.r1 = MakeTestControl(test.card, "R1", compact, 468, 54, 80, 32, 10,
                              idle, accent);
    test.r2 = MakeTestControl(test.card, "R2", compact, 560, 54, 80, 32, 10,
                              idle, accent);
    auto make_trigger_fill = [](lv_obj_t *trigger) {
        auto *fill = lv_obj_create(trigger);
        lv_obj_remove_style_all(fill);
        lv_obj_set_size(fill, 0, 3);
        lv_obj_align(fill, LV_ALIGN_BOTTOM_LEFT, 3, -3);
        lv_obj_set_style_bg_color(fill, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(fill, LV_RADIUS_CIRCLE, 0);
        lv_obj_add_flag(fill, LV_OBJ_FLAG_HIDDEN);
        RemoveInteraction(fill);
        return fill;
    };
    test.l2_fill = make_trigger_fill(test.l2);
    test.r2_fill = make_trigger_fill(test.r2);

    // A compact, original LVGL silhouette inspired by the DualSense control
    // layout. It is assembled from primitives and contains no Sony artwork.
    auto make_decor = [&](int x, int y, int width, int height, int radius,
                          uint32_t color) {
        auto *part = lv_obj_create(test.card);
        lv_obj_remove_style_all(part);
        lv_obj_set_pos(part, x, y);
        lv_obj_set_size(part, width, height);
        lv_obj_set_style_bg_color(part, Color(color), 0);
        lv_obj_set_style_bg_opa(part, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(part, radius, 0);
        RemoveInteraction(part);
        return part;
    };
    make_decor(105, 226, 150, 126, 58, palette.row);
    make_decor(505, 226, 150, 126, 58, palette.row);
    auto *shell = make_decor(96, 88, 568, 248, 88, palette.row);
    lv_obj_set_style_border_color(shell, Color(palette.border), 0);
    lv_obj_set_style_border_width(shell, 2, 0);
    make_decor(322, 91, 116, 4, 2, accent);

    test.touchpad = MakeTestControl(test.card, "TOUCHPAD", compact,
                                    292, 104, 176, 68, 12, 0x252a35, accent);
    test.create = MakeTestControl(test.card, "Create",
                                  jetson::BuiltinTextFaceAt(10), 230, 126,
                                  54, 22, 11, idle, accent);
    test.options = MakeTestControl(test.card, "Options",
                                   jetson::BuiltinTextFaceAt(10), 476, 126,
                                   54, 22, 11, idle, accent);

    test.dpad_up = MakeTestControl(test.card, LV_SYMBOL_UP, &BUILTIN_ICON_FONT,
                                   184, 160, 42, 42, 9, idle, accent);
    test.dpad_down = MakeTestControl(test.card, LV_SYMBOL_DOWN, &BUILTIN_ICON_FONT,
                                     184, 228, 42, 42, 9, idle, accent);
    test.dpad_left = MakeTestControl(test.card, LV_SYMBOL_LEFT, &BUILTIN_ICON_FONT,
                                     150, 194, 42, 42, 9, idle, accent);
    test.dpad_right = MakeTestControl(test.card, LV_SYMBOL_RIGHT, &BUILTIN_ICON_FONT,
                                      218, 194, 42, 42, 9, idle, accent);

    test.triangle = MakeTestControl(test.card, "△", symbol, 548, 160, 42, 42,
                                    LV_RADIUS_CIRCLE, idle, accent);
    test.cross = MakeTestControl(test.card, "×", symbol, 548, 228, 42, 42,
                                 LV_RADIUS_CIRCLE, idle, accent);
    test.square = MakeTestControl(test.card, "□", symbol, 514, 194, 42, 42,
                                  LV_RADIUS_CIRCLE, idle, accent);
    test.circle = MakeTestControl(test.card, "○", symbol, 582, 194, 42, 42,
                                  LV_RADIUS_CIRCLE, idle, accent);

    auto make_stick = [&](int x, const char *label, lv_obj_t **knob) {
        auto *ring = MakeTestControl(test.card, nullptr, compact, x, 252, 70, 70,
                                     LV_RADIUS_CIRCLE, 0x20242d, accent);
        *knob = make_decor(x + 19, 271, 32, 32, LV_RADIUS_CIRCLE, 0x747986);
        auto *name = MakeLabel(*knob, label, jetson::BuiltinTextFaceAt(10),
                               0xffffff);
        lv_obj_center(name);
        return ring;
    };
    test.l3 = make_stick(270, "L3", &test.left_knob);
    test.r3 = make_stick(420, "R3", &test.right_knob);
    test.ps = MakeTestControl(test.card, "PS", jetson::BuiltinTextFaceAt(10),
                              365, 270, 30, 30, LV_RADIUS_CIRCLE, idle, accent);

    test.last_input_label = MakeLabel(
        test.card, "Nhấn hoặc di chuyển tay cầm để kiểm tra",
        jetson::BuiltinTextFaceAt(13), palette.sub_text);
    lv_obj_set_width(test.last_input_label, 700);
    lv_obj_set_style_text_align(test.last_input_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(test.last_input_label, LV_ALIGN_BOTTOM_MID, 0, -10);

    jetson::input::GamepadDeviceInfo info;
    info.connected = controller_connected_;
    info.readable = controller_readable_;
    info.backend = controller_uses_evdev_
                       ? jetson::input::GamepadBackend::Evdev
                       : jetson::input::GamepadBackend::Joystick;
    info.path = controller_path_;
    info.name = controller_name_;
    const bool opened = test.device.Open(info);
    test.state.connected = opened;
    if (!opened) {
        const std::string message = test.device.LastError().empty()
                                        ? "Không thể đọc input từ tay cầm"
                                        : test.device.LastError();
        lv_label_set_text(test.last_input_label, message.c_str());
        lv_obj_set_style_text_color(test.last_input_label, Color(kRed), 0);
    } else {
        controller_input_timer_ = lv_timer_create(OnControllerInputPoll, 20, this);
        PollControllerInput();
    }

    lv_obj_move_foreground(controller_test_modal_);
}

void PsRemotePlayView::CloseControllerTest() {
    if (controller_input_timer_) {
        lv_timer_del(controller_input_timer_);
        controller_input_timer_ = nullptr;
    }
    controller_test_.reset();
    if (controller_test_modal_) {
        lv_obj_del(controller_test_modal_);
        controller_test_modal_ = nullptr;
    }
}

void PsRemotePlayView::PollControllerInput() {
    if (!controller_test_) return;
    const bool changed = controller_test_->device.Poll(controller_test_->state);
    if (!changed) return;
    UpdateControllerTestUi();
    if (!controller_test_->state.connected) RefreshControllerState();
}

void PsRemotePlayView::UpdateControllerTestUi() {
    if (!controller_test_) return;
    ControllerTestSession &test = *controller_test_;
    const auto &state = test.state;

    SetControlPressed(test.dpad_up, state.dpad_up);
    SetControlPressed(test.dpad_down, state.dpad_down);
    SetControlPressed(test.dpad_left, state.dpad_left);
    SetControlPressed(test.dpad_right, state.dpad_right);
    SetControlPressed(test.cross, state.cross);
    SetControlPressed(test.circle, state.circle);
    SetControlPressed(test.triangle, state.triangle);
    SetControlPressed(test.square, state.square);
    SetControlPressed(test.l1, state.l1);
    SetControlPressed(test.r1, state.r1);
    SetTriggerLevel(test.l2, test.l2_fill,
                    state.has_l2_axis ? state.l2
                                      : state.l2_button ? 1.0f : 0.0f,
                    state.l2_button);
    SetTriggerLevel(test.r2, test.r2_fill,
                    state.has_r2_axis ? state.r2
                                      : state.r2_button ? 1.0f : 0.0f,
                    state.r2_button);
    SetStickPosition(test.l3, test.left_knob, state.left_x, state.left_y,
                     state.l3);
    SetStickPosition(test.r3, test.right_knob, state.right_x, state.right_y,
                     state.r3);
    SetControlPressed(test.create, state.create);
    SetControlPressed(test.options, state.options);
    SetControlPressed(test.ps, state.ps);
    SetControlPressed(test.touchpad, state.touchpad);

    if (!test.last_input_label) return;
    if (!state.connected) {
        const std::string message = test.device.LastError().empty()
                                        ? "Tay cầm đã ngắt kết nối"
                                        : test.device.LastError();
        lv_label_set_text(test.last_input_label, message.c_str());
        lv_obj_set_style_text_color(test.last_input_label, Color(kRed), 0);
    } else {
        lv_label_set_text(test.last_input_label,
                          state.last_input.empty()
                              ? "Nhấn hoặc di chuyển tay cầm để kiểm tra"
                              : state.last_input.c_str());
        const auto &palette = jetson::UiTheme::Instance().Palette();
        lv_obj_set_style_text_color(test.last_input_label, Color(palette.sub_text), 0);
    }
}

void PsRemotePlayView::StartRegister() {
    if (host_.empty()) {
        Notify("Nhập IP PS5 trong Cài đặt trước");
        OpenSettingsModal();
        return;
    }
    if (!launch_cb_) {
        Notify("Launcher Remote Play chưa sẵn sàng trong phiên này");
        return;
    }
    // chiaki-ng has no headless registration (no CLI `regist`): the first
    // device-link MUST be completed inside chiaki-ng's own GUI, where the
    // user enters the IP and the 8-digit Remote Play PIN shown on the PS5
    // (Settings > System > Remote Play > Link Device). After that the
    // console is registered permanently and "Chơi ngay" streams directly
    // with no PIN.
    WriteLauncherState();  // ensure ps-remote-play.conf has host= for the launcher
    Notify("Đang mở chiaki-ng — chọn Register, nhập IP + mã PIN trên PS5");
    launch_cb_(true);
}

void PsRemotePlayView::StartStream() {
    if (host_.empty()) {
        Notify("Nhập IP PS5 trong Cài đặt trước");
        OpenSettingsModal();
        return;
    }
    if (!HasChiakiRegistration()) {
        Notify("PS5 chưa đăng ký — bấm 'Đăng ký PS5' trước");
        return;
    }
    if (!launch_cb_) {
        Notify("Launcher Remote Play chưa sẵn sàng trong phiên này");
        return;
    }
    WriteLauncherState();  // ensure ps-remote-play.conf has host= for stream/wakeup
    Notify("Đang kết nối PS5...");
    launch_cb_(false);
}

void PsRemotePlayView::OpenSettingsModal() {
    if (settings_modal_) return;
    const auto &palette = jetson::UiTheme::Instance().Palette();
    RefreshControllerState();
    draft_preset_ = preset_;

    settings_modal_ = MakeModalBackdrop(overlay_, width_, height_, palette.scrim,
                                        OnSettingsDismiss, this);
    constexpr int kSheetHeight = 420;
    settings_card_ = MakeBottomSheet(settings_modal_, 720, kSheetHeight,
                                     palette.panel);
    AddGrabber(settings_card_, palette.sub_text);
    MakeSheetHeader(settings_card_, "Cài đặt", OnSettingsCancel,
                    OnSettingsSave, this);

    auto *status_title = MakeLabel(settings_card_, "Trạng thái",
                                   &BUILTIN_SMALL_TEXT_FONT, palette.sub_text);
    lv_obj_set_width(status_title, lv_pct(100));

    auto *device_group = lv_obj_create(settings_card_);
    StyleSurface(device_group, palette.row, palette.border, 14);
    lv_obj_set_size(device_group, lv_pct(100), 86);
    lv_obj_set_style_pad_all(device_group, 0, 0);
    lv_obj_set_flex_flow(device_group, LV_FLEX_FLOW_COLUMN);

    auto make_info_row = [&](int height) {
        auto *row = lv_obj_create(device_group);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), height);
        lv_obj_set_style_pad_hor(row, 12, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        RemoveInteraction(row);
        return row;
    };

    auto *controller_row = make_info_row(42);
    settings_controller_label_ = MakeLabel(
        controller_row, "Kết nối: Chưa kết nối", &BUILTIN_SMALL_TEXT_FONT,
        palette.text);
    lv_obj_set_flex_grow(settings_controller_label_, 1);
    lv_label_set_long_mode(settings_controller_label_, LV_LABEL_LONG_DOT);

    auto *connect = lv_button_create(controller_row);
    lv_obj_set_size(connect, 38, 34);
    lv_obj_set_style_bg_color(connect, Color(palette.button), 0);
    lv_obj_set_style_bg_opa(connect, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(connect, 0, 0);
    lv_obj_set_style_radius(connect, 9, 0);
    lv_obj_set_style_shadow_width(connect, 0, 0);
    lv_obj_set_style_pad_all(connect, 0, 0);
    lv_obj_add_event_cb(connect, OnConnectController, LV_EVENT_CLICKED, this);
    auto *connect_icon = jetson::ui::CreateAppIcon(connect, "connect", 21);
    if (connect_icon) {
        lv_obj_set_style_image_recolor(connect_icon, Color(palette.accent), 0);
        lv_obj_set_style_image_recolor_opa(connect_icon, LV_OPA_COVER, 0);
    } else {
        connect_icon = MakeLabel(connect, LV_SYMBOL_BLUETOOTH,
                                 &BUILTIN_ICON_FONT, palette.accent);
    }
    lv_obj_center(connect_icon);

    auto *divider = lv_obj_create(device_group);
    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, lv_pct(100), 1);
    lv_obj_set_style_bg_color(divider, Color(palette.border), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
    RemoveInteraction(divider);

    auto *ps5_row = make_info_row(42);
    settings_ps5_name_label_ = MakeLabel(
        ps5_row, "Tên PS5: Chưa kết nối PS5 nào", &BUILTIN_SMALL_TEXT_FONT,
        palette.text);
    lv_obj_set_width(settings_ps5_name_label_, lv_pct(100));
    lv_label_set_long_mode(settings_ps5_name_label_, LV_LABEL_LONG_DOT);

    auto *ip_form = lv_obj_create(settings_card_);
    StyleSurface(ip_form, palette.row, palette.border, 14);
    lv_obj_set_size(ip_form, lv_pct(100), 50);
    lv_obj_set_style_pad_hor(ip_form, 12, 0);
    lv_obj_set_style_pad_ver(ip_form, 4, 0);
    lv_obj_set_style_pad_column(ip_form, 10, 0);
    lv_obj_set_flex_flow(ip_form, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ip_form, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    auto *ip_label = MakeLabel(ip_form, "IP PS5", &BUILTIN_SMALL_TEXT_FONT,
                               palette.text);
    lv_obj_set_width(ip_label, 68);
    /* TelexInput, not lv_textarea: the native textarea never received EV_KEY
     * input from the Jetson evdev keyboard, so the IP could not be typed at
     * all ("con trỏ bàn phím không có"). TelexInput owns LV_EVENT_KEY and
     * joins the keypad group in its constructor. */
    settings_ip_input_ = new TelexInput(ip_form, 1, 42);
    lv_obj_set_flex_grow(settings_ip_input_->obj(), 1);
    settings_ip_input_->SetTelex(false);
    settings_ip_input_->SetMaxLen(15);
    settings_ip_input_->SetAcceptedChars("0123456789.");
    settings_ip_input_->SetFont(&BUILTIN_SMALL_TEXT_FONT);
    settings_ip_input_->SetPlaceholder("Chưa kết nối tới IP PS5");
    if (!host_.empty()) settings_ip_input_->SetText(host_);

    settings_ip_error_ = MakeLabel(settings_card_, " ",
                                   &BUILTIN_SMALL_TEXT_FONT, palette.sub_text);
    lv_obj_set_width(settings_ip_error_, lv_pct(100));
    lv_obj_set_style_text_align(settings_ip_error_, LV_TEXT_ALIGN_CENTER, 0);

    auto *mode_title = MakeLabel(settings_card_, "Chế độ hiển thị",
                                 &BUILTIN_SMALL_TEXT_FONT, palette.sub_text);
    lv_obj_set_width(mode_title, lv_pct(100));

    auto *cards = lv_obj_create(settings_card_);
    lv_obj_remove_style_all(cards);
    lv_obj_set_size(cards, lv_pct(100), 100);
    lv_obj_set_style_pad_column(cards, 12, 0);
    lv_obj_set_flex_flow(cards, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cards, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    RemoveInteraction(cards);

    auto make_preset_card = [&](const char *icon_name, const char *title_text,
                                const char *detail, lv_event_cb_t callback,
                                lv_obj_t **radio_out) {
        auto *card = lv_obj_create(cards);
        StyleSurface(card, palette.row, palette.border, 14);
        lv_obj_set_height(card, lv_pct(100));
        lv_obj_set_width(card, 1);
        lv_obj_set_flex_grow(card, 1);
        lv_obj_set_style_border_color(card, Color(palette.accent),
                                      LV_STATE_CHECKED);
        lv_obj_set_style_border_width(card, 3, LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(card, Color(palette.row_active),
                                  LV_STATE_CHECKED);
        lv_obj_set_style_pad_all(card, 12, 0);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, callback, LV_EVENT_CLICKED, this);

        auto *icon_bg = lv_obj_create(card);
        lv_obj_remove_style_all(icon_bg);
        lv_obj_set_size(icon_bg, 38, 38);
        lv_obj_set_pos(icon_bg, 0, 0);
        lv_obj_set_style_bg_color(icon_bg, Color(palette.button), 0);
        lv_obj_set_style_bg_opa(icon_bg, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(icon_bg, 10, 0);
        RemoveInteraction(icon_bg);
        auto *icon = jetson::ui::CreateAppIcon(icon_bg, icon_name, 24);
        if (icon) {
            lv_obj_set_style_image_recolor(icon, Color(palette.sub_text), 0);
            lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
            lv_obj_center(icon);
        }

        auto *title_label = MakeLabel(card, title_text, &BUILTIN_TEXT_FONT,
                                      palette.text);
        lv_obj_set_pos(title_label, 52, 0);
        auto *detail_label = MakeLabel(card, detail, &BUILTIN_SMALL_TEXT_FONT,
                                       palette.sub_text);
        lv_obj_set_pos(detail_label, 52, 36);

        auto *radio = lv_obj_create(card);
        lv_obj_remove_style_all(radio);
        lv_obj_set_size(radio, 24, 24);
        lv_obj_align(radio, LV_ALIGN_TOP_RIGHT, 0, 0);
        lv_obj_set_style_border_color(radio, Color(palette.sub_text), 0);
        lv_obj_set_style_border_color(radio, Color(palette.accent),
                                      LV_STATE_CHECKED);
        lv_obj_set_style_border_width(radio, 3, 0);
        lv_obj_set_style_bg_color(radio, Color(palette.accent),
                                  LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(radio, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_opa(radio, LV_OPA_COVER, LV_STATE_CHECKED);
        lv_obj_set_style_radius(radio, LV_RADIUS_CIRCLE, 0);
        RemoveInteraction(radio);
        *radio_out = radio;
        return card;
    };

    performance_card_ = make_preset_card(
        "performance", "Hiệu năng", "540p · 60 FPS", OnPerformance,
        &performance_radio_);
    quality_card_ = make_preset_card(
        "quality", "Chất lượng", "720p · 40 FPS", OnQuality,
        &quality_radio_);

    UpdateSettingsUi();
    UpdatePresetCards();
    settings_ip_input_->Focus();
    lv_obj_move_foreground(settings_modal_);
    AnimateSheetIn(settings_card_, kSheetHeight);
}

void PsRemotePlayView::UpdatePresetCards() {
    const bool performance = draft_preset_ == Preset::Performance;
    auto set_checked = [](lv_obj_t *object, bool checked) {
        if (!object) return;
        if (checked) lv_obj_add_state(object, LV_STATE_CHECKED);
        else lv_obj_remove_state(object, LV_STATE_CHECKED);
    };
    set_checked(performance_card_, performance);
    set_checked(performance_radio_, performance);
    set_checked(quality_card_, !performance);
    set_checked(quality_radio_, !performance);
}

void PsRemotePlayView::CloseSettingsModal() {
    if (!settings_modal_) return;
    lv_obj_del(settings_modal_);
    settings_modal_ = nullptr;
    settings_card_ = nullptr;
    settings_controller_label_ = nullptr;
    settings_ps5_name_label_ = nullptr;
    settings_ip_input_ = nullptr;
    settings_ip_error_ = nullptr;
    performance_card_ = nullptr;
    quality_card_ = nullptr;
    performance_radio_ = nullptr;
    quality_radio_ = nullptr;
}

void PsRemotePlayView::SaveSettingsModal() {
    if (!settings_ip_input_) return;
    std::string canonical;
    if (!CanonicalIpv4(settings_ip_input_->Text(), canonical)) {
        if (settings_ip_error_) {
            lv_label_set_text(settings_ip_error_, "Địa chỉ IP PS5 không hợp lệ");
            lv_obj_set_style_text_color(settings_ip_error_, Color(kRed), 0);
        }
        lv_obj_set_style_border_width(settings_ip_input_->obj(), 2, 0);
        lv_obj_set_style_border_color(settings_ip_input_->obj(), Color(kRed), 0);
        Notify("Địa chỉ IP PS5 không hợp lệ");
        return;
    }

    host_ = std::move(canonical);
    preset_ = draft_preset_;
    if (host_.empty()) ps5_name_.clear();
    Settings settings("remote_play", true);
    settings.SetString("ps5_ip", host_);
    settings.SetString("ps5_name", ps5_name_);
    settings.SetString("preset",
                       preset_ == Preset::Performance ? "performance" : "quality");
    // Keep the launcher's state file in sync, otherwise `stream` mode exits
    // with "PS5 address is not configured" even though the UI saved an IP.
    WriteLauncherState();

    CloseSettingsModal();
    Notify(host_.empty() ? "Đã lưu cài đặt Remote Play"
                         : "Đã lưu IP PS5");
}

void PsRemotePlayView::OpenBluetoothSettings() {
    CloseSettingsModal();
    if (open_bluetooth_cb_) open_bluetooth_cb_();
    else Notify("Mở Cài đặt > Bluetooth để kết nối tay cầm");
}

namespace {
constexpr char kLauncherStateDir[] = "/var/lib/jetson-fw";
constexpr char kLauncherStateFile[] = "/var/lib/jetson-fw/ps-remote-play.conf";

std::string DecodeQsettingsText(std::string value) {
    if (value.rfind("@String(", 0) == 0 && !value.empty() && value.back() == ')') {
        value = value.substr(8, value.size() - 9);
    }
    return value;
}

bool SafeChiakiProfile(const std::string &profile) {
    if (profile.empty() || profile.size() > 96) return false;
    if (!std::isalnum(static_cast<unsigned char>(profile.front()))) return false;
    for (unsigned char c : profile) {
        if (!std::isalnum(c) && c != '.' && c != '_' && c != '-' && c != ' ')
            return false;
    }
    return true;
}

bool ConfigHasRegistKey(const std::string &path) {
    FILE *file = std::fopen(path.c_str(), "r");
    if (!file) return false;
    char line[512];
    bool found = false;
    while (!found && std::fgets(line, sizeof(line), file))
        found = std::strstr(line, "rp_regist_key=") != nullptr;
    std::fclose(file);
    return found;
}

std::string ReadChiakiProfile(const std::string &default_config) {
    FILE *file = std::fopen(default_config.c_str(), "r");
    if (!file) return {};
    char line[512];
    std::string profile;
    while (profile.empty() && std::fgets(line, sizeof(line), file)) {
        std::string entry(line);
        while (!entry.empty() && (entry.back() == '\n' || entry.back() == '\r'))
            entry.pop_back();
        if (entry.rfind("current_profile=", 0) != 0) continue;
        profile = Trim(DecodeQsettingsText(entry.substr(16)));
        if (!SafeChiakiProfile(profile)) profile.clear();
    }
    std::fclose(file);
    return profile;
}
} // namespace

void PsRemotePlayView::WriteLauncherState() const {
    // Preserve fields the UI does not own (nickname/passcode may have been
    // written by ps_remote_play_ctl.sh save).
    std::string nickname;
    std::string passcode;
    if (FILE *file = std::fopen(kLauncherStateFile, "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), file)) {
            std::string entry(line);
            while (!entry.empty() &&
                   (entry.back() == '\n' || entry.back() == '\r'))
                entry.pop_back();
            if (entry.rfind("nickname=", 0) == 0) nickname = entry.substr(9);
            else if (entry.rfind("passcode=", 0) == 0) passcode = entry.substr(9);
        }
        std::fclose(file);
    }

    ::mkdir(kLauncherStateDir, 0755);  // best effort; firmware runs as root
    const std::string temporary = std::string(kLauncherStateFile) + ".tmp";
    FILE *file = std::fopen(temporary.c_str(), "w");
    if (!file) {
        ESP_LOGW("PsRemotePlay", "cannot write %s: %s", kLauncherStateFile,
                 std::strerror(errno));
        return;
    }
    std::fprintf(file, "host=%s\nnickname=%s\npreset=%s\npasscode=%s\n",
                 host_.c_str(), nickname.c_str(),
                 preset_ == Preset::Performance ? "smooth" : "quality",
                 passcode.c_str());
    std::fclose(file);
    ::chmod(temporary.c_str(), 0600);
    ::rename(temporary.c_str(), kLauncherStateFile);
}

bool PsRemotePlayView::HasChiakiRegistration() const {
    // The launcher keeps Chiaki's QSettings under /var/lib/jetson-fw/chiaki.
    // A registered console always serializes an rp_regist_key entry.
    static const char *kConfigDirs[] = {
        "/var/lib/jetson-fw/chiaki/.config/Chiaki",
        "/var/lib/jetson-fw/chiaki/.config/chiaki",
    };
    for (const char *dir : kConfigDirs) {
        const std::string default_config = std::string(dir) + "/Chiaki.conf";
        const std::string profile = ReadChiakiProfile(default_config);
        if (!profile.empty() &&
            ConfigHasRegistKey(std::string(dir) + "/Chiaki-" + profile + ".conf"))
            return true;
        if (ConfigHasRegistKey(default_config)) return true;
    }
    return false;
}

void PsRemotePlayView::OnRegister(lv_event_t *e) {
    LvglLockGuard lock;
    static_cast<PsRemotePlayView *>(lv_event_get_user_data(e))->StartRegister();
}

void PsRemotePlayView::OnPlay(lv_event_t *e) {
    LvglLockGuard lock;
    static_cast<PsRemotePlayView *>(lv_event_get_user_data(e))->StartStream();
}

void PsRemotePlayView::OnSettingsDismiss(lv_event_t *e) {
    if (lv_event_get_target(e) != lv_event_get_current_target(e)) return;
    LvglLockGuard lock;
    static_cast<PsRemotePlayView *>(lv_event_get_user_data(e))->CloseSettingsModal();
}

void PsRemotePlayView::OnSettingsCancel(lv_event_t *e) {
    LvglLockGuard lock;
    static_cast<PsRemotePlayView *>(lv_event_get_user_data(e))->CloseSettingsModal();
}

void PsRemotePlayView::OnSettingsSave(lv_event_t *e) {
    LvglLockGuard lock;
    static_cast<PsRemotePlayView *>(lv_event_get_user_data(e))->SaveSettingsModal();
}

void PsRemotePlayView::OnControllerIcon(lv_event_t *e) {
    LvglLockGuard lock;
    static_cast<PsRemotePlayView *>(lv_event_get_user_data(e))->OpenControllerTest();
}

void PsRemotePlayView::OnControllerTestDismiss(lv_event_t *e) {
    if (lv_event_get_target(e) != lv_event_get_current_target(e)) return;
    LvglLockGuard lock;
    static_cast<PsRemotePlayView *>(lv_event_get_user_data(e))->CloseControllerTest();
}

void PsRemotePlayView::OnControllerTestClose(lv_event_t *e) {
    LvglLockGuard lock;
    static_cast<PsRemotePlayView *>(lv_event_get_user_data(e))->CloseControllerTest();
}

void PsRemotePlayView::OnConnectController(lv_event_t *e) {
    LvglLockGuard lock;
    static_cast<PsRemotePlayView *>(lv_event_get_user_data(e))->OpenBluetoothSettings();
}

void PsRemotePlayView::OnPerformance(lv_event_t *e) {
    LvglLockGuard lock;
    auto *self = static_cast<PsRemotePlayView *>(lv_event_get_user_data(e));
    self->draft_preset_ = Preset::Performance;
    self->UpdatePresetCards();
    self->Notify("540p và 60 FPS hiệu năng tốt nhất");
}

void PsRemotePlayView::OnQuality(lv_event_t *e) {
    LvglLockGuard lock;
    auto *self = static_cast<PsRemotePlayView *>(lv_event_get_user_data(e));
    self->draft_preset_ = Preset::Quality;
    self->UpdatePresetCards();
    self->Notify("720p và 40 FPS độ phân giải cao");
}

void PsRemotePlayView::OnControllerPoll(lv_timer_t *timer) {
    auto *self = static_cast<PsRemotePlayView *>(lv_timer_get_user_data(timer));
    if (self) self->RefreshControllerState();
}

void PsRemotePlayView::OnControllerInputPoll(lv_timer_t *timer) {
    auto *self = static_cast<PsRemotePlayView *>(lv_timer_get_user_data(timer));
    if (self) self->PollControllerInput();
}

} // namespace home
