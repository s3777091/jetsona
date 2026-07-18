#include "app/boot_prefetch.h"

#include "application.h"
#include "display/common/lvgl_utils.h"
#include "esp_log.h"
#include "net/zing_discover_cache.h"
#include "net/zing_music_client.h"
#include "platform/perf_governor.h"
#include "platform/task_pool.h"

#include <lvgl.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#define TAG "BootPrefetch"

namespace jetson {
namespace {

constexpr int kDefaultImageCacheMb = 128;
constexpr int kDefaultMinFreeMb = 500;
/* Give the splash, first frames and DHCP a moment before burning CPU. */
constexpr int kWarmupDelayMs = 3000;
constexpr int kFetchAttempts = 3;
constexpr int kFetchRetryDelayMs = 20000;
constexpr int kMemCheckPeriodSec = 30;
/* After an emergency cache drop, back off so a persistently tight system
 * does not thrash decode -> drop -> decode. */
constexpr int kMemDropBackoffSec = 300;

int EnvMb(const char *name, int fallback) {
    const char *value = std::getenv(name);
    if (!value || !value[0]) return fallback;
    const long parsed = std::strtol(value, nullptr, 10);
    return (parsed > 0 && parsed < 4096) ? static_cast<int>(parsed) : fallback;
}

/* MemAvailable in MB, or -1 when unreadable (non-Linux dev host). */
long MemAvailableMb() {
    std::FILE *file = std::fopen("/proc/meminfo", "r");
    if (!file) return -1;
    char line[128];
    long kb = -1;
    while (std::fgets(line, sizeof(line), file)) {
        if (std::sscanf(line, "MemAvailable: %ld kB", &kb) == 1) break;
    }
    std::fclose(file);
    return kb < 0 ? -1 : kb / 1024;
}

void RefreshDiscoverSnapshot() {
    for (int attempt = 0; attempt < kFetchAttempts; ++attempt) {
        if (attempt > 0)
            std::this_thread::sleep_for(
                std::chrono::milliseconds(kFetchRetryDelayMs));
        // Fresh token per attempt: the boost never outlives one fetch and is
        // not held across the long retry sleeps.
        auto boost = PerfGovernor::Instance().Acquire("boot-prefetch");
        jetson::music::DiscoverData data;
        std::string error;
        bool ok = false;
        try {
            ZingMusicClient client;
            ok = client.FetchDiscover(data, error);
        } catch (const std::exception &exception) {
            error = exception.what();
        } catch (...) {
            error = "unknown exception";
        }
        if (ok) {
            jetson::music::SaveDiscoverCache(data);
            ESP_LOGI(TAG, "discover snapshot warmed on attempt %d", attempt + 1);
            return;
        }
        ESP_LOGW(TAG, "discover warm-up attempt %d failed: %s", attempt + 1,
                 error.c_str());
    }
}

void StartMemoryMonitor() {
    const int min_free_mb = EnvMb("JETSON_MIN_FREE_MB", kDefaultMinFreeMb);
    // Forever-loop, so a plain detached thread instead of a pool worker.
    std::thread([min_free_mb]() {
        for (;;) {
            std::this_thread::sleep_for(
                std::chrono::seconds(kMemCheckPeriodSec));
            const long available = MemAvailableMb();
            if (available < 0) return; // no /proc/meminfo: nothing to guard
            if (available >= min_free_mb) continue;
            ESP_LOGW(TAG, "MemAvailable %ld MB < %d MB; dropping image cache",
                     available, min_free_mb);
            Application::GetInstance().Schedule([]() {
                jetson::ui::LvglLockGuard lock;
                lv_image_cache_drop(nullptr);
            });
            std::this_thread::sleep_for(
                std::chrono::seconds(kMemDropBackoffSec));
        }
    }).detach();
}

} // namespace

void StartBootPrefetch() {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;

    // Enlarge the shared image cache before anything heavy is decoded. Done
    // on the UI thread with the LVGL lock like every other lv_ call.
    const int cache_mb = EnvMb("JETSON_IMG_CACHE_MB", kDefaultImageCacheMb);
    Application::GetInstance().Schedule([cache_mb]() {
        jetson::ui::LvglLockGuard lock;
        lv_image_cache_resize(
            static_cast<uint32_t>(cache_mb) * 1024u * 1024u, false);
        ESP_LOGI(TAG, "image cache sized to %d MB", cache_mb);
    });

    TaskPool::Instance().Post(TaskPool::Lane::Prefetch, []() {
        std::this_thread::sleep_for(std::chrono::milliseconds(kWarmupDelayMs));
        RefreshDiscoverSnapshot();
    });

    StartMemoryMonitor();
}

} // namespace jetson
