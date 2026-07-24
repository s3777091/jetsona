#include "board.h"
#include "settings.h"
#include "ina219.h"
#include "esp_log.h"

#include <cstdlib>
#include <cstring>

#define TAG "Board"

Board::Board()
    : audio_codec_(16000, 16000) {
    // Apply the persisted mixer state before anything else can touch the codec.
    Settings display_settings("display");
    audio_codec_.SetOutputState(display_settings.GetInt("volume", 50),
                                display_settings.GetBool("muted", false));
    ESP_LOGI(TAG, "Ekko Lite board ready (headless, audio-only)");
}

Board::~Board() = default;

bool Board::GetBatteryLevel(int &level, bool &charging, bool &discharging) {
    /* Reads the Waveshare UPS Power Module battery via the INA219 on I2C.
     * Lazy singleton so the I2C fd lives as long as the Board. If the read
     * fails (no /dev/i2c, wrong address, UPS disconnected) fall back to a full
     * battery so the low-battery alert path stays quiet instead of spamming. */
    static Ina219 ina;
    if (ina.Read(level, charging, discharging)) return true;

    level = 100;
    charging = true;
    discharging = false;
    return false;
}

std::string Board::GetUuid() {
    Settings s("board", false);
    std::string uuid = s.GetString("uuid", "");
    if (uuid.empty()) {
        Settings w("board", true);
        uuid = "jetson-";
        const char *mac = std::getenv("JETSON_DEVICE_MAC");
        uuid += mac ? mac : "000000";
        w.SetString("uuid", uuid);
    }
    return uuid;
}