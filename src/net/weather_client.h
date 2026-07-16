#pragma once

#include <string>

namespace jetson {

/* Current conditions + today's range, from the free open-meteo.com API
 * (no API key). */
struct WeatherInfo {
    double temp_c = 0;
    double temp_min_c = 0;
    double temp_max_c = 0;
    int humidity = 0;      // %
    int weather_code = 0;  // WMO code
    std::string description; // Vietnamese text for the WMO code
};

/* Tiny open-meteo client for the standby screen's weather line.
 *
 * Blocking (libcurl): call on a worker thread, never the LVGL thread.
 *
 * Location config (env, all optional):
 *   JETSON_WEATHER_LAT / JETSON_WEATHER_LON  — default 10.7769 / 106.7009 (TP.HCM)
 *   JETSON_WEATHER_NAME — display name prefixed to the line (default "TP.HCM") */
class WeatherClient {
public:
    static bool Fetch(WeatherInfo &out, std::string &err);
    // "TP.HCM: Mưa rào · 31°C (26°–33°) · Ẩm 78%"
    static std::string FormatLine(const WeatherInfo &info);
    static const char *DescribeWmoCode(int code);
};

} // namespace jetson
