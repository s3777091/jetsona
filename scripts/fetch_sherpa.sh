#!/bin/bash
# Build sherpa-onnx (v1.10.30) for the Jetson Nano B01 with the GPU EP, into a
# prefix the firmware CMake links via -DJETSON_SHERPA_DIR=<prefix>.
#
# Why a prefix instead of FetchContent every build: sherpa + onnxruntime is a
# long CUDA build; doing it once and reusing the .a keeps the firmware inner
# loop fast. The firmware CMake falls back to FetchContent if JETSON_SHERPA_DIR
# is empty, so this script is optional but recommended.
#
# Prereqs (JetPack 4.6.3 already provides CUDA 10.2 + cuDNN 8): build-essential,
# cmake >= 3.14, git. sherpa's CMake downloads a matching onnxruntime 1.11
# (CUDA build) for aarch64 itself.
#
# After this runs, build the firmware with:
#   cmake -DJETSON_SHERPA_DIR="$PREFIX" -DJETSON_SHERPA_EXTRA_LIBS="<ort>;<cuda>" ...
# This script prints the exact JETSON_SHERPA_EXTRA_LIBS it discovered.
set -e

JETSON_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SHERPA_TAG="${JETSON_SHERPA_TAG:-v1.10.30}"
PREFIX="${JETSON_SHERPA_PREFIX:-$JETSON_DIR/third_party/sherpa-prefix}"
SRC="${JETSON_SHERPA_SRC:-$JETSON_DIR/third_party/sherpa-onnx}"

echo "==> cloning sherpa-onnx $SHERPA_TAG"
if [ ! -d "$SRC/.git" ]; then
    git clone --depth 1 --branch "$SHERPA_TAG" https://github.com/k2-fsa/sherpa-onnx.git "$SRC"
fi

BUILD="$SRC/build-gpu"
echo "==> configuring (GPU) into $PREFIX"
mkdir -p "$BUILD"
cmake -S "$SRC" -B "$BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSHERPA_ONNX_ENABLE_GPU=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DSHERPA_ONNX_BUILD_TESTS=OFF \
    -DSHERPA_ONNX_BUILD_EXAMPLES=OFF

echo "==> building (this compiles onnxruntime + sherpa; takes a while on the A57)"
cmake --build "$BUILD" --target install -j"$(nproc)"

# Locate the onnxruntime archive sherpa downloaded, so the firmware link line
# can include it (the static sherpa lib references onnxruntime symbols).
ORT_LIB=$(find "$BUILD" -name 'libonnxruntime*.a' 2>/dev/null | head -1)
ORT_SO=$(find "$BUILD" -name 'libonnxruntime*.so' 2>/dev/null | head -1)

echo ""
echo "==> sherpa-onnx installed to: $PREFIX"
echo "==> include:  $PREFIX/include/sherpa-onnx/c-api/c-api.h"
echo "==> lib:      $PREFIX/lib/libsherpa-onnx.a"
if [ -n "$ORT_LIB" ]; then
    echo "==> onnxruntime static: $ORT_LIB"
fi
if [ -n "$ORT_SO" ] && [ -z "$ORT_LIB" ]; then
    echo "==> onnxruntime shared: $ORT_SO  (set LD_LIBRARY_PATH to its dir at runtime)"
fi
echo ""
echo "Now configure the firmware with:"
echo "  cmake -DJETSON_SHERPA_DIR=\"$PREFIX\" \\"
if [ -n "$ORT_LIB" ]; then
    echo "        -DJETSON_SHERPA_EXTRA_LIBS=\"$ORT_LIB\" \\"
fi
echo "        -S $JETSON_DIR -B $JETSON_DIR/build"
echo ""
echo "If the static link reports missing onnxruntime/cuda symbols, add the"
echo "discovered libs to JETSON_SHERPA_EXTRA_LIBS (semicolon-separated)."