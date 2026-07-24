#pragma once

/* Linux thread tuning helpers for the Jetson Nano B01 (quad A57).
 *
 * The voice loop's latency-sensitive path -- the mic capture thread feeding the
 * wake-word detector -- must not be starved by agent / STT bursts. Pinning it to
 * a dedicated core keeps capture jitter low; the agent and STT run on the other
 * cores (STT also offloads to the GPU via the CUDA EP). These are best-effort:
 * they no-op outside Linux. */

#include <pthread.h>
#include <sched.h>
#include <string>

#include <thread>

namespace jetson::platform {

inline void SetThreadName(std::thread &t, const char *name) {
#ifdef __linux__
    pthread_setname_np(t.native_handle(), name);
#else
    (void)t; (void)name;
#endif
}

inline bool PinToCore(std::thread &t, int core) {
#ifdef __linux__
    if (core < 0) return false;
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(core, &cs);
    return pthread_setaffinity_np(t.native_handle(), sizeof(cs), &cs) == 0;
#else
    (void)t; (void)core;
    return false;
#endif
}

} // namespace jetson::platform