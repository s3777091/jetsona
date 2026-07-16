#include "net/airplane_mode.h"

#include "esp_log.h"
#include "settings.h"

#include <mutex>
#include <string>
#include <utility>

#define TAG "AirplaneMode"

namespace jetson {
namespace {

constexpr const char *kNamespace = "system";
constexpr const char *kEnabledKey = "airplane_mode";
constexpr const char *kWifiWasOnKey = "airplane_wifi_was_on";
constexpr const char *kBluetoothWasOnKey = "airplane_bluetooth_was_on";

std::mutex &TransitionMutex() {
    static std::mutex mutex;
    return mutex;
}

struct RadioState {
    bool wifi_available = false;
    bool wifi_enabled = false;
    bool bluetooth_available = false;
    bool bluetooth_powered = false;
};

RadioState ReadRadios(IWifiManager &wifi, IBluetoothManager &bluetooth) {
    RadioState state;
    state.wifi_available = wifi.Available();
    if (state.wifi_available) state.wifi_enabled = wifi.IsEnabled();
    state.bluetooth_available = bluetooth.Available();
    if (state.bluetooth_available) state.bluetooth_powered = bluetooth.IsPowered();
    return state;
}

void AppendError(std::string &error, const char *radio, const std::string &detail) {
    if (!error.empty()) error += "; ";
    error += radio;
    if (!detail.empty()) error += ": " + detail;
}

AirplaneModeResult MakeResult(bool success, bool enabled, const RadioState &state,
                              std::string error = {}) {
    AirplaneModeResult result;
    result.success = success;
    result.enabled = enabled;
    result.wifi_enabled = state.wifi_enabled;
    result.bluetooth_powered = state.bluetooth_powered;
    result.error = std::move(error);
    return result;
}

AirplaneModeResult EnableLocked(IWifiManager &wifi, IBluetoothManager &bluetooth) {
    const bool was_enabled = IsAirplaneModeEnabled();
    const RadioState before = ReadRadios(wifi, bluetooth);

    bool wifi_command_ok = true;
    bool bluetooth_command_ok = true;
    std::string wifi_error;
    std::string bluetooth_error;
    if (before.wifi_enabled) {
        wifi_command_ok = wifi.Enable(false);
        if (!wifi_command_ok) wifi_error = wifi.LastError();
    }
    if (before.bluetooth_powered) {
        bluetooth_command_ok = bluetooth.PowerOff();
        if (!bluetooth_command_ok) bluetooth_error = bluetooth.LastError();
    }

    RadioState after = ReadRadios(wifi, bluetooth);
    std::string error;
    const bool wifi_failed = !wifi_command_ok || after.wifi_enabled;
    const bool bluetooth_failed = !bluetooth_command_ok || after.bluetooth_powered;
    if (wifi_failed)
        AppendError(error, "WiFi", wifi_error.empty() ? wifi.LastError() : wifi_error);
    if (bluetooth_failed)
        AppendError(error, "Bluetooth",
                    bluetooth_error.empty() ? bluetooth.LastError() : bluetooth_error);

    if (wifi_failed || bluetooth_failed) {
        // A partially enabled airplane mode is misleading. Restore the state
        // changed by this attempt and leave the persisted flag untouched.
        if (!was_enabled) {
            if (before.wifi_enabled && !after.wifi_enabled) (void)wifi.Enable(true);
            if (before.bluetooth_powered && !after.bluetooth_powered)
                (void)bluetooth.PowerOn();
            after = ReadRadios(wifi, bluetooth);
        }
        ESP_LOGE(TAG, "enable failed: %s", error.c_str());
        return MakeResult(false, was_enabled, after, std::move(error));
    }

    Settings settings(kNamespace, true);
    if (!was_enabled) {
        settings.SetBool(kWifiWasOnKey, before.wifi_enabled);
        settings.SetBool(kBluetoothWasOnKey, before.bluetooth_powered);
    }
    settings.SetBool(kEnabledKey, true);
    ESP_LOGI(TAG, "enabled (WiFi and Bluetooth are off)");
    return MakeResult(true, true, after);
}

AirplaneModeResult DisableLocked(IWifiManager &wifi, IBluetoothManager &bluetooth) {
    if (!IsAirplaneModeEnabled()) {
        return MakeResult(true, false, ReadRadios(wifi, bluetooth));
    }

    Settings saved(kNamespace, false);
    const bool restore_wifi = saved.GetBool(kWifiWasOnKey, false);
    const bool restore_bluetooth = saved.GetBool(kBluetoothWasOnKey, false);

    // Release the global guard before calling PowerOn()/Enable(true).
    Settings settings(kNamespace, true);
    settings.SetBool(kEnabledKey, false);

    bool wifi_ok = true;
    bool bluetooth_ok = true;
    if (restore_wifi) wifi_ok = wifi.Enable(true);
    if (restore_bluetooth) bluetooth_ok = bluetooth.PowerOn();

    const RadioState after = ReadRadios(wifi, bluetooth);
    std::string error;
    if (restore_wifi && (!wifi_ok || !after.wifi_enabled))
        AppendError(error, "WiFi", wifi.LastError());
    if (restore_bluetooth && (!bluetooth_ok || !after.bluetooth_powered))
        AppendError(error, "Bluetooth", bluetooth.LastError());

    settings.EraseKey(kWifiWasOnKey);
    settings.EraseKey(kBluetoothWasOnKey);
    const bool success = error.empty();
    if (success) ESP_LOGI(TAG, "disabled; previous radio state restored");
    else ESP_LOGW(TAG, "disabled with restore warning: %s", error.c_str());
    return MakeResult(success, false, after, std::move(error));
}

} // namespace

bool IsAirplaneModeEnabled() {
    return Settings(kNamespace, false).GetBool(kEnabledKey, false);
}

AirplaneModeResult SetAirplaneMode(bool enabled, IWifiManager &wifi,
                                   IBluetoothManager &bluetooth) {
    std::lock_guard<std::mutex> lock(TransitionMutex());
    return enabled ? EnableLocked(wifi, bluetooth)
                   : DisableLocked(wifi, bluetooth);
}

AirplaneModeResult EnforcePersistedAirplaneMode(IWifiManager &wifi,
                                                IBluetoothManager &bluetooth) {
    std::lock_guard<std::mutex> lock(TransitionMutex());
    if (!IsAirplaneModeEnabled()) {
        AirplaneModeResult result;
        result.success = true;
        return result;
    }
    auto result = EnableLocked(wifi, bluetooth);
    if (result.success) return result;

    const std::string enforce_error = result.error;
    auto disabled = DisableLocked(wifi, bluetooth);
    const std::string restore_error = disabled.error;
    disabled.success = false;
    disabled.error = enforce_error;
    if (!restore_error.empty()) AppendError(disabled.error, "restore", restore_error);
    return disabled;
}

} // namespace jetson
