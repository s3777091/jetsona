#pragma once

/* Linux WiFi manager backed by NetworkManager (`nmcli`).
 *
 * The Jetson Nano 4GB B01 has no onboard WiFi — a USB WiFi dongle must be
 * plugged in. NetworkManager ships with JetPack and exposes nmcli, which we
 * shell out to (popen) for scan / connect / status. nmcli blocks for ~1-2s on
 * a rescan, so callers should run Scan()/Connect() off the LVGL thread.
 *
 * All methods are thread-safe enough for our usage (each is an independent
 * nmcli invocation); they do not touch LVGL. */

#include <string>
#include <vector>

namespace jetson {

struct WifiNetwork {
    std::string ssid;
    int signal = 0;      // 0..100 (nmcli SIGNAL)
    bool secured = false; // has WPA/WEP/etc.
    bool in_use = false;  // currently connected
};

class WifiManager {
public:
    static WifiManager &Instance();

    // True if nmcli is installed and NetworkManager is active.
    bool Available() const;

    // SSID of the currently connected WiFi, or "" if none.
    std::string ActiveSsid() const;

    // Scan and return networks sorted by signal desc. Blocking (~1-2s).
    std::vector<WifiNetwork> Scan();

    // Connect to an SSID. password may be empty for open networks.
    // Returns true on success; sets LastError() on failure.
    bool Connect(const std::string &ssid, const std::string &password);

    bool Disconnect();
    bool Forget(const std::string &ssid);

    std::string LastError() const { return last_error_; }

private:
    WifiManager() = default;
    mutable std::string last_error_;
};

} // namespace jetson