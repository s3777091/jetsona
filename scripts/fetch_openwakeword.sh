#!/bin/bash
# Fetch the pinned custom "Hey Nova" classifier used by openWakeWord.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JETSON_DIR="$(dirname "$SCRIPT_DIR")"
MODEL_DIR="$JETSON_DIR/assets/models/openwakeword"
MODEL="$MODEL_DIR/hey_nova.onnx"
MODEL_SHA256="ded9d18fb849086b411004a7d2413595c9601265759227e9a48101a9851ea198"
ARCHIVE_SHA256="4b062ba4ac21e1e0a2a0e1e922f4aa73e9d88ed2c677840de8257e932e686b2f"
ARCHIVE_URL="https://files.pythonhosted.org/packages/8b/5c/2a9f6fe06f83766a58affbc67fd9b30656f5787c32d9c6dfa97a58b164b7/private_assistant_comms_satellite-1.1.0.tar.gz"
ARCHIVE_MEMBER="private_assistant_comms_satellite-1.1.0/assets/wakeword_models/hey_nova.onnx"

mkdir -p "$MODEL_DIR"
if [ -f "$MODEL" ] &&
   printf '%s  %s\n' "$MODEL_SHA256" "$MODEL" | sha256sum -c - >/dev/null 2>&1; then
    echo "openWakeWord model: cached"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
curl --fail --location --silent --show-error "$ARCHIVE_URL" -o "$tmp/package.tar.gz"
printf '%s  %s\n' "$ARCHIVE_SHA256" "$tmp/package.tar.gz" | sha256sum -c -
tar -xOf "$tmp/package.tar.gz" "$ARCHIVE_MEMBER" > "$tmp/hey_nova.onnx"
printf '%s  %s\n' "$MODEL_SHA256" "$tmp/hey_nova.onnx" | sha256sum -c -
install -m 0644 "$tmp/hey_nova.onnx" "$MODEL"
echo "openWakeWord model: installed"
