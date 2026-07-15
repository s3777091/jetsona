#pragma once

/* Shared list of DS-02 wallpaper filenames (under assets/backgrounds/).
 * Single source so the home screen and the background gallery agree. */

#include <cstddef>
#include <cstdlib>
#include <string>

namespace home {

constexpr size_t kBackgroundCount = 10;

inline const char *const kBackgroundFiles[kBackgroundCount] = {
    "abtract.png", "anime_light.png", "cat_dog_chill.png", "europe.png",
    "haivan.png", "linh_ung_pagoda.png", "morning_beach.png", "night_light.png",
    "rong_river.png", "sa_mac.png",
};

inline const char *BackgroundFile(size_t index) {
    return (index < kBackgroundCount) ? kBackgroundFiles[index] : nullptr;
}

inline std::string BackgroundsDir() {
    const char *env = std::getenv("JETSON_ASSETS_DIR_OVERRIDE");
    std::string dir = (env && env[0]) ? env : JETSON_ASSETS_DIR;
    return dir + "/backgrounds";
}

} // namespace home