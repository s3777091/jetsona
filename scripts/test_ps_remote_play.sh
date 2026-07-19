#!/bin/bash
# Offline regression tests for the PS Remote Play state/config/launcher flow.
# No PS5, Xorg, network access, or real Chiaki binary is required.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMP_ROOT="$(mktemp -d)"

cleanup()
{
    # TMP_ROOT is created by mktemp above. Refuse to delete anything that does
    # not look like a dedicated temporary directory.
    case "$TMP_ROOT" in
        /tmp/*|"${TMPDIR:-/tmp}"/*) rm -rf -- "$TMP_ROOT" ;;
        *) echo "Refusing to clean unexpected path: $TMP_ROOT" >&2 ;;
    esac
}
trap cleanup EXIT

fail()
{
    echo "PS Remote Play test failed: $*" >&2
    exit 1
}

assert_line()
{
    local expected="$1" file="$2"
    grep -Fqx -- "$expected" "$file" || fail "missing '$expected' in $file"
}

FAKE_CHIAKI="$TMP_ROOT/chiaki-ng"
cat > "$FAKE_CHIAKI" <<'EOF'
#!/bin/bash
case " $* " in
    *" --help "*)
        echo "--exit-app-on-stream-exit"
        ;;
    *" list "*)
        # FAKE_CHIAKI_NO_LIST simulates a client with no registered console
        # in its own config.
        [ -n "${FAKE_CHIAKI_NO_LIST:-}" ] && exit 0
        # Upstream intentionally prints a space before the newline.
        printf 'Host: PS5 Living Room \n'
        ;;
    *" discover "*)
        echo "Host State: Ready"
        ;;
esac
EOF
chmod 700 "$FAKE_CHIAKI"

export CHIAKI_BIN="$FAKE_CHIAKI"
export PS_REMOTE_PLAY_HOME="$TMP_ROOT/home"
export PS_REMOTE_PLAY_STATE_FILE="$TMP_ROOT/ps-remote-play.conf"
export PS_REMOTE_PLAY_DRY_RUN=1
export PS_REMOTE_PLAY_RENDER_BACKEND=vulkan
export PS_REMOTE_PLAY_HW_DECODER=software
export PS_REMOTE_PLAY_INPUT_ROOT="$TMP_ROOT/dev-input"
export PS_REMOTE_PLAY_SYS_INPUT_ROOT="$TMP_ROOT/sys-input"
mkdir -p "$PS_REMOTE_PLAY_INPUT_ROOT" "$PS_REMOTE_PLAY_SYS_INPUT_ROOT"
# Do not let the offline test import the developer machine's real config.
JETSON_CONFIG_FILE="$TMP_ROOT/config.yaml"
: > "$JETSON_CONFIG_FILE"
export JETSON_CONFIG_FILE

CTL="$SCRIPT_DIR/ps_remote_play_ctl.sh"
LAUNCHER="$SCRIPT_DIR/launch_ps_remote_play.sh"
CONFIG_DIR="$PS_REMOTE_PLAY_HOME/.config/Chiaki"
DEFAULT_CONFIG="$CONFIG_DIR/Chiaki.conf"
ACTIVE_CONFIG="$CONFIG_DIR/Chiaki-Deck.conf"

# A fresh installation must be able to create Chiaki.conf before registration.
"$CTL" save --host 192.168.1.50 --preset smooth --passcode 1234
"$LAUNCHER" configure >/dev/null
assert_line 'resolution_local_ps5=540p' "$DEFAULT_CONFIG"
assert_line 'resolution_remote_ps5=540p' "$DEFAULT_CONFIG"
assert_line 'fps_local_ps5=60' "$DEFAULT_CONFIG"
assert_line 'fps_remote_ps5=60' "$DEFAULT_CONFIG"
assert_line 'codec_local_ps5=h264' "$DEFAULT_CONFIG"
assert_line 'bitrate_local_ps5=8000' "$DEFAULT_CONFIG"
assert_line 'hw_decoder=' "$DEFAULT_CONFIG"
assert_line 'render_backend=vulkan' "$DEFAULT_CONFIG"
assert_line 'window_type=Fullscreen' "$DEFAULT_CONFIG"

# Simulate a registration stored in a named QSettings profile. The state file
# deliberately contains a stale nickname; discovery must replace it with the
# exact registered nickname, including correct trimming of upstream output.
printf '\ncurrent_profile=Deck\n' >> "$DEFAULT_CONFIG"
cat > "$ACTIVE_CONFIG" <<'EOF'
[registered_hosts]
1\server_nickname=PS5 Living Room
1\rp_regist_key=@ByteArray(12345678\0)
EOF
"$CTL" save --host 192.168.1.50 --preset quality \
    --nickname Stale-Nickname --passcode 1234

status_output="$("$CTL" status)"
grep -Fqx 'installed=1' <<< "$status_output" || fail "Chiaki not detected"
grep -Fqx 'registered=1' <<< "$status_output" || fail "registration not detected"
grep -Fqx 'nickname=PS5 Living Room' <<< "$status_output" || \
    fail "registered nickname was not normalized"
grep -Fqx 'controller=0' <<< "$status_output" || fail "empty input tree reported a controller"

# Modern Bluetooth gamepads can expose only eventN (no optional /dev/input/jsN).
# BTN_GAMEPAD is bit 0x130. Kernel sysfs prints native-long words high-to-low.
mkdir -p "$PS_REMOTE_PLAY_SYS_INPUT_ROOT/event7/device/capabilities"
: > "$PS_REMOTE_PLAY_INPUT_ROOT/event7"
if [ "$(getconf LONG_BIT 2>/dev/null || echo 64)" = 32 ]; then
    printf '10000 0 0 0 0 0 0 0 0 0\n' > \
        "$PS_REMOTE_PLAY_SYS_INPUT_ROOT/event7/device/capabilities/key"
else
    printf '1000000000000 0 0 0 0\n' > \
        "$PS_REMOTE_PLAY_SYS_INPUT_ROOT/event7/device/capabilities/key"
fi
status_output="$("$CTL" status)"
grep -Fqx 'controller=1' <<< "$status_output" || \
    fail "evdev-only BTN_GAMEPAD controller was not detected"

stream_output="$("$LAUNCHER" stream)"
[[ "$stream_output" == *"--profile Deck"* ]] || fail "active profile not passed to CLI"
[[ "$stream_output" == *"stream PS5\\ Living\\ Room 192.168.1.50"* ]] || \
    fail "direct stream argv is incorrect"
[[ "$stream_output" == *"<redacted-passcode>"* ]] || fail "passcode was not redacted"
[[ "$stream_output" != *" 1234"* ]] || fail "passcode leaked in dry-run output"

# An extracted chiaki-ng AppImage can be launched through a wrapper next to
# squashfs-root. QtWebEngineProcess then needs the AppImage lib paths from the
# launcher environment or PSN Login crashes before loading Qt/NSS.
FAKE_XINIT_DIR="$TMP_ROOT/fake-xinit-bin"
mkdir -p "$FAKE_XINIT_DIR"
cat > "$FAKE_XINIT_DIR/xinit" <<'EOF'
#!/bin/bash
client=()
while [ "$#" -gt 0 ] && [ "$1" != "--" ]; do
    client+=("$1")
    shift
done
"${client[@]}"
EOF
chmod 700 "$FAKE_XINIT_DIR/xinit"

EXTRACTED_CHIAKI_DIR="$TMP_ROOT/extracted-chiaki"
EXTRACTED_APPDIR="$EXTRACTED_CHIAKI_DIR/squashfs-root"
mkdir -p "$EXTRACTED_APPDIR/usr/lib" \
    "$EXTRACTED_APPDIR/usr/lib/aarch64-linux-gnu/nss" \
    "$EXTRACTED_APPDIR/usr/libexec"
: > "$EXTRACTED_APPDIR/usr/libexec/QtWebEngineProcess"
chmod 700 "$EXTRACTED_APPDIR/usr/libexec/QtWebEngineProcess"
cat > "$EXTRACTED_CHIAKI_DIR/chiaki-ng" <<'EOF'
#!/bin/bash
case ":${LD_LIBRARY_PATH:-}:" in
    *":$EXPECTED_APPDIR/usr/lib:"*) ;;
    *) echo "missing AppImage usr/lib in LD_LIBRARY_PATH" >&2; exit 42 ;;
esac
case ":${LD_LIBRARY_PATH:-}:" in
    *":$EXPECTED_APPDIR/usr/lib/aarch64-linux-gnu/nss:"*) ;;
    *) echo "missing AppImage NSS dir in LD_LIBRARY_PATH" >&2; exit 43 ;;
esac
[ "${QTWEBENGINEPROCESS_PATH:-}" = "$EXPECTED_APPDIR/usr/libexec/QtWebEngineProcess" ] || {
    echo "missing QtWebEngineProcess path" >&2
    exit 44
}
: > "$EXPECTED_MARKER"
EOF
chmod 700 "$EXTRACTED_CHIAKI_DIR/chiaki-ng"
env PATH="$FAKE_XINIT_DIR:$PATH" \
    CHIAKI_BIN="$EXTRACTED_CHIAKI_DIR/chiaki-ng" \
    PS_REMOTE_PLAY_DRY_RUN=0 \
    EXPECTED_APPDIR="$EXTRACTED_APPDIR" \
    EXPECTED_MARKER="$TMP_ROOT/extracted-runtime-ok" \
    "$LAUNCHER" configure >/dev/null
[[ -e "$TMP_ROOT/extracted-runtime-ok" ]] || \
    fail "extracted Chiaki runtime was not launched"

# Applying a new preset must update both the default file and active profile.
(
    # shellcheck source=ps_remote_play_ctl.sh
    . "$CTL"
    psrp_apply_preset quality
)
for config in "$DEFAULT_CONFIG" "$ACTIVE_CONFIG"; do
    assert_line 'resolution_local_ps5=720p' "$config"
    assert_line 'resolution_remote_ps5=720p' "$config"
    assert_line 'fps_local_ps5=30' "$config"
    assert_line 'fps_remote_ps5=30' "$config"
    assert_line 'bitrate_local_ps5=10000' "$config"
done

# State is parsed as allowlisted data and must never be sourced as shell code.
marker="$TMP_ROOT/must-not-exist"
printf 'host=192.168.1.50\nnickname=$(touch %s)\npreset=smooth\npasscode=\n' \
    "$marker" > "$PS_REMOTE_PLAY_STATE_FILE"
(
    # shellcheck source=ps_remote_play_ctl.sh
    . "$CTL"
    psrp_load_state
    [[ "$PSRP_NICKNAME" == '$(touch '* ]] || fail "state text was altered"
)
[[ ! -e "$marker" ]] || fail "state file content was executed"

echo "PS Remote Play tests: PASS"
