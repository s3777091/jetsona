#include "lvgl_runtime.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include "esp_log.h"

#include "lvgl.h"

#define TAG "lvgl"

namespace jetson {

LvglRuntime &LvglRuntime::Instance() {
    static LvglRuntime inst;
    return inst;
}

LvglRuntime::~LvglRuntime() { Stop(); }

lv_display_t *LvglRuntime::createDisplayDrm(int width, int height) {
    (void)width;
    (void)height;
    lv_display_t *disp = lv_linux_drm_create();
    if (!disp) {
        ESP_LOGE(TAG, "lv_linux_drm_create failed");
        return nullptr;
    }
    /* connector_id = -1 -> auto-select first connected connector.
     * The 7" HDMI LCD B advertises 800x480 as its preferred mode. */
    const char *card = std::getenv("JETSON_DRM_CARD");
    if (!card) card = "/dev/dri/card0";
    lv_linux_drm_set_file(disp, card, -1);
    ESP_LOGI(TAG, "DRM display on %s (%dx%d)", card,
             (int)lv_display_get_hor_res(disp), (int)lv_display_get_ver_res(disp));
    return disp;
}

lv_display_t *LvglRuntime::createDisplaySdl(int width, int height) {
#if defined(JETSON_DISPLAY_BACKEND_SDL)
    lv_display_t *disp = lv_sdl_window_create(width, height);
    if (disp) lv_sdl_window_set_title(disp, "Jetson DS-02");
    return disp;
#else
    (void)width;
    (void)height;
    ESP_LOGE(TAG, "Firmware built without SDL backend. Rebuild with -DJETSON_DISPLAY_BACKEND=SDL");
    return nullptr;
#endif
}

void LvglRuntime::openTouch() {
    /* Scan /dev/input/event* and register the first usable device as a
     * pointer (the 7" HDMI LCD B touch screen). */
    const char *forced = std::getenv("JETSON_TOUCH_DEVICE");
    if (forced && forced[0]) {
        pointer_ = lv_evdev_create(LV_INDEV_TYPE_POINTER, forced);
        if (pointer_) {
            ESP_LOGI(TAG, "touch: %s", forced);
            return;
        }
    }
    for (int i = 0; i < 12; ++i) {
        char path[32];
        std::snprintf(path, sizeof(path), "/dev/input/event%d", i);
        pointer_ = lv_evdev_create(LV_INDEV_TYPE_POINTER, path);
        if (pointer_) {
            ESP_LOGI(TAG, "touch: %s", path);
            return;
        }
    }
    ESP_LOGW(TAG, "no evdev touch device found (UI will be non-interactive)");
}

bool LvglRuntime::Init(int width, int height) {
    lv_init();

#if defined(JETSON_DISPLAY_BACKEND_SDL)
    display_ = createDisplaySdl(width, height);
#else
    display_ = createDisplayDrm(width, height);
#endif
    if (!display_) {
        ESP_LOGE(TAG, "display creation failed");
        return false;
    }

    openTouch();

    running_ = true;
    tick_thread_ = std::thread([this]() {
        while (running_.load()) {
            lv_tick_inc(5);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    return true;
}

void LvglRuntime::StartHandler() {
    running_ = true;
    handler_thread_ = std::thread([this]() {
        while (running_.load()) {
            uint32_t ms = lv_timer_handler();
            if (ms == 0) ms = 5;
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        }
    });
}

void LvglRuntime::Stop() {
    if (!running_.exchange(false)) return;
    if (tick_thread_.joinable()) tick_thread_.join();
    if (handler_thread_.joinable()) handler_thread_.join();
}

} // namespace jetson