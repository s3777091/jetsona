#pragma once

/* PWM fan control for the Jetson Nano carrier fan header (J15).
 *
 * The in-kernel pwm-fan driver keeps target_pwm at 0 until thermal-fan-est
 * crosses its first trip point (51 C), so on an idle board the fan never
 * spins and reads as broken hardware. The device therefore runs a small
 * userspace curve daemon (jetson-fan.service, /usr/local/sbin/
 * jetson-fan-curve.sh) that owns temp_control=0 and drives target_pwm.
 *
 * This module is the firmware's side of that contract: it reports the fan's
 * live state from sysfs and changes mode/speed by rewriting the daemon's
 * config file, which the daemon re-reads every couple of seconds. When the
 * daemon is not installed the setters fall back to driving sysfs directly so
 * the UI still works, it just loses the curve on reboot.
 */

#include <string>

namespace jetson::fan {

enum class Mode { Auto, Manual, Off };

// Auto-mode curves. Idle duty is what you actually hear: Quiet holds PWM 25
// (~1450 rpm) and only ramps past 50 C, which costs about 1 C at idle versus
// Cool's PWM 90 (~5400 rpm). See scripts/jetson-fan-curve.sh for the points.
enum class Profile { Quiet, Balanced, Cool };

struct Status {
    bool available = false;  // pwm-fan sysfs present
    bool daemon = false;     // jetson-fan-curve.sh config found
    Mode mode = Mode::Auto;
    Profile profile = Profile::Quiet;
    int manual_pwm = 128;    // configured manual duty, 0..255
    int target_pwm = 0;      // duty the fan is being driven at now, 0..255
    int rpm = 0;             // measured, 0 when the tachometer is disabled
    int temp_c = 0;          // hottest of CPU-therm / GPU-therm
};

Status Read();

// All three persist to the daemon config; SetManualPwm also switches to
// manual, and SetProfile only has an effect in auto mode.
void SetMode(Mode mode);
void SetManualPwm(int pwm);
void SetProfile(Profile profile);

// UI helpers. Percent is what the slider speaks; kMinPercent is the lowest
// duty that reliably keeps a stopped sleeve-bearing fan turning.
constexpr int kMinPercent = 30;
int PwmToPercent(int pwm);
int PercentToPwm(int percent);
const char *ModeLabel(Mode mode);
const char *ProfileLabel(Profile profile);

} // namespace jetson::fan
