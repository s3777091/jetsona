#include "ina219.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "esp_log.h"

#define TAG "ina219"

namespace {
// "env overrides default" helper, mirroring src/agent/llm_client.cc's EnvOr.
std::string EnvOr(const char *name, const std::string &fallback) {
    const char *v = std::getenv(name);
    if (v && v[0]) return std::string(v);
    return fallback;
}
float EnvFloatOr(const char *name, float fallback) {
    const char *v = std::getenv(name);
    if (v && v[0]) {
        char *end = nullptr;
        float f = std::strtof(v, &end);
        if (end != v) return f;
    }
    return fallback;
}
// Parse "0x43", "43" (hex) or a decimal address string into a 7-bit address.
uint8_t ParseAddr(const std::string &s) {
    return (uint8_t)std::strtoul(s.c_str(), nullptr, 0);
}

// INA219 register addresses.
constexpr uint8_t kRegConfig = 0x00;
constexpr uint8_t kRegShunt  = 0x01;
constexpr uint8_t kRegBus    = 0x02;
constexpr uint8_t kRegPower  = 0x03;
constexpr uint8_t kRegCurrent = 0x04;
constexpr uint8_t kRegCal    = 0x05;

// Config: BRNG=0 (16V), PG=01 (±40mV), BADC=12-bit, SADC=12-bit / 532µs,
// MODE=0x07 (continuous shunt+bus). This is the datasheet reset default 0x399F,
// written explicitly so a stale register from a prior driver run is overwritten.
constexpr uint16_t kConfig = 0x399F;
} // namespace

Ina219::Ina219() {
    bus_path_ = EnvOr("INA219_BUS", "/dev/i2c-1");
    addr_     = ParseAddr(EnvOr("INA219_ADDR", "0x43"));
    vmin_     = EnvFloatOr("INA219_VMIN", 6.6f);
    vmax_     = EnvFloatOr("INA219_VMAX", 8.4f);
    shunt_    = EnvFloatOr("INA219_SHUNT", 0.1f);
}

Ina219::~Ina219() {
    if (fd_ >= 0) ::close(fd_);
}

void Ina219::Reset() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    tried_ = false;
}

bool Ina219::ReadReg16(uint8_t reg, uint16_t &out) {
    // INA219 is big-endian: write the 1-byte register pointer, then read 2 bytes.
    if (::write(fd_, &reg, 1) != 1) return false;
    uint8_t buf[2];
    if (::read(fd_, buf, 2) != 2) return false;
    out = (uint16_t)((buf[0] << 8) | buf[1]);
    return true;
}

bool Ina219::WriteReg16(uint8_t reg, uint16_t val) {
    uint8_t buf[3] = {reg, (uint8_t)(val >> 8), (uint8_t)(val & 0xFF)};
    return ::write(fd_, buf, 3) == 3;
}

bool Ina219::Init() {
    if (fd_ >= 0) return true;
    if (tried_) return false;   // don't retry every tick once it has failed
    tried_ = true;

    fd_ = ::open(bus_path_.c_str(), O_RDWR);
    if (fd_ < 0) {
        ESP_LOGW(TAG, "open %s failed: %s (set INA219_BUS to override)", bus_path_.c_str(),
                 std::strerror(errno));
        return false;
    }
    if (ioctl(fd_, I2C_SLAVE, addr_) < 0) {
        ESP_LOGW(TAG, "I2C_SLAVE 0x%02x on %s failed: %s (set INA219_ADDR to override)",
                 addr_, bus_path_.c_str(), std::strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    if (!WriteReg16(kRegConfig, kConfig)) {
        ESP_LOGW(TAG, "write config on 0x%02x failed: %s — wrong address / no INA219",
                 addr_, std::strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // Calibration for current readout. cal = trunc(0.04096 / (shunt * I_lsb)).
    // Pick I_lsb so ~5 A stays in the 15-bit signed range: I_lsb = 5.0 / 32768.
    if (shunt_ > 0.0f) {
        current_lsb_ = 5.0f / 32768.0f;                 // ~152.6 µA / step
        uint16_t cal = (uint16_t)(0.04096f / (shunt_ * current_lsb_));
        WriteReg16(kRegCal, cal);                       // best-effort; current is optional
    }

    ESP_LOGI(TAG, "INA219 ready on %s @0x%02x (Vmin=%.2f Vmax=%.2f shunt=%.3f)",
             bus_path_.c_str(), addr_, vmin_, vmax_, shunt_);
    return true;
}

bool Ina219::Read(int &level_pct, bool &charging, bool &discharging) {
    if (!Init()) return false;

    uint16_t bus_raw = 0;
    if (!ReadReg16(kRegBus, bus_raw)) {
        ESP_LOGW(TAG, "read bus voltage failed: %s", std::strerror(errno));
        ::close(fd_);
        fd_ = -1;
        tried_ = false;   // transient: allow one retry on the next tick
        return false;
    }
    // Bus voltage: bits [15:3] in units of 4 mV; bit 1 = CNVR, bit 0 = OVF.
    float vbus_v = ((bus_raw >> 3) * 4) / 1000.0f;

    float range = vmax_ - vmin_;
    int level = range > 0.0f ? (int)((vbus_v - vmin_) / range * 100.0f + 0.5f) : 100;
    level = std::max(0, std::min(100, level));
    level_pct = level;

    // Current is signed 16-bit × current_lsb (amperes). Only meaningful when the
    // calibration register was written. Sign convention on this module: positive
    // current = battery being charged, negative = discharging into the Jetson.
    charging = false;
    discharging = false;
    uint16_t cur_raw = 0;
    if (current_lsb_ > 0.0f && ReadReg16(kRegCurrent, cur_raw)) {
        int16_t signed_cur = (int16_t)cur_raw;
        float amps = signed_cur * current_lsb_;
        float ma = amps * 1000.0f;
        if (ma > 50.0f) charging = true;
        else if (ma < -50.0f) discharging = true;
    }

    return true;
}