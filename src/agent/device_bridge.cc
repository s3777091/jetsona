#include "device_bridge.h"

#include "application.h"
#include "esp_log.h"

#include <algorithm>
#include <cctype>
#include <sstream>

#define TAG "DeviceBridge"

namespace jetson {

namespace {

std::string Lower(std::string s) {
    for (auto &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

/* Aliases carry no diacritics on purpose: the model writes "lich" as often as
 * "lịch", and stripping accents from user input reliably is more code than
 * listing both spellings that actually occur. */
const std::vector<AgentApp> kApps = {
    {"calendar",       "Lịch",              "lich calendar schedule ngay thang"},
    {"reminders",      "Nhắc nhở",          "nhacnho nhac-nho reminder todo viec cangiam"},
    {"music",          "Nhạc",              "nhac music nhacso player bainhac"},
    {"documents",      "Tài liệu",          "tailieu documents files finder tep thumuc"},
    {"settings",       "Cài đặt",           "caidat settings preferences tuychinh"},
    {"terminal",       "Terminal",          "terminal console shell dongolenh"},
    {"trash",          "Thùng rác",         "thungrac trash rac bin"},
    {"chat",           "Ekko",              "ekko chat assistant troly"},
    {"gallery",        "Hình nền",          "hinhnen wallpaper gallery anh photos"},
    {"wifi",           "Cài đặt Wi-Fi",     "wifi mang network wireless"},
    {"bluetooth",      "Cài đặt Bluetooth", "bluetooth bt tainghe"},
    {"lock_screen",    "Màn hình khoá",     "khoa lock lockscreen manhinhkhoa"},
};

} // namespace

DeviceBridge &DeviceBridge::Instance() {
    static DeviceBridge inst;
    return inst;
}

const std::vector<AgentApp> &DeviceBridge::Apps() { return kApps; }

std::string DeviceBridge::ResolveAppId(const std::string &name) {
    const std::string want = Lower(name);
    if (want.empty()) return "";

    for (const auto &app : kApps) if (want == Lower(app.id)) return app.id;
    for (const auto &app : kApps) if (want == Lower(app.label)) return app.id;

    // Alias match: whole token only, so "web" hits browser but "webhook" does not.
    for (const auto &app : kApps) {
        std::istringstream tokens(app.aliases);
        std::string token;
        while (tokens >> token) if (token == want) return app.id;
    }
    // Last resort: the model often sends "mo ung dung nhac" as the whole name.
    for (const auto &app : kApps) {
        std::istringstream tokens(app.aliases);
        std::string token;
        while (tokens >> token) {
            if (token.size() >= 4 && want.find(token) != std::string::npos) return app.id;
        }
    }
    return "";
}

void DeviceBridge::SetAppOpener(AppOpener fn) {
    std::lock_guard<std::mutex> lk(mtx_);
    app_opener_ = std::move(fn);
}
void DeviceBridge::SetNotifier(Notifier fn) {
    std::lock_guard<std::mutex> lk(mtx_);
    notifier_ = std::move(fn);
}
void DeviceBridge::SetVolumeSetter(VolumeSetter fn) {
    std::lock_guard<std::mutex> lk(mtx_);
    volume_setter_ = std::move(fn);
}
void DeviceBridge::SetBrightnessSetter(BrightnessSetter fn) {
    std::lock_guard<std::mutex> lk(mtx_);
    brightness_setter_ = std::move(fn);
}
void DeviceBridge::SetReminderReloader(StoreReloader fn) {
    std::lock_guard<std::mutex> lk(mtx_);
    reminder_reloader_ = std::move(fn);
}
void DeviceBridge::SetCalendarReloader(StoreReloader fn) {
    std::lock_guard<std::mutex> lk(mtx_);
    calendar_reloader_ = std::move(fn);
}

/* Each request copies its handler out under the lock and releases it before
 * scheduling: the handler runs on the main loop, which must never be waiting
 * on a mutex the agent holds. */

bool DeviceBridge::OpenApp(const std::string &name, std::string &out_label,
                           std::string &out_err) {
    const std::string id = ResolveAppId(name);
    if (id.empty()) {
        out_err = "khong co ung dung nao ten '" + name + "'";
        return false;
    }
    AppOpener fn;
    { std::lock_guard<std::mutex> lk(mtx_); fn = app_opener_; }
    if (!fn) {
        out_err = "giao dien chua san sang";
        return false;
    }
    for (const auto &app : kApps) if (id == app.id) { out_label = app.label; break; }

    Application::GetInstance().Schedule([fn, id]() { fn(id); });
    ESP_LOGI(TAG, "agent open app: %s", id.c_str());
    return true;
}

bool DeviceBridge::Notify(const std::string &text) {
    if (text.empty()) return false;
    Notifier fn;
    { std::lock_guard<std::mutex> lk(mtx_); fn = notifier_; }
    if (!fn) return false;
    Application::GetInstance().Schedule([fn, text]() { fn(text); });
    return true;
}

bool DeviceBridge::SetVolume(int volume, bool muted) {
    VolumeSetter fn;
    { std::lock_guard<std::mutex> lk(mtx_); fn = volume_setter_; }
    if (!fn) return false;
    volume = std::max(0, std::min(100, volume));
    Application::GetInstance().Schedule([fn, volume, muted]() { fn(volume, muted); });
    return true;
}

void DeviceBridge::ReloadReminders() {
    StoreReloader fn;
    { std::lock_guard<std::mutex> lk(mtx_); fn = reminder_reloader_; }
    if (!fn) return;
    Application::GetInstance().Schedule([fn]() { fn(); });
}

void DeviceBridge::ReloadCalendar() {
    StoreReloader fn;
    { std::lock_guard<std::mutex> lk(mtx_); fn = calendar_reloader_; }
    if (!fn) return;
    Application::GetInstance().Schedule([fn]() { fn(); });
}

bool DeviceBridge::SetBrightness(int percent) {
    BrightnessSetter fn;
    { std::lock_guard<std::mutex> lk(mtx_); fn = brightness_setter_; }
    if (!fn) return false;
    percent = std::max(10, std::min(100, percent));
    Application::GetInstance().Schedule([fn, percent]() { fn(percent); });
    return true;
}

} // namespace jetson
