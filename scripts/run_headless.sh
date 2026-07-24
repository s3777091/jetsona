#!/bin/bash
# Run a locally built Ekko Lite firmware headless (no display, no display
# manager to stop). Just loads config/secrets and launches the binary in the
# foreground -- Ctrl+C exits cleanly.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JETSON_DIR="$(dirname "$SCRIPT_DIR")"
if [ ! -f "$JETSON_DIR/CMakeLists.txt" ]; then
    echo "run_headless: repository root not found above $SCRIPT_DIR" >&2
    exit 1
fi

# shellcheck disable=SC1091
. "$SCRIPT_DIR/config_loader.sh"
jetson_load_config "${JETSON_CONFIG_FILE:-$JETSON_DIR/config.yaml}"
jetson_load_secrets "${JETSON_ENV_FILE:-$JETSON_DIR/.env}"

BUILD_DIR="${JETSON_BUILD_DIR:-$JETSON_DIR/build}"
case "$BUILD_DIR" in
    /*) ;;
    *) BUILD_DIR="$JETSON_DIR/$BUILD_DIR" ;;
esac

FW="${JETSON_FW_BIN:-$BUILD_DIR/jetson_fw}"
if [ ! -x "$FW" ]; then
    echo "run_headless: firmware binary not found: $FW" >&2
    echo "Build it first: bash scripts/build.sh" >&2
    exit 1
fi

echo "==> launching $FW (headless) -- Ctrl+C to stop"
cd "$JETSON_DIR"
exec "$FW" "$@"