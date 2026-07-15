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
#include <mutex>
#include <vector>

namespace jetson {

struct WifiNetwork {
    std::string ssid;
    int signal = 0;      // 0..100 (nmcli SIGNAL)
    bool secured = false; // has WPA/WEP/etc.
    bool in_use = false;  // currently connected
    bool known = false;   // NetworkManager has a saved connection profile
    std::string security; // WPA2/WPA3/WEP/etc. as reported by NetworkManager
    std::string bssid;    // strongest/current access-point address
};

struct WifiDetails {
    std::string ssid;
    int signal = 0;
    bool connected = false;
    bool known = false;
    std::string security;
    std::string bssid;
    std::string interface_name;
    std::string adapter_address;
    std::string ip_address;
    std::string gateway;
    std::string dns;
    std::string channel;
    std::string frequency;
    std::string rate;
    std::string password; // populated only by an explicit Details() request
};

class IWifiManager {
public:
    virtual ~IWifiManager() = default;

    virtual bool Available() const = 0;
    virtual bool IsEnabled() const = 0;
    virtual bool Enable(bool on) = 0;
    virtual std::string ActiveSsid() const = 0;
    virtual std::vector<WifiNetwork> Scan() = 0;
    // Potentially shells out several times and may reveal a stored password;
    // call only from a worker after the user explicitly opens WiFi details.
    virtual WifiDetails Details(const std::string &ssid) { return WifiDetails{ssid}; }
    virtual bool Connect(const std::string &ssid, const std::string &password) = 0;
    virtual bool Disconnect() = 0;
    virtual bool Forget(const std::string &ssid) = 0;
    virtual std::string LastError() const = 0;
};

class WifiManager final : public IWifiManager {
public:
    static WifiManager &Instance();

    // True if nmcli is installed and NetworkManager is active.
    bool Available() const override;

    // True if the WiFi radio is powered on (`nmcli radio wifi`).
    bool IsEnabled() const override;
    // Power the WiFi radio on/off (`nmcli radio wifi on|off`).
    bool Enable(bool on) override;

    // SSID of the currently connected WiFi, or "" if none.
    std::string ActiveSsid() const override;

    // Scan and return networks sorted by signal desc. Blocking (~1-2s).
    std::vector<WifiNetwork> Scan() override;

    WifiDetails Details(const std::string &ssid) override;

    // Connect to an SSID. password may be empty for open networks.
    // Returns true on success; sets LastError() on failure.
    bool Connect(const std::string &ssid, const std::string &password) override;

    bool Disconnect() override;
    bool Forget(const std::string &ssid) override;

    std::string LastError() const override {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return last_error_;
    }

private:
    WifiManager() = default;
    mutable std::recursive_mutex mutex_;
    mutable std::string last_error_;
};

} // namespace jetson
