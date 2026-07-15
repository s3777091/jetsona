#include "bluetooth_manager.h"
#include "esp_log.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#define TAG "Bt"

namespace jetson {

namespace {

int runCmd(const std::string &cmd, std::string &out) {
    out.clear();
    std::string full = cmd + " 2>&1";
    FILE *p = popen(full.c_str(), "r");
    if (!p) return -1;
    char buf[1024];
    while (fgets(buf, sizeof(buf), p)) out += buf;
    return pclose(p);
}

std::string sh(const std::string &s) {
    std::string r = "'";
    for (char c : s) {
        if (c == '\'') r += "'\\''";
        else r += c;
    }
    r += "'";
    return r;
}

// Run a multi-line bluetoothctl command script (piped to stdin).
int runBt(const std::string &script, std::string &out) {
    std::string cmd = "printf '%s' " + sh(script) + " | bluetoothctl";
    return runCmd(cmd, out);
}

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

// Parse "Name: value" -> value (first match, trimmed).
std::string field(const std::string &out, const std::string &key) {
    std::string k = key + ":";
    size_t p = out.find(k);
    if (p == std::string::npos) return "";
    p += k.size();
    while (p < out.size() && (out[p] == ' ' || out[p] == '\t')) ++p;
    size_t end = out.find('\n', p);
    std::string v = (end == std::string::npos) ? out.substr(p) : out.substr(p, end - p);
    // trim trailing whitespace/CR
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) v.pop_back();
    return v;
}

bool fieldBool(const std::string &out, const std::string &key) {
    return field(out, key) == "yes";
}

} // namespace

BluetoothManager &BluetoothManager::Instance() {
    static BluetoothManager inst;
    return inst;
}

bool BluetoothManager::Available() const {
    std::string out;
    int rc = runCmd("bluetoothctl show", out);
    if (rc != 0) { last_error_ = "bluetoothctl not available"; return false; }
    // `bluetoothctl show` prints "Controller <addr> <name>" when a controller
    // exists; otherwise "No default controller available".
    return contains(out, "Controller");
}

bool BluetoothManager::PowerOn() {
    std::string out;
    int rc = runBt("power on\n", out);
    if (rc != 0) { last_error_ = out; return false; }
    return true;
}

bool BluetoothManager::PowerOff() {
    std::string out;
    int rc = runBt("power off\n", out);
    if (rc != 0) { last_error_ = out; return false; }
    return true;
}

bool BluetoothManager::IsPowered() const {
    if (!Available()) return false;
    std::string out;
    runCmd("bluetoothctl show", out);
    return fieldBool(out, "Powered");
}

std::vector<BtDevice> BluetoothManager::Scan(int duration_s) {
    std::vector<BtDevice> devs;
    if (!Available()) return devs;
    PowerOn();

    // Warm up the scan for a few seconds so fresh devices appear, then list.
    std::string scanCmd = "timeout " + std::to_string(duration_s) +
                          " bluetoothctl scan on";
    std::string junk;
    runCmd(scanCmd, junk);

    std::string devicesOut;
    runBt("devices\n", devicesOut);

    std::istringstream iss(devicesOut);
    std::string line;
    while (std::getline(iss, line)) {
        // Lines: "Device AA:BB:CC:DD:EE:FF Name with spaces"
        const char *prefix = "Device ";
        size_t p = line.find(prefix);
        if (p == std::string::npos) continue;
        std::string rest = line.substr(p + std::strlen(prefix));
        // first token = address
        size_t sp = rest.find(' ');
        if (sp == std::string::npos) continue;
        BtDevice d;
        d.address = rest.substr(0, sp);
        d.name = rest.substr(sp + 1);
        // trim
        while (!d.name.empty() && (d.name.front() == ' ' || d.name.front() == '\t')) d.name.erase(d.name.begin());
        while (!d.name.empty() && (d.name.back() == ' ' || d.name.back() == '\t' || d.name.back() == '\r')) d.name.pop_back();
        if (d.name.empty()) d.name = "(không tên)";

        // Per-device info for state + RSSI.
        std::string info;
        runBt("info " + d.address + "\n", info);
        d.paired = fieldBool(info, "Paired");
        d.connected = fieldBool(info, "Connected");
        std::string rssi = field(info, "RSSI");
        if (!rssi.empty()) {
            try { d.rssi = std::stoi(rssi); } catch (...) { d.rssi = 0; }
        }
        devs.push_back(d);
    }

    std::sort(devs.begin(), devs.end(), [](const BtDevice &a, const BtDevice &b) {
        if (a.connected != b.connected) return a.connected;
        if (a.paired != b.paired) return a.paired;
        return a.rssi > b.rssi; // stronger (closer to 0) first
    });
    return devs;
}

bool BluetoothManager::PairAndConnect(const std::string &address) {
    if (!Available()) return false;
    PowerOn();
    std::string script =
        "agent on\n"
        "default-agent\n"
        "pair " + address + "\n"
        "trust " + address + "\n"
        "connect " + address + "\n";
    std::string out;
    int rc = runBt(script, out);
    (void)rc; // bluetoothctl exits 0 even on partial failure; verify via info.

    std::string info;
    runBt("info " + address + "\n", info);
    if (fieldBool(info, "Connected")) {
        ESP_LOGI(TAG, "connected to %s", address.c_str());
        return true;
    }
    last_error_ = "pair/connect failed: " + out;
    ESP_LOGE(TAG, "pair/connect %s failed: %s", address.c_str(), out.c_str());
    return false;
}

bool BluetoothManager::Disconnect(const std::string &address) {
    std::string out;
    runBt("disconnect " + address + "\n", out);
    std::string info;
    runBt("info " + address + "\n", info);
    if (!fieldBool(info, "Connected")) return true;
    last_error_ = out;
    return false;
}

bool BluetoothManager::Remove(const std::string &address) {
    std::string out;
    int rc = runBt("remove " + address + "\n", out);
    if (rc != 0) { last_error_ = out; return false; }
    return true;
}

} // namespace jetson