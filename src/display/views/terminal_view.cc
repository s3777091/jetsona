#include "display/views/terminal_view.h"

#include "display/common/lvgl_utils.h"
#include "display/theme/terminal_theme.h"
#include "esp_log.h"
#include "fonts.h"
#include "lvgl_runtime.h"
#include "settings.h"

#include <lvgl.h>

#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>

#define TAG "TerminalView"

namespace home {

using jetson::ui::Color;
using LvLockGuard = jetson::ui::LvglLockGuard;

namespace {
std::mutex g_terminal_views_mtx;
std::vector<TerminalView *> g_terminal_views;
} // namespace

TerminalView::TerminalView(lv_obj_t *parent, int width, int height, ClosedCb on_closed)
    : OverlayView(parent, width, height, "Terminal", std::move(on_closed)) {
    /* One uninterrupted black canvas: no output card, footer input or Send
     * button. The textarea itself owns scrollback, prompt, command and caret. */
    lv_obj_set_style_bg_color(body_, Color(0x0c0c0c), 0);
    lv_obj_set_style_bg_opa(body_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(body_, 0, 0);
    lv_obj_clear_flag(body_, LV_OBJ_FLAG_SCROLLABLE);

    terminal_ = lv_textarea_create(body_);
    lv_obj_set_size(terminal_, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(terminal_, 0, 0);
    lv_obj_set_style_text_font(terminal_, jetson::BuiltinTerminalFontAt(text_size_), 0);
    lv_obj_set_style_text_color(terminal_, Color(0xe7e7e7), 0);
    lv_obj_set_style_text_line_space(terminal_, 2, 0);
    lv_obj_set_style_bg_color(terminal_, Color(0x0c0c0c), 0);
    lv_obj_set_style_bg_opa(terminal_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(terminal_, 0, 0);
    lv_obj_set_style_outline_width(terminal_, 0, 0);
    lv_obj_set_style_outline_width(terminal_, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(terminal_, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_shadow_width(terminal_, 0, 0);
    lv_obj_set_style_radius(terminal_, 0, 0);
    lv_obj_set_style_pad_all(terminal_, 10, 0);
    lv_obj_set_scroll_dir(terminal_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(terminal_, LV_SCROLLBAR_MODE_AUTO);

    lv_textarea_set_one_line(terminal_, false);
    lv_textarea_set_text_selection(terminal_, true);
    lv_textarea_set_cursor_click_pos(terminal_, true);
    lv_textarea_set_text(terminal_, "");

    /* A bright block caret remains visible on the almost-black canvas. */
    lv_obj_set_style_bg_color(terminal_, lv_color_white(), LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(terminal_, LV_OPA_COVER, LV_PART_CURSOR);
    lv_obj_set_style_text_color(terminal_, Color(0x0c0c0c), LV_PART_CURSOR);
    lv_obj_set_style_bg_color(terminal_, Color(0x343331), LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(terminal_, LV_OPA_COVER, LV_PART_SELECTED);
    lv_obj_set_style_text_color(terminal_, lv_color_white(), LV_PART_SELECTED);

    /* The textarea's internal label exposes the mouse selection bounds. */
    terminal_label_ = lv_obj_get_child(terminal_, 0);
    if (terminal_label_) {
        lv_obj_set_style_bg_color(terminal_label_, Color(0x343331), LV_PART_SELECTED);
        lv_obj_set_style_bg_opa(terminal_label_, LV_OPA_COVER, LV_PART_SELECTED);
        lv_obj_set_style_text_color(terminal_label_, lv_color_white(), LV_PART_SELECTED);
    }

    const auto key_filter = static_cast<lv_event_code_t>(LV_EVENT_KEY | LV_EVENT_PREPROCESS);
    lv_obj_add_event_cb(terminal_, OnTerminalKey, key_filter, this);
    if (auto *g = jetson::LvglRuntime::Instance().keypad_group()) {
        lv_group_add_obj(g, terminal_);
    }

    {
        std::lock_guard<std::mutex> lock(g_terminal_views_mtx);
        g_terminal_views.push_back(this);
    }
    ApplyAppearance();
}

TerminalView::~TerminalView() {
    {
        std::lock_guard<std::mutex> lock(g_terminal_views_mtx);
        g_terminal_views.erase(
            std::remove(g_terminal_views.begin(), g_terminal_views.end(), this),
            g_terminal_views.end());
    }
    StopShell();
    LvLockGuard lock;
    if (flush_timer_) {
        lv_timer_del(flush_timer_);
        flush_timer_ = nullptr;
    }
}

void TerminalView::RefreshOpenTerminals() {
    std::lock_guard<std::mutex> lock(g_terminal_views_mtx);
    for (auto *view : g_terminal_views) {
        if (view) view->ApplyAppearance();
    }
}

void TerminalView::ApplyAppearance() {
    if (!terminal_) return;

    Settings settings("terminal", false);
    text_size_ = std::clamp(
        settings.GetInt("text_size", jetson::kDefaultTerminalTextSize),
        jetson::kMinTerminalTextSize, jetson::kMaxTerminalTextSize);
    const auto &theme = jetson::FindTerminalTheme(
        settings.GetString("theme", jetson::kDefaultTerminalTheme));

    lv_obj_set_style_bg_color(body_, Color(theme.background), 0);
    lv_obj_set_style_bg_color(terminal_, Color(theme.background), LV_PART_MAIN);
    lv_obj_set_style_text_color(terminal_, Color(theme.foreground), LV_PART_MAIN);
    lv_obj_set_style_text_font(terminal_, jetson::BuiltinTerminalFontAt(text_size_),
                               LV_PART_MAIN);
    lv_obj_set_style_text_line_space(terminal_, std::max(1, text_size_ / 10),
                                     LV_PART_MAIN);
    lv_obj_set_style_bg_color(terminal_, Color(theme.cursor), LV_PART_CURSOR);
    lv_obj_set_style_text_color(terminal_, Color(theme.background), LV_PART_CURSOR);
    lv_obj_set_style_bg_color(terminal_, Color(theme.selection), LV_PART_SELECTED);
    lv_obj_set_style_text_color(terminal_, Color(theme.foreground), LV_PART_SELECTED);
    lv_obj_set_style_bg_color(terminal_, Color(theme.accent), LV_PART_SCROLLBAR);

    if (terminal_label_) {
        lv_obj_set_style_bg_color(terminal_label_, Color(theme.selection), LV_PART_SELECTED);
        lv_obj_set_style_text_color(terminal_label_, Color(theme.foreground),
                                    LV_PART_SELECTED);
    }

    if (master_fd_ >= 0) {
        struct winsize ws {};
        const int char_width = std::max(7, text_size_ * 3 / 5);
        const int row_height = std::max(16, text_size_ + 4);
        ws.ws_col = static_cast<unsigned short>(std::max(20, (width_ - 20) / char_width));
        ws.ws_row = static_cast<unsigned short>(
            std::max(5, (height_ - kHeaderHeight - 20) / row_height));
        (void)ioctl(master_fd_, TIOCSWINSZ, &ws);
    }
    lv_obj_invalidate(body_);
}

void TerminalView::OnStart() {
    if (!SpawnShell()) {
        SetStatus("Khong mo duoc shell (forkpty loi)");
        const std::string error =
            "[Loi: khong the mo terminal. Kiem tra /bin/sh va quyen.]\n";
        {
            std::lock_guard<std::mutex> lock(out_mtx_);
            buffer_ = error;
        }
        Render(error, "", 0);
        return;
    }

    SetStatus("Shell san sang tai /root");
    if (terminal_) lv_group_focus_obj(terminal_);
    reader_ = std::thread([this]() { ReaderLoop(); });
    /* 100 ms coalescing: every flush replaces the textarea text and forces a
     * full-screen repaint in the FULL render mode, so flushing at 20 Hz made
     * bursty command output (find/ls -laR) starve the UI thread. 10 Hz is
     * still instant to the eye at a fraction of the render cost. */
    flush_timer_ = lv_timer_create(OnFlushTimer, 100, this);
}

bool TerminalView::SpawnShell() {
    struct winsize ws {};
    const int char_width = std::max(7, text_size_ * 3 / 5);
    const int row_height = std::max(16, text_size_ + 4);
    ws.ws_col = static_cast<unsigned short>(std::max(20, (width_ - 20) / char_width));
    ws.ws_row = static_cast<unsigned short>(
        std::max(5, (height_ - kHeaderHeight - 20) / row_height));

    pid_t pid = forkpty(&master_fd_, nullptr, nullptr, &ws);
    if (pid < 0) {
        ESP_LOGE(TAG, "forkpty failed: %s", std::strerror(errno));
        master_fd_ = -1;
        return false;
    }
    if (pid == 0) {
        /* The firmware service runs as root. Start in root's home and expose the
         * requested compact prompt regardless of the repository/service cwd.
         * glibc marks chdir warn_unused_result; a (void) cast does not silence
         * it, so test both calls and accept the inherited cwd as last resort. */
        if (chdir("/root") != 0 && chdir("/") != 0) {
            /* keep the service's working directory */
        }
        setenv("HOME", "/root", 1);
        setenv("USER", "root", 1);
        setenv("LOGNAME", "root", 1);
        setenv("PS1", "root# ", 1);
        setenv("TERM", "dumb", 1);
        setenv("LS_COLORS", "", 1);
        unsetenv("CLICOLOR");
        unsetenv("CLICOLOR_FORCE");

        /* Commands are drawn immediately by the textarea. Suppress the PTY's
         * second echo so each submitted line appears exactly once. */
        struct termios tio {};
        if (tcgetattr(STDIN_FILENO, &tio) == 0) {
            tio.c_lflag &= static_cast<tcflag_t>(~(ECHO | ECHONL));
            (void)tcsetattr(STDIN_FILENO, TCSANOW, &tio);
        }

        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);

        /* /bin/sh stays in canonical, line-oriented mode here. That is
         * intentional: readline-style shells repaint typed characters on the
         * PTY and would duplicate the command already drawn by our textarea. */
        execl("/bin/sh", "sh", "-i", static_cast<char *>(nullptr));
        _exit(127);
    }

    child_pid_ = pid;
    return true;
}

void TerminalView::ReaderLoop() {
    char buf[4096];
    while (!stopping_) {
        const ssize_t n = read(master_fd_, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        AppendOutput(StripAnsi(std::string(buf, static_cast<size_t>(n))));
    }
    if (!stopping_) AppendOutput("\n[shell da thoat]\n");
}

void TerminalView::StopShell() {
    if (stopping_.exchange(true)) {
        if (reader_.joinable()) reader_.join();
        return;
    }

    if (master_fd_ >= 0) {
        if (child_pid_ > 0) {
            const char exit_cmd[] = "exit\n";
            if (write(master_fd_, exit_cmd, sizeof(exit_cmd) - 1) < 0) {
                /* best effort; the SIGHUP/SIGKILL fallback below still runs */
            }
            for (int i = 0; i < 20 && child_pid_ > 0; ++i) {
                int status = 0;
                const pid_t waited = waitpid(child_pid_, &status, WNOHANG);
                if (waited == child_pid_) {
                    child_pid_ = -1;
                    break;
                }
                usleep(50 * 1000);
            }
            if (child_pid_ > 0) {
                kill(child_pid_, SIGHUP);
                kill(child_pid_, SIGKILL);
                int status = 0;
                (void)waitpid(child_pid_, &status, 0);
                child_pid_ = -1;
            }
        }
        close(master_fd_);
        master_fd_ = -1;
    }
    if (reader_.joinable()) reader_.join();
}

void TerminalView::AppendOutput(const std::string &text) {
    if (text.empty()) return;
    {
        std::lock_guard<std::mutex> lock(out_mtx_);
        buffer_ += text;
        CapBufferLocked();
    }
    dirty_.store(true);
}

void TerminalView::CapBufferLocked() {
    if (buffer_.size() <= kBufCap) return;

    size_t erase_count = buffer_.size() - kBufCap;
    /* Prefer dropping a complete oldest line so the top of the visible
     * scrollback never begins in the middle of a command/output line. */
    const size_t next_line = buffer_.find('\n', erase_count);
    if (next_line != std::string::npos && next_line + 1 < buffer_.size()) {
        erase_count = next_line + 1;
    }
    buffer_.erase(0, erase_count);
}

void TerminalView::Flush() {
    if (!dirty_.exchange(false)) return;

    std::string committed;
    {
        std::lock_guard<std::mutex> lock(out_mtx_);
        committed = buffer_;
    }

    LvLockGuard lock;
    if (!terminal_) return;
    const std::string input = CurrentInput();
    const uint32_t cursor = lv_textarea_get_cursor_pos(terminal_);
    const uint32_t input_cursor = cursor > editable_char_
                                      ? std::min(cursor - editable_char_, Utf8Length(input))
                                      : Utf8Length(input);
    Render(committed, input, input_cursor);
}

void TerminalView::Render(const std::string &committed, const std::string &input,
                          uint32_t input_cursor) {
    if (!terminal_) return;
    std::string text = committed;
    text += input;
    lv_textarea_set_text(terminal_, text.c_str());
    editable_byte_ = committed.size();
    editable_char_ = Utf8Length(committed);
    input_cursor = std::min(input_cursor, Utf8Length(input));
    lv_textarea_set_cursor_pos(terminal_, static_cast<int32_t>(editable_char_ + input_cursor));
    lv_obj_scroll_to_y(terminal_, LV_COORD_MAX, LV_ANIM_OFF);
}

std::string TerminalView::CurrentInput() const {
    if (!terminal_) return {};
    const char *raw = lv_textarea_get_text(terminal_);
    if (!raw) return {};
    const size_t length = std::strlen(raw);
    if (editable_byte_ > length) return {};
    return std::string(raw + editable_byte_);
}

void TerminalView::ReplaceCurrentInput(const std::string &text) {
    if (!terminal_) return;
    const char *raw = lv_textarea_get_text(terminal_);
    const size_t length = raw ? std::strlen(raw) : 0;
    const size_t prefix_length = std::min(editable_byte_, length);
    const std::string committed(raw ? std::string(raw, prefix_length) : std::string());
    Render(committed, text, Utf8Length(text));
}

void TerminalView::SubmitInput() {
    if (master_fd_ < 0) return;
    const std::string input = CurrentInput();

    if (input.find_first_not_of(" \t") != std::string::npos) {
        if (history_.empty() || history_.back() != input) history_.push_back(input);
        if (history_.size() > kHistoryCap) history_.erase(history_.begin());
    }
    history_index_ = history_.size();
    history_draft_.clear();

    std::string committed;
    {
        std::lock_guard<std::mutex> lock(out_mtx_);
        buffer_ += input;
        buffer_ += '\n';
        CapBufferLocked();
        committed = buffer_;
        /* This exact snapshot is rendered synchronously below. Any reader append
         * that acquires the mutex after us will set dirty back to true. */
        dirty_.store(false);
    }
    Render(committed, "", 0);

    std::string line = input;
    line += '\n';
    size_t offset = 0;
    while (offset < line.size()) {
        const ssize_t written = write(master_fd_, line.data() + offset, line.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) break;
        offset += static_cast<size_t>(written);
    }
}

void TerminalView::RecallPrevious() {
    if (history_.empty()) return;
    if (history_index_ >= history_.size()) {
        history_draft_ = CurrentInput();
        history_index_ = history_.size();
    }
    if (history_index_ > 0) --history_index_;
    ReplaceCurrentInput(history_[history_index_]);
}

void TerminalView::RecallNext() {
    if (history_.empty() || history_index_ >= history_.size()) return;
    ++history_index_;
    ReplaceCurrentInput(history_index_ < history_.size()
                            ? history_[history_index_]
                            : history_draft_);
}

bool TerminalView::SelectionBounds(uint32_t &start, uint32_t &end) const {
    if (!terminal_label_) return false;
    start = lv_label_get_text_selection_start(terminal_label_);
    end = lv_label_get_text_selection_end(terminal_label_);
    if (start == LV_LABEL_TEXT_SELECTION_OFF || end == LV_LABEL_TEXT_SELECTION_OFF) return false;
    if (start > end) std::swap(start, end);
    return start < end;
}

void TerminalView::CopySelection() {
    if (!terminal_) return;
    const char *raw = lv_textarea_get_text(terminal_);
    const std::string all = raw ? raw : "";
    std::string copied;

    uint32_t start = 0;
    uint32_t end = 0;
    if (SelectionBounds(start, end)) {
        const size_t begin_byte = Utf8ByteIndex(all, start);
        const size_t end_byte = Utf8ByteIndex(all, end);
        copied = all.substr(begin_byte, end_byte - begin_byte);
    } else {
        copied = CurrentInput();
    }

    if (!copied.empty()) {
        if (copied.size() > kPasteCap) copied.resize(kPasteCap);
        clipboard_ = std::move(copied);
    }
    lv_textarea_clear_selection(terminal_);
}

void TerminalView::PasteClipboard() {
    if (!terminal_ || clipboard_.empty()) return;
    PrepareForEditing();

    /* Keep paste line-oriented: copied multi-line output becomes one command
     * line and is never executed until the user explicitly presses Enter. */
    std::string pasted;
    pasted.reserve(std::min(clipboard_.size(), kPasteCap));
    for (unsigned char c : clipboard_) {
        if (pasted.size() >= kPasteCap) break;
        if (c == '\r' || c == '\n' || c == '\t') {
            if (!pasted.empty() && pasted.back() != ' ') pasted.push_back(' ');
        } else if (c >= 0x20) {
            pasted.push_back(static_cast<char>(c));
        }
    }
    if (!pasted.empty()) lv_textarea_add_text(terminal_, pasted.c_str());
}

void TerminalView::PrepareForEditing() {
    if (!terminal_) return;
    uint32_t start = 0;
    uint32_t end = 0;
    if (SelectionBounds(start, end) && start < editable_char_) {
        lv_textarea_clear_selection(terminal_);
        lv_textarea_set_cursor_pos(terminal_, LV_TEXTAREA_CURSOR_LAST);
        return;
    }
    if (lv_textarea_get_cursor_pos(terminal_) < editable_char_) {
        lv_textarea_set_cursor_pos(terminal_, LV_TEXTAREA_CURSOR_LAST);
    }
}

size_t TerminalView::Utf8ByteIndex(const std::string &text, uint32_t char_index) {
    size_t byte = 0;
    uint32_t chars = 0;
    while (byte < text.size() && chars < char_index) {
        const unsigned char c = static_cast<unsigned char>(text[byte]);
        size_t width = 1;
        if ((c & 0xe0) == 0xc0) width = 2;
        else if ((c & 0xf0) == 0xe0) width = 3;
        else if ((c & 0xf8) == 0xf0) width = 4;
        byte += std::min(width, text.size() - byte);
        ++chars;
    }
    return byte;
}

uint32_t TerminalView::Utf8Length(const std::string &text) {
    uint32_t length = 0;
    for (unsigned char c : text) {
        if ((c & 0xc0) != 0x80) ++length;
    }
    return length;
}

std::string TerminalView::StripAnsi(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 0x1b && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size()) {
                const unsigned char part = static_cast<unsigned char>(s[i++]);
                if (part >= 0x40 && part <= 0x7e) break;
            }
        } else if (c == 0x1b && i + 1 < s.size() && s[i + 1] == ']') {
            i += 2;
            while (i < s.size()) {
                if (s[i] == '\a') {
                    ++i;
                    break;
                }
                if (s[i] == 0x1b && i + 1 < s.size() && s[i + 1] == '\\') {
                    i += 2;
                    break;
                }
                ++i;
            }
        } else if (c == '\r' || c == 0) {
            ++i;
        } else if (c == '\b') {
            if (!out.empty() && out.back() != '\n') out.pop_back();
            ++i;
        } else if (c < 0x20 && c != '\n' && c != '\t') {
            ++i;
        } else {
            out.push_back(static_cast<char>(c));
            ++i;
        }
    }
    return out;
}

void TerminalView::OnTerminalKey(lv_event_t *e) {
    auto *self = static_cast<TerminalView *>(lv_event_get_user_data(e));
    const uint32_t key = lv_event_get_key(e);
    const bool ctrl = jetson::LvglRuntime::Instance().KeyboardCtrlPressed();

    if (ctrl && (key == 'c' || key == 'C')) {
        self->CopySelection();
        lv_event_stop_processing(e);
        return;
    }
    if (ctrl && (key == 'v' || key == 'V')) {
        self->PasteClipboard();
        lv_event_stop_processing(e);
        return;
    }

    switch (key) {
        case LV_KEY_ENTER:
            self->SubmitInput();
            lv_event_stop_processing(e);
            return;
        case LV_KEY_UP:
            self->RecallPrevious();
            lv_event_stop_processing(e);
            return;
        case LV_KEY_DOWN:
            self->RecallNext();
            lv_event_stop_processing(e);
            return;
        case LV_KEY_HOME:
            lv_textarea_clear_selection(self->terminal_);
            lv_textarea_set_cursor_pos(self->terminal_, self->editable_char_);
            lv_event_stop_processing(e);
            return;
        case LV_KEY_END:
            lv_textarea_clear_selection(self->terminal_);
            lv_textarea_set_cursor_pos(self->terminal_, LV_TEXTAREA_CURSOR_LAST);
            lv_event_stop_processing(e);
            return;
        case LV_KEY_LEFT:
        case LV_KEY_BACKSPACE:
            self->PrepareForEditing();
            if (lv_textarea_get_cursor_pos(self->terminal_) <= self->editable_char_) {
                lv_event_stop_processing(e);
            }
            return;
        case LV_KEY_RIGHT:
        case LV_KEY_DEL:
            self->PrepareForEditing();
            return;
        default:
            self->PrepareForEditing();
            return;
    }
}

void TerminalView::OnFlushTimer(lv_timer_t *t) {
    auto *self = static_cast<TerminalView *>(lv_timer_get_user_data(t));
    self->Flush();
}

} // namespace home
