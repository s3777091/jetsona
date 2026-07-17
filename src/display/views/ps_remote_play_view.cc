#include "display/views/ps_remote_play_view.h"

#include "display/common/lvgl_utils.h"
#include "display/core/app_icons.h"
#include "display/theme/ui_theme.h"
#include "fonts.h"
#include "lvgl_runtime.h"
#include "settings.h"

#include <arpa/inet.h>
#include <glob.h>
#include <limits.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <string>

namespace home {

using jetson::ui::Color;
using jetson::ui::LvglLockGuard;

namespace {

constexpr uint32_t kBlue = 0x1677ff;
constexpr uint32_t kGreen = 0x30d158;
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

std::string ReadOneLine(const std::string &path) {
    std::ifstream input(path);
    std::string value;
    std::getline(input, value);
    return Trim(std::move(value));
}

struct ControllerState {
    bool connected = false;
    std::string name;
};

ControllerState ReadControllerState() {
    const char *patterns[] = {
        "/dev/input/js*",
        "/dev/input/by-id/*-joystick",
        "/dev/input/by-path/*-joystick",
    };

    std::string path;
    for (const char *pattern : patterns) {
        glob_t matches{};
        const int result = glob(pattern, 0, nullptr, &matches);
        if (result == 0 && matches.gl_pathc > 0 && matches.gl_pathv &&
            matches.gl_pathv[0]) {
            path = matches.gl_pathv[0];
        }
        globfree(&matches);
        if (!path.empty()) break;
    }

    ControllerState state;
    if (!path.empty()) {
        state.connected = true;
        char resolved[PATH_MAX]{};
        if (realpath(path.c_str(), resolved)) path = resolved;
        const size_t slash = path.find_last_of('/');
        const std::string device = slash == std::string::npos
                                       ? path
                                       : path.substr(slash + 1);
        if (device.rfind("js", 0) == 0) {
            state.name = ReadOneLine("/sys/class/input/" + device + "/device/name");
        }
        if (state.name.empty()) state.name = "Tay cầm";
    }
    return state;
}

void RemoveInteraction(lv_obj_t *obj) {
    lv_obj_clear_flag(obj, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE |
                                           LV_OBJ_FLAG_CLICKABLE));
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
    const auto &palette = jetson::UiTheme::Instance().Palette();
    lv_obj_clean(root);

    const uint32_t stroke = connected ? palette.accent : palette.sub_text;
    auto *body = lv_obj_create(root);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, 98, 54);
    lv_obj_set_pos(body, 7, 11);
    lv_obj_set_style_bg_color(body, Color(palette.panel), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(body, Color(stroke), 0);
    lv_obj_set_style_border_width(body, 3, 0);
    lv_obj_set_style_radius(body, 23, 0);
    RemoveInteraction(body);

    auto add_shape = [&](int x, int y, int width, int height, int radius) {
        auto *shape = lv_obj_create(body);
        lv_obj_remove_style_all(shape);
        lv_obj_set_size(shape, width, height);
        lv_obj_set_pos(shape, x, y);
        lv_obj_set_style_bg_color(shape, Color(stroke), 0);
        lv_obj_set_style_bg_opa(shape, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(shape, radius, 0);
        RemoveInteraction(shape);
        return shape;
    };

    // D-pad and face buttons make the state icon readable without relying on
    // an emoji/font glyph that may be missing on the embedded image.
    add_shape(20, 24, 22, 6, 3);
    add_shape(28, 16, 6, 22, 3);
    add_shape(68, 18, 8, 8, LV_RADIUS_CIRCLE);
    add_shape(79, 29, 8, 8, LV_RADIUS_CIRCLE);

    auto *badge = lv_obj_create(root);
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, 24, 24);
    lv_obj_set_pos(badge, 84, 0);
    lv_obj_set_style_bg_color(badge, Color(connected ? kGreen : palette.button), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(badge, Color(palette.panel), 0);
    lv_obj_set_style_border_width(badge, 3, 0);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
    RemoveInteraction(badge);
    auto *state = MakeLabel(badge, connected ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE,
                            &BUILTIN_ICON_FONT,
                            connected ? 0xffffff : palette.sub_text);
    lv_obj_center(state);

    if (!connected) {
        static lv_point_precise_t slash_points[] = {{16, 67}, {99, 8}};
        auto *slash = lv_line_create(root);
        lv_line_set_points(slash, slash_points, 2);
        lv_obj_set_style_line_color(slash, Color(kRed), 0);
        lv_obj_set_style_line_width(slash, 5, 0);
        lv_obj_set_style_line_rounded(slash, true, 0);
        RemoveInteraction(slash);
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
    RemoveInteraction(controller_icon_);
    DrawController(controller_icon_, false);

    controller_state_label_ = MakeLabel(
        body_, "Chưa kết nối tay cầm", &BUILTIN_SMALL_TEXT_FONT,
        palette.sub_text);
    lv_obj_set_width(controller_state_label_, 360);
    lv_obj_set_style_text_align(controller_state_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(controller_state_label_, LV_ALIGN_TOP_MID, 0, 208);

    sign_in_btn_ = lv_button_create(body_);
    lv_obj_set_size(sign_in_btn_, 190, 48);
    lv_obj_align(sign_in_btn_, LV_ALIGN_BOTTOM_MID, 0, -38);
    lv_obj_set_style_bg_color(sign_in_btn_, Color(kBlue), 0);
    lv_obj_set_style_bg_opa(sign_in_btn_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sign_in_btn_, 0, 0);
    lv_obj_set_style_radius(sign_in_btn_, 24, 0);
    lv_obj_set_style_shadow_width(sign_in_btn_, 0, 0);
    lv_obj_add_event_cb(sign_in_btn_, OnSignIn, LV_EVENT_CLICKED, this);

    auto *sign_in_label = MakeLabel(sign_in_btn_, "Sign In to PSN",
                                    &BUILTIN_SMALL_TEXT_FONT, 0xffffff);
    lv_obj_center(sign_in_label);
}

void PsRemotePlayView::RefreshControllerState() {
    const ControllerState state = ReadControllerState();
    const bool changed = state.connected != controller_connected_ ||
                         state.name != controller_name_;
    controller_connected_ = state.connected;
    controller_name_ = state.name;
    if (changed) UpdateWelcomeUi();
    UpdateSettingsUi();
}

void PsRemotePlayView::UpdateWelcomeUi() {
    const auto &palette = jetson::UiTheme::Instance().Palette();
    DrawController(controller_icon_, controller_connected_);
    if (!controller_state_label_) return;

    const std::string text = controller_connected_
                                 ? "Đã kết nối · " + controller_name_
                                 : "Chưa kết nối tay cầm";
    lv_label_set_text(controller_state_label_, text.c_str());
    lv_obj_set_style_text_color(
        controller_state_label_,
        Color(controller_connected_ ? kGreen : palette.sub_text), 0);
}

void PsRemotePlayView::UpdateSettingsUi() {
    if (settings_controller_label_) {
        const std::string state = controller_connected_
                                      ? "Kết nối: " + controller_name_
                                      : "Kết nối: Chưa kết nối";
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

void PsRemotePlayView::OpenPinModal() {
    if (pin_modal_) return;
    const auto &palette = jetson::UiTheme::Instance().Palette();

    pin_modal_ = MakeModalBackdrop(overlay_, width_, height_, palette.scrim,
                                   OnPinDismiss, this);
    constexpr int kSheetHeight = 230;
    auto *card = MakeBottomSheet(pin_modal_, 580, kSheetHeight, palette.panel);
    AddGrabber(card, palette.sub_text);
    MakeSheetHeader(card, "Đăng nhập mã PIN", OnPinCancel, OnPinSave, this);

    auto *hint = MakeLabel(card, "Nhập mã PIN đang hiển thị trên PS5 của bạn",
                           &BUILTIN_SMALL_TEXT_FONT, palette.sub_text);
    lv_obj_set_width(hint, lv_pct(100));
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);

    auto *form = lv_obj_create(card);
    StyleSurface(form, palette.row, palette.border, 14);
    lv_obj_set_size(form, lv_pct(100), 54);
    lv_obj_set_style_pad_all(form, 5, 0);
    lv_obj_set_style_pad_column(form, 6, 0);
    lv_obj_set_flex_flow(form, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(form, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    pin_input_ = lv_textarea_create(form);
    lv_obj_set_size(pin_input_, 1, 44);
    lv_obj_set_flex_grow(pin_input_, 1);
    lv_obj_set_style_bg_opa(pin_input_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pin_input_, 0, 0);
    lv_obj_set_style_text_font(pin_input_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(pin_input_, Color(palette.text), 0);
    lv_obj_set_style_text_color(pin_input_, Color(palette.sub_text),
                                LV_PART_TEXTAREA_PLACEHOLDER);
    lv_textarea_set_one_line(pin_input_, true);
    lv_textarea_set_max_length(pin_input_, 8);
    lv_textarea_set_accepted_chars(pin_input_, "0123456789");
    lv_textarea_set_password_mode(pin_input_, true);
    lv_textarea_set_placeholder_text(pin_input_, "Nhập mã PIN PS5 của bạn");
    lv_obj_add_event_cb(pin_input_, OnPinSave, LV_EVENT_READY, this);

    auto *submit = lv_button_create(form);
    lv_obj_set_size(submit, 44, 44);
    lv_obj_set_style_bg_color(submit, Color(palette.accent), 0);
    lv_obj_set_style_bg_opa(submit, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(submit, 0, 0);
    lv_obj_set_style_radius(submit, 11, 0);
    lv_obj_set_style_shadow_width(submit, 0, 0);
    lv_obj_set_style_pad_all(submit, 0, 0);
    lv_obj_add_event_cb(submit, OnPinSave, LV_EVENT_CLICKED, this);
    auto *enter = jetson::ui::CreateAppIcon(submit, "enter", 22);
    if (enter) {
        lv_obj_set_style_image_recolor(enter, lv_color_white(), 0);
        lv_obj_set_style_image_recolor_opa(enter, LV_OPA_COVER, 0);
    } else {
        enter = MakeLabel(submit, LV_SYMBOL_OK, &BUILTIN_ICON_FONT, 0xffffff);
    }
    lv_obj_center(enter);

    pin_error_ = MakeLabel(card, " ", &BUILTIN_SMALL_TEXT_FONT,
                           palette.sub_text);
    lv_obj_set_width(pin_error_, lv_pct(100));
    lv_obj_set_style_text_align(pin_error_, LV_TEXT_ALIGN_CENTER, 0);

    if (auto *group = jetson::LvglRuntime::Instance().keypad_group()) {
        lv_group_add_obj(group, pin_input_);
        lv_group_focus_obj(pin_input_);
    }
    lv_obj_move_foreground(pin_modal_);
    AnimateSheetIn(card, kSheetHeight);
}

void PsRemotePlayView::ClosePinModal() {
    if (!pin_modal_) return;
    lv_obj_del(pin_modal_);
    pin_modal_ = nullptr;
    pin_input_ = nullptr;
    pin_error_ = nullptr;
}

void PsRemotePlayView::AcceptPin() {
    if (!pin_input_) return;
    const char *value = lv_textarea_get_text(pin_input_);
    const std::string pin = value ? value : "";
    if (pin.empty()) {
        if (pin_error_) {
            lv_label_set_text(pin_error_, "Vui lòng nhập mã PIN PS5");
            lv_obj_set_style_text_color(pin_error_, Color(kRed), 0);
        }
        return;
    }

    // This screen is intentionally only a form.  Do not persist or log the
    // passcode; acknowledge it through the Dynamic Island and discard it.
    ClosePinModal();
    Notify("Đã nhận mã PIN PS5");
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
    settings_ip_input_ = lv_textarea_create(ip_form);
    lv_obj_set_size(settings_ip_input_, 1, 42);
    lv_obj_set_flex_grow(settings_ip_input_, 1);
    lv_obj_set_style_bg_opa(settings_ip_input_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(settings_ip_input_, 0, 0);
    lv_obj_set_style_text_font(settings_ip_input_, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(settings_ip_input_, Color(palette.text), 0);
    lv_obj_set_style_text_color(settings_ip_input_, Color(palette.sub_text),
                                LV_PART_TEXTAREA_PLACEHOLDER);
    lv_textarea_set_one_line(settings_ip_input_, true);
    lv_textarea_set_max_length(settings_ip_input_, 15);
    lv_textarea_set_accepted_chars(settings_ip_input_, "0123456789.");
    lv_textarea_set_placeholder_text(settings_ip_input_,
                                     "Chưa kết nối tới IP PS5");
    lv_textarea_set_text(settings_ip_input_, host_.c_str());

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
    if (auto *group = jetson::LvglRuntime::Instance().keypad_group()) {
        lv_group_add_obj(group, settings_ip_input_);
    }
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
    const char *input = lv_textarea_get_text(settings_ip_input_);
    std::string canonical;
    if (!CanonicalIpv4(input ? input : "", canonical)) {
        if (settings_ip_error_) {
            lv_label_set_text(settings_ip_error_, "Địa chỉ IP PS5 không hợp lệ");
            lv_obj_set_style_text_color(settings_ip_error_, Color(kRed), 0);
        }
        lv_obj_set_style_border_width(settings_ip_input_, 2, 0);
        lv_obj_set_style_border_color(settings_ip_input_, Color(kRed), 0);
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

    CloseSettingsModal();
    Notify(host_.empty() ? "Đã lưu cài đặt Remote Play"
                         : "Đã lưu IP PS5");
}

void PsRemotePlayView::OpenBluetoothSettings() {
    CloseSettingsModal();
    if (open_bluetooth_cb_) open_bluetooth_cb_();
    else Notify("Mở Cài đặt > Bluetooth để kết nối tay cầm");
}

void PsRemotePlayView::OnSignIn(lv_event_t *e) {
    LvglLockGuard lock;
    static_cast<PsRemotePlayView *>(lv_event_get_user_data(e))->OpenPinModal();
}

void PsRemotePlayView::OnPinDismiss(lv_event_t *e) {
    if (lv_event_get_target(e) != lv_event_get_current_target(e)) return;
    LvglLockGuard lock;
    static_cast<PsRemotePlayView *>(lv_event_get_user_data(e))->ClosePinModal();
}

void PsRemotePlayView::OnPinCancel(lv_event_t *e) {
    LvglLockGuard lock;
    static_cast<PsRemotePlayView *>(lv_event_get_user_data(e))->ClosePinModal();
}

void PsRemotePlayView::OnPinSave(lv_event_t *e) {
    LvglLockGuard lock;
    static_cast<PsRemotePlayView *>(lv_event_get_user_data(e))->AcceptPin();
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

} // namespace home
