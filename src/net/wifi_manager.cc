#include "wifi_manager.h"
#include "esp_log.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#define TAG "Wifi"

namespace jetson {

namespace {

// Run `cmd` via /bin/sh, capturing combined stdout+stderr. Returns exit code
// (0 on success). out receives the captured output.
int runCmd(const std::string &cmd, std::string &out) {
    out.clear();
    // 2>&1 so we see nmcli error text.
    std::string full = cmd + " 2>&1";
    FILE *p = popen(full.c_str(), "r");
    if (!p) return -1;
    char buf[1024];
    while (fgets(buf, sizeof(buf), p)) out += buf;
    return pclose(p); // exit status encoded as status << 8
}

// Single-quote-escape a value for safe shell interpolation: foo'bar -> 'foo'\''bar'
std::string sh(const std::string &s) {
    std::string r = "'";
    for (char c : s) {
        if (c == '\'') r += "'\\''";
        else r += c;
    }
    r += "'";
    return r;
}

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
    std::string out;
    int rc = runCmd("nmcli -t -f STATE general", out);
    if (rc != 0) {
        last_error_ = "nmcli not available: " + out;
        return false;
    }
    // STATE is "connected" / "asleep" / "disconnected" etc. Anything but errors
    // means NetworkManager is running.
    return true;
}

bool WifiManager::IsEnabled() const {
    // `nmcli -t -f WIFI radio wifi` prints "enabled" / "disabled".
    std::string out;
    int rc = runCmd("nmcli -t -f WIFI radio wifi", out);
    if (rc != 0) { last_error_ = out; return false; }
    // Trim whitespace/newlines.
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' ||
                            out.back() == ' ' || out.back() == '\t')) {
        out.pop_back();
    }
    return out == "enabled";
}

bool WifiManager::Enable(bool on) {
    std::string out;
    int rc = runCmd(on ? "nmcli radio wifi on" : "nmcli radio wifi off", out);
    if (rc != 0) { last_error_ = out; return false; }
    return true;
}

std::string WifiManager::ActiveSsid() const {
    std::string out;
    int rc = runCmd("nmcli -t -f ACTIVE,SSID dev wifi", out);
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
    std::vector<WifiNetwork> nets;
    if (!Available()) return nets;

    // --escape no so SSID text is literal; we parse from the right to tolerate
    // SSIDs that contain ':'.
    std::string out;
    int rc = runCmd("nmcli -t --escape no -f IN-USE,SSID,SIGNAL,SECURITY "
                    "device wifi list --rescan yes", out);
    if (rc != 0) {
        last_error_ = "scan failed: " + out;
        // Fall back to a no-rescan list (still useful).
        runCmd("nmcli -t --escape no -f IN-USE,SSID,SIGNAL,SECURITY device wifi list", out);
    }

    std::istringstream iss(out);
    std::string line;
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
        nets.push_back(n);
    }

    std::sort(nets.begin(), nets.end(), [](const WifiNetwork &a, const WifiNetwork &b) {
        if (a.in_use != b.in_use) return a.in_use; // connected first
        return a.signal > b.signal;                // then by signal desc
    });
    return nets;
}

bool WifiManager::Connect(const std::string &ssid, const std::string &password) {
    if (!Available()) return false;
    std::string name = "jetson-fw-" + ssid;
    std::string cmd = "nmcli device wifi connect " + sh(ssid) +
                      " name " + sh(name);
    if (!password.empty()) cmd += " password " + sh(password);
    std::string out;
    int rc = runCmd(cmd, out);
    if (rc != 0) {
        last_error_ = "connect failed: " + out;
        ESP_LOGE(TAG, "connect '%s' failed: %s", ssid.c_str(), out.c_str());
        return false;
    }
    ESP_LOGI(TAG, "connected to '%s'", ssid.c_str());
    return true;
}

bool WifiManager::Disconnect() {
    std::string out;
    // Find the wifi interface, then disconnect it.
    int rc = runCmd("nmcli -t -f DEVICE,TYPE,STATE dev", out);
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
    rc = runCmd("nmcli dev disconnect " + sh(ifname), out);
    if (rc != 0) { last_error_ = out; return false; }
    return true;
}

bool WifiManager::Forget(const std::string &ssid) {
    std::string out;
    int rc = runCmd("nmcli connection delete " + sh("jetson-fw-" + ssid), out);
    if (rc != 0) {
        // try by SSID directly
        rc = runCmd("nmcli connection delete " + sh(ssid), out);
    }
    return rc == 0;
}

} // namespace jetson