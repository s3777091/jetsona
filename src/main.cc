#include "application.h"
#include "esp_log.h"
#include "lvgl_runtime.h"
#include "platform/perf_governor.h"

#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <signal.h>
#include <thread>

#define TAG "main"

int main(int /*argc*/, char ** /*argv*/) {
    // Block termination signals before any worker threads are created. A
    // dedicated sigwait thread handles them synchronously, so logging and the
    // application/LVGL shutdown path never run inside an async signal handler.
    sigset_t termination_signals;
    sigemptyset(&termination_signals);
    sigaddset(&termination_signals, SIGINT);
    sigaddset(&termination_signals, SIGTERM);
    const int mask_rc = pthread_sigmask(SIG_BLOCK, &termination_signals, nullptr);
    if (mask_rc != 0) {
        ESP_LOGE(TAG, "could not block termination signals: %s", std::strerror(mask_rc));
        return EXIT_FAILURE;
    }

    ESP_LOGI(TAG, "Jetson Nano DS-02 firmware starting (display=%dx%d, backend=%s)",
             JETSON_DISPLAY_WIDTH, JETSON_DISPLAY_HEIGHT, JETSON_DISPLAY_BACKEND);

    // Replay a clock baseline left behind by a crash mid-boost, before any
    // new boost can be requested.
    jetson::PerfGovernor::Instance().Init();

    auto &app = Application::GetInstance();
    if (!app.Initialize()) return EXIT_FAILURE;

    std::thread signal_thread([&app, termination_signals]() mutable {
        int sig = 0;
        const int wait_rc = sigwait(&termination_signals, &sig);
        if (wait_rc != 0) {
            ESP_LOGE(TAG, "sigwait failed: %s", std::strerror(wait_rc));
            app.RequestStop();
            return;
        }
        ESP_LOGW(TAG, "signal %d received, stopping", sig);
        app.RequestStop();
    });

    app.Run();
    signal_thread.join();
    jetson::LvglRuntime::Instance().Stop();
    ESP_LOGI(TAG, "shutdown complete");
    return EXIT_SUCCESS;
}
