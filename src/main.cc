#include "application.h"
#include "esp_log.h"

#include <csignal>
#include <cstdlib>

#define TAG "main"

static void onSignal(int /*sig*/) {
    ESP_LOGW(TAG, "signal received, exiting");
    std::exit(0);
}

int main(int /*argc*/, char ** /*argv*/) {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    ESP_LOGI(TAG, "Jetson Nano DS-02 firmware starting (display=%dx%d, backend=%s)",
             JETSON_DISPLAY_WIDTH, JETSON_DISPLAY_HEIGHT, JETSON_DISPLAY_BACKEND);

    auto &app = Application::GetInstance();
    if (!app.Initialize()) return EXIT_FAILURE;
    app.Run(); // never returns
    return 0;
}
