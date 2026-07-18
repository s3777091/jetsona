#include "media/user_library.h"

#include "esp_log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>

#define TAG "UserLibrary"

namespace jetson::music {
namespace {
using json = nlohmann::json;
} // namespace

UserLibrary &UserLibrary::Instance() {
    static UserLibrary instance;
    return instance;
}

UserLibrary::UserLibrary() {
    std::lock_guard<std::mutex> lock(mutex_);
    LoadLocked();
}

std::string UserLibrary::FilePath() const {
    const char *env = std::getenv("JETSON_MUSIC_ALBUMS_FILE");
    if (env && env[0]) return env;
    const char *home = std::getenv("HOME");
    std::string dir = (home && home[0]) ? std::string(home) + "/.jetson-fw"
                                        : std::string("/tmp/.jetson-fw");
    ::mkdir(dir.c_str(), 0775);
    return dir + "/music_albums.json";
}

void UserLibrary::LoadLocked() {
    albums_.clear();
    next_id_ = 1;

    std::ifstream in(FilePath(), std::ios::binary);
    if (!in) return;
    json root = json::parse(in, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        ESP_LOGW(TAG, "album store is not valid JSON; starting empty");
        return;
    }

    auto albums = root.find("albums");
    if (albums != root.end() && albums->is_array()) {
        for (const auto &entry : *albums) {
            if (!entry.is_object()) continue;
            UserAlbum album;
            album.id = entry.value("id", std::string());
            album.name = entry.value("name", std::string());
            if (album.id.empty() || album.name.empty()) continue;

            auto tracks = entry.find("tracks");
            if (tracks != entry.end() && tracks->is_array()) {
                for (const auto &item : *tracks) {
                    if (!item.is_object()) continue;
                    Track track;
                    track.id = item.value("id", std::string());
                    track.title = item.value("title", std::string());
                    track.artist = item.value("artist", std::string());
                    track.album = item.value("album", std::string());
                    track.artwork_url = item.value("artwork_url", std::string());
                    track.artwork_path = item.value("artwork_path", std::string());
                    track.duration_ms = item.value("duration_ms",
                                                   static_cast<int64_t>(0));
                    track.premium = item.value("premium", false);
                    if (!track.id.empty() && !track.title.empty())
                        album.tracks.push_back(std::move(track));
                }
            }
            albums_.push_back(std::move(album));
        }
    }

    next_id_ = root.value("next_id", static_cast<int>(albums_.size()) + 1);
    if (next_id_ < 1) next_id_ = 1;
}

void UserLibrary::SaveLocked() {
    json root;
    root["next_id"] = next_id_;
    json albums = json::array();
    for (const auto &album : albums_) {
        json entry;
        entry["id"] = album.id;
        entry["name"] = album.name;
        json tracks = json::array();
        for (const auto &track : album.tracks) {
            // streaming_url is deliberately not persisted: Zing URLs expire, so
            // playback re-resolves them from the track id.
            tracks.push_back({
                {"id", track.id},
                {"title", track.title},
                {"artist", track.artist},
                {"album", track.album},
                {"artwork_url", track.artwork_url},
                {"artwork_path", track.artwork_path},
                {"duration_ms", track.duration_ms},
                {"premium", track.premium},
            });
        }
        entry["tracks"] = std::move(tracks);
        albums.push_back(std::move(entry));
    }
    root["albums"] = std::move(albums);

    // Write through a temp file so a crash mid-write cannot truncate the store.
    const std::string path = FilePath();
    const std::string temporary = path + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            ESP_LOGW(TAG, "cannot open %s for writing", temporary.c_str());
            return;
        }
        out << root.dump();
        if (!out) {
            ESP_LOGW(TAG, "write failed for %s", temporary.c_str());
            return;
        }
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        ESP_LOGW(TAG, "cannot finalize %s", path.c_str());
        std::remove(temporary.c_str());
    }
}

std::vector<UserAlbum> UserLibrary::Albums() {
    std::lock_guard<std::mutex> lock(mutex_);
    return albums_;
}

bool UserLibrary::GetAlbum(const std::string &album_id, UserAlbum &out) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &album : albums_) {
        if (album.id == album_id) { out = album; return true; }
    }
    return false;
}

bool UserLibrary::Contains(const std::string &album_id,
                           const std::string &track_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &album : albums_) {
        if (album.id != album_id) continue;
        return std::any_of(album.tracks.begin(), album.tracks.end(),
            [&](const Track &track) { return track.id == track_id; });
    }
    return false;
}

std::string UserLibrary::CreateAlbum(const std::string &name) {
    std::lock_guard<std::mutex> lock(mutex_);
    UserAlbum album;
    album.id = "ua_" + std::to_string(next_id_++);
    album.name = !name.empty()
        ? name
        : (albums_.empty()
               ? std::string("Album của tôi")
               : "Album của tôi " + std::to_string(albums_.size() + 1));
    const std::string id = album.id;
    albums_.push_back(std::move(album));
    SaveLocked();
    return id;
}

bool UserLibrary::AddTrack(const std::string &album_id, const Track &track) {
    if (track.id.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &album : albums_) {
        if (album.id != album_id) continue;
        const bool duplicate = std::any_of(album.tracks.begin(),
            album.tracks.end(),
            [&](const Track &existing) { return existing.id == track.id; });
        if (!duplicate) {
            Track stored = track;
            stored.streaming_url.clear();
            album.tracks.push_back(std::move(stored));
            SaveLocked();
        }
        return true;
    }
    return false;
}

bool UserLibrary::RemoveTrack(const std::string &album_id,
                              const std::string &track_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &album : albums_) {
        if (album.id != album_id) continue;
        const auto found = std::remove_if(album.tracks.begin(),
            album.tracks.end(),
            [&](const Track &track) { return track.id == track_id; });
        if (found == album.tracks.end()) return false;
        album.tracks.erase(found, album.tracks.end());
        SaveLocked();
        return true;
    }
    return false;
}

bool UserLibrary::RemoveAlbum(const std::string &album_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = std::remove_if(albums_.begin(), albums_.end(),
        [&](const UserAlbum &album) { return album.id == album_id; });
    if (found == albums_.end()) return false;
    albums_.erase(found, albums_.end());
    SaveLocked();
    return true;
}

} // namespace jetson::music
