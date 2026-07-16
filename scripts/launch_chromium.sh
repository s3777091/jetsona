#!/bin/bash
# Launch a Chromium kiosk on the HDMI panel, taking over /dev/fb0 from the
# DS-02 firmware. Invoked by scripts/jetson_fw_run.sh when the firmware exits
# with code 42 (the "Chromium" drawer tile). When Chromium exits, control
# returns to the supervisor, which restarts the firmware so it re-acquires
# /dev/fb0.
#
# Runtime prerequisite (install once on the Jetson):
#   sudo apt install -y xserver-xorg-video-all xserver-xorg-input-libinput \
#       x11-xkb-utils xinit chromium-browser libx11-dev
# (libx11-dev is a build dependency for tools/kiosk_bar, the Dynamic Island
# strip + micro-WM that runs on top of Chromium.)
# The HDMI LCD is driven by the stock tegra/nvidia X driver; this script just
# starts a bare X server (no desktop) with Chromium as its only client, so the
# default /etc/X11/xorg.conf is reused as-is.
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# xinit must start an executable as its first client, so it re-enters this
# script in client mode.  Keep Chromium out of the service's stale desktop
# environment and install an emergency way out of kiosk mode.  Killing the X
# server makes xinit terminate the client and lets jetson_fw_run.sh restart the
# firmware.
if [ "${1:-}" = "--x-client" ]; then
    shift
    unset DBUS_SESSION_BUS_ADDRESS
    export HOME="${CHROMIUM_KIOSK_HOME:-/tmp/chromium-kiosk-home}"
    export XDG_CONFIG_HOME="${CHROMIUM_KIOSK_CONFIG_HOME:-$HOME/.config}"
    export XDG_CACHE_HOME="${CHROMIUM_KIOSK_CACHE_HOME:-$HOME/.cache}"
    mkdir -p "$HOME" "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME"

    if command -v setxkbmap >/dev/null 2>&1; then
        # Ctrl+Alt+Backspace exits the bare-X session even though Chromium has
        # no close button in --kiosk mode.
        setxkbmap -option '' -option terminate:ctrl_alt_bksp >/dev/null 2>&1 ||
            echo "launch_chromium: failed to enable Ctrl+Alt+Backspace" >&2
    else
        echo "launch_chromium: setxkbmap missing; Ctrl+Alt+Backspace unavailable" >&2
    fi

    # With the kiosk bar available, run it next to Chromium. The bar draws the
    # Dynamic Island strip on top and acts as a micro-WM: it hands Chromium the
    # X input focus (without it the USB keyboard is dead in a WM-less session)
    # and exits when the island pill is double-clicked. Whichever of the two
    # dies first ends the session: bar exit -> user asked to leave the browser;
    # chromium exit -> nothing left to show. Either way xinit tears X down and
    # jetson_fw_run.sh restarts the firmware.
    if [ -n "${JETSON_KIOSK_BAR:-}" ] && [ -x "$JETSON_KIOSK_BAR" ]; then
        "$JETSON_KIOSK_BAR" &
        BAR_PID=$!
        "$@" &
        APP_PID=$!

        # Extra kiosk windows: every URL beyond the first (newline-separated
        # in CHROMIUM_EXTRA_URLS) becomes its own --app window in the already
        # running Chromium -- the relaunch hands the URL to the instance
        # holding the profile singleton and exits. The bar cycles between the
        # windows on island clicks. Wait for the main process to claim the
        # singleton (SingletonLock symlink in the profile) first, or the
        # relaunch races it and tries to start a second full browser; cold
        # Chromium start on the Jetson can take well over 10s.
        if [ -n "${CHROMIUM_EXTRA_URLS:-}" ] && [ -n "${CHROMIUM_BIN:-}" ]; then
            (
                PROFILE="${CHROMIUM_PROFILE_DIR:-/tmp/chromium-kiosk-profile}"
                tries=0
                while [ ! -L "$PROFILE/SingletonLock" ] && [ "$tries" -lt 30 ]; do
                    sleep 1
                    tries=$((tries + 1))
                done
                sleep 2
                while IFS= read -r u; do
                    [ -n "$u" ] || continue
                    "$CHROMIUM_BIN" --no-sandbox \
                        --user-data-dir="$PROFILE" \
                        --app="$u" >/dev/null 2>&1 || true
                done <<EOF
${CHROMIUM_EXTRA_URLS}
EOF
            ) &
        fi

        # Session lifetime: bar exit = user double-clicked the island (leave
        # the browser); chromium exit = nothing left to show. Poll instead of
        # `wait -n` so the extra-URL helper above can't end the session.
        trap 'kill "$BAR_PID" "$APP_PID" 2>/dev/null' EXIT INT TERM
        while kill -0 "$BAR_PID" 2>/dev/null && kill -0 "$APP_PID" 2>/dev/null; do
            sleep 1
        done
        kill "$BAR_PID" "$APP_PID" 2>/dev/null
        # Let Chromium shut down cleanly so cookies/localStorage (GitHub &c.
        # logins) hit the persistent profile before X goes away.
        wait "$APP_PID" 2>/dev/null
        wait "$BAR_PID" 2>/dev/null
        exit 0
    fi
    exec "$@"
fi

JETSON_DIR="$(dirname "$SCRIPT_DIR")"
if [ -r "$SCRIPT_DIR/config_loader.sh" ]; then
    # shellcheck disable=SC1091
    . "$SCRIPT_DIR/config_loader.sh"
    jetson_load_config "${JETSON_CONFIG_FILE:-$JETSON_DIR/config.yaml}"
fi

HOME_URL="${CHROMIUM_HOME_URL:-https://www.google.com}"
# One-shot start URL(s) written by the firmware right before the kiosk
# hand-off (the exit-42 path goes through the supervisor, so the firmware's
# environment can't reach us). Used by the GitHub tile and the Pods/Studio
# web-IDE flow. One URL per line: the first replaces the home URL, each
# additional line opens as its own kiosk window (cycled via the island pill).
URL_FILE=/tmp/jetson_chromium_url
EXTRA_URLS=""
if [ -f "$URL_FILE" ]; then
    FIRST=1
    # `|| [ -n "$line" ]`: the firmware writes the file without a trailing
    # newline, and plain `read` would drop that final unterminated line.
    while IFS= read -r line || [ -n "$line" ]; do
        line="$(printf '%s' "$line" | tr -d '[:space:]')"
        [ -n "$line" ] || continue
        if [ "$FIRST" = 1 ]; then
            HOME_URL="$line"
            FIRST=0
        else
            EXTRA_URLS="${EXTRA_URLS}${line}
"
        fi
    done < <(head -c 4096 "$URL_FILE")
    rm -f "$URL_FILE"
fi
DISPLAY_NO="${CHROMIUM_DISPLAY:-:0}"
export DISPLAY="$DISPLAY_NO"

# Pick whichever Chromium binary the distro ships. xinit only treats a client
# name beginning with '/' or '.' as the client program; a bare
# "chromium-browser" is interpreted as an argument to its default xterm
# client. Resolve the executable here so --kiosk is sent to Chromium, not
# xterm.
CHROMIUM=""
for c in chromium-browser chromium chromium-browser.real; do
    chromium_path="$(command -v "$c" 2>/dev/null || true)"
    if [ -n "$chromium_path" ] && [ -x "$chromium_path" ]; then
        CHROMIUM="$chromium_path"
        break
    fi
done
if [ -z "$CHROMIUM" ]; then
    echo "launch_chromium: chromium not found. apt install chromium-browser" >&2
    exit 1
fi

# Locate the kiosk status bar (Dynamic Island strip + micro-WM). Installed
# next to the firmware; the build tree is checked for the run_fbdev.sh /
# non-installed path. When found, Chromium runs as a borderless --app window
# tucked below the 42px strip instead of a full-screen --kiosk (which would
# paint over the bar); the bar re-asserts that geometry anyway if a page goes
# full-screen. Double-clicking the island pill exits back to the firmware.
BAR_H=42
PANEL_W="${CHROMIUM_PANEL_WIDTH:-800}"
PANEL_H="${CHROMIUM_PANEL_HEIGHT:-480}"
JETSON_KIOSK_BAR=""
for p in /opt/jetson-fw/jetson_kiosk_bar "$JETSON_DIR/build/jetson_kiosk_bar"; do
    if [ -x "$p" ]; then
        JETSON_KIOSK_BAR="$p"
        break
    fi
done
export JETSON_KIOSK_BAR

# Profile lives on persistent storage so web logins (GitHub, Facebook, ...)
# survive kiosk restarts AND reboots -- the old /tmp profile was wiped with
# every boot, which threw the user's sessions away. The page cache still goes
# to /tmp: it is rebuildable data and keeping it off the eMMC/SD saves wear.
PROFILE_DIR="${CHROMIUM_PROFILE_DIR:-/var/lib/jetson-fw/chromium-profile}"
if ! mkdir -p "$PROFILE_DIR" 2>/dev/null; then
    echo "launch_chromium: cannot create $PROFILE_DIR; falling back to /tmp (logins will not survive reboots)" >&2
    PROFILE_DIR=/tmp/chromium-kiosk-profile
    mkdir -p "$PROFILE_DIR"
fi
chmod 700 "$PROFILE_DIR" 2>/dev/null || true
# The supervisor serializes kiosk sessions, so any singleton left in the
# (now persistent) profile is from a dead process. Clearing it up front keeps
# the extra-URL relaunches from racing a stale lock into a second browser.
rm -f "$PROFILE_DIR/SingletonLock" "$PROFILE_DIR/SingletonSocket" \
      "$PROFILE_DIR/SingletonCookie" 2>/dev/null || true

# Hand the client re-entry everything it needs to open the extra windows.
export CHROMIUM_BIN="$CHROMIUM"
export CHROMIUM_PROFILE_DIR="$PROFILE_DIR"
export CHROMIUM_EXTRA_URLS="$EXTRA_URLS"

# App list for the island long-press launcher in jetson_kiosk_bar ("Name|URL"
# per line). Regenerated every session so entries never go stale. The Pods
# view in the firmware appends the user's running GPU pods (their web-IDE
# proxy URLs) via /tmp/jetson_kiosk_extra_apps, rewritten on every pod-list
# refresh. Studio only appears when config/.env provides JETSON_STUDIO_URL.
APPS_FILE=/tmp/jetson_kiosk_apps
{
    echo "YouTube|https://www.youtube.com"
    echo "GitHub|https://github.com"
    echo "Facebook|https://www.facebook.com"
    echo "Messenger|https://www.messenger.com"
    if [ -n "${JETSON_STUDIO_URL:-}" ]; then
        echo "Studio|$JETSON_STUDIO_URL"
    fi
    echo "Gmail|https://mail.google.com"
    echo "ChatGPT|https://chatgpt.com"
    if [ -f /tmp/jetson_kiosk_extra_apps ]; then
        cat /tmp/jetson_kiosk_extra_apps
    fi
} > "$APPS_FILE"

# Common flags: --no-sandbox (runs as root under the service), --user-data-dir
# so a root profile doesn't collide with any existing one, --disable-gpu for
# the software-only tegra stack. The RAM block keeps a multi-window session
# viable on the 4GB Jetson: one renderer per *site* instead of per window,
# hard cap on renderer count, and no extension/sync/telemetry background work.
CHROMIUM_FLAGS=(
    --no-sandbox
    --user-data-dir="$PROFILE_DIR"
    --disk-cache-dir=/tmp/chromium-kiosk-cache
    --disk-cache-size=67108864
    --disable-gpu
    --disable-software-rasterizer
    --no-first-run
    --no-default-browser-check
    --disable-popup-blocking
    --disable-translate
    --disable-session-crashed-bubble
    --disable-infobars
    --process-per-site
    --renderer-process-limit=4
    --disable-extensions
    --disable-background-networking
    --disable-component-update
    --disable-sync
    --disable-breakpad
    --metrics-recording-only
    --no-pings
)
if [ -n "$JETSON_KIOSK_BAR" ]; then
    CHROMIUM_FLAGS+=(
        --app="$HOME_URL"
        --window-position=0,"$BAR_H"
        --window-size="$PANEL_W","$((PANEL_H - BAR_H))"
    )
else
    # No bar built (libx11-dev missing at build time): legacy full-screen
    # kiosk. --kiosk + --start-maximized fill the panel and hide all chrome.
    # Extra URLs would open as unreachable tabs here, so they are dropped.
    if [ -n "$EXTRA_URLS" ]; then
        echo "launch_chromium: kiosk bar missing, ignoring extra URLs" >&2
        export CHROMIUM_EXTRA_URLS=""
    fi
    CHROMIUM_FLAGS+=(
        --kiosk
        --start-maximized
        --window-size="$PANEL_W","$PANEL_H"
        "$HOME_URL"
    )
fi

# xinit runs Chromium as the sole X client; when Chromium exits, xinit kills
# the X server. vt1 is the console VT the firmware renders its framebuffer on;
# the firmware has already exited, so it is free. Keep the X cursor visible:
# unlike the firmware, Chromium does not draw its own mouse cursor, so
# -nocursor makes a working mouse look disconnected.
if command -v xinit >/dev/null 2>&1; then
    exec xinit "$SCRIPT_DIR/launch_chromium.sh" --x-client \
        "$CHROMIUM" "${CHROMIUM_FLAGS[@]}" -- \
        "$DISPLAY_NO" vt1 -nolisten tcp -s 0 -dpms
fi

# Fallback: start Xorg ourselves, run Chromium, then tear X down.
echo "launch_chromium: xinit not found, falling back to Xorg + chromium" >&2
Xorg "$DISPLAY_NO" vt1 -nolisten tcp -s 0 -dpms &
XPID=$!
cleanup_xorg()
{
    kill "$XPID" 2>/dev/null || true
    wait "$XPID" 2>/dev/null || true
}
trap cleanup_xorg EXIT INT TERM HUP
sleep 2
"$SCRIPT_DIR/launch_chromium.sh" --x-client "$CHROMIUM" "${CHROMIUM_FLAGS[@]}"
RC=$?
exit "$RC"
