#!/bin/bash
# Run the Jetson DS-02 firmware with config from .env (gitignored).
# Source .env, export every KEY=VALUE, then exec the binary.
# Usage: ./run.sh            (uses default backend DRM)
#        ./run.sh --sdl       (SDL backend, for debugging without the panel)
#
# The firmware reads OLLAMA_* / LIGHTPANDA_* from its process environment
# (LlmClient + WebSearchTool). .env holds them so they never enter git.

set -e
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

ENV_FILE="$HERE/.env"
if [ ! -f "$ENV_FILE" ]; then
    echo "run.sh: $ENV_FILE not found. Copy .env.example to .env and fill it in." >&2
    exit 1
fi

# `set -a` makes every subsequent variable assignment an export, so sourcing
# .env exports all KEY=VALUE lines into the environment.
set -a
. "$ENV_FILE"
set +a

BIN="$HERE/build/jetson_fw"
if [ ! -x "$BIN" ]; then
    echo "run.sh: $BIN not found. Build first:" >&2
    echo "  cd $HERE && mkdir -p build && cd build && cmake .. && make -j4" >&2
    exit 1
fi

if [ "$1" = "--sdl" ]; then
    shift
    export JETSON_DISPLAY_BACKEND=SDL
    # SDL often needs a video driver; on a Tegra console try kmsdrm.
    : "${SDL_VIDEODRIVER:=kmsdrm}"
    export SDL_VIDEODRIVER
fi

echo "run.sh: launching $BIN (model=${OLLAMA_MODEL:-unset}, websearch=${LIGHTPANDA_SEARCH_URL:-off})"
exec "$BIN" "$@"