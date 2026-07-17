#pragma once

/* Linux Bluetooth manager backed by BlueZ (`bluetoothctl`).
 *
 * Bluetooth comes from the Intel Wireless-AC 8265 in the M.2 E-key slot (its
 * BT half enumerates over the slot's USB lines via btusb; needs the ibt-*
 * firmware from linux-firmware) or from a USB BT dongle. BlueZ ships with
 * JetPack (install `bluez` if missing) and exposes bluetoothctl, which we drive
 * non-interactively by piping a command script into it. Scan blocks for several
 * seconds (it holds a bluetoothctl session open while discovery runs — BlueZ
 * stops discovery as soon as the requesting client exits), so callers should
 * run Scan()/PairAndConnect() off the LVGL thread.
 *
 * No password entry is needed from the UI: bluetoothctl's default-agent handles
 * PIN/Just-Works pairing automatically. */

#include <chrono>
#include <string>
#include <mutex>
#include <vector>

namespace jetson {

// Coarse device category derived from BlueZ's `Icon` hint (`bluetoothctl
// info` prints e.g. "Icon: input-gaming" for gamepads, "Icon: audio-headset"
// for headphones). The numeric values are stable so the UI can publish the
// kind through an atomic<int>.
enum class BtDeviceKind {
    None = 0,        // no connected device
    Controller = 1,  // gamepad / joystick
    Headphones = 2,  // headset / headphones / other audio sink
    Unknown = 3,     // device present but not a recognized category
};

// Status-icon asset (assets/icons/app) for a device category. Single source
// for the Dynamic Island mini icon and the Bluetooth device lists.
inline const char *BtKindIconName(BtDeviceKind kind) {
    switch (kind) {
        case BtDeviceKind::Controller: return "controller-mini";
        case BtDeviceKind::Headphones: return "headphones";
        default: return "unknow-device";
    }
}

struct BtDevice {
    std::string address;   // "AA:BB:CC:DD:EE:FF"
    std::string name;
    bool paired = false;
    bool connected = false;
    int rssi = 0;          // dBm, negative (0 if unknown)
    BtDeviceKind kind = BtDeviceKind::Unknown;
};

class IBluetoothManager {
public:
    virtual ~IBluetoothManager() = default;

    virtual bool Available() const = 0;
    virtual bool PowerOn() = 0;
    virtual bool PowerOff() = 0;
    virtual bool IsPowered() const = 0;
    virtual std::vector<BtDevice> Scan(int duration_s = 8) = 0;
    virtual BtDeviceKind ConnectedDeviceKind() const = 0;
    virtual bool PairAndConnect(const std::string &address) = 0;
    virtual bool Disconnect(const std::string &address) = 0;
    virtual bool Remove(const std::string &address) = 0;
    virtual std::string LastError() const = 0;
};

class BluetoothManager final : public IBluetoothManager {
public:
    static BluetoothManager &Instance();

    // True if bluetoothctl is installed and a controller is present.
    bool Available() const override;

    // Power on the adapter (no-op if already on).
    bool PowerOn() override;
    // Power off the adapter.
    bool PowerOff() override;
    // True if the adapter is powered on (`bluetoothctl show` -> Powered: yes).
    bool IsPowered() const override;

    // Scan for `duration_s` seconds, then return all known devices. Blocking.
    std::vector<BtDevice> Scan(int duration_s = 8) override;

    // Category of the currently connected paired device (best match when
    // several are connected: controller > headphones > unknown). Blocking
    // (one `info` per paired device) — call off the LVGL thread.
    BtDeviceKind ConnectedDeviceKind() const override;

    // Pair + trust + connect to a device by address. Returns true if connected.
    bool PairAndConnect(const std::string &address) override;

    bool Disconnect(const std::string &address) override;
    bool Remove(const std::string &address) override;

    std::string LastError() const override {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return last_error_;
    }

private:
    BluetoothManager() = default;

    // Make sure bluetoothd is reachable, (re)starting bluetooth.service if
    // needed. With `force_restart` the service is restarted even if systemd
    // reports it active (bluetoothctl can still fail to reach it on D-Bus).
    // Failed start attempts are rate-limited so every UI call afterwards
    // fails fast instead of burning a full bluetoothctl timeout.
    bool EnsureDaemon(bool force_restart = false) const;

    mutable std::recursive_mutex mutex_;
    mutable std::string last_error_;
    mutable std::chrono::steady_clock::time_point daemon_retry_after_{};
};

} // namespace jetson
