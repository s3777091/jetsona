#pragma once

#include "media/music_types.h"

#include <string>
#include <vector>

namespace jetson {

/* Blocking Zing MP3 browse/stream client.
 *
 * Every public request performs network and disk I/O; callers must run it on a
 * worker thread.  API credentials and cookies are read from the process
 * environment by ReloadConfig(). The compatibility key/secret defaults mirror
 * the Strawberry reference implementation; anonymous requests establish their
 * own in-memory Zing session, while deployments may still provide account
 * cookies or override rotated credentials without rebuilding the firmware.
 */
class ZingMusicClient {
public:
    ZingMusicClient();

    bool FetchDiscover(music::DiscoverData &out, std::string &err);
    /* Full-text song search, best match first. Returns at most `limit` tracks
     * with id/title/artist/artwork_url filled; streaming_url and artwork_path
     * are left empty for the caller to resolve (PlayerController already does
     * the former when a queue is played). False with `err` set when the API
     * rejects the request or nothing matched. */
    bool SearchSongs(const std::string &query, int limit,
                     std::vector<music::Track> &out, std::string &err);
    bool FetchAlbum(const std::string &id, music::Album &out, std::string &err);
    /* Download the per-track covers after album metadata is already visible.
     * This is intentionally separate from FetchAlbum: a large playlist must
     * not hold the UI on its loading skeleton while dozens of CDN requests
     * finish. The supplied album is updated with the cached file paths. */
    void WarmAlbumArtwork(music::Album &album);
    bool FetchStreamingUrl(const std::string &id, std::string &out,
                           std::string &err);
    bool DownloadArtwork(const std::string &url, std::string &out_path,
                         std::string &err);

    void ReloadConfig();

    /* Directory holding cached artwork and the persisted discover snapshot
     * (see net/zing_discover_cache.h). Honors ZING_ARTWORK_CACHE_DIR. */
    static std::string CacheDir();

private:
    std::string version_;
    std::string api_key_;
    std::string secret_key_;
    std::string base_url_;
    std::string cookies_;
    std::string artwork_cache_dir_;
};

} // namespace jetson
