#include "net/zing_discover_cache.h"

#include "esp_log.h"
#include "net/zing_music_client.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#define TAG "ZingCache"

namespace jetson::music {
namespace {

using json = nlohmann::json;

constexpr int kSchemaVersion = 1;
/* A week-old rail list is still better than a skeleton, but eventually the
 * editorial content is too stale to pretend it is current. */
constexpr std::time_t kMaxAgeSeconds = 7 * 24 * 60 * 60;
constexpr char kFileName[] = "discover_cache.json";

std::string CachePath() {
    return jetson::ZingMusicClient::CacheDir() + "/" + kFileName;
}

void EnsureDir(const std::string &path) {
    // mkdir -p: create each component, ignoring EEXIST.
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i != path.size() && path[i] != '/') continue;
        mkdir(path.substr(0, i).c_str(), 0755);
    }
}

bool FileExists(const std::string &path) {
    struct stat st{};
    return !path.empty() && stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

json ItemToJson(const CatalogItem &item) {
    return json{{"id", item.id},
                {"kind", static_cast<int>(item.kind)},
                {"title", item.title},
                {"subtitle", item.subtitle},
                {"thumbnail_url", item.thumbnail_url},
                {"thumbnail_path", item.thumbnail_path},
                {"streaming_url", item.streaming_url},
                {"duration_seconds", item.duration_seconds},
                {"premium", item.premium}};
}

CatalogItem ItemFromJson(const json &value) {
    CatalogItem item;
    item.id = value.value("id", "");
    item.kind = static_cast<CatalogKind>(value.value("kind", 0));
    item.title = value.value("title", "");
    item.subtitle = value.value("subtitle", "");
    item.thumbnail_url = value.value("thumbnail_url", "");
    item.thumbnail_path = value.value("thumbnail_path", "");
    item.streaming_url = value.value("streaming_url", "");
    item.duration_seconds = value.value("duration_seconds", 0);
    item.premium = value.value("premium", false);
    // Artwork may have been evicted since the snapshot was written; a cleared
    // path renders as the neutral placeholder instead of a broken image.
    if (!item.thumbnail_path.empty() && !FileExists(item.thumbnail_path))
        item.thumbnail_path.clear();
    return item;
}

json SectionToJson(const std::vector<CatalogItem> &items) {
    json array = json::array();
    for (const auto &item : items) array.push_back(ItemToJson(item));
    return array;
}

void SectionFromJson(const json &array, std::vector<CatalogItem> &out) {
    if (!array.is_array()) return;
    for (const auto &value : array)
        if (value.is_object()) out.push_back(ItemFromJson(value));
}

} // namespace

bool LoadDiscoverCache(DiscoverData &out) {
    const std::string path = CachePath();
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    std::string body;
    char buffer[4096];
    size_t got;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
        body.append(buffer, got);
    std::fclose(file);

    try {
        const json root = json::parse(body);
        if (root.value("schema", 0) != kSchemaVersion) return false;
        const std::time_t written = root.value("written", (std::time_t)0);
        const std::time_t now = std::time(nullptr);
        if (written <= 0 || now - written > kMaxAgeSeconds) return false;

        DiscoverData data;
        SectionFromJson(root.value("personalized", json::array()), data.personalized);
        SectionFromJson(root.value("new_releases", json::array()), data.new_releases);
        SectionFromJson(root.value("chill", json::array()), data.chill);
        SectionFromJson(root.value("top100", json::array()), data.top100);
        SectionFromJson(root.value("artists", json::array()), data.artists);
        SectionFromJson(root.value("radio", json::array()), data.radio);
        if (data.personalized.empty() && data.new_releases.empty() &&
            data.chill.empty() && data.top100.empty() && data.artists.empty() &&
            data.radio.empty())
            return false;
        out = std::move(data);
        ESP_LOGI(TAG, "loaded discover snapshot (%lds old)",
                 (long)(now - written));
        return true;
    } catch (const std::exception &exception) {
        ESP_LOGW(TAG, "snapshot unreadable: %s", exception.what());
        return false;
    }
}

void SaveDiscoverCache(const DiscoverData &data) {
    json root{{"schema", kSchemaVersion},
              {"written", (std::int64_t)std::time(nullptr)},
              {"personalized", SectionToJson(data.personalized)},
              {"new_releases", SectionToJson(data.new_releases)},
              {"chill", SectionToJson(data.chill)},
              {"top100", SectionToJson(data.top100)},
              {"artists", SectionToJson(data.artists)},
              {"radio", SectionToJson(data.radio)}};

    const std::string dir = jetson::ZingMusicClient::CacheDir();
    EnsureDir(dir);
    const std::string path = CachePath();
    const std::string tmp = path + ".tmp";
    std::FILE *file = std::fopen(tmp.c_str(), "wb");
    if (!file) {
        ESP_LOGW(TAG, "cannot write %s", tmp.c_str());
        return;
    }
    const std::string body = root.dump();
    const bool ok =
        std::fwrite(body.data(), 1, body.size(), file) == body.size();
    if (std::fclose(file) != 0 || !ok) {
        std::remove(tmp.c_str());
        ESP_LOGW(TAG, "short write on discover snapshot");
        return;
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        ESP_LOGW(TAG, "could not swap discover snapshot into place");
        return;
    }
    ESP_LOGI(TAG, "discover snapshot saved");
}

} // namespace jetson::music
