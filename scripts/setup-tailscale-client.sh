#!/bin/bash
# Install and join Tailscale on the Jetson that runs the firmware.
#
# Interactive login:
#   sudo bash scripts/setup-tailscale-client.sh
# Unattended login:
#   sudo TS_AUTHKEY=tskey-auth-... bash scripts/setup-tailscale-client.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JETSON_DIR="$(dirname "$SCRIPT_DIR")"
if [ -r "$SCRIPT_DIR/config_loader.sh" ]; then
    # shellcheck disable=SC1091
    . "$SCRIPT_DIR/config_loader.sh"
    jetson_load_config "${JETSON_CONFIG_FILE:-$JETSON_DIR/config.yaml}"
fi

if [ "$(id -u)" -ne 0 ]; then
    echo "Run this script as root (sudo bash $0)." >&2
    exit 1
fi

TS_HOSTNAME="${JETSON_TAILSCALE_HOSTNAME:-jetsona}"
EXIT_NODE="${JETSON_VPN_EXIT_NODE:-jetsona-vpn}"

if ! command -v tailscale >/dev/null 2>&1; then
    echo "==> Installing Tailscale"
    curl -fsSL https://tailscale.com/install.sh | sh
fi

systemctl enable --now tailscaled

if tailscale status --json 2>/dev/null | grep -q '"BackendState": *"Running"'; then
    echo "==> Jetson is already connected to a tailnet"
    tailscale set --hostname="$TS_HOSTNAME"
else
    echo "==> Joining the Jetson to the tailnet"
    if [ -n "${TS_AUTHKEY:-}" ]; then
        tailscale up --auth-key="$TS_AUTHKEY" --hostname="$TS_HOSTNAME"
    else
        tailscale up --hostname="$TS_HOSTNAME"
    fi
fi

# Tailscale SSH is a persisted daemon preference, not a foreground process.
# Enabling it once here is enough: the tailscaled systemd service above starts
# in the background on every boot and restores the SSH listener automatically.
echo "==> Enabling persistent Tailscale SSH"
tailscale set --ssh

echo
echo "Jetson Tailscale client is ready. The firmware VPN toggle will select:"
echo "  $EXIT_NODE"
echo "Make sure config.yaml contains: JETSON_VPN_EXIT_NODE: \"$EXIT_NODE\""
echo "Do not select the exit node here; the Settings toggle owns that preference."
echo "Tailscale SSH is enabled and will return automatically after every reboot."
