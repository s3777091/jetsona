#include "lvgl_runtime.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <linux/input.h>
#include <string>
#include <unistd.h>
#include <vector>

#include "esp_log.h"

#include "lvgl.h"
#include "lvgl_image.h"

#define TAG "lvgl"

namespace {
/* State for the custom USB-keyboard keypad indev (see openKeyboard). LVGL's
 * stock evdev keypad only maps navigation keys (arrows/enter/esc/...) and
 * discards letters, so a physical keyboard can't type into textareas. We read
 * evdev ourselves and map Linux keycodes to ASCII chars (with Shift/CapsLock),
 * letting the keypad deliver printable glyphs to the focused textarea through
 * its group. */
struct KeyboardState {
    int fd = -1;
    bool shift = false;
    bool caps = false;
    int key = 0;
    lv_indev_state_t state = LV_INDEV_STATE_RELEASED;
};

/* US-keyboard evdev keycode -> LVGL key. Returns 0 for unmapped / modifier-only
 * keys. Printable keys return their ASCII char; navigation/edit keys return the
 * matching LV_KEY_* constant. */
static int linux_key_to_lv(uint16_t code, bool upper)
{
    switch(code) {
        /* Letters */
        case 30: return upper ? 'A' : 'a';   /* KEY_A */
        case 48: return upper ? 'B' : 'b';
        case 46: return upper ? 'C' : 'c';
        case 32: return upper ? 'D' : 'd';
        case 18: return upper ? 'E' : 'e';
        case 33: return upper ? 'F' : 'f';
        case 34: return upper ? 'G' : 'g';
        case 35: return upper ? 'H' : 'h';
        case 23: return upper ? 'I' : 'i';
        case 36: return upper ? 'J' : 'j';
        case 37: return upper ? 'K' : 'k';
        case 38: return upper ? 'L' : 'l';
        case 50: return upper ? 'M' : 'm';
        case 49: return upper ? 'N' : 'n';
        case 24: return upper ? 'O' : 'o';
        case 25: return upper ? 'P' : 'p';
        case 16: return upper ? 'Q' : 'q';
        case 19: return upper ? 'R' : 'r';
        case 31: return upper ? 'S' : 's';
        case 20: return upper ? 'T' : 't';
        case 22: return upper ? 'U' : 'u';
        case 47: return upper ? 'V' : 'v';
        case 17: return upper ? 'W' : 'w';
        case 45: return upper ? 'X' : 'x';
        case 21: return upper ? 'Y' : 'y';
        case 44: return upper ? 'Z' : 'z';
        /* Top-row digits + shifted symbols */
        case 2:  return upper ? '!' : '1';
        case 3:  return upper ? '@' : '2';
        case 4:  return upper ? '#' : '3';
        case 5:  return upper ? '$' : '4';
        case 6:  return upper ? '%' : '5';
        case 7:  return upper ? '^' : '6';
        case 8:  return upper ? '&' : '7';
        case 9:  return upper ? '*' : '8';
        case 10: return upper ? '(' : '9';
        case 11: return upper ? ')' : '0';
        case 12: return upper ? '_' : '-';   /* KEY_MINUS */
        case 13: return upper ? '+' : '=';   /* KEY_EQUAL */
        /* Punctuation */
        case 26: return upper ? '{' : '[';    /* KEY_LEFTBRACE */
        case 27: return upper ? '}' : ']';    /* KEY_RIGHTBRACE */
        case 39: return upper ? ':' : ';';    /* KEY_SEMICOLON */
        case 40: return upper ? '"' : '\'';   /* KEY_APOSTROPHE */
        case 41: return upper ? '~' : '`';    /* KEY_GRAVE */
        case 43: return upper ? '|' : '\\';   /* KEY_BACKSLASH */
        case 51: return upper ? '<' : ',';    /* KEY_COMMA */
        case 52: return upper ? '>' : '.';    /* KEY_DOT */
        case 53: return upper ? '?' : '/';    /* KEY_SLASH */
        case 57: return ' ';                   /* KEY_SPACE */
        /* Editing / navigation */
        case 28: return LV_KEY_ENTER;          /* KEY_ENTER */
        case 14: return LV_KEY_BACKSPACE;      /* KEY_BACKSPACE */
        case 111: return LV_KEY_DEL;           /* KEY_DELETE */
        case 15: return LV_KEY_NEXT;           /* KEY_TAB */
        case 1: return LV_KEY_ESC;             /* KEY_ESC */
        case 103: return LV_KEY_UP;
        case 108: return LV_KEY_DOWN;
        case 105: return LV_KEY_LEFT;
        case 106: return LV_KEY_RIGHT;
        case 102: return LV_KEY_HOME;
        case 107: return LV_KEY_END;
        default: return 0;
    }
}

static void keyboard_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    auto *st = static_cast<KeyboardState *>(lv_indev_get_driver_data(indev));
    if (!st || st->fd < 0) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    struct input_event ev;
    while (read(st->fd, &ev, sizeof(ev)) > 0) {
        if (ev.type != EV_KEY) continue;
        if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT) {
            st->shift = (ev.value != 0);
            continue;
        }
        if (ev.code == KEY_CAPSLOCK) {
            if (ev.value) st->caps = !st->caps;
            continue;
        }
        int mapped = linux_key_to_lv((uint16_t)ev.code, st->shift ^ st->caps);
        if (mapped == 0) continue;
        st->key = mapped;
        st->state = ev.value ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    }
    data->key = st->key;
    data->state = st->state;
}

/* LVGL pointer indevs have no visible cursor by default; a relative mouse
 * moves the focus point but the user can't see where it is. Attach an
 * arrow-shaped cursor (macOS/Windows style) loaded from a small PNG.
 *
 * The cursor is parented to lv_layer_top() (not the active screen) so it stays
 * above every overlay/app view -- otherwise opening a full-screen app covers
 * the cursor and the pointer "disappears". lv_indev_set_cursor positions the
 * object's top-left at the pointer, and the arrow's tip sits at that corner, so
 * the hot-spot is the visible tip. The LvglImage is held in a function-local
 * static so its buffer outlives every cursor that references its dsc. */
static void attach_pointer_cursor(lv_indev_t *indev)
{
    if (!indev) return;
    lv_obj_t *cur = lv_image_create(lv_layer_top());
    lv_obj_remove_style_all(cur);
    static auto cursor_img = LvglImageFromFile("assets/icons/app/cursor.png");
    if (cursor_img) {
        lv_image_set_src(cur, cursor_img->image_dsc());
    } else {
        /* Fallback: a small high-contrast circle if the PNG is missing. */
        lv_obj_set_size(cur, 12, 12);
        lv_obj_set_style_radius(cur, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(cur, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(cur, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(cur, 2, 0);
        lv_obj_set_style_border_color(cur, lv_color_black(), 0);
    }
    lv_indev_set_cursor(indev, cur);
}
} // namespace

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
             (int)lv_display_get_horizontal_resolution(disp), (int)lv_display_get_vertical_resolution(disp));
    return disp;
}

lv_display_t *LvglRuntime::createDisplayFbdev(int width, int height) {
    (void)width;
    (void)height;
    /* Linux framebuffer path (e.g. /dev/fb0). Use this on the stock JetPack
     * 4.6.1 kernel, which has no DRM/KMS (NVIDIA tegrafb + nvidia_drv X driver).
     * The display's resolution comes from the framebuffer's fixed/virtual screen
     * info, so the requested width/height are advisory. */
    lv_display_t *disp = lv_linux_fbdev_create();
    if (!disp) {
        ESP_LOGE(TAG, "lv_linux_fbdev_create failed");
        return nullptr;
    }
    const char *fb = std::getenv("JETSON_FB_DEVICE");
    if (!fb) fb = "/dev/fb0";
    lv_linux_fbdev_set_file(disp, fb);
    // Partial refresh: only invalidated areas are flushed to the framebuffer.
    // force_refresh=true re-flushes the whole 800x480 every frame, which on the
    // single-buffered tegrafb panel shows as continuous full-screen flicker and
    // wipes one-shot draws (e.g. splash wordmark) between frames.
    lv_linux_fbdev_set_force_refresh(disp, false);
    ESP_LOGI(TAG, "FBDEV display on %s (%dx%d)", fb,
             (int)lv_display_get_horizontal_resolution(disp), (int)lv_display_get_vertical_resolution(disp));
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

/* Wayland backend: the firmware runs as a Wayland client under a compositor
 * (weston with the KMS/DRM backend). lv_wayland_window_create registers the
 * display plus its own pointer + keyboard indev from Wayland seat events, so
 * we must NOT also open evdev here (that would double-register touch). */
lv_display_t *LvglRuntime::createDisplayWayland(int width, int height) {
#if defined(JETSON_DISPLAY_BACKEND_WAYLAND)
    lv_display_t *disp = lv_wayland_window_create(
        (uint32_t)width, (uint32_t)height, (char *)"Jetson FW", nullptr);
    if (!disp) {
        ESP_LOGE(TAG, "lv_wayland_window_create failed - is weston running and "
                      "WAYLAND_DISPLAY exported in its env?");
        return nullptr;
    }
    lv_wayland_window_set_fullscreen(disp, true);
    ESP_LOGI(TAG, "Wayland window %dx%d (fullscreen) under compositor", width, height);
    return disp;
#else
    (void)width;
    (void)height;
    ESP_LOGE(TAG, "Firmware built without Wayland backend. Rebuild with "
                 "-DJETSON_DISPLAY_BACKEND=WAYLAND");
    return nullptr;
#endif
}

/* Read the first space-separated word of a /sys file as hex (the low
 * 64-bit word of a capability/property bitmask, which holds bits 0-63). */
static unsigned long readSysFirstHexWord(const char *path) {
    std::ifstream f(path);
    std::string word;
    if (!(f >> word)) return 0;
    return std::strtoul(word.c_str(), nullptr, 16);
}

void LvglRuntime::openTouch() {
    /* A touch screen exposes either the legacy single-touch axes ABS_X (bit 0)
     * + ABS_Y (bit 1), or the multitouch axes ABS_MT_POSITION_X (0x35) +
     * ABS_MT_POSITION_Y (0x36) -- many USB panels (e.g. Waveshare 7" HDMI LCD)
     * only report the multitouch axes. A direct touch panel also sets
     * INPUT_PROP_DIRECT (properties bit 0). Grabbing the first event device
     * that merely opens would mis-select non-pointing devices (Tegra HDMI/audio
     * event0, gpio-keys, USB mice), so scan by capability and prefer a
     * direct-touch device, preferring legacy ABS_X/Y over multitouch-only. */
    constexpr unsigned long kAbsXY = 0x3UL;                              // ABS_X | ABS_Y
    constexpr unsigned long kAbsMtXY = (1UL << 0x35) | (1UL << 0x36);   // ABS_MT_POSITION_X | ABS_MT_POSITION_Y

    const char *forced = std::getenv("JETSON_TOUCH_DEVICE");
    if (forced && forced[0]) {
        pointer_ = lv_evdev_create(LV_INDEV_TYPE_POINTER, forced);
        if (pointer_) {
            ESP_LOGI(TAG, "touch: %s (forced)", forced);
            return;
        }
        ESP_LOGW(TAG, "forced touch %s failed to open", forced);
    }

    int best = -1;
    bool bestDirect = false;
    bool bestLegacy = false;
    for (int i = 0; i < 16; ++i) {
        char sysp[96];
        std::snprintf(sysp, sizeof(sysp),
                      "/sys/class/input/event%d/device/capabilities/abs", i);
        unsigned long abs = readSysFirstHexWord(sysp);
        bool legacy = (abs & kAbsXY) == kAbsXY;
        bool mt = (abs & kAbsMtXY) == kAbsMtXY;
        if (!legacy && !mt) continue;
        std::snprintf(sysp, sizeof(sysp),
                      "/sys/class/input/event%d/device/properties", i);
        unsigned long props = readSysFirstHexWord(sysp);
        bool direct = (props & 0x1UL) != 0; /* INPUT_PROP_DIRECT */
        ESP_LOGI(TAG, "touch candidate: event%d abs=0x%lx direct=%d %s",
                 i, abs, direct ? 1 : 0, legacy ? "(ABS_X/Y)" : "(ABS_MT)");
        /* Pick: direct > indirect; within that, legacy ABS_X/Y > multitouch. */
        bool take = false;
        if (best < 0) take = true;
        else if (direct && !bestDirect) take = true;
        else if (direct == bestDirect && legacy && !bestLegacy) take = true;
        if (take) { best = i; bestDirect = direct; bestLegacy = legacy; }
    }
    if (best >= 0) {
        char path[32];
        std::snprintf(path, sizeof(path), "/dev/input/event%d", best);
        pointer_ = lv_evdev_create(LV_INDEV_TYPE_POINTER, path);
        if (pointer_) {
            ESP_LOGI(TAG, "touch: %s%s%s", path,
                     bestDirect ? " (direct)" : "",
                     bestLegacy ? "" : " (multitouch)");
            return;
        }
        ESP_LOGW(TAG, "touch: open %s failed", path);
    }
    ESP_LOGW(TAG, "no evdev touch device found (UI will be non-interactive)");
}

void LvglRuntime::openKeyboard() {
    /* Register a USB keyboard as a LVGL keypad indev so the chat/terminal
     * textareas receive key events. A real keyboard sets KEY_Q (bit 16) in its
     * key capability bitmask; gpio-keys (only a power button) does not, so this
     * avoids mis-selecting it.
     *
     * We do NOT use lv_evdev_create here: its keypad read only maps navigation
     * keys (arrows/enter/esc/...) and drops letters, so a physical keyboard
     * couldn't type. Instead we open the evdev fd ourselves and install a read
     * callback that maps Linux keycodes to ASCII (with Shift/CapsLock). Keys
     * are delivered to the focused textarea through keypad_group_, which the
     * views add their textareas to. */
    std::string devpath;
    const char *forced = std::getenv("JETSON_KEYBOARD_DEVICE");
    if (forced && forced[0]) {
        std::string f(forced);
        devpath = (f.find('/') == std::string::npos) ? std::string("/dev/input/") + f : f;
    } else {
        for (int i = 0; i < 16; ++i) {
            char sysp[96];
            std::snprintf(sysp, sizeof(sysp),
                          "/sys/class/input/event%d/device/capabilities/key", i);
            unsigned long key = readSysFirstHexWord(sysp);
            if (((key >> 16) & 0x1UL) == 0) continue; /* KEY_Q => real keyboard */
            char path[32];
            std::snprintf(path, sizeof(path), "/dev/input/event%d", i);
            devpath = path;
            break;
        }
    }
    if (devpath.empty()) {
        ESP_LOGW(TAG, "no evdev keyboard found");
        return;
    }

    int fd = open(devpath.c_str(), O_RDONLY | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) {
        ESP_LOGW(TAG, "keyboard: open %s failed: %s", devpath.c_str(), strerror(errno));
        return;
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);

    auto *st = new KeyboardState();
    st->fd = fd;
    keyboard_ = lv_indev_create();
    lv_indev_set_type(keyboard_, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(keyboard_, keyboard_read_cb);
    lv_indev_set_driver_data(keyboard_, st);

    /* Keypads deliver keys to the focused object of their group; create one
     * shared group and let the chat/terminal/wifi views add their textareas. */
    if (!keypad_group_) keypad_group_ = lv_group_create();
    lv_indev_set_group(keyboard_, keypad_group_);

    ESP_LOGI(TAG, "keyboard: %s", devpath.c_str());
}

void LvglRuntime::openMouse() {
    /* Register a single USB (relative) mouse as the LVGL pointer indev -- one
     * mouse, one cursor. A relative mouse has REL_X (rel bit 0); a touch panel
     * has ABS_X+ABS_Y or ABS_MT (owned by openTouch), and a keyboard (e.g. a
     * "Hyperwork" USB keyboard) may also report REL_X yet is not a mouse, so
     * skip anything with KEY_Q (key bit 16) -- that's how openKeyboard detects
     * a real keyboard. Taking only the first qualifying device avoids the
     * "two cursors" symptom when a keyboard/combo node also exposes REL_X.
     * LVGL's evdev driver accumulates REL_X/REL_Y for pointer devices, so a
     * relative mouse works without ABS axes (it logs a harmless EVIOCGABS
     * error at create time). Override with JETSON_MOUSE_DEVICE if needed. */
    std::string devpath;
    const char *forced = std::getenv("JETSON_MOUSE_DEVICE");
    if (forced && forced[0]) {
        std::string f(forced);
        devpath = (f.find('/') == std::string::npos) ? std::string("/dev/input/") + f : f;
    } else {
        constexpr unsigned long kAbsMtXY = (1UL << 0x35) | (1UL << 0x36);
        for (int i = 0; i < 16; ++i) {
            char sysp[96];
            std::snprintf(sysp, sizeof(sysp),
                          "/sys/class/input/event%d/device/capabilities/rel", i);
            unsigned long rel = readSysFirstHexWord(sysp);
            if ((rel & 0x1UL) == 0) continue; /* REL_X => relative pointer */
            std::snprintf(sysp, sizeof(sysp),
                          "/sys/class/input/event%d/device/capabilities/key", i);
            unsigned long key = readSysFirstHexWord(sysp);
            if ((key >> 16) & 0x1UL) continue; /* KEY_Q => keyboard, not a mouse */
            std::snprintf(sysp, sizeof(sysp),
                          "/sys/class/input/event%d/device/capabilities/abs", i);
            unsigned long abs = readSysFirstHexWord(sysp);
            if ((abs & 0x3UL) == 0x3UL) continue; /* ABS_X+ABS_Y => touch, not mouse */
            if ((abs & kAbsMtXY) == kAbsMtXY) continue; /* multitouch panel => touch */
            char path[32];
            std::snprintf(path, sizeof(path), "/dev/input/event%d", i);
            devpath = path;
            break; /* one mouse is enough -> a single cursor */
        }
    }
    if (devpath.empty()) {
        ESP_LOGW(TAG, "no evdev mouse found");
        return;
    }
    lv_indev_t *m = lv_evdev_create(LV_INDEV_TYPE_POINTER, devpath.c_str());
    if (!m) {
        ESP_LOGW(TAG, "mouse: open %s failed", devpath.c_str());
        return;
    }
    attach_pointer_cursor(m);
    mouse_ = m;
    ESP_LOGI(TAG, "mouse: %s", devpath.c_str());
}

bool LvglRuntime::Init(int width, int height) {
    lv_init();

#if defined(JETSON_DISPLAY_BACKEND_SDL)
    display_ = createDisplaySdl(width, height);
#elif defined(JETSON_DISPLAY_BACKEND_WAYLAND)
    display_ = createDisplayWayland(width, height);
#elif defined(JETSON_DISPLAY_BACKEND_FBDEV)
    display_ = createDisplayFbdev(width, height);
#else
    display_ = createDisplayDrm(width, height);
#endif
    if (!display_) {
        ESP_LOGE(TAG, "display creation failed");
        return false;
    }

    /* The Wayland driver registers its own pointer + keyboard indev from the
     * compositor's seat events; opening evdev on top of that would
     * double-register touch. Only open evdev for the DRM/SDL/fbdev paths. */
#if !defined(JETSON_DISPLAY_BACKEND_WAYLAND)
    openTouch();
    openKeyboard();
    openMouse();
#endif

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