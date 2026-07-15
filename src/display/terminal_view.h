#pragma once

#include "overlay_view.h"

#include <lvgl.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace home {

/* Full-screen terminal overlay for the DS-02 launcher, styled after the
 * macOS Terminal window (dark canvas, traffic-light controls from OverlayView,
 * prompt + one-line input at the bottom).
 *
 * Spawns an interactive /bin/sh on a pseudo-terminal (forkpty) so the user can
 * run any command the firmware user is allowed to -- including sudo, pipes,
 * redirects and stdin-reading prompts. The systemd unit runs as root, so this
 * is a root shell.
 *
 * Input comes from the USB keyboard (the textarea is wired into the keypad
 * group); there is no on-screen lv_keyboard -- it was redundant with the USB
 * keyboard and added render cost. Typed text is sent to the PTY on Enter / the
 * "Gui" button; the shell echoes it back through the PTY so input shows in the
 * output naturally.
 *
 * Performance: a reader thread blocks on read(master_fd) and appends cleaned
 * (ANSI/CR-stripped) chunks to a capped ring buffer under a mutex, then marks
 * it dirty. A periodic LVGL timer (OnFlushTimer, ~12 Hz) is the ONLY thing that
 * calls lv_label_set_text -- so no matter how fast the shell emits, the UI
 * re-wraps the (small, 4 KB) buffer at most ~12 times a second instead of once
 * per read chunk. This is what fixes the previous O(N^2) lag on `dmesg`/`ls`.
 *
 * Lifetime: shared_ptr-owned (OverlayView); on close the master fd is shut
 * (unblocking the reader, SIGHUP/SIGKILL-ing the child), the destructor joins
 * the thread and deletes the flush timer. */
class TerminalView : public OverlayView {
public:
    TerminalView(lv_obj_t *parent, int width, int height, ClosedCb on_closed);
    ~TerminalView() override;

protected:
    void OnStart() override;

private:
    static constexpr size_t kBufCap = 4096;   // visible scrollback, in bytes

    int master_fd_ = -1;
    pid_t child_pid_ = -1;
    std::thread reader_;
    std::atomic<bool> stopping_{false};

    lv_obj_t *output_ = nullptr;        // scrollable output canvas
    lv_obj_t *output_label_ = nullptr;
    lv_obj_t *prompt_label_ = nullptr;  // ">" prefix before the input
    lv_obj_t *input_ = nullptr;         // lv_textarea (one line)
    lv_obj_t *send_btn_ = nullptr;

    lv_timer_t *flush_timer_ = nullptr; // coalesces reader output -> UI

    std::string buffer_;                // capped, ANSI-stripped output text
    std::mutex out_mtx_;                // guards buffer_ against the reader
    std::atomic<bool> dirty_{false};    // set by reader, cleared by flush timer

    bool SpawnShell();
    void ReaderLoop();
    void StopShell();

    void AppendOutput(const std::string &text); // thread-safe: append + mark dirty
    void Flush();                               // timer: copies buffer_ -> label
    void DoSend();                              // reads input, writes to PTY

    static std::string StripAnsi(const std::string &s);
    static void OnSendClicked(lv_event_t *e);
    static void OnInputReady(lv_event_t *e);
    static void OnFlushTimer(lv_timer_t *t);
};

} // namespace home