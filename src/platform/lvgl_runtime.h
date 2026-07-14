#ifndef JETSON_PLATFORM_LVGL_RUNTIME_H
#define JETSON_PLATFORM_LVGL_RUNTIME_H

#include <atomic>
#include <thread>

#ifdef __cplusplus
extern "C" {
#endif
typedef struct _lv_display_t lv_display_t;
typedef struct _lv_indev_t lv_indev_t;
#ifdef __cplusplus
}
#endif

namespace jetson {

class LvglRuntime {
public:
    static LvglRuntime &Instance();

    /* Initialise LVGL, create the display (DRM or SDL) at width x height,
     * open the evdev touch pointer, and start the lv_tick thread. */
    bool Init(int width, int height);

    /* Start the LVGL timer-handler loop in a background thread. */
    void StartHandler();

    /* Stop everything and join threads (for clean shutdown). */
    void Stop();

    lv_display_t *display() const { return display_; }
    lv_indev_t *pointer() const { return pointer_; }

private:
    LvglRuntime() = default;
    ~LvglRuntime();
    LvglRuntime(const LvglRuntime &) = delete;
    LvglRuntime &operator=(const LvglRuntime &) = delete;

    lv_display_t *createDisplayDrm(int width, int height);
    lv_display_t *createDisplaySdl(int width, int height);
    lv_display_t *createDisplayWayland(int width, int height);
    void openTouch();

    lv_display_t *display_ = nullptr;
    lv_indev_t *pointer_ = nullptr;
    std::thread tick_thread_;
    std::thread handler_thread_;
    std::atomic<bool> running_{false};
};

} // namespace jetson

#endif