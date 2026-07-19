#!/bin/bash
# Hand the 800x480 panel from the framebuffer firmware to Chiaki running as
# the only client of a bare Xorg server. The supervisor restarts the firmware
# after Chiaki exits and re-acquires /dev/fb0.
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JETSON_DIR="$(dirname "$SCRIPT_DIR")"
if [ -r "$SCRIPT_DIR/config_loader.sh" ]; then
    # shellcheck disable=SC1091
    . "$SCRIPT_DIR/config_loader.sh"
    jetson_load_config "${JETSON_CONFIG_FILE:-$JETSON_DIR/config.yaml}"
fi

CTL="$SCRIPT_DIR/ps_remote_play_ctl.sh"
if [ ! -r "$CTL" ]; then
    echo "launch_ps_remote_play: missing $CTL" >&2
    exit 1
fi
# shellcheck source=ps_remote_play_ctl.sh
. "$CTL"

MODE="${1:-}"
case "$MODE" in
    configure|stream) ;;
    *) echo "Usage: launch_ps_remote_play.sh configure|stream" >&2; exit 2 ;;
esac

DISPLAY_NO="${PS_REMOTE_PLAY_DISPLAY:-:0}"
VT="${PS_REMOTE_PLAY_VT:-vt1}"
RUNTIME_DIR="${PS_REMOTE_PLAY_XDG_RUNTIME_DIR:-$PSRP_HOME/runtime}"
export DISPLAY="$DISPLAY_NO"
export HOME="$PSRP_HOME"
export XDG_CONFIG_HOME="$PSRP_XDG_CONFIG_HOME"
export XDG_RUNTIME_DIR="$RUNTIME_DIR"
export SDL_AUDIODRIVER="${PS_REMOTE_PLAY_AUDIO_DRIVER:-alsa}"
export QT_QPA_PLATFORM=xcb
export QT_QUICK_CONTROLS_MATERIAL_VARIANT="${QT_QUICK_CONTROLS_MATERIAL_VARIANT:-Dense}"
if [ "$(id -u)" -eq 0 ]; then
    export QTWEBENGINE_DISABLE_SANDBOX="${QTWEBENGINE_DISABLE_SANDBOX:-1}"
    # Chiaki's PSN login webview is QtWebEngine/Chromium; root launches need
    # the real Chromium flags too, otherwise PSN Login exits immediately.
    PSRP_QTWEBENGINE_ROOT_FLAGS="--no-sandbox --disable-setuid-sandbox"
else
    PSRP_QTWEBENGINE_ROOT_FLAGS=""
fi
export QTWEBENGINE_CHROMIUM_FLAGS="${PSRP_QTWEBENGINE_ROOT_FLAGS} ${QTWEBENGINE_CHROMIUM_FLAGS:---disable-gpu --disable-gpu-compositing --disable-dev-shm-usage}"

psrp_ensure_dirs || {
    echo "launch_ps_remote_play: cannot prepare $PSRP_HOME" >&2
    exit 1
}
chmod 700 "$RUNTIME_DIR" 2>/dev/null || true

if ! psrp_resolve_chiaki; then
    echo "launch_ps_remote_play: chiaki-ng/chiaki/AppImage not found" >&2
    echo "Set CHIAKI_BIN to the ARM64 executable or AppImage path." >&2
    exit 127
fi
if [ "$PSRP_APPIMAGE_EXTRACT" -eq 1 ]; then
    export APPIMAGE_EXTRACT_AND_RUN=1
fi

PSRP_CHIAKI_APPDIR="${APPDIR:-}"
PSRP_CHIAKI_LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"
PSRP_CHIAKI_QTWEBENGINEPROCESS_PATH="${QTWEBENGINEPROCESS_PATH:-}"

psrp_prepend_chiaki_library_path()
{
    local dir="$1"
    [ -d "$dir" ] || return 0
    case ":$PSRP_CHIAKI_LD_LIBRARY_PATH:" in
        *":$dir:"*) return 0 ;;
    esac
    if [ -n "$PSRP_CHIAKI_LD_LIBRARY_PATH" ]; then
        PSRP_CHIAKI_LD_LIBRARY_PATH="$dir:$PSRP_CHIAKI_LD_LIBRARY_PATH"
    else
        PSRP_CHIAKI_LD_LIBRARY_PATH="$dir"
    fi
}

psrp_configure_chiaki_runtime()
{
    local bin dir probe appdir nss_dir
    bin="${PSRP_CHIAKI[0]}"
    dir="$(cd "$(dirname "$bin")" && pwd -P)" || return 0
    appdir=""
    probe="$dir"
    while [ "$probe" != "/" ]; do
        if [ -d "$probe/squashfs-root/usr" ]; then
            appdir="$probe/squashfs-root"
            break
        fi
        if [ -d "$probe/usr" ] && [ -d "$probe/usr/libexec" ]; then
            appdir="$probe"
            break
        fi
        probe="$(dirname "$probe")"
    done

    if [ -n "$appdir" ]; then
        PSRP_CHIAKI_APPDIR="${PSRP_CHIAKI_APPDIR:-$appdir}"
        psrp_prepend_chiaki_library_path "$appdir/usr/lib"
        psrp_prepend_chiaki_library_path "$appdir/usr/lib/aarch64-linux-gnu"
        psrp_prepend_chiaki_library_path "$appdir/usr/lib/aarch64-linux-gnu/nss"
        psrp_prepend_chiaki_library_path "$appdir/usr/lib/nss"
        if [ -x "$appdir/usr/libexec/QtWebEngineProcess" ] &&
            [ -z "$PSRP_CHIAKI_QTWEBENGINEPROCESS_PATH" ]; then
            PSRP_CHIAKI_QTWEBENGINEPROCESS_PATH="$appdir/usr/libexec/QtWebEngineProcess"
        fi
    fi

    for nss_dir in \
        /usr/lib/aarch64-linux-gnu/nss \
        /usr/lib/arm-linux-gnueabihf/nss \
        /usr/lib/x86_64-linux-gnu/nss \
        /usr/lib/nss; do
        psrp_prepend_chiaki_library_path "$nss_dir"
    done
}

psrp_configure_chiaki_runtime

psrp_load_state
if ! psrp_apply_preset "$PSRP_PRESET"; then
    echo "launch_ps_remote_play: failed to apply the $PSRP_PRESET preset" >&2
    exit 1
fi

CLIENT_ARGS=()
if [ "$MODE" = "stream" ]; then
    if [ -z "$PSRP_HOST" ]; then
        echo "launch_ps_remote_play: PS5 address is not configured" >&2
        exit 2
    fi

    psrp_find_registration
    # Always use the exact nickname found in the active Chiaki profile. A stale
    # optional nickname in our state file must not make direct streaming fail.
    [ -z "$PSRP_REGISTERED_NICKNAME" ] || \
        PSRP_NICKNAME="$PSRP_REGISTERED_NICKNAME"
    if [ "$PSRP_REGISTERED" -eq 0 ] || [ -z "$PSRP_NICKNAME" ]; then
        echo "launch_ps_remote_play: no registered PS5 was found; run setup first" >&2
        exit 2
    fi

    # Added by chiaki-ng to close its main process when a CLI-started session
    # ends. Omit it only when an older Chiaki help explicitly lacks the flag.
    supports_exit_flag=1
    psrp_capture_cli "${PS_REMOTE_PLAY_HELP_TIMEOUT:-4}" --help || true
    if [ -n "$PSRP_CLI_OUTPUT" ] &&
        [[ "$PSRP_CLI_OUTPUT" != *--exit-app-on-stream-exit* ]]; then
        supports_exit_flag=0
    fi
    # Positional CLI commands do not automatically switch to current_profile.
    # QCommandLineParser also requires every option to precede `stream`.
    [ -z "$PSRP_PROFILE" ] || CLIENT_ARGS+=(--profile "$PSRP_PROFILE")
    [ "$supports_exit_flag" -eq 0 ] || CLIENT_ARGS+=(--exit-app-on-stream-exit)
    CLIENT_ARGS+=(--fullscreen)
    if [ -n "$PSRP_PASSCODE" ]; then
        CLIENT_ARGS+=(--passcode "$PSRP_PASSCODE")
    fi
    CLIENT_ARGS+=(stream "$PSRP_NICKNAME" "$PSRP_HOST")
fi

psrp_best_effort_wakeup()
{
    local wake_timeout="${PS_REMOTE_PLAY_WAKE_TIMEOUT:-25}"
    local deadline
    psrp_probe_host "$PSRP_HOST" || true
    [ "$PSRP_PROBE_STATE" = "standby" ] || return 0

    echo "launch_ps_remote_play: PS5 is in rest mode; requesting wakeup"
    if ! psrp_extract_regist_key "$PSRP_NICKNAME"; then
        echo "launch_ps_remote_play: wake key unavailable; stream will still be attempted"
        return 0
    fi
    # Neither the registration key nor passcode is written to stdout/stderr.
    psrp_run_cli_quiet 8 wakeup -5 -h "$PSRP_HOST" -r "$PSRP_REGIST_KEY" || true
    PSRP_REGIST_KEY=""

    [[ "$wake_timeout" =~ ^[0-9]+$ ]] || wake_timeout=25
    deadline=$((SECONDS + wake_timeout))
    while [ "$SECONDS" -lt "$deadline" ]; do
        sleep 2
        psrp_probe_host "$PSRP_HOST" || true
        [ "$PSRP_PROBE_STATE" = "ready" ] && return 0
    done
    echo "launch_ps_remote_play: PS5 did not report ready; trying the stream anyway"
}

if [ "$MODE" = "stream" ] && ! psrp_is_true "${PS_REMOTE_PLAY_DRY_RUN:-0}"; then
    psrp_best_effort_wakeup
fi

if psrp_is_true "${PS_REMOTE_PLAY_DRY_RUN:-0}"; then
    dry_command="${PSRP_CHIAKI[0]}"
    printf 'launch_ps_remote_play: dry-run: %s' "$dry_command"
    for arg in "${CLIENT_ARGS[@]}"; do
        if [ -n "$PSRP_PASSCODE" ] && [ "$arg" = "$PSRP_PASSCODE" ]; then
            printf ' %s' '<redacted-passcode>'
        else
            printf ' %q' "$arg"
        fi
    done
    printf '\n'
    exit 0
fi

X_PID=""
CLOCKS_ACTIVE=0
CLOCKS_STATE="$PSRP_HOME/jetson-clocks.restore"
CLOCKS_MARKER="$PSRP_HOME/jetson-clocks.active"

psrp_restore_clocks()
{
    local clocks
    clocks="$(command -v jetson_clocks 2>/dev/null || true)"
    if [ -n "$clocks" ] && [ -s "$CLOCKS_STATE" ]; then
        if ! "$clocks" --restore "$CLOCKS_STATE" >/dev/null 2>&1; then
            # Keep the baseline + marker so a later launch can retry recovery.
            echo "launch_ps_remote_play: warning: could not restore Jetson clocks" >&2
            CLOCKS_ACTIVE=0
            return 1
        fi
    fi
    CLOCKS_ACTIVE=0
    rm -f "$CLOCKS_MARKER" "$CLOCKS_STATE"
    return 0
}

psrp_enable_clocks()
{
    local clocks
    [ "$MODE" = "stream" ] || return 0
    psrp_is_true "${PS_REMOTE_PLAY_MAX_CLOCKS:-1}" || return 0
    clocks="$(command -v jetson_clocks 2>/dev/null || true)"
    [ -n "$clocks" ] || return 0

    # Recover a prior session killed before its EXIT trap, then take a fresh
    # baseline. The marker prevents accidentally storing already-maxed clocks.
    if [ -e "$CLOCKS_MARKER" ] && [ -s "$CLOCKS_STATE" ]; then
        if ! "$clocks" --restore "$CLOCKS_STATE" >/dev/null 2>&1; then
            echo "launch_ps_remote_play: warning: stale max-clock state could not be restored" >&2
            return 0
        fi
    fi
    rm -f "$CLOCKS_MARKER" "$CLOCKS_STATE"
    if "$clocks" --store "$CLOCKS_STATE" >/dev/null 2>&1; then
        chmod 600 "$CLOCKS_STATE" 2>/dev/null || true
        if ! : > "$CLOCKS_MARKER"; then
            "$clocks" --restore "$CLOCKS_STATE" >/dev/null 2>&1 || true
            rm -f "$CLOCKS_STATE"
            return 0
        fi
        chmod 600 "$CLOCKS_MARKER" 2>/dev/null || true
        if "$clocks" >/dev/null 2>&1; then
            CLOCKS_ACTIVE=1
            return 0
        fi
        psrp_restore_clocks || true
    fi
    return 0
}

psrp_cleanup()
{
    local rc=$?
    trap - EXIT INT TERM HUP
    if [ -n "$X_PID" ]; then
        kill "$X_PID" 2>/dev/null || true
        wait "$X_PID" 2>/dev/null || true
        X_PID=""
    fi
    if [ "$CLOCKS_ACTIVE" -eq 1 ] || [ -e "$CLOCKS_MARKER" ]; then
        psrp_restore_clocks || true
    fi
    exit "$rc"
}

trap psrp_cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

psrp_enable_clocks

unset DBUS_SESSION_BUS_ADDRESS DBUS_SYSTEM_BUS_ADDRESS
PSRP_CLIENT_ENV=()
PSRP_CLIENT_CMD=()
PSRP_ENV_BIN="$(command -v env 2>/dev/null || true)"
[ -n "$PSRP_ENV_BIN" ] || PSRP_ENV_BIN="/usr/bin/env"
[ -n "$PSRP_CHIAKI_APPDIR" ] && PSRP_CLIENT_ENV+=("APPDIR=$PSRP_CHIAKI_APPDIR")
[ -n "$PSRP_CHIAKI_LD_LIBRARY_PATH" ] && PSRP_CLIENT_ENV+=("LD_LIBRARY_PATH=$PSRP_CHIAKI_LD_LIBRARY_PATH")
if [ -n "$PSRP_CHIAKI_QTWEBENGINEPROCESS_PATH" ]; then
    PSRP_CLIENT_ENV+=("QTWEBENGINEPROCESS_PATH=$PSRP_CHIAKI_QTWEBENGINEPROCESS_PATH")
fi
PSRP_DBUS_RUN_SESSION="$(command -v dbus-run-session 2>/dev/null || true)"
if [ -n "$PSRP_DBUS_RUN_SESSION" ]; then
    # xinit only treats absolute/relative paths as the client program; a bare
    # command name falls through to its default xterm client.
    PSRP_CLIENT_CMD=("$PSRP_DBUS_RUN_SESSION" "$PSRP_ENV_BIN")
else
    echo "launch_ps_remote_play: dbus-run-session missing; PSN login webview may be unstable" >&2
    PSRP_CLIENT_CMD=("$PSRP_ENV_BIN")
fi

X_ARGS=("$DISPLAY_NO" "$VT" -nolisten tcp -nocursor -s 0 -dpms)
if command -v xinit >/dev/null 2>&1; then
    xinit "${PSRP_CLIENT_CMD[@]}" "${PSRP_CLIENT_ENV[@]}" \
        "${PSRP_CHIAKI[@]}" \
        "${CLIENT_ARGS[@]}" -- "${X_ARGS[@]}"
    client_rc=$?
    exit "$client_rc"
fi

XORG="$(command -v Xorg 2>/dev/null || true)"
if [ -z "$XORG" ]; then
    echo "launch_ps_remote_play: xinit/Xorg not found" >&2
    exit 127
fi

echo "launch_ps_remote_play: xinit unavailable; starting bare Xorg directly" >&2
"$XORG" "${X_ARGS[@]}" &
X_PID=$!
sleep "${PS_REMOTE_PLAY_X_START_DELAY:-2}"
if ! kill -0 "$X_PID" 2>/dev/null; then
    wait "$X_PID" 2>/dev/null
    echo "launch_ps_remote_play: Xorg failed to start" >&2
    exit 1
fi

"${PSRP_CLIENT_CMD[@]}" "${PSRP_CLIENT_ENV[@]}" \
    "${PSRP_CHIAKI[@]}" "${CLIENT_ARGS[@]}"
client_rc=$?
kill "$X_PID" 2>/dev/null || true
wait "$X_PID" 2>/dev/null || true
X_PID=""
exit "$client_rc"
