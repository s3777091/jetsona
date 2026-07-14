#!/bin/bash
# Configure and build from the repository root, regardless of the caller's cwd.
# This avoids accidentally asking CMake to use $HOME (which has no
# CMakeLists.txt) when the repository was cloned under a different name.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JETSON_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${JETSON_BUILD_DIR:-$JETSON_DIR/build}"
BACKEND="${JETSON_DISPLAY_BACKEND:-DRM}"
BUILD_JOBS="${BUILD_JOBS:-4}"

if [ ! -f "$JETSON_DIR/CMakeLists.txt" ]; then
    echo "build.sh: CMakeLists.txt not found at $JETSON_DIR" >&2
    echo "Run the build script from the cloned jetsona repository." >&2
    exit 1
fi

echo "==> Source:  $JETSON_DIR"
echo "==> Build:   $BUILD_DIR"
echo "==> Backend: $BACKEND"

cmake -S "$JETSON_DIR" -B "$BUILD_DIR" \
    -DJETSON_DISPLAY_BACKEND="$BACKEND" "$@"
cmake --build "$BUILD_DIR" --parallel "$BUILD_JOBS"

echo "==> Built: $BUILD_DIR/jetson_fw"
