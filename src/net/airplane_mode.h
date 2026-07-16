#pragma once

#include "net/bluetooth_manager.h"
#include "net/wifi_manager.h"

#include <string>

namespace jetson {

/* Result of an airplane-mode transition. `enabled` is the persisted mode
 * after the operation; the radio fields are freshly read from the real
 * NetworkManager/BlueZ backends. */
struct AirplaneModeResult {
    bool success = false;
    bool enabled = false;
    bool wifi_enabled = false;
    bool bluetooth_powered = false;
    std::string error;
};

// Persisted global mode flag. Radio managers use this to reject attempts to
// power a radio back on while airplane mode is active.
bool IsAirplaneModeEnabled();

/* Change the real radio state and persist the mode. Enabling is transactional:
 * if either available radio remains on, radios already changed are restored
 * and airplane mode stays off. Disabling always releases the mode and attempts
 * to restore only radios that were on before it was enabled. */
AirplaneModeResult SetAirplaneMode(bool enabled, IWifiManager &wifi,
                                   IBluetoothManager &bluetooth);

// Re-apply a persisted enabled state after reboot without overwriting the
// saved pre-airplane radio state.
AirplaneModeResult EnforcePersistedAirplaneMode(IWifiManager &wifi,
                                                IBluetoothManager &bluetooth);

} // namespace jetson
