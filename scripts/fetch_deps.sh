#!/bin/bash
# Fetch build dependencies for the Jetson Nano firmware.
# Run once on the Jetson (aarch64, JetPack 4.x / Ubuntu 18.04).
set -e

echo "==> Installing build tools + libs (apt)"
sudo apt-get update
# NOTE: bionic arm64 has no libegl-dev/libgles2-dev (virtual names) and no
# nlohmann-json3-dev. Use the concrete mesa dev packages; nlohmann v3 is
# installed as a single header below (the code uses v3-only .contains()).
sudo apt-get install -y \
    build-essential git pkg-config \
    python3 \
    libdrm-dev libgbm-dev libegl1-mesa-dev libgles2-mesa-dev \
    libsdl2-dev libsdl2-ttf-dev \
    libcurl4-openssl-dev libssl-dev libopus-dev libasound2-dev mpv \
    bluez bluez-tools rfkill \
    network-manager \
    xserver-xorg-video-all xserver-xorg-input-libinput \
    x11-xkb-utils x11-xserver-utils xinit libx11-dev chromium-browser

# nlohmann json v3 single-header -> /usr/local/include (GCC searches it before
# /usr/include, so this shadows bionic's v2.1.1 nlohmann-json-dev header).
NLOHJSON_HPP=/usr/local/include/nlohmann/json.hpp
if [ ! -f "$NLOHJSON_HPP" ]; then
    echo "==> Installing nlohmann json v3.11.3 single-header to /usr/local/include"
    sudo mkdir -p /usr/local/include/nlohmann
    sudo wget -qO "$NLOHJSON_HPP" \
        https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp
fi

# Ubuntu 18.04 (Bionic) ships CMake 3.10.2 in apt, but CMakeLists.txt needs >=3.13.
# Install a modern CMake from the Kitware PPA when the system one is too old.
NEEDED_CMAKE="3.13"
if ! command -v cmake >/dev/null 2>&1 || ! cmake --version 2>/dev/null | head -1 \
        | awk '{print $3}' | awk -v n="$NEEDED_CMAKE" \
        '{split($1,v,"."); exit !(v[1]"."v[2]+0 < n)}'; then
    : # cmake OK
else
    echo "==> System CMake too old (< $NEEDED_CMAKE); installing from Kitware PPA (bionic)"
    sudo apt-get install -y apt-transport-https ca-certificates gnupg wget
    wget -qO - https://apt.kitware.com/keys/kitware-archive-latest.asc 2>/dev/null \
        | sudo apt-key add -
    sudo apt-add-repository -y 'deb https://apt.kitware.com/ubuntu/ bionic main'
    sudo apt-get update
    sudo apt-get install -y cmake
fi
cmake --version

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JETSON_DIR="$(dirname "$SCRIPT_DIR")"

if [ ! -d "$JETSON_DIR/third_party/lvgl/src" ]; then
    echo "==> Cloning LVGL 9.2.2"
    mkdir -p "$JETSON_DIR/third_party"
    git clone --depth 1 --branch v9.2.2 https://github.com/lvgl/lvgl.git \
        "$JETSON_DIR/third_party/lvgl"
else
    echo "==> LVGL already present at third_party/lvgl"
fi

echo "==> Done. Build with:"
echo "    bash $JETSON_DIR/scripts/build.sh"
