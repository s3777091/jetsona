#pragma once

#include "media/music_types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace jetson::music {

/* Process-backed music player shared by MusicView and the Dynamic Island.
 *
 * Public commands only update the in-memory snapshot and enqueue work. Stream
 * resolution and all process operations run on the controller's worker thread,
 * so callers may safely invoke these methods from an LVGL event callback.
 * Snapshot() is thread-safe and derives an approximate position from a steady
 * clock while playback is active. */
class PlayerController {
public:
    static PlayerController &Instance();

    PlayerSnapshot Snapshot() const;

    void PlayQueue(std::vector<Track> queue, size_t start);
    void Toggle();
    void Pause();
    void Resume();
    void Previous();
    void Next();
    void SeekTo(int64_t position_ms);
    void Stop();
    void SetVolume(int volume);
    void SetMuted(bool muted);

    ~PlayerController();

    PlayerController(const PlayerController &) = delete;
    PlayerController &operator=(const PlayerController &) = delete;

private:
    PlayerController();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace jetson::music
