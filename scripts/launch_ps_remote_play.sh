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
PSRP_CHIAKI_LOADER=""
PSRP_CHIAKI_REAL_BIN=""
PSRP_CHIAKI_QTWEBENGINEPROCESS_PATH="${QTWEBENGINEPROCESS_PATH:-}"
PSRP_BROWSER_LD_LIBRARY_PATH=""
PSRP_BROWSER_RUN_USER=""
PSRP_BROWSER_HOME=""
PSRP_BROWSER_PROFILE=""
PSRP_BROWSER_CACHE=""
PSRP_SESSION_WINDOW_MANAGER=""
PSRP_SESSION_ONBOARD=""
PSRP_SESSION_WRAPPER=""
PSRP_CHIAKI_RUNTIME_WRAPPER=""
PSRP_QTWEBENGINEPROCESS_WRAPPER=""
PSRP_BROWSER_WRAPPER=""

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

psrp_append_chiaki_library_path()
{
    local dir="$1"
    [ -d "$dir" ] || return 0
    case ":$PSRP_CHIAKI_LD_LIBRARY_PATH:" in
        *":$dir:"*) return 0 ;;
    esac
    if [ -n "$PSRP_CHIAKI_LD_LIBRARY_PATH" ]; then
        PSRP_CHIAKI_LD_LIBRARY_PATH="$PSRP_CHIAKI_LD_LIBRARY_PATH:$dir"
    else
        PSRP_CHIAKI_LD_LIBRARY_PATH="$dir"
    fi
}

psrp_prepend_browser_library_path()
{
    local dir="$1"
    [ -d "$dir" ] || return 0
    case ":$PSRP_BROWSER_LD_LIBRARY_PATH:" in
        *":$dir:"*) return 0 ;;
    esac
    if [ -n "$PSRP_BROWSER_LD_LIBRARY_PATH" ]; then
        PSRP_BROWSER_LD_LIBRARY_PATH="$dir:$PSRP_BROWSER_LD_LIBRARY_PATH"
    else
        PSRP_BROWSER_LD_LIBRARY_PATH="$dir"
    fi
}

psrp_configure_chiaki_runtime()
{
    local bin dir probe appdir loader main_bin nss_dir system_dir
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
        if [ -f "$appdir/usr/libexec/QtWebEngineProcess" ] &&
            [ -z "$PSRP_CHIAKI_QTWEBENGINEPROCESS_PATH" ]; then
            PSRP_CHIAKI_QTWEBENGINEPROCESS_PATH="$appdir/usr/libexec/QtWebEngineProcess"
        fi

        # JetPack 4 / Ubuntu 18.04 cannot load current chiaki-ng releases with
        # its system glibc. Local installations can provide a newer sysroot and
        # start the main Chiaki binary through its dynamic loader. QtWebEngine
        # is a separate executable, so merely setting LD_LIBRARY_PATH makes it
        # fall back to the old system loader and crash. Detect the companion
        # loader and use it for QtWebEngineProcess as well.
        for loader in \
            "$dir/sysroot/root/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1" \
            "$dir/sysroot/root/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1"; do
            [ -x "$loader" ] || continue
            PSRP_CHIAKI_LOADER="$loader"
            psrp_prepend_chiaki_library_path "$(dirname "$loader")"
            break
        done
        if [ -n "$PSRP_CHIAKI_LOADER" ]; then
            for main_bin in "$appdir/usr/bin/chiaki" "$appdir/usr/bin/chiaki-ng"; do
                [ -x "$main_bin" ] || continue
                PSRP_CHIAKI_REAL_BIN="$main_bin"
                break
            done
        fi
    fi

    if [ -n "${PS_REMOTE_PLAY_NSS_LIBRARY_PATH:-}" ]; then
        psrp_prepend_chiaki_library_path "$PS_REMOTE_PLAY_NSS_LIBRARY_PATH"
        [ ! -f "$PS_REMOTE_PLAY_NSS_LIBRARY_PATH/libsoftokn3.so" ] || \
            psrp_prepend_browser_library_path "$PS_REMOTE_PLAY_NSS_LIBRARY_PATH"
    fi
    for nss_dir in \
        /usr/lib/aarch64-linux-gnu/nss \
        /usr/lib/arm-linux-gnueabihf/nss \
        /usr/lib/x86_64-linux-gnu/nss \
        /usr/lib/nss; do
        psrp_prepend_chiaki_library_path "$nss_dir"
        # Chromium loads libsoftokn3.so by basename. These NSS module folders
        # are not always in ld.so's default search path (notably on Bionic).
        # Keep this system-only path separate from the AppImage libraries.
        [ ! -f "$nss_dir/libsoftokn3.so" ] || \
            psrp_prepend_browser_library_path "$nss_dir"
    done
    # Preserve the Tegra/system fallbacks used by local JetPack-compatible
    # wrappers, while keeping the companion glibc and AppImage libraries first.
    for system_dir in \
        /usr/lib/aarch64-linux-gnu \
        /lib/aarch64-linux-gnu \
        /usr/lib/aarch64-linux-gnu/tegra; do
        psrp_append_chiaki_library_path "$system_dir"
    done
}

psrp_configure_chiaki_runtime

psrp_hardware_keyboard_present()
{
    local input_root="${PS_REMOTE_PLAY_INPUT_ROOT:-/dev/input}"
    local device
    for device in \
        "$input_root"/by-id/*-event-kbd \
        "$input_root"/by-path/*-event-kbd; do
        [ -e "$device" ] && return 0
    done
    return 1
}

psrp_resolve_session_helpers()
{
    local requested
    [ "$MODE" = "configure" ] || return 0

    requested="${PS_REMOTE_PLAY_WINDOW_MANAGER:-auto}"
    case "$requested" in
        0|false|FALSE|no|NO|off|OFF|none) ;;
        auto) PSRP_SESSION_WINDOW_MANAGER="$(command -v openbox 2>/dev/null || true)" ;;
        *)
            if [[ "$requested" == */* ]]; then
                [ ! -x "$requested" ] || PSRP_SESSION_WINDOW_MANAGER="$requested"
            else
                PSRP_SESSION_WINDOW_MANAGER="$(command -v "$requested" 2>/dev/null || true)"
            fi
            [ -n "$PSRP_SESSION_WINDOW_MANAGER" ] || \
                echo "launch_ps_remote_play: window manager '$requested' not found" >&2
            ;;
    esac

    requested="${PS_REMOTE_PLAY_ONSCREEN_KEYBOARD:-auto}"
    case "$requested" in
        0|false|FALSE|no|NO|off|OFF|none) ;;
        auto)
            if ! psrp_hardware_keyboard_present; then
                PSRP_SESSION_ONBOARD="$(command -v onboard 2>/dev/null || true)"
            fi
            ;;
        1|true|TRUE|yes|YES|on|ON)
            PSRP_SESSION_ONBOARD="$(command -v onboard 2>/dev/null || true)"
            [ -n "$PSRP_SESSION_ONBOARD" ] || \
                echo "launch_ps_remote_play: onboard keyboard not installed" >&2
            ;;
        *) echo "launch_ps_remote_play: invalid PS_REMOTE_PLAY_ONSCREEN_KEYBOARD" >&2 ;;
    esac

    requested="${PS_REMOTE_PLAY_BROWSER_USER-jetson-kiosk}"
    if [ -n "$requested" ] && id "$requested" >/dev/null 2>&1 &&
        command -v runuser >/dev/null 2>&1 &&
        command -v dbus-run-session >/dev/null 2>&1; then
        PSRP_BROWSER_RUN_USER="$requested"
        PSRP_BROWSER_HOME="${PS_REMOTE_PLAY_BROWSER_HOME:-/var/lib/jetson-fw/chromium-home}"
        PSRP_BROWSER_PROFILE="${PS_REMOTE_PLAY_BROWSER_PROFILE:-/var/lib/jetson-fw/chromium-profile}"
        PSRP_BROWSER_CACHE="${PS_REMOTE_PLAY_BROWSER_CACHE:-/tmp/chromium-kiosk-cache}"
        # install.sh creates these with the kiosk account. Fall back to the
        # root browser path if an incomplete installation cannot write them.
        if [ ! -d "$PSRP_BROWSER_HOME" ] || [ ! -d "$PSRP_BROWSER_PROFILE" ]; then
            PSRP_BROWSER_RUN_USER=""
        fi
    fi
}

psrp_resolve_session_helpers

psrp_write_runtime_wrappers()
{
    PSRP_SESSION_WRAPPER="$RUNTIME_DIR/psrp-x-session"
    cat > "$PSRP_SESSION_WRAPPER" <<'EOF'
#!/bin/bash
set -u
mode="${1:-}"
[ "$#" -gt 0 ] && shift
wm_pid=""
onboard_pid=""
app_pid=""

psrp_session_cleanup()
{
    trap - EXIT INT TERM HUP
    [ -z "$onboard_pid" ] || kill "$onboard_pid" 2>/dev/null || true
    [ -z "$wm_pid" ] || kill "$wm_pid" 2>/dev/null || true
    [ -z "$app_pid" ] || kill "$app_pid" 2>/dev/null || true
    [ -z "$onboard_pid" ] || wait "$onboard_pid" 2>/dev/null || true
    [ -z "$wm_pid" ] || wait "$wm_pid" 2>/dev/null || true
}
trap psrp_session_cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

if [ "$mode" = "configure" ]; then
    command -v xsetroot >/dev/null 2>&1 && xsetroot -solid black >/dev/null 2>&1 || true
    if command -v setxkbmap >/dev/null 2>&1; then
        setxkbmap -option '' -option terminate:ctrl_alt_bksp >/dev/null 2>&1 || true
    fi
    if [ -n "${PSRP_SESSION_BROWSER_USER:-}" ] && command -v xhost >/dev/null 2>&1; then
        xhost "+SI:localuser:$PSRP_SESSION_BROWSER_USER" >/dev/null 2>&1 || true
    fi
    if [ -n "${PSRP_SESSION_WINDOW_MANAGER:-}" ] &&
        [ -x "$PSRP_SESSION_WINDOW_MANAGER" ]; then
        "$PSRP_SESSION_WINDOW_MANAGER" --sm-disable &
        wm_pid=$!
        sleep 0.25
    fi
fi

"$@" &
app_pid=$!
if [ "$mode" = "configure" ] && [ -n "${PSRP_SESSION_ONBOARD:-}" ] &&
    [ -x "$PSRP_SESSION_ONBOARD" ]; then
    sleep 0.5
    if command -v gsettings >/dev/null 2>&1; then
        # Onboard defaults to a full-screen dock whose transparent area turns
        # black without a compositor. Floating mode keeps the X window exactly
        # on the 205px keyboard and leaves Chiaki visible above it.
        gsettings set org.onboard.window docking-enabled false >/dev/null 2>&1 || true
        gsettings set org.onboard.window window-decoration false >/dev/null 2>&1 || true
        gsettings set org.onboard.window force-to-top true >/dev/null 2>&1 || true
    fi
    "$PSRP_SESSION_ONBOARD" --size=800x205 -x 0 -y 275 \
        --layout=Compact --theme=Nightshade &
    onboard_pid=$!
fi
wait "$app_pid"
app_rc=$?
app_pid=""
exit "$app_rc"
EOF
    chmod 700 "$PSRP_SESSION_WRAPPER" 2>/dev/null || true

    if [ -n "$PSRP_CHIAKI_LOADER" ] && [ -n "$PSRP_CHIAKI_REAL_BIN" ]; then
        PSRP_CHIAKI_RUNTIME_WRAPPER="$RUNTIME_DIR/chiaki-ng"
        cat > "$PSRP_CHIAKI_RUNTIME_WRAPPER" <<'EOF'
#!/bin/bash
if [ -n "${PSRP_RUNTIME_CHIAKI_APPDIR:-}" ]; then
    export APPDIR="$PSRP_RUNTIME_CHIAKI_APPDIR"
    export QT_PLUGIN_PATH="$PSRP_RUNTIME_CHIAKI_APPDIR/usr/plugins"
    export QML_IMPORT_PATH="$PSRP_RUNTIME_CHIAKI_APPDIR/usr/qml"
    export QML2_IMPORT_PATH="$PSRP_RUNTIME_CHIAKI_APPDIR/usr/qml"
fi
# The main Qt process initializes Chromium/NSS before it starts a separate
# QtWebEngineProcess. Bypass compatibility launchers whose fixed --library-path
# omits the NSS module directory, and give both processes one complete path.
unset LD_LIBRARY_PATH
exec "$PSRP_RUNTIME_CHIAKI_LOADER" \
    --library-path "$PSRP_RUNTIME_CHIAKI_LD_LIBRARY_PATH" \
    "$PSRP_RUNTIME_CHIAKI_REAL_BIN" "$@"
EOF
        chmod 700 "$PSRP_CHIAKI_RUNTIME_WRAPPER" 2>/dev/null || true
    fi

    if [ -n "$PSRP_CHIAKI_QTWEBENGINEPROCESS_PATH" ]; then
        PSRP_QTWEBENGINEPROCESS_WRAPPER="$RUNTIME_DIR/QtWebEngineProcess"
        cat > "$PSRP_QTWEBENGINEPROCESS_WRAPPER" <<'EOF'
#!/bin/bash
if [ -n "${PSRP_QTWEBENGINE_APPDIR:-}" ]; then
    export APPDIR="$PSRP_QTWEBENGINE_APPDIR"
fi
if [ -n "${PSRP_QTWEBENGINE_LOADER:-}" ]; then
    # QtWebEngine re-executes QTWEBENGINEPROCESS_PATH for its child processes.
    # Do not export the newer sysroot through LD_LIBRARY_PATH here: the shell
    # wrapper itself uses the host /bin/bash and would then mix the host loader
    # with the companion libc. The explicit loader path is inherited safely via
    # PSRP_* and each child comes back through this wrapper.
    unset LD_LIBRARY_PATH
    exec "$PSRP_QTWEBENGINE_LOADER" \
        --library-path "$PSRP_QTWEBENGINE_LD_LIBRARY_PATH" \
        "$PSRP_REAL_QTWEBENGINEPROCESS" "$@"
fi
if [ -n "${PSRP_QTWEBENGINE_LD_LIBRARY_PATH:-}" ]; then
    export LD_LIBRARY_PATH="$PSRP_QTWEBENGINE_LD_LIBRARY_PATH"
fi
exec "$PSRP_REAL_QTWEBENGINEPROCESS" "$@"
EOF
        chmod 700 "$PSRP_QTWEBENGINEPROCESS_WRAPPER" 2>/dev/null || true
    fi

    PSRP_BROWSER_WRAPPER="$RUNTIME_DIR/psrp-browser"
    cat > "$PSRP_BROWSER_WRAPPER" <<'EOF'
#!/bin/bash
browser_ld_library_path="${PSRP_BROWSER_LD_LIBRARY_PATH:-}"
browser_run_user="${PSRP_BROWSER_RUN_USER:-}"
browser_home="${PSRP_BROWSER_HOME:-}"
browser_profile="${PSRP_BROWSER_PROFILE:-}"
browser_cache="${PSRP_BROWSER_CACHE:-}"
browser_display="${DISPLAY:-:0}"
unset APPDIR LD_LIBRARY_PATH QTWEBENGINEPROCESS_PATH
unset PSRP_REAL_QTWEBENGINEPROCESS PSRP_QTWEBENGINE_APPDIR PSRP_QTWEBENGINE_LD_LIBRARY_PATH
unset PSRP_QTWEBENGINE_LOADER PSRP_BROWSER_LD_LIBRARY_PATH PSRP_BROWSER_RUN_USER
unset PSRP_BROWSER_HOME PSRP_BROWSER_PROFILE PSRP_BROWSER_CACHE
unset PSRP_RUNTIME_CHIAKI_APPDIR PSRP_RUNTIME_CHIAKI_LOADER
unset PSRP_RUNTIME_CHIAKI_LD_LIBRARY_PATH PSRP_RUNTIME_CHIAKI_REAL_BIN
if [ -n "$browser_ld_library_path" ]; then
    export LD_LIBRARY_PATH="$browser_ld_library_path"
fi

psrp_psn_redirect_requested()
{
    local arg
    case "${PS_REMOTE_PLAY_PSN_REDIRECT_AUTO:-1}" in
        0|false|FALSE|no|NO|off|OFF) return 1 ;;
    esac
    for arg in "$@"; do
        case "$arg" in
            https://auth.api.sonyentertainmentnetwork.com/*/oauth/authorize\?*)
                return 0
                ;;
        esac
    done
    return 1
}

psrp_extract_psn_redirect()
{
    python3 -c '
import json
import sys
from urllib.parse import parse_qsl, urlsplit

try:
    pages = json.load(sys.stdin)
except Exception:
    raise SystemExit(0)

for page in pages if isinstance(pages, list) else []:
    url = page.get("url", "") if isinstance(page, dict) else ""
    try:
        parsed = urlsplit(url)
    except ValueError:
        continue
    if (parsed.scheme == "https" and
            parsed.hostname == "remoteplay.dl.playstation.net" and
            parsed.path == "/remoteplay/redirect" and
            any(key == "code" and value
                for key, value in parse_qsl(parsed.query, keep_blank_values=True))):
        sys.stdout.write(url)
        break
'
}

psrp_deliver_psn_redirect()
{
    local redirect_url="$1" browser_windows="" focused_name=""
    local window attempt

    browser_windows="$(DISPLAY="$browser_display" \
        xdotool search --onlyvisible --class chromium 2>/dev/null || true)"
    for window in $browser_windows; do
        DISPLAY="$browser_display" xdotool windowclose "$window" >/dev/null 2>&1 || true
    done

    # Both PSNLoginDialog and PSNTokenDialog leave their Paste button focused
    # while the external browser is open. Left selects the adjacent URL field;
    # Menu activates DialogView's enabled submit action after the URL is typed.
    # Openbox focuses Chiaki's reparenting frame after the Chromium window is
    # closed. Searching/activating the client window itself can hang because
    # that frame owns X input focus, so verify the actual focused window name.
    for attempt in {1..20}; do
        focused_name="$(DISPLAY="$browser_display" \
            xdotool getwindowfocus getwindowname 2>/dev/null || true)"
        case "$focused_name" in
            *chiaki-ng*) break ;;
        esac
        sleep 0.1
    done
    case "$focused_name" in
        *chiaki-ng*) ;;
        *) return 1 ;;
    esac
    DISPLAY="$browser_display" xdotool key --clearmodifiers Left \
        >/dev/null 2>&1 || return 1
    DISPLAY="$browser_display" xdotool key --clearmodifiers ctrl+a \
        >/dev/null 2>&1 || return 1
    printf '%s' "$redirect_url" | DISPLAY="$browser_display" \
        xdotool type --clearmodifiers --delay 1 --file - \
        >/dev/null 2>&1 || return 1
    DISPLAY="$browser_display" xdotool key --clearmodifiers Menu \
        >/dev/null 2>&1 || return 1
}

psrp_monitor_psn_redirect()
{
    local browser_pid="$1" devtools_file="$browser_profile/DevToolsActivePort"
    local port="" ignored="" page_json="" redirect_url=""

    while kill -0 "$browser_pid" 2>/dev/null; do
        if [ -r "$devtools_file" ]; then
            IFS= read -r port ignored < "$devtools_file" || true
            case "$port" in
                ''|*[!0-9]*) port="" ;;
            esac
        fi
        if [ -n "$port" ]; then
            page_json="$(curl --silent --fail --max-time 1 \
                "http://127.0.0.1:$port/json/list" 2>/dev/null || true)"
            if [ -n "$page_json" ]; then
                redirect_url="$(printf '%s' "$page_json" | \
                    psrp_extract_psn_redirect 2>/dev/null || true)"
            fi
            if [ -n "$redirect_url" ] && psrp_deliver_psn_redirect "$redirect_url"; then
                echo "launch_ps_remote_play: PSN redirect returned to Chiaki" >&2
                return 0
            fi
        fi
        sleep 0.25
    done
    return 1
}

if [ -n "${PS_REMOTE_PLAY_BROWSER:-}" ] && [ -x "$PS_REMOTE_PLAY_BROWSER" ]; then
    exec "$PS_REMOTE_PLAY_BROWSER" "$@"
fi

resolved=""
for candidate in chromium-browser chromium google-chrome; do
    resolved="$(command -v "$candidate" 2>/dev/null || true)"
    [ -n "$resolved" ] || continue
    break
done
if [ -n "$resolved" ]; then
    if [ -n "$browser_run_user" ] && [ -n "$browser_home" ] &&
        [ -n "$browser_profile" ] && command -v runuser >/dev/null 2>&1 &&
        command -v dbus-run-session >/dev/null 2>&1; then
        runuser -u "$browser_run_user" -- rm -f \
            "$browser_profile/SingletonLock" \
            "$browser_profile/SingletonSocket" \
            "$browser_profile/SingletonCookie" \
            "$browser_profile/DevToolsActivePort" 2>/dev/null || true
        if psrp_psn_redirect_requested "$@" &&
            command -v curl >/dev/null 2>&1 &&
            command -v python3 >/dev/null 2>&1 &&
            command -v xdotool >/dev/null 2>&1; then
            runuser -u "$browser_run_user" -- env \
                -u DBUS_SESSION_BUS_ADDRESS -u DBUS_SYSTEM_BUS_ADDRESS \
                HOME="$browser_home" \
                XDG_CONFIG_HOME="$browser_home/.config" \
                XDG_CACHE_HOME="$browser_home/.cache" \
                XDG_RUNTIME_DIR="${PS_REMOTE_PLAY_BROWSER_RUNTIME:-/tmp/chromium-kiosk-runtime}" \
                DISPLAY="$browser_display" \
                LD_LIBRARY_PATH="$browser_ld_library_path" \
                dbus-run-session -- "$resolved" \
                --user-data-dir="$browser_profile" \
                --disk-cache-dir="$browser_cache" \
                --remote-debugging-address=127.0.0.1 --remote-debugging-port=0 \
                --disable-gpu --disable-software-rasterizer \
                --no-first-run --no-default-browser-check \
                --disable-session-crashed-bubble \
                --start-maximized --window-size=800,480 "$@" &
            browser_pid=$!
            if psrp_monitor_psn_redirect "$browser_pid"; then
                for attempt in {1..20}; do
                    kill -0 "$browser_pid" 2>/dev/null || break
                    sleep 0.1
                done
                kill "$browser_pid" 2>/dev/null || true
                wait "$browser_pid" 2>/dev/null || true
                exit 0
            fi
            wait "$browser_pid"
            exit $?
        fi
        exec runuser -u "$browser_run_user" -- env \
            -u DBUS_SESSION_BUS_ADDRESS -u DBUS_SYSTEM_BUS_ADDRESS \
            HOME="$browser_home" \
            XDG_CONFIG_HOME="$browser_home/.config" \
            XDG_CACHE_HOME="$browser_home/.cache" \
            XDG_RUNTIME_DIR="${PS_REMOTE_PLAY_BROWSER_RUNTIME:-/tmp/chromium-kiosk-runtime}" \
            DISPLAY="$browser_display" \
            LD_LIBRARY_PATH="$browser_ld_library_path" \
            dbus-run-session -- "$resolved" \
            --user-data-dir="$browser_profile" \
            --disk-cache-dir="$browser_cache" \
            --disable-gpu --disable-software-rasterizer \
            --no-first-run --no-default-browser-check \
            --disable-session-crashed-bubble \
            --start-maximized --window-size=800,480 "$@"
    fi
    exec "$resolved" --no-sandbox --disable-setuid-sandbox "$@"
fi

for candidate in www-browser x-www-browser; do
    resolved="$(command -v "$candidate" 2>/dev/null || true)"
    [ -n "$resolved" ] || continue
    exec "$resolved" "$@"
done

echo "launch_ps_remote_play: no browser available for Chiaki PSN login URL" >&2
exit 127
EOF
    chmod 700 "$PSRP_BROWSER_WRAPPER" 2>/dev/null || true
}

psrp_write_runtime_wrappers
PSRP_GUI_CHIAKI=("${PSRP_CHIAKI[@]}")
if [ -n "$PSRP_CHIAKI_RUNTIME_WRAPPER" ]; then
    PSRP_GUI_CHIAKI=("$PSRP_CHIAKI_RUNTIME_WRAPPER")
fi

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
    dry_command="${PSRP_GUI_CHIAKI[0]}"
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
if [ -n "$PSRP_CHIAKI_RUNTIME_WRAPPER" ]; then
    PSRP_CLIENT_ENV+=("PSRP_RUNTIME_CHIAKI_APPDIR=$PSRP_CHIAKI_APPDIR")
    PSRP_CLIENT_ENV+=("PSRP_RUNTIME_CHIAKI_LOADER=$PSRP_CHIAKI_LOADER")
    PSRP_CLIENT_ENV+=("PSRP_RUNTIME_CHIAKI_LD_LIBRARY_PATH=$PSRP_CHIAKI_LD_LIBRARY_PATH")
    PSRP_CLIENT_ENV+=("PSRP_RUNTIME_CHIAKI_REAL_BIN=$PSRP_CHIAKI_REAL_BIN")
fi
if [ -n "$PSRP_QTWEBENGINEPROCESS_WRAPPER" ]; then
    PSRP_CLIENT_ENV+=("QTWEBENGINEPROCESS_PATH=$PSRP_QTWEBENGINEPROCESS_WRAPPER")
    PSRP_CLIENT_ENV+=("PSRP_REAL_QTWEBENGINEPROCESS=$PSRP_CHIAKI_QTWEBENGINEPROCESS_PATH")
    PSRP_CLIENT_ENV+=("PSRP_QTWEBENGINE_APPDIR=$PSRP_CHIAKI_APPDIR")
    PSRP_CLIENT_ENV+=("PSRP_QTWEBENGINE_LD_LIBRARY_PATH=$PSRP_CHIAKI_LD_LIBRARY_PATH")
    PSRP_CLIENT_ENV+=("PSRP_QTWEBENGINE_LOADER=$PSRP_CHIAKI_LOADER")
fi
if [ -n "$PSRP_BROWSER_WRAPPER" ]; then
    PSRP_CLIENT_ENV+=("BROWSER=$PSRP_BROWSER_WRAPPER")
    PSRP_CLIENT_ENV+=("PSRP_BROWSER_LD_LIBRARY_PATH=$PSRP_BROWSER_LD_LIBRARY_PATH")
    PSRP_CLIENT_ENV+=("PSRP_BROWSER_RUN_USER=$PSRP_BROWSER_RUN_USER")
    PSRP_CLIENT_ENV+=("PSRP_BROWSER_HOME=$PSRP_BROWSER_HOME")
    PSRP_CLIENT_ENV+=("PSRP_BROWSER_PROFILE=$PSRP_BROWSER_PROFILE")
    PSRP_CLIENT_ENV+=("PSRP_BROWSER_CACHE=$PSRP_BROWSER_CACHE")
fi
PSRP_CLIENT_ENV+=("PSRP_SESSION_WINDOW_MANAGER=$PSRP_SESSION_WINDOW_MANAGER")
PSRP_CLIENT_ENV+=("PSRP_SESSION_ONBOARD=$PSRP_SESSION_ONBOARD")
PSRP_CLIENT_ENV+=("PSRP_SESSION_BROWSER_USER=$PSRP_BROWSER_RUN_USER")
PSRP_DBUS_RUN_SESSION="$(command -v dbus-run-session 2>/dev/null || true)"
if [ -n "$PSRP_DBUS_RUN_SESSION" ]; then
    # xinit only treats absolute/relative paths as the client program; a bare
    # command name falls through to its default xterm client.
    PSRP_CLIENT_CMD=("$PSRP_DBUS_RUN_SESSION" "$PSRP_ENV_BIN")
else
    echo "launch_ps_remote_play: dbus-run-session missing; PSN login webview may be unstable" >&2
    PSRP_CLIENT_CMD=("$PSRP_ENV_BIN")
fi

X_ARGS=("$DISPLAY_NO" "$VT" -nolisten tcp -s 0 -dpms)
if [ "$MODE" = "stream" ]; then
    X_ARGS+=(-nocursor)
fi

psrp_filter_xinit_stderr()
{
    local line
    while IFS= read -r line; do
        case "$line" in
            *"Simulate User Activity Error:"*"org.freedesktop.ScreenSaver"*) ;;
            *) printf '%s\n' "$line" >&2 ;;
        esac
    done
}

if command -v xinit >/dev/null 2>&1; then
    xinit "${PSRP_CLIENT_CMD[@]}" "${PSRP_CLIENT_ENV[@]}" \
        "$PSRP_SESSION_WRAPPER" "$MODE" \
        "${PSRP_GUI_CHIAKI[@]}" \
        "${CLIENT_ARGS[@]}" -- "${X_ARGS[@]}" \
        2> >(psrp_filter_xinit_stderr)
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
    "$PSRP_SESSION_WRAPPER" "$MODE" \
    "${PSRP_GUI_CHIAKI[@]}" "${CLIENT_ARGS[@]}" \
    2> >(psrp_filter_xinit_stderr)
client_rc=$?
kill "$X_PID" 2>/dev/null || true
wait "$X_PID" 2>/dev/null || true
X_PID=""
exit "$client_rc"
