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
#include <vector>

namespace jetson {

struct BtDevice {
    std::string address;   // "AA:BB:CC:DD:EE:FF"
    std::string name;
    bool paired = false;
    bool connected = false;
    int rssi = 0;          // dBm, negative (0 if unknown)
};

class BluetoothManager {
public:
    static BluetoothManager &Instance();

    // True if bluetoothctl is installed and a controller is present.
    bool Available() const;

    // Power on the adapter (no-op if already on).
    bool PowerOn();

    // Scan for `duration_s` seconds, then return all known devices. Blocking.
    std::vector<BtDevice> Scan(int duration_s = 8);

    // Pair + trust + connect to a device by address. Returns true if connected.
    bool PairAndConnect(const std::string &address);

    bool Disconnect(const std::string &address);
    bool Remove(const std::string &address);

    std::string LastError() const { return last_error_; }

private:
    BluetoothManager() = default;
    mutable std::string last_error_;
};

} // namespace jetson