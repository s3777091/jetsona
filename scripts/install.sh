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
sudo cp "$JETSON_DIR/config.yaml" /opt/jetson-fw/
sudo mkdir -p /opt/jetson-fw/scripts
sudo cp "$JETSON_DIR/scripts/s3_assets.py" /opt/jetson-fw/scripts/
sudo cp "$JETSON_DIR/scripts/config_loader.sh" /opt/jetson-fw/scripts/
sudo chmod +x /opt/jetson-fw/scripts/s3_assets.py
# Supervisor + bare-X launchers. Chromium and PS Remote Play each take the
# panel while the framebuffer firmware is stopped; it restarts on app exit.
sudo cp "$JETSON_DIR/scripts/jetson_fw_run.sh" /opt/jetson-fw/scripts/
sudo cp "$JETSON_DIR/scripts/launch_chromium.sh" /opt/jetson-fw/scripts/
sudo cp "$JETSON_DIR/scripts/launch_ps_remote_play.sh" /opt/jetson-fw/scripts/
sudo cp "$JETSON_DIR/scripts/ps_remote_play_ctl.sh" /opt/jetson-fw/scripts/
sudo chmod +x \
    /opt/jetson-fw/scripts/jetson_fw_run.sh \
    /opt/jetson-fw/scripts/config_loader.sh \
    /opt/jetson-fw/scripts/launch_chromium.sh \
    /opt/jetson-fw/scripts/launch_ps_remote_play.sh \
    /opt/jetson-fw/scripts/ps_remote_play_ctl.sh
sudo mkdir -p /var/lib/jetson-fw/chiaki
sudo chmod 700 /var/lib/jetson-fw/chiaki
if [ -f "$JETSON_DIR/.env" ]; then
    sudo cp "$JETSON_DIR/.env" /opt/jetson-fw/.env
    sudo chmod 600 /opt/jetson-fw/.env
fi
sudo cp "$JETSON_DIR/scripts/jetson-fw.service" /etc/systemd/system/

# Cooling fan helper (NOT auto-enabled — requires MOSFET rewiring first).
sudo cp "$JETSON_DIR/scripts/fan.sh"        /opt/jetson-fw/
sudo chmod +x /opt/jetson-fw/fan.sh
sudo cp "$JETSON_DIR/scripts/fan.service"   /etc/systemd/system/

echo "==> Enabling systemd service"
sudo systemctl daemon-reload
sudo systemctl enable jetson-fw
sudo systemctl restart jetson-fw

# Optional: branded login banner on the HDMI console (replaces the bare
# "jetson login:" prompt shown during boot / service restart). Backs up
# /etc/issue to /etc/issue.orig first. Off by default — set to install:
#   INSTALL_LOGIN_BANNER=1 sudo ./scripts/install.sh
if [ "${INSTALL_LOGIN_BANNER:-0}" = "1" ]; then
    bash "$SCRIPT_DIR/install-login-banner.sh"
else
    echo "==> Skipped login banner (set INSTALL_LOGIN_BANNER=1 to install it)"
fi

echo "==> Installed. Check:  sudo systemctl status jetson-fw ; tail -f /var/log/jetson-fw.log"
