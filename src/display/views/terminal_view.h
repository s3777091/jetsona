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
 * The editable command and the scrollback deliberately use different render
 * surfaces.  LVGL's textarea locates its caret by laying out all preceding
 * text; putting several KB of scrollback before the command therefore made
 * every key O(scrollback) and unusable on the Nano.  The command now lives in
 * a short one-line textarea, while committed output is rasterized only when it
 * changes into an opaque canvas.  FBDEV FULL-mode repaints can then blit that
 * cached canvas instead of re-laying-out and blending thousands of glyphs on
 * every key/caret blink.
 *
 * A child shell runs on a PTY with its own PS1 suppressed: the prompt is drawn
 * by the fixed input row and submitted commands are committed to scrollback
 * before being written to the PTY.  A reader thread appends cleaned output to
 * a capped buffer; an LVGL timer coalesces expensive canvas refreshes. */
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
    static constexpr size_t kBufCap = 4 * 1024;
    static constexpr size_t kHistoryCap = 100;
    static constexpr size_t kPasteCap = 4096;
    static constexpr int kInputHeight = 44;
    static constexpr int kCanvasPad = 10;

    int master_fd_ = -1;
    pid_t child_pid_ = -1;
    std::thread reader_;
    std::atomic<bool> stopping_{false};

    lv_obj_t *output_holder_ = nullptr;   // clips the canvas/fallback label
    lv_obj_t *output_canvas_ = nullptr;   // cached, already-rasterized output
    lv_draw_buf_t *output_draw_buf_ = nullptr;
    lv_obj_t *output_fallback_ = nullptr; // used only if canvas allocation fails
    lv_obj_t *input_row_ = nullptr;
    lv_obj_t *prompt_label_ = nullptr;
    lv_obj_t *input_ = nullptr;           // current command only
    lv_obj_t *input_label_ = nullptr;     // textarea label (selection bounds)
    lv_timer_t *flush_timer_ = nullptr;

    std::string buffer_;                  // committed, capped terminal text
    std::mutex out_mtx_;
    std::atomic<bool> dirty_{false};

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
    void RenderOutput(const std::string &committed);
    void ApplyAppearance();

    std::string CurrentInput() const;
    void ReplaceCurrentInput(const std::string &text);
    void SubmitInput();
    void RecallPrevious();
    void RecallNext();
    void CopySelection();
    void PasteClipboard();
    bool InputSelectionBounds(uint32_t &start, uint32_t &end) const;

    static size_t Utf8ByteIndex(const std::string &text, uint32_t char_index);
    static std::string StripAnsi(const std::string &s);
    static void OnInputKey(lv_event_t *e);
    static void OnFlushTimer(lv_timer_t *t);
};

} // namespace home
