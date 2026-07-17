#pragma once

#include "media/music_types.h"

#include <string>

namespace jetson {

/* Blocking Zing MP3 browse/stream client.
 *
 * Every public request performs network and disk I/O; callers must run it on a
 * worker thread.  API credentials and cookies are read from the process
 * environment by ReloadConfig().  The compatibility key/secret defaults mirror
 * the Strawberry reference implementation, but Zing rotates/rejects them from
 * time to time, so deployments can override all authentication inputs without
 * rebuilding the firmware.
 */
class ZingMusicClient {
public:
    ZingMusicClient();

    bool FetchDiscover(music::DiscoverData &out, std::string &err);
    bool FetchAlbum(const std::string &id, music::Album &out, std::string &err);
    bool FetchStreamingUrl(const std::string &id, std::string &out,
                           std::string &err);
    bool DownloadArtwork(const std::string &url, std::string &out_path,
                         std::string &err);

    void ReloadConfig();

private:
    std::string version_;
    std::string api_key_;
    std::string secret_key_;
    std::string base_url_;
    std::string cookies_;
    std::string artwork_cache_dir_;
};

} // namespace jetson
