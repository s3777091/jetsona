#!/bin/bash
# Launch a Chromium kiosk on the HDMI panel, taking over /dev/fb0 from the
# DS-02 firmware. Invoked by scripts/jetson_fw_run.sh when the firmware exits
# with code 42 (the "Chromium" drawer tile). When Chromium exits, control
# returns to the supervisor, which restarts the firmware so it re-acquires
# /dev/fb0.
#
# Runtime prerequisite (install once on the Jetson):
#   sudo apt install -y xserver-xorg-video-all xinit chromium-browser
# The HDMI LCD is driven by the stock tegra/nvidia X driver; this script just
# starts a bare X server (no desktop) with Chromium as its only client, so the
# default /etc/X11/xorg.conf is reused as-is.
set -u

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

# Pick whichever chromium binary the distro ships.
CHROMIUM=""
for c in chromium-browser chromium chromium-browser.real; do
    if command -v "$c" >/dev/null 2>&1; then CHROMIUM="$c"; break; fi
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
# the firmware has already exited, so it is free. -nocursor hides the X cursor
# (the firmware draws its own; once we hand back, it draws it again).
if command -v xinit >/dev/null 2>&1; then
    exec xinit "$CHROMIUM" "${CHROMIUM_FLAGS[@]}" -- "$DISPLAY_NO" vt1 -nolisten tcp -nocursor
fi

# Fallback: start Xorg ourselves, run Chromium, then tear X down.
echo "launch_chromium: xinit not found, falling back to Xorg + chromium" >&2
Xorg "$DISPLAY_NO" vt1 -nolisten tcp -nocursor &
XPID=$!
sleep 2
"$CHROMIUM" "${CHROMIUM_FLAGS[@]}"
RC=$?
kill "$XPID" 2>/dev/null
wait "$XPID" 2>/dev/null
exit "$RC"