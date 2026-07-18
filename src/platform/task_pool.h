#pragma once

/* Shared fixed-size worker pool for finite background jobs.
 *
 * The firmware used to spawn a detached std::thread per fetch/scan/toggle,
 * which is unbounded: a busy moment could pile a dozen runnable threads onto
 * the Nano's four cores and starve the LVGL render thread. The pool caps
 * background concurrency and splits it into two lanes:
 *
 *  - Interactive: work the user is waiting on (music fetch, wifi connect).
 *    Runs at normal priority.
 *  - Prefetch: opportunistic warm-up work (boot prefetch, cache refills).
 *    Runs at nice +10 so it never competes with rendering or interactive
 *    jobs, and at most half the workers may run prefetch at once so an
 *    interactive job always finds a free worker quickly.
 *
 * Jobs must be finite (no forever-loops — keep those on their own threads)
 * and must not hold the LVGL lock; marshal results back to the UI through
 * Application::Schedule + LvglLockGuard like every existing worker does.
 * Exceptions escaping a job are caught and logged, never fatal. */

#include <functional>

namespace jetson {

class TaskPool {
public:
    enum class Lane { Interactive, Prefetch };

    static TaskPool &Instance();

    /* Queue a job. Never blocks; jobs run in FIFO order within their lane,
     * interactive before prefetch when both are pending. */
    void Post(Lane lane, std::function<void()> job);

private:
    TaskPool();
    ~TaskPool() = delete; // leaked singleton: workers live for the process
    TaskPool(const TaskPool &) = delete;
    TaskPool &operator=(const TaskPool &) = delete;

    struct State;
    State *state_;
};

} // namespace jetson
