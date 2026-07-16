#!/bin/bash
# Run a locally built FBDEV firmware as the only owner of the HDMI panel.
# The installed jetson-fw service and the desktop display manager are stopped
# for the session, then the previously active owner is restored on exit.

set -u

if [ "$(id -u)" -ne 0 ]; then
    echo "run_fbdev: run this script with sudo" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JETSON_DIR="$(dirname "$SCRIPT_DIR")"
if [ ! -f "$JETSON_DIR/CMakeLists.txt" ]; then
    echo "run_fbdev: repository root not found above $SCRIPT_DIR" >&2
    echo "Run the checked-in script: sudo bash ~/jetsona/scripts/run_fbdev.sh" >&2
    exit 1
fi

# shellcheck disable=SC1091
. "$SCRIPT_DIR/config_loader.sh"
jetson_load_config "${JETSON_CONFIG_FILE:-$JETSON_DIR/config.yaml}"
jetson_load_secrets "${JETSON_ENV_FILE:-$JETSON_DIR/.env}"

if [ -n "${JETSON_FW_BIN:-}" ]; then
    FW="$JETSON_FW_BIN"
elif [ -x "$JETSON_DIR/build-fbdev/jetson_fw" ]; then
    FW="$JETSON_DIR/build-fbdev/jetson_fw"
else
    FW="$JETSON_DIR/build/jetson_fw"
fi

if [ ! -x "$FW" ]; then
    echo "run_fbdev: firmware binary not found: $FW" >&2
    echo "Build it first:" >&2
    echo "  JETSON_BUILD_DIR=build-fbdev JETSON_DISPLAY_BACKEND=FBDEV bash scripts/build.sh" >&2
    exit 1
fi

service_was_active=0
desktop_was_active=0
systemctl is-active --quiet jetson-fw.service && service_was_active=1
systemctl is-active --quiet display-manager.service && desktop_was_active=1

restore_previous_owner()
{
    rc=$?
    trap - EXIT INT TERM HUP

    # A direct framebuffer renderer and Xorg must never be restored together.
    # If an old configuration had both active, prefer returning the user to the
    # desktop; multi-user-only kiosk boots restore the firmware service.
    if [ "$desktop_was_active" -eq 1 ]; then
        echo "==> restoring desktop..."
        systemctl start display-manager.service || true
    elif [ "$service_was_active" -eq 1 ]; then
        echo "==> restoring jetson-fw.service..."
        systemctl start jetson-fw.service || true
    else
        echo "==> leaving the console active (no previous display owner)"
    fi
    echo "==> done (jetson_fw exit=$rc)"
    exit "$rc"
}
trap restore_previous_owner EXIT INT TERM HUP

if [ "$desktop_was_active" -eq 1 ]; then
    echo "==> stopping desktop display manager..."
    systemctl stop display-manager.service
fi

# Stop this after the desktop. Using `systemctl isolate multi-user.target`
# here is incorrect: jetson-fw.service is WantedBy=multi-user.target and can be
# started again immediately, which makes the manual binary fail its owner lock.
if [ "$service_was_active" -eq 1 ]; then
    echo "==> stopping installed jetson-fw.service..."
    systemctl stop jetson-fw.service
fi

if pgrep -x jetson_fw >/dev/null 2>&1; then
    echo "run_fbdev: another unmanaged jetson_fw process is still running:" >&2
    pgrep -a -x jetson_fw >&2 || true
    echo "Stop it with 'sudo pkill -TERM -x jetson_fw', then retry." >&2
    exit 1
fi

export JETSON_FB_DEVICE="${JETSON_FB_DEVICE:-/dev/fb0}"
echo "==> launching $FW (fbdev=$JETSON_FB_DEVICE) -- Ctrl+C to stop"
cd "$JETSON_DIR"
"$FW" "$@"
