#include "bluetooth_manager.h"
#include "esp_log.h"
#include "platform/shell_command.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#define TAG "Bt"

namespace jetson {

namespace {
using platform::QuoteShellArgument;
using platform::RunShellCommand;

// Run a multi-line bluetoothctl command script (piped to stdin).
int runBt(const std::string &script, std::string &out) {
    // A missing/half-initialized BlueZ controller must never leave a detached
    // worker blocked in bluetoothctl forever.
    std::string cmd = "printf '%s' " + QuoteShellArgument(script) +
                      " | timeout --signal=TERM 30s bluetoothctl";
    return RunShellCommand(cmd, out);
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
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::string out;
    int rc = RunShellCommand("timeout --signal=TERM 5s bluetoothctl show", out);
    if (rc != 0) {
        last_error_ = "bluetoothctl not available: " + out;
        ESP_LOGE(TAG, "availability check failed (rc=%d): %s", rc, out.c_str());
        return false;
    }
    // `bluetoothctl show` prints "Controller <addr> <name>" when a controller
    // exists; otherwise "No default controller available".
    const bool found = contains(out, "Controller");
    if (!found) {
        last_error_ = "no Bluetooth controller: " + out;
        ESP_LOGE(TAG, "%s", last_error_.c_str());
    }
    else last_error_.clear();
    return found;
}

bool BluetoothManager::PowerOn() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // The AC8265's BT half is often rfkill-soft-blocked after boot, which makes
    // `power on` fail with org.bluez.Error.Blocked. Unblock first, best-effort.
    std::string rfkillOut;
    RunShellCommand("timeout --signal=TERM 3s rfkill unblock bluetooth", rfkillOut);
    std::string out;
    int rc = RunShellCommand("timeout --signal=TERM 6s bluetoothctl power on", out);
    if (rc != 0) {
        last_error_ = out.empty() ? "bluetoothctl power on failed" : out;
        ESP_LOGE(TAG, "power on failed (rc=%d): %s", rc, out.c_str());
        return false;
    }

    // BlueZ output wording differs between versions. Verify the controller
    // state instead of depending on a particular "succeeded" message.
    std::string state;
    const int stateRc = RunShellCommand("timeout --signal=TERM 5s bluetoothctl show", state);
    if (stateRc != 0 || !fieldBool(state, "Powered")) {
        last_error_ = state.empty() ? "Bluetooth adapter did not turn on" : state;
        ESP_LOGE(TAG, "adapter stayed off (rc=%d): %s", stateRc, state.c_str());
        return false;
    }
    last_error_.clear();
    ESP_LOGI(TAG, "adapter powered on");
    return true;
}

bool BluetoothManager::PowerOff() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::string out;
    int rc = RunShellCommand("timeout --signal=TERM 6s bluetoothctl power off", out);
    if (rc != 0) {
        last_error_ = out.empty() ? "bluetoothctl power off failed" : out;
        ESP_LOGE(TAG, "power off failed (rc=%d): %s", rc, out.c_str());
        return false;
    }

    std::string state;
    const int stateRc = RunShellCommand("timeout --signal=TERM 5s bluetoothctl show", state);
    if (stateRc != 0 || fieldBool(state, "Powered")) {
        last_error_ = state.empty() ? "Bluetooth adapter did not turn off" : state;
        ESP_LOGE(TAG, "adapter stayed on (rc=%d): %s", stateRc, state.c_str());
        return false;
    }
    last_error_.clear();
    ESP_LOGI(TAG, "adapter powered off");
    return true;
}

bool BluetoothManager::IsPowered() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!Available()) return false;
    std::string out;
    const int rc = RunShellCommand("timeout --signal=TERM 5s bluetoothctl show", out);
    if (rc != 0) {
        last_error_ = out;
        ESP_LOGE(TAG, "power state failed (rc=%d): %s", rc, out.c_str());
        return false;
    }
    const bool powered = fieldBool(out, "Powered");
    last_error_.clear();
    return powered;
}

std::vector<BtDevice> BluetoothManager::Scan(int duration_s) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    duration_s = std::max(1, std::min(duration_s, 15));
    const auto started = std::chrono::steady_clock::now();
    std::vector<BtDevice> devs;
    ESP_LOGI(TAG, "scan started (%d seconds)", duration_s);
    if (!Available()) {
        ESP_LOGE(TAG, "scan stopped: %s", last_error_.c_str());
        return devs;
    }
    if (!PowerOn()) {
        ESP_LOGE(TAG, "cannot power adapter: %s", last_error_.c_str());
        return devs;
    }

    // BlueZ ties discovery to the D-Bus client that requested it: the moment
    // that client disconnects, bluetoothd stops discovering. A one-shot
    // `bluetoothctl scan on` exits right after printing "Discovery started",
    // killing the scan before any device can answer. Keep a single bluetoothctl
    // alive for the whole window by feeding its stdin through a shell sleep.
    std::string scanCmd =
        "{ printf 'scan on\\n'; sleep " + std::to_string(duration_s) +
        "; printf 'scan off\\n'; } | timeout --signal=TERM " +
        std::to_string(duration_s + 10) + "s bluetoothctl";
    std::string junk;
    RunShellCommand(scanCmd, junk);

    std::string devicesOut;
    int listRc = RunShellCommand("timeout --signal=TERM 5s bluetoothctl devices", devicesOut);
    if (listRc != 0) {
        last_error_ = devicesOut;
        ESP_LOGE(TAG, "device list failed (rc=%d): %s", listRc, devicesOut.c_str());
        return devs;
    }

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
        RunShellCommand("timeout --signal=TERM 4s bluetoothctl info " +
                        QuoteShellArgument(d.address), info);
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
    for (const auto &dev : devs) {
        ESP_LOGI(TAG, "device name='%s' address=%s rssi=%d paired=%d connected=%d",
                 dev.name.c_str(), dev.address.c_str(), dev.rssi,
                 dev.paired ? 1 : 0, dev.connected ? 1 : 0);
    }
    last_error_.clear();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    ESP_LOGI(TAG, "scan completed: %zu devices in %lld ms",
             devs.size(), static_cast<long long>(elapsed));
    return devs;
}

bool BluetoothManager::PairAndConnect(const std::string &address) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!Available()) return false;
    if (!PowerOn()) return false;
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
    RunShellCommand("timeout --signal=TERM 5s bluetoothctl info " +
                    QuoteShellArgument(address), info);
    if (fieldBool(info, "Connected")) {
        last_error_.clear();
        ESP_LOGI(TAG, "connected to %s", address.c_str());
        return true;
    }
    last_error_ = "pair/connect failed: " + out;
    ESP_LOGE(TAG, "pair/connect %s failed: %s", address.c_str(), out.c_str());
    return false;
}

bool BluetoothManager::Disconnect(const std::string &address) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::string out;
    RunShellCommand("timeout --signal=TERM 8s bluetoothctl disconnect " +
                    QuoteShellArgument(address), out);
    std::string info;
    RunShellCommand("timeout --signal=TERM 5s bluetoothctl info " +
                    QuoteShellArgument(address), info);
    if (!fieldBool(info, "Connected")) {
        last_error_.clear();
        ESP_LOGI(TAG, "disconnected from %s", address.c_str());
        return true;
    }
    last_error_ = out;
    ESP_LOGE(TAG, "disconnect %s failed: %s", address.c_str(), out.c_str());
    return false;
}

bool BluetoothManager::Remove(const std::string &address) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::string out;
    int rc = RunShellCommand("timeout --signal=TERM 8s bluetoothctl remove " +
                             QuoteShellArgument(address), out);
    if (rc != 0) {
        last_error_ = out;
        ESP_LOGE(TAG, "remove %s failed (rc=%d): %s", address.c_str(), rc, out.c_str());
        return false;
    }
    last_error_.clear();
    ESP_LOGI(TAG, "removed %s", address.c_str());
    return true;
}

} // namespace jetson
