#!/bin/bash
# Keep one RTSP publisher online and follow the active display owner. The
# firmware renders directly into fb0, while Chromium/Chiaki render through
# Xorg :0. Switching the capture source here keeps the same browser URL alive.

set -u

child_pid=""
mode=""

x11_ready()
{
    [ -S /tmp/.X11-unix/X0 ] &&
        DISPLAY=:0 xdpyinfo >/dev/null 2>&1
}

active_mode()
{
    if x11_ready; then
        printf '%s' x11
    else
        printf '%s' fb
    fi
}

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
    mode="$(active_mode)"
    if [ "$mode" = x11 ]; then
        echo "jetsona-webrtc-stream: capture source -> X11 :0" >&2
        DISPLAY=:0 JETSON_WEBRTC_SOURCE=x11 /usr/local/bin/jetsona-fb-webrtc &
    else
        echo "jetsona-webrtc-stream: capture source -> /dev/fb0" >&2
        JETSON_WEBRTC_SOURCE=fb /usr/local/bin/jetsona-fb-webrtc &
    fi
    child_pid=$!

    while kill -0 "$child_pid" 2>/dev/null; do
        sleep 0.25
        [ "$(active_mode)" = "$mode" ] || break
    done
    stop_child
    sleep 0.25
done
