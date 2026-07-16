/* kiosk_bar: Dynamic Island status bar + micro window manager for the
 * Chromium kiosk hand-off (scripts/launch_chromium.sh).
 *
 * The DS-02 firmware owns /dev/fb0 and exits with code 42 when the user opens
 * Chromium; the supervisor then starts a bare X session (xinit, no desktop, no
 * window manager). This program runs alongside Chromium inside that session
 * and provides the two things a WM-less kiosk is missing:
 *
 *  1. The top bar. A 42px strip that mirrors the firmware's StatusBar
 *     (src/display/widgets/status_bar.cc): clock + date on the left, Wi-Fi +
 *     battery on the right, and the black island pill in the center.
 *     Double-clicking the pill exits this process; launch_chromium.sh treats
 *     that as "leave the browser" and tears the session down, which returns
 *     the panel to the firmware.
 *
 *  2. Micro-WM duties. Without a window manager Chromium never receives X
 *     input focus, so the mouse works but every keystroke is ignored. On each
 *     MapNotify of a normal (non-override-redirect) window we assign it input
 *     focus, push any bar-overlapping full-screen window down below the strip,
 *     and re-raise the bar.
 *
 * Only libX11 is required (core protocol, core fonts); the Jetson already
 * ships it as an Xorg dependency. Build needs libx11-dev.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>

#include <dirent.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BAR_H       42
#define PILL_W      132
#define PILL_H      36
#define PILL_Y      3
#define PILL_HIT    8     /* extra click slack around the pill, like the
                           * firmware's lv_obj_set_ext_click_area(pill_, 6) */
#define DBLCLICK_MS 450

/* Palette lifted from the firmware status bar. */
#define COL_BAR_BG    0x0b0f14
#define COL_PILL_BG   0x000000
#define COL_PILL_EDGE 0x253142
#define COL_TEXT      0xffffff
#define COL_BAT_GREEN 0x34c759
#define COL_BAT_YELLO 0xffcc00
#define COL_BAT_RED   0xff3b30

static Display *dpy;
static Window bar, root;
static GC gc;
static int sw, sh;
static XFontStruct *font_big, *font_small;
static Atom atom_net_wm_name, atom_utf8;

/* Switcher list: browser-sized top-level windows. A single click on the
 * island pill cycles through them (the firmware island opens its app switcher
 * the same way); a double-click still exits the kiosk. */
#define MAX_WINS 16
static Window wins[MAX_WINS];
static int win_app[MAX_WINS]; /* launcher entry that opened wins[i], -1 */
static int nwins = 0, active = -1;

/* Long-press app launcher. Holding the pill drops a menu of "Name|URL"
 * entries read from a file, so launch_chromium.sh provides the static web
 * apps and the firmware's Pods view appends the user's running GPU pods.
 * Re-read on every open: entries never go stale and no RAM is held between
 * uses. Launch goes through the Chromium profile singleton (a short-lived
 * relaunch that hands the URL to the running browser), and windows are
 * tagged with their entry so re-picking an open app focuses it instead of
 * burning ~100 MB on a duplicate renderer. */
#define MAX_APPS     24
#define MENU_W       240
#define MENU_ROW     34
#define MENU_PAD     6
#define LONGPRESS_MS 600
#define MENU_AUTOCLOSE_S 8
static struct { char name[40]; char url[512]; } apps[MAX_APPS];
static int napps = 0;
static Window menu = None;
static int menu_open = 0, menu_hover = -1, menu_ticks = 0;
static int pending_app[8]; /* spawned entries awaiting their MapNotify */
static int npending = 0;

static void fetch_title(char *out, size_t cap); /* used by redraw below */

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

static int battery_read(int *level, int *charging)
{
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) return 0;
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
    return found;
}

static int wifi_up(void)
{
    DIR *d = opendir("/sys/class/net");
    if (!d) return 0;
    struct dirent *e;
    int up = 0;
    while (!up && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char path[512], buf[32];
        snprintf(path, sizeof(path), "/sys/class/net/%s/wireless", e->d_name);
        if (access(path, F_OK) != 0) continue;
        snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        buf[0] = 0;
        if (fgets(buf, sizeof(buf), f) && strncmp(buf, "up", 2) == 0) up = 1;
        fclose(f);
    }
    closedir(d);
    return up;
}

/* ---------------------------------------------------------------- drawing */

static void draw_string(Drawable d, XFontStruct *fs, int x, int baseline,
                        const char *s)
{
    XSetFont(dpy, gc, fs->fid);
    XDrawString(dpy, d, gc, x, baseline, s, (int)strlen(s));
}

static void fill_round_rect(int x, int y, int w, int h, int r, int fill)
{
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    if (fill) {
        XFillArc(dpy, bar, gc, x, y, 2 * r, 2 * r, 90 * 64, 90 * 64);
        XFillArc(dpy, bar, gc, x + w - 2 * r, y, 2 * r, 2 * r, 0, 90 * 64);
        XFillArc(dpy, bar, gc, x, y + h - 2 * r, 2 * r, 2 * r, 180 * 64, 90 * 64);
        XFillArc(dpy, bar, gc, x + w - 2 * r, y + h - 2 * r, 2 * r, 2 * r, 270 * 64, 90 * 64);
        XFillRectangle(dpy, bar, gc, x + r, y, w - 2 * r, h);
        XFillRectangle(dpy, bar, gc, x, y + r, w, h - 2 * r);
    } else {
        XDrawArc(dpy, bar, gc, x, y, 2 * r, 2 * r, 90 * 64, 90 * 64);
        XDrawArc(dpy, bar, gc, x + w - 2 * r - 1, y, 2 * r, 2 * r, 0, 90 * 64);
        XDrawArc(dpy, bar, gc, x, y + h - 2 * r - 1, 2 * r, 2 * r, 180 * 64, 90 * 64);
        XDrawArc(dpy, bar, gc, x + w - 2 * r - 1, y + h - 2 * r - 1, 2 * r, 2 * r, 270 * 64, 90 * 64);
        XDrawLine(dpy, bar, gc, x + r, y, x + w - r - 1, y);
        XDrawLine(dpy, bar, gc, x + r, y + h - 1, x + w - r - 1, y + h - 1);
        XDrawLine(dpy, bar, gc, x, y + r, x, y + h - r - 1);
        XDrawLine(dpy, bar, gc, x + w - 1, y + r, x + w - 1, y + h - r - 1);
    }
}

static void draw_wifi(int cx, int cy)
{
    /* Three upward arcs + a dot, same silhouette as LV_SYMBOL_WIFI. */
    int r;
    for (r = 4; r <= 12; r += 4)
        XDrawArc(dpy, bar, gc, cx - r, cy - r, 2 * r, 2 * r, 40 * 64, 100 * 64);
    XFillArc(dpy, bar, gc, cx - 2, cy - 2, 4, 4, 0, 360 * 64);
}

static void redraw(int bat_level, int bat_charging, int wifi)
{
    XSetForeground(dpy, gc, px(COL_BAR_BG));
    XFillRectangle(dpy, bar, gc, 0, 0, sw, BAR_H);

    /* Left cluster: HH:MM  DD/MM (firmware default region format). */
    char ts[32];
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    strftime(ts, sizeof(ts), "%H:%M  %d/%m", &t);
    XSetForeground(dpy, gc, px(COL_TEXT));
    draw_string(bar, font_big, 14,
                (BAR_H + font_big->ascent - font_big->descent) / 2, ts);

    /* Center: the resting island pill. Single click cycles the switcher,
     * double click exits. Shows the active window's title (and a position
     * indicator when several windows are open) so the user knows which app
     * a cycle click will leave. */
    int pillx = (sw - PILL_W) / 2;
    XSetForeground(dpy, gc, px(COL_PILL_BG));
    fill_round_rect(pillx, PILL_Y, PILL_W, PILL_H, PILL_H / 2, 1);
    XSetForeground(dpy, gc, px(COL_PILL_EDGE));
    fill_round_rect(pillx, PILL_Y, PILL_W, PILL_H, PILL_H / 2, 0);

    char label[128] = "", title[96] = "";
    fetch_title(title, sizeof(title));
    if (nwins > 1)
        snprintf(label, sizeof(label), "%d/%d %s", active + 1, nwins, title);
    else
        snprintf(label, sizeof(label), "%s", title);
    int len = (int)strlen(label);
    const int maxw = PILL_W - 26; /* stay inside the rounded ends */
    int trunc = 0;
    while (len > 0 && XTextWidth(font_small, label, len) > maxw) {
        len--;
        trunc = 1;
    }
    if (trunc && len > 2) { label[len - 2] = '.'; label[len - 1] = '.'; }
    if (len > 0) {
        label[len] = 0;
        int lw = XTextWidth(font_small, label, len);
        XSetForeground(dpy, gc, px(COL_TEXT));
        draw_string(bar, font_small, pillx + (PILL_W - lw) / 2,
                    PILL_Y + (PILL_H + font_small->ascent - font_small->descent) / 2,
                    label);
    }

    /* Right cluster: wifi + battery body/fill/nub/percent, mirroring the
     * firmware's battery widget geometry (47x20 body, 41x16 fill, 3x8 nub). */
    int bx = sw - 10 - 52;
    int by = (BAR_H - 20) / 2;
    unsigned long fill_col = (bat_charging || bat_level > 50) ? COL_BAT_GREEN
                             : (bat_level > 20 ? COL_BAT_YELLO : COL_BAT_RED);
    if (wifi) {
        XSetForeground(dpy, gc, px(COL_TEXT));
        draw_wifi(bx - 22, BAR_H / 2 + 5);
    }
    XSetForeground(dpy, gc, px(fill_col));
    fill_round_rect(bx, by, 47, 20, 6, 0);
    XFillRectangle(dpy, bar, gc, bx + 48, by + 6, 3, 8);
    int fw = 41 * bat_level / 100;
    if (fw > 0) {
        if (fw < 1) fw = 1;
        XFillRectangle(dpy, bar, gc, bx + 3, by + 2, fw, 16);
    }
    char pct[8];
    snprintf(pct, sizeof(pct), "%d", bat_level);
    int pw = XTextWidth(font_small, pct, (int)strlen(pct));
    XSetForeground(dpy, gc, px(COL_TEXT));
    draw_string(bar, font_small, bx + (47 - pw) / 2,
                by + (20 + font_small->ascent - font_small->descent) / 2, pct);

    XFlush(dpy);
}

/* ---------------------------------------------------------------- micro-WM */

static int win_index(Window w)
{
    for (int i = 0; i < nwins; i++)
        if (wins[i] == w) return i;
    return -1;
}

static void focus_active(void)
{
    if (active < 0) return;
    XRaiseWindow(dpy, wins[active]);
    XSetInputFocus(dpy, wins[active], RevertToPointerRoot, CurrentTime);
    XRaiseWindow(dpy, bar);
}

static void remove_win(Window w)
{
    int i = win_index(w);
    if (i < 0) return;
    memmove(&wins[i], &wins[i + 1], (size_t)(nwins - 1 - i) * sizeof(Window));
    memmove(&win_app[i], &win_app[i + 1], (size_t)(nwins - 1 - i) * sizeof(int));
    nwins--;
    if (active > i) {
        active--;
    } else if (active == i) {
        /* The window under the user vanished: fall back to the most recently
         * opened survivor so the panel never shows the bare root window. */
        if (active >= nwins) active = nwins - 1;
        focus_active();
    }
}

static void manage(Window w, int give_focus)
{
    if (w == bar || w == root) return;
    XWindowAttributes a;
    if (!XGetWindowAttributes(dpy, w, &a)) return;
    if (a.override_redirect || a.map_state != IsViewable) return;

    /* Anything big enough to be a browser window: kick it out from under the
     * bar and track it in the switcher list. Menus/tooltips are
     * override-redirect and never reach this point; small normal windows
     * (e.g. an OAuth popup) still get focus below but are not cycled. */
    if (a.width >= sw * 3 / 5) {
        if (a.y < BAR_H || a.height > sh - BAR_H)
            XMoveResizeWindow(dpy, w, 0, BAR_H, (unsigned)sw, (unsigned)(sh - BAR_H));
        int i = win_index(w);
        if (i < 0 && nwins < MAX_WINS) {
            i = nwins++;
            wins[i] = w;
            /* Windows map in spawn order, so the oldest pending launcher
             * entry is the one this window belongs to. */
            win_app[i] = -1;
            if (npending > 0) {
                win_app[i] = pending_app[0];
                npending--;
                memmove(pending_app, &pending_app[1],
                        (size_t)npending * sizeof(int));
            }
        }
        if (give_focus && i >= 0) active = i;
    }

    /* The whole reason the kiosk keyboard was dead: with no WM nobody ever
     * assigns X input focus, and Chromium drops key events while unfocused.
     * Focus is granted on map only -- re-focusing on every ConfigureNotify
     * would steal it back from popups (e.g. an OAuth sign-in window). */
    if (give_focus)
        XSetInputFocus(dpy, w, RevertToPointerRoot, CurrentTime);
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
        napps++;
    }
    fclose(f);
}

static void open_app(int i)
{
    if (i < 0 || i >= napps) return;
    /* Already open? Focus it instead of spawning a duplicate renderer --
     * every extra window costs real RAM on the Jetson. */
    for (int j = 0; j < nwins; j++)
        if (win_app[j] == i) {
            active = j;
            focus_active();
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
        execlp(bin, bin, "--no-sandbox", profflag, appflag, (char *)NULL);
        _exit(127);
    }
    if (pid > 0 && npending < (int)(sizeof(pending_app) / sizeof(int)))
        pending_app[npending++] = i;
}

static int menu_h(void) { return MENU_PAD * 2 + napps * MENU_ROW; }

static void draw_menu(void)
{
    if (!menu_open) return;
    XSetForeground(dpy, gc, px(COL_BAR_BG));
    XFillRectangle(dpy, menu, gc, 0, 0, MENU_W, (unsigned)menu_h());
    XSetForeground(dpy, gc, px(COL_PILL_EDGE));
    XDrawRectangle(dpy, menu, gc, 0, 0, MENU_W - 1, menu_h() - 1);
    for (int i = 0; i < napps; i++) {
        int y = MENU_PAD + i * MENU_ROW;
        if (i == menu_hover) {
            XSetForeground(dpy, gc, px(0x1f2937));
            XFillRectangle(dpy, menu, gc, 2, y, MENU_W - 4, MENU_ROW);
        }
        XSetForeground(dpy, gc, px(COL_TEXT));
        draw_string(menu, font_big, 16,
                    y + (MENU_ROW + font_big->ascent - font_big->descent) / 2,
                    apps[i].name);
    }
    XFlush(dpy);
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
    if (napps == 0) return;
    if (menu == None) {
        XSetWindowAttributes ma;
        ma.override_redirect = True;
        ma.background_pixel = px(COL_BAR_BG);
        ma.event_mask = ExposureMask | ButtonPressMask | PointerMotionMask |
                        LeaveWindowMask;
        menu = XCreateWindow(dpy, root, 0, 0, MENU_W, 1, 0, CopyFromParent,
                             InputOutput, CopyFromParent,
                             CWOverrideRedirect | CWBackPixel | CWEventMask,
                             &ma);
        XDefineCursor(dpy, menu, XCreateFontCursor(dpy, XC_left_ptr));
    }
    int h = menu_h();
    if (h > sh - BAR_H - 8) h = sh - BAR_H - 8; /* keep on the panel */
    XMoveResizeWindow(dpy, menu, (sw - MENU_W) / 2, BAR_H + 4, MENU_W,
                      (unsigned)h);
    XMapRaised(dpy, menu);
    menu_open = 1;
    menu_hover = -1;
    menu_ticks = 0;
}

/* Active window's title, folded to the printable-ASCII subset the core X
 * fonts can render, for the label inside the pill. */
static void fetch_title(char *out, size_t cap)
{
    out[0] = 0;
    if (active < 0) return;
    Atom type;
    int fmt;
    unsigned long n = 0, after;
    unsigned char *data = NULL;
    char *src = NULL, *fetched = NULL;
    if (XGetWindowProperty(dpy, wins[active], atom_net_wm_name, 0, 64, False,
                           atom_utf8, &type, &fmt, &n, &after, &data) == Success &&
        data && n > 0) {
        src = (char *)data;
    } else {
        if (data) { XFree(data); data = NULL; }
        if (XFetchName(dpy, wins[active], &fetched) && fetched) src = fetched;
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
    wa.override_redirect = True; /* nothing may reparent or move the strip */
    wa.background_pixel = px(COL_BAR_BG);
    wa.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask;
    bar = XCreateWindow(dpy, root, 0, 0, (unsigned)sw, BAR_H, 0,
                        CopyFromParent, InputOutput, CopyFromParent,
                        CWOverrideRedirect | CWBackPixel | CWEventMask, &wa);
    XStoreName(dpy, bar, "jetson_kiosk_bar");
    XDefineCursor(dpy, bar, XCreateFontCursor(dpy, XC_left_ptr));
    gc = XCreateGC(dpy, bar, 0, NULL);

    /* Watch the whole session: every map/configure of a sibling window. */
    XSelectInput(dpy, root, SubstructureNotifyMask);
    XMapRaised(dpy, bar);
    manage_existing();

    int bat_level = 100, bat_charging = 0, wifi = 0;
    battery_read(&bat_level, &bat_charging);
    wifi = wifi_up();

    Time last_click = 0;
    long press_start = 0; /* monotonic ms of a pill ButtonPress, 0 = idle */
    int ticks = 0;
    struct pollfd pfd = {ConnectionNumber(dpy), POLLIN, 0};

    redraw(bat_level, bat_charging, wifi);
    for (;;) {
        int dirty = 0;
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            switch (ev.type) {
            case Expose:
                if (ev.xexpose.count != 0) break;
                if (ev.xexpose.window == menu) draw_menu();
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
            case MotionNotify:
                if (ev.xmotion.window == menu) {
                    int h = (ev.xmotion.y - MENU_PAD) / MENU_ROW;
                    if (h < 0 || h >= napps) h = -1;
                    if (h != menu_hover) {
                        menu_hover = h;
                        draw_menu();
                    }
                }
                break;
            case LeaveNotify:
                if (ev.xcrossing.window == menu && menu_hover != -1) {
                    menu_hover = -1;
                    draw_menu();
                }
                break;
            case ButtonPress:
                if (ev.xbutton.window == menu) {
                    int h = (ev.xbutton.y - MENU_PAD) / MENU_ROW;
                    close_menu();
                    open_app(h >= 0 && h < napps ? h : -1);
                    dirty = 1;
                } else {
                    int pillx = (sw - PILL_W) / 2;
                    int x = ev.xbutton.x, y = ev.xbutton.y;
                    if (x >= pillx - PILL_HIT && x <= pillx + PILL_W + PILL_HIT &&
                        y >= PILL_Y - PILL_HIT && y <= PILL_Y + PILL_H + PILL_HIT)
                        press_start = now_ms();
                }
                break;
            case ButtonRelease: {
                if (ev.xbutton.window != bar || press_start == 0) break;
                long held = now_ms() - press_start;
                press_start = 0;
                if (held >= LONGPRESS_MS) {
                    /* Long-press on the island: drop the app launcher. */
                    if (menu_open) close_menu(); else open_menu();
                    break;
                }
                if (menu_open) {
                    close_menu(); /* short pill tap dismisses an open menu */
                    break;
                }
                if (ev.xbutton.time - last_click < DBLCLICK_MS) {
                    /* Double-click on the island: leave the kiosk. The
                     * launcher script kills Chromium when we exit, which
                     * ends the X session and restarts the firmware. */
                    XCloseDisplay(dpy);
                    return 0;
                }
                last_click = ev.xbutton.time;
                /* Single click: cycle to the next window, app-switcher
                 * style. (The first click of a double-click cycles once
                 * before the exit lands -- harmless, the session is torn
                 * down anyway.) */
                if (nwins > 1) {
                    active = (active + 1) % nwins;
                    focus_active();
                    dirty = 1;
                }
                break;
            }
            }
        }
        if (dirty) redraw(bat_level, bat_charging, wifi);
        /* Short poll while the pill is held so the launcher can drop at the
         * long-press threshold instead of waiting for the release. */
        if (poll(&pfd, 1, press_start ? 50 : 1000) == 0) {
            if (press_start) {
                if (now_ms() - press_start >= LONGPRESS_MS) {
                    press_start = 0;
                    if (menu_open) close_menu(); else open_menu();
                }
            } else {
                if (++ticks % 5 == 0) {
                    battery_read(&bat_level, &bat_charging);
                    wifi = wifi_up();
                }
                if (menu_open && ++menu_ticks >= MENU_AUTOCLOSE_S)
                    close_menu();
                redraw(bat_level, bat_charging, wifi);
            }
        }
    }
}
