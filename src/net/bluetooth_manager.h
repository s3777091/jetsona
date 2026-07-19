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
 * Just-Works devices need no password. Bluetooth keyboards receive a passkey
 * callback so the UI can tell the user what to type on the remote keyboard. */

#include <chrono>
#include <functional>
#include <string>
#include <mutex>
#include <vector>

namespace jetson {

// Called while pairing a keyboard when BlueZ asks the user to type a six-digit
// passkey on that keyboard and press Enter.
using BtPairingPromptCb = std::function<void(const std::string &passkey)>;

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
    // Bonded devices only — no discovery, so it answers in ~1-2 s and works
    // for devices that are currently off/asleep. Default keeps existing
    // test/fake managers source-compatible.
    virtual std::vector<BtDevice> PairedDevices() { return {}; }
    virtual BtDeviceKind ConnectedDeviceKind() const = 0;
    virtual bool PairAndConnect(const std::string &address) = 0;
    // Pair with an optional interactive passkey prompt. Existing test/fake
    // managers only implementing PairAndConnect remain source-compatible.
    virtual bool PairAndConnectWithPrompt(const std::string &address,
                                          BtPairingPromptCb prompt_cb) {
        (void)prompt_cb;
        return PairAndConnect(address);
    }
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

    // Previously paired devices straight from BlueZ's bonding store, without
    // running discovery. Lets the UI list "old" devices immediately.
    std::vector<BtDevice> PairedDevices() override;

    // Category of the currently connected paired device (best match when
    // several are connected: controller > headphones > unknown). Blocking
    // (one `info` per paired device) — call off the LVGL thread.
    BtDeviceKind ConnectedDeviceKind() const override;

    // Pair + trust + connect to a device by address. Returns true if connected.
    bool PairAndConnect(const std::string &address) override;
    bool PairAndConnectWithPrompt(const std::string &address,
                                  BtPairingPromptCb prompt_cb) override;

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
