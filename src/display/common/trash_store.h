#pragma once

#include <cstddef>
#include <ctime>
#include <string>
#include <vector>

namespace jetson::ui::trash {

/* A wallpaper moved to the persistent backgrounds/.trash directory.  The
 * stored filename carries the deletion timestamp and original basename, so
 * the trash remains recoverable across application restarts without a
 * separate metadata database. */
struct Item {
    std::string id;
    std::string original_name;
    std::string stored_path;
    std::string thumbnail_path;
    long size = 0;
    std::time_t deleted_at = 0;
    bool has_thumbnail = false;
};

std::string TrashDir();

// Move the full wallpaper and its optional thumbnail into Trash.
bool MoveBackgroundToTrash(const std::string &file);

// Newest deleted items are returned first.
std::vector<Item> ListBackgroundTrash();

// Restore both files to assets/backgrounds and assets/backgrounds/thumbs.
// Returns false without overwriting if a file with the original name exists.
bool RestoreBackground(const Item &item);

// Permanently remove one item, or every item currently in Trash.
bool PermanentlyDeleteBackground(const Item &item);
size_t EmptyBackgroundTrash();

} // namespace jetson::ui::trash
