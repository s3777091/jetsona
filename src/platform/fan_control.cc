#include "platform/fan_control.h"

#include "esp_log.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#define TAG "FanControl"

namespace jetson::fan {
namespace {

constexpr const char *kFanDir = "/sys/devices/pwm-fan";
constexpr const char *kConfPath = "/etc/jetson-fan.conf";

std::string ReadFileTrimmed(const std::string &path) {
    std::ifstream in(path);
    if (!in) return {};
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    return text;
}

int ReadInt(const std::string &path, int fallback) {
    const std::string text = ReadFileTrimmed(path);
    if (text.empty()) return fallback;
    return std::atoi(text.c_str());
}

bool WriteSysfs(const std::string &path, int value) {
    std::ofstream out(path);
    if (!out) return false;
    out << value;
    return out.good();
}

// The daemon config is a handful of KEY=value lines. Every field is round
// tripped so changing one setting from the UI does not drop the others.
struct Conf {
    bool found = false;
    Mode mode = Mode::Auto;
    Profile profile = Profile::Quiet;
    int manual_pwm = 128;
};

Conf ReadConf() {
    Conf conf;
    std::ifstream in(kConfPath);
    if (!in) return conf;
    conf.found = true;
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) value.pop_back();
        if (key == "MODE") {
            if (value == "manual") conf.mode = Mode::Manual;
            else if (value == "off") conf.mode = Mode::Off;
            else conf.mode = Mode::Auto;
        } else if (key == "PROFILE") {
            if (value == "cool") conf.profile = Profile::Cool;
            else if (value == "balanced") conf.profile = Profile::Balanced;
            else conf.profile = Profile::Quiet;
        } else if (key == "MANUAL_PWM") {
            conf.manual_pwm = std::atoi(value.c_str());
        }
    }
    return conf;
}

const char *ModeKey(Mode mode) {
    switch (mode) {
        case Mode::Manual: return "manual";
        case Mode::Off:    return "off";
        case Mode::Auto:   break;
    }
    return "auto";
}

const char *ProfileKey(Profile profile) {
    switch (profile) {
        case Profile::Cool:     return "cool";
        case Profile::Balanced: return "balanced";
        case Profile::Quiet:    break;
    }
    return "quiet";
}

bool WriteConf(const Conf &conf) {
    // Truncating in place (rather than write-temp-and-rename) is deliberate:
    // the config is mode 0666 but /etc is not writable by the firmware user,
    // so a rename would fail on any build that is not running as root.
    std::ofstream out(kConfPath, std::ios::trunc);
    if (!out) return false;
    out << "# Jetson Nano fan control -- read every 2s by jetson-fan-curve.sh.\n"
        << "# Written by the DS-02 firmware (Settings > Cai dat chung > Quat).\n"
        << "MODE=" << ModeKey(conf.mode) << "\n"
        << "PROFILE=" << ProfileKey(conf.profile) << "\n"
        << "MANUAL_PWM=" << conf.manual_pwm << "\n";
    return out.good();
}

// Fallback for boards without the daemon: drive the sysfs knobs ourselves.
void ApplyDirect(Mode mode, int manual_pwm) {
    if (mode == Mode::Auto) {
        WriteSysfs(std::string(kFanDir) + "/temp_control", 1);
        return;
    }
    WriteSysfs(std::string(kFanDir) + "/temp_control", 0);
    WriteSysfs(std::string(kFanDir) + "/target_pwm", mode == Mode::Off ? 0 : manual_pwm);
}

void Apply(Mode mode, Profile profile, int manual_pwm) {
    Conf conf = ReadConf();
    conf.mode = mode;
    conf.profile = profile;
    conf.manual_pwm = std::clamp(manual_pwm, 0, 255);
    if (conf.found && WriteConf(conf)) return;
    ESP_LOGW(TAG, "fan daemon config unavailable, driving sysfs directly");
    ApplyDirect(mode, conf.manual_pwm);
}

} // namespace

Status Read() {
    Status status;
    const std::string dir = kFanDir;
    const std::string target = ReadFileTrimmed(dir + "/target_pwm");
    if (target.empty()) return status;  // no pwm-fan on this board

    status.available = true;
    status.target_pwm = std::atoi(target.c_str());
    status.rpm = ReadInt(dir + "/rpm_measured", 0);

    // thermal_zone1 is CPU-therm and thermal_zone2 GPU-therm on t210; the fan
    // curve follows whichever is hotter.
    for (int zone : {1, 2}) {
        char path[64];
        std::snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", zone);
        status.temp_c = std::max(status.temp_c, ReadInt(path, 0) / 1000);
    }

    const Conf conf = ReadConf();
    status.daemon = conf.found;
    if (conf.found) {
        status.mode = conf.mode;
        status.profile = conf.profile;
        status.manual_pwm = conf.manual_pwm;
    } else {
        // Without the daemon, temp_control tells us who is driving the fan.
        status.mode = ReadInt(dir + "/temp_control", 1) ? Mode::Auto : Mode::Manual;
        status.manual_pwm = status.target_pwm;
    }
    return status;
}

void SetMode(Mode mode) {
    const Conf conf = ReadConf();
    Apply(mode, conf.profile, conf.manual_pwm);
}

void SetManualPwm(int pwm) { Apply(Mode::Manual, ReadConf().profile, pwm); }

void SetProfile(Profile profile) {
    const Conf conf = ReadConf();
    Apply(conf.mode, profile, conf.manual_pwm);
}

int PwmToPercent(int pwm) {
    return std::clamp((pwm * 100 + 127) / 255, 0, 100);
}

int PercentToPwm(int percent) {
    return std::clamp(percent * 255 / 100, 0, 255);
}

const char *ModeLabel(Mode mode) {
    switch (mode) {
        case Mode::Manual: return "Thủ công";
        case Mode::Off:    return "Tắt";
        case Mode::Auto:   break;
    }
    return "Tự động";
}

const char *ProfileLabel(Profile profile) {
    switch (profile) {
        case Profile::Cool:     return "Mát";
        case Profile::Balanced: return "Cân bằng";
        case Profile::Quiet:    break;
    }
    return "Im lặng";
}

} // namespace jetson::fan
