#include "net/zing_music_client.h"

#include "esp_log.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <functional>
#include <initializer_list>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

#define TAG "ZingMusic"

namespace jetson {
namespace {

using json = nlohmann::json;
using music::Album;
using music::CatalogItem;
using music::CatalogKind;
using music::DiscoverData;
using music::Track;

constexpr char kDefaultVersion[] = "1.19.3";
constexpr char kDefaultApiKey[] = "X5BM3w8N7MKozC0B85o4KMlzLZKhV00y";
constexpr char kDefaultSecretKey[] = "acOrvUS15XRW2o9JksiK1KgQ6Vbds8ZW";
constexpr char kDefaultBaseUrl[] = "https://zingmp3.vn";

constexpr char kHomeEndpoint[] = "/api/v2/page/get/home";
constexpr char kPlaylistEndpoint[] = "/api/v2/page/get/playlist";
constexpr char kStreamingEndpoint[] = "/api/v2/song/get/streaming";
constexpr char kRadioEndpoint[] = "/api/v2/page/get/radio";
constexpr char kTop100Endpoint[] = "/api/v2/page/get/top-100";
constexpr char kRealtimeChartEndpoint[] = "/api/v2/page/get/rt-chart";
constexpr char kChartHomeEndpoint[] = "/api/v2/page/get/chart-home";

constexpr size_t kMaxJsonBytes = 8 * 1024 * 1024;
constexpr size_t kMaxArtworkBytes = 12 * 1024 * 1024;
constexpr size_t kMaxDiscoverItems = 8;

struct ClientConfig {
    std::string version;
    std::string api_key;
    std::string secret_key;
    std::string base_url;
    std::string cookies;
    std::string cache_dir;
};

struct HttpResult {
    bool ok = false;
    long status = 0;
    std::string body;
    std::string error;
};

std::once_flag g_curl_init_once;

void EnsureCurlInit() {
    std::call_once(g_curl_init_once,
                   []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

std::string GetEnv(const char *name) {
    const char *value = std::getenv(name);
    return (value && value[0]) ? std::string(value) : std::string();
}

std::string EnvOr(const char *name, const char *fallback) {
    std::string value = GetEnv(name);
    return value.empty() ? std::string(fallback) : value;
}

std::string TrimTrailingSlashes(std::string value) {
    while (value.size() > 8 && !value.empty() && value.back() == '/')
        value.pop_back();
    return value;
}

std::string DefaultCacheDir() {
    std::string configured = GetEnv("ZING_ARTWORK_CACHE_DIR");
    if (!configured.empty()) return configured;

    std::string xdg = GetEnv("XDG_CACHE_HOME");
    if (!xdg.empty()) return xdg + "/jetson-fw/music";

    std::string home = GetEnv("HOME");
    if (!home.empty()) return home + "/.cache/jetson-fw/music";
    return "/tmp/jetson-fw-music";
}

std::string Hex(const unsigned char *bytes, size_t count) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(count * 2, '0');
    for (size_t i = 0; i < count; ++i) {
        out[i * 2] = kDigits[(bytes[i] >> 4) & 0x0f];
        out[i * 2 + 1] = kDigits[bytes[i] & 0x0f];
    }
    return out;
}

bool Sha256Hex(const std::string &input, std::string &out) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (EVP_Digest(input.data(), input.size(), digest, &length, EVP_sha256(),
                   nullptr) != 1)
        return false;
    out = Hex(digest, length);
    return true;
}

bool HmacSha512Hex(const std::string &message, const std::string &key,
                   std::string &out) {
    if (key.size() > static_cast<size_t>(INT_MAX)) return false;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (!HMAC(EVP_sha512(), key.data(), static_cast<int>(key.size()),
              reinterpret_cast<const unsigned char *>(message.data()),
              message.size(), digest, &length))
        return false;
    out = Hex(digest, length);
    return true;
}

std::string UrlEncode(const std::string &value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() + 8);
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0f]);
        }
    }
    return out;
}

bool IsSignatureParameter(const std::string &key) {
    // This is the whitelist used by the current Zing landing-page bundle.
    // Parameters such as `q` still belong in the request URL but must not be
    // included in the SHA-256 input.
    return key == "ctime" || key == "id" || key == "type" ||
           key == "page" || key == "count" || key == "version";
}

bool BuildSignedUrl(const ClientConfig &config, const std::string &endpoint,
                    const std::map<std::string, std::string> &request_params,
                    std::string &out, std::string &err) {
    if (config.api_key.empty() || config.secret_key.empty()) {
        err = "Zing API key/secret are empty";
        return false;
    }

    const std::time_t now = std::time(nullptr);
    if (now < 1704067200) { // 2024-01-01 UTC: system/NTP is not ready.
        err = "system clock is not synchronized; cannot sign a Zing request";
        return false;
    }

    std::map<std::string, std::string> params = request_params;
    params["ctime"] = std::to_string(static_cast<long long>(now));
    params["version"] = config.version;

    // The landing-page signer sorts only its explicit whitelist, concatenates
    // key=value without separators, SHA-256s that string, then signs
    // endpoint+hash with HMAC-SHA512.
    std::string canonical;
    for (const auto &entry : params) {
        if (IsSignatureParameter(entry.first))
            canonical += entry.first + "=" + entry.second;
    }

    std::string hash;
    std::string signature;
    if (!Sha256Hex(canonical, hash) ||
        !HmacSha512Hex(endpoint + hash, config.secret_key, signature)) {
        err = "OpenSSL could not generate the Zing signature";
        return false;
    }

    params["apiKey"] = config.api_key;
    params["sig"] = signature;

    std::ostringstream url;
    url << config.base_url << endpoint << '?';
    bool first = true;
    for (const auto &entry : params) {
        if (!first) url << '&';
        first = false;
        url << UrlEncode(entry.first) << '=' << UrlEncode(entry.second);
    }
    out = url.str();
    return true;
}

struct StringSink {
    std::string data;
    size_t limit = 0;
    bool overflow = false;
};

size_t WriteString(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *sink = static_cast<StringSink *>(userdata);
    const size_t bytes = size * nmemb;
    if (bytes > sink->limit || sink->data.size() > sink->limit - bytes) {
        sink->overflow = true;
        return 0;
    }
    sink->data.append(ptr, bytes);
    return bytes;
}

void AddCommonHeaders(const ClientConfig &config,
                      struct curl_slist **headers) {
    *headers = curl_slist_append(*headers,
        "Accept: application/json, text/plain, */*");
    *headers = curl_slist_append(*headers,
        "Accept-Language: vi,en-US;q=0.9,en;q=0.8");
    const std::string origin = "Origin: " + config.base_url;
    *headers = curl_slist_append(*headers, origin.c_str());
}

HttpResult HttpGetJson(const ClientConfig &config, const std::string &url,
                       const std::string &cookies) {
    EnsureCurlInit();
    HttpResult result;
    CURL *curl = curl_easy_init();
    if (!curl) {
        result.error = "curl_easy_init failed";
        return result;
    }

    StringSink sink;
    sink.limit = kMaxJsonBytes;
    struct curl_slist *headers = nullptr;
    AddCommonHeaders(config, &headers);
    const std::string referer = config.base_url + "/";

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
        "Mozilla/5.0 (X11; Linux aarch64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
    curl_easy_setopt(curl, CURLOPT_REFERER, referer.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    // Signed API responses are expected to be JSON. Do not follow transport
    // redirects while a Cookie header is attached; Zing's JSON fallback URL is
    // validated separately below before it is requested.
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (!cookies.empty()) curl_easy_setopt(curl, CURLOPT_COOKIE, cookies.c_str());

    const CURLcode code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    result.body = std::move(sink.data);
    if (code != CURLE_OK) {
        result.error = sink.overflow
            ? "response exceeded 8 MiB"
            : curl_easy_strerror(code);
        return result;
    }
    if (result.status < 200 || result.status >= 300) {
        result.error = "HTTP " + std::to_string(result.status);
        return result;
    }
    result.ok = true;
    return result;
}

int JsonInt(const json &value, int fallback = 0) {
    if (value.is_number_unsigned()) {
        const auto parsed = value.get<unsigned long long>();
        return parsed <= static_cast<unsigned long long>(INT_MAX)
                   ? static_cast<int>(parsed) : fallback;
    }
    if (value.is_number_integer()) {
        const auto parsed = value.get<long long>();
        return parsed >= INT_MIN && parsed <= INT_MAX
                   ? static_cast<int>(parsed) : fallback;
    }
    if (value.is_number_float()) {
        const double parsed = value.get<double>();
        return std::isfinite(parsed) && parsed >= INT_MIN && parsed <= INT_MAX
                   ? static_cast<int>(parsed) : fallback;
    }
    if (value.is_string()) {
        errno = 0;
        char *end = nullptr;
        const long long parsed = std::strtoll(
            value.get_ref<const std::string &>().c_str(), &end, 10);
        if (errno == 0 && end && *end == '\0' &&
            parsed >= INT_MIN && parsed <= INT_MAX)
            return static_cast<int>(parsed);
    }
    return fallback;
}

std::string JsonString(const json &object,
                       std::initializer_list<const char *> keys) {
    if (!object.is_object()) return {};
    for (const char *key : keys) {
        auto it = object.find(key);
        if (it == object.end() || it->is_null()) continue;
        if (it->is_string()) {
            std::string parsed = it->get<std::string>();
            if (!parsed.empty()) return parsed;
            continue;
        }
        if (it->is_number_unsigned()) return std::to_string(it->get<unsigned long long>());
        if (it->is_number_integer()) return std::to_string(it->get<long long>());
    }
    return {};
}

std::string ResolveUrl(const ClientConfig &config, const std::string &url) {
    if (url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0)
        return url;
    if (url.rfind("//", 0) == 0) return "https:" + url;
    if (!url.empty() && url.front() == '/') return config.base_url + url;
    return config.base_url + "/" + url;
}

bool ExtractHttpsHost(const std::string &url, std::string &host) {
    host.clear();
    size_t start = 0;
    if (url.rfind("https://", 0) == 0) start = 8;
    else if (url.rfind("//", 0) == 0) start = 2;
    else return false;
    const size_t end = url.find_first_of("/?#", start);
    std::string authority = url.substr(start, end == std::string::npos
                                                 ? std::string::npos
                                                 : end - start);
    if (authority.empty() || authority.find('@') != std::string::npos) return false;
    const size_t port = authority.find(':');
    if (port != std::string::npos) authority.resize(port);
    if (authority.empty()) return false;
    for (char &c : authority) {
        const unsigned char value = static_cast<unsigned char>(c);
        if (value < 128) c = static_cast<char>(std::tolower(value));
    }
    host = std::move(authority);
    return true;
}

bool HostMatches(const std::string &host, const std::string &suffix) {
    if (host == suffix) return true;
    return host.size() > suffix.size() &&
           host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0 &&
           host[host.size() - suffix.size() - 1] == '.';
}

bool IsSafeApiFallback(const ClientConfig &config, const std::string &url) {
    if (!url.empty() && url.front() == '/' && url.rfind("//", 0) != 0)
        return true;
    std::string host;
    std::string base_host;
    if (!ExtractHttpsHost(url, host) ||
        !ExtractHttpsHost(config.base_url, base_host)) return false;
    return host == base_host || HostMatches(host, "zingmp3.vn");
}

bool IsSafeZingAssetUrl(const ClientConfig &config, const std::string &url) {
    std::string host;
    std::string base_host;
    if (!ExtractHttpsHost(ResolveUrl(config, url), host)) return false;
    ExtractHttpsHost(config.base_url, base_host);
    return (!base_host.empty() && host == base_host) ||
           HostMatches(host, "zingmp3.vn") ||
           HostMatches(host, "zmdcdn.me") ||
           HostMatches(host, "zadn.vn");
}

std::string ExplainZingError(int code) {
    if (code == -201)
        return "authentication rejected (-201; compatibility key/cookie may have rotated)";
    if (code == -104)
        return "incorrect signature (-104; API version/key/secret may have rotated)";
    if (code == -1150 || code == -1151)
        return "content is unavailable or requires Zing VIP";
    return "Zing error " + std::to_string(code);
}

bool ParseEnvelope(const json &root, json &data, int &code, std::string &message,
                   std::string &fallback_url) {
    code = 0;
    message.clear();
    fallback_url.clear();
    if (!root.is_object()) {
        message = "JSON root is not an object";
        return false;
    }

    auto err_it = root.find("err");
    const bool has_err = err_it != root.end();
    if (has_err) code = JsonInt(*err_it, -1);
    message = JsonString(root, {"msg", "message", "error"});
    fallback_url = JsonString(root, {"url"});

    if (has_err && code != 0) return false;
    auto data_it = root.find("data");
    if (data_it != root.end() && !data_it->is_null()) data = *data_it;
    else data = root;
    return true;
}

std::string JoinDiagnostics(const std::vector<std::string> &parts,
                            const char *separator = "; ") {
    std::ostringstream out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out << separator;
        out << parts[i];
    }
    return out.str();
}

bool RequestApi(const ClientConfig &config, const std::string &endpoint,
                const std::map<std::string, std::string> &params, json &out,
                std::string &err) {
    std::vector<std::string> diagnostics;

    // With configured cookies, retry anonymously to distinguish stale-cookie
    // failures from signature failures.  Without cookies, the second anonymous
    // attempt still covers transient network/upstream errors with a fresh URL.
    for (int attempt = 0; attempt < 2; ++attempt) {
        const bool use_cookie = attempt == 0 && !config.cookies.empty();
        const std::string variant = use_cookie ? "cookie" : "anonymous";
        std::string signed_url;
        std::string sign_error;
        if (!BuildSignedUrl(config, endpoint, params, signed_url, sign_error)) {
            err = sign_error;
            return false;
        }

        HttpResult response = HttpGetJson(
            config, signed_url, use_cookie ? config.cookies : std::string());
        if (!response.ok) {
            diagnostics.push_back(variant + " attempt " + std::to_string(attempt + 1) +
                                  ": " + response.error);
            continue;
        }

        json root = json::parse(response.body, nullptr, false);
        if (root.is_discarded()) {
            diagnostics.push_back(variant + " attempt " + std::to_string(attempt + 1) +
                                  ": invalid JSON");
            continue;
        }

        json data;
        int zing_code = 0;
        std::string message;
        std::string fallback_url;
        if (ParseEnvelope(root, data, zing_code, message, fallback_url)) {
            out = std::move(data);
            err.clear();
            return true;
        }

        std::string detail = ExplainZingError(zing_code);
        if (!message.empty()) detail += " (" + message + ")";

        // Strawberry follows the URL supplied with -201.  Preserve that
        // behavior, but validate the second envelope instead of assuming the
        // redirect fixed authentication.
        if (!fallback_url.empty() && IsSafeApiFallback(config, fallback_url)) {
            HttpResult fallback = HttpGetJson(
                config, ResolveUrl(config, fallback_url),
                use_cookie ? config.cookies : std::string());
            if (fallback.ok) {
                json fallback_root = json::parse(fallback.body, nullptr, false);
                json fallback_data;
                int fallback_code = 0;
                std::string fallback_message;
                std::string ignored_url;
                if (!fallback_root.is_discarded() &&
                    ParseEnvelope(fallback_root, fallback_data, fallback_code,
                                  fallback_message, ignored_url)) {
                    out = std::move(fallback_data);
                    err.clear();
                    return true;
                }
                if (!fallback_root.is_discarded()) {
                    detail += ", fallback: " + ExplainZingError(fallback_code);
                    if (!fallback_message.empty())
                        detail += " (" + fallback_message + ")";
                } else {
                    detail += ", fallback: invalid JSON";
                }
            } else {
                detail += ", fallback: " + fallback.error;
            }
        } else if (!fallback_url.empty()) {
            detail += ", fallback URL rejected by host allowlist";
        }

        diagnostics.push_back(variant + " attempt " + std::to_string(attempt + 1) +
                              ": " + detail);
        // A second request without a new cookie is the same signed operation
        // and cannot repair a deterministic -201/-104 response. Network/HTTP
        // failures above still get the second attempt.
        if (config.cookies.empty()) break;
    }

    err = "Zing " + endpoint + " failed: " +
          JoinDiagnostics(diagnostics) +
          ". Override ZING_API_VERSION/ZING_API_KEY/ZING_SECRET_KEY and "
          "provide a current ZING_COOKIES value when Zing requires a session.";
    return false;
}

std::string LowerAscii(std::string value) {
    for (char &c : value) {
        unsigned char u = static_cast<unsigned char>(c);
        if (u < 128) c = static_cast<char>(std::tolower(u));
    }
    return value;
}

bool ContainsAny(const std::string &haystack,
                 std::initializer_list<const char *> needles) {
    const std::string lower = LowerAscii(haystack);
    for (const char *needle : needles) {
        if (lower.find(LowerAscii(needle)) != std::string::npos) return true;
    }
    return false;
}

bool LooksLikeSong(const json &item) {
    const std::string type = JsonString(item, {"objectType", "type", "sectionType"});
    if (ContainsAny(type, {"song"})) return true;
    return item.is_object() && item.contains("duration") &&
           (!JsonString(item, {"encodeId", "id"}).empty());
}

bool LooksLikeArtist(const json &item) {
    const std::string type = JsonString(item, {"objectType", "type"});
    if (ContainsAny(type, {"artist"})) return true;
    return item.is_object() && item.contains("alias") && item.contains("name") &&
           !item.contains("duration") && !item.contains("song");
}

bool LooksLikeRadio(const json &item) {
    const std::string type = JsonString(item, {"objectType", "type", "sectionType"});
    return ContainsAny(type, {"radio", "livestream", "live-stream"}) ||
           (item.is_object() && (item.contains("host") || item.contains("program")) &&
            item.contains("livestream"));
}

bool IsCandidate(const json &item) {
    if (!item.is_object()) return false;
    const std::string id = JsonString(item, {"encodeId", "id", "alias"});
    const std::string title = JsonString(item, {"title", "name"});
    return !id.empty() && !title.empty();
}

void CollectCandidates(const json &node, std::vector<const json *> &out,
                       int depth = 0) {
    if (depth > 8) return;
    if (node.is_array()) {
        for (const auto &child : node) {
            if (IsCandidate(child)) out.push_back(&child);
            else CollectCandidates(child, out, depth + 1);
        }
        return;
    }
    if (!node.is_object()) return;
    if (IsCandidate(node)) {
        out.push_back(&node);
        return; // Do not accidentally collect songs/artists nested in a card.
    }
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (it.value().is_array() || it.value().is_object())
            CollectCandidates(it.value(), out, depth + 1);
    }
}

std::string StripHtml(const std::string &input) {
    std::string out;
    out.reserve(input.size());
    bool in_tag = false;
    for (char c : input) {
        if (c == '<') { in_tag = true; continue; }
        if (c == '>') { in_tag = false; continue; }
        if (!in_tag) out.push_back(c);
    }
    return out;
}

bool IsPremium(const json &item) {
    if (!item.is_object()) return false;
    auto premium = item.find("isPremium");
    if (premium != item.end() && premium->is_boolean()) return premium->get<bool>();
    auto status = item.find("streamingStatus");
    return status != item.end() && JsonInt(*status) == 2;
}

CatalogItem ParseCatalogItem(const json &item, CatalogKind hint) {
    CatalogItem result;
    const bool radio = LooksLikeRadio(item);
    const bool artist = !radio && LooksLikeArtist(item);
    if (radio && item.is_object()) {
        auto livestream = item.find("livestream");
        if (livestream != item.end() && livestream->is_object())
            result.id = JsonString(*livestream, {"encodeId", "id"});
    }
    if (result.id.empty()) {
        result.id = artist
            ? JsonString(item, {"playlistId", "encodeId", "id", "alias"})
            : JsonString(item, {"encodeId", "id", "alias"});
    }
    result.title = JsonString(item, {"title", "name"});
    result.subtitle = JsonString(item,
        {"artistsNames", "sortDescription", "description", "subtitle"});
    if (result.subtitle.empty() && radio && item.is_object()) {
        auto host = item.find("host");
        if (host != item.end() && host->is_object())
            result.subtitle = JsonString(*host, {"name", "title"});
    }
    result.subtitle = StripHtml(result.subtitle);
    result.thumbnail_url = JsonString(item,
        {"thumbnailM", "thumbnail", "thumbnailR", "thumbnailHasText"});
    if (item.is_object()) {
        auto duration = item.find("duration");
        if (duration != item.end()) result.duration_seconds = JsonInt(*duration);
    }
    result.premium = IsPremium(item);
    result.kind = radio ? CatalogKind::Radio
                : artist ? CatalogKind::Artist
                : LooksLikeSong(item) ? CatalogKind::Song
                : hint;
    return result;
}

void AddUnique(std::vector<CatalogItem> &items, CatalogItem item) {
    if (item.id.empty() || item.title.empty()) return;
    auto duplicate = std::find_if(items.begin(), items.end(),
        [&](const CatalogItem &existing) { return existing.id == item.id; });
    if (duplicate == items.end() && items.size() < kMaxDiscoverItems)
        items.push_back(std::move(item));
}

void CollectArtistsFromSong(const json &song, std::vector<CatalogItem> &artists) {
    if (!song.is_object()) return;
    auto it = song.find("artists");
    if (it == song.end() || !it->is_array()) return;
    for (const auto &artist : *it)
        AddUnique(artists, ParseCatalogItem(artist, CatalogKind::Artist));
}

CatalogKind ClassifySection(const json &section) {
    const std::string label = JsonString(section, {"title"}) + " " +
                              JsonString(section, {"sectionType"}) + " " +
                              JsonString(section, {"type"});
    if (ContainsAny(label, {"top 100", "top100"})) return CatalogKind::Top100;
    if (ContainsAny(label, {"radio", "livestream"})) return CatalogKind::Radio;
    if (ContainsAny(label, {"artist", "nghệ sĩ", "nghe si"})) return CatalogKind::Artist;
    if (ContainsAny(label, {"trending", "thịnh hành", "thinh hanh", "new-release",
                            "mới", "moi", "song"}))
        return CatalogKind::Song;
    return CatalogKind::Playlist;
}

void ParseHomeData(const json &data, DiscoverData &out) {
    const json *sections = &data;
    if (data.is_object()) {
        auto items = data.find("items");
        if (items != data.end()) sections = &*items;
    }

    if (sections->is_array()) {
        for (const auto &section : *sections) {
            CatalogKind hint = ClassifySection(section);
            const json *content = &section;
            if (section.is_object()) {
                auto items = section.find("items");
                if (items != section.end()) content = &*items;
            }
            std::vector<const json *> candidates;
            CollectCandidates(*content, candidates);
            for (const json *candidate : candidates) {
                if (!candidate) continue;
                if (LooksLikeArtist(*candidate) || hint == CatalogKind::Artist) {
                    AddUnique(out.artists, ParseCatalogItem(*candidate, CatalogKind::Artist));
                } else if (LooksLikeRadio(*candidate) || hint == CatalogKind::Radio) {
                    AddUnique(out.radio, ParseCatalogItem(*candidate, CatalogKind::Radio));
                } else if (hint == CatalogKind::Top100) {
                    AddUnique(out.top100, ParseCatalogItem(*candidate, CatalogKind::Top100));
                } else if (LooksLikeSong(*candidate) || hint == CatalogKind::Song) {
                    AddUnique(out.trending, ParseCatalogItem(*candidate, CatalogKind::Song));
                    CollectArtistsFromSong(*candidate, out.artists);
                }
            }
        }
    }

    // Tolerate response shapes without section wrappers.
    if (out.trending.empty()) {
        std::vector<const json *> candidates;
        CollectCandidates(data, candidates);
        for (const json *candidate : candidates) {
            if (LooksLikeSong(*candidate)) {
                AddUnique(out.trending, ParseCatalogItem(*candidate, CatalogKind::Song));
                CollectArtistsFromSong(*candidate, out.artists);
            }
        }
    }
}

void ParseDedicatedSection(const json &data, CatalogKind kind,
                           std::vector<CatalogItem> &out) {
    std::vector<const json *> candidates;
    CollectCandidates(data, candidates);
    for (const json *candidate : candidates) {
        CatalogItem item = ParseCatalogItem(*candidate, kind);
        item.kind = kind;
        AddUnique(out, std::move(item));
    }
}

void ParseSongData(const json &data, DiscoverData &out) {
    std::vector<const json *> candidates;
    CollectCandidates(data, candidates);
    for (const json *candidate : candidates) {
        if (!candidate || !LooksLikeSong(*candidate)) continue;
        CatalogItem item = ParseCatalogItem(*candidate, CatalogKind::Song);
        item.kind = CatalogKind::Song;
        AddUnique(out.trending, std::move(item));
        CollectArtistsFromSong(*candidate, out.artists);
    }
}

void ParseRadioNode(const json &node, CatalogKind context, DiscoverData &out,
                    int depth = 0) {
    if (depth > 10) return;
    if (node.is_array()) {
        for (const auto &child : node)
            ParseRadioNode(child, context, out, depth + 1);
        return;
    }
    if (!node.is_object()) return;

    const CatalogKind section_kind = ClassifySection(node);
    if (section_kind == CatalogKind::Artist) context = CatalogKind::Artist;
    else if (section_kind == CatalogKind::Radio) context = CatalogKind::Radio;

    if (IsCandidate(node)) {
        const bool artist = context == CatalogKind::Artist || LooksLikeArtist(node);
        CatalogItem item = ParseCatalogItem(
            node, artist ? CatalogKind::Artist : CatalogKind::Radio);
        item.kind = artist ? CatalogKind::Artist : CatalogKind::Radio;
        AddUnique(artist ? out.artists : out.radio, std::move(item));
        return;
    }

    for (auto it = node.begin(); it != node.end(); ++it) {
        CatalogKind child_context = context;
        if (ContainsAny(it.key(), {"artistspotlight", "artist_spotlight",
                                   "artist spotlight"})) {
            child_context = CatalogKind::Artist;
        } else if (ContainsAny(it.key(), {"radio", "livestream", "live-stream"})) {
            child_context = CatalogKind::Radio;
        }
        ParseRadioNode(it.value(), child_context, out, depth + 1);
    }
}

void ParseRadioData(const json &data, DiscoverData &out) {
    // The current /radio response mixes playable stations with an
    // `artistSpotlight` collection. Keep those groups separate so circular
    // artist cards never leak into the Radio row.
    ParseRadioNode(data, CatalogKind::Radio, out);
}

CatalogItem DemoItem(const char *id, CatalogKind kind, const char *title,
                     const char *subtitle) {
    CatalogItem item;
    item.id = id;
    item.kind = kind;
    item.title = title;
    item.subtitle = subtitle;
    return item;
}

void FillDemoMetadata(DiscoverData &out, std::vector<std::string> &warnings) {
    constexpr char kDemoNote[] = "Dữ liệu mẫu — Zing tạm thời không khả dụng";
    if (out.trending.empty()) {
        warnings.push_back("Trending is using demo metadata");
        out.trending = {
            DemoItem("demo:song:daily", CatalogKind::Song,
                     "Giai điệu nổi bật", kDemoNote),
            DemoItem("demo:song:new", CatalogKind::Song,
                     "Khám phá hôm nay", kDemoNote),
            DemoItem("demo:song:vpop", CatalogKind::Song,
                     "V-Pop tuyển chọn", kDemoNote),
        };
    }
    if (out.artists.empty()) {
        warnings.push_back("Popular artists is using demo metadata");
        out.artists = {
            DemoItem("demo:artist:sontung", CatalogKind::Artist,
                     "Sơn Tùng M-TP", kDemoNote),
            DemoItem("demo:artist:soobin", CatalogKind::Artist,
                     "SOOBIN", kDemoNote),
            DemoItem("demo:artist:hieuthuhai", CatalogKind::Artist,
                     "HIEUTHUHAI", kDemoNote),
        };
    }
    if (out.radio.empty()) {
        warnings.push_back("Radio is using demo metadata");
        out.radio = {
            DemoItem("demo:radio:vpop", CatalogKind::Radio,
                     "V-Pop Radio", kDemoNote),
            DemoItem("demo:radio:acoustic", CatalogKind::Radio,
                     "Acoustic Radio", kDemoNote),
            DemoItem("demo:radio:chill", CatalogKind::Radio,
                     "Chill Radio", kDemoNote),
        };
    }
    if (out.top100.empty()) {
        warnings.push_back("Top 100 is using demo metadata");
        out.top100 = {
            DemoItem("demo:top100:vietnam", CatalogKind::Top100,
                     "Top 100 Nhạc Việt", kDemoNote),
            DemoItem("demo:top100:pop", CatalogKind::Top100,
                     "Top 100 Pop", kDemoNote),
            DemoItem("demo:top100:asia", CatalogKind::Top100,
                     "Top 100 Châu Á", kDemoNote),
        };
    }
}

bool HasImageMagic(const std::string &path) {
    unsigned char magic[8] = {};
    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    const size_t count = std::fread(magic, 1, sizeof(magic), file);
    std::fclose(file);
    const bool png = count >= 4 && magic[0] == 0x89 && magic[1] == 0x50 &&
                     magic[2] == 0x4e && magic[3] == 0x47;
    const bool jpeg = count >= 2 && magic[0] == 0xff && magic[1] == 0xd8;
    // LV_USE_GIF is disabled in this firmware; accepting GIF here would cache
    // an image that LvglImageFromFile can identify but LVGL cannot decode.
    return png || jpeg;
}

bool IsRegularNonEmptyFile(const std::string &path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 32;
}

bool EnsureDirectory(const std::string &path, std::string &err) {
    if (path.empty()) { err = "artwork cache directory is empty"; return false; }
    std::string current;
    size_t pos = 0;
    if (path.front() == '/') { current = "/"; pos = 1; }
    while (pos <= path.size()) {
        const size_t slash = path.find('/', pos);
        const std::string part = path.substr(pos, slash == std::string::npos
                                                  ? std::string::npos
                                                  : slash - pos);
        if (!part.empty()) {
            if (!current.empty() && current.back() != '/') current += '/';
            current += part;
            if (::mkdir(current.c_str(), 0775) != 0 && errno != EEXIST) {
                err = "mkdir " + current + ": " + std::strerror(errno);
                return false;
            }
        }
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    return true;
}

struct FileSink {
    FILE *file = nullptr;
    size_t bytes = 0;
    bool overflow = false;
};

size_t WriteFile(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *sink = static_cast<FileSink *>(userdata);
    const size_t bytes = size * nmemb;
    if (bytes > kMaxArtworkBytes || sink->bytes > kMaxArtworkBytes - bytes) {
        sink->overflow = true;
        return 0;
    }
    const size_t written = std::fwrite(ptr, 1, bytes, sink->file);
    sink->bytes += written;
    return written;
}

struct ArtworkTarget {
    std::string url;
    std::vector<std::string *> paths;
    std::string cached_path;
};

void CacheArtworkTargets(ZingMusicClient &client,
                         std::vector<ArtworkTarget> &targets) {
    if (targets.empty()) return;
    std::atomic<size_t> next{0};
    const size_t worker_count = std::min<size_t>(8, targets.size());
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (size_t worker = 0; worker < worker_count; ++worker) {
        workers.emplace_back([&]() {
            while (true) {
                const size_t index = next.fetch_add(1);
                if (index >= targets.size()) return;
                std::string ignored_error;
                try {
                    client.DownloadArtwork(targets[index].url,
                                           targets[index].cached_path,
                                           ignored_error);
                } catch (const std::exception &exception) {
                    ESP_LOGW(TAG, "artwork worker failed: %s", exception.what());
                } catch (...) {
                    ESP_LOGW(TAG, "artwork worker failed with an unknown exception");
                }
            }
        });
    }
    for (auto &worker : workers) worker.join();
    for (auto &target : targets) {
        if (target.cached_path.empty()) continue;
        for (std::string *path : target.paths)
            if (path) *path = target.cached_path;
    }
}

void AddArtworkTarget(std::vector<ArtworkTarget> &targets,
                      std::unordered_map<std::string, size_t> &indices,
                      const std::string &url, std::string *path) {
    if (url.empty() || !path) return;
    auto existing = indices.find(url);
    if (existing != indices.end()) {
        targets[existing->second].paths.push_back(path);
        return;
    }
    indices[url] = targets.size();
    ArtworkTarget target;
    target.url = url;
    target.paths.push_back(path);
    targets.push_back(std::move(target));
}

void CacheDiscoverArtwork(ZingMusicClient &client, DiscoverData &data) {
    std::vector<ArtworkTarget> targets;
    std::unordered_map<std::string, size_t> indices;
    auto add_group = [&](std::vector<CatalogItem> &items) {
        for (auto &item : items)
            AddArtworkTarget(targets, indices, item.thumbnail_url,
                             &item.thumbnail_path);
    };
    add_group(data.trending);
    add_group(data.artists);
    add_group(data.radio);
    add_group(data.top100);
    CacheArtworkTargets(client, targets);
}

void CacheAlbumArtwork(ZingMusicClient &client, Album &album) {
    std::vector<ArtworkTarget> targets;
    std::unordered_map<std::string, size_t> indices;
    AddArtworkTarget(targets, indices, album.artwork_url, &album.artwork_path);
    for (auto &track : album.tracks)
        AddArtworkTarget(targets, indices, track.artwork_url, &track.artwork_path);
    CacheArtworkTargets(client, targets);
}

void BuildDemoAlbum(const std::string &id, Album &album) {
    album = {};
    album.id = id;
    album.title = ContainsAny(id, {"top100"}) ? "Top 100 — bản xem trước"
                                               : "Tuyển chọn — bản xem trước";
    album.creator = "Jetsona Music";
    album.description =
        "Metadata mẫu được hiển thị vì Zing đang từ chối bộ chữ ký hiện tại.";
    for (int i = 0; i < 3; ++i) {
        Track track;
        track.id = id + ":track:" + std::to_string(i + 1);
        track.title = "Bài hát mẫu " + std::to_string(i + 1);
        track.artist = "Dữ liệu ngoại tuyến";
        track.album = album.title;
        track.duration_ms = (180 + i * 15) * 1000LL;
        album.tracks.push_back(std::move(track));
    }
}

std::string FindStreamingUrl(const json &data) {
    if (data.is_string()) {
        const std::string value = data.get<std::string>();
        if (value.rfind("https://", 0) == 0 || value.rfind("http://", 0) == 0 ||
            value.rfind("//", 0) == 0)
            return value.rfind("//", 0) == 0 ? "https:" + value : value;
        return {};
    }
    if (data.is_object()) {
        static constexpr const char *kPreferred[] = {
            "128", "320", "lossless", "url", "streamingUrl", "streamUrl"
        };
        for (const char *key : kPreferred) {
            auto it = data.find(key);
            if (it != data.end()) {
                std::string found = FindStreamingUrl(*it);
                if (!found.empty()) return found;
            }
        }
        // Only descend through known streaming envelopes. Recursing through
        // every response field can accidentally hand an artwork/link URL to
        // mpv when Zing changes the response shape.
        static constexpr const char *kContainers[] = {
            "data", "audio", "streaming", "streams", "items"
        };
        for (const char *key : kContainers) {
            auto it = data.find(key);
            if (it == data.end()) continue;
            std::string found = FindStreamingUrl(*it);
            if (!found.empty()) return found;
        }
    } else if (data.is_array()) {
        for (const auto &entry : data) {
            std::string found = FindStreamingUrl(entry);
            if (!found.empty()) return found;
        }
    }
    return {};
}

} // namespace

ZingMusicClient::ZingMusicClient() {
    EnsureCurlInit();
    ReloadConfig();
}

void ZingMusicClient::ReloadConfig() {
    version_ = EnvOr("ZING_API_VERSION", kDefaultVersion);
    api_key_ = EnvOr("ZING_API_KEY", kDefaultApiKey);
    secret_key_ = EnvOr("ZING_SECRET_KEY", kDefaultSecretKey);
    base_url_ = TrimTrailingSlashes(EnvOr("ZING_BASE_URL", kDefaultBaseUrl));
    cookies_ = GetEnv("ZING_COOKIES");
    artwork_cache_dir_ = DefaultCacheDir();
}

bool ZingMusicClient::FetchDiscover(DiscoverData &out, std::string &err) {
    ReloadConfig();
    out = {};
    std::vector<std::string> warnings;
    ClientConfig config{version_, api_key_, secret_key_, base_url_, cookies_,
                        artwork_cache_dir_};

    struct PendingRequest {
        const char *endpoint = nullptr;
        std::map<std::string, std::string> params;
        json data;
        std::string error;
        bool ok = false;
    };
    PendingRequest home{kHomeEndpoint, {{"count", "30"}, {"page", "1"}}};
    PendingRequest chart{kRealtimeChartEndpoint,
                         {{"count", "30"}, {"type", "song"}}};
    PendingRequest chart_home{kChartHomeEndpoint, {}};
    PendingRequest radio{kRadioEndpoint, {{"count", "30"}, {"page", "1"}}};
    PendingRequest top100{kTop100Endpoint, {}};
    std::vector<PendingRequest *> requests = {
        &home, &chart, &chart_home, &radio, &top100
    };
    std::vector<std::thread> workers;
    workers.reserve(requests.size());
    for (PendingRequest *request : requests) {
        workers.emplace_back([&config, request]() {
            try {
                request->ok = RequestApi(config, request->endpoint,
                                         request->params, request->data,
                                         request->error);
            } catch (const std::exception &exception) {
                request->ok = false;
                request->error = std::string("parser/request exception: ") +
                                 exception.what();
            } catch (...) {
                request->ok = false;
                request->error = "unknown parser/request exception";
            }
        });
    }
    for (auto &worker : workers) worker.join();

    if (home.ok) ParseHomeData(home.data, out);
    else warnings.push_back(home.error);

    // Home layouts change frequently. Prefer it, then consume the two chart
    // responses that were fetched in parallel so one unavailable endpoint
    // cannot make the skeleton wait through several serial timeout windows.
    if (out.trending.empty()) {
        if (chart.ok) ParseSongData(chart.data, out);
        else warnings.push_back(chart.error);
    }
    if (out.trending.empty()) {
        if (chart_home.ok) ParseSongData(chart_home.data, out);
        else warnings.push_back(chart_home.error);
    }
    if (radio.ok) ParseRadioData(radio.data, out);
    else warnings.push_back(radio.error);
    if (top100.ok)
        ParseDedicatedSection(top100.data, CatalogKind::Top100, out.top100);
    else
        warnings.push_back(top100.error);

    const bool needs_demo = out.trending.empty() || out.artists.empty() ||
                            out.radio.empty() || out.top100.empty();
    const bool any_live = home.ok || chart.ok || chart_home.ok ||
                          radio.ok || top100.ok;
    FillDemoMetadata(out, warnings);
    CacheDiscoverArtwork(*this, out);

    if (!warnings.empty()) {
        const std::string diagnostics = JoinDiagnostics(warnings, " | ");
        ESP_LOGW(TAG, "discover degraded: %s", diagnostics.c_str());
    }
    if (!any_live) {
        err = "Zing đang từ chối phiên hiện tại; đang hiển thị dữ liệu mẫu. "
              "Hãy cập nhật ZING_COOKIES rồi khởi động lại ứng dụng.";
    } else if (needs_demo) {
        err = "Một số mục Zing chưa khả dụng nên đang dùng dữ liệu mẫu.";
    } else {
        err.clear();
    }
    return any_live;
}

bool ZingMusicClient::FetchAlbum(const std::string &id, Album &out,
                                 std::string &err) {
    ReloadConfig();
    out = {};
    if (id.empty()) { err = "Zing album id is empty"; return false; }
    if (id.rfind("demo:", 0) == 0) {
        BuildDemoAlbum(id, out);
        err = "Album metadata is an offline demo; streaming is unavailable.";
        return true;
    }

    ClientConfig config{version_, api_key_, secret_key_, base_url_, cookies_,
                        artwork_cache_dir_};
    json data;
    if (!RequestApi(config, kPlaylistEndpoint, {{"id", id}}, data, err))
        return false;
    if (!data.is_object()) {
        err = "Zing playlist response has no album object";
        return false;
    }

    out.id = JsonString(data, {"encodeId", "id"});
    if (out.id.empty()) out.id = id;
    out.title = JsonString(data, {"title", "name"});
    out.creator = JsonString(data, {"artistsNames", "creator", "artist"});
    out.description = StripHtml(JsonString(data, {"description", "sortDescription"}));
    out.artwork_url = JsonString(data,
        {"thumbnailM", "thumbnail", "thumbnailR", "thumbnailHasText"});

    const json *songs = nullptr;
    auto song = data.find("song");
    if (song != data.end()) {
        if (song->is_object()) {
            auto items = song->find("items");
            if (items != song->end()) songs = &*items;
        } else if (song->is_array()) {
            songs = &*song;
        }
    }
    if (!songs) {
        auto items = data.find("songs");
        if (items != data.end()) songs = &*items;
    }
    if (!songs) {
        auto items = data.find("items");
        if (items != data.end()) songs = &*items;
    }

    if (songs) {
        std::vector<const json *> candidates;
        CollectCandidates(*songs, candidates);
        for (const json *item : candidates) {
            if (!item || !LooksLikeSong(*item)) continue;
            Track track;
            track.id = JsonString(*item, {"encodeId", "id"});
            track.title = JsonString(*item, {"title", "name"});
            track.artist = JsonString(*item, {"artistsNames", "artist", "creator"});
            if (track.artist.empty()) track.artist = out.creator;
            track.album = out.title;
            track.artwork_url = JsonString(*item,
                {"thumbnailM", "thumbnail", "thumbnailR"});
            if (track.artwork_url.empty()) track.artwork_url = out.artwork_url;
            auto duration = item->find("duration");
            if (duration != item->end())
                track.duration_ms = static_cast<int64_t>(JsonInt(*duration)) * 1000LL;
            track.premium = IsPremium(*item);
            if (!track.id.empty() && !track.title.empty())
                out.tracks.push_back(std::move(track));
        }
    }

    if (out.title.empty()) {
        err = "Zing playlist response is missing a title";
        return false;
    }
    CacheAlbumArtwork(*this, out);
    err.clear();
    return true;
}

bool ZingMusicClient::FetchStreamingUrl(const std::string &id, std::string &out,
                                        std::string &err) {
    ReloadConfig();
    out.clear();
    if (id.empty()) { err = "Zing song id is empty"; return false; }
    if (id.rfind("demo:", 0) == 0 || id.find(":track:") != std::string::npos) {
        err = "This is offline demo metadata and has no streaming URL.";
        return false;
    }

    ClientConfig config{version_, api_key_, secret_key_, base_url_, cookies_,
                        artwork_cache_dir_};
    json data;
    if (!RequestApi(config, kStreamingEndpoint, {{"id", id}}, data, err))
        return false;

    out = FindStreamingUrl(data);
    if (out.empty() || !IsSafeZingAssetUrl(config, out)) {
        out.clear();
        err = "Zing returned no playable 128/320 kbps URL (the song may require VIP).";
        return false;
    }
    err.clear();
    return true;
}

bool ZingMusicClient::DownloadArtwork(const std::string &url,
                                      std::string &out_path,
                                      std::string &err) {
    out_path.clear();
    if (url.empty()) { err = "artwork URL is empty"; return false; }

    std::string hash;
    if (!Sha256Hex(url, hash)) {
        err = "OpenSSL could not hash artwork URL";
        return false;
    }
    if (!EnsureDirectory(artwork_cache_dir_, err)) return false;

    const std::string target = artwork_cache_dir_ + "/" + hash + ".img";
    if (IsRegularNonEmptyFile(target) && HasImageMagic(target)) {
        out_path = target;
        err.clear();
        return true;
    }

    const std::string temporary = target + ".part." + std::to_string(getpid()) +
        "." + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    FILE *file = std::fopen(temporary.c_str(), "wb");
    if (!file) {
        err = "cannot create artwork cache file: " + std::string(std::strerror(errno));
        return false;
    }

    EnsureCurlInit();
    CURL *curl = curl_easy_init();
    if (!curl) {
        std::fclose(file);
        ::unlink(temporary.c_str());
        err = "curl_easy_init failed";
        return false;
    }

    FileSink sink;
    sink.file = file;
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers,
        "Accept: image/jpeg,image/png,image/*;q=0.8,*/*;q=0.5");
    ClientConfig config{version_, api_key_, secret_key_, base_url_, cookies_,
                        artwork_cache_dir_};
    const std::string resolved_url = ResolveUrl(config, url);
    if (!IsSafeZingAssetUrl(config, resolved_url)) {
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        std::fclose(file);
        ::unlink(temporary.c_str());
        err = "artwork URL host is not allowed";
        return false;
    }
    const std::string referer = base_url_ + "/";
    curl_easy_setopt(curl, CURLOPT_URL, resolved_url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
        "Mozilla/5.0 (X11; Linux aarch64) AppleWebKit/537.36 Chrome/120 Safari/537.36");
    curl_easy_setopt(curl, CURLOPT_REFERER, referer.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteFile);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    // Thumbnail hosts are public CDNs. Never attach the private Zing session
    // and never follow a CDN redirect to an unvalidated host.
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    std::fclose(file);

    if (code != CURLE_OK || status < 200 || status >= 300 || sink.overflow) {
        ::unlink(temporary.c_str());
        if (sink.overflow) err = "artwork exceeds 12 MiB";
        else if (code != CURLE_OK) err = curl_easy_strerror(code);
        else err = "artwork HTTP " + std::to_string(status);
        return false;
    }
    if (!HasImageMagic(temporary)) {
        ::unlink(temporary.c_str());
        err = "artwork is not PNG/JPEG (enabled LVGL decoders)";
        return false;
    }

    if (::rename(temporary.c_str(), target.c_str()) != 0) {
        // A parallel request for the same URL may have won the race.
        if (IsRegularNonEmptyFile(target) && HasImageMagic(target)) {
            ::unlink(temporary.c_str());
        } else {
            err = "cannot finalize artwork cache file: " +
                  std::string(std::strerror(errno));
            ::unlink(temporary.c_str());
            return false;
        }
    }
    out_path = target;
    err.clear();
    return true;
}

} // namespace jetson
