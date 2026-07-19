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

// GET `url` into `out`. Short timeouts: geolocation must never stall the
// weather loop, it just falls through to the next provider.
bool HttpGet(const std::string &url, std::string &out, long timeout_s = 10) {
    EnsureCurlInit();
    CURL *curl = curl_easy_init();
    if (!curl) return false;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "jetson-ds02/1.0");
    const CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK && http_code >= 200 && http_code < 300;
}

std::mutex g_geo_mutex;
GeoLocation g_geo;
bool g_geo_resolved = false;
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

GeoLocation WeatherClient::ResolveLocation() {
    {
        std::lock_guard<std::mutex> guard(g_geo_mutex);
        if (g_geo_resolved) return g_geo;
    }

    GeoLocation geo;              // TP.HCM fallback from the struct defaults
    bool resolved = false;

    /* An explicit config pin wins: all three vars must be set, otherwise the
     * device is free to follow its own IP (someone who moved from Saigon to Da
     * Nang should not keep reading Saigon weather). */
    const std::string env_lat = EnvOr("JETSON_WEATHER_LAT", "");
    const std::string env_lon = EnvOr("JETSON_WEATHER_LON", "");
    const std::string env_name = EnvOr("JETSON_WEATHER_NAME", "");
    if (!env_lat.empty() && !env_lon.empty()) {
        try {
            geo.lat = std::stod(env_lat);
            geo.lon = std::stod(env_lon);
            geo.name = env_name.empty() ? std::string("") : env_name;
            resolved = true;
        } catch (const std::exception &) {
            ESP_LOGW(TAG, "bad JETSON_WEATHER_LAT/LON, falling back to IP lookup");
        }
    }

    /* Two free keyless providers; ipapi.co speaks HTTPS, ip-api.com is the
     * plain-HTTP backup for when the first is rate-limited (1k/day). */
    if (!resolved) {
        struct Provider { const char *url; const char *lat; const char *lon; const char *city; };
        static const Provider kProviders[] = {
            {"https://ipapi.co/json/", "latitude", "longitude", "city"},
            {"http://ip-api.com/json/?fields=status,city,lat,lon", "lat", "lon", "city"},
        };
        for (const auto &provider : kProviders) {
            std::string resp;
            if (!HttpGet(provider.url, resp)) continue;
            try {
                auto j = nlohmann::json::parse(resp);
                if (!j.contains(provider.lat) || !j.contains(provider.lon)) continue;
                geo.lat = j.at(provider.lat).get<double>();
                geo.lon = j.at(provider.lon).get<double>();
                geo.name = j.value(provider.city, std::string(""));
                resolved = true;
                ESP_LOGI(TAG, "location: %s (%.4f, %.4f)", geo.name.c_str(), geo.lat, geo.lon);
                break;
            } catch (const std::exception &ex) {
                ESP_LOGW(TAG, "geo parse (%s): %s", provider.url, ex.what());
            }
        }
    }

    std::lock_guard<std::mutex> guard(g_geo_mutex);
    /* Only a real answer is cached: an offline boot must retry on the next
     * weather tick instead of pinning the fallback city for the whole session. */
    if (resolved) {
        g_geo = geo;
        g_geo_resolved = true;
    }
    return geo;
}

bool WeatherClient::Fetch(WeatherInfo &out, std::string &err) {
    EnsureCurlInit();
    const GeoLocation geo = ResolveLocation();
    char coord[64];
    snprintf(coord, sizeof(coord), "%.4f", geo.lat);
    const std::string lat = coord;
    snprintf(coord, sizeof(coord), "%.4f", geo.lon);
    const std::string lon = coord;

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
        out.location = geo.name;
    } catch (const std::exception &ex) {
        err = std::string("parse: ") + ex.what();
        return false;
    }
    return true;
}

std::string WeatherClient::FormatLine(const WeatherInfo &info) {
    const std::string name = info.location;
    char buf[160];
    snprintf(buf, sizeof(buf), "%s%s%s · %.0f°C (%.0f°–%.0f°) · Ẩm %d%%",
             name.c_str(), name.empty() ? "" : ": ",
             info.description.c_str(),
             info.temp_c, info.temp_min_c, info.temp_max_c, info.humidity);
    return buf;
}

} // namespace jetson
