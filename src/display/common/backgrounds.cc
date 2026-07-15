#include "display/common/backgrounds.h"

#include <algorithm>
#include <cstdlib>
#include <dirent.h>
#include <utility>

namespace jetson::ui::backgrounds {

std::string BackgroundsDir() {
    const char *env = std::getenv("JETSON_ASSETS_DIR_OVERRIDE");
    std::string dir = (env && env[0]) ? env : JETSON_ASSETS_DIR;
    return dir + "/backgrounds";
}

std::vector<std::string> ListBackgroundFiles() {
    std::vector<std::string> files;
    const std::string dir = BackgroundsDir();
    if (DIR *handle = opendir(dir.c_str())) {
        while (dirent *entry = readdir(handle)) {
            std::string name = entry->d_name;
            if (name.size() < 5 || name == "thumbs") continue;
            if (name.compare(name.size() - 4, 4, ".png") != 0) continue;
            files.push_back(std::move(name));
        }
        closedir(handle);
        std::sort(files.begin(), files.end());
    }
    return files;
}

std::string BackgroundPath(const std::string &file) {
    return BackgroundsDir() + "/" + file;
}

std::string ThumbPath(const std::string &file) {
    return BackgroundsDir() + "/thumbs/" + file;
}

} // namespace jetson::ui::backgrounds
