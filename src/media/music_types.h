#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace jetson::music {

enum class CatalogKind {
    Song,
    Playlist,
    Artist,
    Radio,
    Top100,
    // A locally stored album owned by the user (see media/user_library.h).
    // Never produced by the Zing parser; only by MusicView's own section.
    UserAlbum,
};

struct CatalogItem {
    std::string id;
    CatalogKind kind = CatalogKind::Playlist;
    std::string title;
    std::string subtitle;
    std::string thumbnail_url;
    std::string thumbnail_path;
    std::string streaming_url;
    int duration_seconds = 0;
    bool premium = false;
};

struct DiscoverData {
    std::vector<CatalogItem> personalized;
    std::vector<CatalogItem> new_releases;
    std::vector<CatalogItem> chill;
    std::vector<CatalogItem> top100;
    std::vector<CatalogItem> artists;
    std::vector<CatalogItem> radio;
};

struct Track {
    std::string id;
    std::string title;
    std::string artist;
    std::string album;
    std::string artwork_url;
    std::string artwork_path;
    std::string streaming_url;
    int64_t duration_ms = 0;
    bool premium = false;
};

struct Album {
    std::string id;
    std::string title;
    std::string creator;
    std::string description;
    std::string artwork_url;
    std::string artwork_path;
    std::vector<Track> tracks;
};

enum class PlaybackStatus {
    Idle,
    Resolving,
    Buffering,
    Playing,
    Paused,
    Ended,
    Error,
};

struct PlayerSnapshot {
    uint64_t revision = 0;
    PlaybackStatus status = PlaybackStatus::Idle;
    bool has_current = false;
    Track current;
    std::vector<Track> queue;
    size_t index = 0;
    int64_t position_ms = 0;
    int64_t duration_ms = 0;
    int volume = 50;
    bool muted = false;
    bool can_previous = false;
    bool can_next = false;
    std::string error;
};

} // namespace jetson::music
