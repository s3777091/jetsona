/* kiosk_bar: Dynamic Island + micro window manager for the
 * Chromium kiosk hand-off (scripts/launch_chromium.sh).
 *
 * The DS-02 firmware owns /dev/fb0 and exits with code 42 when the user opens
 * Chromium; the supervisor then starts a bare X session (xinit, no desktop, no
 * window manager). This program runs alongside Chromium inside that session
 * and provides the two things a WM-less kiosk is missing:
 *
 *  1. A compact black Dynamic Island centered over the web page. Its width
 *     follows the number of Chromium session windows. The optional advanced
 *     mode adds the current time and battery percentage inside the island;
 *     there is deliberately no full-width status strip or quick-setting row.
 *     Holding the island drops the app rail; its last slot is a red power
 *     button that exits this process and returns the panel to the firmware.
 *
 *  2. Micro-WM duties. Without a window manager Chromium never receives X
 *     input focus, so the mouse works but every keystroke is ignored. On each
 *     MapNotify of a normal (non-override-redirect) window we assign it input
 *     focus, keep full-screen web windows at panel size, and re-raise the
 *     island.
 *
 * Only libX11 is required (core protocol, core fonts); the Jetson already
 * ships it as an Xorg dependency. Build needs libx11-dev; libxext-dev gives
 * the floating windows their rounded shape.
 */
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#ifdef KIOSK_HAVE_SHAPE
#include <X11/extensions/shape.h> /* rounded window corners (libXext) */
#endif

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/i2c-dev.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "kiosk_icons.h" /* generated 48x48 RGBA minis of the drawer icons */

#define OSK_H       205
#define PILL_H      36
#define PILL_Y      3
#define ISLAND_ICON 22  /* even size centres exactly in the even 36px pill */
#define ISLAND_STEP 18
#define ISLAND_MAX_ICONS 7

/* Palette lifted from the firmware Dynamic Island. */
#define COL_BAR_BG    0x000000
#define COL_PILL_BG   0x000000
#define COL_PILL_EDGE 0x253142
#define COL_MENU_EDGE 0x5a5a5e  /* rail hairline: bright enough to read on black */
#define COL_TEXT      0xffffff
#define COL_BAT_GREEN 0x34c759
#define COL_BAT_YELLO 0xffcc00
#define COL_BAT_RED   0xff3b30

static Display *dpy;
static Window bar, root;
static GC gc;
static Pixmap bar_buffer = None;
static int sw, sh;
static XFontStruct *font_big, *font_small;
static Atom atom_net_wm_name, atom_utf8;
/* The bar window itself is only this wide, so the web page remains visible
 * (and receives input) everywhere around the island. */
static int island_w = PILL_H;
static int island_advanced = 0;

/* Settings applied once when the bare-X session starts. They have no controls
 * in Chromium; the firmware Settings app remains their single owner. */
static int session_volume = 50, session_muted = 0, session_brightness = 100;

/* Switcher list: browser-sized top-level windows. A single click on the
 * island pill cycles through them (the firmware island opens its app switcher
 * the same way); leaving the kiosk is the app rail's power button. */
#define MAX_WINS 16
static Window wins[MAX_WINS];
/* Onboard is an input surface, not a Chromium app window. Keep it above the
 * browser without putting it in the island's app switcher or stealing the
 * focused portal text field. */
static Window onscreen_keyboard = None;
static int win_app[MAX_WINS]; /* launcher entry that opened wins[i], -1 */
static char win_title[MAX_WINS][128];
static int win_badge[MAX_WINS];
static int nwins = 0, active = -1;
static Window queue_old[MAX_WINS];
static int queue_old_n = 0;
static long queue_anim_start = 0;

/* Long-press app launcher. Holding the pill bounces down a horizontal icon
 * rail backed by "Name|URL" entries, so launch_chromium.sh provides the web
 * apps and the firmware's Pods view appends the user's running GPU pods.
 * Re-read on every open: entries never go stale and no RAM is held between
 * uses. Launch goes through the Chromium profile singleton (a short-lived
 * relaunch that hands the URL to the running browser), and windows are
 * tagged with their entry so re-picking an open app focuses it instead of
 * burning ~100 MB on a duplicate renderer. */
#define MAX_APPS     24
#define MENU_H       66
#define MENU_ICON    42
/* Stops 2px short of MENU_STEP so a lifted icon never touches its neighbour,
 * and inside MENU_H so it is not clipped by the rail. */
#define MENU_ICON_ZOOM 50
#define MENU_HOVER_MS  140
#define MENU_STEP    52
#define MENU_PAD     8
#define MENU_RADIUS  18   /* all four corners, like the firmware pill */
#define LONGPRESS_MS 600
#define MENU_AUTOCLOSE_S 8
static struct { char name[40]; char url[512]; } apps[MAX_APPS];
static int napps = 0;
static Window menu = None;
static Pixmap menu_buffer = None; /* menu_w x MENU_H back buffer */
static int menu_open = 0, menu_hover = -1, menu_ticks = 0;
/* Hover feedback is a gentle scale, not a ring: the entering icon grows to
 * MENU_ICON_ZOOM while the one being left shrinks back to MENU_ICON over the
 * same MENU_HOVER_MS, so exactly one icon ever looks lifted. */
static int menu_hover_prev = -1;
static long menu_hover_anim = 0;
static int menu_w = 0, menu_visible = 0, menu_scroll = 0;
static long menu_anim_start = 0;
static int pending_app[8]; /* spawned entries awaiting their MapNotify */
static int npending = 0;

/* The rail shows one slot per launcher entry plus the trailing power button
 * that leaves the kiosk (replaces the accident-prone double-click gesture). */
static int menu_total(void) { return napps + 1; }

/* Window-switch transition: a full app-area override-redirect cover that
 * plays the island zoom. The leaving app collapses up into the island (screen
 * "closing"), the arriving app zooms back out to fill the panel -- the real
 * window swap underneath happens while the cover is opaque, so it is never
 * seen. Notifications (toast) still bloom compactly out of the island. */
static Window transition = None, transition_target = None;
static Pixmap transition_buffer = None;
static long transition_start = 0;
static int transition_switched = 0;
static int transition_rotate = 0;
static int transition_from_app = -1; /* app card shown while closing */
static int transition_show_app = -1; /* app card the current frame draws */
static double transition_scale = 1.0;/* 1 = fills panel, 0 = tucked in island */
/* Firmware hand-off: play only the second (bloom) half of the zoom, then hold
 * the card as a splash until the browser is actually on screen. */
static int transition_open_only = 0;
static long transition_hold = 0;     /* ms the splash started waiting, 0 = no */
static long transition_ready = 0;    /* ms Chromium first mapped a window */
static Window toast = None, toast_target = None;
static Pixmap toast_buffer = None;
static int toast_app = -1;
static int toast_base_w = PILL_H;
static long toast_start = 0, toast_until = 0;
static char toast_message[96];
#define TRANS_MS 460  /* island zoom: close over 0..0.5, open over 0.5..1 */
/* Hand-off splash: how long the bloomed card waits for Chromium to map a
 * window before lifting anyway, so a browser that fails to start can never
 * leave the panel covered for good. */
#define HANDOFF_MAX_MS 15000
/* Chromium maps its window before it has painted the page, so the splash
 * outstays the map by this much rather than landing on a white flash. */
#define HANDOFF_SETTLE_MS 450
#define TOAST_MAX_W 318
#define TOAST_MAX_H 68

static void fetch_window_title(Window w, char *out, size_t cap);
static void redraw(int bat_level, int bat_charging);
static long now_ms(void);
static void start_switch(Window target);
static void inspect_notification_window(Window w);
static void close_menu(void);
static int contains_ci(const char *haystack, const char *needle);
static void sync_island_geometry(void);

static int ignore_xerror(Display *d, XErrorEvent *e)
{
    /* Kiosk windows come and go while we hold their ids (BadWindow/BadMatch
     * races are normal for a WM-ish client); never abort on them. */
    (void)d; (void)e;
    return 0;
}

static unsigned long px(unsigned long rgb)
{
    /* The tegra X server runs a 24-bit TrueColor visual, so the pixel value
     * is the RGB triple itself; XAllocColor round-trips would be overkill. */
    return rgb;
}

/* ------------------------------------------------------------------ sysfs */

/* The firmware reads the Waveshare UPS through INA219 rather than sysfs.
 * Keep the same fallback here so Chromium does not suddenly show a fake 100%
 * battery while the rest of the firmware shows the real pack level. */
static int ina_fd = -1;
static int ina_tried = 0;

static int ina_write16(uint8_t reg, uint16_t value)
{
    uint8_t b[3] = {reg, (uint8_t)(value >> 8), (uint8_t)value};
    return write(ina_fd, b, sizeof(b)) == (ssize_t)sizeof(b);
}

static int ina_read16(uint8_t reg, uint16_t *value)
{
    uint8_t b[2];
    if (write(ina_fd, &reg, 1) != 1 || read(ina_fd, b, 2) != 2) return 0;
    *value = (uint16_t)((b[0] << 8) | b[1]);
    return 1;
}

static int ina_battery_read(int *level, int *charging)
{
    if (ina_fd < 0) {
        if (ina_tried) return 0;
        ina_tried = 1;
        const char *bus = getenv("INA219_BUS");
        if (!bus || !*bus) bus = "/dev/i2c-1";
        ina_fd = open(bus, O_RDWR);
        unsigned long addr = strtoul(getenv("INA219_ADDR") ? getenv("INA219_ADDR") : "0x42", NULL, 0);
        if (ina_fd < 0 || ioctl(ina_fd, I2C_SLAVE, addr) < 0 ||
            !ina_write16(0x00, 0x3eef)) {
            if (ina_fd >= 0) close(ina_fd);
            ina_fd = -1;
            return 0;
        }
        (void)ina_write16(0x05, 4096); /* 0.1 ohm shunt, 100 uA/step */
    }
    uint16_t bus_raw = 0, current_raw = 0;
    if (!ina_read16(0x02, &bus_raw)) {
        close(ina_fd);
        ina_fd = -1;
        ina_tried = 0; /* transient I2C loss: retry on the next 5s refresh */
        return 0;
    }
    float volts = ((bus_raw >> 3) * 4) / 1000.0f;
    float vmin = getenv("INA219_VMIN") ? strtof(getenv("INA219_VMIN"), NULL) : 6.0f;
    float vmax = getenv("INA219_VMAX") ? strtof(getenv("INA219_VMAX"), NULL) : 8.4f;
    int pct = vmax > vmin ? (int)((volts - vmin) / (vmax - vmin) * 100.0f + 0.5f) : 100;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    *level = pct;
    *charging = 0;
    if (ina_read16(0x04, &current_raw)) {
        /* Same orientation knob as the firmware (src/power/ina219.h): after the
         * multiply a positive current always means "into the pack", so both
         * bars agree on charging regardless of how the shunt is wired. */
        const char *s = getenv("INA219_CHARGE_SIGN");
        const char *t = getenv("INA219_CHARGE_MA");
        float sign = (s && strtof(s, NULL) < 0.0f) ? -1.0f : 1.0f;
        float thresh = t ? strtof(t, NULL) : 50.0f;
        if (thresh < 0.0f) thresh = -thresh;
        float ma = (float)(int16_t)current_raw * 0.1f * sign; /* 100uA/step */
        *charging = ma > thresh;
    }
    return 1;
}

static int battery_read(int *level, int *charging)
{
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) return ina_battery_read(level, charging);
    struct dirent *e;
    int found = 0;
    while (!found && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char path[512], buf[64];
        FILE *f;
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/type", e->d_name);
        if (!(f = fopen(path, "r"))) continue;
        buf[0] = 0;
        if (!fgets(buf, sizeof(buf), f)) buf[0] = 0;
        fclose(f);
        if (strncmp(buf, "Battery", 7) != 0) continue;
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/capacity", e->d_name);
        if (!(f = fopen(path, "r"))) continue;
        if (fscanf(f, "%d", level) == 1) found = 1;
        fclose(f);
        if (!found) continue;
        *charging = 0;
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/status", e->d_name);
        if ((f = fopen(path, "r"))) {
            buf[0] = 0;
            if (fgets(buf, sizeof(buf), f) && strncmp(buf, "Charging", 8) == 0)
                *charging = 1;
            fclose(f);
        }
    }
    closedir(d);
    return found ? 1 : ina_battery_read(level, charging);
}

/* Read one firmware KV value so Chromium follows the selected Web View,
 * clock, sound and display preferences. */
static void setting_get(const char *ns, const char *key, const char *fallback,
                        char *out, size_t cap)
{
    snprintf(out, cap, "%s", fallback);
    const char *path = getenv("JETSON_SETTINGS_FILE");
    if (!path || !*path) path = "/root/.jetson-fw/settings.kv";
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512], wanted[128];
    snprintf(wanted, sizeof(wanted), "%s\t%s\t", ns, key);
    size_t n = strlen(wanted);
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, wanted, n) != 0) continue;
        char *v = line + n;
        v[strcspn(v, "\r\n")] = 0;
        snprintf(out, cap, "%s", v);
        break;
    }
    fclose(f);
}

static void run_command(const char *command)
{
    int status = system(command);
    if (status == -1) {
        fprintf(stderr, "kiosk_bar: unable to start shell command\n");
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "kiosk_bar: shell command terminated by signal %d\n",
                WTERMSIG(status));
    } else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "kiosk_bar: shell command exited with status %d\n",
                WIFEXITED(status) ? WEXITSTATUS(status) : status);
    }
}

static void *command_worker(void *arg)
{
    char *command = (char *)arg;
    if (command) { run_command(command); free(command); }
    return NULL;
}

static void spawn_command(const char *command)
{
    pthread_t thread;
    char *copy = strdup(command);
    if (!copy) return;
    if (pthread_create(&thread, NULL, command_worker, copy) == 0)
        pthread_detach(thread);
    else free(copy);
}

static void apply_system_volume(void)
{
    char command[512];
    const char *mute = session_muted ? "mute" : "unmute";
    snprintf(command, sizeof(command),
        "amixer -q sset Master %d%% %s 2>/dev/null || "
        "amixer -q sset PCM %d%% %s 2>/dev/null || "
        "wpctl set-volume @DEFAULT_AUDIO_SINK@ %.2f 2>/dev/null",
        session_volume, mute, session_volume, mute, session_volume / 100.0);
    spawn_command(command);
}

static void apply_x_brightness(void)
{
    char command[512];
    snprintf(command, sizeof(command),
        "output=$(xrandr --current 2>/dev/null | awk '/ connected/{print $1; exit}'); "
        "[ -z \"$output\" ] || xrandr --output \"$output\" --brightness %.2f",
        session_brightness / 100.0);
    spawn_command(command);
}

/* ---------------------------------------------------------------- drawing */

static void draw_string(Drawable d, XFontStruct *fs, int x, int baseline,
                        const char *s)
{
    XSetFont(dpy, gc, fs->fid);
    XDrawString(dpy, d, gc, x, baseline, s, (int)strlen(s));
}

static void fill_round_rect_gc(Drawable d, GC g, int x, int y, int w, int h,
                               int r)
{
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    XFillArc(dpy, d, g, x, y, 2 * r, 2 * r, 90 * 64, 90 * 64);
    XFillArc(dpy, d, g, x + w - 2 * r, y, 2 * r, 2 * r, 0, 90 * 64);
    XFillArc(dpy, d, g, x, y + h - 2 * r, 2 * r, 2 * r, 180 * 64, 90 * 64);
    XFillArc(dpy, d, g, x + w - 2 * r, y + h - 2 * r, 2 * r, 2 * r, 270 * 64, 90 * 64);
    XFillRectangle(dpy, d, g, x + r, y, w - 2 * r, h);
    XFillRectangle(dpy, d, g, x, y + r, w, h - 2 * r);
}

static void fill_round_rect(Drawable d, int x, int y, int w, int h, int r,
                            int fill)
{
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    if (fill) {
        fill_round_rect_gc(d, gc, x, y, w, h, r);
    } else {
        XDrawArc(dpy, d, gc, x, y, 2 * r, 2 * r, 90 * 64, 90 * 64);
        XDrawArc(dpy, d, gc, x + w - 2 * r - 1, y, 2 * r, 2 * r, 0, 90 * 64);
        XDrawArc(dpy, d, gc, x, y + h - 2 * r - 1, 2 * r, 2 * r, 180 * 64, 90 * 64);
        XDrawArc(dpy, d, gc, x + w - 2 * r - 1, y + h - 2 * r - 1, 2 * r, 2 * r, 270 * 64, 90 * 64);
        XDrawLine(dpy, d, gc, x + r, y, x + w - r - 1, y);
        XDrawLine(dpy, d, gc, x + r, y + h - 1, x + w - r - 1, y + h - 1);
        XDrawLine(dpy, d, gc, x, y + r, x, y + h - r - 1);
        XDrawLine(dpy, d, gc, x + w - 1, y + r, x + w - 1, y + h - r - 1);
    }
}

/* Round every floating window with a symmetric pixel-centre raster. XFillArc
 * on a 1-bit Shape mask produces visibly uneven left/right stair steps at the
 * island's tiny 34-36 px size. Building one exact scanline per row keeps both
 * caps identical and is much closer to LVGL's rounded firmware island.
 * Without the shape extension the windows still work, just square. */
static void shape_round_corners(Window win, int w, int h, int r)
{
#ifdef KIOSK_HAVE_SHAPE
    static int shape_ok = -1;
    if (shape_ok < 0) {
        int ev, err;
        shape_ok = XShapeQueryExtension(dpy, &ev, &err) ? 1 : 0;
    }
    if (!shape_ok || w < 2 || h < 2) return;
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    XRectangle *rows = calloc((size_t)h, sizeof(*rows));
    if (!rows) return;
    const int diameter = r * 2;
    const int radius_sq = diameter * diameter;
    int count = 0;
    for (int y = 0; y < h; y++) {
        int dy = 0;
        if (y < r) dy = diameter - (y * 2 + 1);
        else if (y >= h - r) dy = (y * 2 + 1) - 2 * (h - r);

        int inset = 0;
        if (dy) {
            while (inset < r) {
                const int dx = diameter - (inset * 2 + 1);
                if (dx * dx + dy * dy <= radius_sq) break;
                inset++;
            }
        }
        const int row_w = w - inset * 2;
        if (row_w <= 0) continue;
        rows[count].x = (short)inset;
        rows[count].y = (short)y;
        rows[count].width = (unsigned short)row_w;
        rows[count].height = 1;
        count++;
    }
    XShapeCombineRectangles(dpy, win, ShapeBounding, 0, 0, rows, count,
                            ShapeSet, Unsorted);
    free(rows);
#else
    (void)win; (void)w; (void)h; (void)r;
#endif
}

/* ---------------------------------------------------------- island layout */

static int visible_window_count(void)
{
    return nwins > ISLAND_MAX_ICONS ? ISLAND_MAX_ICONS : nwins;
}

static int queue_content_width(void)
{
    const int count = visible_window_count();
    if (count <= 0) return ISLAND_ICON;
    int width = ISLAND_ICON + (count - 1) * ISLAND_STEP;
    if (nwins > count) width += 16; /* trailing overflow ellipsis */
    return width;
}

static int desired_island_width(void)
{
    int width = queue_content_width() + 12;
    if (width < PILL_H) width = PILL_H;
    /* HH:MM, divider and battery percentage. This is intentionally compact:
     * advanced mode is wider, but the page still remains the main surface. */
    if (island_advanced) width += 78;
    if (width > sw - 16) width = sw - 16;
    return width;
}

static void sync_island_geometry(void)
{
    static int shaped_w = -1;
    const int wanted = desired_island_width();
    if (wanted != island_w) {
        island_w = wanted;
        XMoveResizeWindow(dpy, bar, (sw - island_w) / 2, PILL_Y,
                          (unsigned)island_w, PILL_H);
    }
    if (shaped_w != island_w) {
        shape_round_corners(bar, island_w, PILL_H, PILL_H / 2);
        shaped_w = island_w;
    }
}

/* ---------------------------------------------------------- real app icons */

/* Size-specific XImages produced once from the embedded 48x48 RGBA minis
 * (kiosk_icons.h, generated from assets/icons/drawer -- only the apps that
 * can run inside Chromium). Every surface an icon lands on (pill, rail,
 * toast) is black, so alpha is resolved at scale time by blending over
 * black; XPutImage then needs no mask and no per-frame math. */
#define ICON_CACHE_MAX 96
static struct { int icon, size; XImage *img; } icon_ximages[ICON_CACHE_MAX];
static int nicon_ximages = 0;

static XImage *icon_ximage(int icon, int size)
{
    if (icon < 0 || size < 4) return NULL;
    for (int i = 0; i < nicon_ximages; i++)
        if (icon_ximages[i].icon == icon && icon_ximages[i].size == size)
            return icon_ximages[i].img;
    if (nicon_ximages >= ICON_CACHE_MAX) {
        /* The zoom churns through many transient sizes. Drop the whole cache
         * and rebuild on demand rather than returning NULL, which would flash
         * the letter-disc fallback mid-animation. */
        for (int i = 0; i < nicon_ximages; i++) XDestroyImage(icon_ximages[i].img);
        nicon_ximages = 0;
    }

    char *data = malloc((size_t)size * size * 4);
    if (!data) return NULL;
    const unsigned char *src = kiosk_icons[icon].rgba;
    /* Alpha-weighted box filter 48 -> size, composited over black. */
    for (int y = 0; y < size; y++) {
        int sy0 = y * KIOSK_ICON_PX / size, sy1 = (y + 1) * KIOSK_ICON_PX / size;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < size; x++) {
            int sx0 = x * KIOSK_ICON_PX / size, sx1 = (x + 1) * KIOSK_ICON_PX / size;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int sy = sy0; sy < sy1; sy++)
                for (int sx = sx0; sx < sx1; sx++) {
                    const unsigned char *p =
                        src + ((size_t)sy * KIOSK_ICON_PX + sx) * 4;
                    r += (unsigned)p[0] * p[3];
                    g += (unsigned)p[1] * p[3];
                    b += (unsigned)p[2] * p[3];
                    n++;
                }
            n *= 255;
            /* Client and the tegra X server share the little-endian 24-bit
             * TrueColor layout, so the pixel is the plain 0xRRGGBB triple. */
            ((uint32_t *)data)[(size_t)y * size + x] =
                ((r / n) << 16) | ((g / n) << 8) | (b / n);
        }
    }
    XImage *img = XCreateImage(dpy, DefaultVisual(dpy, DefaultScreen(dpy)), 24,
                               ZPixmap, 0, data, (unsigned)size, (unsigned)size,
                               32, 0);
    if (!img) { free(data); return NULL; }
    icon_ximages[nicon_ximages].icon = icon;
    icon_ximages[nicon_ximages].size = size;
    icon_ximages[nicon_ximages].img = img;
    nicon_ximages++;
    return img;
}

static int contains_ci(const char *haystack, const char *needle)
{
    if (!haystack || !needle || !*needle) return 0;
    size_t n = strlen(needle);
    for (const char *h = haystack; *h; h++) {
        size_t i = 0;
        while (i < n && h[i] &&
               tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return 1;
    }
    return 0;
}

static int guess_app(const char *text)
{
    if (!text || !*text) return -1;
    for (int i = 0; i < napps; i++)
        if (contains_ci(text, apps[i].name)) return i;
    if (contains_ci(text, "facebook"))
        for (int i = 0; i < napps; i++) if (contains_ci(apps[i].name, "facebook")) return i;
    if (contains_ci(text, "messenger"))
        for (int i = 0; i < napps; i++) if (contains_ci(apps[i].name, "messenger")) return i;
    return -1;
}

static unsigned long app_color(int app)
{
    const char *n = (app >= 0 && app < napps) ? apps[app].name : "Chromium";
    if (contains_ci(n, "facebook")) return 0x1877f2;
    if (contains_ci(n, "messenger")) return 0x0084ff;
    if (contains_ci(n, "youtube")) return 0xff0033;
    if (contains_ci(n, "github")) return 0x24292f;
    if (contains_ci(n, "gmail")) return 0xea4335;
    if (contains_ci(n, "teams")) return 0x6264a7;
    if (contains_ci(n, "chatgpt")) return 0x10a37f;
    if (contains_ci(n, "studio")) return 0x7c3aed;
    return 0x4285f4;
}

/* Map a launcher entry onto one of the embedded mini icons: first by name
 * ("YouTube" -> youtube), then by URL for entries with free-form names such
 * as rented RunPod pods. -1 = no artwork, fall back to the letter disc. */
static int app_icon[MAX_APPS];

static int icon_for_entry(const char *name, const char *url)
{
    const int count = (int)(sizeof(kiosk_icons) / sizeof(kiosk_icons[0]));
    for (int i = 0; i < count; i++)
        if (contains_ci(name, kiosk_icons[i].name)) return i;
    static const struct { const char *frag; const char *icon; } by_url[] = {
        {"youtube.com", "youtube"},   {"github.com", "github"},
        {"messenger.com", "messenger"}, {"facebook.com", "facebook"},
        {"teams.microsoft", "teams"}, {"chatgpt.com", "chatgpt"},
        {"chat.openai.com", "chatgpt"}, {"runpod", "pods"},
        {"mail.google.com", "gmail"},
    };
    for (size_t u = 0; u < sizeof(by_url) / sizeof(by_url[0]); u++)
        if (contains_ci(url, by_url[u].frag))
            for (int i = 0; i < count; i++)
                if (strcmp(kiosk_icons[i].name, by_url[u].icon) == 0) return i;
    return -1;
}

static int icon_index(int app)
{
    if (app >= 0 && app < napps) return app_icon[app];
    return icon_for_entry("Chromium", ""); /* untagged window */
}

static void draw_app_icon(Drawable d, int app, int cx, int cy, int size)
{
    XImage *img = icon_ximage(icon_index(app), size);
    if (img) {
        XPutImage(dpy, d, gc, img, 0, 0, cx - size / 2, cy - size / 2,
                  (unsigned)size, (unsigned)size);
        return;
    }

    /* Letter disc fallback for entries without embedded artwork (unrecognised
     * pods and any free-form launcher entry). */
    const char *n = (app >= 0 && app < napps) ? apps[app].name : "Chromium";
    int x = cx - size / 2, y = cy - size / 2;
    XSetForeground(dpy, gc, px(app_color(app)));
    XFillArc(dpy, d, gc, x, y, (unsigned)size, (unsigned)size, 0, 360 * 64);
    XSetForeground(dpy, gc, px(0xffffff));
    if (contains_ci(n, "messenger")) {
        XDrawLine(dpy, d, gc, x + size / 5, y + size * 3 / 5,
                  x + size * 2 / 5, y + size * 2 / 5);
        XDrawLine(dpy, d, gc, x + size * 2 / 5, y + size * 2 / 5,
                  x + size * 3 / 5, y + size / 2);
        XDrawLine(dpy, d, gc, x + size * 3 / 5, y + size / 2,
                  x + size * 4 / 5, y + size / 3);
    } else if (contains_ci(n, "youtube")) {
        XPoint tri[3] = {{(short)(cx - 2), (short)(cy - 5)},
                         {(short)(cx - 2), (short)(cy + 5)},
                         {(short)(cx + 6), (short)cy}};
        XFillPolygon(dpy, d, gc, tri, 3, Convex, CoordModeOrigin);
    } else {
        char glyph[2] = {(char)toupper((unsigned char)n[0]), 0};
        if (contains_ci(n, "facebook")) glyph[0] = 'f';
        int tw = XTextWidth(font_big, glyph, 1);
        draw_string(d, font_big, cx - tw / 2,
                    cy + (font_big->ascent - font_big->descent) / 2, glyph);
    }
    XSetForeground(dpy, gc, px(0xffffff));
    XDrawArc(dpy, d, gc, x, y, (unsigned)(size - 1), (unsigned)(size - 1), 0, 360 * 64);
}

static int effective_app(int i)
{
    if (i < 0 || i >= nwins) return -1;
    if (win_app[i] >= 0) return win_app[i];
    return guess_app(win_title[i]);
}

static int queue_x_for(int index, int count)
{
    int total = ISLAND_ICON + (count - 1) * ISLAND_STEP;
    if (nwins > count) total += 16;
    int area_w = queue_content_width();
    int left = island_advanced ? 6 : (island_w - area_w) / 2;
    return left + (area_w - total) / 2 + ISLAND_ICON / 2 +
           index * ISLAND_STEP;
}

static void draw_queue(Drawable d)
{
    int count = visible_window_count();
    if (count <= 0) {
        /* Sensor/camera hint while Chromium is still starting. */
        XSetForeground(dpy, gc, px(0x202833));
        int cx = island_advanced ? 6 + queue_content_width() / 2 : island_w / 2;
        XFillArc(dpy, d, gc, cx - 5, PILL_H / 2 - 5, 10, 10, 0, 360 * 64);
        return;
    }
    long elapsed = queue_anim_start ? now_ms() - queue_anim_start : 999;
    double t = elapsed >= 260 ? 1.0 : (double)elapsed / 260.0;
    double ease = 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);
    if (t >= 1.0) queue_anim_start = 0;

    /* Draw back-to-front: icon 0 is the selected app and overlaps 20% of the
     * following icon, exactly like a small notification/avatar stack. */
    for (int i = count - 1; i >= 0; i--) {
        int old_i = i;
        if (queue_anim_start) {
            old_i = -1;
            for (int j = 0; j < queue_old_n; j++) if (queue_old[j] == wins[i]) old_i = j;
            if (old_i < 0 || old_i >= count) old_i = count - 1;
        }
        int from_x = queue_x_for(old_i, count);
        int to_x = queue_x_for(i, count);
        int x = from_x + (int)((to_x - from_x) * ease);
        int lift = (i == 0 && t < 1.0) ? (int)(18.0 * t * (1.0 - t)) : 0;
        draw_app_icon(d, effective_app(i), x, PILL_H / 2 - lift, ISLAND_ICON);
    }
    if (nwins > count) {
        XSetForeground(dpy, gc, px(0xffffff));
        int x = queue_x_for(count - 1, count) + 14;
        for (int i = 0; i < 3; i++) XFillArc(dpy, d, gc, x + i * 4, 20, 2, 2, 0, 360 * 64);
    }
}

static void redraw(int bat_level, int bat_charging)
{
    sync_island_geometry();
    Drawable d = bar_buffer != None ? bar_buffer : bar;
    /* Match the firmware's subtle black -> navy vertical surface. The outer
     * silhouette comes exclusively from the Shape mask: no XDrawArc outline,
     * which was the jagged bright ring visible on the physical panel. */
    for (int y = 0; y < PILL_H; y++) {
        const unsigned red = 0x0bU * (unsigned)y / (PILL_H - 1);
        const unsigned green = 0x12U * (unsigned)y / (PILL_H - 1);
        const unsigned blue = 0x20U * (unsigned)y / (PILL_H - 1);
        XSetForeground(dpy, gc, px((red << 16) | (green << 8) | blue));
        XFillRectangle(dpy, d, gc, 0, y, (unsigned)island_w, 1);
    }
    draw_queue(d);

    if (island_advanced) {
        const int divider_x = 6 + queue_content_width() + 5;
        XSetForeground(dpy, gc, px(COL_PILL_EDGE));
        XDrawLine(dpy, d, gc, divider_x, 9, divider_x, PILL_H - 9);

        char clock_text[16], h24[8], battery_text[16];
        setting_get("display", "clock_24h", "1", h24, sizeof(h24));
        time_t now = time(NULL);
        struct tm t;
        localtime_r(&now, &t);
        strftime(clock_text, sizeof(clock_text),
                 strcmp(h24, "0") == 0 ? "%I:%M" : "%H:%M", &t);
        snprintf(battery_text, sizeof(battery_text), "%d%%", bat_level);
        const int baseline = (PILL_H + font_small->ascent - font_small->descent) / 2;
        const int time_x = divider_x + 8;
        XSetForeground(dpy, gc, px(COL_TEXT));
        draw_string(d, font_small, time_x, baseline, clock_text);
        int battery_x = island_w - 7 - XTextWidth(font_small, battery_text,
                                                  (int)strlen(battery_text));
        unsigned long battery_color = bat_level <= 20 ? COL_BAT_RED :
                                      bat_level <= 40 ? COL_BAT_YELLO : COL_BAT_GREEN;
        if (bat_charging) battery_color = COL_BAT_GREEN;
        XSetForeground(dpy, gc, px(battery_color));
        draw_string(d, font_small, battery_x, baseline, battery_text);
    }

    /* One copy makes the island appear atomically. This removes the
     * visible clear/draw flicker from the original once-per-second repaint. */
    if (bar_buffer != None)
        XCopyArea(dpy, bar_buffer, bar, gc, 0, 0,
                  (unsigned)island_w, PILL_H, 0, 0);
    XFlush(dpy);
}

/* ---------------------------------------------------------------- micro-WM */

static int win_index(Window w)
{
    for (int i = 0; i < nwins; i++)
        if (wins[i] == w) return i;
    return -1;
}

static void remember_queue(void)
{
    queue_old_n = nwins;
    memcpy(queue_old, wins, (size_t)nwins * sizeof(Window));
}

static void promote_index(int i, int animate)
{
    if (i < 0 || i >= nwins) return;
    if (animate) remember_queue();
    if (i > 0) {
        Window w = wins[i];
        int app = win_app[i], badge = win_badge[i];
        char title[128];
        memcpy(title, win_title[i], sizeof(title));
        memmove(&wins[1], &wins[0], (size_t)i * sizeof(Window));
        memmove(&win_app[1], &win_app[0], (size_t)i * sizeof(int));
        memmove(&win_badge[1], &win_badge[0], (size_t)i * sizeof(int));
        memmove(&win_title[1], &win_title[0], (size_t)i * sizeof(win_title[0]));
        wins[0] = w;
        win_app[0] = app;
        win_badge[0] = badge;
        memcpy(win_title[0], title, sizeof(title));
    }
    active = nwins ? 0 : -1;
    if (animate) queue_anim_start = now_ms();
}

static void rotate_next(void)
{
    if (nwins < 2) return;
    remember_queue();
    Window w = wins[0];
    int app = win_app[0], badge = win_badge[0];
    char title[128];
    memcpy(title, win_title[0], sizeof(title));
    memmove(&wins[0], &wins[1], (size_t)(nwins - 1) * sizeof(Window));
    memmove(&win_app[0], &win_app[1], (size_t)(nwins - 1) * sizeof(int));
    memmove(&win_badge[0], &win_badge[1], (size_t)(nwins - 1) * sizeof(int));
    memmove(&win_title[0], &win_title[1],
            (size_t)(nwins - 1) * sizeof(win_title[0]));
    wins[nwins - 1] = w;
    win_app[nwins - 1] = app;
    win_badge[nwins - 1] = badge;
    memcpy(win_title[nwins - 1], title, sizeof(title));
    active = 0;
    queue_anim_start = now_ms();
}

static void focus_active(void)
{
    if (active < 0) return;
    XRaiseWindow(dpy, wins[active]);
    XSetInputFocus(dpy, wins[active], RevertToPointerRoot, CurrentTime);
    if (onscreen_keyboard != None) XRaiseWindow(dpy, onscreen_keyboard);
    XRaiseWindow(dpy, bar);
    if (transition != None && transition_start) XRaiseWindow(dpy, transition);
    if (menu_open) XRaiseWindow(dpy, menu);
    if (toast != None && toast_start) XRaiseWindow(dpy, toast);
}

static void remove_win(Window w)
{
    if (w == onscreen_keyboard) {
        onscreen_keyboard = None;
        return;
    }
    int i = win_index(w);
    if (i < 0) return;
    remember_queue();
    memmove(&wins[i], &wins[i + 1], (size_t)(nwins - 1 - i) * sizeof(Window));
    memmove(&win_app[i], &win_app[i + 1], (size_t)(nwins - 1 - i) * sizeof(int));
    memmove(&win_badge[i], &win_badge[i + 1], (size_t)(nwins - 1 - i) * sizeof(int));
    memmove(&win_title[i], &win_title[i + 1],
            (size_t)(nwins - 1 - i) * sizeof(win_title[0]));
    nwins--;
    active = nwins ? 0 : -1;
    queue_anim_start = now_ms();
    if (active >= 0) focus_active();
}

static int is_onscreen_keyboard(Window w)
{
    XClassHint hint = {0};
    if (!XGetClassHint(dpy, w, &hint)) return 0;
    int result = (hint.res_name && contains_ci(hint.res_name, "onboard")) ||
                 (hint.res_class && contains_ci(hint.res_class, "onboard"));
    if (hint.res_name) XFree(hint.res_name);
    if (hint.res_class) XFree(hint.res_class);
    return result;
}

static void manage(Window w, int give_focus)
{
    if (w == bar || w == root || w == menu || w == transition || w == toast) return;
    XWindowAttributes a;
    if (!XGetWindowAttributes(dpy, w, &a)) return;
    if (give_focus && a.map_state == IsViewable && a.width < sw * 3 / 5)
        inspect_notification_window(w);
    if (a.override_redirect || a.map_state != IsViewable) return;

    /* The portal's Onboard keyboard deliberately spans the panel width, so a
     * size-only micro-WM would mistake it for Chromium and stretch it over the
     * whole screen. Classify it first, pin it to the lower 205 px, and leave X
     * input focus on Chromium so its active text field receives injected keys. */
    if (is_onscreen_keyboard(w)) {
        onscreen_keyboard = w;
        if (a.x != 0 || a.y != sh - OSK_H || a.width != sw || a.height != OSK_H)
            XMoveResizeWindow(dpy, w, 0, sh - OSK_H,
                              (unsigned)sw, (unsigned)OSK_H);
        XRaiseWindow(dpy, w);
        XRaiseWindow(dpy, bar);
        if (transition != None && transition_start) XRaiseWindow(dpy, transition);
        if (menu_open) XRaiseWindow(dpy, menu);
        if (toast != None && toast_start) XRaiseWindow(dpy, toast);
        return;
    }

    /* Anything big enough to be a browser window fills the panel behind the
     * floating island and is tracked in the switcher list. Menus/tooltips are
     * override-redirect and never reach this point; small normal windows
     * (e.g. an OAuth popup) still get focus below but are not cycled. */
    if (a.width >= sw * 3 / 5) {
        if (a.x != 0 || a.y != 0 || a.width != sw || a.height != sh)
            XMoveResizeWindow(dpy, w, 0, 0, (unsigned)sw, (unsigned)sh);
        int i = win_index(w);
        if (i < 0 && nwins < MAX_WINS) {
            i = nwins++;
            wins[i] = w;
            /* Windows map in spawn order, so the oldest pending launcher
             * entry is the one this window belongs to. */
            win_app[i] = -1;
            win_badge[i] = 0;
            win_title[i][0] = 0;
            if (npending > 0) {
                win_app[i] = pending_app[0];
                npending--;
                memmove(pending_app, &pending_app[1],
                        (size_t)npending * sizeof(int));
            } else if (i == 0 && napps > 0) {
                /* Entry 0 is the session's starting Chromium/Home app. */
                win_app[i] = 0;
            }
            fetch_window_title(w, win_title[i], sizeof(win_title[i]));
            XSelectInput(dpy, w, PropertyChangeMask | StructureNotifyMask);
        }
        if (give_focus && i >= 0) promote_index(i, nwins > 1);
    }

    /* The whole reason the kiosk keyboard was dead: with no WM nobody ever
     * assigns X input focus, and Chromium drops key events while unfocused.
     * Focus is granted on map only -- re-focusing on every ConfigureNotify
     * would steal it back from popups (e.g. an OAuth sign-in window). */
    if (give_focus) {
        XSetInputFocus(dpy, w, RevertToPointerRoot, CurrentTime);
        if (active >= 0) XRaiseWindow(dpy, wins[active]);
    }
    if (onscreen_keyboard != None) XRaiseWindow(dpy, onscreen_keyboard);
    XRaiseWindow(dpy, bar);
    if (menu_open) XRaiseWindow(dpy, menu);
}

/* ------------------------------------------------------------ app launcher */

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void load_apps(void)
{
    napps = 0;
    const char *path = getenv("JETSON_KIOSK_APPS_FILE");
    if (!path || !*path) path = "/tmp/jetson_kiosk_apps";
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[600];
    while (napps < MAX_APPS && fgets(line, sizeof(line), f)) {
        char *sep = strchr(line, '|');
        if (!sep) continue;
        *sep = 0;
        char *url = sep + 1;
        url[strcspn(url, "\r\n")] = 0;
        if (!*url) continue;
        size_t o = 0;
        for (const char *p = line; *p && o + 1 < sizeof(apps[0].name); p++)
            if (*p >= 0x20 && *p < 0x7f) apps[napps].name[o++] = *p;
        apps[napps].name[o] = 0;
        if (o == 0) continue;
        snprintf(apps[napps].url, sizeof(apps[napps].url), "%s", url);
        app_icon[napps] = icon_for_entry(apps[napps].name, apps[napps].url);
        napps++;
    }
    fclose(f);
}

static void drop_chromium_privileges(void)
{
    const char *uid_s = getenv("CHROMIUM_RUN_UID");
    const char *gid_s = getenv("CHROMIUM_RUN_GID");
    const char *user = getenv("CHROMIUM_RUN_USER");
    if (geteuid() != 0 || !uid_s || !*uid_s || !gid_s || !*gid_s) return;
    uid_t uid = (uid_t)strtoul(uid_s, NULL, 10);
    gid_t gid = (gid_t)strtoul(gid_s, NULL, 10);
    if (user && *user) (void)initgroups(user, gid);
    if (setgid(gid) != 0 || setuid(uid) != 0) _exit(126);
}

static void open_app(int i)
{
    if (i < 0 || i >= napps) return;
    /* Already open? Focus it instead of spawning a duplicate renderer --
     * every extra window costs real RAM on the Jetson. */
    for (int j = 0; j < nwins; j++)
        if (win_app[j] == i) {
            if (j > 0) start_switch(wins[j]);
            return;
        }
    const char *bin = getenv("CHROMIUM_BIN");
    if (!bin || !*bin) {
        fprintf(stderr, "kiosk_bar: CHROMIUM_BIN not set, cannot launch %s\n",
                apps[i].name);
        return;
    }
    const char *prof = getenv("CHROMIUM_PROFILE_DIR");
    char profflag[600], appflag[600];
    snprintf(profflag, sizeof(profflag), "--user-data-dir=%s",
             prof && *prof ? prof : "/tmp/chromium-kiosk-profile");
    snprintf(appflag, sizeof(appflag), "--app=%s", apps[i].url);
    /* The relaunch finds the running Chromium through the profile singleton,
     * asks it to open one more --app window and exits immediately (SIGCHLD is
     * ignored, so no zombie). */
    pid_t pid = fork();
    if (pid == 0) {
        drop_chromium_privileges();
        signal(SIGCHLD, SIG_DFL);
        /* The bar lives outside Chromium's private D-Bus session. Give each
         * short profile-singleton relaunch its own valid session address too;
         * otherwise it emits the same bus.cc "Unknown address type" warning
         * before forwarding the new app window to the main browser. */
        execlp("dbus-run-session", "dbus-run-session", "--", bin,
               profflag, appflag, (char *)NULL);
        /* Older installations without the dbus package still retain the
         * previous, functional relaunch behaviour. */
        execlp(bin, bin, profflag, appflag, (char *)NULL);
        _exit(127);
    }
    if (pid > 0 && npending < (int)(sizeof(pending_app) / sizeof(int)))
        pending_app[npending++] = i;
}

/* The red rail-terminating power button: the only way out of the kiosk. */
static void draw_power_icon(Drawable d, int cx, int cy, int size)
{
    int x = cx - size / 2, y = cy - size / 2;
    XSetForeground(dpy, gc, px(0xff3b30));
    XFillArc(dpy, d, gc, x, y, (unsigned)size, (unsigned)size, 0, 360 * 64);
    XSetForeground(dpy, gc, px(0xffffff));
    XSetLineAttributes(dpy, gc, 2, LineSolid, CapRound, JoinRound);
    int r = size * 9 / 42;
    if (r < 5) r = 5;
    /* Broken ring with the stem through the top gap. */
    XDrawArc(dpy, d, gc, cx - r, cy - r + 2, 2 * r, 2 * r, 125 * 64, 290 * 64);
    XDrawLine(dpy, d, gc, cx, cy - r - 3, cx, cy + 1);
    XSetLineAttributes(dpy, gc, 0, LineSolid, CapButt, JoinMiter);
}

/* Copy the finished back buffer onto the (possibly mid-animation, shorter)
 * menu window. XCopyArea clips at the window edge, so the drop animation
 * reveals a fully drawn rail instead of re-rendering icons every frame. */
static void menu_present(void)
{
    if (menu == None || menu_buffer == None) return;
    XCopyArea(dpy, menu_buffer, menu, gc, 0, 0, (unsigned)menu_w, MENU_H, 0, 0);
    XFlush(dpy);
}

static void draw_menu(void)
{
    if (!menu_open || menu_buffer == None) return;
    Drawable d = menu_buffer;
    XSetForeground(dpy, gc, px(COL_BAR_BG));
    XFillRectangle(dpy, d, gc, 0, 0, (unsigned)menu_w, MENU_H);
    /* A 2px hairline so the rail reads as a floating panel over the web page,
     * not a stray black rectangle. COL_PILL_EDGE was too dark on black to see;
     * inset by 1px so the stroke is not clipped by the rounded shape mask. */
    XSetForeground(dpy, gc, px(COL_MENU_EDGE));
    XSetLineAttributes(dpy, gc, 2, LineSolid, CapRound, JoinRound);
    fill_round_rect(d, 1, 1, menu_w - 2, MENU_H - 2, MENU_RADIUS - 1, 0);
    XSetLineAttributes(dpy, gc, 0, LineSolid, CapButt, JoinMiter);
    int total = menu_total();

    /* One shared progress drives both the grow and the shrink, so the pair
     * always crosses over cleanly. */
    double p = 1.0;
    if (menu_hover_anim) {
        p = (double)(now_ms() - menu_hover_anim) / (double)MENU_HOVER_MS;
        if (p >= 1.0) {
            p = 1.0;
            menu_hover_anim = 0;
            menu_hover_prev = -1;
        }
    }
    double ease = 1.0 - (1.0 - p) * (1.0 - p);   /* easeOutQuad */
    const int grow = MENU_ICON_ZOOM - MENU_ICON;
    int size_up = MENU_ICON + (int)(grow * ease + 0.5);
    int size_down = MENU_ICON_ZOOM - (int)(grow * ease + 0.5);
    /* Quantise to even pixels: icon_ximage caches one scaled XImage per size,
     * and a continuous sweep would evict the whole cache every frame. */
    size_up &= ~1;
    size_down &= ~1;

    /* Two passes so the lifted icon overlaps its neighbours instead of being
     * painted over by the next slot. */
    for (int pass = 0; pass < 2; pass++) {
        for (int slot = 0; slot < menu_visible; slot++) {
            int i = menu_scroll + slot;
            if (i >= total) break;
            int lifted = (i == menu_hover || i == menu_hover_prev);
            if (lifted != pass) continue;
            int size = i == menu_hover ? size_up :
                       (i == menu_hover_prev ? size_down : MENU_ICON);
            int cx = MENU_PAD + MENU_ICON / 2 + slot * MENU_STEP;
            if (i == napps) draw_power_icon(d, cx, MENU_H / 2, size);
            else draw_app_icon(d, i, cx, MENU_H / 2, size);
        }
    }
    menu_present();
}

static void close_menu(void)
{
    if (!menu_open) return;
    menu_open = 0;
    if (menu != None) XUnmapWindow(dpy, menu);
    XFlush(dpy);
}

static void open_menu(void)
{
    load_apps(); /* fresh every open: picks up newly rented pods */
    /* Even with no readable apps file the rail still opens: the power slot
     * is the kiosk's only exit gesture. */
    int total = menu_total();
    menu_visible = (sw - 2 * MENU_PAD - 28) / MENU_STEP;
    if (menu_visible < 1) menu_visible = 1;
    if (menu_visible > total) menu_visible = total;
    menu_w = MENU_PAD * 2 + MENU_ICON + (menu_visible - 1) * MENU_STEP;
    menu_scroll = 0;
    if (menu == None) {
        XSetWindowAttributes ma;
        ma.override_redirect = True;
        ma.background_pixel = px(COL_BAR_BG);
        ma.event_mask = ExposureMask | ButtonPressMask | PointerMotionMask |
                        LeaveWindowMask;
        menu = XCreateWindow(dpy, root, 0, 0, 1, 1, 0, CopyFromParent,
                             InputOutput, CopyFromParent,
                             CWOverrideRedirect | CWBackPixel | CWEventMask,
                             &ma);
        XDefineCursor(dpy, menu, XCreateFontCursor(dpy, XC_left_ptr));
    }
    /* The back buffer width follows the entry count; rebuild per open. */
    if (menu_buffer != None) XFreePixmap(dpy, menu_buffer);
    menu_buffer = XCreatePixmap(dpy, menu, (unsigned)menu_w, MENU_H,
                                (unsigned)DefaultDepth(dpy, DefaultScreen(dpy)));
    XMoveResizeWindow(dpy, menu, (sw - menu_w) / 2, PILL_Y + PILL_H - 3,
                      (unsigned)menu_w, 1);
    XMapRaised(dpy, menu);
    menu_open = 1;
    menu_hover = -1;
    menu_hover_prev = -1;
    menu_hover_anim = 0;
    menu_ticks = 0;
    menu_anim_start = now_ms();
    draw_menu(); /* render once into the buffer; frames only copy from it */
}

/* Window title folded to the printable-ASCII subset supported by core X fonts. */
static void fetch_window_title(Window w, char *out, size_t cap)
{
    out[0] = 0;
    if (w == None) return;
    Atom type;
    int fmt;
    unsigned long n = 0, after;
    unsigned char *data = NULL;
    char *src = NULL, *fetched = NULL;
    if (XGetWindowProperty(dpy, w, atom_net_wm_name, 0, 64, False,
                           atom_utf8, &type, &fmt, &n, &after, &data) == Success &&
        data && n > 0) {
        src = (char *)data;
    } else {
        if (data) { XFree(data); data = NULL; }
        if (XFetchName(dpy, w, &fetched) && fetched) src = fetched;
    }
    if (src) {
        size_t o = 0;
        for (const char *p = src; *p && o + 1 < cap; p++)
            if (*p >= 0x20 && *p < 0x7f) out[o++] = *p;
        out[o] = 0;
    }
    if (data) XFree(data);
    if (fetched) XFree(fetched);
}

static int title_badge(const char *title)
{
    if (!title || title[0] != '(') return 0;
    int value = 0, digits = 0;
    for (const char *p = title + 1; *p && isdigit((unsigned char)*p); p++) {
        value = value * 10 + (*p - '0');
        digits++;
    }
    return digits ? value : 0;
}

static void draw_transition(void)
{
    if (transition == None || !transition_start || transition_buffer == None)
        return;
    int W = sw, H = sh;                      /* cover spans the full web area */
    Drawable d = transition_buffer;
    double s = transition_scale;             /* 1 = full panel, 0 = island */

    /* Black backdrop == the "screen closing" moment; the app card shrinks into
     * / grows out of the island against it. */
    XSetForeground(dpy, gc, px(COL_BAR_BG));
    XFillRectangle(dpy, d, gc, 0, 0, (unsigned)W, (unsigned)H);

    const int cw = island_w, ch = 6;         /* collapsed footprint = island */
    int w = cw + (int)((W - cw) * s);
    int h = ch + (int)((H - ch) * s);
    if (w < 2) w = 2;
    if (h < 2) h = 2;
    int x = (W - w) / 2;                      /* centred; top-anchored so it   */
    int y = 0;                                /* tucks up under the island     */
    int r = 6 + (int)(22.0 * s);              /* 28 full -> 6 collapsed        */
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;

    /* Elevated dark tile with a brand-coloured hairline + centred app icon. */
    XSetForeground(dpy, gc, px(0x1c1c1e));
    fill_round_rect(d, x, y, w, h, r, 1);
    XSetForeground(dpy, gc, px(app_color(transition_show_app)));
    XSetLineAttributes(dpy, gc, 2, LineSolid, CapRound, JoinRound);
    fill_round_rect(d, x, y, w, h, r, 0);
    XSetLineAttributes(dpy, gc, 0, LineSolid, CapButt, JoinMiter);

    int isz = ((w < h ? w : h) * 42 / 100) & ~7;  /* quantise to 8px steps */
    if (isz < 8) isz = 8;
    if (isz > 96) isz = 96;
    draw_app_icon(d, transition_show_app, x + w / 2, y + h / 2, isz);

    XCopyArea(dpy, transition_buffer, transition, gc, 0, 0,
              (unsigned)W, (unsigned)H, 0, 0);
}

static void start_switch(Window target)
{
    if (target == None || win_index(target) < 0) return;
    unsigned W = (unsigned)sw, H = (unsigned)sh;
    if (transition == None) {
        XSetWindowAttributes a;
        a.override_redirect = True;
        a.background_pixel = px(COL_BAR_BG);
        a.event_mask = ExposureMask;
        transition = XCreateWindow(dpy, root, 0, 0, W, H, 0,
                                   CopyFromParent, InputOutput, CopyFromParent,
                                   CWOverrideRedirect | CWBackPixel | CWEventMask, &a);
        transition_buffer = XCreatePixmap(dpy, transition, W, H,
                                          (unsigned)DefaultDepth(dpy, DefaultScreen(dpy)));
    }
    transition_target = target;
    transition_rotate = 0;
    transition_from_app = effective_app(0);   /* window currently in front */
    transition_show_app = transition_from_app;
    transition_scale = 1.0;
    transition_start = now_ms();
    transition_switched = 0;
    XMoveResizeWindow(dpy, transition, 0, 0, W, H);
    XMapRaised(dpy, transition);
}

static void start_cycle(void)
{
    if (nwins < 2) return;
    start_switch(wins[1]);
    transition_rotate = 1;
}

/* Continue the island zoom the firmware started. The firmware collapses the
 * app card *into* the island and leaves the panel black, then exits; we come
 * up and bloom the same card back *out* of the island. Across the process
 * swap it reads as one motion instead of two unrelated ones.
 *
 * The card then holds as a splash until Chromium maps a window: the browser
 * needs a second or two to start, and lifting the cover on schedule would
 * expose a bare black panel in the middle of the transition. */
static void start_handoff(void)
{
    const char *url = getenv("JETSON_KIOSK_HANDOFF_URL");
    unsigned W = (unsigned)sw, H = (unsigned)sh;
    if (transition == None) {
        XSetWindowAttributes a;
        a.override_redirect = True;
        a.background_pixel = px(COL_BAR_BG);
        a.event_mask = ExposureMask;
        transition = XCreateWindow(dpy, root, 0, 0, W, H, 0,
                                   CopyFromParent, InputOutput, CopyFromParent,
                                   CWOverrideRedirect | CWBackPixel | CWEventMask, &a);
        transition_buffer = XCreatePixmap(dpy, transition, W, H,
                                          (unsigned)DefaultDepth(dpy, DefaultScreen(dpy)));
    }
    /* Show the launcher entry we are opening; an unknown URL falls back to the
     * plain Chromium mark, which is what is starting anyway. */
    int app = -1;
    if (url && *url)
        for (int i = 0; i < napps; i++)
            if (strcmp(apps[i].url, url) == 0) { app = i; break; }
    transition_target = None;
    transition_rotate = 0;
    transition_switched = 1;      /* nothing to swap: no windows exist yet */
    transition_open_only = 1;
    transition_hold = 0;
    transition_ready = 0;
    transition_from_app = app;
    transition_show_app = app;
    transition_scale = 0.0;
    transition_start = now_ms();
    XMoveResizeWindow(dpy, transition, 0, 0, W, H);
    XMapRaised(dpy, transition);
}

static void update_transition(long now)
{
    if (!transition_start || transition == None) return;

    /* Hand-off splash: the bloom has finished, so the card just sits there
     * until the browser is really on screen (or gives up trying). */
    if (transition_hold) {
        if (nwins > 0 && !transition_ready) transition_ready = now;
        if ((transition_ready && now - transition_ready >= HANDOFF_SETTLE_MS) ||
            now - transition_hold >= HANDOFF_MAX_MS) {
            XUnmapWindow(dpy, transition);
            transition_start = 0;
            transition_hold = 0;
            transition_ready = 0;
            transition_open_only = 0;
            XFlush(dpy);
        }
        return;
    }

    /* The hand-off enters at the midpoint, so its bloom runs for TRANS_MS/2 --
     * the same length as the collapse the firmware just played. */
    double raw = (double)(now - transition_start) / (double)TRANS_MS;
    double t = transition_open_only ? 0.5 + raw : raw;
    if (t >= 1.0) {
        if (transition_open_only) {
            transition_scale = 1.0;
            transition_show_app = transition_from_app;
            transition_hold = now;       /* freeze the card as the splash */
            XRaiseWindow(dpy, transition);
            draw_transition();
            return;
        }
        XUnmapWindow(dpy, transition);   /* reveal the already-swapped app */
        transition_start = 0;
        transition_target = None;
        XFlush(dpy);
        return;
    }
    if (t < 0.0) t = 0.0;

    if (t < 0.5) {
        /* Close: ease-in -- the leaving app lingers, then snaps up into the
         * island like a screen powering off. */
        double p = t / 0.5;
        transition_scale = 1.0 - p * p;
        transition_show_app = transition_from_app;
    } else {
        /* Open: ease-out -- the arriving app bursts out of the island then
         * settles. The real window swap runs here, hidden by the opaque cover
         * at its smallest, so it is never seen. */
        double q = (t - 0.5) / 0.5;
        transition_scale = 1.0 - (1.0 - q) * (1.0 - q);
        if (!transition_switched) {
            int i = win_index(transition_target);
            if (i >= 0) {
                if (transition_rotate && i == 1) rotate_next();
                else promote_index(i, 1);
                focus_active();
            }
            transition_switched = 1;
        }
        /* On a hand-off there is no window to read the app from yet, so keep
         * showing the launcher entry the firmware handed us. */
        if (!transition_open_only) transition_show_app = effective_app(0);
    }
    XRaiseWindow(dpy, transition);
    draw_transition();
}

static void draw_toast(long now)
{
    if (toast == None || !toast_start || toast_buffer == None) return;
    XWindowAttributes a;
    if (!XGetWindowAttributes(dpy, toast, &a)) return;
    Drawable d = toast_buffer;
    XSetForeground(dpy, gc, px(COL_PILL_BG));
    XFillRectangle(dpy, d, gc, 0, 0, (unsigned)a.width, (unsigned)a.height);
    XSetForeground(dpy, gc, px(app_color(toast_app)));
    fill_round_rect(d, 0, 0, a.width, a.height, 22, 0);
    /* Ease-in/out pulse: the app icon floats a few pixels without flashing. */
    int phase = (int)((now - toast_start) % 900);
    int lift = phase < 450 ? phase * 4 / 450 : (900 - phase) * 4 / 450;
    draw_app_icon(d, toast_app, 34, a.height / 2 - lift, 38);
    const char *name = (toast_app >= 0 && toast_app < napps) ? apps[toast_app].name : "Chromium";
    XSetForeground(dpy, gc, px(app_color(toast_app)));
    draw_string(d, font_small, 64, 23, name);
    XSetForeground(dpy, gc, px(COL_TEXT));
    draw_string(d, font_big, 64, 46, toast_message);
    XCopyArea(dpy, toast_buffer, toast, gc, 0, 0,
              (unsigned)a.width, (unsigned)a.height, 0, 0);
}

static void show_notification(Window target, int app, const char *message)
{
    if (target == None && app >= 0) {
        for (int i = 0; i < nwins; i++) if (effective_app(i) == app) { target = wins[i]; break; }
    }
    if (toast == None) {
        XSetWindowAttributes a;
        a.override_redirect = True;
        a.background_pixel = px(COL_PILL_BG);
        a.event_mask = ExposureMask | ButtonPressMask;
        toast = XCreateWindow(dpy, root, 0, PILL_Y, 1, 1, 0, CopyFromParent,
                              InputOutput, CopyFromParent,
                              CWOverrideRedirect | CWBackPixel | CWEventMask, &a);
        XDefineCursor(dpy, toast, XCreateFontCursor(dpy, XC_left_ptr));
        toast_buffer = XCreatePixmap(dpy, toast, TOAST_MAX_W, TOAST_MAX_H,
                                     (unsigned)DefaultDepth(dpy, DefaultScreen(dpy)));
    }
    toast_target = target;
    toast_app = app;
    snprintf(toast_message, sizeof(toast_message), "%s",
             message && *message ? message : "Tin nhan moi");
    toast_start = now_ms();
    toast_until = toast_start + 5500;
    toast_base_w = island_w;
    XMoveResizeWindow(dpy, toast, (sw - toast_base_w) / 2, PILL_Y,
                      (unsigned)toast_base_w, PILL_H);
    XMapRaised(dpy, toast);
}

static void update_toast(long now)
{
    if (!toast_start || toast == None) return;
    if (now >= toast_until) {
        XUnmapWindow(dpy, toast);
        toast_start = 0;
        return;
    }
    double t = (double)(now - toast_start) / 330.0;
    if (t > 1.0) t = 1.0;
    if (t < 0.0) t = 0.0;
    double ease = t * t * (3.0 - 2.0 * t);
    int w = toast_base_w + (int)((TOAST_MAX_W - toast_base_w) * ease);
    int h = PILL_H + (int)((TOAST_MAX_H - PILL_H) * ease);
    XMoveResizeWindow(dpy, toast, (sw - w) / 2, PILL_Y, (unsigned)w, (unsigned)h);
    shape_round_corners(toast, w, h, 22);
    XRaiseWindow(dpy, toast);
    draw_toast(now);
}

static void update_window_title(Window w)
{
    int i = win_index(w);
    if (i < 0) return;
    char title[128];
    fetch_window_title(w, title, sizeof(title));
    if (!title[0] || strcmp(title, win_title[i]) == 0) return;
    int old_badge = win_badge[i];
    int badge = title_badge(title);
    snprintf(win_title[i], sizeof(win_title[i]), "%s", title);
    win_badge[i] = badge;
    int guessed = guess_app(title);
    if (win_app[i] < 0 || (win_app[i] == 0 && guessed > 0)) win_app[i] = guessed;
    if (badge > 0 && badge > old_badge)
        show_notification(w, effective_app(i), "Tin nhan moi");
}

static void inspect_notification_window(Window w)
{
    char title[128];
    fetch_window_title(w, title, sizeof(title));
    int app = guess_app(title);
    if (app < 0) return;
    if (!contains_ci(title, "message") && !contains_ci(title, "notification") &&
        !contains_ci(title, "messenger")) return;
    Window target = None;
    for (int i = 0; i < nwins; i++) if (effective_app(i) == app) { target = wins[i]; break; }
    show_notification(target, app, "Tin nhan moi");
}

static void update_menu_animation(long now)
{
    if (!menu_open || !menu_anim_start) return;
    double t = (double)(now - menu_anim_start) / 360.0;
    if (t >= 1.0) {
        t = 1.0;
        menu_anim_start = 0;
    }
    /* easeOutBack: a small overshoot gives the requested downward bounce. */
    double q = t - 1.0;
    double ease = 1.0 + 2.70158 * q * q * q + 1.70158 * q * q;
    int h = 1 + (int)((MENU_H - 1) * ease);
    if (h < 1) h = 1;
    if (h > MENU_H) h = MENU_H; /* overshoot would tear the rounded mask */
    XMoveResizeWindow(dpy, menu, (sw - menu_w) / 2, PILL_Y + PILL_H - 3,
                      (unsigned)menu_w, (unsigned)h);
    shape_round_corners(menu, menu_w, h, MENU_RADIUS);
    XRaiseWindow(dpy, menu);
    menu_present();
}

static void manage_existing(void)
{
    Window r, parent, *kids = NULL;
    unsigned int n = 0;
    if (!XQueryTree(dpy, root, &r, &parent, &kids, &n)) return;
    for (unsigned int i = 0; i < n; i++) manage(kids[i], 1);
    if (kids) XFree(kids);
}

/* ------------------------------------------------------------------- main */

static XFontStruct *load_font(const char *const *names)
{
    for (; *names; names++) {
        XFontStruct *f = XLoadQueryFont(dpy, *names);
        if (f) return f;
    }
    return NULL;
}

int main(void)
{
    /* Launcher relaunches (open_app) are fire-and-forget. */
    signal(SIGCHLD, SIG_IGN);

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "kiosk_bar: cannot open display\n");
        return 1;
    }
    XSetErrorHandler(ignore_xerror);

    int screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);
    sw = DisplayWidth(dpy, screen);
    sh = DisplayHeight(dpy, screen);
    atom_net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);
    atom_utf8 = XInternAtom(dpy, "UTF8_STRING", False);

    static const char *const big_names[] = {
        "-*-helvetica-bold-r-normal--14-*", "9x15bold", "9x15", "fixed", NULL};
    static const char *const small_names[] = {"6x13", "fixed", NULL};
    font_big = load_font(big_names);
    font_small = load_font(small_names);
    if (!font_big || !font_small) {
        fprintf(stderr, "kiosk_bar: no usable core font\n");
        return 1;
    }

    XSetWindowAttributes wa;
    wa.override_redirect = True; /* nothing may reparent or move the island */
    wa.background_pixel = px(COL_BAR_BG);
    wa.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask;
    {
        char mode[24];
        setting_get("chromium", "web_view_mode", "basic", mode, sizeof(mode));
        island_advanced = strcmp(mode, "advanced") == 0;
        island_w = desired_island_width();
    }
    bar = XCreateWindow(dpy, root, (sw - island_w) / 2, PILL_Y,
                        (unsigned)island_w, PILL_H, 0,
                        CopyFromParent, InputOutput, CopyFromParent,
                        CWOverrideRedirect | CWBackPixel | CWEventMask, &wa);
    XStoreName(dpy, bar, "jetson_kiosk_bar");
    XDefineCursor(dpy, bar, XCreateFontCursor(dpy, XC_left_ptr));
    gc = XCreateGC(dpy, bar, 0, NULL);
    bar_buffer = XCreatePixmap(dpy, bar, (unsigned)sw, PILL_H,
                               (unsigned)DefaultDepth(dpy, screen));
    sync_island_geometry();

    /* Watch the whole session: every map/configure of a sibling window. */
    XSelectInput(dpy, root, SubstructureNotifyMask);
    XMapRaised(dpy, bar);
    {
        char value[16];
        setting_get("display", "volume", "50", value, sizeof(value));
        session_volume = atoi(value);
        setting_get("display", "muted", "0", value, sizeof(value));
        session_muted = atoi(value) != 0 || contains_ci(value, "true");
        setting_get("display", "brightness", "100", value, sizeof(value));
        session_brightness = atoi(value);
        if (session_brightness < 20) session_brightness = 20;
        if (session_brightness > 100) session_brightness = 100;
        apply_system_volume();
        apply_x_brightness();
    }
    load_apps();
    manage_existing();
    /* Only when the firmware handed the panel over: a bar started on its own
     * has no collapse to continue, and covering the screen would be wrong. */
    if (getenv("JETSON_KIOSK_HANDOFF_URL")) start_handoff();

    int bat_level = 100, bat_charging = 0;
    if (island_advanced) battery_read(&bat_level, &bat_charging);

    long press_start = 0; /* monotonic ms of a pill ButtonPress, 0 = idle */
    int ticks = 0;
    long last_status = now_ms();
    long menu_opened = 0;
    struct pollfd pfd = {ConnectionNumber(dpy), POLLIN, 0};

    redraw(bat_level, bat_charging);
    for (;;) {
        int dirty = 0;
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            switch (ev.type) {
            case Expose:
                if (ev.xexpose.count != 0) break;
                if (ev.xexpose.window == menu) draw_menu();
                else if (ev.xexpose.window == transition) draw_transition();
                else if (ev.xexpose.window == toast) draw_toast(now_ms());
                else dirty = 1;
                break;
            case MapNotify:
                manage(ev.xmap.window, 1);
                dirty = 1;
                break;
            case ConfigureNotify:
                /* Chromium occasionally re-asserts full-screen geometry
                 * (e.g. after a page-triggered fullscreen); push it back and
                 * keep the strip on top. */
                if (ev.xconfigure.window != bar) manage(ev.xconfigure.window, 0);
                break;
            case UnmapNotify:
                remove_win(ev.xunmap.window);
                dirty = 1;
                break;
            case DestroyNotify:
                remove_win(ev.xdestroywindow.window);
                dirty = 1;
                break;
            case PropertyNotify:
                if (ev.xproperty.atom == atom_net_wm_name ||
                    ev.xproperty.atom == XA_WM_NAME) {
                    update_window_title(ev.xproperty.window);
                    dirty = 1;
                }
                break;
            case MotionNotify:
                if (ev.xmotion.window == menu) {
                    int slot = ev.xmotion.x < MENU_PAD ? -1 :
                               (ev.xmotion.x - MENU_PAD) / MENU_STEP;
                    int item = (slot >= 0 && slot < menu_visible) ? menu_scroll + slot : -1;
                    if (item < 0 || item >= menu_total()) item = -1;
                    if (item != menu_hover) {
                        menu_hover_prev = menu_hover;
                        menu_hover = item;
                        menu_hover_anim = now_ms();
                        draw_menu();
                    }
                }
                break;
            case LeaveNotify:
                if (ev.xcrossing.window == menu && menu_hover != -1) {
                    menu_hover_prev = menu_hover;
                    menu_hover = -1;
                    menu_hover_anim = now_ms();
                    draw_menu();
                }
                break;
            case ButtonPress:
                if (ev.xbutton.window == menu) {
                    if (ev.xbutton.button == Button4 && menu_scroll > 0) {
                        menu_scroll--;
                        draw_menu();
                        break;
                    }
                    if (ev.xbutton.button == Button5 &&
                        menu_scroll + menu_visible < menu_total()) {
                        menu_scroll++;
                        draw_menu();
                        break;
                    }
                    int slot = ev.xbutton.x < MENU_PAD ? -1 :
                               (ev.xbutton.x - MENU_PAD) / MENU_STEP;
                    int item = (slot >= 0 && slot < menu_visible) ? menu_scroll + slot : -1;
                    close_menu();
                    if (item == napps) {
                        /* The rail's trailing power button: leave the kiosk.
                         * The launcher script kills Chromium when we exit,
                         * which ends the X session and restarts the
                         * firmware. */
                        XCloseDisplay(dpy);
                        return 0;
                    }
                    open_app(item >= 0 && item < napps ? item : -1);
                    dirty = 1;
                } else if (ev.xbutton.window == toast) {
                    Window target = toast_target;
                    XUnmapWindow(dpy, toast);
                    toast_start = 0;
                    if (target != None) start_switch(target);
                } else if (ev.xbutton.window == bar) {
                    press_start = now_ms();
                }
                break;
            case ButtonRelease: {
                if (ev.xbutton.window != bar || press_start == 0) break;
                long held = now_ms() - press_start;
                press_start = 0;
                if (held >= LONGPRESS_MS) {
                    /* Long-press on the island: drop the app launcher. */
                    if (menu_open) close_menu(); else { open_menu(); menu_opened = now_ms(); }
                    break;
                }
                if (menu_open) {
                    close_menu(); /* short pill tap dismisses an open menu */
                    break;
                }
                /* Single click: cycle to the next window, app-switcher
                 * style. Exiting the kiosk moved to the rail's power button
                 * (long-press, last icon) -- the old double-click was one
                 * mistimed switcher click away from killing the session. */
                if (nwins > 1) {
                    /* Index 0 is active; the next icon blooms down from the
                     * island and is promoted at the transition midpoint. */
                    start_cycle();
                    dirty = 1;
                }
                break;
            }
            }
        }
        long frame_now = now_ms();
        update_menu_animation(frame_now);
        if (menu_hover_anim) draw_menu(); /* clears menu_hover_anim when done */
        update_transition(frame_now);
        update_toast(frame_now);
        if (queue_anim_start) dirty = 1;
        if (dirty) redraw(bat_level, bat_charging);
        /* Short poll while the pill is held so the launcher can drop at the
         * long-press threshold instead of waiting for the release. 16 ms
         * animation frames (~60 fps) keep the drop/switch/toast motion
         * smooth; everything is double-buffered so the extra frames only
         * cost XCopyArea calls, not full redraws. */
        /* A held hand-off splash is a still frame, not an animation: polling it
         * at 60 fps would spin the CPU for the seconds Chromium takes to load. */
        int animating = menu_anim_start || menu_hover_anim ||
                        (transition_start && !transition_hold) ||
                        toast_start || queue_anim_start;
        int timeout = press_start ? 30 : (animating ? 16 : 250);
        if (poll(&pfd, 1, timeout) == 0) {
            if (press_start) {
                if (now_ms() - press_start >= LONGPRESS_MS) {
                    press_start = 0;
                    if (menu_open) close_menu(); else { open_menu(); menu_opened = now_ms(); }
                }
            }
            frame_now = now_ms();
            if (frame_now - last_status >= 1000) {
                last_status = frame_now;
                if (island_advanced && ++ticks % 5 == 0) {
                    battery_read(&bat_level, &bat_charging);
                }
                if (menu_open && menu_opened &&
                    frame_now - menu_opened >= MENU_AUTOCLOSE_S * 1000L)
                    close_menu();
                redraw(bat_level, bat_charging);
            }
        }
    }
}
