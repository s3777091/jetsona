#pragma once

#include "display/views/overlay_view.h"

#include <lvgl.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <thread>
#include <vector>

namespace home {

/* Full-screen, line-oriented root terminal for the DS-02 launcher.
 *
 * Output, prompt and the editable command share one multi-line textarea. This
 * makes the caret sit immediately after `root# command`, like a normal terminal,
 * instead of putting input in a separate footer. The prefix before
 * editable_char_ is read-only; only the command after the latest prompt can be
 * edited. Up/Down walk command history, Enter submits, and Ctrl+C/Ctrl+V use an
 * in-app clipboard (raw framebuffer sessions do not have a desktop clipboard).
 * Mouse-drag selection is enabled, so output as well as the current command can
 * be copied.
 *
 * A child shell runs on a PTY. PTY echo is disabled because the textarea draws
 * the command locally; on Enter the line is committed to the scrollback before
 * it is written to the shell. A reader thread appends command output to a capped
 * buffer, while an LVGL timer coalesces rendering on the UI thread. */
class TerminalView : public OverlayView {
public:
    TerminalView(lv_obj_t *parent, int width, int height, ClosedCb on_closed);
    ~TerminalView() override;

    /* Repaint every currently open terminal after Settings changes its theme
     * or text size. Call on the LVGL thread (or while holding the LVGL lock). */
    static void RefreshOpenTerminals();

protected:
    void OnStart() override;

private:
    /* The whole scrollback lives in ONE textarea label. LVGL re-runs the
     * label's line-wrap layout (per-glyph TTF metric lookups) on every frame
     * the screen repaints, and the fbdev backend renders in FULL mode, so any
     * invalidation (mouse move, caret blink, output) repaints everything.
     * 12 KB of text made that per-frame layout dominate and the terminal
     * unusably laggy; 4 KB (~50-60 lines of scrollback) keeps typing and
     * `ls`-style bursts fluid on the Jetson while still allowing scrollback. */
    static constexpr size_t kBufCap = 4 * 1024;
    static constexpr size_t kHistoryCap = 100;
    static constexpr size_t kPasteCap = 4096;

    int master_fd_ = -1;
    pid_t child_pid_ = -1;
    std::thread reader_;
    std::atomic<bool> stopping_{false};

    lv_obj_t *terminal_ = nullptr;       // output + prompt + current command
    lv_obj_t *terminal_label_ = nullptr; // textarea's internal label (selection)
    lv_timer_t *flush_timer_ = nullptr;

    std::string buffer_;                 // committed, capped terminal text
    std::mutex out_mtx_;
    std::atomic<bool> dirty_{false};

    size_t editable_byte_ = 0;           // beginning of current input in UTF-8 bytes
    uint32_t editable_char_ = 0;         // same boundary in Unicode characters
    std::vector<std::string> history_;
    size_t history_index_ = 0;
    std::string history_draft_;
    std::string clipboard_;
    int text_size_ = 14;

    bool SpawnShell();
    void ReaderLoop();
    void StopShell();

    void AppendOutput(const std::string &text);
    void CapBufferLocked();
    void Flush();
    void Render(const std::string &committed, const std::string &input,
                uint32_t input_cursor);

    std::string CurrentInput() const;
    void ReplaceCurrentInput(const std::string &text);
    void SubmitInput();
    void RecallPrevious();
    void RecallNext();
    void CopySelection();
    void PasteClipboard();
    void PrepareForEditing();
    void ApplyAppearance();
    bool SelectionBounds(uint32_t &start, uint32_t &end) const;

    static size_t Utf8ByteIndex(const std::string &text, uint32_t char_index);
    static uint32_t Utf8Length(const std::string &text);
    static std::string StripAnsi(const std::string &s);
    static void OnTerminalKey(lv_event_t *e);
    static void OnFlushTimer(lv_timer_t *t);
};

} // namespace home
