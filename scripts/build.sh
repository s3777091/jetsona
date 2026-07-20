#!/bin/bash
# Configure and build from the repository root, regardless of the caller's cwd.
# This avoids accidentally asking CMake to use $HOME (which has no
# CMakeLists.txt) when the repository was cloned under a different name.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JETSON_DIR="$(dirname "$SCRIPT_DIR")"
# shellcheck disable=SC1091
. "$SCRIPT_DIR/config_loader.sh"
jetson_load_config "${JETSON_CONFIG_FILE:-$JETSON_DIR/config.yaml}"

BUILD_DIR="${JETSON_BUILD_DIR:-$JETSON_DIR/build}"
BACKEND="${JETSON_DISPLAY_BACKEND:-DRM}"
BUILD_JOBS="${BUILD_JOBS:-4}"

source_tree_sha256() {
    {
        git -C "$JETSON_DIR" rev-parse HEAD
        git -C "$JETSON_DIR" diff --no-ext-diff --binary --submodule=diff HEAD --
        git -C "$JETSON_DIR" ls-files --others --exclude-standard -z |
            while IFS= read -r -d '' rel; do
                [ -f "$JETSON_DIR/$rel" ] || continue
                printf 'untracked:%s\n' "$rel"
                sha256sum "$JETSON_DIR/$rel"
            done
        if [ -d "$JETSON_DIR/assets" ]; then
            find "$JETSON_DIR/assets" -type f -print0 | LC_ALL=C sort -z |
                xargs -0 -r sha256sum
        fi
    } | sha256sum | awk '{print $1}'
}

if [ ! -f "$JETSON_DIR/CMakeLists.txt" ]; then
    echo "build.sh: CMakeLists.txt not found at $JETSON_DIR" >&2
    echo "Run the build script from the cloned jetsona repository." >&2
    exit 1
fi

echo "==> Source:  $JETSON_DIR"
echo "==> Build:   $BUILD_DIR"
echo "==> Backend: $BACKEND"

# Fetch runtime assets from MinIO (S3) before building. ETag checks make this
# cheap after the first download while still detecting same-size icon changes.
# A normal build is strict; opt into a cached/offline build explicitly with
# JETSON_SKIP_ASSET_FETCH=1.
if [ "${JETSON_SKIP_ASSET_FETCH:-0}" != "1" ]; then
    JETSON_ASSET_FETCH_STRICT=1 bash "$SCRIPT_DIR/fetch_assets.sh"
fi

cmake -S "$JETSON_DIR" -B "$BUILD_DIR" \
    -DJETSON_DISPLAY_BACKEND="$BACKEND" "$@"
cmake --build "$BUILD_DIR" --parallel "$BUILD_JOBS"

# Receipt prevents install.sh from accidentally installing a binary left by a
# previous git revision when a later build failed or was skipped.
BUILD_GIT_HEAD="$(git -C "$JETSON_DIR" rev-parse HEAD 2>/dev/null || printf unknown)"
BUILD_SOURCE_SHA256="$(source_tree_sha256)"
BUILD_BINARY_SHA256="$(sha256sum "$BUILD_DIR/jetson_fw" | awk '{print $1}')"
RECEIPT="$BUILD_DIR/.jetsona-build-receipt"
{
    printf 'BUILD_GIT_HEAD=%s\n' "$BUILD_GIT_HEAD"
    printf 'BUILD_SOURCE_SHA256=%s\n' "$BUILD_SOURCE_SHA256"
    printf 'BUILD_BINARY_SHA256=%s\n' "$BUILD_BINARY_SHA256"
} > "$RECEIPT.tmp"
mv "$RECEIPT.tmp" "$RECEIPT"

echo "==> Built: $BUILD_DIR/jetson_fw"
