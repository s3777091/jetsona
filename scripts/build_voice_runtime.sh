#!/bin/bash
# Build the ARM-native Python runtime used by openWakeWord and Edge TTS.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JETSON_DIR="$(dirname "$SCRIPT_DIR")"
IMAGE="${JETSON_VOICE_RUNTIME_IMAGE:-jetsona/voice-runtime:oww0.6-edge7.2.8-genai2.14}"

docker build --pull=false --tag "$IMAGE" "$JETSON_DIR/docker/voice-runtime"
docker image inspect "$IMAGE" --format 'voice runtime: {{.Id}} {{.Size}} bytes'
