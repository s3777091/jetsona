#!/bin/bash
# Cooling fan control for the Jetson Nano via sysfs GPIO (one-shot, state persists).
#
# !! HARDWARE REQUIREMENT (read before enabling) !!
# The fan is 12V / 0.12A. A Jetson GPIO pin is 3.3V and can source only a few mA.
# Wiring the fan directly to GPIO 40 WILL NOT spin it and can burn the pin.
# Drive it through a logic-level MOSFET module (e.g. IRF520 "MOSFET trigger"
# module, or better an IRLZ44N/AO3400 module rated for low Vgs). Wiring:
#
#     external 12V (+)  -> fan (+)
#     fan (-)          -> MOSFET DRAIN
#     MOSFET SOURCE    -> GND  (shared with Jetson pin 39)
#     MOSFET GATE      -> Jetson pin 40 (GPIO)
#
# Set FAN_GPIO to the Linux sysfs GPIO number that corresponds to BOARD pin 40.
# Find it on the Jetson (run once, note the gpioN node that appears):
#
#     python3 - <<'EOF' &
#     import Jetson.GPIO as G, time
#     G.setmode(G.BOARD); G.setup(40, G.OUT); G.output(40, 1); time.sleep(60)
#     EOF
#     sleep 2; ls -d /sys/class/gpio/gpio* | grep -v gpiochip
#
# The new gpioN node (e.g. gpio440) is pin 40 -> put just the number in FAN_GPIO.

set -e
FAN_GPIO="${FAN_GPIO:-}"
[ -z "$FAN_GPIO" ] && { echo "fan.sh: FAN_GPIO unset. See header for discovery." >&2; exit 1; }

GPIO_ROOT="/sys/class/gpio"
case "$1" in
    on)
        [ -d "$GPIO_ROOT/gpio$FAN_GPIO" ] || echo "$FAN_GPIO" | sudo tee "$GPIO_ROOT/export" >/dev/null 2>&1 || true
        echo out | sudo tee "$GPIO_ROOT/gpio$FAN_GPIO/direction" >/dev/null
        echo 1   | sudo tee "$GPIO_ROOT/gpio$FAN_GPIO/value"        >/dev/null
        echo "fan.sh: fan ON (gpio$FAN_GPIO=1)"
        ;;
    off)
        if [ -d "$GPIO_ROOT/gpio$FAN_GPIO" ]; then
            echo 0          | sudo tee "$GPIO_ROOT/gpio$FAN_GPIO/value"   >/dev/null || true
            echo "$FAN_GPIO" | sudo tee "$GPIO_ROOT/unexport"             >/dev/null 2>&1 || true
        fi
        echo "fan.sh: fan OFF (gpio$FAN_GPIO)"
        ;;
    *)
        echo "usage: FAN_GPIO=<n> fan.sh on|off" >&2
        exit 1
        ;;
esac