#!/bin/bash
# Run the Jetson DS-02 firmware with setup from config.yaml and secrets from
# .env (gitignored), then exec the binary.
# Usage: ./run.sh            (uses default backend DRM)
#        ./run.sh --sdl       (SDL backend, for debugging without the panel)
#
# Explicit environment variables override config.yaml, which is useful for
# one-off display/backend debugging.

set -e
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# shellcheck disable=SC1091
. "$HERE/scripts/config_loader.sh"
jetson_load_config "${JETSON_CONFIG_FILE:-$HERE/config.yaml}"
jetson_load_secrets "${JETSON_ENV_FILE:-$HERE/.env}"

BUILD_DIR="${JETSON_BUILD_DIR:-$HERE/build}"
case "$BUILD_DIR" in
    /*) ;;
    *) BUILD_DIR="$HERE/$BUILD_DIR" ;;
esac
BIN="${JETSON_FW_BIN:-$BUILD_DIR/jetson_fw}"
if [ ! -x "$BIN" ]; then
    echo "run.sh: $BIN not found. Build first:" >&2
    echo "  bash $HERE/scripts/build.sh" >&2
    exit 1
fi

if [ "$1" = "--sdl" ]; then
    shift
    export JETSON_DISPLAY_BACKEND=SDL
    # SDL often needs a video driver; on a Tegra console try kmsdrm.
    : "${SDL_VIDEODRIVER:=kmsdrm}"
    export SDL_VIDEODRIVER
fi

WEBSEARCH=off
[ -n "${EXA_API_KEY:-}" ] && WEBSEARCH="exa:${EXA_SEARCH_TYPE:-fast}"
echo "run.sh: launching $BIN (model=${OLLAMA_MODEL:-unset}, websearch=$WEBSEARCH)"
exec "$BIN" "$@"
