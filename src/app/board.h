#ifndef BOARD_H
#define BOARD_H

#include "display/core/display.h"
#include "audio_codec.h"
#include "backlight.h"
#include "led.h"
#include "button.h"

#include <functional>
#include <string>

class Board {
public:
    static Board &GetInstance() {
        static Board instance;
        return instance;
    }

    Display *GetDisplay() { return display_; }
    AudioCodec *GetAudioCodec() { return &audio_codec_; }
    Backlight *GetBacklight() { return &backlight_; }
    Led *GetLed() { return &led_; }

    void StartNetwork();              // phase 1: no-op (network handled by OS)
    const char *GetNetworkStateIcon();
    bool GetBatteryLevel(int &level, bool &charging, bool &discharging);
    void SetPowerSaveLevel(int /*level*/) {}
    std::string GetUuid();
    void SetNetworkEventCallback(std::function<void(bool)> /*cb*/) {}
    std::string GetBoardType() { return "jetson-nano"; }

private:
    Board();
    ~Board();
    Board(const Board &) = delete;
    Board &operator=(const Board &) = delete;

    Display *display_ = nullptr;
    DummyAudioCodec audio_codec_;
    PwmBacklight backlight_;
    NoLed led_;
    Button boot_button_;
};

#endif
