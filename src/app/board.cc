#include "board.h"
#include "ds02_home_display.h"
#include "lvgl_runtime.h"
#include "fonts.h"
#include "settings.h"
#include "font_awesome.h"
#include "esp_log.h"

#include <cstdlib>
#include <cstring>

#define TAG "Board"

Board::Board()
    : audio_codec_(16000, 16000),
      backlight_(0, false),
      led_(),
      boot_button_(0) {
    int w = JETSON_DISPLAY_WIDTH;
    int h = JETSON_DISPLAY_HEIGHT;

    if (!jetson::LvglRuntime::Instance().Init(w, h)) {
        ESP_LOGE(TAG, "LVGL runtime init failed");
        return;
    }
    jetson::InitBuiltinFonts(JETSON_ASSETS_DIR);
    jetson::LvglRuntime::Instance().StartHandler();

    display_ = new home::Ds02HomeDisplay(nullptr, nullptr, w, h, 0, 0, false, false, false);
    ESP_LOGI(TAG, "DS-02 home display created %dx%d", w, h);
}

Board::~Board() {
    delete display_;
}

void Board::StartNetwork() {
    /* On Jetson, networking is managed by the OS (NetworkManager/wpa_supplicant
     * or Ethernet). Nothing to do here in phase 1. */
}

const char *Board::GetNetworkStateIcon() {
    return FONT_AWESOME_WIFI;
}

bool Board::GetBatteryLevel(int &level, bool &charging, bool &discharging) {
    /* The Waveshare UPS module exposes battery via I2C; not wired in phase 1.
     * Report a full battery so the UI icon is sensible. */
    level = 100;
    charging = true;
    discharging = false;
    return true;
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