#!/bin/sh
# jetson-fan-curve -- userspace PWM fan controller for Jetson Nano (L4T 32.x).
#
# The in-kernel pwm-fan driver only spins the fan once thermal-fan-est crosses
# its first trip point (51 C), so on an idle Nano the fan never moves and looks
# broken.  This daemon takes the fan off kernel thermal control
# (temp_control=0) and drives target_pwm itself from a temperature curve, with
# hysteresis so it does not oscillate at a band edge.
#
# Runtime configuration lives in /etc/jetson-fan.conf and is re-read every
# tick, so the firmware UI can change mode/profile/speed by rewriting that file
# -- no restart, no root-owned IPC.
#
# Measured on the stock 5V 4-pin fan (2026-07-18): PWM 10 is enough to start it
# from a standstill (470 rpm), and RPM tracks PWM near-linearly up to 255/9070.
# Idle CPU is 32 C with the fan stopped and 31 C at PWM 25, so the quiet
# profile's low floor costs about a degree at idle and saves a lot of noise.

FAN=/sys/devices/pwm-fan
CONF=/etc/jetson-fan.conf
STATE=/run/jetson-fan.state
TICK=2
HYST=3

# Defaults, overridden by $CONF.
MODE=auto
PROFILE=quiet
MANUAL_PWM=128

[ -d "$FAN" ] || { echo "pwm-fan sysfs missing"; exit 1; }

# Curves are "temp:pwm" points in ascending temperature. The first point must
# start at 0 C -- its pwm is the idle floor the fan never drops below.
curve_for() {
    case "$1" in
        cool)     echo "0:90 42:125 48:160 54:195 60:225 66:255" ;;
        balanced) echo "0:45 46:85 52:120 58:160 64:200 70:255" ;;
        *)        echo "0:25 50:60 56:95 62:140 68:190 74:255" ;;
    esac
}

# Echo the i-th (1-based) token of the remaining arguments.
nth() { i=$1; shift; eval echo "\${$i}"; }

read_temp() {
    hot=0
    for z in 1 2; do
        t=$(cat "/sys/class/thermal/thermal_zone$z/temp" 2>/dev/null) || continue
        [ "$t" -gt "$hot" ] && hot=$t
    done
    echo $((hot / 1000))
}

set_pwm() {
    want=$1
    [ "$want" -lt 0 ] && want=0
    [ "$want" -gt 255 ] && want=255
    [ "$(cat $FAN/target_pwm)" = "$want" ] || echo "$want" > $FAN/target_pwm
}

# Take manual control and enable the tachometer so rpm_measured is meaningful
# (it reads 0 forever otherwise, which is what makes a working fan look dead).
echo 0 > $FAN/temp_control 2>/dev/null
echo 1 > $FAN/tach_enable 2>/dev/null

# Kickstart: a stopped fan will not always break away at the quiet profile's
# floor, so spin up hard for a moment before the curve takes over.
echo 255 > $FAN/target_pwm
sleep 2

band=0
while :; do
    # shellcheck disable=SC1090
    [ -r "$CONF" ] && . "$CONF"

    temp=$(read_temp)
    curve=$(curve_for "$PROFILE")
    # shellcheck disable=SC2086
    set -- $curve
    bands=$#

    case "$MODE" in
        manual)
            pwm=$MANUAL_PWM
            band=0
            ;;
        off)
            pwm=0
            band=0
            ;;
        *)
            [ "$band" -ge "$bands" ] && band=$((bands - 1))
            # Upshift when the next band's entry temperature is reached;
            # downshift only after HYST degrees below this band's own entry.
            if [ $((band + 1)) -lt "$bands" ]; then
                # shellcheck disable=SC2086
                next=$(nth $((band + 2)) $curve)
                [ "$temp" -ge "${next%%:*}" ] && band=$((band + 1))
            fi
            if [ "$band" -gt 0 ]; then
                # shellcheck disable=SC2086
                here=$(nth $((band + 1)) $curve)
                [ "$temp" -lt $(( ${here%%:*} - HYST )) ] && band=$((band - 1))
            fi
            # shellcheck disable=SC2086
            pwm=$(nth $((band + 1)) $curve)
            pwm=${pwm#*:}
            ;;
    esac

    set_pwm "$pwm"
    printf 'mode=%s profile=%s temp=%s band=%s pwm=%s rpm=%s\n' \
        "$MODE" "$PROFILE" "$temp" "$band" "$(cat $FAN/target_pwm)" \
        "$(cat $FAN/rpm_measured 2>/dev/null)" > "$STATE"

    sleep "$TICK"
done
