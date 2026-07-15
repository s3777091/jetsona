#include "terminal_view.h"
#include "application.h"
#include "esp_log.h"
#include "fonts.h"
#include "ui_theme.h"
#include "lvgl_runtime.h"

#include <lvgl.h>

#include <pty.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#define TAG "TerminalView"

namespace home {

namespace {
lv_color_t Color(uint32_t rgb) {
    return lv_color_make((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

class LvLockGuard {
public:
    LvLockGuard() { lv_lock(); }
    ~LvLockGuard() { lv_unlock(); }
};
} // namespace

TerminalView::TerminalView(lv_obj_t *parent, int width, int height, ClosedCb on_closed)
    : OverlayView(parent, width, height, "Terminal", std::move(on_closed)) {
    const auto &p = jetson::UiTheme::Instance().Palette();

    lv_obj_set_flex_flow(body_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body_, 6, 0);
    lv_obj_set_style_pad_row(body_, 6, 0);
    lv_obj_clear_flag(body_, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Output area (scrollable) ----
    output_ = lv_obj_create(body_);
    lv_obj_remove_style_all(output_);
    lv_obj_set_flex_grow(output_, 1);
    lv_obj_set_width(output_, lv_pct(100));
    lv_obj_set_style_bg_color(output_, Color(0x000000), 0);
    lv_obj_set_style_bg_opa(output_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(output_, 8, 0);
    lv_obj_set_style_radius(output_, 8, 0);
    lv_obj_set_style_border_color(output_, Color(p.border), 0);
    lv_obj_set_style_border_width(output_, 1, 0);
    lv_obj_add_flag(output_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(output_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(output_, LV_SCROLLBAR_MODE_AUTO);

    output_label_ = lv_label_create(output_);
    lv_obj_set_style_text_font(output_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(output_label_, Color(0xe6e6e6), 0);
    lv_label_set_long_mode(output_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(output_label_, lv_pct(100));
    lv_label_set_text(output_label_,
        "Terminal (root shell)\n"
        "Nhap lenh va nhan Gui/Enter. Ho tro sudo, pipe, redirect.\n");

    // ---- Input row (textarea + send) ----
    auto *input_row = lv_obj_create(body_);
    lv_obj_remove_style_all(input_row);
    lv_obj_set_width(input_row, lv_pct(100));
    lv_obj_set_height(input_row, 52);
    lv_obj_set_style_pad_all(input_row, 0, 0);
    lv_obj_set_style_pad_column(input_row, 6, 0);
    lv_obj_set_flex_flow(input_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(input_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(input_row, LV_OBJ_FLAG_SCROLLABLE);

    input_ = lv_textarea_create(input_row);
    lv_obj_set_flex_grow(input_, 1);
    lv_obj_set_height(input_, 48);
    lv_obj_set_style_text_font(input_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(input_, Color(p.text), 0);
    lv_obj_set_style_bg_color(input_, Color(p.row), 0);
    lv_obj_set_style_bg_opa(input_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(input_, Color(p.border), 0);
    lv_obj_set_style_border_width(input_, 1, 0);
    lv_obj_set_style_radius(input_, 10, 0);
    lv_textarea_set_placeholder_text(input_, "lenh...");
    lv_textarea_set_one_line(input_, true);
    lv_textarea_set_max_length(input_, 2000);
    lv_obj_add_event_cb(input_, OnInputReady, LV_EVENT_READY, this);
    lv_obj_add_event_cb(input_, OnInputFocused, LV_EVENT_FOCUSED, this);
    /* Let the USB keyboard type into this textarea via the keypad group. */
    if (auto *g = jetson::LvglRuntime::Instance().keypad_group()) lv_group_add_obj(g, input_);

    send_btn_ = lv_button_create(input_row);
    lv_obj_set_size(send_btn_, 80, 48);
    lv_obj_set_style_bg_color(send_btn_, Color(p.accent), 0);
    lv_obj_set_style_radius(send_btn_, 10, 0);
    auto *send_lbl = lv_label_create(send_btn_);
    lv_obj_set_style_text_font(send_lbl, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(send_lbl, lv_color_white(), 0);
    lv_label_set_text(send_lbl, "Gui");
    lv_obj_center(send_lbl);
    lv_obj_add_event_cb(send_btn_, OnSendClicked, LV_EVENT_CLICKED, this);

    // ---- On-screen keyboard ----
    keyboard_ = lv_keyboard_create(body_);
    lv_obj_set_width(keyboard_, lv_pct(100));
    lv_obj_set_height(keyboard_, 180);
    lv_keyboard_set_textarea(keyboard_, input_);
    lv_obj_set_style_bg_color(keyboard_, Color(p.panel), 0);
    lv_obj_add_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
}

TerminalView::~TerminalView() {
    StopShell();
}

void TerminalView::OnStart() {
    if (!SpawnShell()) {
        SetStatus("Khong mo duoc shell (forkpty loi)");
        AppendOutput("\n[Loi: khong the mo terminal. Kiem tra /bin/sh va quyen.]\n");
        return;
    }
    SetStatus("Shell san sang");
    reader_ = std::thread([this]() { ReaderLoop(); });
}

bool TerminalView::SpawnShell() {
    pid_t pid = forkpty(&master_fd_, nullptr, nullptr, nullptr);
    if (pid < 0) {
        ESP_LOGE(TAG, "forkpty failed: %s", std::strerror(errno));
        master_fd_ = -1;
        return false;
    }
    if (pid == 0) {
        // ---- child ----
        // Use TERM=dumb so most programs avoid colour/cursor escape sequences
        // (keeps the plain-text output area readable).
        setenv("TERM", "dumb", 1);
        setenv("PS1", "jetson# ", 1);
        // Disable ls colour by default.
        setenv("LS_COLORS", "", 1);
        unsetenv("CLICOLOR");
        unsetenv("CLICOLOR_FORCE");
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        char *argv[] = {(char *)"sh", (char *)"-i", nullptr};
        execvp("/bin/sh", argv);
        // exec failed
        _exit(127);
    }
    // ---- parent ----
    child_pid_ = pid;

    // Leave the PTY in its default (cooked + ECHO) mode: the shell echoes typed
    // input back through the PTY so the user sees their command in the output,
    // and ICANON means the shell only receives a full line after the newline we
    // send. With TERM=dumb set above, output stays plain-text readable.
    return true;
}

void TerminalView::ReaderLoop() {
    char buf[1024];
    while (!stopping_) {
        ssize_t n = read(master_fd_, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            break; // fd closed / error -> exit
        }
        if (n == 0) break; // child closed the PTY
        AppendOutput(std::string(buf, (size_t)n));
    }
    AppendOutput("\n[shell da thoat]\n");
}

void TerminalView::StopShell() {
    if (stopping_.exchange(true)) {
        if (reader_.joinable()) reader_.join();
        return;
    }
    // Tell the child to exit, then close the master to unblock the reader.
    if (master_fd_ >= 0) {
        if (child_pid_ > 0) {
            const char *exit_cmd = "exit\n";
            write(master_fd_, exit_cmd, std::strlen(exit_cmd));
            // Give it a moment, then force-kill.
            for (int i = 0; i < 20 && child_pid_ > 0; ++i) {
                int status = 0;
                pid_t w = waitpid(child_pid_, &status, WNOHANG);
                if (w == child_pid_) { child_pid_ = -1; break; }
                usleep(50 * 1000);
            }
            if (child_pid_ > 0) {
                kill(child_pid_, SIGHUP);
                kill(child_pid_, SIGKILL);
                int status = 0;
                waitpid(child_pid_, &status, 0);
                child_pid_ = -1;
            }
        }
        close(master_fd_);
        master_fd_ = -1;
    }
    if (reader_.joinable()) reader_.join();
}

void TerminalView::AppendOutput(const std::string &text) {
    {
        std::lock_guard<std::mutex> lk(out_mtx_);
        buffer_ += text;
        // Cap buffer so long sessions (e.g. `dmesg`) don't blow up memory: keep
        // the last ~32 KB.
        if (buffer_.size() > 32768) {
            buffer_.erase(0, buffer_.size() - 32768);
        }
    }
    std::weak_ptr<TerminalView> weak = std::static_pointer_cast<TerminalView>(shared_from_this());
    std::string snapshot;
    {
        std::lock_guard<std::mutex> lk(out_mtx_);
        snapshot = buffer_;
    }
    Application::GetInstance().Schedule([weak, snapshot = std::move(snapshot)]() {
        auto sp = weak.lock();
        if (!sp) return;
        LvLockGuard lk;
        if (!sp->output_label_) return;
        lv_label_set_text(sp->output_label_, snapshot.c_str());
        if (sp->output_) lv_obj_scroll_to_y(sp->output_, LV_COORD_MAX, LV_ANIM_OFF);
    });
}

void TerminalView::DoSend() {
    if (master_fd_ < 0) return;
    std::string text;
    {
        LvLockGuard lk;
        const char *txt = lv_textarea_get_text(input_);
        text = txt ? txt : "";
        lv_textarea_set_text(input_, "");
    }
    text += "\n";
    ssize_t off = 0;
    while (off < (ssize_t)text.size()) {
        ssize_t w = write(master_fd_, text.data() + off, text.size() - off);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            break;
        }
        off += w;
    }
}

void TerminalView::OnSendClicked(lv_event_t *e) {
    auto *self = static_cast<TerminalView *>(lv_event_get_user_data(e));
    self->DoSend();
}

void TerminalView::OnInputReady(lv_event_t *e) {
    // Enter pressed on the one-line textarea -> send.
    auto *self = static_cast<TerminalView *>(lv_event_get_user_data(e));
    self->DoSend();
}

void TerminalView::OnInputFocused(lv_event_t *e) {
    auto *self = static_cast<TerminalView *>(lv_event_get_user_data(e));
    LvLockGuard lk;
    if (self->keyboard_) lv_obj_clear_flag(self->keyboard_, LV_OBJ_FLAG_HIDDEN);
}

} // namespace home