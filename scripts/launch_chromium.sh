#!/bin/bash
# Launch a Chromium kiosk on the HDMI panel, taking over /dev/fb0 from the
# DS-02 firmware. Invoked by scripts/jetson_fw_run.sh when the firmware exits
# with code 42 (the "Chromium" drawer tile). When Chromium exits, control
# returns to the supervisor, which restarts the firmware so it re-acquires
# /dev/fb0.
#
# Runtime prerequisite (install once on the Jetson):
#   sudo apt install -y xserver-xorg-video-all xserver-xorg-input-libinput \
#       x11-xkb-utils xinit chromium-browser
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
    exec "$@"
fi

JETSON_DIR="$(dirname "$SCRIPT_DIR")"
if [ -r "$SCRIPT_DIR/config_loader.sh" ]; then
    # shellcheck disable=SC1091
    . "$SCRIPT_DIR/config_loader.sh"
    jetson_load_config "${JETSON_CONFIG_FILE:-$JETSON_DIR/config.yaml}"
fi

HOME_URL="${CHROMIUM_HOME_URL:-https://www.google.com}"
# One-shot start URL written by the firmware right before the kiosk hand-off
# (the exit-42 path goes through the supervisor, so the firmware's environment
# can't reach us). Used by the GitHub tile and the Pods/Studio web-IDE flow.
URL_FILE=/tmp/jetson_chromium_url
if [ -f "$URL_FILE" ]; then
    REQ_URL="$(head -c 2048 "$URL_FILE" | tr -d '[:space:]')"
    rm -f "$URL_FILE"
    [ -n "$REQ_URL" ] && HOME_URL="$REQ_URL"
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

# Kiosk flags: --no-sandbox (runs as root under the service), --user-data-dir
# so a root profile doesn't collide with any existing one, --disable-gpu for
# the software-only tegra stack. --kiosk + --start-maximized fill the 800x480
# panel and hide all chrome (no address bar, no tabs).
CHROMIUM_FLAGS=(
    --kiosk
    --no-sandbox
    --user-data-dir=/tmp/chromium-kiosk-profile
    --disable-gpu
    --disable-software-rasterizer
    --no-first-run
    --no-default-browser-check
    --disable-popup-blocking
    --disable-translate
    --start-maximized
    --window-size=800,480
    "$HOME_URL"
)

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
