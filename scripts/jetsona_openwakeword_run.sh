#!/bin/bash
# Launcher for the offline openWakeWord sidecar.
#
# The container image tag, the socket path and the wake model all live in
# config.yaml, so read them from there instead of repeating them in the unit
# file -- a unit that hardcodes the tag silently drifts the moment
# build_voice_runtime.sh bumps it.
#
# The worker gets no microphone device and no network: jetson_fw is the only
# ALSA owner and streams 80 ms PCM frames over the Unix socket below.
set -eu

# shellcheck disable=SC1091
. /opt/jetson-fw/scripts/config_loader.sh
jetson_load_config "${JETSON_CONFIG_FILE:-/opt/jetson-fw/config.yaml}"

IMAGE="${JETSON_VOICE_RUNTIME_IMAGE:-jetsona/voice-runtime:oww-0.6-edge-7.2.8}"
SOCKET="${OPENWAKEWORD_SOCKET:-/run/jetsona-voice/openwakeword.sock}"
SOCKET_DIR="$(dirname "$SOCKET")"
MODEL_NAME="${OPENWAKEWORD_MODEL:-hey_nova.onnx}"
VERIFIER_THRESHOLD="${OPENWAKEWORD_VERIFIER_THRESHOLD:-0.30}"

mkdir -p "$SOCKET_DIR"
# A socket left behind by a killed container makes bind() fail with EADDRINUSE.
rm -f "$SOCKET"

exec /usr/bin/docker run --rm --name jetsona-openwakeword \
    --network none \
    --read-only \
    --tmpfs /tmp:rw,noexec,nosuid,size=32m \
    --mount type=bind,src=/opt/jetson-fw/scripts/openwakeword_runtime.py,dst=/app/openwakeword_runtime.py,readonly \
    --mount type=bind,src=/opt/jetson-fw/assets/models/openwakeword,dst=/models,readonly \
    --mount type=bind,src=/var/lib/jetson-fw,dst=/state,readonly \
    --mount "type=bind,src=$SOCKET_DIR,dst=$SOCKET_DIR" \
    "$IMAGE" \
    /app/openwakeword_runtime.py \
    --socket "$SOCKET" \
    --model "/models/$MODEL_NAME" \
    --verifier /state/hey_nova_verifier.pkl \
    --verifier-threshold "$VERIFIER_THRESHOLD"
