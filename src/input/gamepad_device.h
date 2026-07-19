#pragma once

/* Linux gamepad discovery and non-blocking input reader.
 *
 * Modern Bluetooth gamepads are evdev devices even when the optional joydev
 * module (and therefore /dev/input/jsN) is absent.  Discovery consequently
 * follows the kernel gamepad contract first: an event node must advertise
 * EV_KEY + BTN_GAMEPAD/BTN_SOUTH.  The legacy joystick API remains a fallback
 * for old adapters and kernels.
 */

#include <memory>
#include <string>

namespace jetson::input {

enum class GamepadBackend { Evdev, Joystick };

struct GamepadDeviceInfo {
    bool connected = false;
    bool readable = false;
    GamepadBackend backend = GamepadBackend::Evdev;
    std::string path;
    std::string name;
};

struct GamepadSnapshot {
    bool connected = false;

    bool dpad_up = false;
    bool dpad_down = false;
    bool dpad_left = false;
    bool dpad_right = false;

    bool cross = false;
    bool circle = false;
    bool triangle = false;
    bool square = false;
    bool l1 = false;
    bool r1 = false;
    bool l2_button = false;
    bool r2_button = false;
    bool l3 = false;
    bool r3 = false;
    bool create = false;
    bool options = false;
    bool ps = false;
    bool touchpad = false;

    float left_x = 0.0f;
    float left_y = 0.0f;
    float right_x = 0.0f;
    float right_y = 0.0f;
    float l2 = 0.0f;
    float r2 = 0.0f;
    bool has_l2_axis = false;
    bool has_r2_axis = false;

    std::string last_input;
};

/* Detects one usable gamepad. This call only probes local /dev/input and
 * /sys/class/input nodes; it never shells out to bluetoothctl or blocks on a
 * Bluetooth scan. */
GamepadDeviceInfo DetectGamepad();

class GamepadDevice {
public:
    GamepadDevice();
    ~GamepadDevice();

    GamepadDevice(const GamepadDevice &) = delete;
    GamepadDevice &operator=(const GamepadDevice &) = delete;

    bool Open(const GamepadDeviceInfo &device);
    void Close();

    /* Drains currently queued input without blocking. Returns true when the
     * snapshot changed. Disconnection is reported through snapshot.connected. */
    bool Poll(GamepadSnapshot &snapshot);
    const std::string &LastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace jetson::input
