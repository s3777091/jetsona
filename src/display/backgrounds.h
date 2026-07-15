#pragma once

/* Shared helpers for the DS-02 wallpaper set (under assets/backgrounds/).
 *
 * The set is discovered at runtime by scanning the directory (excluding the
 * thumbs/ subfolder) so images can be added or deleted without rebuilding --
 * the gallery's "Xóa ảnh" action deletes files on disk and the list updates.
 * The home screen and the gallery both call ListBackgroundFiles() so they agree
 * on what's available. Selections are stored by filename (not index) so they
 * survive deletions that would otherwise shift indices. */

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <dirent.h>
#include <string>
#include <vector>

namespace home {

inline std::string BackgroundsDir() {
    const char *env = std::getenv("JETSON_ASSETS_DIR_OVERRIDE");
    std::string dir = (env && env[0]) ? env : JETSON_ASSETS_DIR;
    return dir + "/backgrounds";
}

// Wallpaper basenames currently in the backgrounds dir (*.png, excluding the
// thumbs/ subfolder), sorted for a stable display order. Empty on error.
inline std::vector<std::string> ListBackgroundFiles() {
    std::vector<std::string> out;
    std::string dir = BackgroundsDir();
    if (DIR *d = opendir(dir.c_str())) {
        while (struct dirent *de = readdir(d)) {
            std::string name = de->d_name;
            if (name.size() < 5 || name == "thumbs") continue;
            if (name.compare(name.size() - 4, 4, ".png") != 0) continue;
            out.push_back(name);
        }
        closedir(d);
        std::sort(out.begin(), out.end());
    }
    return out;
}

inline std::string BackgroundPath(const std::string &file) {
    return BackgroundsDir() + "/" + file;
}

inline std::string ThumbPath(const std::string &file) {
    return BackgroundsDir() + "/thumbs/" + file;
}

} // namespace home