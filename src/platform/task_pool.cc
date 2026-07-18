#include "platform/task_pool.h"

#include "esp_log.h"

#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

#define TAG "TaskPool"

namespace jetson {
namespace {

/* Nice level for prefetch jobs. High enough that CFS gives interactive and
 * render threads almost the whole machine when they are runnable, low enough
 * that prefetch still makes progress on an idle system. */
constexpr int kPrefetchNice = 10;

void SetThreadNice(int nice_value) {
    // setpriority() with PRIO_PROCESS + gettid() adjusts one thread on Linux.
    const pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));
    if (setpriority(PRIO_PROCESS, tid, nice_value) != 0)
        ESP_LOGW(TAG, "setpriority(%d) failed: %s", nice_value,
                 std::strerror(errno));
}

} // namespace

struct TaskPool::State {
    std::mutex mtx;
    std::condition_variable cv;
    std::deque<std::function<void()>> interactive;
    std::deque<std::function<void()>> prefetch;
    int running_prefetch = 0;
    int max_prefetch = 1;
    std::vector<std::thread> workers;
};

TaskPool &TaskPool::Instance() {
    // Leaked on purpose: workers block on the condition variable forever and
    // the process exits via main() returning; a static destructor would call
    // std::terminate on the still-joinable threads.
    static TaskPool *instance = new TaskPool();
    return *instance;
}

TaskPool::TaskPool() : state_(new State()) {
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    // Leave a core for the LVGL handler + render thread.
    const unsigned worker_count = hw <= 2 ? 2 : hw - 1;
    state_->max_prefetch = static_cast<int>(worker_count / 2);
    if (state_->max_prefetch < 1) state_->max_prefetch = 1;

    State *state = state_;
    for (unsigned i = 0; i < worker_count; ++i) {
        state->workers.emplace_back([state]() {
            int current_nice = 0;
            for (;;) {
                std::function<void()> job;
                bool is_prefetch = false;
                {
                    std::unique_lock<std::mutex> lk(state->mtx);
                    state->cv.wait(lk, [state]() {
                        return !state->interactive.empty() ||
                               (!state->prefetch.empty() &&
                                state->running_prefetch < state->max_prefetch);
                    });
                    if (!state->interactive.empty()) {
                        job = std::move(state->interactive.front());
                        state->interactive.pop_front();
                    } else {
                        job = std::move(state->prefetch.front());
                        state->prefetch.pop_front();
                        is_prefetch = true;
                        ++state->running_prefetch;
                    }
                }
                const int wanted_nice = is_prefetch ? kPrefetchNice : 0;
                if (wanted_nice != current_nice) {
                    SetThreadNice(wanted_nice);
                    current_nice = wanted_nice;
                }
                try {
                    job();
                } catch (const std::exception &exception) {
                    ESP_LOGE(TAG, "job threw: %s", exception.what());
                } catch (...) {
                    ESP_LOGE(TAG, "job threw an unknown exception");
                }
                if (is_prefetch) {
                    std::lock_guard<std::mutex> lk(state->mtx);
                    --state->running_prefetch;
                    // A prefetch slot opened up; wake a waiting worker.
                    state->cv.notify_one();
                }
            }
        });
    }
    ESP_LOGI(TAG, "started %u workers (max %d concurrent prefetch)",
             worker_count, state_->max_prefetch);
}

void TaskPool::Post(Lane lane, std::function<void()> job) {
    if (!job) return;
    {
        std::lock_guard<std::mutex> lk(state_->mtx);
        if (lane == Lane::Interactive)
            state_->interactive.push_back(std::move(job));
        else
            state_->prefetch.push_back(std::move(job));
    }
    state_->cv.notify_one();
}

} // namespace jetson
