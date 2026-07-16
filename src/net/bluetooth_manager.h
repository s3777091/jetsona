#pragma once

/* Linux Bluetooth manager backed by BlueZ (`bluetoothctl`).
 *
 * The Jetson Nano 4GB B01 has no onboard Bluetooth — a USB BT dongle must be
 * plugged in. BlueZ ships with JetPack (install `bluez` if missing) and exposes
 * bluetoothctl, which we drive non-interactively by piping a command script
 * into it. Scan blocks for several seconds (it runs `bluetoothctl scan on` under
 * `timeout`), so callers should run Scan()/PairAndConnect() off the LVGL thread.
 *
 * No password entry is needed from the UI: bluetoothctl's default-agent handles
 * PIN/Just-Works pairing automatically. */

#include <string>
#include <mutex>
#include <vector>

namespace jetson {

struct BtDevice {
    std::string address;   // "AA:BB:CC:DD:EE:FF"
    std::string name;
    bool paired = false;
    bool connected = false;
    int rssi = 0;          // dBm, negative (0 if unknown)
};

class IBluetoothManager {
public:
    virtual ~IBluetoothManager() = default;

    virtual bool Available() const = 0;
    virtual bool PowerOn() = 0;
    virtual bool PowerOff() = 0;
    virtual bool IsPowered() const = 0;
    virtual std::vector<BtDevice> Scan(int duration_s = 8) = 0;
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
    mutable std::recursive_mutex mutex_;
    mutable std::string last_error_;
};

} // namespace jetson
