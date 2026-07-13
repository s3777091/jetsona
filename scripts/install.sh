#!/bin/bash
# Install the built firmware as a boot service on the Jetson.
# Run after a successful build:  sudo ./scripts/install.sh
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JETSON_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$JETSON_DIR/build"

if [ ! -f "$BUILD_DIR/jetson_fw" ]; then
    echo "Build $BUILD_DIR/jetson_fw not found. Run cmake/make first." >&2
    exit 1
fi

echo "==> Installing to /opt/jetson-fw"
sudo mkdir -p /opt/jetson-fw
sudo cp "$BUILD_DIR/jetson_fw" /opt/jetson-fw/
sudo cp -r "$JETSON_DIR/assets" /opt/jetson-fw/
sudo cp "$JETSON_DIR/scripts/jetson-fw.service" /etc/systemd/system/

echo "==> Enabling systemd service"
sudo systemctl daemon-reload
sudo systemctl enable jetson-fw
sudo systemctl restart jetson-fw

echo "==> Installed. Check:  sudo systemctl status jetson-fw ; tail -f /var/log/jetson-fw.log"