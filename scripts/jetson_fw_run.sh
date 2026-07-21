#!/bin/bash
# Supervisor for the lite DS-02 firmware.
#
# The firmware owns /dev/fb0 directly (FBDEV backend, no display server) and
# runs as a 24/7 online server appliance. This supervisor simply keeps it
# alive: if the firmware ever exits (crash or clean shutdown), back off briefly
# and relaunch it. systemd's control-group kill stops this loop on stop.
#
# SIGTERM/SIGINT (systemctl stop / restart): stop the loop cleanly.
set -u

FW=/opt/jetson-fw/jetson_fw
LOG=/var/log/jetson-fw.log

# shellcheck disable=SC1091
. /opt/jetson-fw/scripts/config_loader.sh
jetson_load_config "${JETSON_CONFIG_FILE:-/opt/jetson-fw/config.yaml}"
jetson_load_secrets "${JETSON_ENV_FILE:-/opt/jetson-fw/.env}"

# Tells the firmware a supervisor is watching.
export JETSON_FW_SUPERVISED=1

stop=0
trap 'stop=1' TERM INT

while [ "$stop" -eq 0 ]; do
    "$FW"
    rc=$?
    [ "$stop" -eq 1 ] && exit 0

    # Crash or unexpected exit: back off briefly and relaunch.
    echo "jetson_fw_run: firmware exited rc=$rc, restarting in 1s" >> "$LOG"
    sleep 1
done
