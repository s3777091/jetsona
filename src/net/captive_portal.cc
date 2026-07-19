#include "net/captive_portal.h"

#include "esp_log.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <mutex>

#define TAG "CaptivePortal"

namespace jetson {
namespace {

constexpr const char *kDefaultProbeUrl =
    "http://connectivitycheck.gstatic.com/generate_204";

bool IsHttpUrl(const std::string &url) {
    // This value crosses the firmware-to-launcher file boundary. Reject
    // whitespace/control characters (especially newlines) and unreasonable
    // lengths before it can become a Chromium start URL.
    if (url.size() < 8 || url.size() > 2048) return false;
    for (unsigned char c : url) {
        if (c <= 0x20 || c == 0x7f) return false;
    }
    std::string prefix = url.substr(0, std::min<size_t>(8, url.size()));
    std::transform(prefix.begin(), prefix.end(), prefix.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const size_t scheme_length = prefix.rfind("https://", 0) == 0 ? 8 :
                                 prefix.rfind("http://", 0) == 0 ? 7 : 0;
    if (scheme_length == 0 || url.size() <= scheme_length) return false;
    const char first_host_char = url[scheme_length];
    return first_host_char != '/' && first_host_char != '?' &&
           first_host_char != '#';
}

size_t StopAfterBodyBegins(char *, size_t size, size_t count, void *userdata) {
    const size_t bytes = size * count;
    if (bytes > 0 && userdata) *static_cast<bool *>(userdata) = true;
    // The known-good response is bodyless (204), while any HTML body already
    // proves interception. Stop at its first chunk instead of downloading an
    // advertisement/login page again on every background probe.
    return 0;
}

void EnsureCurlInitialized() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

} // namespace

std::string CaptivePortalProbeUrl() {
    const char *configured = std::getenv("JETSON_CAPTIVE_PORTAL_PROBE_URL");
    if (configured && IsHttpUrl(configured)) return configured;
    return kDefaultProbeUrl;
}

CaptivePortalResult ProbeCaptivePortal() {
    CaptivePortalResult result;
    result.login_url = CaptivePortalProbeUrl();

    EnsureCurlInitialized();
    CURL *curl = curl_easy_init();
    if (!curl) {
        result.state = InternetAccessState::Offline;
        result.error = "curl_easy_init failed";
        return result;
    }

    bool response_body_seen = false;
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Cache-Control: no-cache, no-store");
    headers = curl_slist_append(headers, "Pragma: no-cache");
    headers = curl_slist_append(headers, "Connection: close");

    curl_easy_setopt(curl, CURLOPT_URL, result.login_url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, StopAfterBodyBegins);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body_seen);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 2500L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 3L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     "Mozilla/5.0 (Linux; Jetson) JetsonFW-PortalCheck/1.0");

    const CURLcode code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.http_status);
    char *redirect = nullptr;
    curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &redirect);
    const bool received_response = code == CURLE_OK ||
        (code == CURLE_WRITE_ERROR && response_body_seen);
    if (received_response) {

        if (result.http_status == 204) {
            result.state = InternetAccessState::Online;
            result.login_url.clear();
        } else if ((result.http_status >= 200 && result.http_status < 400) ||
                   result.http_status == 511) {
            // The endpoint has no normal body and only returns 204. Any 2xx
            // page, redirect, or RFC 6585 Network Authentication Required is
            // therefore portal content rather than unrestricted Internet.
            result.state = InternetAccessState::CaptivePortal;
            if (redirect && IsHttpUrl(redirect)) result.login_url = redirect;
        } else {
            result.state = InternetAccessState::Offline;
        }
    } else {
        result.state = InternetAccessState::Offline;
        result.error = curl_easy_strerror(code);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    // Redirect URLs often contain a short-lived token or the client's MAC;
    // report availability without leaking the URL into persistent logs.
    ESP_LOGI(TAG, "probe state=%d http=%ld login_url=%s%s%s",
             static_cast<int>(result.state), result.http_status,
             result.login_url.empty() ? "none" : "available",
             result.error.empty() ? "" : " error=",
             result.error.empty() ? "" : result.error.c_str());
    return result;
}

} // namespace jetson
