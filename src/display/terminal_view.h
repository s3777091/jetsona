#pragma once

#include "overlay_view.h"

#include <lvgl.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace home {

/* Full-screen terminal overlay for the DS-02 launcher.
 *
 * Spawns an interactive /bin/sh on a pseudo-terminal (forkpty) so the user can
 * run any command the firmware user is allowed to — including `sudo`, pipes,
 * redirects, and programs that read stdin (e.g. a sudo password prompt). The
 * firmware systemd unit runs as root, so this is a root shell: sudo is
 * available and every command runs with full privileges.
 *
 * Layout: a scrollable output area (top) + a one-line input with an on-screen
 * keyboard (bottom). Typed text is sent to the PTY on Enter; the shell echoes
 * it back through the PTY, so input appears in the output naturally.
 *
 * Threading: a reader thread blocks on read(master_fd) and marshals chunks to
 * the UI via Application::Schedule (then lv_lock). On close the master fd is
 * shut, which unblocks the reader and SIGHUPs the child; the destructor joins
 * the thread and reaps the child. */
class TerminalView : public OverlayView {
public:
    TerminalView(lv_obj_t *parent, int width, int height, ClosedCb on_closed);
    ~TerminalView() override;

protected:
    void OnStart() override;

private:
    int master_fd_ = -1;
    pid_t child_pid_ = -1;
    std::thread reader_;
    std::atomic<bool> stopping_{false};

    lv_obj_t *output_ = nullptr;     // scrollable output container
    lv_obj_t *output_label_ = nullptr;
    lv_obj_t *input_ = nullptr;      // lv_textarea (one line)
    lv_obj_t *send_btn_ = nullptr;
    lv_obj_t *keyboard_ = nullptr;

    std::string buffer_;             // accumulated output text
    std::mutex out_mtx_;             // guards buffer_ against concurrent appends

    bool SpawnShell();
    void ReaderLoop();
    void StopShell();

    void AppendOutput(const std::string &text); // thread-safe -> schedules UI update
    void DoSend();                               // reads input, writes to PTY

    static void OnSendClicked(lv_event_t *e);
    static void OnInputReady(lv_event_t *e);
    static void OnInputFocused(lv_event_t *e);
};

} // namespace home