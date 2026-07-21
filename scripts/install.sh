#!/bin/bash
# Install the built firmware as a boot service on the Jetson.
# Run after a successful build:  sudo ./scripts/install.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JETSON_DIR="$(dirname "$SCRIPT_DIR")"
# shellcheck disable=SC1091
. "$SCRIPT_DIR/config_loader.sh"
jetson_load_config "${JETSON_CONFIG_FILE:-$JETSON_DIR/config.yaml}"
BUILD_DIR="${JETSON_BUILD_DIR:-$JETSON_DIR/build}"
case "$BUILD_DIR" in
    /*) ;;
    *) BUILD_DIR="$JETSON_DIR/$BUILD_DIR" ;;
esac

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

if [ ! -f "$BUILD_DIR/jetson_fw" ]; then
    echo "Build $BUILD_DIR/jetson_fw not found. Run cmake/make first." >&2
    exit 1
fi

RECEIPT="$BUILD_DIR/.jetsona-build-receipt"
if [ ! -f "$RECEIPT" ]; then
    echo "Build receipt not found: $RECEIPT" >&2
    echo "Run bash scripts/build.sh successfully before installing." >&2
    exit 1
fi
# shellcheck disable=SC1090
. "$RECEIPT"
: "${BUILD_GIT_HEAD:?Invalid build receipt: BUILD_GIT_HEAD is missing}"
: "${BUILD_SOURCE_SHA256:?Invalid build receipt: BUILD_SOURCE_SHA256 is missing}"
: "${BUILD_BINARY_SHA256:?Invalid build receipt: BUILD_BINARY_SHA256 is missing}"
CURRENT_GIT_HEAD="$(git -C "$JETSON_DIR" rev-parse HEAD 2>/dev/null || printf unknown)"
CURRENT_SOURCE_SHA256="$(source_tree_sha256)"
CURRENT_BINARY_SHA256="$(sha256sum "$BUILD_DIR/jetson_fw" | awk '{print $1}')"
if [ "$BUILD_GIT_HEAD" != "$CURRENT_GIT_HEAD" ] ||
   [ "$BUILD_SOURCE_SHA256" != "$CURRENT_SOURCE_SHA256" ] ||
   [ "$BUILD_BINARY_SHA256" != "$CURRENT_BINARY_SHA256" ]; then
    echo "Build receipt does not match the current source/binary." >&2
    echo "Run bash scripts/build.sh again; refusing to install a stale build." >&2
    exit 1
fi

# The service executes /opt/jetson-fw/jetson_fw directly. Stop it before
# replacing that binary; otherwise Linux can reject cp with ETXTBSY ("Text
# file busy"). This is harmless on the first install when the unit is absent.
sudo systemctl stop jetson-fw 2>/dev/null || true

echo "==> Installing to /opt/jetson-fw"
echo "==> Binary: $BUILD_DIR/jetson_fw"

# Settings store, shared by every launch path (see JETSON_SETTINGS_FILE in
# config.yaml and the service unit). Older installs left it at
# $HOME/.jetson-fw/settings.kv, which resolved differently for the systemd boot
# (HOME=/root) than for a manual `sudo run_fbdev.sh` (HOME=/home/<user>), so the
# panel booted with defaults while the hand-run kept the customized look. Adopt
# the newest surviving store once; the originals are left untouched.
sudo install -d -m 755 /var/lib/jetson-fw
if [ ! -f /var/lib/jetson-fw/settings.kv ]; then
    legacy_kv="$(sudo find /root/.jetson-fw /home/*/.jetson-fw -maxdepth 1 \
        -name settings.kv -printf '%T@ %p\n' 2>/dev/null | sort -rn | head -1 | cut -d' ' -f2-)"
    if [ -n "$legacy_kv" ]; then
        echo "==> Adopting existing settings from $legacy_kv"
        sudo cp "$legacy_kv" /var/lib/jetson-fw/settings.kv
    fi
fi

# /opt contains deployed files only; runtime settings/profiles live in
# /var/lib/jetson-fw. Recreate it so deleted scripts/assets cannot survive an
# upgrade and masquerade as part of the new version. No binary backup is kept.
sudo rm -rf -- /opt/jetson-fw
sudo install -d -m 0755 /opt/jetson-fw
sudo install -m 0755 "$BUILD_DIR/jetson_fw" /opt/jetson-fw/jetson_fw
sudo install -d -m 0755 /opt/jetson-fw/assets
sudo cp -a "$JETSON_DIR/assets/." /opt/jetson-fw/assets/
sudo cp "$JETSON_DIR/config.yaml" /opt/jetson-fw/
sudo mkdir -p /opt/jetson-fw/scripts
sudo cp "$JETSON_DIR/scripts/s3_assets.py" /opt/jetson-fw/scripts/
sudo cp "$JETSON_DIR/scripts/config_loader.sh" /opt/jetson-fw/scripts/
sudo chmod +x /opt/jetson-fw/scripts/s3_assets.py
# Supervisor: restarts the firmware if it ever exits.
sudo cp "$JETSON_DIR/scripts/jetson_fw_run.sh" /opt/jetson-fw/scripts/
sudo cp "$JETSON_DIR/scripts/setup-tailscale-client.sh" /opt/jetson-fw/scripts/
sudo chmod +x \
    /opt/jetson-fw/scripts/jetson_fw_run.sh \
    /opt/jetson-fw/scripts/config_loader.sh \
    /opt/jetson-fw/scripts/setup-tailscale-client.sh
if [ -f "$JETSON_DIR/.env" ]; then
    sudo cp "$JETSON_DIR/.env" /opt/jetson-fw/.env
    sudo chmod 600 /opt/jetson-fw/.env
fi
sudo cp "$JETSON_DIR/scripts/jetson-fw.service" /etc/systemd/system/

# Legacy 12V-fan-on-GPIO helper (NOT auto-enabled — requires MOSFET rewiring
# first). Unrelated to the PWM fan header below; kept for boards wired that way.
sudo cp "$JETSON_DIR/scripts/fan.sh"        /opt/jetson-fw/
sudo chmod +x /opt/jetson-fw/fan.sh
sudo cp "$JETSON_DIR/scripts/fan.service"   /etc/systemd/system/

# PWM fan curve for the 4-pin fan on the carrier's J15 header. The kernel
# pwm-fan driver leaves target_pwm at 0 until 51 C, so an idle board looks like
# it has a dead fan; this daemon drives the duty cycle itself. Settings >
# Cài đặt chung > Quạt talks to it through /etc/jetson-fan.conf, which is left
# world-writable on purpose so the firmware can change modes without root.
sudo cp "$JETSON_DIR/scripts/jetson-fan-curve.sh" /usr/local/sbin/
sudo chmod +x /usr/local/sbin/jetson-fan-curve.sh
sudo cp "$JETSON_DIR/scripts/jetson-fan.service"  /etc/systemd/system/
if [ ! -f /etc/jetson-fan.conf ]; then
    sudo cp "$JETSON_DIR/scripts/jetson-fan.conf" /etc/jetson-fan.conf
fi
sudo chmod 0666 /etc/jetson-fan.conf

echo "==> Enabling systemd service"
sudo systemctl daemon-reload

# Tailscale SSH is stored as a persistent tailscaled preference. When this
# Jetson has already joined a tailnet, keep the daemon enabled at boot and turn
# on browser-based SSH as part of the normal firmware installation. Do not make
# a first-time Tailscale login block an otherwise valid firmware install; the
# setup helper handles that interactive/auth-key flow separately.
if command -v tailscale >/dev/null 2>&1; then
    echo "==> Enabling Tailscale background service"
    if sudo systemctl enable --now tailscaled; then
        if sudo tailscale status --json 2>/dev/null |
            grep -q '"BackendState": *"Running"'; then
            echo "==> Enabling persistent Tailscale SSH"
            if ! sudo tailscale set --ssh; then
                echo "WARNING: Could not enable Tailscale SSH; run setup-tailscale-client.sh." >&2
            fi
        else
            echo "WARNING: Tailscale is not logged in; run setup-tailscale-client.sh once." >&2
        fi
    else
        echo "WARNING: Could not enable tailscaled; run setup-tailscale-client.sh." >&2
    fi
else
    echo "==> Tailscale not installed; remote SSH setup skipped"
    echo "    Run: sudo /opt/jetson-fw/scripts/setup-tailscale-client.sh" >&2
fi

sudo systemctl enable jetson-fw
sudo systemctl restart jetson-fw
sudo systemctl enable jetson-fan
sudo systemctl restart jetson-fan

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
