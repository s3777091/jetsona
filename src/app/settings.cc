#include "settings.h"
#include "esp_log.h"

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>

#define TAG "settings"

static std::string kvFilePath() {
    const char *env = std::getenv("JETSON_SETTINGS_FILE");
    if (env && env[0]) return env;
    const char *home = std::getenv("HOME");
    if (!home || !home[0]) {
        struct passwd *pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : "/root";
    }
    std::string dir = std::string(home) + "/.jetson-fw";
    ::mkdir(dir.c_str(), 0775);
    return dir + "/settings.kv";
}

namespace {
class KvStore {
public:
    KvStore() { path_ = kvFilePath(); load(); }

    std::string get(const std::string &ns, const std::string &key,
                    const std::string &def) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = map_.find(ns + "." + key);
        return it == map_.end() ? def : it->second;
    }
    void set(const std::string &ns, const std::string &key, const std::string &val) {
        std::lock_guard<std::mutex> lk(mtx_);
        map_[ns + "." + key] = val;
        save();
    }
    void eraseKey(const std::string &ns, const std::string &key) {
        std::lock_guard<std::mutex> lk(mtx_);
        map_.erase(ns + "." + key);
        save();
    }
    void eraseAll(const std::string &ns) {
        std::lock_guard<std::mutex> lk(mtx_);
        std::string prefix = ns + ".";
        for (auto it = map_.begin(); it != map_.end();) {
            if (it->first.compare(0, prefix.size(), prefix) == 0) it = map_.erase(it);
            else ++it;
        }
        save();
    }

private:
    void load() {
        std::ifstream in(path_);
        if (!in.is_open()) return;
        std::string ns, key, val;
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream iss(line);
            if (std::getline(iss, ns, '\t') && std::getline(iss, key, '\t') &&
                std::getline(iss, val)) {
                map_[ns + "." + key] = val;
            }
        }
    }
    void save() {
        std::ofstream out(path_, std::ios::trunc);
        if (!out.is_open()) { ESP_LOGE(TAG, "failed to write %s", path_.c_str()); return; }
        for (auto &kv : map_) {
            const std::string &k = kv.first;
            auto dot = k.find('.');
            std::string ns = k.substr(0, dot);
            std::string key = k.substr(dot + 1);
            out << ns << '\t' << key << '\t' << kv.second << '\n';
        }
    }

    std::string path_;
    std::map<std::string, std::string> map_;
    std::mutex mtx_;
};
KvStore &store() {
    static KvStore s;
    return s;
}
} // namespace

Settings::Settings(const std::string &ns, bool read_write)
    : ns_(ns), read_write_(read_write) {}

Settings::~Settings() {
    /* KV is persisted immediately on each set, so nothing to commit. */
}

std::string Settings::GetString(const std::string &key, const std::string &default_value) {
    return store().get(ns_, key, default_value);
}
void Settings::SetString(const std::string &key, const std::string &value) {
    if (!read_write_) return;
    store().set(ns_, key, value);
    dirty_ = true;
}
int32_t Settings::GetInt(const std::string &key, int32_t default_value) {
    std::string v = store().get(ns_, key, "");
    if (v.empty()) return default_value;
    return (int32_t)std::strtol(v.c_str(), nullptr, 10);
}
void Settings::SetInt(const std::string &key, int32_t value) {
    if (!read_write_) return;
    store().set(ns_, key, std::to_string(value));
    dirty_ = true;
}
bool Settings::GetBool(const std::string &key, bool default_value) {
    std::string v = store().get(ns_, key, "");
    if (v.empty()) return default_value;
    return v == "1" || v == "true";
}
void Settings::SetBool(const std::string &key, bool value) {
    if (!read_write_) return;
    store().set(ns_, key, value ? "1" : "0");
    dirty_ = true;
}
void Settings::EraseKey(const std::string &key) {
    if (!read_write_) return;
    store().eraseKey(ns_, key);
}
void Settings::EraseAll() {
    if (!read_write_) return;
    store().eraseAll(ns_);
}