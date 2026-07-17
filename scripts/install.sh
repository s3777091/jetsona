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

# Launcher icons are required runtime inputs, not optional decoration. Catch a
# stale/incomplete asset cache before replacing the installed firmware.
STUDIO_ICON="$JETSON_DIR/assets/icons/drawer/studio.png"
if [ ! -s "$STUDIO_ICON" ]; then
    echo "Required Studio icon not found: $STUDIO_ICON" >&2
    echo "Run bash scripts/fetch_assets.sh, then install again." >&2
    exit 1
fi

# The service executes /opt/jetson-fw/jetson_fw directly. Stop it before
# replacing that binary; otherwise Linux can reject cp with ETXTBSY ("Text
# file busy"). This is harmless on the first install when the unit is absent.
sudo systemctl stop jetson-fw 2>/dev/null || true

echo "==> Installing to /opt/jetson-fw"
sudo mkdir -p /opt/jetson-fw

# Keep Chromium out of the root service account. Xorg/firmware still need root
# for the physical display, but the browser can then retain its normal process
# sandbox and never needs the unsafe --no-sandbox flag.
CHROMIUM_KIOSK_USER="${CHROMIUM_KIOSK_USER:-jetson-kiosk}"
if ! id "$CHROMIUM_KIOSK_USER" >/dev/null 2>&1; then
    echo "==> Creating unprivileged Chromium user: $CHROMIUM_KIOSK_USER"
    sudo useradd --system --create-home \
        --home-dir /var/lib/jetson-fw/chromium-home \
        --shell /usr/sbin/nologin "$CHROMIUM_KIOSK_USER"
fi
# Web audio and distro Chromium GPU helpers may need these device groups. The
# group list differs between JetPack releases, so add only groups that exist.
for kiosk_group in audio video render; do
    if getent group "$kiosk_group" >/dev/null 2>&1; then
        sudo usermod -a -G "$kiosk_group" "$CHROMIUM_KIOSK_USER"
    fi
done
chromium_uid="$(id -u "$CHROMIUM_KIOSK_USER")"
chromium_gid="$(id -g "$CHROMIUM_KIOSK_USER")"
sudo install -d -m 700 -o "$chromium_uid" -g "$chromium_gid" \
    /var/lib/jetson-fw/chromium-home \
    /var/lib/jetson-fw/chromium-profile

sudo cp "$BUILD_DIR/jetson_fw" /opt/jetson-fw/
# Chromium kiosk status bar (Dynamic Island strip + keyboard-focus micro-WM).
# Optional: only built when libx11-dev was present at cmake time.
if [ -f "$BUILD_DIR/jetson_kiosk_bar" ]; then
    sudo cp "$BUILD_DIR/jetson_kiosk_bar" /opt/jetson-fw/
    sudo chmod +x /opt/jetson-fw/jetson_kiosk_bar
else
    echo "==> jetson_kiosk_bar not built (libx11-dev missing?); Chromium keeps full-screen kiosk mode" >&2
fi
sudo mkdir -p /opt/jetson-fw/assets
sudo cp -r "$JETSON_DIR/assets/." /opt/jetson-fw/assets/
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
