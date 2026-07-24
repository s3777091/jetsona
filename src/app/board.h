#ifndef BOARD_H
#define BOARD_H

#include "audio_codec.h"

#include <functional>
#include <string>

/* Headless Jetson Nano "Ekko Lite" board.
 *
 * No display, no backlight, no LEDs, no buttons, no Wi-Fi/Bluetooth. The device
 * is a voice-first AI appliance: a USB reSpeaker (mic + speaker) for the audio
 * loop, Ethernet for the network, an INA219 UPS for battery, and the on-board
 * PWM fan for thermal balance. Everything that existed only to drive the LVGL
 * panel has been removed. */
class Board {
public:
    static Board &GetInstance() {
        static Board instance;
        return instance;
    }

    AudioCodec *GetAudioCodec() { return &audio_codec_; }

    bool GetBatteryLevel(int &level, bool &charging, bool &discharging);
    std::string GetUuid();
    std::string GetBoardType() { return "jetson-nano"; }

private:
    Board();
    ~Board();
    Board(const Board &) = delete;
    Board &operator=(const Board &) = delete;

    LinuxAudioCodec audio_codec_;
};

#endif