#!/bin/bash
# Deploy the air-conditioner bridge to the Jetson, from the PC.
#
#   bash scripts/deploy_ac.sh                # bridge only
#   bash scripts/deploy_ac.sh --build        # bridge + rebuild/reinstall firmware
#
# Everything rides on ONE ssh connection (ControlMaster), so you type the
# Jetson password once instead of once per copy. sudo on the Jetson may ask for
# it again -- that is the remote sudo prompt, not this script.
#
# Config:
#   JETSON_HOST   user@host           (default ekkohuynh@192.168.50.96)
#   JETSON_DIR    repo path on Jetson (default ~/jetsona)
#   LGE_ENV       LG creds on the PC  (default ../../IOT/.env)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JETSON_DIR_LOCAL="$(dirname "$SCRIPT_DIR")"

JETSON_HOST="${JETSON_HOST:-ekkohuynh@192.168.50.96}"
JETSON_DIR="${JETSON_DIR:-\$HOME/jetsona}"
LGE_ENV="${LGE_ENV:-$(dirname "$(dirname "$JETSON_DIR_LOCAL")")/IOT/.env}"
DO_BUILD=0
[ "${1:-}" = "--build" ] && DO_BUILD=1

if [ ! -f "$LGE_ENV" ]; then
    echo "LG credentials not found at: $LGE_ENV" >&2
    echo "Set LGE_ENV=/path/to/.env (the IOT project's) and re-run." >&2
    exit 1
fi

# One authenticated connection, reused by every scp/ssh below.
CTL="$(mktemp -u "${TMPDIR:-/tmp}/jetsona-ac-%r@%h.XXXXXX")"
SSH_OPTS=(-o ControlMaster=auto -o "ControlPath=$CTL" -o ControlPersist=300)
cleanup() { ssh "${SSH_OPTS[@]}" -O exit "$JETSON_HOST" 2>/dev/null || true; }
trap cleanup EXIT

echo "==> Connecting to $JETSON_HOST (password prompt is for this one connection)"
ssh "${SSH_OPTS[@]}" "$JETSON_HOST" 'echo "    connected as $(whoami) on $(hostname)"'

echo "==> Copying the bridge + changed firmware sources"
scp "${SSH_OPTS[@]}" -q \
    "$JETSON_DIR_LOCAL/scripts/jetsona_ac.py" \
    "$JETSON_DIR_LOCAL/scripts/jetsona-ac.service" \
    "$JETSON_DIR_LOCAL/scripts/jetsona_ac_install.sh" \
    "$JETSON_HOST:$JETSON_DIR/scripts/"
scp "${SSH_OPTS[@]}" -q \
    "$JETSON_DIR_LOCAL/src/agent/system_tools.h" \
    "$JETSON_DIR_LOCAL/src/agent/system_tools.cc" \
    "$JETSON_DIR_LOCAL/src/agent/tools.cc" \
    "$JETSON_HOST:$JETSON_DIR/src/agent/"

echo "==> Copying LG credentials (deleted from the Jetson once installed)"
scp "${SSH_OPTS[@]}" -q "$LGE_ENV" "$JETSON_HOST:\$HOME/iot-lge.env"

# config.yaml is edited in place rather than copied: the Jetson's copy carries
# device-specific values (JETSON_WOL_URL among them) that the repo's does not,
# and overwriting it would quietly undo them.
echo "==> Adding JETSON_AC_URL to config.yaml if missing"
ssh "${SSH_OPTS[@]}" "$JETSON_HOST" "
    set -e
    cfg=$JETSON_DIR/config.yaml
    if grep -q '^JETSON_AC_URL:' \"\$cfg\"; then
        echo '    already present, left alone'
    else
        printf '\n# Room air conditioner bridge (scripts/jetsona_ac.py).\nJETSON_AC_URL: \"http://127.0.0.1:46003\"\n' >> \"\$cfg\"
        echo '    appended'
    fi"

echo "==> Installing the bridge service (sudo may prompt)"
ssh "${SSH_OPTS[@]}" -t "$JETSON_HOST" \
    "sudo bash $JETSON_DIR/scripts/jetsona_ac_install.sh \$HOME/iot-lge.env"

echo "==> Wiring JETSON_AC_TOKEN into the firmware .env files"
ssh "${SSH_OPTS[@]}" -t "$JETSON_HOST" "
    set -e
    tok=\$(sudo grep '^JETSON_AC_TOKEN=' /etc/jetsona-ac.env | cut -d= -f2-)
    for f in $JETSON_DIR/.env /opt/jetson-fw/.env; do
        [ -e \"\$f\" ] || continue
        if sudo grep -q '^JETSON_AC_TOKEN=' \"\$f\"; then
            sudo sed -i \"s|^JETSON_AC_TOKEN=.*|JETSON_AC_TOKEN=\$tok|\" \"\$f\"
            echo \"    updated \$f\"
        else
            echo \"JETSON_AC_TOKEN=\$tok\" | sudo tee -a \"\$f\" >/dev/null
            echo \"    added to \$f\"
        fi
    done
    rm -f \$HOME/iot-lge.env
    echo '    removed ~/iot-lge.env (PAT now only in /etc/jetsona-ac.env, mode 600)'"

if [ "$DO_BUILD" -eq 1 ]; then
    echo "==> Rebuilding the firmware (this takes a while)"
    ssh "${SSH_OPTS[@]}" -t "$JETSON_HOST" "
        set -e
        cd $JETSON_DIR
        JETSON_SKIP_ASSET_FETCH=1 bash scripts/build.sh
        sudo bash scripts/install.sh
        sudo systemctl restart jetson-fw"
else
    echo
    echo "Bridge installed. The firmware still has no air_conditioner tool until"
    echo "it is rebuilt -- re-run with --build, or on the Jetson:"
    echo "    cd ~/jetsona && JETSON_SKIP_ASSET_FETCH=1 bash scripts/build.sh"
    echo "    sudo bash scripts/install.sh && sudo systemctl restart jetson-fw"
fi

echo
echo "==> Checking the bridge"
ssh "${SSH_OPTS[@]}" -t "$JETSON_HOST" "
    tok=\$(sudo grep '^JETSON_AC_TOKEN=' /etc/jetsona-ac.env | cut -d= -f2-)
    curl -fsS --max-time 10 -H \"X-AC-Token: \$tok\" http://127.0.0.1:46003/status && echo
    systemctl is-active jetsona-ac"
