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
    std::string location;    // resolved city name ("Đà Nẵng")
};

/* Where the forecast is taken from. Resolved once per process. */
struct GeoLocation {
    double lat = 10.7769;
    double lon = 106.7009;
    std::string name = "TP.HCM";
};

/* Tiny open-meteo client for the lock screen's weather line.
 *
 * Blocking (libcurl): call on a worker thread, never the LVGL thread.
 *
 * Location: resolved from the device's public IP (ipapi.co, then ip-api.com)
 * so the reading follows wherever the device actually is. Set all three of
 * JETSON_WEATHER_LAT / JETSON_WEATHER_LON / JETSON_WEATHER_NAME to pin it to a
 * fixed place instead; leaving them empty (the config.yaml default) keeps
 * auto-detection on. TP.HCM is only the offline fallback. */
class WeatherClient {
public:
    static bool Fetch(WeatherInfo &out, std::string &err);
    // Resolve (and cache) the device location. Blocking, like Fetch.
    static GeoLocation ResolveLocation();
    // "Đà Nẵng: Mưa rào · 31°C (26°–33°) · Ẩm 78%"
    static std::string FormatLine(const WeatherInfo &info);
    static const char *DescribeWmoCode(int code);
};

} // namespace jetson
