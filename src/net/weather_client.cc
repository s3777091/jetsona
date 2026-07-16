#include "weather_client.h"
#include "esp_log.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <mutex>

#define TAG "WeatherClient"

namespace jetson {

namespace {
std::once_flag g_curl_init_once;

void EnsureCurlInit() {
    std::call_once(g_curl_init_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

size_t CurlWriteCb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *out = static_cast<std::string *>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string EnvOr(const char *name, const char *fallback) {
    const char *v = std::getenv(name);
    return (v && v[0]) ? std::string(v) : std::string(fallback);
}
} // namespace

const char *WeatherClient::DescribeWmoCode(int code) {
    // WMO weather interpretation codes, grouped.
    if (code == 0) return "Trời quang";
    if (code <= 2) return "Ít mây";
    if (code == 3) return "Nhiều mây";
    if (code == 45 || code == 48) return "Sương mù";
    if (code >= 51 && code <= 57) return "Mưa phùn";
    if (code >= 61 && code <= 67) return "Mưa";
    if (code >= 71 && code <= 77) return "Tuyết";
    if (code >= 80 && code <= 82) return "Mưa rào";
    if (code == 85 || code == 86) return "Mưa tuyết";
    if (code == 95) return "Dông";
    if (code >= 96) return "Dông kèm mưa đá";
    return "—";
}

bool WeatherClient::Fetch(WeatherInfo &out, std::string &err) {
    EnsureCurlInit();
    const std::string lat = EnvOr("JETSON_WEATHER_LAT", "10.7769");
    const std::string lon = EnvOr("JETSON_WEATHER_LON", "106.7009");

    const std::string url =
        "https://api.open-meteo.com/v1/forecast?latitude=" + lat +
        "&longitude=" + lon +
        "&current=temperature_2m,relative_humidity_2m,weather_code"
        "&daily=temperature_2m_max,temperature_2m_min"
        "&timezone=auto&forecast_days=1";

    CURL *curl = curl_easy_init();
    if (!curl) { err = "curl init"; return false; }
    std::string resp;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) { err = curl_easy_strerror(rc); return false; }
    if (http_code < 200 || http_code >= 300) {
        err = "HTTP " + std::to_string(http_code);
        return false;
    }

    try {
        auto j = nlohmann::json::parse(resp);
        const auto &cur = j.at("current");
        out.temp_c       = cur.value("temperature_2m", 0.0);
        out.humidity     = (int)cur.value("relative_humidity_2m", 0.0);
        out.weather_code = cur.value("weather_code", 0);
        const auto &daily = j.at("daily");
        out.temp_max_c = daily.at("temperature_2m_max").at(0).get<double>();
        out.temp_min_c = daily.at("temperature_2m_min").at(0).get<double>();
        out.description = DescribeWmoCode(out.weather_code);
    } catch (const std::exception &ex) {
        err = std::string("parse: ") + ex.what();
        return false;
    }
    return true;
}

std::string WeatherClient::FormatLine(const WeatherInfo &info) {
    const std::string name = EnvOr("JETSON_WEATHER_NAME", "TP.HCM");
    char buf[160];
    snprintf(buf, sizeof(buf), "%s%s%s · %.0f°C (%.0f°–%.0f°) · Ẩm %d%%",
             name.c_str(), name.empty() ? "" : ": ",
             info.description.c_str(),
             info.temp_c, info.temp_min_c, info.temp_max_c, info.humidity);
    return buf;
}

} // namespace jetson
