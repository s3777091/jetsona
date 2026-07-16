#!/bin/bash
# State/probe helper for the PS Remote Play drawer app. This file is also
# sourced by launch_ps_remote_play.sh so state parsing and Chiaki discovery use
# one implementation. State files are data, never shell code.
set -u

PSRP_HOME="${PS_REMOTE_PLAY_HOME:-/var/lib/jetson-fw/chiaki}"
PSRP_STATE_FILE="${PS_REMOTE_PLAY_STATE_FILE:-/var/lib/jetson-fw/ps-remote-play.conf}"
PSRP_XDG_CONFIG_HOME="${PS_REMOTE_PLAY_XDG_CONFIG_HOME:-$PSRP_HOME/.config}"

# Keep QSettings deterministic even when the firmware service runs as root.
export HOME="$PSRP_HOME"
export XDG_CONFIG_HOME="$PSRP_XDG_CONFIG_HOME"

PSRP_HOST=""
PSRP_NICKNAME=""
PSRP_PRESET="smooth"
PSRP_PASSCODE=""
PSRP_CHIAKI=()
PSRP_CHIAKI_IS_APPIMAGE=0
PSRP_APPIMAGE_EXTRACT=0
PSRP_CLI_OUTPUT=""
PSRP_DEFAULT_CONFIG=""
PSRP_ACTIVE_CONFIG=""
PSRP_REGISTERED=0
PSRP_REGISTERED_NICKNAME=""
PSRP_REGIST_KEY=""
PSRP_PROBE_STATE="unknown"
PSRP_PROBE_MESSAGE=""

psrp_usage()
{
    cat >&2 <<'EOF'
Usage:
  ps_remote_play_ctl.sh status
  ps_remote_play_ctl.sh probe --host <address>
  ps_remote_play_ctl.sh save --host <address> --preset <smooth|quality>
                             [--nickname <name>] [--passcode <4 digits>]
EOF
}

psrp_is_true()
{
    case "${1:-}" in
        1|true|TRUE|yes|YES|on|ON) return 0 ;;
        *) return 1 ;;
    esac
}

psrp_valid_host()
{
    local value="${1:-}"
    [ -n "$value" ] &&
        [ "${#value}" -le 253 ] &&
        [[ "$value" != -* ]] &&
        [[ "$value" =~ ^[A-Za-z0-9:.][A-Za-z0-9._:-]*$ ]]
}

psrp_valid_text()
{
    local value="${1:-}"
    local max_len="${2:-128}"
    [ "${#value}" -le "$max_len" ] || return 1
    [[ "$value" != *$'\n'* && "$value" != *$'\r'* ]] || return 1
    # Reject all ASCII control characters. Spaces and UTF-8 are allowed.
    [[ "$value" != *[$'\001'-$'\010'$'\013'$'\014'$'\016'-$'\037'$'\177']* ]]
}

psrp_ensure_dirs()
{
    local state_dir runtime_dir
    state_dir="$(dirname "$PSRP_STATE_FILE")"
    runtime_dir="${PS_REMOTE_PLAY_XDG_RUNTIME_DIR:-$PSRP_HOME/runtime}"
    umask 077
    mkdir -p "$PSRP_HOME" "$PSRP_XDG_CONFIG_HOME" "$state_dir" "$runtime_dir" || return 1
    chmod 700 "$PSRP_HOME" "$PSRP_XDG_CONFIG_HOME" "$runtime_dir" 2>/dev/null || true
}

psrp_load_state()
{
    local line key value
    PSRP_HOST=""
    PSRP_NICKNAME=""
    PSRP_PRESET="${PS_REMOTE_PLAY_DEFAULT_PRESET:-smooth}"
    PSRP_PASSCODE=""

    [ -r "$PSRP_STATE_FILE" ] || {
        case "$PSRP_PRESET" in smooth|quality) ;; *) PSRP_PRESET="smooth" ;; esac
        return 0
    }

    while IFS= read -r line || [ -n "$line" ]; do
        line="${line%$'\r'}"
        [[ "$line" == *=* ]] || continue
        key="${line%%=*}"
        value="${line#*=}"
        case "$key" in
            host) psrp_valid_host "$value" && PSRP_HOST="$value" ;;
            nickname) psrp_valid_text "$value" 128 && PSRP_NICKNAME="$value" ;;
            preset) case "$value" in smooth|quality) PSRP_PRESET="$value" ;; esac ;;
            passcode)
                if [ -z "$value" ] || [[ "$value" =~ ^[0-9]{4}$ ]]; then
                    PSRP_PASSCODE="$value"
                fi
                ;;
        esac
    done < "$PSRP_STATE_FILE"
}

psrp_save_state()
{
    local host="$1" nickname="$2" preset="$3" passcode="$4"
    local state_dir tmp

    psrp_valid_host "$host" || {
        echo "ps_remote_play_ctl: invalid host" >&2
        return 2
    }
    psrp_valid_text "$nickname" 128 || {
        echo "ps_remote_play_ctl: invalid nickname" >&2
        return 2
    }
    case "$preset" in
        smooth|quality) ;;
        *) echo "ps_remote_play_ctl: preset must be smooth or quality" >&2; return 2 ;;
    esac
    if [ -n "$passcode" ] && ! [[ "$passcode" =~ ^[0-9]{4}$ ]]; then
        echo "ps_remote_play_ctl: passcode must be empty or four digits" >&2
        return 2
    fi

    psrp_ensure_dirs || {
        echo "ps_remote_play_ctl: cannot create state directory" >&2
        return 1
    }
    state_dir="$(dirname "$PSRP_STATE_FILE")"
    tmp="$(mktemp "$state_dir/.ps-remote-play.conf.XXXXXX")" || return 1
    if ! {
        umask 077
        printf 'host=%s\nnickname=%s\npreset=%s\npasscode=%s\n' \
            "$host" "$nickname" "$preset" "$passcode" > "$tmp" &&
        chmod 600 "$tmp" &&
        mv -f "$tmp" "$PSRP_STATE_FILE" &&
        chmod 600 "$PSRP_STATE_FILE"
    }; then
        rm -f "$tmp"
        echo "ps_remote_play_ctl: failed to save state" >&2
        return 1
    fi
}

psrp_set_appimage_mode()
{
    local candidate="$1" requested="${PS_REMOTE_PLAY_APPIMAGE_EXTRACT:-auto}"
    PSRP_CHIAKI_IS_APPIMAGE=0
    PSRP_APPIMAGE_EXTRACT=0
    case "${candidate,,}" in
        *.appimage)
            PSRP_CHIAKI_IS_APPIMAGE=1
            case "$requested" in
                1|true|TRUE|yes|YES|on|ON) PSRP_APPIMAGE_EXTRACT=1 ;;
                0|false|FALSE|no|NO|off|OFF) PSRP_APPIMAGE_EXTRACT=0 ;;
                *) [ -r /dev/fuse ] || PSRP_APPIMAGE_EXTRACT=1 ;;
            esac
            ;;
    esac
}

psrp_resolve_chiaki()
{
    local requested="${CHIAKI_BIN:-${PS_REMOTE_PLAY_BIN:-}}"
    local candidate resolved
    PSRP_CHIAKI=()

    if [ -n "$requested" ]; then
        if [[ "$requested" == */* ]]; then
            [ -x "$requested" ] || return 1
            resolved="$requested"
        else
            resolved="$(command -v "$requested" 2>/dev/null || true)"
            [ -n "$resolved" ] || return 1
        fi
        PSRP_CHIAKI=("$resolved")
        psrp_set_appimage_mode "$resolved"
        return 0
    fi

    for candidate in chiaki-ng chiaki; do
        resolved="$(command -v "$candidate" 2>/dev/null || true)"
        if [ -n "$resolved" ]; then
            PSRP_CHIAKI=("$resolved")
            psrp_set_appimage_mode "$resolved"
            return 0
        fi
    done

    for candidate in \
        /opt/chiaki-ng/chiaki-ng \
        /opt/chiaki-ng/chiaki \
        /opt/jetson-fw/chiaki-ng \
        /opt/jetson-fw/chiaki; do
        [ -x "$candidate" ] || continue
        PSRP_CHIAKI=("$candidate")
        psrp_set_appimage_mode "$candidate"
        return 0
    done

    for candidate in \
        /opt/jetson-fw/[Cc]hiaki*.AppImage \
        /opt/chiaki-ng/[Cc]hiaki*.AppImage \
        /usr/local/bin/[Cc]hiaki*.AppImage \
        "$PSRP_HOME"/[Cc]hiaki*.AppImage; do
        [ -x "$candidate" ] || continue
        PSRP_CHIAKI=("$candidate")
        psrp_set_appimage_mode "$candidate"
        return 0
    done
    return 1
}

psrp_capture_cli()
{
    local seconds="$1" rc
    shift
    local prefix=()
    command -v timeout >/dev/null 2>&1 && prefix=(timeout "${seconds}s")

    if [ "$PSRP_APPIMAGE_EXTRACT" -eq 1 ]; then
        PSRP_CLI_OUTPUT="$("${prefix[@]}" env APPIMAGE_EXTRACT_AND_RUN=1 \
            QT_QPA_PLATFORM=offscreen SDL_AUDIODRIVER=dummy \
            "${PSRP_CHIAKI[@]}" "$@" 2>/dev/null)"
        rc=$?
    else
        PSRP_CLI_OUTPUT="$("${prefix[@]}" env QT_QPA_PLATFORM=offscreen \
            SDL_AUDIODRIVER=dummy "${PSRP_CHIAKI[@]}" "$@" 2>/dev/null)"
        rc=$?
    fi
    return "$rc"
}

psrp_run_cli_quiet()
{
    local seconds="$1"
    shift
    local prefix=()
    command -v timeout >/dev/null 2>&1 && prefix=(timeout "${seconds}s")
    if [ "$PSRP_APPIMAGE_EXTRACT" -eq 1 ]; then
        "${prefix[@]}" env APPIMAGE_EXTRACT_AND_RUN=1 QT_QPA_PLATFORM=offscreen \
            SDL_AUDIODRIVER=dummy "${PSRP_CHIAKI[@]}" "$@" >/dev/null 2>&1
    else
        "${prefix[@]}" env QT_QPA_PLATFORM=offscreen SDL_AUDIODRIVER=dummy \
            "${PSRP_CHIAKI[@]}" "$@" >/dev/null 2>&1
    fi
}

psrp_select_configs()
{
    local candidate line section="" key value profile=""
    if [ -n "${PS_REMOTE_PLAY_CONFIG_FILE:-}" ]; then
        PSRP_DEFAULT_CONFIG="$PS_REMOTE_PLAY_CONFIG_FILE"
    else
        PSRP_DEFAULT_CONFIG="$PSRP_XDG_CONFIG_HOME/Chiaki/Chiaki.conf"
        for candidate in \
            "$PSRP_XDG_CONFIG_HOME/Chiaki/Chiaki.conf" \
            "$PSRP_XDG_CONFIG_HOME/chiaki/Chiaki.conf"; do
            if [ -f "$candidate" ]; then
                PSRP_DEFAULT_CONFIG="$candidate"
                break
            fi
        done
    fi
    PSRP_ACTIVE_CONFIG=""

    [ -r "$PSRP_DEFAULT_CONFIG" ] || return 0
    while IFS= read -r line || [ -n "$line" ]; do
        line="${line%$'\r'}"
        case "$line" in
            \[*\]) section="${line#[}"; section="${section%]}"; continue ;;
        esac
        [ "$section" = "settings" ] || continue
        [[ "$line" == *=* ]] || continue
        key="${line%%=*}"
        value="${line#*=}"
        if [ "$key" = "current_profile" ]; then
            profile="$value"
            break
        fi
    done < "$PSRP_DEFAULT_CONFIG"

    if [ -n "$profile" ] && [ "${#profile}" -le 96 ] &&
        [[ "$profile" =~ ^[A-Za-z0-9][A-Za-z0-9._\ -]*$ ]]; then
        PSRP_ACTIVE_CONFIG="$(dirname "$PSRP_DEFAULT_CONFIG")/Chiaki-$profile.conf"
    fi
}

psrp_decode_qsettings_text()
{
    local value="$1"
    case "$value" in
        @String\(*) value="${value#@String(}"; value="${value%)}" ;;
    esac
    printf '%s' "$value"
}

psrp_find_registration()
{
    local line value config
    PSRP_REGISTERED=0
    PSRP_REGISTERED_NICKNAME=""

    if [ "${#PSRP_CHIAKI[@]}" -gt 0 ]; then
        psrp_capture_cli "${PS_REMOTE_PLAY_LIST_TIMEOUT:-5}" list || true
        while IFS= read -r line; do
            line="${line%$'\r'}"
            case "$line" in
                Host:*)
                    value="${line#Host:}"
                    value="${value#${value%%[![:space:]]*}}"
                    value="${value%${value##*[![:space:]]}}"
                    if [ -n "$value" ] && psrp_valid_text "$value" 128; then
                        PSRP_REGISTERED=1
                        PSRP_REGISTERED_NICKNAME="$value"
                        break
                    fi
                    ;;
            esac
        done <<< "$PSRP_CLI_OUTPUT"
    fi

    psrp_select_configs
    for config in "$PSRP_ACTIVE_CONFIG" "$PSRP_DEFAULT_CONFIG"; do
        [ -r "$config" ] || continue
        while IFS= read -r line || [ -n "$line" ]; do
            line="${line%$'\r'}"
            case "$line" in
                *"\\rp_regist_key="*) PSRP_REGISTERED=1 ;;
                *"\\server_nickname="*)
                    if [ -z "$PSRP_REGISTERED_NICKNAME" ]; then
                        value="${line#*=}"
                        value="$(psrp_decode_qsettings_text "$value")"
                        psrp_valid_text "$value" 128 && PSRP_REGISTERED_NICKNAME="$value"
                    fi
                    ;;
            esac
        done < "$config"
        [ "$PSRP_REGISTERED" -eq 1 ] && [ -n "$PSRP_REGISTERED_NICKNAME" ] && break
    done
}

psrp_extract_regist_key()
{
    local wanted_nickname="$1" config line value prefix wanted_prefix=""
    local fallback_value=""
    PSRP_REGIST_KEY=""
    psrp_select_configs

    for config in "$PSRP_ACTIVE_CONFIG" "$PSRP_DEFAULT_CONFIG"; do
        [ -r "$config" ] || continue
        wanted_prefix=""
        if [ -n "$wanted_nickname" ]; then
            while IFS= read -r line || [ -n "$line" ]; do
                line="${line%$'\r'}"
                case "$line" in
                    *"\\server_nickname="*)
                        value="$(psrp_decode_qsettings_text "${line#*=}")"
                        if [ "$value" = "$wanted_nickname" ]; then
                            wanted_prefix="${line%%server_nickname=*}"
                            break
                        fi
                        ;;
                esac
            done < "$config"
        fi

        while IFS= read -r line || [ -n "$line" ]; do
            line="${line%$'\r'}"
            case "$line" in
                *"\\rp_regist_key="*)
                    prefix="${line%%rp_regist_key=*}"
                    value="${line#*=}"
                    [ -n "$fallback_value" ] || fallback_value="$value"
                    if [ -z "$wanted_prefix" ] || [ "$prefix" = "$wanted_prefix" ]; then
                        fallback_value="$value"
                        break
                    fi
                    ;;
            esac
        done < "$config"
        [ -n "$fallback_value" ] && break
    done

    value="$fallback_value"
    case "$value" in
        @ByteArray\(*) value="${value#@ByteArray(}"; value="${value%)}" ;;
    esac
    value="${value%%\\*}"
    if [[ "$value" =~ ^[A-Fa-f0-9]{8,64}$ ]]; then
        PSRP_REGIST_KEY="$value"
        return 0
    fi
    return 1
}

psrp_probe_host()
{
    local host="$1" lowered
    PSRP_PROBE_STATE="offline"
    PSRP_PROBE_MESSAGE="PS5 not reachable"
    psrp_valid_host "$host" || {
        PSRP_PROBE_MESSAGE="Invalid PS5 address"
        return 2
    }
    if [ "${#PSRP_CHIAKI[@]}" -eq 0 ] && ! psrp_resolve_chiaki; then
        PSRP_PROBE_MESSAGE="Chiaki not installed"
        return 3
    fi

    psrp_capture_cli "${PS_REMOTE_PLAY_PROBE_TIMEOUT:-5}" discover -h "$host" || true
    lowered="$(printf '%s' "$PSRP_CLI_OUTPUT" | tr '[:upper:]' '[:lower:]')"
    if [[ "$lowered" == *standby* ]]; then
        PSRP_PROBE_STATE="standby"
        PSRP_PROBE_MESSAGE="PS5 is in rest mode"
        return 0
    fi
    if [[ "$lowered" == *ready* ]]; then
        PSRP_PROBE_STATE="ready"
        PSRP_PROBE_MESSAGE="PS5 is ready"
        return 0
    fi
    return 1
}

psrp_update_ini_file()
{
    local file="$1" resolution="$2" fps="$3" bitrate="$4" hw_decoder="$5"
    local render_backend="$6"
    local dir tmp input
    dir="$(dirname "$file")"
    mkdir -p "$dir" || return 1
    chmod 700 "$dir" 2>/dev/null || true
    tmp="$(mktemp "$dir/.Chiaki.conf.XXXXXX")" || return 1
    input="$file"
    [ -r "$input" ] || input=/dev/null

    if ! awk \
        -v resolution="$resolution" -v fps="$fps" -v bitrate="$bitrate" \
        -v hw_decoder="$hw_decoder" -v render_backend="$render_backend" '
        function emit_settings() {
            print "resolution_local_ps5=" resolution
            print "resolution_remote_ps5=" resolution
            print "fps_local_ps5=" fps
            print "fps_remote_ps5=" fps
            print "codec_local_ps5=h264"
            print "codec_remote_ps5=h264"
            print "bitrate_local_ps5=" bitrate
            print "bitrate_remote_ps5=" bitrate
            print "hw_decoder=" hw_decoder
            print "render_backend=" render_backend
            print "placebo_preset=fast"
            print "window_type=Fullscreen"
            # Legacy Chiaki reads the unsuffixed fields.
            print "resolution=" resolution
            print "fps=" fps
            print "codec=h264"
            print "bitrate=" bitrate
        }
        function is_managed(key) {
            return key == "resolution_local_ps5" || key == "resolution_remote_ps5" ||
                key == "fps_local_ps5" || key == "fps_remote_ps5" ||
                key == "codec_local_ps5" || key == "codec_remote_ps5" ||
                key == "bitrate_local_ps5" || key == "bitrate_remote_ps5" ||
                key == "hw_decoder" || key == "render_backend" ||
                key == "placebo_preset" || key == "window_type" ||
                key == "resolution" || key == "fps" || key == "codec" ||
                key == "bitrate"
        }
        BEGIN { in_settings=0; found_settings=0; emitted=0 }
        {
            clean=$0
            sub(/\r$/, "", clean)
            if (clean ~ /^\[[^]]+\]$/) {
                if (in_settings && !emitted) { emit_settings(); emitted=1 }
                in_settings=(clean == "[settings]")
                if (in_settings) found_settings=1
                print clean
                next
            }
            if (in_settings) {
                equal=index(clean, "=")
                if (equal > 0) {
                    key=substr(clean, 1, equal-1)
                    gsub(/^[ \t]+|[ \t]+$/, "", key)
                    if (is_managed(key)) next
                }
            }
            print clean
        }
        END {
            if (in_settings && !emitted) { emit_settings(); emitted=1 }
            if (!found_settings) {
                if (NR > 0) print ""
                print "[settings]"
                emit_settings()
            }
        }
    ' "$input" 2>/dev/null > "$tmp"; then
        rm -f "$tmp"
        return 1
    fi
    chmod 600 "$tmp" || { rm -f "$tmp"; return 1; }
    mv -f "$tmp" "$file" || { rm -f "$tmp"; return 1; }
    chmod 600 "$file" 2>/dev/null || true
}

psrp_apply_preset()
{
    local preset="$1" resolution fps bitrate decoder requested_decoder render_backend
    case "$preset" in
        smooth) resolution="540p"; fps="60"; bitrate="8000" ;;
        quality) resolution="720p"; fps="30"; bitrate="10000" ;;
        *) return 2 ;;
    esac

    requested_decoder="${PS_REMOTE_PLAY_HW_DECODER:-software}"
    case "$requested_decoder" in
        ""|software) decoder="" ;;
        auto|vulkan|vaapi|cuda) decoder="$requested_decoder" ;;
        *) echo "launch_ps_remote_play: invalid PS_REMOTE_PLAY_HW_DECODER" >&2; return 2 ;;
    esac
    render_backend="${PS_REMOTE_PLAY_RENDER_BACKEND:-vulkan}"
    case "$render_backend" in
        vulkan|opengl) ;;
        *) echo "launch_ps_remote_play: invalid PS_REMOTE_PLAY_RENDER_BACKEND" >&2; return 2 ;;
    esac

    psrp_ensure_dirs || return 1
    psrp_select_configs
    # Read current_profile before atomically replacing the default file.
    local active="$PSRP_ACTIVE_CONFIG"
    psrp_update_ini_file "$PSRP_DEFAULT_CONFIG" "$resolution" "$fps" "$bitrate" "$decoder" "$render_backend" || return 1
    if [ -n "$active" ]; then
        psrp_update_ini_file "$active" "$resolution" "$fps" "$bitrate" "$decoder" "$render_backend" || return 1
    fi
}

psrp_network_type()
{
    local iface=""
    if command -v ip >/dev/null 2>&1; then
        iface="$(ip -o route show default 2>/dev/null | awk 'NR == 1 { for (i=1; i<=NF; i++) if ($i == "dev") { print $(i+1); exit } }')"
    fi
    if [ -z "$iface" ]; then
        printf 'offline'
    elif [ -d "/sys/class/net/$iface/wireless" ] || [[ "$iface" == wl* ]]; then
        printf 'wifi'
    else
        printf 'ethernet'
    fi
}

psrp_controller_present()
{
    local device
    for device in /dev/input/js* /dev/input/by-id/*-joystick /dev/input/by-path/*-joystick; do
        [ -e "$device" ] && return 0
    done
    return 1
}

psrp_cmd_save()
{
    local host="" preset="" nickname="" passcode=""
    local nickname_set=0 passcode_set=0
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --host) [ "$#" -ge 2 ] || { psrp_usage; return 2; }; host="$2"; shift 2 ;;
            --preset) [ "$#" -ge 2 ] || { psrp_usage; return 2; }; preset="$2"; shift 2 ;;
            --nickname) [ "$#" -ge 2 ] || { psrp_usage; return 2; }; nickname="$2"; nickname_set=1; shift 2 ;;
            --passcode) [ "$#" -ge 2 ] || { psrp_usage; return 2; }; passcode="$2"; passcode_set=1; shift 2 ;;
            *) psrp_usage; return 2 ;;
        esac
    done
    [ -n "$host" ] && [ -n "$preset" ] || { psrp_usage; return 2; }

    psrp_load_state
    [ "$nickname_set" -eq 1 ] || nickname="$PSRP_NICKNAME"
    [ "$passcode_set" -eq 1 ] || passcode="$PSRP_PASSCODE"
    psrp_save_state "$host" "$nickname" "$preset" "$passcode"
}

psrp_cmd_probe()
{
    local host=""
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --host) [ "$#" -ge 2 ] || { psrp_usage; return 2; }; host="$2"; shift 2 ;;
            *) psrp_usage; return 2 ;;
        esac
    done
    [ -n "$host" ] || { psrp_usage; return 2; }
    psrp_resolve_chiaki || true
    psrp_probe_host "$host"
    local rc=$?
    printf 'state=%s\nmessage=%s\n' "$PSRP_PROBE_STATE" "$PSRP_PROBE_MESSAGE"
    return "$rc"
}

psrp_cmd_status()
{
    local installed=0 registered=0 controller=0 network state message nickname
    psrp_load_state
    psrp_resolve_chiaki && installed=1
    psrp_find_registration
    registered="$PSRP_REGISTERED"
    nickname="$PSRP_NICKNAME"
    [ -n "$nickname" ] || nickname="$PSRP_REGISTERED_NICKNAME"
    psrp_controller_present && controller=1
    network="$(psrp_network_type)"

    state="unknown"
    if [ "$installed" -eq 0 ]; then
        message="Chiaki not installed"
    elif [ "$registered" -eq 0 ]; then
        message="Open setup to register this PS5"
    elif [ -z "$PSRP_HOST" ]; then
        message="Choose the PS5 address"
    elif [ "$network" = "offline" ]; then
        state="offline"
        message="Network is offline"
    else
        psrp_probe_host "$PSRP_HOST" || true
        state="$PSRP_PROBE_STATE"
        message="$PSRP_PROBE_MESSAGE"
    fi

    printf 'installed=%s\n' "$installed"
    printf 'registered=%s\n' "$registered"
    printf 'nickname=%s\n' "$nickname"
    printf 'host=%s\n' "$PSRP_HOST"
    printf 'preset=%s\n' "$PSRP_PRESET"
    printf 'controller=%s\n' "$controller"
    printf 'network=%s\n' "$network"
    printf 'state=%s\n' "$state"
    printf 'message=%s\n' "$message"
    return 0
}

psrp_main()
{
    local command="${1:-}"
    [ "$#" -gt 0 ] && shift
    case "$command" in
        status) psrp_cmd_status "$@" ;;
        probe) psrp_cmd_probe "$@" ;;
        save) psrp_cmd_save "$@" ;;
        *) psrp_usage; return 2 ;;
    esac
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    psrp_main "$@"
fi
