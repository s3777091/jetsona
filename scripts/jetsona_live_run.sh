#!/bin/bash
# Launcher for the Gemini Live realtime sidecar.
#
# Unlike the openWakeWord sidecar this one needs the network -- it holds the
# WebSocket session -- but it still never touches ALSA. jetson_fw remains the
# only owner of the microphone and the speaker and streams PCM over the Unix
# socket below, so raw audio only leaves the device inside a session the wake
# word opened.
set -eu

# shellcheck disable=SC1091
. /opt/jetson-fw/scripts/config_loader.sh
jetson_load_config "${JETSON_CONFIG_FILE:-/opt/jetson-fw/config.yaml}"
jetson_load_secrets "${JETSON_ENV_FILE:-/opt/jetson-fw/.env}"

IMAGE="${JETSON_VOICE_RUNTIME_IMAGE:-jetsona/voice-runtime:oww-0.6-edge-7.2.8}"
SOCKET="${GEMINI_LIVE_SOCKET:-/run/jetsona-live/realtime.sock}"
SOCKET_DIR="$(dirname "$SOCKET")"
MODEL="${GEMINI_LIVE_MODEL:-gemini-2.5-flash-native-audio-preview-12-2025}"
VOICE="${GEMINI_LIVE_VOICE:-Aoede}"

if [ -z "${GEMINI_API_KEY:-}" ]; then
    echo "GEMINI_API_KEY chưa có trong .env; sidecar không thể chạy" >&2
    exit 1
fi

mkdir -p "$SOCKET_DIR"
# A socket left behind by a killed container makes bind() fail with EADDRINUSE.
rm -f "$SOCKET"

exec /usr/bin/docker run --rm --name jetsona-gemini-live \
    --network bridge \
    --read-only \
    --tmpfs /tmp:rw,noexec,nosuid,size=32m \
    --env "GEMINI_API_KEY=$GEMINI_API_KEY" \
    --mount type=bind,src=/opt/jetson-fw/scripts/gemini_live_runtime.py,dst=/app/gemini_live_runtime.py,readonly \
    --mount "type=bind,src=$SOCKET_DIR,dst=$SOCKET_DIR" \
    "$IMAGE" \
    /app/gemini_live_runtime.py \
    --socket "$SOCKET" \
    --model "$MODEL" \
    --voice "$VOICE"
