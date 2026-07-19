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
#include <utility>

#define TAG "TerminalView"

namespace home {

using jetson::ui::Color;
using LvLockGuard = jetson::ui::LvglLockGuard;

namespace {
std::mutex g_terminal_views_mtx;
std::vector<TerminalView *> g_terminal_views;

lv_color_format_t CanvasColorFormat(lv_obj_t *obj) {
    lv_display_t *display = obj ? lv_obj_get_display(obj) : nullptr;
    const lv_color_format_t format = display
                                         ? lv_display_get_color_format(display)
                                         : LV_COLOR_FORMAT_RGB565;
    switch (format) {
        case LV_COLOR_FORMAT_RGB565:
        case LV_COLOR_FORMAT_RGB888:
        case LV_COLOR_FORMAT_XRGB8888:
        case LV_COLOR_FORMAT_ARGB8888:
            return format;
        default:
            return LV_COLOR_FORMAT_RGB565;
    }
}
} // namespace

TerminalView::TerminalView(lv_obj_t *parent, int width, int height, ClosedCb on_closed)
    : OverlayView(parent, width, height, "Terminal", std::move(on_closed)) {
    lv_obj_set_style_bg_color(body_, Color(0x0c0c0c), 0);
    lv_obj_set_style_bg_opa(body_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(body_, 0, 0);
    lv_obj_clear_flag(body_, LV_OBJ_FLAG_SCROLLABLE);

    const int output_height = std::max(1, height_ - kHeaderHeight - kInputHeight);

    /* The output is an opaque, pre-rasterized image.  FBDEV uses FULL render
     * mode, so it will still visit this object on every input invalidation, but
     * drawing it is a bounded image blit rather than thousands of TinyTTF
     * lookups/blends. */
    output_holder_ = lv_obj_create(body_);
    lv_obj_remove_style_all(output_holder_);
    lv_obj_set_pos(output_holder_, 0, 0);
    lv_obj_set_size(output_holder_, width_, output_height);
    lv_obj_clear_flag(output_holder_, LV_OBJ_FLAG_SCROLLABLE);

    output_canvas_ = lv_canvas_create(output_holder_);
    output_draw_buf_ = lv_draw_buf_create(
        static_cast<uint32_t>(std::max(1, width_)),
        static_cast<uint32_t>(output_height), CanvasColorFormat(body_), LV_STRIDE_AUTO);
    if (output_canvas_ && output_draw_buf_) {
        lv_canvas_set_draw_buf(output_canvas_, output_draw_buf_);
        lv_obj_set_pos(output_canvas_, 0, 0);
        lv_obj_clear_flag(output_canvas_,
                          static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE |
                                                     LV_OBJ_FLAG_SCROLLABLE));
    } else {
        ESP_LOGE(TAG, "terminal canvas allocation failed; using label fallback");
        if (output_canvas_) {
            lv_obj_del(output_canvas_);
            output_canvas_ = nullptr;
        }
        if (output_draw_buf_) {
            lv_draw_buf_destroy(output_draw_buf_);
            output_draw_buf_ = nullptr;
        }
        output_fallback_ = lv_label_create(output_holder_);
        lv_obj_set_pos(output_fallback_, kCanvasPad, kCanvasPad);
        lv_obj_set_width(output_fallback_, std::max(1, width_ - 2 * kCanvasPad));
        lv_label_set_long_mode(output_fallback_, LV_LABEL_LONG_WRAP);
        lv_label_set_text(output_fallback_, "");
    }

    /* The only editable LVGL object contains the current command, never the
     * scrollback.  Its layout cost is therefore bounded by one short line. */
    input_row_ = lv_obj_create(body_);
    lv_obj_remove_style_all(input_row_);
    lv_obj_set_pos(input_row_, 0, output_height);
    lv_obj_set_size(input_row_, width_, kInputHeight);
    lv_obj_set_style_pad_left(input_row_, kCanvasPad, 0);
    lv_obj_set_style_pad_right(input_row_, kCanvasPad, 0);
    lv_obj_set_style_pad_column(input_row_, 0, 0);
    lv_obj_set_flex_flow(input_row_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(input_row_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(input_row_, LV_OBJ_FLAG_SCROLLABLE);

    prompt_label_ = lv_label_create(input_row_);
    lv_label_set_text(prompt_label_, "root# ");

    input_ = lv_textarea_create(input_row_);
    lv_obj_set_width(input_, 10);
    lv_obj_set_height(input_, kInputHeight - 4);
    lv_obj_set_flex_grow(input_, 1);
    lv_obj_set_style_bg_opa(input_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(input_, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(input_, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(input_, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(input_, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_shadow_width(input_, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(input_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(input_, 0, LV_PART_MAIN);
    lv_textarea_set_one_line(input_, true);
    lv_textarea_set_text_selection(input_, true);
    lv_textarea_set_cursor_click_pos(input_, true);
    lv_textarea_set_text(input_, "");

    input_label_ = lv_obj_get_child(input_, 0);
    const auto key_filter = static_cast<lv_event_code_t>(LV_EVENT_KEY | LV_EVENT_PREPROCESS);
    lv_obj_add_event_cb(input_, OnInputKey, key_filter, this);
    if (auto *group = jetson::LvglRuntime::Instance().keypad_group()) {
        lv_group_add_obj(group, input_);
    }

    {
        std::lock_guard<std::mutex> lock(g_terminal_views_mtx);
        g_terminal_views.push_back(this);
    }
    ApplyAppearance();
    RenderOutput("");
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
    /* The canvas object must stop referencing its pixels before the draw buffer
     * is released.  The remaining children are deleted by OverlayView. */
    if (output_canvas_) {
        lv_obj_del(output_canvas_);
        output_canvas_ = nullptr;
    }
    if (output_draw_buf_) {
        lv_draw_buf_destroy(output_draw_buf_);
        output_draw_buf_ = nullptr;
    }
}

void TerminalView::RefreshOpenTerminals() {
    std::lock_guard<std::mutex> lock(g_terminal_views_mtx);
    for (auto *view : g_terminal_views) {
        if (view) view->ApplyAppearance();
    }
}

void TerminalView::ApplyAppearance() {
    if (!input_) return;

    Settings settings("terminal", false);
    text_size_ = std::clamp(
        settings.GetInt("text_size", jetson::kDefaultTerminalTextSize),
        jetson::kMinTerminalTextSize, jetson::kMaxTerminalTextSize);
    const auto &theme = jetson::FindTerminalTheme(
        settings.GetString("theme", jetson::kDefaultTerminalTheme));
    const lv_font_t *font = jetson::BuiltinTerminalFontAt(text_size_);
    const int line_space = std::max(1, text_size_ / 10);

    lv_obj_set_style_bg_color(body_, Color(theme.background), LV_PART_MAIN);
    lv_obj_set_style_bg_color(output_holder_, Color(theme.background), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(output_holder_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(input_row_, Color(theme.background), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(input_row_, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_set_style_text_font(prompt_label_, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(prompt_label_, Color(theme.foreground), LV_PART_MAIN);
    lv_obj_set_style_text_font(input_, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(input_, Color(theme.foreground), LV_PART_MAIN);
    lv_obj_set_style_text_line_space(input_, line_space, LV_PART_MAIN);
    lv_obj_set_style_bg_color(input_, Color(theme.cursor), LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(input_, LV_OPA_COVER, LV_PART_CURSOR);
    lv_obj_set_style_text_color(input_, Color(theme.background), LV_PART_CURSOR);
    lv_obj_set_style_bg_color(input_, Color(theme.selection), LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(input_, LV_OPA_COVER, LV_PART_SELECTED);
    lv_obj_set_style_text_color(input_, Color(theme.foreground), LV_PART_SELECTED);

    if (input_label_) {
        lv_obj_set_style_bg_color(input_label_, Color(theme.selection), LV_PART_SELECTED);
        lv_obj_set_style_bg_opa(input_label_, LV_OPA_COVER, LV_PART_SELECTED);
        lv_obj_set_style_text_color(input_label_, Color(theme.foreground),
                                    LV_PART_SELECTED);
    }
    if (output_fallback_) {
        lv_obj_set_style_text_font(output_fallback_, font, LV_PART_MAIN);
        lv_obj_set_style_text_color(output_fallback_, Color(theme.foreground),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_line_space(output_fallback_, line_space, LV_PART_MAIN);
    }

    if (master_fd_ >= 0) {
        struct winsize ws {};
        const int char_width = std::max(7, text_size_ * 3 / 5);
        const int row_height = std::max(16, text_size_ + 4);
        ws.ws_col = static_cast<unsigned short>(
            std::max(20, (width_ - 2 * kCanvasPad) / char_width));
        ws.ws_row = static_cast<unsigned short>(std::max(
            5, (height_ - kHeaderHeight - kInputHeight - 2 * kCanvasPad) /
                   row_height));
        (void)ioctl(master_fd_, TIOCSWINSZ, &ws);
    }

    dirty_.store(true);
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
        RenderOutput(error);
        return;
    }

    SetStatus("Shell san sang tai /root");
    if (input_) lv_group_focus_obj(input_);
    reader_ = std::thread([this]() { ReaderLoop(); });
    /* Canvas rasterization is the only expensive output operation.  Coalesce
     * arbitrary PTY bursts to at most 10 refreshes per second; typing never
     * invokes this path. */
    flush_timer_ = lv_timer_create(OnFlushTimer, 100, this);
}

bool TerminalView::SpawnShell() {
    struct winsize ws {};
    const int char_width = std::max(7, text_size_ * 3 / 5);
    const int row_height = std::max(16, text_size_ + 4);
    ws.ws_col = static_cast<unsigned short>(
        std::max(20, (width_ - 2 * kCanvasPad) / char_width));
    ws.ws_row = static_cast<unsigned short>(std::max(
        5, (height_ - kHeaderHeight - kInputHeight - 2 * kCanvasPad) / row_height));

    pid_t pid = forkpty(&master_fd_, nullptr, nullptr, &ws);
    if (pid < 0) {
        ESP_LOGE(TAG, "forkpty failed: %s", std::strerror(errno));
        master_fd_ = -1;
        return false;
    }
    if (pid == 0) {
        if (chdir("/root") != 0 && chdir("/") != 0) {
            /* Keep the service's inherited working directory. */
        }
        setenv("HOME", "/root", 1);
        setenv("USER", "root", 1);
        setenv("LOGNAME", "root", 1);
        /* The fixed input row owns the prompt.  Suppressing the shell's PS1
         * avoids a duplicate prompt in the cached output surface. */
        setenv("PS1", "", 1);
        setenv("TERM", "dumb", 1);
        setenv("LS_COLORS", "", 1);
        unsetenv("CLICOLOR");
        unsetenv("CLICOLOR_FORCE");

        /* Commands are drawn locally.  Disable the PTY's second echo so each
         * submitted line is committed exactly once by SubmitInput(). */
        struct termios tio {};
        if (tcgetattr(STDIN_FILENO, &tio) == 0) {
            tio.c_lflag &= static_cast<tcflag_t>(~(ECHO | ECHONL));
            (void)tcsetattr(STDIN_FILENO, TCSANOW, &tio);
        }

        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
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
                /* Best effort; the SIGHUP/SIGKILL fallback below still runs. */
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
    /* A background terminal keeps collecting output but does not spend CPU
     * rasterizing frames nobody can see.  The dirty bit causes one catch-up
     * render immediately after it is restored. */
    if (overlay_ && lv_obj_has_flag(overlay_, LV_OBJ_FLAG_HIDDEN)) {
        dirty_.store(true);
        return;
    }
    RenderOutput(committed);
}

void TerminalView::RenderOutput(const std::string &committed) {
    const lv_font_t *font = input_
                                ? lv_obj_get_style_text_font(input_, LV_PART_MAIN)
                                : jetson::BuiltinTerminalFontAt(text_size_);
    const lv_color_t foreground = input_
                                      ? lv_obj_get_style_text_color(input_, LV_PART_MAIN)
                                      : Color(0xe7e7e7);
    const lv_color_t background = body_
                                      ? lv_obj_get_style_bg_color(body_, LV_PART_MAIN)
                                      : Color(0x0c0c0c);
    const int line_space = std::max(1, text_size_ / 10);

    if (output_canvas_ && output_draw_buf_) {
        lv_canvas_fill_bg(output_canvas_, background, LV_OPA_COVER);
        if (!committed.empty()) {
            const int canvas_width = std::max(1, width_);
            const int canvas_height = std::max(
                1, height_ - kHeaderHeight - kInputHeight);
            const int text_width = std::max(1, canvas_width - 2 * kCanvasPad);

            lv_point_t text_size {};
            lv_text_get_size(&text_size, committed.c_str(), font, 0, line_space,
                             text_width, LV_TEXT_FLAG_NONE);
            const int text_height = std::max<int>(lv_font_get_line_height(font),
                                                   text_size.y);
            /* Negative y clips old scrollback before it reaches the draw unit;
             * the newest line always sits directly above the input row. */
            const int y = std::min(kCanvasPad,
                                   canvas_height - kCanvasPad - text_height);

            lv_layer_t layer;
            lv_canvas_init_layer(output_canvas_, &layer);
            lv_draw_label_dsc_t descriptor;
            lv_draw_label_dsc_init(&descriptor);
            descriptor.text = committed.c_str();
            descriptor.font = font;
            descriptor.color = foreground;
            descriptor.opa = LV_OPA_COVER;
            descriptor.line_space = line_space;
            descriptor.letter_space = 0;
            descriptor.flag = LV_TEXT_FLAG_NONE;
            lv_area_t area = {
                kCanvasPad,
                y,
                canvas_width - kCanvasPad - 1,
                y + text_height - 1,
            };
            lv_draw_label(&layer, &descriptor, &area);
            lv_canvas_finish_layer(output_canvas_, &layer);
        }
        lv_obj_invalidate(output_canvas_);
        return;
    }

    if (output_fallback_) {
        lv_label_set_text(output_fallback_, committed.c_str());
        lv_obj_update_layout(output_fallback_);
        const int holder_height = lv_obj_get_height(output_holder_);
        const int label_height = lv_obj_get_height(output_fallback_);
        lv_obj_set_y(output_fallback_,
                     std::min(kCanvasPad,
                              holder_height - kCanvasPad - label_height));
    }
}

std::string TerminalView::CurrentInput() const {
    if (!input_) return {};
    const char *raw = lv_textarea_get_text(input_);
    return raw ? raw : "";
}

void TerminalView::ReplaceCurrentInput(const std::string &text) {
    if (!input_) return;
    lv_textarea_set_text(input_, text.c_str());
    lv_textarea_set_cursor_pos(input_, LV_TEXTAREA_CURSOR_LAST);
}

void TerminalView::SubmitInput() {
    if (master_fd_ < 0 || !input_) return;
    const std::string input = CurrentInput();

    if (input.find_first_not_of(" \t") != std::string::npos) {
        if (history_.empty() || history_.back() != input) history_.push_back(input);
        if (history_.size() > kHistoryCap) history_.erase(history_.begin());
    }
    history_index_ = history_.size();
    history_draft_.clear();

    /* Clearing the short input is immediate.  Do not synchronously repaint the
     * output here: the coalescing timer commits the cached canvas within 100 ms,
     * so Enter can never block behind a full scrollback raster. */
    lv_textarea_set_text(input_, "");
    {
        std::lock_guard<std::mutex> lock(out_mtx_);
        buffer_ += "root# ";
        buffer_ += input;
        buffer_ += '\n';
        CapBufferLocked();
    }
    dirty_.store(true);

    std::string line = input;
    line += '\n';
    size_t offset = 0;
    while (offset < line.size()) {
        const ssize_t written = write(master_fd_, line.data() + offset,
                                      line.size() - offset);
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

bool TerminalView::InputSelectionBounds(uint32_t &start, uint32_t &end) const {
    if (!input_label_) return false;
    start = lv_label_get_text_selection_start(input_label_);
    end = lv_label_get_text_selection_end(input_label_);
    if (start == LV_LABEL_TEXT_SELECTION_OFF ||
        end == LV_LABEL_TEXT_SELECTION_OFF) {
        return false;
    }
    if (start > end) std::swap(start, end);
    return start < end;
}

void TerminalView::CopySelection() {
    if (!input_) return;
    const std::string input = CurrentInput();
    std::string copied;

    uint32_t start = 0;
    uint32_t end = 0;
    if (InputSelectionBounds(start, end)) {
        const size_t begin_byte = Utf8ByteIndex(input, start);
        const size_t end_byte = Utf8ByteIndex(input, end);
        copied = input.substr(begin_byte, end_byte - begin_byte);
    } else if (!input.empty()) {
        copied = input;
    } else {
        /* The canvas has no editable label to select.  Ctrl+C on an empty
         * command copies the latest output, preserving a useful framebuffer-
         * only clipboard path without putting scrollback back in a textarea. */
        std::lock_guard<std::mutex> lock(out_mtx_);
        const size_t begin = buffer_.size() > kPasteCap
                                 ? buffer_.size() - kPasteCap
                                 : 0;
        copied = buffer_.substr(begin);
    }

    if (!copied.empty()) {
        if (copied.size() > kPasteCap) copied.resize(kPasteCap);
        clipboard_ = std::move(copied);
    }
    lv_textarea_clear_selection(input_);
}

void TerminalView::PasteClipboard() {
    if (!input_ || clipboard_.empty()) return;

    /* Multi-line output becomes one command and is never executed until the
     * user explicitly presses Enter. */
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
    if (!pasted.empty()) lv_textarea_add_text(input_, pasted.c_str());
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

void TerminalView::OnInputKey(lv_event_t *e) {
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
        default:
            return;
    }
}

void TerminalView::OnFlushTimer(lv_timer_t *t) {
    auto *self = static_cast<TerminalView *>(lv_timer_get_user_data(t));
    self->Flush();
}

} // namespace home
