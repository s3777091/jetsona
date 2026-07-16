#include "wifi_manager.h"
#include "net/airplane_mode.h"
#include "esp_log.h"
#include "platform/shell_command.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <unordered_map>
#include <vector>

#define TAG "Wifi"

namespace jetson {

namespace {
using platform::QuoteShellArgument;
using platform::RunShellCommand;

// Split `s` on the first `:` -> [head, tail].
void splitFirst(const std::string &s, char sep, std::string &head, std::string &tail) {
    size_t p = s.find(sep);
    if (p == std::string::npos) { head = s; tail.clear(); return; }
    head = s.substr(0, p);
    tail = s.substr(p + 1);
}

// Right-split `s` on `:` -> [head, last] (last = text after the final ':').
void splitLast(const std::string &s, char sep, std::string &head, std::string &last) {
    size_t p = s.rfind(sep);
    if (p == std::string::npos) { head.clear(); last = s; return; }
    head = s.substr(0, p);
    last = s.substr(p + 1);
}

int toInt(const std::string &s) {
    if (s.empty()) return 0;
    try { return std::stoi(s); } catch (...) { return 0; }
}

void trimTrailingWhitespace(std::string &s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                          s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
}

// nmcli terse output escapes separators as `\:`. Split while removing the
// escape marker so SSIDs/profile names containing ':' remain intact.
std::vector<std::string> splitNmcliFields(const std::string &line) {
    std::vector<std::string> fields(1);
    bool escaped = false;
    for (char c : line) {
        if (escaped) {
            fields.back().push_back(c);
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == ':') {
            fields.emplace_back();
        } else {
            fields.back().push_back(c);
        }
    }
    if (escaped) fields.back().push_back('\\');
    return fields;
}

using SavedProfiles = std::unordered_map<std::string, std::string>; // SSID -> profile name

SavedProfiles loadSavedWifiProfiles() {
    SavedProfiles profiles;
    std::string out;
    if (RunShellCommand("nmcli -w 5 -t --escape yes -f NAME,TYPE connection show", out) != 0)
        return profiles;

    std::istringstream iss(out);
    std::string line;
    while (std::getline(iss, line)) {
        auto fields = splitNmcliFields(line);
        if (fields.size() < 2 ||
            (fields[1] != "wifi" && fields[1] != "802-11-wireless")) {
            continue;
        }

        const std::string &profile = fields[0];
        std::string ssid;
        std::string ssidOut;
        if (RunShellCommand("nmcli -w 5 --escape no -g 802-11-wireless.ssid connection show " +
                            QuoteShellArgument(profile), ssidOut) == 0) {
            trimTrailingWhitespace(ssidOut);
            ssid = std::move(ssidOut);
        }
        // Profiles created by older firmware builds always use this prefix.
        // Keep the fallback for NetworkManager versions that cannot print the
        // setting through `-g`.
        if (ssid.empty() && profile.rfind("jetson-fw-", 0) == 0)
            ssid = profile.substr(std::strlen("jetson-fw-"));
        if (ssid.empty()) ssid = profile;
        profiles.emplace(std::move(ssid), profile);
    }
    return profiles;
}

void appendCsv(std::string &dst, const std::string &value) {
    if (value.empty() || value == "--") return;
    if (!dst.empty()) dst += ", ";
    dst += value;
}

} // namespace

WifiManager &WifiManager::Instance() {
    static WifiManager inst;
    return inst;
}

bool WifiManager::Available() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::string out;
    int rc = RunShellCommand("nmcli -w 3 -t -f STATE general", out);
    if (rc != 0) {
        last_error_ = "nmcli not available: " + out;
        ESP_LOGE(TAG, "availability check failed (rc=%d): %s", rc, out.c_str());
        return false;
    }
    // STATE is "connected" / "asleep" / "disconnected" etc. Anything but errors
    // means NetworkManager is running.
    last_error_.clear();
    return true;
}

bool WifiManager::IsEnabled() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // `nmcli -t -f WIFI radio wifi` prints "enabled" / "disabled".
    std::string out;
    int rc = RunShellCommand("nmcli -w 3 -t -f WIFI radio wifi", out);
    if (rc != 0) {
        last_error_ = out;
        ESP_LOGE(TAG, "radio state failed (rc=%d): %s", rc, out.c_str());
        return false;
    }
    // Trim whitespace/newlines.
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' ||
                            out.back() == ' ' || out.back() == '\t')) {
        out.pop_back();
    }
    last_error_.clear();
    return out == "enabled";
}

bool WifiManager::Enable(bool on) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (on && IsAirplaneModeEnabled()) {
        last_error_ = "Chế độ máy bay đang bật";
        ESP_LOGW(TAG, "radio on rejected while airplane mode is enabled");
        return false;
    }
    std::string out;
    int rc = RunShellCommand(on ? "nmcli -w 8 radio wifi on" : "nmcli -w 8 radio wifi off", out);
    if (rc != 0) {
        last_error_ = out;
        ESP_LOGE(TAG, "radio %s failed (rc=%d): %s", on ? "on" : "off", rc, out.c_str());
        return false;
    }
    last_error_.clear();
    ESP_LOGI(TAG, "radio %s", on ? "on" : "off");
    return true;
}

std::string WifiManager::ActiveSsid() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::string out;
    int rc = RunShellCommand("nmcli -w 5 -t --escape no -f ACTIVE,SSID dev wifi", out);
    if (rc != 0) return "";
    std::istringstream iss(out);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.empty()) continue;
        std::string active, ssid;
        splitFirst(line, ':', active, ssid);
        if (active == "yes" && !ssid.empty() && ssid != "--") return ssid;
    }
    return "";
}

std::vector<WifiNetwork> WifiManager::Scan() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const auto started = std::chrono::steady_clock::now();
    std::vector<WifiNetwork> nets;
    ESP_LOGI(TAG, "scan started");
    if (IsAirplaneModeEnabled()) {
        last_error_ = "Chế độ máy bay đang bật";
        ESP_LOGW(TAG, "scan rejected while airplane mode is enabled");
        return nets;
    }
    if (!Available()) {
        ESP_LOGE(TAG, "scan stopped: %s", last_error_.c_str());
        return nets;
    }

    // Force a REAL scan of all surrounding APs (not just the connected one).
    // `nmcli ... list --rescan yes` can return in ~100 ms with the stale cache
    // (NetworkManager throttles redundant rescan requests, and the connect
    // operation only cached the joined BSSID). A separate `device wifi rescan`
    // blocks until a fresh scan completes (~2-5 s), so the list that follows
    // reflects the real surroundings. Ignore its rc -- some drivers reject a
    // rescan while associated, in which case we still list the cached APs.
    std::string rescanOut;
    int rescanRc = RunShellCommand("nmcli -w 15 device wifi rescan", rescanOut);
    if (rescanRc != 0) {
        ESP_LOGW(TAG, "rescan request failed (rc=%d): %s -- listing cached APs",
                 rescanRc, rescanOut.c_str());
    } else {
        ESP_LOGI(TAG, "rescan done");
    }

    // Keep escaping enabled and parse it below. This lets both SSIDs and BSSIDs
    // contain ':' without making the terse output ambiguous.
    std::string out;
    int rc = RunShellCommand("nmcli -w 10 -t --escape yes -f IN-USE,SSID,SIGNAL,SECURITY,BSSID "
                    "device wifi list --rescan no", out);
    if (rc != 0) {
        last_error_ = "scan failed: " + out;
        ESP_LOGW(TAG, "AP list failed (rc=%d), retrying with rescan: %s",
                 rc, out.c_str());
        // Fall back to letting nmcli rescan itself.
        rc = RunShellCommand("nmcli -w 20 -t --escape yes -f IN-USE,SSID,SIGNAL,SECURITY,BSSID "
                             "device wifi list --rescan yes", out);
        if (rc != 0) {
            last_error_ = "scan failed: " + out;
            ESP_LOGE(TAG, "AP list failed (rc=%d): %s", rc, out.c_str());
            return nets;
        }
    }

    std::istringstream iss(out);
    std::string line;
    std::unordered_map<std::string, size_t> by_ssid;
    while (std::getline(iss, line)) {
        if (line.empty()) continue;

        auto fields = splitNmcliFields(line);
        if (fields.size() < 5 || fields[1].empty() || fields[1] == "--") continue;

        WifiNetwork n;
        n.ssid = fields[1];
        n.signal = toInt(fields[2]);
        n.in_use = (fields[0].find('*') != std::string::npos);
        n.security = fields[3];
        n.secured = !n.security.empty() && n.security != "--";
        n.bssid = fields[4] == "--" ? "" : fields[4];
        // NetworkManager reports one row per access point/BSSID.  Collapse
        // duplicate SSIDs while preserving the strongest signal and active
        // state, otherwise a busy scan can show many identical rows.
        auto existing = by_ssid.find(n.ssid);
        if (existing == by_ssid.end()) {
            by_ssid.emplace(n.ssid, nets.size());
            nets.push_back(std::move(n));
        } else {
            auto &saved = nets[existing->second];
            const bool preferMetadata = n.in_use || (!saved.in_use && n.signal > saved.signal);
            saved.signal = std::max(saved.signal, n.signal);
            saved.in_use = saved.in_use || n.in_use;
            saved.secured = saved.secured || n.secured;
            if (preferMetadata) {
                saved.security = std::move(n.security);
                saved.bssid = std::move(n.bssid);
            }
        }
    }

    const SavedProfiles profiles = loadSavedWifiProfiles();
    for (auto &net : nets) net.known = profiles.find(net.ssid) != profiles.end();

    std::sort(nets.begin(), nets.end(), [](const WifiNetwork &a, const WifiNetwork &b) {
        if (a.in_use != b.in_use) return a.in_use; // connected first
        return a.signal > b.signal;                // then by signal desc
    });
    for (const auto &net : nets) {
        ESP_LOGI(TAG, "AP ssid='%s' signal=%d secure=%d active=%d known=%d",
                 net.ssid.c_str(), net.signal, net.secured ? 1 : 0,
                 net.in_use ? 1 : 0, net.known ? 1 : 0);
    }
    last_error_.clear();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    ESP_LOGI(TAG, "scan completed: %zu networks in %lld ms",
             nets.size(), static_cast<long long>(elapsed));
    return nets;
}

WifiDetails WifiManager::Details(const std::string &ssid) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    WifiDetails details;
    details.ssid = ssid;

    const SavedProfiles profiles = loadSavedWifiProfiles();
    auto saved = profiles.find(ssid);
    details.known = saved != profiles.end();

    // Read cached AP metadata only; opening the info sheet must be quick and
    // must not start another visible rescan.
    std::string aps;
    if (RunShellCommand(
            "nmcli -w 5 -t --escape yes -f IN-USE,SSID,BSSID,SIGNAL,SECURITY,CHAN,FREQ,RATE "
            "device wifi list --rescan no", aps) == 0) {
        std::istringstream iss(aps);
        std::string line;
        int bestSignal = -1;
        while (std::getline(iss, line)) {
            auto f = splitNmcliFields(line);
            if (f.size() < 8 || f[1] != ssid) continue;
            const bool active = f[0].find('*') != std::string::npos;
            const int signal = toInt(f[3]);
            if (!active && details.connected) continue;
            if (!active && signal < bestSignal) continue;
            details.connected = active;
            details.bssid = f[2] == "--" ? "" : f[2];
            details.signal = signal;
            details.security = f[4] == "--" ? "Mở" : f[4];
            details.channel = f[5] == "--" ? "" : f[5];
            details.frequency = f[6] == "--" ? "" : f[6] + " MHz";
            details.rate = f[7] == "--" ? "" : f[7];
            bestSignal = signal;
        }
    }
    if (!details.connected) details.connected = ActiveSsid() == ssid;

    if (details.known) {
        std::string secret;
        if (RunShellCommand("nmcli -w 5 -s -g 802-11-wireless-security.psk connection show " +
                            QuoteShellArgument(saved->second), secret) == 0) {
            trimTrailingWhitespace(secret);
            if (secret != "--") details.password = std::move(secret);
        }
    }

    // The adapter address is useful even for an AP that is not connected, so
    // resolve the Wi-Fi interface unconditionally. IPv4 fields will naturally
    // stay empty while the interface has no active lease.
    std::string devices;
    if (RunShellCommand("nmcli -w 5 -t --escape yes -f DEVICE,TYPE,STATE,CONNECTION "
                        "device status", devices) == 0) {
        std::istringstream iss(devices);
        std::string line;
        while (std::getline(iss, line)) {
            auto f = splitNmcliFields(line);
            if (f.size() < 4 || f[1] != "wifi") continue;
            if (details.interface_name.empty() || f[2] == "connected")
                details.interface_name = f[0];
            if (f[2] == "connected") break;
        }
    }

    if (!details.interface_name.empty()) {
        std::string deviceInfo;
        if (RunShellCommand(
                "nmcli -w 5 -t --escape no -f GENERAL.HWADDR,IP4.ADDRESS,IP4.GATEWAY,IP4.DNS "
                "device show " + QuoteShellArgument(details.interface_name), deviceInfo) == 0) {
            std::istringstream iss(deviceInfo);
            std::string line;
            while (std::getline(iss, line)) {
                std::string key, value;
                splitFirst(line, ':', key, value);
                if (key == "GENERAL.HWADDR") details.adapter_address = value;
                else if (details.connected && key.rfind("IP4.ADDRESS", 0) == 0 &&
                         details.ip_address.empty())
                    details.ip_address = value;
                else if (details.connected && key == "IP4.GATEWAY") details.gateway = value;
                else if (details.connected && key.rfind("IP4.DNS", 0) == 0)
                    appendCsv(details.dns, value);
            }
        }
    }

    last_error_.clear();
    return details;
}

bool WifiManager::Connect(const std::string &ssid, const std::string &password) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (IsAirplaneModeEnabled()) {
        last_error_ = "Chế độ máy bay đang bật";
        ESP_LOGW(TAG, "connection rejected while airplane mode is enabled");
        return false;
    }
    if (!Available()) return false;
    const SavedProfiles profiles = loadSavedWifiProfiles();
    auto saved = profiles.find(ssid);
    if (password.empty() && saved != profiles.end()) {
        std::string out;
        ESP_LOGI(TAG, "activating saved WiFi profile for '%s'", ssid.c_str());
        const int rc = RunShellCommand("nmcli -w 25 connection up id " +
                                       QuoteShellArgument(saved->second), out);
        if (rc != 0) {
            last_error_ = "connect failed: " + out;
            ESP_LOGE(TAG, "saved connection '%s' failed (rc=%d): %s",
                     ssid.c_str(), rc, out.c_str());
            return false;
        }
        last_error_.clear();
        return true;
    }

    std::string name = "jetson-fw-" + ssid;
    std::string cmd = "nmcli device wifi connect " + QuoteShellArgument(ssid) +
                      " name " + QuoteShellArgument(name);
    if (!password.empty()) cmd += " password " + QuoteShellArgument(password);
    std::string out;
    // nmcli's own wait bound prevents a bad driver or missing AP from hanging
    // the worker forever.  Do not log cmd: it contains the Wi-Fi password.
    cmd.insert(6, "-w 25 ");
    ESP_LOGI(TAG, "connecting to '%s'", ssid.c_str());
    int rc = RunShellCommand(cmd, out);
    if (rc != 0) {
        last_error_ = "connect failed: " + out;
        ESP_LOGE(TAG, "connect '%s' failed (rc=%d): %s", ssid.c_str(), rc, out.c_str());
        return false;
    }
    last_error_.clear();
    ESP_LOGI(TAG, "connected to '%s'", ssid.c_str());
    return true;
}

bool WifiManager::Disconnect() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::string out;
    // Find the wifi interface, then disconnect it.
    int rc = RunShellCommand("nmcli -w 5 -t -f DEVICE,TYPE,STATE dev", out);
    if (rc != 0) return false;
    std::istringstream iss(out);
    std::string line, ifname;
    while (std::getline(iss, line)) {
        std::string dev, rest;
        splitFirst(line, ':', dev, rest);
        std::string type, state;
        splitFirst(rest, ':', type, state);
        if (type == "wifi") { ifname = dev; break; }
    }
    if (ifname.empty()) { last_error_ = "no wifi device"; return false; }
    rc = RunShellCommand("nmcli -w 10 dev disconnect " + QuoteShellArgument(ifname), out);
    if (rc != 0) {
        last_error_ = out;
        ESP_LOGE(TAG, "disconnect failed (rc=%d): %s", rc, out.c_str());
        return false;
    }
    last_error_.clear();
    ESP_LOGI(TAG, "WiFi disconnected on %s", ifname.c_str());
    return true;
}

bool WifiManager::Forget(const std::string &ssid) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::string out;
    const SavedProfiles profiles = loadSavedWifiProfiles();
    auto saved = profiles.find(ssid);
    const std::string profile = saved == profiles.end() ? "jetson-fw-" + ssid
                                                        : saved->second;
    int rc = RunShellCommand("nmcli -w 10 connection delete " +
                             QuoteShellArgument(profile), out);
    if (rc != 0) {
        // try by SSID directly
        rc = RunShellCommand("nmcli -w 10 connection delete " + QuoteShellArgument(ssid), out);
    }
    if (rc != 0) {
        last_error_ = out;
        ESP_LOGE(TAG, "forget '%s' failed (rc=%d): %s", ssid.c_str(), rc, out.c_str());
        return false;
    }
    last_error_.clear();
    ESP_LOGI(TAG, "forgot '%s'", ssid.c_str());
    return true;
}

} // namespace jetson
