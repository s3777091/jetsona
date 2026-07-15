#!/bin/bash
# Colorful console login banner for the Jetson HDMI panel.
#
# The firmware app takes over the DRM display once jetson-fw.service starts,
# but DURING BOOT (while waiting on network-online.target) and whenever the
# service restarts, the Linux VT shows the bare "jetson login:" prompt on a
# black screen. agetty prints /etc/issue right before that prompt, so we put a
# branded banner there.
#
# We embed ANSI colour escapes (the terminal renders them) plus agetty field
# codes that agetty itself expands:
#   \s = kernel name    \r = kernel release    \n = hostname
#   \m = machine arch    \d = date              \t = time
#
# Run on the Jetson:   sudo bash scripts/install-login-banner.sh
# Restore the stock banner:  sudo cp /etc/issue.orig /etc/issue
set -e

ISSUE=/etc/issue
if [ "$(id -u)" -ne 0 ]; then
    echo "install-login-banner.sh: must run with sudo" >&2
    exit 1
fi

# Keep the original issue the first time so this is reversible.
cp -n "$ISSUE" "$ISSUE.orig" 2>/dev/null || true

CYAN='\033[1;36m'; DIM='\033[0;36m'; GREY='\033[0;37m'; RST='\033[0m'
{
    printf '\033c'           # RIS: clear the screen for a clean banner
    printf "${CYAN}"
    cat <<'BOX'
  +============================================+
  |                                            |
  |              D S   -   0 2                 |
  |         Jetson Nano  -  AI Firmware        |
  |                                            |
  +============================================+
BOX
    printf "${GREY}"
    # \\n etc. emit a literal backslash-n so agetty expands it; \n is a newline.
    printf '\n  Host: \\n   |   Kernel: \\s \\r   |   Arch: \\m\n'
    printf '  \\d   \\t\n'
    printf "${RST}\n"
} > "$ISSUE"

echo "==> Login banner installed to $ISSUE (backup: $ISSUE.orig)"
echo "    Restore stock:  sudo cp $ISSUE.orig $ISSUE"