#pragma once

#include "media/music_types.h"

#include <mutex>
#include <string>
#include <vector>

namespace jetson::music {

/* A user-curated album ("Album của tôi") holding favourite tracks. */
struct UserAlbum {
    std::string id;
    std::string name;
    std::vector<Track> tracks;
};

/* Persistent store for the user's own albums, backed by a small JSON file next
 * to settings.kv (~/.jetson-fw/music_albums.json). All methods are thread-safe;
 * callers use them from LVGL event callbacks. Writes are flushed immediately so
 * the list survives a reboot. */
class UserLibrary {
public:
    static UserLibrary &Instance();

    std::vector<UserAlbum> Albums();
    bool GetAlbum(const std::string &album_id, UserAlbum &out);
    bool Contains(const std::string &album_id, const std::string &track_id);

    // Creates an album and returns its id. An empty name is auto-generated
    // ("Album của tôi", "Album của tôi 2", ...).
    std::string CreateAlbum(const std::string &name);
    // Adds a track (deduped by track id). Returns false if the album is gone.
    bool AddTrack(const std::string &album_id, const Track &track);
    bool RemoveTrack(const std::string &album_id, const std::string &track_id);
    bool RemoveAlbum(const std::string &album_id);

    UserLibrary(const UserLibrary &) = delete;
    UserLibrary &operator=(const UserLibrary &) = delete;

private:
    UserLibrary();
    void LoadLocked();
    void SaveLocked();
    std::string FilePath() const;

    std::mutex mutex_;
    std::vector<UserAlbum> albums_;
    int next_id_ = 1;
};

} // namespace jetson::music
