#ifndef JETSON_PLATFORM_LVGL_RUNTIME_H
#define JETSON_PLATFORM_LVGL_RUNTIME_H

#include <atomic>
#include <thread>

#ifdef __cplusplus
extern "C" {
#endif
typedef struct lv_display_t lv_display_t;
typedef struct lv_indev_t lv_indev_t;
typedef struct lv_group_t lv_group_t;
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

    /* Cap the handler loop's sleep between LVGL cycles. Default 5 ms keeps
     * interactive input snappy; raising it (e.g. 100 ms) while the UI is
     * asleep cuts CPU wakeups ~20x. Input is still read inside the handler
     * loop, so wake latency is bounded by this value. */
    void SetIdleSleepMs(uint32_t ms) { max_sleep_ms_.store(ms); }

    lv_display_t *display() const { return display_; }
    lv_indev_t *pointer() const { return pointer_; }
    lv_indev_t *keyboard() const { return keyboard_; }
    lv_indev_t *mouse() const { return mouse_; }

    /* Modifier state from the custom evdev keyboard. Views that implement
     * desktop-style shortcuts (for example TerminalView's Ctrl+C / Ctrl+V)
     * can distinguish a chord from an ordinary printable c/v key. */
    bool KeyboardCtrlPressed() const { return keyboard_ctrl_pressed_.load(); }

    /* Group the USB keyboard delivers key events to. The chat/terminal/wifi
     * views add their textareas here so a physical keyboard can type into them. */
    lv_group_t *keypad_group() const { return keypad_group_; }

private:
    LvglRuntime() = default;
    ~LvglRuntime();
    LvglRuntime(const LvglRuntime &) = delete;
    LvglRuntime &operator=(const LvglRuntime &) = delete;

    lv_display_t *createDisplayDrm(int width, int height);
    lv_display_t *createDisplayFbdev(int width, int height);
    lv_display_t *createDisplaySdl(int width, int height);
    lv_display_t *createDisplayWayland(int width, int height);
    void openTouch();
    void openKeyboard();
    void openMouse();
    bool acquireDisplayLease();
    void releaseDisplayLease();
    void enterGraphicsConsole();
    void restoreTextConsole();

    lv_display_t *display_ = nullptr;
    lv_indev_t *pointer_ = nullptr;
    lv_indev_t *keyboard_ = nullptr;
    lv_indev_t *mouse_ = nullptr;
    lv_group_t *keypad_group_ = nullptr;
    int display_lease_fd_ = -1;
    int console_fd_ = -1;
    int console_prev_mode_ = -1;
    std::thread tick_thread_;
    std::thread handler_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> keyboard_ctrl_pressed_{false};
    std::atomic<uint32_t> max_sleep_ms_{5};
};

} // namespace jetson

#endif
