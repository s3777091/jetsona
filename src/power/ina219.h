#ifndef JETSON_POWER_INA219_H
#define JETSON_POWER_INA219_H

/* Battery monitor for the Waveshare UPS Power Module on the Jetson Nano
 * (https://www.waveshare.com/wiki/UPS_Power_Module).
 *
 * The module exposes an INA219 current/voltage monitor over I2C (6-pin cable →
 * /dev/i2c-1 @ 0x42, per Waveshare's UPS-Power-Module demo code). We read it
 * directly with a raw /dev/i2c-N ioctl (I2C_SLAVE + register read/write) — no
 * extra library, matching the POSIX fd+ioctl style already used in
 * src/app/system_info.cc.
 *
 * State of charge is derived from the INA219 bus-voltage register (the battery
 * pack voltage; 2S2P 18650 pack). Waveshare's demo maps it linearly:
 * percent = (V − 6.0) / 2.4 × 100, i.e. 8.4 V full / 6.0 V empty.
 * Charging/discharging come from the calibrated current register's sign. Which
 * sign means "charging" depends on how the shunt is wired on the individual
 * module, so it is a knob rather than an assumption: with the default +1 a
 * positive current means the pack is being charged. If the bolt shows while
 * running on battery (or stays dark on the charger), flip INA219_CHARGE_SIGN
 * to -1. Every successful first read logs the measured mA under the "ina219"
 * tag, which is the quickest way to tell which way round this module is.
 *
 * Everything is env-configurable so a wrong I2C bus/address or a different cell
 * chemistry is a runtime fix, not a rebuild:
 *   INA219_BUS         "/dev/i2c-1"  I2C character device path
 *   INA219_ADDR        "0x42"        7-bit I2C address (hex) of the INA219
 *   INA219_VMIN        "6.0"         pack voltage mapped to 0 %
 *   INA219_VMAX        "8.4"         pack voltage mapped to 100 %
 *   INA219_SHUNT       "0.1"         shunt resistor value (ohm) for current cal
 *   INA219_CHARGE_SIGN "1"           +1 or -1: current sign that means charging
 *   INA219_CHARGE_MA   "50"          |mA| below this counts as idle, not charging
 *
 * On any I2C failure Read() returns false so the caller can keep the UI sane.
 */

#include <cstdint>
#include <string>

class Ina219 {
public:
    Ina219();
    ~Ina219();

    Ina219(const Ina219 &) = delete;
    Ina219 &operator=(const Ina219 &) = delete;

    // Open the bus, set the slave address, write config + calibration. Idempotent:
    // once it has failed it stops retrying (call Reset() to retry).
    bool Init();

    // Read one sample. Returns false on any I2C error (caller should fall back).
    bool Read(int &level_pct, bool &charging, bool &discharging);

    bool Ready() const { return fd_ >= 0; }
    void Reset();  // close fd so the next Init() retries from scratch

private:
    bool ReadReg16(uint8_t reg, uint16_t &out);
    bool WriteReg16(uint8_t reg, uint16_t val);

    int         fd_ = -1;
    std::string bus_path_;
    uint8_t     addr_ = 0x42;
    float       vmin_ = 6.0f;
    float       vmax_ = 8.4f;
    float       shunt_ = 0.1f;
    float       charge_sign_ = 1.0f;  // current sign that means "charging"
    float       charge_ma_ = 50.0f;   // idle band around zero, in mA
    // current_lsb in amperes per ADC step, set during Init() from shunt_.
    float       current_lsb_ = 0.0f;
    bool        tried_ = false;   // Init() has been attempted at least once
    bool        logged_current_ = false;  // first current sample has been logged
};

#endif // JETSON_POWER_INA219_H