#pragma once

#include <lvgl.h>

namespace jetson::ui {

// Converts a Bluetooth RSSI value (-100..0 dBm) to the percentage used by the
// shared WiFi/Bluetooth strength indicator.
int RssiToSignalPercent(int rssi_dbm);

// Creates the standard four-bar signal indicator and returns its root object.
// signal_percent is clamped to 0..100.
lv_obj_t *CreateSignalBars(lv_obj_t *parent, int signal_percent);

} // namespace jetson::ui
