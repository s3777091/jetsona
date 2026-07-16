#include "display/views/ps_remote_play_view.h"

#include "application.h"
#include "display/common/lvgl_utils.h"
#include "display/theme/ui_theme.h"
#include "fonts.h"
#include "lvgl_runtime.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace home {

using jetson::ui::Color;
using jetson::ui::LvglLockGuard;

namespace {

constexpr uint32_t kGreen = 0x30d158;
constexpr uint32_t kAmber = 0xff9f0a;
constexpr uint32_t kRed = 0xff453a;
constexpr uint32_t kBlue = 0x0a84ff;
constexpr size_t kMaxHelperOutput = 16 * 1024;

struct CommandResult {
    int exit_code = 127;
    bool timed_out = false;
    bool helper_missing = false;
    std::string output;
};

using Fields = std::map<std::string, std::string>;

std::string Trim(std::string value) {
    auto blank = [](unsigned char c) { return std::isspace(c) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(),
                                             [&](char c) { return !blank(c); }));
    value.erase(std::find_if(value.rbegin(), value.rend(),
                             [&](char c) { return !blank(c); }).base(),
                value.end());
    return value;
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string SafeUiText(std::string value, size_t max_len = 160) {
    value = Trim(std::move(value));
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c) {
                    return c < 0x20 && c != '\t';
                }),
                value.end());
    if (value.size() > max_len) value.resize(max_len);
    return value;
}

Fields ParseFields(const std::string &output) {
    Fields fields;
    size_t at = 0;
    while (at < output.size()) {
        const size_t end = output.find('\n', at);
        const size_t len = (end == std::string::npos ? output.size() : end) - at;
        std::string line = output.substr(at, len);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t equal = line.find('=');
        if (equal != std::string::npos) {
            std::string key = Lower(Trim(line.substr(0, equal)));
            std::string value = SafeUiText(line.substr(equal + 1));
            if (!key.empty() && key.size() <= 32) fields[key] = std::move(value);
        }
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return fields;
}

bool FieldBool(const Fields &fields, const char *key, bool fallback) {
    const auto it = fields.find(key);
    if (it == fields.end()) return fallback;
    const std::string value = Lower(it->second);
    if (value == "1" || value == "true" || value == "yes" || value == "on" ||
        value == "ok")
        return true;
    if (value == "0" || value == "false" || value == "no" || value == "off" ||
        value == "missing")
        return false;
    return fallback;
}

std::string Field(const Fields &fields, const char *key,
                  const std::string &fallback = "") {
    const auto it = fields.find(key);
    return it == fields.end() ? fallback : it->second;
}

bool CanonicalIpv4(const std::string &input, std::string &canonical) {
    const std::string value = Trim(input);
    in_addr address{};
    if (value.empty() || inet_pton(AF_INET, value.c_str(), &address) != 1) return false;
    // Broadcast/unspecified are syntactically IPv4 but cannot identify a PS5.
    const uint32_t host_order = ntohl(address.s_addr);
    if (host_order == 0 || host_order == 0xffffffffU) return false;
    char out[INET_ADDRSTRLEN]{};
    if (!inet_ntop(AF_INET, &address, out, sizeof(out))) return false;
    canonical = out;
    return true;
}

const char *FindHelper() {
    static constexpr const char *kInstalled =
        "/opt/jetson-fw/scripts/ps_remote_play_ctl.sh";
    static constexpr const char *kDevelopment =
        "scripts/ps_remote_play_ctl.sh";
    if (::access(kInstalled, R_OK) == 0) return kInstalled;
    if (::access(kDevelopment, R_OK) == 0) return kDevelopment;
    return nullptr;
}

/* Execute bash with an argv vector, never a shell command string. This is the
 * equivalent of strict shell quoting but stronger: a host such as
 * "1.2.3.4;..." remains one inert argument (and is rejected as IPv4 anyway).
 * Output is captured only for the key=value protocol and is never logged. */
CommandResult RunHelper(const std::vector<std::string> &args, int timeout_seconds) {
    CommandResult result;
    const char *helper = FindHelper();
    if (!helper) {
        result.helper_missing = true;
        return result;
    }

    // Prepare argv before fork. The firmware is multi-threaded, so the child
    // must avoid heap allocation (and its possibly inherited locked mutexes)
    // between fork and exec.
    std::vector<std::string> owned;
    owned.reserve(args.size() + 2);
    owned.emplace_back("bash");
    owned.emplace_back(helper);
    owned.insert(owned.end(), args.begin(), args.end());
    std::vector<char *> argv;
    argv.reserve(owned.size() + 1);
    for (auto &item : owned) argv.push_back(item.data());
    argv.push_back(nullptr);

    int pipe_fd[2] = {-1, -1};
    if (::pipe(pipe_fd) != 0) return result;

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipe_fd[0]);
        ::close(pipe_fd[1]);
        return result;
    }
    if (pid == 0) {
        (void)::setpgid(0, 0); // timeout can terminate bash and any CLI child
        ::close(pipe_fd[0]);
        ::dup2(pipe_fd[1], STDOUT_FILENO);
        ::dup2(pipe_fd[1], STDERR_FILENO);
        ::close(pipe_fd[1]);
        ::execv("/bin/bash", argv.data());
        _exit(127);
    }

    ::close(pipe_fd[1]);
    (void)::setpgid(pid, pid); // harmless if the child already exec'd/exited
    const int old_flags = ::fcntl(pipe_fd[0], F_GETFL, 0);
    if (old_flags >= 0) ::fcntl(pipe_fd[0], F_SETFL, old_flags | O_NONBLOCK);

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(std::max(1, timeout_seconds));
    bool child_done = false;
    bool eof = false;
    int status = 0;

    while (!child_done || !eof) {
        for (;;) {
            char buffer[1024];
            const ssize_t count = ::read(pipe_fd[0], buffer, sizeof(buffer));
            if (count > 0) {
                if (result.output.size() < kMaxHelperOutput) {
                    const size_t room = kMaxHelperOutput - result.output.size();
                    result.output.append(buffer, std::min(room, static_cast<size_t>(count)));
                }
                continue;
            }
            if (count == 0) eof = true;
            if (count < 0 && errno == EINTR) continue;
            break;
        }

        if (!child_done) {
            const pid_t waited = ::waitpid(pid, &status, WNOHANG);
            if (waited == pid) child_done = true;
        }
        if (child_done && eof) break;

        if (std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true;
            ::kill(-pid, SIGKILL);
            ::kill(pid, SIGKILL);
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            child_done = true;
            break;
        }

        pollfd descriptor{pipe_fd[0], POLLIN | POLLHUP, 0};
        (void)::poll(&descriptor, 1, 50);
    }

    // One final non-blocking drain after the child exits.
    for (;;) {
        char buffer[1024];
        const ssize_t count = ::read(pipe_fd[0], buffer, sizeof(buffer));
        if (count <= 0) break;
        if (result.output.size() < kMaxHelperOutput) {
            const size_t room = kMaxHelperOutput - result.output.size();
            result.output.append(buffer, std::min(room, static_cast<size_t>(count)));
        }
    }
    ::close(pipe_fd[0]);
    if (!child_done) {
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }

    if (!result.timed_out && WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
    else if (!result.timed_out && WIFSIGNALED(status)) result.exit_code = 128 + WTERMSIG(status);
    return result;
}

void StylePanel(lv_obj_t *obj, uint32_t color, uint32_t border) {
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_bg_color(obj, Color(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, 12, 0);
    lv_obj_set_style_border_color(obj, Color(border), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *MakeButton(lv_obj_t *parent, const char *text, uint32_t bg,
                     lv_event_cb_t callback, void *user_data, int width,
                     int height = 42) {
    auto *button = lv_obj_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_bg_color(button, Color(bg), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(button, 10, 0);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    auto *label = lv_label_create(button);
    lv_obj_set_style_text_font(label, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
    return button;
}

void SetBadge(lv_obj_t *label, const char *text, uint32_t color) {
    if (!label) return;
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, Color(color), 0);
    lv_obj_set_style_bg_color(label, Color(color), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_20, 0);
    lv_obj_set_style_border_color(label, Color(color), 0);
    lv_obj_set_style_border_width(label, 1, 0);
}

std::string FriendlyState(const std::string &raw) {
    const std::string state = Lower(raw);
    if (state == "ready" || state == "online") return "PS5 đang bật";
    if (state == "standby" || state == "rest") return "PS5 đang ở Chế độ nghỉ";
    if (state == "offline") return "PS5 chưa phản hồi";
    return "Chưa kiểm tra kết nối";
}

std::string FriendlyHelperMessage(const std::string &raw) {
    const std::string message = Lower(Trim(raw));
    if (message == "chiaki not installed") return "Chưa cài chiaki-ng";
    if (message == "open setup to register this ps5")
        return "Mở Đăng ký / Thiết lập để ghép PS5";
    if (message == "choose the ps5 address") return "Hãy nhập địa chỉ IPv4 của PS5";
    if (message == "network is offline") return "Jetson chưa có kết nối mạng";
    if (message == "ps5 not reachable") return "PS5 chưa phản hồi";
    if (message == "invalid ps5 address") return "Địa chỉ PS5 không hợp lệ";
    if (message == "ps5 is in rest mode") return "PS5 đang ở Chế độ nghỉ";
    if (message == "ps5 is ready") return "PS5 đang bật và sẵn sàng";
    // The helper protocol intentionally exposes only user-safe messages. Keep
    // unknown output off-screen so a future helper cannot accidentally surface
    // a registration key or passcode through a diagnostic string.
    return "";
}

std::string FriendlyController(const std::string &raw) {
    const std::string value = Lower(raw);
    if (value == "1" || value == "connected" || value == "ready") return "đã nhận";
    if (value == "0" || value == "missing" || value == "disconnected") return "chưa nhận";
    return "tự động";
}

std::string FriendlyNetwork(const std::string &raw) {
    const std::string value = Lower(raw);
    if (value == "ethernet" || value == "wired") return "Ethernet";
    if (value == "wifi" || value == "wi-fi") return "Wi-Fi";
    if (value == "offline") return "ngoại tuyến";
    return "chưa rõ";
}

} // namespace

PsRemotePlayView::PsRemotePlayView(lv_obj_t *parent, int width, int height,
                                   ClosedCb on_closed)
    : OverlayView(parent, width, height, "PS5 Remote Play", std::move(on_closed)) {
    BuildBody();
}

std::shared_ptr<PsRemotePlayView> PsRemotePlayView::Self() {
    return std::static_pointer_cast<PsRemotePlayView>(shared_from_this());
}

void PsRemotePlayView::OnStart() {
    // Start() is invoked only after make_shared and while the home display owns
    // the UI lock, so shared_from_this is safe and no nested lv_lock is needed.
    RefreshStatus();
}

void PsRemotePlayView::BuildBody() {
    const auto &p = jetson::UiTheme::Instance().Palette();
    lv_obj_set_flex_flow(body_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body_, 8, 0);
    lv_obj_set_style_pad_row(body_, 6, 0);
    lv_obj_clear_flag(body_, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Explicit installation/registration banner -----------------------
    auto *status = lv_obj_create(body_);
    StylePanel(status, p.row, p.border);
    lv_obj_set_width(status, lv_pct(100));
    lv_obj_set_height(status, 60);

    status_dot_ = lv_obj_create(status);
    lv_obj_remove_style_all(status_dot_);
    lv_obj_set_size(status_dot_, 11, 11);
    lv_obj_set_pos(status_dot_, 12, 15);
    lv_obj_set_style_radius(status_dot_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(status_dot_, LV_OPA_COVER, 0);

    status_summary_ = lv_label_create(status);
    lv_obj_set_pos(status_summary_, 32, 7);
    lv_obj_set_width(status_summary_, 380);
    lv_obj_set_style_text_font(status_summary_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(status_summary_, Color(p.text), 0);
    lv_label_set_long_mode(status_summary_, LV_LABEL_LONG_DOT);

    status_detail_ = lv_label_create(status);
    lv_obj_set_pos(status_detail_, 32, 34);
    lv_obj_set_width(status_detail_, 380);
    lv_obj_set_style_text_font(status_detail_, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(status_detail_, Color(p.sub_text), 0);
    lv_label_set_long_mode(status_detail_, LV_LABEL_LONG_DOT);

    install_badge_ = lv_label_create(status);
    lv_obj_set_size(install_badge_, 130, 26);
    lv_obj_set_pos(install_badge_, 425, 17);
    lv_obj_set_style_radius(install_badge_, 13, 0);
    lv_obj_set_style_text_font(install_badge_, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_align(install_badge_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(install_badge_, 4, 0);
    lv_label_set_long_mode(install_badge_, LV_LABEL_LONG_DOT);

    register_badge_ = lv_label_create(status);
    lv_obj_set_size(register_badge_, 141, 26);
    lv_obj_set_pos(register_badge_, 561, 17);
    lv_obj_set_style_radius(register_badge_, 13, 0);
    lv_obj_set_style_text_font(register_badge_, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_align(register_badge_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(register_badge_, 4, 0);
    lv_label_set_long_mode(register_badge_, LV_LABEL_LONG_DOT);

    refresh_btn_ = MakeButton(status, LV_SYMBOL_REFRESH, p.button, OnRefresh, this, 42, 42);
    lv_obj_set_style_text_font(lv_obj_get_child(refresh_btn_, 0), &BUILTIN_ICON_FONT, 0);
    lv_obj_align(refresh_btn_, LV_ALIGN_RIGHT_MID, -8, 0);

    // ---- Main two-column panel --------------------------------------------
    auto *content = lv_obj_create(body_);
    lv_obj_remove_style_all(content);
    lv_obj_set_width(content, lv_pct(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(content, 8, 0);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    auto *left = lv_obj_create(content);
    lv_obj_remove_style_all(left);
    lv_obj_set_width(left, 315);
    lv_obj_set_height(left, lv_pct(100));
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 6, 0);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);

    auto *host_card = lv_obj_create(left);
    StylePanel(host_card, p.row, p.border);
    lv_obj_set_width(host_card, lv_pct(100));
    lv_obj_set_height(host_card, 112);
    lv_obj_set_style_pad_all(host_card, 10, 0);

    auto *host_title = lv_label_create(host_card);
    lv_obj_set_style_text_font(host_title, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(host_title, Color(p.sub_text), 0);
    lv_label_set_text(host_title, "MÁY PS5 Ở NHÀ · IPv4");
    lv_obj_set_pos(host_title, 0, 0);

    host_label_ = lv_label_create(host_card);
    lv_obj_set_style_text_font(host_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(host_label_, Color(p.text), 0);
    lv_obj_set_pos(host_label_, 0, 28);
    lv_obj_set_width(host_label_, 205);
    lv_label_set_long_mode(host_label_, LV_LABEL_LONG_DOT);
    lv_obj_add_flag(host_label_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(host_label_, OnEditHost, LV_EVENT_CLICKED, this);

    edit_host_btn_ = MakeButton(host_card, "Sửa IP", p.button, OnEditHost, this, 72, 36);
    lv_obj_align(edit_host_btn_, LV_ALIGN_TOP_RIGHT, 0, 20);

    probe_btn_ = MakeButton(host_card, "Kiểm tra kết nối", p.button,
                            OnProbe, this, 157, 34);
    lv_obj_align(probe_btn_, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    auto *rest_badge = lv_label_create(host_card);
    lv_obj_set_style_text_font(rest_badge, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(rest_badge, Color(kGreen), 0);
    lv_label_set_text(rest_badge, "Hỗ trợ Rest Mode");
    lv_obj_align(rest_badge, LV_ALIGN_BOTTOM_RIGHT, 0, -7);

    auto *help = lv_obj_create(left);
    StylePanel(help, p.row, p.border);
    lv_obj_set_width(help, lv_pct(100));
    lv_obj_set_flex_grow(help, 1);
    lv_obj_set_style_pad_all(help, 10, 0);
    lv_obj_add_flag(help, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(help, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(help, LV_SCROLLBAR_MODE_AUTO);

    auto *help_title = lv_label_create(help);
    lv_obj_set_style_text_font(help_title, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(help_title, Color(p.text), 0);
    lv_label_set_text(help_title, "TRÊN PS5");

    auto *help_text = lv_label_create(help);
    lv_obj_set_pos(help_text, 0, 25);
    lv_obj_set_width(help_text, 293);
    lv_obj_set_style_text_font(help_text, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(help_text, Color(p.sub_text), 0);
    lv_label_set_long_mode(help_text, LV_LABEL_LONG_WRAP);
    lv_label_set_text(
        help_text,
        "1. Bật Chơi từ xa trong Cài đặt > Hệ thống.\n"
        "2. Chế độ nghỉ: bật Kết nối Internet + Cho phép bật PS5 từ mạng.\n"
        "3. PS5 và Jetson nên dùng Ethernet.");

    auto *right = lv_obj_create(content);
    lv_obj_remove_style_all(right);
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_height(right, lv_pct(100));
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(right, 6, 0);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    auto *preset_title = lv_label_create(right);
    lv_obj_set_width(preset_title, lv_pct(100));
    lv_obj_set_height(preset_title, 20);
    lv_obj_set_style_text_font(preset_title, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(preset_title, Color(p.sub_text), 0);
    lv_label_set_text(preset_title, "CHỌN CHẤT LƯỢNG CHO MÀN HÌNH 800×480");

    auto *presets = lv_obj_create(right);
    lv_obj_remove_style_all(presets);
    lv_obj_set_width(presets, lv_pct(100));
    lv_obj_set_height(presets, 124);
    lv_obj_set_flex_flow(presets, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(presets, 8, 0);
    lv_obj_clear_flag(presets, LV_OBJ_FLAG_SCROLLABLE);

    auto make_preset = [&](lv_obj_t **out, const char *name, const char *resolution,
                           const char *codec, const char *caption,
                           lv_event_cb_t callback) {
        auto *card = lv_obj_create(presets);
        StylePanel(card, p.row, p.border);
        lv_obj_set_flex_grow(card, 1);
        lv_obj_set_height(card, lv_pct(100));
        lv_obj_set_style_pad_all(card, 10, 0);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, callback, LV_EVENT_CLICKED, this);

        auto *name_label = lv_label_create(card);
        lv_obj_set_style_text_font(name_label, &BUILTIN_SMALL_TEXT_FONT, 0);
        lv_obj_set_style_text_color(name_label, Color(p.accent), 0);
        lv_label_set_text(name_label, name);

        auto *resolution_label = lv_label_create(card);
        lv_obj_set_pos(resolution_label, 0, 27);
        lv_obj_set_style_text_font(resolution_label, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(resolution_label, Color(p.text), 0);
        lv_label_set_text(resolution_label, resolution);

        auto *codec_label = lv_label_create(card);
        lv_obj_set_pos(codec_label, 0, 57);
        lv_obj_set_style_text_font(codec_label, &BUILTIN_SMALL_TEXT_FONT, 0);
        lv_obj_set_style_text_color(codec_label, Color(p.sub_text), 0);
        lv_label_set_text(codec_label, codec);

        auto *caption_label = lv_label_create(card);
        lv_obj_set_pos(caption_label, 0, 82);
        lv_obj_set_style_text_font(caption_label, &BUILTIN_SMALL_TEXT_FONT, 0);
        lv_obj_set_style_text_color(caption_label, Color(p.sub_text), 0);
        lv_label_set_text(caption_label, caption);
        *out = card;
    };

    make_preset(&smooth_card_, "MƯỢT · THỬ NGHIỆM", "540p · 60 FPS",
                "H.264 · 8 Mbps", "Cần thử trên Jetson Nano", OnSmooth);
    make_preset(&quality_card_, "HÌNH ẢNH RÕ", "720p · 30 FPS",
                "H.264 · 10 Mbps", "Chi tiết cao hơn", OnQuality);

    auto *runtime = lv_obj_create(right);
    StylePanel(runtime, p.row, p.border);
    lv_obj_set_width(runtime, lv_pct(100));
    lv_obj_set_flex_grow(runtime, 1);
    lv_obj_set_style_pad_all(runtime, 10, 0);

    auto *runtime_title = lv_label_create(runtime);
    lv_obj_set_style_text_font(runtime_title, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(runtime_title, Color(p.text), 0);
    lv_label_set_text(runtime_title, "TRẠNG THÁI PHÁT");

    runtime_detail_ = lv_label_create(runtime);
    lv_obj_set_pos(runtime_detail_, 0, 27);
    lv_obj_set_width(runtime_detail_, lv_pct(100));
    lv_obj_set_style_text_font(runtime_detail_, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(runtime_detail_, Color(p.sub_text), 0);
    lv_label_set_long_mode(runtime_detail_, LV_LABEL_LONG_WRAP);

    // ---- Action footer -----------------------------------------------------
    auto *actions = lv_obj_create(body_);
    lv_obj_remove_style_all(actions);
    lv_obj_set_width(actions, lv_pct(100));
    lv_obj_set_height(actions, 54);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(actions, 8, 0);
    lv_obj_clear_flag(actions, LV_OBJ_FLAG_SCROLLABLE);

    action_status_ = lv_label_create(actions);
    lv_obj_set_flex_grow(action_status_, 1);
    lv_obj_set_style_text_font(action_status_, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(action_status_, Color(p.sub_text), 0);
    lv_label_set_long_mode(action_status_, LV_LABEL_LONG_DOT);

    configure_btn_ = MakeButton(actions, "Đăng ký / Thiết lập", p.button,
                                OnConfigure, this, 174, 46);
    configure_btn_label_ = lv_obj_get_child(configure_btn_, 0);

    play_btn_ = MakeButton(actions, "Chơi ngay", kBlue, OnPlay, this, 145, 46);

    UpdateUi();
}

void PsRemotePlayView::SetActionStatus(const std::string &message, bool error) {
    if (!action_status_) return;
    const auto &p = jetson::UiTheme::Instance().Palette();
    lv_label_set_text(action_status_, message.c_str());
    lv_obj_set_style_text_color(action_status_, Color(error ? kRed : p.sub_text), 0);
}

void PsRemotePlayView::SetBusy(bool busy, const std::string &message) {
    busy_ = busy;
    lv_obj_t *controls[] = {refresh_btn_, edit_host_btn_, probe_btn_, smooth_card_,
                            quality_card_, configure_btn_, play_btn_};
    for (auto *control : controls) {
        if (!control) continue;
        if (busy) lv_obj_add_state(control, LV_STATE_DISABLED);
        else lv_obj_clear_state(control, LV_STATE_DISABLED);
    }
    if (!message.empty()) SetActionStatus(message);
}

void PsRemotePlayView::UpdatePresetCards() {
    const auto &p = jetson::UiTheme::Instance().Palette();
    const auto update = [&](lv_obj_t *card, bool selected) {
        if (!card) return;
        lv_obj_set_style_bg_color(card, Color(selected ? p.row_active : p.row), 0);
        lv_obj_set_style_border_color(card, Color(selected ? p.accent : p.border), 0);
        lv_obj_set_style_border_width(card, selected ? 2 : 1, 0);
    };
    update(smooth_card_, preset_ == Preset::Smooth);
    update(quality_card_, preset_ == Preset::Quality);
}

void PsRemotePlayView::UpdateUi() {
    const auto &p = jetson::UiTheme::Instance().Palette();
    const uint32_t dot_color = !status_loaded_ ? kAmber
                                : !installed_ ? kRed
                                : !registered_ ? kAmber
                                : (Lower(remote_state_) == "offline" ? kRed : kGreen);
    if (status_dot_) lv_obj_set_style_bg_color(status_dot_, Color(dot_color), 0);

    std::string summary;
    if (!status_loaded_) summary = "Đang kiểm tra chiaki-ng và cấu hình PS5...";
    else if (!installed_) summary = "Chưa cài chiaki-ng";
    else if (!registered_) summary = "Chiaki-ng đã cài · PS5 chưa đăng ký";
    else summary = "Sẵn sàng chơi PS5";
    if (status_summary_) lv_label_set_text(status_summary_, summary.c_str());

    std::string detail = nickname_.empty() ? "PS5 tại nhà" : nickname_;
    detail += " · ";
    detail += host_.empty() ? "chưa nhập IPv4" : host_;
    detail += " · " + FriendlyState(remote_state_);
    if (status_detail_) lv_label_set_text(status_detail_, detail.c_str());

    SetBadge(install_badge_, installed_ ? "CHIAKI: ĐÃ CÀI" : "CHIAKI: CHƯA CÀI",
             installed_ ? kGreen : kRed);
    SetBadge(register_badge_, registered_ ? "PS5: ĐÃ ĐĂNG KÝ" : "PS5: CHƯA ĐĂNG KÝ",
             registered_ ? kGreen : kAmber);

    if (host_label_) lv_label_set_text(host_label_, host_.empty() ? "Chạm để nhập IP" : host_.c_str());

    std::string runtime = "Tay cầm: " + FriendlyController(controller_) +
                          " · Mạng: " + FriendlyNetwork(network_);
    runtime += "\nLuồng: ";
    runtime += preset_ == Preset::Smooth ? "540p60 · H.264 · 8 Mbps"
                                         : "720p30 · H.264 · 10 Mbps";
    runtime += " · hiển thị 800×450, viền 15 px";
    runtime += "\nHiệu năng giải mã phần cứng cần được kiểm tra trên thiết bị.";
    if (runtime_detail_) lv_label_set_text(runtime_detail_, runtime.c_str());

    if (configure_btn_label_) {
        const char *text = !installed_ ? "Cần cài Chiaki"
                           : !registered_ ? "Đăng ký PS5"
                                          : "Thiết lập lại";
        lv_label_set_text(configure_btn_label_, text);
    }
    UpdatePresetCards();
}

void PsRemotePlayView::RefreshStatus() {
    if (busy_) return;
    SetBusy(true, "Đang đọc cấu hình Remote Play...");
    std::weak_ptr<PsRemotePlayView> weak = Self();
    std::thread([weak]() {
        CommandResult result = RunHelper({"status"}, 15);
        Application::GetInstance().Schedule([weak, result = std::move(result)]() mutable {
            auto self = weak.lock();
            if (!self) return;
            LvglLockGuard lock;
            self->status_loaded_ = true;
            self->SetBusy(false);
            if (result.helper_missing) {
                self->installed_ = false;
                self->registered_ = false;
                self->UpdateUi();
                self->SetActionStatus("Không tìm thấy ps_remote_play_ctl.sh", true);
                return;
            }
            if (result.timed_out) {
                self->UpdateUi();
                self->SetActionStatus("Kiểm tra cấu hình quá thời gian", true);
                return;
            }
            const Fields fields = ParseFields(result.output);
            self->installed_ = FieldBool(fields, "installed", self->installed_);
            self->registered_ = FieldBool(fields, "registered", self->registered_);
            self->nickname_ = Field(fields, "nickname", self->nickname_);
            const std::string saved_host = Field(fields, "host");
            std::string canonical;
            if (CanonicalIpv4(saved_host, canonical)) self->host_ = canonical;
            const std::string saved_preset = Lower(Field(fields, "preset"));
            if (saved_preset == "quality") self->preset_ = Preset::Quality;
            else if (saved_preset == "smooth") self->preset_ = Preset::Smooth;
            self->controller_ = Field(fields, "controller", self->controller_);
            self->network_ = Field(fields, "network", self->network_);
            self->remote_state_ = Field(fields, "state", self->remote_state_);
            self->helper_message_ = FriendlyHelperMessage(Field(fields, "message"));
            self->UpdateUi();
            if (result.exit_code != 0) {
                self->SetActionStatus(self->helper_message_.empty()
                                          ? "Không đọc được trạng thái Remote Play"
                                          : self->helper_message_,
                                      true);
            } else if (!self->helper_message_.empty()) {
                self->SetActionStatus(self->helper_message_);
            } else if (self->host_.empty()) {
                self->SetActionStatus("Nhập IPv4 của PS5 để bắt đầu");
            } else if (!self->registered_) {
                self->SetActionStatus("Nhấn Đăng ký PS5 để ghép máy lần đầu");
            } else {
                self->SetActionStatus("Cấu hình đã sẵn sàng");
            }
        });
    }).detach();
}

void PsRemotePlayView::ProbeHost() {
    if (busy_) return;
    std::string canonical;
    if (!CanonicalIpv4(host_, canonical)) {
        SetActionStatus("Hãy nhập IPv4 hợp lệ của PS5", true);
        OpenHostModal();
        return;
    }
    SetBusy(true, "Đang kiểm tra PS5 trong mạng...");
    std::weak_ptr<PsRemotePlayView> weak = Self();
    std::thread([weak, host = std::move(canonical)]() {
        CommandResult result = RunHelper({"probe", "--host", host}, 12);
        Application::GetInstance().Schedule(
            [weak, result = std::move(result)]() mutable {
                auto self = weak.lock();
                if (!self) return;
                LvglLockGuard lock;
                self->SetBusy(false);
                if (result.helper_missing) {
                    self->SetActionStatus("Không tìm thấy công cụ kiểm tra PS5", true);
                    return;
                }
                if (result.timed_out) {
                    self->remote_state_ = "offline";
                    self->UpdateUi();
                    self->SetActionStatus("PS5 không phản hồi (quá thời gian)", true);
                    return;
                }
                const Fields fields = ParseFields(result.output);
                self->remote_state_ = Field(fields, "state",
                                            result.exit_code == 0 ? "ready" : "offline");
                self->helper_message_ = FriendlyHelperMessage(Field(fields, "message"));
                self->UpdateUi();
                const bool reachable = result.exit_code == 0 &&
                                       (Lower(self->remote_state_) == "ready" ||
                                        Lower(self->remote_state_) == "standby");
                if (!self->helper_message_.empty()) {
                    self->SetActionStatus(self->helper_message_, !reachable);
                } else if (Lower(self->remote_state_) == "standby") {
                    self->SetActionStatus("PS5 đang nghỉ và có thể được đánh thức");
                } else if (reachable) {
                    self->SetActionStatus("Đã tìm thấy PS5 · kết nối sẵn sàng");
                } else {
                    self->SetActionStatus("Không tìm thấy PS5 tại địa chỉ này", true);
                }
            });
    }).detach();
}

void PsRemotePlayView::SaveThenLaunch(bool configure) {
    if (busy_) return;
    if (!installed_) {
        SetActionStatus("Chưa cài chiaki-ng ARM64 · xem hướng dẫn cài đặt", true);
        return;
    }
    std::string canonical;
    if (!CanonicalIpv4(host_, canonical)) {
        SetActionStatus("Hãy nhập IPv4 hợp lệ trước khi tiếp tục", true);
        OpenHostModal();
        return;
    }
    if (!configure && !registered_) {
        SetActionStatus("Cần đăng ký PS5 trước khi Chơi ngay", true);
        return;
    }

    const std::string preset = preset_ == Preset::Smooth ? "smooth" : "quality";
    SetBusy(true, "Đang lưu cấu hình an toàn...");
    std::weak_ptr<PsRemotePlayView> weak = Self();
    std::thread([weak, host = std::move(canonical), preset, configure]() {
        CommandResult result =
            RunHelper({"save", "--host", host, "--preset", preset}, 8);
        Application::GetInstance().Schedule(
            [weak, result = std::move(result), configure]() mutable {
                auto self = weak.lock();
                if (!self) return;
                LvglLockGuard lock;
                self->SetBusy(false);
                const Fields fields = ParseFields(result.output);
                const std::string message = FriendlyHelperMessage(Field(fields, "message"));
                if (result.helper_missing) {
                    self->SetActionStatus("Không thể lưu: thiếu ps_remote_play_ctl.sh", true);
                    return;
                }
                if (result.timed_out) {
                    self->SetActionStatus("Lưu cấu hình quá thời gian", true);
                    return;
                }
                if (result.exit_code != 0) {
                    self->SetActionStatus(message.empty() ? "Không lưu được cấu hình Remote Play"
                                                          : message,
                                          true);
                    return;
                }
                self->SetActionStatus(configure ? "Đã lưu · đang mở thiết lập PS5..."
                                                : "Đã lưu · đang mở Remote Play...");
                auto launch = self->launch_cb_;
                if (launch) launch(configure);
            });
    }).detach();
}

void PsRemotePlayView::OpenHostModal() {
    if (host_modal_) return;
    const auto &p = jetson::UiTheme::Instance().Palette();

    host_modal_ = lv_obj_create(overlay_);
    lv_obj_remove_style_all(host_modal_);
    lv_obj_set_size(host_modal_, width_, height_);
    lv_obj_set_pos(host_modal_, 0, 0);
    lv_obj_set_style_bg_color(host_modal_, Color(p.scrim), 0);
    lv_obj_set_style_bg_opa(host_modal_, LV_OPA_80, 0);
    lv_obj_add_flag(host_modal_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(host_modal_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(host_modal_, OnModalDismiss, LV_EVENT_CLICKED, this);

    auto *card = lv_obj_create(host_modal_);
    StylePanel(card, p.panel, p.border);
    lv_obj_set_size(card, 600, 390);
    lv_obj_center(card);
    lv_obj_set_style_pad_all(card, 14, 0);

    auto *title = lv_label_create(card);
    lv_obj_set_style_text_font(title, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(title, Color(p.text), 0);
    lv_label_set_text(title, "Nhập địa chỉ IPv4 của PS5");

    auto *hint = lv_label_create(card);
    lv_obj_set_pos(hint, 0, 27);
    lv_obj_set_style_text_font(hint, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(hint, Color(p.sub_text), 0);
    lv_label_set_text(hint, "Ví dụ: 192.168.1.50 · nên đặt IP tĩnh/DHCP reservation");

    host_input_ = lv_textarea_create(card);
    lv_obj_set_pos(host_input_, 0, 52);
    lv_obj_set_size(host_input_, 572, 48);
    lv_obj_set_style_text_font(host_input_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(host_input_, Color(p.text), 0);
    lv_obj_set_style_bg_color(host_input_, Color(p.bg), 0);
    lv_obj_set_style_bg_opa(host_input_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(host_input_, Color(p.accent), 0);
    lv_obj_set_style_border_width(host_input_, 2, 0);
    lv_obj_set_style_radius(host_input_, 10, 0);
    lv_textarea_set_one_line(host_input_, true);
    lv_textarea_set_max_length(host_input_, 15);
    lv_textarea_set_accepted_chars(host_input_, "0123456789.");
    lv_textarea_set_placeholder_text(host_input_, "192.168.1.50");
    lv_textarea_set_text(host_input_, host_.c_str());

    host_keyboard_ = lv_keyboard_create(card);
    lv_obj_set_pos(host_keyboard_, 42, 109);
    lv_obj_set_size(host_keyboard_, 488, 225);
    lv_obj_set_style_bg_color(host_keyboard_, Color(p.panel), 0);
    lv_obj_set_style_text_font(host_keyboard_, &lv_font_montserrat_14, 0);
    static const char *const kIpv4Map[] = {
        "1", "2", "3", LV_SYMBOL_BACKSPACE, "\n",
        "4", "5", "6", ".", "\n",
        "7", "8", "9", "0", "\n",
        LV_SYMBOL_CLOSE, LV_SYMBOL_OK, "",
    };
    static const lv_buttonmatrix_ctrl_t kIpv4Controls[] = {
        static_cast<lv_buttonmatrix_ctrl_t>(1),
        static_cast<lv_buttonmatrix_ctrl_t>(1),
        static_cast<lv_buttonmatrix_ctrl_t>(1),
        static_cast<lv_buttonmatrix_ctrl_t>(LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2),
        static_cast<lv_buttonmatrix_ctrl_t>(1),
        static_cast<lv_buttonmatrix_ctrl_t>(1),
        static_cast<lv_buttonmatrix_ctrl_t>(1),
        static_cast<lv_buttonmatrix_ctrl_t>(1),
        static_cast<lv_buttonmatrix_ctrl_t>(1),
        static_cast<lv_buttonmatrix_ctrl_t>(1),
        static_cast<lv_buttonmatrix_ctrl_t>(1),
        static_cast<lv_buttonmatrix_ctrl_t>(1),
        static_cast<lv_buttonmatrix_ctrl_t>(LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2),
        static_cast<lv_buttonmatrix_ctrl_t>(LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2),
    };
    lv_keyboard_set_map(host_keyboard_, LV_KEYBOARD_MODE_USER_1,
                        kIpv4Map, kIpv4Controls);
    lv_keyboard_set_mode(host_keyboard_, LV_KEYBOARD_MODE_USER_1);
    lv_keyboard_set_textarea(host_keyboard_, host_input_);
    lv_obj_add_event_cb(host_keyboard_, OnKeyboardReady, LV_EVENT_READY, this);
    lv_obj_add_event_cb(host_keyboard_, OnKeyboardCancel, LV_EVENT_CANCEL, this);

    host_error_ = lv_label_create(card);
    lv_obj_set_pos(host_error_, 0, 346);
    lv_obj_set_width(host_error_, 572);
    lv_obj_set_style_text_font(host_error_, &BUILTIN_SMALL_TEXT_FONT, 0);
    lv_obj_set_style_text_color(host_error_, Color(p.sub_text), 0);
    lv_obj_set_style_text_align(host_error_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(host_error_, "✓ để lưu · × để hủy");

    if (auto *group = jetson::LvglRuntime::Instance().keypad_group()) {
        lv_group_add_obj(group, host_input_);
        lv_group_focus_obj(host_input_);
    }
    lv_obj_move_foreground(host_modal_);
}

void PsRemotePlayView::CloseHostModal() {
    if (!host_modal_) return;
    if (host_keyboard_) lv_keyboard_set_textarea(host_keyboard_, nullptr);
    lv_obj_del(host_modal_);
    host_modal_ = nullptr;
    host_input_ = nullptr;
    host_keyboard_ = nullptr;
    host_error_ = nullptr;
}

void PsRemotePlayView::AcceptHostModal() {
    if (!host_input_) return;
    const char *text = lv_textarea_get_text(host_input_);
    std::string canonical;
    if (!CanonicalIpv4(text ? text : "", canonical)) {
        if (host_error_) {
            lv_label_set_text(host_error_, "IPv4 không hợp lệ · cần đủ 4 nhóm, mỗi nhóm 0–255");
            lv_obj_set_style_text_color(host_error_, Color(kRed), 0);
        }
        return;
    }
    host_ = std::move(canonical);
    remote_state_.clear();
    helper_message_.clear();
    CloseHostModal();
    UpdateUi();
    SetActionStatus("Đã nhận IP · nhấn Kiểm tra kết nối hoặc tiếp tục thiết lập");
}

void PsRemotePlayView::OnRefresh(lv_event_t *e) {
    LvglLockGuard lock;
    static_cast<PsRemotePlayView *>(lv_event_get_user_data(e))->RefreshStatus();
}

void PsRemotePlayView::OnEditHost(lv_event_t *e) {
    LvglLockGuard lock;
    auto *self = static_cast<PsRemotePlayView *>(lv_event_get_user_data(e));
    if (!self->busy_) self->OpenHostModal();
}

void PsRemotePlayView::OnProbe(lv_event_t *e) {
    LvglLockGuard lock;
    static_cast<PsRemotePlayView *>(lv_event_get_user_data(e))->ProbeHost();
}

void PsRemotePlayView::OnSmooth(lv_event_t *e) {
    LvglLockGuard lock;
    auto *self = static_cast<PsRemotePlayView *>(lv_event_get_user_data(e));
    if (self->busy_) return;
    self->preset_ = Preset::Smooth;
    self->UpdateUi();
    self->SetActionStatus("Đã chọn 540p 60 FPS thử nghiệm · nếu giật, dùng 720p30");
}

void PsRemotePlayView::OnQuality(lv_event_t *e) {
    LvglLockGuard lock;
    auto *self = static_cast<PsRemotePlayView *>(lv_event_get_user_data(e));
    if (self->busy_) return;
    self->preset_ = Preset::Quality;
    self->UpdateUi();
    self->SetActionStatus("Đã chọn 720p 30 FPS · ưu tiên hình ảnh");
}

void PsRemotePlayView::OnConfigure(lv_event_t *e) {
    LvglLockGuard lock;
    static_cast<PsRemotePlayView *>(lv_event_get_user_data(e))->SaveThenLaunch(true);
}

void PsRemotePlayView::OnPlay(lv_event_t *e) {
    LvglLockGuard lock;
    static_cast<PsRemotePlayView *>(lv_event_get_user_data(e))->SaveThenLaunch(false);
}

void PsRemotePlayView::OnModalDismiss(lv_event_t *e) {
    // Only a click whose current target is the scrim dismisses the modal.
    if (lv_event_get_target(e) != lv_event_get_current_target(e)) return;
    LvglLockGuard lock;
    static_cast<PsRemotePlayView *>(lv_event_get_user_data(e))->CloseHostModal();
}

void PsRemotePlayView::OnKeyboardReady(lv_event_t *e) {
    LvglLockGuard lock;
    static_cast<PsRemotePlayView *>(lv_event_get_user_data(e))->AcceptHostModal();
}

void PsRemotePlayView::OnKeyboardCancel(lv_event_t *e) {
    LvglLockGuard lock;
    static_cast<PsRemotePlayView *>(lv_event_get_user_data(e))->CloseHostModal();
}

} // namespace home
