#pragma once

/* Persisted snapshot of the last successful Zing discover fetch.
 *
 * MusicView renders this snapshot instantly on open (the artwork it points to
 * is already on disk in the same cache directory) and refreshes from the
 * network in the background — stale-while-revalidate — so the skeleton page
 * only ever appears on a genuinely cold cache. Boot prefetch writes the same
 * file so the very first open after power-on is warm too.
 *
 * Both functions do disk I/O; call them from a worker, never the LVGL
 * thread. */

#include "media/music_types.h"

namespace jetson::music {

/* Load the snapshot into `out`. Returns false (and leaves `out` untouched)
 * when the file is missing, unparsable, from an incompatible schema, or
 * expired. Thumbnail paths whose files vanished are cleared, not fatal. */
bool LoadDiscoverCache(DiscoverData &out);

/* Atomically replace the snapshot (tmp file + rename). Best-effort. */
void SaveDiscoverCache(const DiscoverData &data);

} // namespace jetson::music
