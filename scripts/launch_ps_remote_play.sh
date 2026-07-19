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

psrp_load_state
if ! psrp_apply_preset "$PSRP_PRESET"; then
    echo "launch_ps_remote_play: failed to apply the $PSRP_PRESET preset" >&2
    exit 1
fi

# One-shot registration request queued by the firmware's IP + PIN sheet.
# Consumed (deleted) immediately so the PIN never lingers on disk.
REGIST_REQUEST_FILE="${PS_REMOTE_PLAY_REGIST_REQUEST:-/var/lib/jetson-fw/ps-remote-play-regist.pending}"
# Direct-stream credentials saved by a successful headless registration
# (chiaki-cli cannot write Chiaki's Qt config, so the launcher keeps them).
REGIST_KEYS_FILE="${PS_REMOTE_PLAY_REGIST_KEYS:-/var/lib/jetson-fw/ps-remote-play-regist.conf}"
REGIST_PIN=""
REGIST_HOST=""
AUTO_STREAM_AFTER_CONFIGURE=0
if [ "$MODE" = "configure" ] && [ -r "$REGIST_REQUEST_FILE" ]; then
    while IFS= read -r line || [ -n "$line" ]; do
        line="${line%$'\r'}"
        case "$line" in
            host=*) REGIST_HOST="${line#host=}" ;;
            pin=*) REGIST_PIN="${line#pin=}" ;;
        esac
    done < "$REGIST_REQUEST_FILE"
    rm -f "$REGIST_REQUEST_FILE"
    psrp_valid_host "$REGIST_HOST" || REGIST_HOST=""
    [[ "$REGIST_PIN" =~ ^[0-9]{8}$ ]] || REGIST_PIN=""
    [ -n "$REGIST_HOST" ] && PSRP_HOST="$REGIST_HOST"
    # The user asked for "IP + PIN -> play": whatever way the registration
    # completes below (headless or chiaki's own setup UI), continue straight
    # into the stream afterwards.
    AUTO_STREAM_AFTER_CONFIGURE=1
fi

# Locate a chiaki-cli capable of `regist` (separate binary; the Qt client
# itself has no regist command).
psrp_find_regist_cli()
{
    local candidate resolved
    resolved="$(command -v chiaki-cli 2>/dev/null || true)"
    if [ -n "$resolved" ]; then printf '%s' "$resolved"; return 0; fi
    for candidate in \
        /opt/chiaki-ng/chiaki-cli \
        /opt/jetson-fw/chiaki-cli \
        "$(dirname "${PSRP_CHIAKI[0]}")/chiaki-cli"; do
        [ -x "$candidate" ] && { printf '%s' "$candidate"; return 0; }
    done
    return 1
}

# Try to register fully headless: chiaki-cli regist with the queued PIN and a
# configured PSN account id. On success the regist key + RP key are stored in
# $REGIST_KEYS_FILE so `stream` can connect directly with --registkey/--morning
# even though Chiaki's own Qt config knows nothing about the console. Any
# missing piece falls through to chiaki's setup UI. Secrets never hit logs.
psrp_try_headless_regist()
{
    local cli account output regist_key rp_key morning
    [ -n "$REGIST_PIN" ] && [ -n "$PSRP_HOST" ] || return 1
    account="${PS_REMOTE_PLAY_PSN_ACCOUNT_ID:-}"
    if [ -z "$account" ]; then
        echo "launch_ps_remote_play: PS_REMOTE_PLAY_PSN_ACCOUNT_ID is not set; using Chiaki setup UI"
        return 1
    fi
    cli="$(psrp_find_regist_cli)" || {
        echo "launch_ps_remote_play: chiaki-cli not found; using Chiaki setup UI"
        return 1
    }

    echo "launch_ps_remote_play: attempting headless PS5 registration"
    output="$(timeout 45s "$cli" regist -h "$PSRP_HOST" --target ps5 \
        --psn-account-id "$account" --pin "$REGIST_PIN" 2>&1)" || {
        # Older chiaki-cli builds have no --target option; retry without it.
        output="$(timeout 45s "$cli" regist -h "$PSRP_HOST" \
            --psn-account-id "$account" --pin "$REGIST_PIN" 2>&1)" || {
            echo "launch_ps_remote_play: headless registration failed; using Chiaki setup UI"
            return 1
        }
    }

    regist_key="$(printf '%s\n' "$output" | sed -n 's/.*RP-RegistKey:[[:space:]]*//p' | head -n1)"
    regist_key="${regist_key%%[[:space:]]*}"
    rp_key="$(printf '%s\n' "$output" | sed -n 's/.*RP-Key:[[:space:]]*//p' | head -n1)"
    rp_key="${rp_key//[[:space:]]/}"
    if ! [[ "$regist_key" =~ ^[A-Za-z0-9]{4,64}$ ]] || ! [[ "$rp_key" =~ ^[A-Fa-f0-9]{32}$ ]]; then
        echo "launch_ps_remote_play: could not parse the registration result; using Chiaki setup UI"
        return 1
    fi
    # 16 raw bytes, base64 — the format --morning expects.
    morning="$(printf "$(printf '%s' "$rp_key" | sed 's/../\\x&/g')" | base64 | tr -d '\n')"

    # Direct streaming needs client support for --registkey/--morning.
    psrp_capture_cli "${PS_REMOTE_PLAY_HELP_TIMEOUT:-4}" --help || true
    if [[ "$PSRP_CLI_OUTPUT" != *registkey* ]] || [[ "$PSRP_CLI_OUTPUT" != *morning* ]]; then
        echo "launch_ps_remote_play: client lacks --registkey/--morning; using Chiaki setup UI"
        return 1
    fi

    umask 077
    if ! printf 'host=%s\nregistkey=%s\nmorning=%s\n' \
        "$PSRP_HOST" "$regist_key" "$morning" > "$REGIST_KEYS_FILE"; then
        echo "launch_ps_remote_play: cannot store registration keys; using Chiaki setup UI"
        return 1
    fi
    chmod 600 "$REGIST_KEYS_FILE" 2>/dev/null || true
    echo "launch_ps_remote_play: PS5 registered headless"
    return 0
}

# Load saved direct-stream credentials (from a previous headless regist).
PSRP_DIRECT_REGISTKEY=""
PSRP_DIRECT_MORNING=""
psrp_load_direct_keys()
{
    local line
    PSRP_DIRECT_REGISTKEY=""
    PSRP_DIRECT_MORNING=""
    [ -r "$REGIST_KEYS_FILE" ] || return 1
    while IFS= read -r line || [ -n "$line" ]; do
        line="${line%$'\r'}"
        case "$line" in
            registkey=*) PSRP_DIRECT_REGISTKEY="${line#registkey=}" ;;
            morning=*) PSRP_DIRECT_MORNING="${line#morning=}" ;;
        esac
    done < "$REGIST_KEYS_FILE"
    [ -n "$PSRP_DIRECT_REGISTKEY" ] && [ -n "$PSRP_DIRECT_MORNING" ]
}

if [ "$MODE" = "configure" ] && [ -n "$REGIST_PIN" ]; then
    if psrp_try_headless_regist; then
        # Registration done — no setup UI needed. Jump straight to the game.
        REGIST_PIN=""
        exec bash "${BASH_SOURCE[0]}" stream
    fi
    REGIST_PIN=""
fi

CLIENT_ARGS=()
DIRECT_STREAM=0
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
        # No console in Chiaki's own config — fall back to the credentials a
        # headless (chiaki-cli) registration stored for us.
        if psrp_load_direct_keys; then
            DIRECT_STREAM=1
            PSRP_NICKNAME="${PSRP_NICKNAME:-PS5}"
        else
            echo "launch_ps_remote_play: no registered PS5 was found; run setup first" >&2
            exit 2
        fi
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
    if [ "$DIRECT_STREAM" -eq 1 ]; then
        CLIENT_ARGS+=(--registkey "$PSRP_DIRECT_REGISTKEY" \
                      --morning "$PSRP_DIRECT_MORNING")
    fi
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
    if [ "$DIRECT_STREAM" -eq 1 ]; then
        PSRP_REGIST_KEY="$PSRP_DIRECT_REGISTKEY"
    elif ! psrp_extract_regist_key "$PSRP_NICKNAME"; then
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
    redact_next=0
    for arg in "${CLIENT_ARGS[@]}"; do
        if [ "$redact_next" -eq 1 ]; then
            printf ' %s' '<redacted>'
            redact_next=0
            continue
        fi
        case "$arg" in
            --registkey|--morning)
                # Console-access credentials: never echo their values.
                redact_next=1
                printf ' %q' "$arg"
                continue
                ;;
        esac
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

# After the user finishes registration inside Chiaki's setup UI, continue
# straight into the stream ("nhập IP + mã PIN xong là vào remote luôn")
# instead of dropping back to the firmware and requiring another tap.
psrp_maybe_continue_into_stream()
{
    [ "$AUTO_STREAM_AFTER_CONFIGURE" -eq 1 ] || return 1
    [ -n "$PSRP_HOST" ] || return 1
    psrp_find_registration
    if [ "$PSRP_REGISTERED" -eq 1 ] || psrp_load_direct_keys; then
        echo "launch_ps_remote_play: registration detected; continuing into stream"
        trap - EXIT INT TERM HUP
        exec bash "${BASH_SOURCE[0]}" stream
    fi
    return 1
}

X_ARGS=("$DISPLAY_NO" "$VT" -nolisten tcp -nocursor -s 0 -dpms)
if command -v xinit >/dev/null 2>&1; then
    xinit "${PSRP_CHIAKI[@]}" "${CLIENT_ARGS[@]}" -- "${X_ARGS[@]}"
    client_rc=$?
    psrp_maybe_continue_into_stream || true
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

"${PSRP_CHIAKI[@]}" "${CLIENT_ARGS[@]}"
client_rc=$?
kill "$X_PID" 2>/dev/null || true
wait "$X_PID" 2>/dev/null || true
X_PID=""
psrp_maybe_continue_into_stream || true
exit "$client_rc"
