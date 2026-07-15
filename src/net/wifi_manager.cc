#include "wifi_manager.h"
#include "esp_log.h"
#include "platform/shell_command.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <unordered_map>

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
    if (!Available()) {
        ESP_LOGE(TAG, "scan stopped: %s", last_error_.c_str());
        return nets;
    }

    // --escape no so SSID text is literal; we parse from the right to tolerate
    // SSIDs that contain ':'.
    std::string out;
    int rc = RunShellCommand("nmcli -w 20 -t --escape no -f IN-USE,SSID,SIGNAL,SECURITY "
                    "device wifi list --rescan yes", out);
    if (rc != 0) {
        last_error_ = "scan failed: " + out;
        ESP_LOGW(TAG, "active rescan failed (rc=%d), using cached AP list: %s",
                 rc, out.c_str());
        // Fall back to a no-rescan list (still useful).
        rc = RunShellCommand("nmcli -w 5 -t --escape no -f IN-USE,SSID,SIGNAL,SECURITY "
                             "device wifi list --rescan no", out);
        if (rc != 0) {
            last_error_ = "cached scan failed: " + out;
            ESP_LOGE(TAG, "cached AP list failed (rc=%d): %s", rc, out.c_str());
            return nets;
        }
    }

    std::istringstream iss(out);
    std::string line;
    std::unordered_map<std::string, size_t> by_ssid;
    while (std::getline(iss, line)) {
        if (line.empty()) continue;

        // Field 1: IN-USE ("*" or empty).
        std::string inUseField, rest;
        splitFirst(line, ':', inUseField, rest);
        if (rest.empty()) continue;

        // rest = SSID:SIGNAL:SECURITY  (SSID may contain ':').
        std::string before, security;
        splitLast(rest, ':', before, security);
        std::string ssid, signalStr;
        splitLast(before, ':', ssid, signalStr);

        if (ssid.empty() || ssid == "--") continue; // hidden / empty SSID

        WifiNetwork n;
        n.ssid = ssid;
        n.signal = toInt(signalStr);
        n.in_use = (inUseField.find('*') != std::string::npos);
        n.secured = !security.empty() && security != "--";
        // NetworkManager reports one row per access point/BSSID.  Collapse
        // duplicate SSIDs while preserving the strongest signal and active
        // state, otherwise a busy scan can show many identical rows.
        auto existing = by_ssid.find(n.ssid);
        if (existing == by_ssid.end()) {
            by_ssid.emplace(n.ssid, nets.size());
            nets.push_back(std::move(n));
        } else {
            auto &saved = nets[existing->second];
            saved.signal = std::max(saved.signal, n.signal);
            saved.in_use = saved.in_use || n.in_use;
            saved.secured = saved.secured || n.secured;
        }
    }

    std::sort(nets.begin(), nets.end(), [](const WifiNetwork &a, const WifiNetwork &b) {
        if (a.in_use != b.in_use) return a.in_use; // connected first
        return a.signal > b.signal;                // then by signal desc
    });
    for (const auto &net : nets) {
        ESP_LOGI(TAG, "AP ssid='%s' signal=%d secure=%d active=%d",
                 net.ssid.c_str(), net.signal, net.secured ? 1 : 0, net.in_use ? 1 : 0);
    }
    last_error_.clear();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    ESP_LOGI(TAG, "scan completed: %zu networks in %lld ms",
             nets.size(), static_cast<long long>(elapsed));
    return nets;
}

bool WifiManager::Connect(const std::string &ssid, const std::string &password) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!Available()) return false;
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
    int rc = RunShellCommand("nmcli -w 10 connection delete " +
                             QuoteShellArgument("jetson-fw-" + ssid), out);
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
