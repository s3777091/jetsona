#pragma once

/* Shared helpers for the DS-02 wallpaper set (under assets/backgrounds/).
 *
 * The set is discovered at runtime by scanning the directory (excluding the
 * thumbs/ subfolder) so images can be added or deleted without rebuilding --
 * the gallery's "Xóa ảnh" action deletes files on disk and the list updates.
 * The home screen and the gallery both call ListBackgroundFiles() so they agree
 * on what's available. Selections are stored by filename (not index) so they
 * survive deletions that would otherwise shift indices. */

#include <string>
#include <vector>

namespace jetson::ui::backgrounds {

std::string BackgroundsDir();

// Wallpaper basenames currently in the backgrounds dir (*.png, excluding the
// thumbs/ subfolder), sorted for a stable display order. Empty on error.
std::vector<std::string> ListBackgroundFiles();

std::string BackgroundPath(const std::string &file);

std::string ThumbPath(const std::string &file);

} // namespace jetson::ui::backgrounds
