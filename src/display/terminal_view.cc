#include "terminal_view.h"
#include "esp_log.h"
#include "fonts.h"
#include "ui_theme.h"
#include "lvgl_runtime.h"

#include <lvgl.h>

#include <pty.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

#include <cctype>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>

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

    // macOS Terminal look: the whole body is a dark canvas.
    lv_obj_set_flex_flow(body_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(body_, Color(0x1b1b1b), 0);
    lv_obj_set_style_bg_opa(body_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(body_, 8, 0);
    lv_obj_set_style_pad_row(body_, 8, 0);
    lv_obj_clear_flag(body_, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Output canvas (scrollable, fills the space above the input) ----
    output_ = lv_obj_create(body_);
    lv_obj_remove_style_all(output_);
    lv_obj_set_flex_grow(output_, 1);
    lv_obj_set_width(output_, lv_pct(100));
    lv_obj_set_style_bg_color(output_, Color(0x1e1e1e), 0);
    lv_obj_set_style_bg_opa(output_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(output_, 10, 0);
    lv_obj_set_style_radius(output_, 8, 0);
    lv_obj_set_style_border_color(output_, Color(0x333333), 0);
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
        "Nhap lenh + Enter. Ho tro sudo, pipe, redirect.\n");

    // ---- Input row: prompt + textarea + send ----
    auto *input_row = lv_obj_create(body_);
    lv_obj_remove_style_all(input_row);
    lv_obj_set_width(input_row, lv_pct(100));
    lv_obj_set_height(input_row, 52);
    lv_obj_set_style_pad_all(input_row, 0, 0);
    lv_obj_set_style_pad_column(input_row, 6, 0);
    lv_obj_set_flex_flow(input_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(input_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(input_row, LV_OBJ_FLAG_SCROLLABLE);

    prompt_label_ = lv_label_create(input_row);
    lv_obj_set_style_text_font(prompt_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(prompt_label_, Color(p.accent), 0);
    lv_label_set_text(prompt_label_, ">");

    input_ = lv_textarea_create(input_row);
    lv_obj_set_flex_grow(input_, 1);
    lv_obj_set_height(input_, 48);
    lv_obj_set_style_text_font(input_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(input_, Color(0xe6e6e6), 0);
    lv_obj_set_style_bg_color(input_, Color(0x2a2a2a), 0);
    lv_obj_set_style_bg_opa(input_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(input_, Color(0x333333), 0);
    lv_obj_set_style_border_width(input_, 1, 0);
    lv_obj_set_style_radius(input_, 10, 0);
    lv_textarea_set_placeholder_text(input_, "lenh...");
    lv_textarea_set_one_line(input_, true);
    lv_textarea_set_max_length(input_, 2000);
    lv_obj_add_event_cb(input_, OnInputReady, LV_EVENT_READY, this);
    /* USB keyboard types into this textarea via the keypad group. */
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
}

TerminalView::~TerminalView() {
    StopShell();
    // Tear down the flush timer before the base deletes the overlay; the timer
    // callback touches output_label_ (a child of overlay_). lv_lock is not
    // recursive, so release it before ~OverlayView re-takes it.
    lv_lock();
    if (flush_timer_) { lv_timer_del(flush_timer_); flush_timer_ = nullptr; }
    lv_unlock();
}

void TerminalView::OnStart() {
    if (!SpawnShell()) {
        SetStatus("Khong mo duoc shell (forkpty loi)");
        AppendOutput("\n[Loi: khong the mo terminal. Kiem tra /bin/sh va quyen.]\n");
        return;
    }
    SetStatus("Shell san sang");
    reader_ = std::thread([this]() { ReaderLoop(); });
    // Coalesce reader output -> UI at ~12 Hz (80 ms). The reader only marks the
    // buffer dirty; this timer is the single path that re-renders the label, so
    // fast shell output never triggers more than ~12 re-wraps per second.
    flush_timer_ = lv_timer_create(OnFlushTimer, 80, this);
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
        // TERM=dumb + empty LS_COLORS keep most programs from emitting colour /
        // cursor escape sequences, so the plain-text label stays readable. Any
        // stray ANSI / CR is also stripped by StripAnsi in the reader.
        setenv("TERM", "dumb", 1);
        setenv("PS1", "jetson# ", 1);
        setenv("LS_COLORS", "", 1);
        unsetenv("CLICOLOR");
        unsetenv("CLICOLOR_FORCE");
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        char *argv[] = {(char *)"sh", (char *)"-i", nullptr};
        execvp("/bin/sh", argv);
        _exit(127); // exec failed
    }
    // ---- parent ----
    child_pid_ = pid;
    // PTY stays in cooked + ECHO mode: the shell echoes typed input back so the
    // user sees their command, and ICANON delivers a full line only after the
    // newline we send.
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
        AppendOutput(StripAnsi(std::string(buf, (size_t)n)));
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
        // Cap the visible scrollback so long sessions (e.g. `dmesg`) don't grow
        // without bound and wrap stays cheap: keep the last kBufCap bytes.
        if (buffer_.size() > kBufCap) {
            buffer_.erase(0, buffer_.size() - kBufCap);
        }
    }
    dirty_.store(true);
}

void TerminalView::Flush() {
    if (!dirty_.exchange(false)) return;
    std::string snapshot;
    {
        std::lock_guard<std::mutex> lk(out_mtx_);
        snapshot = buffer_;
    }
    LvLockGuard lk;
    if (!output_label_) return;
    lv_label_set_text(output_label_, snapshot.c_str());
    if (output_) lv_obj_scroll_to_y(output_, LV_COORD_MAX, LV_ANIM_OFF);
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

std::string TerminalView::StripAnsi(const std::string &s) {
    // Drop CSI escape sequences (\033[...<letter>) and bare carriage returns so
    // the plain-text label stays readable (TERM=dumb already suppresses most,
    // but progress bars / a few tools still emit them).
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        char c = s[i];
        if (c == '\033' && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) ||
                                   s[i] == ';')) {
                i++;
            }
            if (i < s.size()) i++; // skip the final command letter
        } else if (c == '\r') {
            i++;
        } else {
            out.push_back(c);
            i++;
        }
    }
    return out;
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

void TerminalView::OnFlushTimer(lv_timer_t *t) {
    auto *self = static_cast<TerminalView *>(lv_timer_get_user_data(t));
    self->Flush();
}

} // namespace home