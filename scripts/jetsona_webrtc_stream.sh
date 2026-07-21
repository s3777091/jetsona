#!/bin/bash
# Keep one WebRTC publisher online for the lite firmware. The firmware owns
# /dev/fb0 directly (FBDEV backend, no display server), so the capture source
# is always fb. This supervisor just relaunches the publisher if it ever exits,
# which keeps the 24/7 remote stream alive across crashes.

set -u

PUBLISHER="${JETSON_WEBRTC_BIN:-/usr/local/bin/jetsona-fb-webrtc}"
child_pid=""

stop_child()
{
    [ -n "$child_pid" ] || return 0
    if kill -0 "$child_pid" 2>/dev/null; then
        kill "$child_pid" 2>/dev/null || true
        for _ in 1 2 3 4 5 6 7 8 9 10; do
            kill -0 "$child_pid" 2>/dev/null || break
            sleep 0.1
        done
    fi
    wait "$child_pid" 2>/dev/null || true
    child_pid=""
}

trap 'stop_child; exit 0' INT TERM HUP

while true; do
    echo "jetsona-webrtc-stream: capture source -> /dev/fb0" >&2
    JETSON_WEBRTC_SOURCE=fb "$PUBLISHER" &
    child_pid=$!
    wait "$child_pid" 2>/dev/null || true
    child_pid=""
    echo "jetsona-webrtc-stream: publisher exited, relaunching in 1s" >&2
    sleep 1
done
