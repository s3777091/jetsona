#!/bin/bash
# Launch a Chromium kiosk on the HDMI panel, taking over /dev/fb0 from the
# DS-02 firmware. Invoked by scripts/jetson_fw_run.sh when the firmware exits
# with code 42 (the "Chromium" drawer tile). When Chromium exits, control
# returns to the supervisor, which restarts the firmware so it re-acquires
# /dev/fb0.
#
# Runtime prerequisite (install once on the Jetson):
#   sudo apt install -y xserver-xorg-video-all xserver-xorg-input-libinput \
#       x11-xkb-utils x11-xserver-utils xinit openbox onboard curl dbus \
#       chromium-browser libx11-dev
# (libx11-dev is a build dependency for tools/kiosk_bar, the Dynamic Island
# strip + micro-WM that runs on top of Chromium.)
# The HDMI LCD is driven by the stock tegra/nvidia X driver; this script just
# starts a bare X server (no desktop) with Chromium as its only client, so the
# default /etc/X11/xorg.conf is reused as-is.
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# The firmware runs as root and persists its UI choices here. Preserve the
# path before client mode replaces HOME with Chromium's private runtime home.
export JETSON_SETTINGS_FILE="${JETSON_SETTINGS_FILE:-${HOME:-/root}/.jetson-fw/settings.kv}"

hardware_keyboard_present()
{
    local input_root="${JETSON_INPUT_ROOT:-/dev/input}"
    local device
    for device in \
        "$input_root"/by-id/*-event-kbd \
        "$input_root"/by-path/*-event-kbd; do
        [ -e "$device" ] && return 0
    done
    return 1
}

# xinit must start an executable as its first client, so it re-enters this
# script in client mode.  Keep Chromium out of the service's stale desktop
# environment and install an emergency way out of kiosk mode.  Killing the X
# server makes xinit terminate the client and lets jetson_fw_run.sh restart the
# firmware.
if [ "${1:-}" = "--x-client" ]; then
    shift
    # A systemd/bare-X hand-off has no desktop session bus. Leaving a stale
    # address here (or relying on D-Bus' X11 autolaunch fallback) makes
    # Chromium repeatedly report "Unknown address type". Each Chromium
    # invocation below is therefore given a small private session bus by
    # dbus-run-session. Clear both inherited overrides first; the system bus
    # then uses its standard well-known Unix socket.
    unset DBUS_SESSION_BUS_ADDRESS DBUS_SYSTEM_BUS_ADDRESS
    export HOME="${CHROMIUM_KIOSK_HOME:-/tmp/chromium-kiosk-home}"
    export XDG_CONFIG_HOME="${CHROMIUM_KIOSK_CONFIG_HOME:-$HOME/.config}"
    export XDG_CACHE_HOME="${CHROMIUM_KIOSK_CACHE_HOME:-$HOME/.cache}"
    export XDG_RUNTIME_DIR="${CHROMIUM_KIOSK_RUNTIME_DIR:-/tmp/chromium-kiosk-runtime}"
    mkdir -p "$HOME" "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$XDG_RUNTIME_DIR"

    # Xorg remains privileged because it owns the physical display, while the
    # browser runs as the dedicated unprivileged account created by install.sh.
    # This keeps Chromium's real sandbox enabled and removes the unsafe
    # --no-sandbox warning instead of merely hiding its infobar.
    if [ -z "${CHROMIUM_RUN_USER:-}" ] || ! id "$CHROMIUM_RUN_USER" >/dev/null 2>&1; then
        echo "launch_chromium: kiosk user is missing; run scripts/install.sh again" >&2
        exit 1
    fi
    if ! command -v runuser >/dev/null 2>&1 || ! command -v xhost >/dev/null 2>&1; then
        echo "launch_chromium: runuser/xhost missing (install util-linux x11-xserver-utils)" >&2
        exit 1
    fi
    if ! command -v dbus-run-session >/dev/null 2>&1; then
        echo "launch_chromium: dbus-run-session missing (install package 'dbus')" >&2
        exit 1
    fi
    if ! xhost "+SI:localuser:$CHROMIUM_RUN_USER" >/dev/null 2>&1; then
        echo "launch_chromium: cannot authorize $CHROMIUM_RUN_USER on $DISPLAY" >&2
        exit 1
    fi
    chromium_run()
    {
        runuser -u "$CHROMIUM_RUN_USER" -- env \
            -u DBUS_SESSION_BUS_ADDRESS \
            -u DBUS_SYSTEM_BUS_ADDRESS \
            HOME="$HOME" \
            XDG_CONFIG_HOME="$XDG_CONFIG_HOME" \
            XDG_CACHE_HOME="$XDG_CACHE_HOME" \
            XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" \
            DISPLAY="$DISPLAY" \
            dbus-run-session -- "$@"
    }

    # Paint the root window the same black the firmware left in the
    # framebuffer. X's default root is a grey stipple, and a flash of it
    # between the firmware's collapse and the bar's bloom is exactly what
    # breaks the illusion of a single hand-off animation.
    if command -v xsetroot >/dev/null 2>&1; then
        xsetroot -solid black >/dev/null 2>&1 || true
    fi

    if command -v setxkbmap >/dev/null 2>&1; then
        # Ctrl+Alt+Backspace exits the bare-X session even though Chromium has
        # no close button in --kiosk mode.
        setxkbmap -option '' -option terminate:ctrl_alt_bksp >/dev/null 2>&1 ||
            echo "launch_chromium: failed to enable Ctrl+Alt+Backspace" >&2
    else
        echo "launch_chromium: setxkbmap missing; Ctrl+Alt+Backspace unavailable" >&2
    fi

    portal_watch()
    {
        local target_pid="$1"
        local successes=0
        local status
        if ! command -v curl >/dev/null 2>&1; then
            echo "launch_chromium: curl missing; portal cannot auto-return" >&2
            return
        fi
        while kill -0 "$target_pid" 2>/dev/null; do
            status="$(curl --silent --show-error --output /dev/null \
                --write-out '%{http_code}' --connect-timeout 3 --max-time 5 \
                --header 'Cache-Control: no-cache, no-store' \
                --user-agent 'Mozilla/5.0 (Linux; Jetson) JetsonFW-PortalCheck/1.0' \
                -- "${JETSON_CAPTIVE_PORTAL_PROBE_URL:-http://connectivitycheck.gstatic.com/generate_204}" \
                2>/dev/null || true)"
            if [ "$status" = 204 ]; then
                successes=$((successes + 1))
                if [ "$successes" -ge 2 ]; then
                    echo "launch_chromium: portal authentication complete; returning to firmware" >&2
                    kill "$target_pid" 2>/dev/null || true
                    return
                fi
            else
                successes=0
            fi
            sleep 2
        done
    }

    # With the kiosk bar available, run it next to Chromium. The bar draws the
    # Dynamic Island strip on top and acts as a micro-WM: it hands Chromium the
    # X input focus (without it the USB keyboard is dead in a WM-less session)
    # and exits via the app rail's power button. Without the bar, openbox is a
    # best-effort focus/stacking fallback, especially for the touch keyboard.
    BAR_PID=""
    WM_PID=""
    ONBOARD_PID=""
    PORTAL_WATCH_PID=""
    if [ -n "${JETSON_KIOSK_BAR:-}" ] && [ -x "$JETSON_KIOSK_BAR" ]; then
        "$JETSON_KIOSK_BAR" &
        BAR_PID=$!
    elif command -v openbox >/dev/null 2>&1; then
        openbox --sm-disable >/dev/null 2>&1 &
        WM_PID=$!
    fi

    chromium_run "$@" &
    APP_PID=$!

    if [ -n "${JETSON_CAPTIVE_PORTAL_ONBOARD:-}" ] &&
       [ -x "$JETSON_CAPTIVE_PORTAL_ONBOARD" ]; then
        if command -v gsettings >/dev/null 2>&1; then
            chromium_run gsettings set org.onboard.window docking-enabled false >/dev/null 2>&1 || true
            chromium_run gsettings set org.onboard.window window-decoration false >/dev/null 2>&1 || true
            chromium_run gsettings set org.onboard.window force-to-top true >/dev/null 2>&1 || true
        fi
        chromium_run "$JETSON_CAPTIVE_PORTAL_ONBOARD" \
            --size=800x205 -x 0 -y 275 --layout=Compact --theme=Nightshade &
        ONBOARD_PID=$!
    fi

    # Extra kiosk windows: every URL beyond the first (newline-separated in
    # CHROMIUM_EXTRA_URLS) becomes its own --app window in the already running
    # Chromium. The bar cycles between them.
    if [ -n "$BAR_PID" ] && [ -n "${CHROMIUM_EXTRA_URLS:-}" ] &&
       [ -n "${CHROMIUM_BIN:-}" ]; then
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
                chromium_run "$CHROMIUM_BIN" \
                    --user-data-dir="$PROFILE" --app="$u" >/dev/null 2>&1 || true
            done <<EOF
${CHROMIUM_EXTRA_URLS}
EOF
        ) &
    fi

    if [ "${JETSON_CAPTIVE_PORTAL_MODE:-0}" = 1 ]; then
        portal_watch "${BAR_PID:-$APP_PID}" &
        PORTAL_WATCH_PID=$!
    fi

    session_cleanup()
    {
        trap - EXIT INT TERM HUP
        [ -z "$PORTAL_WATCH_PID" ] || kill "$PORTAL_WATCH_PID" 2>/dev/null || true
        [ -z "$ONBOARD_PID" ] || kill "$ONBOARD_PID" 2>/dev/null || true
        [ -z "$BAR_PID" ] || kill "$BAR_PID" 2>/dev/null || true
        [ -z "$WM_PID" ] || kill "$WM_PID" 2>/dev/null || true
        kill "$APP_PID" 2>/dev/null || true
        # Let Chromium shut down cleanly so portal cookies and other persisted
        # sessions reach the profile before Xorg gives the panel back.
        wait "$APP_PID" 2>/dev/null || true
        [ -z "$ONBOARD_PID" ] || wait "$ONBOARD_PID" 2>/dev/null || true
        [ -z "$BAR_PID" ] || wait "$BAR_PID" 2>/dev/null || true
        [ -z "$WM_PID" ] || wait "$WM_PID" 2>/dev/null || true
    }
    trap session_cleanup EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM HUP

    while kill -0 "$APP_PID" 2>/dev/null &&
          { [ -z "$BAR_PID" ] || kill -0 "$BAR_PID" 2>/dev/null; }; do
        sleep 1
    done
    exit 0
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
PORTAL_FILE=/tmp/jetson_captive_portal_session
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
JETSON_CAPTIVE_PORTAL_MODE=0
if [ -f "$PORTAL_FILE" ]; then
    JETSON_CAPTIVE_PORTAL_MODE=1
    rm -f "$PORTAL_FILE"
fi
export JETSON_CAPTIVE_PORTAL_MODE
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
# full-screen. Long-press the island and tap the rail's trailing power
# button to exit back to the firmware.
BAR_H=42
PANEL_W="${CHROMIUM_PANEL_WIDTH:-800}"
PANEL_H="${CHROMIUM_PANEL_HEIGHT:-480}"
# Prefer whichever copy is NEWEST, not just the installed one. build.sh
# refreshes build/jetson_kiosk_bar but leaves the old /opt/jetson-fw copy
# untouched (only install.sh replaces that), so a plain "first match wins"
# kept running a stale bar -- letter-disc icons, no rail power button, an
# invisible border -- until the next reinstall. "-nt" picks up a rebuild
# even without running install.sh.
JETSON_KIOSK_BAR=""
for p in /opt/jetson-fw/jetson_kiosk_bar "$JETSON_DIR/build/jetson_kiosk_bar"; do
    if [ -x "$p" ] && { [ -z "$JETSON_KIOSK_BAR" ] || [ "$p" -nt "$JETSON_KIOSK_BAR" ]; }; then
        JETSON_KIOSK_BAR="$p"
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

# Browser sandbox boundary. The service itself stays root for DRM/Xorg, but no
# Chromium process (including Dynamic Island app relaunches) inherits root.
CHROMIUM_RUN_USER="${CHROMIUM_KIOSK_USER:-jetson-kiosk}"
if ! run_identity="$(id -u "$CHROMIUM_RUN_USER" 2>/dev/null)"; then
    echo "launch_chromium: user '$CHROMIUM_RUN_USER' not found; run scripts/install.sh" >&2
    exit 1
fi
run_group="$(id -g "$CHROMIUM_RUN_USER")"
export CHROMIUM_RUN_USER CHROMIUM_RUN_UID="$run_identity" CHROMIUM_RUN_GID="$run_group"

# Captive portals must remain usable on the touch-only appliance. In auto mode
# Onboard appears only when X has no USB keyboard; touch itself is the pointer,
# and the normal visible X cursor remains available to an attached mouse.
JETSON_CAPTIVE_PORTAL_ONBOARD=""
if [ "$JETSON_CAPTIVE_PORTAL_MODE" = 1 ]; then
    case "${JETSON_CAPTIVE_PORTAL_ONSCREEN_KEYBOARD:-auto}" in
        0|false|FALSE|no|NO|off|OFF|none) ;;
        auto)
            if ! hardware_keyboard_present; then
                JETSON_CAPTIVE_PORTAL_ONBOARD="$(command -v onboard 2>/dev/null || true)"
            fi
            ;;
        1|true|TRUE|yes|YES|on|ON)
            JETSON_CAPTIVE_PORTAL_ONBOARD="$(command -v onboard 2>/dev/null || true)"
            [ -n "$JETSON_CAPTIVE_PORTAL_ONBOARD" ] ||
                echo "launch_chromium: onboard keyboard not installed" >&2
            ;;
        *)
            echo "launch_chromium: invalid JETSON_CAPTIVE_PORTAL_ONSCREEN_KEYBOARD" >&2
            ;;
    esac
fi
export JETSON_CAPTIVE_PORTAL_ONBOARD
RUNTIME_HOME="${CHROMIUM_KIOSK_HOME:-/var/lib/jetson-fw/chromium-home}"
RUNTIME_DIR="${CHROMIUM_KIOSK_RUNTIME_DIR:-/tmp/chromium-kiosk-runtime}"
mkdir -p "$RUNTIME_HOME" "$RUNTIME_HOME/.config" "$RUNTIME_HOME/.cache" \
         "$RUNTIME_DIR" /tmp/chromium-kiosk-cache
chown -R "$run_identity:$run_group" "$PROFILE_DIR" "$RUNTIME_HOME" \
         "$RUNTIME_DIR" /tmp/chromium-kiosk-cache
chmod 700 "$PROFILE_DIR" "$RUNTIME_HOME" "$RUNTIME_DIR"
export CHROMIUM_KIOSK_HOME="$RUNTIME_HOME"
export CHROMIUM_KIOSK_RUNTIME_DIR="$RUNTIME_DIR"
# The supervisor serializes kiosk sessions, so any singleton left in the
# (now persistent) profile is from a dead process. Clearing it up front keeps
# the extra-URL relaunches from racing a stale lock into a second browser.
rm -f "$PROFILE_DIR/SingletonLock" "$PROFILE_DIR/SingletonSocket" \
      "$PROFILE_DIR/SingletonCookie" 2>/dev/null || true

# Hand the client re-entry everything it needs to open the extra windows.
export CHROMIUM_BIN="$CHROMIUM"
export CHROMIUM_PROFILE_DIR="$PROFILE_DIR"
export CHROMIUM_EXTRA_URLS="$EXTRA_URLS"
# Tells jetson_kiosk_bar which launcher entry the firmware was opening, so it
# can continue that app's island zoom instead of starting a generic one. Its
# presence is also the signal that this really is a firmware hand-off.
export JETSON_KIOSK_HANDOFF_URL="$HOME_URL"

# App list for the island long-press launcher in jetson_kiosk_bar ("Name|URL"
# per line). Regenerated every session so entries never go stale. The Pods
# view in the firmware appends the user's running GPU pods (their web-IDE
# proxy URLs) via /tmp/jetson_kiosk_extra_apps, rewritten on every pod-list
# refresh. Studio only appears when config/.env provides JETSON_STUDIO_URL.
APPS_FILE=/tmp/jetson_kiosk_apps
{
    echo "Chromium|$HOME_URL"
    echo "YouTube|https://www.youtube.com"
    echo "GitHub|https://github.com"
    echo "Facebook|https://www.facebook.com"
    echo "Messenger|https://www.messenger.com"
    echo "Teams|https://teams.microsoft.com"
    if [ -n "${JETSON_STUDIO_URL:-}" ]; then
        echo "Studio|$JETSON_STUDIO_URL"
    fi
    echo "Gmail|https://mail.google.com"
    echo "ChatGPT|https://chatgpt.com"
    if [ -f /tmp/jetson_kiosk_extra_apps ]; then
        cat /tmp/jetson_kiosk_extra_apps
    fi
} > "$APPS_FILE"

# Common flags: the browser runs as jetson-kiosk so Chromium's sandbox stays
# enabled; --user-data-dir isolates its persistent profile, --disable-gpu for
# the software-only tegra stack. The RAM block keeps a multi-window session
# viable on the 4GB Jetson: one renderer per *site* instead of per window,
# hard cap on renderer count, and no extension/sync/telemetry background work.
CHROMIUM_FLAGS=(
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
filter_xinit_stderr()
{
    # xinit prints this even during the launcher's intentional X shutdown.
    # Preserve every useful Xorg/client diagnostic and discard only that one
    # expected lifecycle message.
    while IFS= read -r line; do
        [ "$line" = "xinit: connection to X server lost" ] ||
            printf '%s\n' "$line" >&2
    done
}

if command -v xinit >/dev/null 2>&1; then
    # -background none: do not repaint the root at startup, so the firmware's
    # final black frame survives in the framebuffer until the bar draws over
    # it. Without it X clears to its grey stipple and the hand-off flashes.
    exec xinit "$SCRIPT_DIR/launch_chromium.sh" --x-client \
        "$CHROMIUM" "${CHROMIUM_FLAGS[@]}" -- \
        "$DISPLAY_NO" vt1 -nolisten tcp -s 0 -dpms -background none \
        2> >(filter_xinit_stderr)
fi

# Fallback: start Xorg ourselves, run Chromium, then tear X down.
echo "launch_chromium: xinit not found, falling back to Xorg + chromium" >&2
Xorg "$DISPLAY_NO" vt1 -nolisten tcp -s 0 -dpms -background none &
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
