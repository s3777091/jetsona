#pragma once

/* Dynamic CPU-frequency boost with a dead-man's switch.
 *
 * Heavy moments (boot warm-up, music discover fetch, album open) briefly pin
 * cpufreq's scaling_min_freq to the maximum so the four A57 cores render and
 * decode at full clock, then restore the saved baseline so the Nano idles
 * cool again. Every safety concern is handled here, not at call sites:
 *
 *  - RAII + refcount: Acquire() returns a Token; the boost holds while any
 *    token is alive and the baseline is restored when the last one dies.
 *  - Watchdog: a leaked or wedged token cannot pin the clocks — a monitor
 *    thread force-restores the baseline kBoostTimeoutMs after the most recent
 *    Acquire() (each new acquire extends the deadline).
 *  - Crash recovery: the baseline min-freq of every core is persisted to
 *    /var/lib/jetson-fw/perf.baseline before the first boost and deleted on
 *    restore. Init() (call once at startup) replays a leftover file, so a
 *    crash mid-boost fixes itself on the next launch.
 *  - Thermal guard: no boost is applied above kBoostMaxTempC, and the
 *    watchdog drops an active boost if the hottest thermal zone passes
 *    kAbortTempC — this cooperates with the jetson-fan curve daemon instead
 *    of fighting it.
 *
 * Everything is best-effort: on a machine without the sysfs nodes (or without
 * root) Acquire() degrades to a no-op token. JETSON_PERF_BOOST=0 disables the
 * whole mechanism. */

#include <memory>

namespace jetson {

class PerfGovernor {
public:
    /* Keeps the boost alive while in scope; copyable/movable shared handle. */
    using Token = std::shared_ptr<void>;

    static PerfGovernor &Instance();

    /* Replay a persisted baseline left by a crash. Call once, early in main,
     * before any Acquire(). Safe no-op when there is nothing to recover. */
    void Init();

    /* Raise scaling_min_freq to max on every core (refcounted, watchdogged,
     * thermally gated). `reason` is only for logging. */
    Token Acquire(const char *reason);

private:
    PerfGovernor() = default;
    ~PerfGovernor() = delete; // leaked singleton, same rationale as TaskPool
    PerfGovernor(const PerfGovernor &) = delete;
    PerfGovernor &operator=(const PerfGovernor &) = delete;

    void Release();
    struct Impl;
    Impl *impl();
};

} // namespace jetson
