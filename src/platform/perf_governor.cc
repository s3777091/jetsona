#include "platform/perf_governor.h"

#include "esp_log.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define TAG "PerfGov"

namespace jetson {
namespace {

constexpr int kBoostTimeoutMs = 15000;
/* Millicelsius. Above kBoostMaxTemp new boosts are refused; above kAbortTemp
 * the watchdog drops an active boost. */
constexpr long kBoostMaxTemp = 70000;
constexpr long kAbortTemp = 75000;

constexpr char kCpuRoot[] = "/sys/devices/system/cpu";
constexpr char kThermalRoot[] = "/sys/class/thermal";
constexpr char kBaselineDir[] = "/var/lib/jetson-fw";
constexpr char kBaselineFile[] = "/var/lib/jetson-fw/perf.baseline";
constexpr char kBaselineFallback[] = "/tmp/jetson-fw-perf.baseline";

bool ReadSysfs(const std::string &path, std::string &out) {
    std::FILE *file = std::fopen(path.c_str(), "r");
    if (!file) return false;
    char buffer[128] = {0};
    const size_t got = std::fread(buffer, 1, sizeof(buffer) - 1, file);
    std::fclose(file);
    if (got == 0) return false;
    out.assign(buffer, got);
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back())))
        out.pop_back();
    return !out.empty();
}

bool WriteSysfs(const std::string &path, const std::string &value) {
    std::FILE *file = std::fopen(path.c_str(), "w");
    if (!file) return false;
    const bool ok = std::fwrite(value.data(), 1, value.size(), file) ==
                    value.size();
    std::fclose(file);
    return ok;
}

/* Hottest thermal zone in millicelsius, or -1 when unreadable. */
long MaxTempMilli() {
    DIR *dir = opendir(kThermalRoot);
    if (!dir) return -1;
    long max_temp = -1;
    while (struct dirent *entry = readdir(dir)) {
        if (std::strncmp(entry->d_name, "thermal_zone", 12) != 0) continue;
        std::string value;
        if (!ReadSysfs(std::string(kThermalRoot) + "/" + entry->d_name +
                           "/temp",
                       value))
            continue;
        const long temp = std::strtol(value.c_str(), nullptr, 10);
        if (temp > max_temp) max_temp = temp;
    }
    closedir(dir);
    return max_temp;
}

struct CpuNode {
    std::string min_freq_path;
    std::string baseline_min;
    std::string max_freq;
};

std::vector<CpuNode> DiscoverCpus() {
    std::vector<CpuNode> cpus;
    DIR *dir = opendir(kCpuRoot);
    if (!dir) return cpus;
    while (struct dirent *entry = readdir(dir)) {
        const char *name = entry->d_name;
        if (std::strncmp(name, "cpu", 3) != 0 || !std::isdigit((unsigned char)name[3]))
            continue;
        bool all_digits = true;
        for (const char *c = name + 3; *c; ++c)
            if (!std::isdigit((unsigned char)*c)) { all_digits = false; break; }
        if (!all_digits) continue;
        const std::string cpufreq = std::string(kCpuRoot) + "/" + name + "/cpufreq";
        CpuNode node;
        node.min_freq_path = cpufreq + "/scaling_min_freq";
        if (!ReadSysfs(node.min_freq_path, node.baseline_min)) continue;
        if (!ReadSysfs(cpufreq + "/cpuinfo_max_freq", node.max_freq)) continue;
        cpus.push_back(std::move(node));
    }
    closedir(dir);
    return cpus;
}

bool BoostDisabled() {
    const char *env = std::getenv("JETSON_PERF_BOOST");
    return env && env[0] == '0' && env[1] == '\0';
}

} // namespace

struct PerfGovernor::Impl {
    std::mutex mtx;
    std::condition_variable cv;
    std::vector<CpuNode> boosted_cpus; // nodes we changed, with their baselines
    int count = 0;
    bool boosted = false;
    bool watchdog_started = false;
    std::chrono::steady_clock::time_point deadline{};
    std::string baseline_path; // where the crash-recovery file was written

    /* mtx held. Capture baselines, persist them, raise min freq to max. */
    void Apply() {
        std::vector<CpuNode> cpus = DiscoverCpus();
        if (cpus.empty()) {
            ESP_LOGW(TAG, "no cpufreq nodes writable; boost is a no-op");
            return;
        }
        // Persist the baseline before touching anything, so a crash between
        // the writes below and Restore() is recoverable on the next launch.
        mkdir(kBaselineDir, 0755);
        const char *candidates[] = {kBaselineFile, kBaselineFallback};
        baseline_path.clear();
        for (const char *candidate : candidates) {
            std::FILE *file = std::fopen(candidate, "w");
            if (!file) continue;
            bool ok = true;
            for (const auto &cpu : cpus)
                if (std::fprintf(file, "%s %s\n", cpu.min_freq_path.c_str(),
                                 cpu.baseline_min.c_str()) < 0)
                    ok = false;
            if (std::fclose(file) != 0) ok = false;
            if (ok) { baseline_path = candidate; break; }
            std::remove(candidate);
        }
        if (baseline_path.empty())
            ESP_LOGW(TAG, "could not persist clock baseline; boosting anyway "
                          "(watchdog still restores in-process)");

        int raised = 0;
        for (auto &cpu : cpus)
            if (WriteSysfs(cpu.min_freq_path, cpu.max_freq)) ++raised;
        boosted_cpus = std::move(cpus);
        boosted = raised > 0;
        if (boosted)
            ESP_LOGI(TAG, "boost on: %d cores min_freq -> max", raised);
        else if (!baseline_path.empty()) {
            std::remove(baseline_path.c_str());
            baseline_path.clear();
        }
    }

    /* mtx held. Put every changed core back to its captured baseline. */
    void Restore(const char *why) {
        if (!boosted) return;
        int restored = 0;
        for (const auto &cpu : boosted_cpus)
            if (WriteSysfs(cpu.min_freq_path, cpu.baseline_min)) ++restored;
        ESP_LOGI(TAG, "boost off (%s): %d cores restored", why, restored);
        boosted = false;
        boosted_cpus.clear();
        if (!baseline_path.empty()) {
            std::remove(baseline_path.c_str());
            baseline_path.clear();
        }
    }

    /* mtx held. */
    void StartWatchdog() {
        if (watchdog_started) return;
        watchdog_started = true;
        std::thread([this]() {
            std::unique_lock<std::mutex> lk(mtx);
            for (;;) {
                cv.wait_for(lk, std::chrono::seconds(1));
                if (!boosted) continue;
                if (std::chrono::steady_clock::now() >= deadline) {
                    ESP_LOGW(TAG, "boost held past %d ms; forcing restore",
                             kBoostTimeoutMs);
                    Restore("watchdog timeout");
                    continue;
                }
                const long temp = MaxTempMilli();
                if (temp > kAbortTemp) {
                    ESP_LOGW(TAG, "SoC at %ld mC; dropping boost", temp);
                    Restore("thermal");
                }
            }
        }).detach();
    }
};

PerfGovernor &PerfGovernor::Instance() {
    static PerfGovernor *instance = new PerfGovernor();
    return *instance;
}

PerfGovernor::Impl *PerfGovernor::impl() {
    static Impl *impl = new Impl();
    return impl;
}

void PerfGovernor::Init() {
    const char *candidates[] = {kBaselineFile, kBaselineFallback};
    for (const char *path : candidates) {
        std::FILE *file = std::fopen(path, "r");
        if (!file) continue;
        int restored = 0;
        char node[256];
        char value[64];
        while (std::fscanf(file, "%255s %63s", node, value) == 2)
            if (WriteSysfs(node, value)) ++restored;
        std::fclose(file);
        std::remove(path);
        ESP_LOGW(TAG,
                 "recovered stale clock baseline from %s (%d cores restored)",
                 path, restored);
    }
}

PerfGovernor::Token PerfGovernor::Acquire(const char *reason) {
    if (BoostDisabled()) return Token();
    Impl *state = impl();
    std::lock_guard<std::mutex> lk(state->mtx);
    ++state->count;
    state->deadline = std::chrono::steady_clock::now() +
                      std::chrono::milliseconds(kBoostTimeoutMs);
    if (!state->boosted) {
        const long temp = MaxTempMilli();
        if (temp > kBoostMaxTemp) {
            ESP_LOGW(TAG, "skip boost for %s: SoC at %ld mC",
                     reason ? reason : "?", temp);
        } else {
            ESP_LOGI(TAG, "boost requested by %s", reason ? reason : "?");
            state->Apply();
        }
    }
    state->StartWatchdog();
    state->cv.notify_all();
    return Token(reinterpret_cast<void *>(1),
                 [](void *) { PerfGovernor::Instance().Release(); });
}

void PerfGovernor::Release() {
    Impl *state = impl();
    std::lock_guard<std::mutex> lk(state->mtx);
    if (state->count > 0) --state->count;
    if (state->count == 0) state->Restore("last token released");
}

} // namespace jetson
