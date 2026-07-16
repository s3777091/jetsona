#!/bin/bash
# Prepare a Linux VM as Jetsona's Tailscale exit node.
#
# Run on the VM itself:
#   sudo bash setup-tailscale-exit-node.sh
# Or, for unattended tailnet login, pass a reusable/pre-authorized auth key:
#   sudo TS_AUTHKEY=tskey-auth-... bash setup-tailscale-exit-node.sh
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "Run this script as root (sudo bash $0)." >&2
    exit 1
fi

TS_HOSTNAME="${JETSON_VPN_EXIT_NODE:-jetsona-vpn}"

if ! command -v tailscale >/dev/null 2>&1; then
    echo "==> Installing Tailscale"
    curl -fsSL https://tailscale.com/install.sh | sh
fi

echo "==> Enabling IP forwarding"
install -d -m 0755 /etc/sysctl.d
install -m 0644 /dev/stdin /etc/sysctl.d/99-tailscale-exit-node.conf <<'EOF'
net.ipv4.ip_forward = 1
net.ipv6.conf.all.forwarding = 1
EOF
sysctl --system >/dev/null

# Linux 6.2+ can forward Tailscale UDP traffic much faster with these offload
# settings. Apply them now and, when available, through networkd-dispatcher on
# every interface transition/reboot.
if command -v ethtool >/dev/null 2>&1; then
    NETDEV="$(ip -o route get 8.8.8.8 | cut -f 5 -d ' ')"
    if [ -n "$NETDEV" ]; then
        ethtool -K "$NETDEV" rx-udp-gro-forwarding on rx-gro-list off
    fi
    if systemctl is-enabled networkd-dispatcher >/dev/null 2>&1; then
        install -d -m 0755 /etc/networkd-dispatcher/routable.d
        install -m 0755 /dev/stdin \
            /etc/networkd-dispatcher/routable.d/50-tailscale <<'EOF'
#!/bin/sh
NETDEV="$(ip -o route get 8.8.8.8 | cut -f 5 -d ' ')"
[ -n "$NETDEV" ] && ethtool -K "$NETDEV" rx-udp-gro-forwarding on rx-gro-list off
EOF
    fi
fi

systemctl enable --now tailscaled

if tailscale status --json 2>/dev/null | grep -q '"BackendState": *"Running"'; then
    echo "==> Updating the existing Tailscale node"
    tailscale set --hostname="$TS_HOSTNAME" --advertise-exit-node
else
    echo "==> Joining the tailnet as $TS_HOSTNAME"
    if [ -n "${TS_AUTHKEY:-}" ]; then
        tailscale up --auth-key="$TS_AUTHKEY" --hostname="$TS_HOSTNAME" \
            --advertise-exit-node
    else
        tailscale up --hostname="$TS_HOSTNAME" --advertise-exit-node
    fi
fi

echo
echo "VM is advertising as exit node: $TS_HOSTNAME"
echo "Final required step: approve 'Use as exit node' for this machine in"
echo "https://login.tailscale.com/admin/machines"
