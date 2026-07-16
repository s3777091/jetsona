#!/bin/bash
# Supervisor for the DS-02 firmware with Chromium kiosk hand-off.
#
# The firmware owns /dev/fb0 directly (FBDEV backend, no display server), so a
# browser cannot run alongside it. When the "Chromium" drawer tile is tapped,
# the firmware stops its render loop and exits with code 42; this supervisor
# then runs scripts/launch_chromium.sh (Xorg + chromium --kiosk on the panel),
# and once the browser closes it restarts the firmware, which re-acquires
# /dev/fb0.
#
# Exit code convention:
#   42  -> launch the Chromium kiosk, then restart the firmware
#   *   -> crash or clean exit: back off 1s and restart the firmware
#
# SIGTERM/SIGINT (systemctl stop / restart): stop the loop cleanly. systemd's
# control-group kill also terminates any Xorg/Chromium we spawned, so a stop
# tears the whole kiosk down with us.
set -u

FW=/opt/jetson-fw/jetson_fw
KIOSK=/opt/jetson-fw/scripts/launch_chromium.sh
LOG=/var/log/jetson-fw.log

stop=0
trap 'stop=1' TERM INT

while [ "$stop" -eq 0 ]; do
    "$FW"
    rc=$?
    [ "$stop" -eq 1 ] && exit 0

    if [ "$rc" -eq 42 ]; then
        echo "jetson_fw_run: firmware requested Chromium kiosk (exit 42)" >> "$LOG"
        bash "$KIOSK" >> "$LOG" 2>&1 || true
        [ "$stop" -eq 1 ] && exit 0
        continue
    fi

    # Crash or unexpected exit: back off briefly and relaunch.
    echo "jetson_fw_run: firmware exited rc=$rc, restarting in 1s" >> "$LOG"
    sleep 1
done