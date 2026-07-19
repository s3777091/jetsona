#include "system_tools.h"

#include "device_bridge.h"

#include "board.h"
#include "settings.h"
#include "esp_log.h"
#include "media/player_controller.h"
#include "net/wifi_manager.h"
#include "net/zing_music_client.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <sstream>
#include <string>
#include <vector>

#define TAG "SystemTools"

namespace jetson {

namespace {
using json = nlohmann::json;

json ParseArgs(const std::string &s) {
    // Models occasionally send "" or malformed JSON for a no-argument tool.
    if (s.empty()) return json::object();
    try {
        json j = json::parse(s);
        return j.is_object() ? j : json::object();
    } catch (...) {
        return json::object();
    }
}

std::string Str(const json &j, const char *key, const std::string &def = "") {
    if (!j.contains(key)) return def;
    const json &v = j[key];
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_integer()) return std::to_string(v.get<int>());
    return def;
}

int Int(const json &j, const char *key, int def) {
    if (!j.contains(key)) return def;
    const json &v = j[key];
    if (v.is_number()) return (int)v.get<double>();
    if (v.is_string()) { try { return std::stoi(v.get<std::string>()); } catch (...) {} }
    return def;
}

bool Bool(const json &j, const char *key, bool def) {
    if (!j.contains(key)) return def;
    const json &v = j[key];
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number()) return v.get<double>() != 0.0;
    if (v.is_string()) {
        std::string s = v.get<std::string>();
        for (auto &c : s) c = (char)std::tolower((unsigned char)c);
        return s == "true" || s == "1" || s == "yes" || s == "on";
    }
    return def;
}

std::string Lower(std::string s) {
    for (auto &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string Trim(const std::string &s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

// ---- date/time helpers ---------------------------------------------------

std::tm LocalNow() {
    std::time_t now = std::time(nullptr);
    std::tm out{};
#if defined(_WIN32)
    localtime_s(&out, &now);
#else
    localtime_r(&now, &out);
#endif
    return out;
}

std::string Today() {
    std::tm t = LocalNow();
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    return buf;
}

/* Accepts the ISO date the schema asks for, plus the two words the model keeps
 * sending anyway. Returns "" when the value is not a usable date. */
std::string NormalizeDate(const std::string &raw) {
    const std::string v = Lower(Trim(raw));
    if (v.empty() || v == "today" || v == "hom nay" || v == "hôm nay") return Today();
    if (v == "tomorrow" || v == "ngay mai" || v == "ngày mai") {
        std::time_t now = std::time(nullptr) + 24 * 60 * 60;
        std::tm t{};
#if defined(_WIN32)
        localtime_s(&t, &now);
#else
        localtime_r(&now, &t);
#endif
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
        return buf;
    }
    if (v.size() != 10 || v[4] != '-' || v[7] != '-') return "";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i == 4 || i == 7) continue;
        if (!std::isdigit((unsigned char)v[i])) return "";
    }
    return v;
}

/* CalendarView::IsValidTime accepts empty (all-day) or exactly HH:MM. */
std::string NormalizeTime(const std::string &raw) {
    const std::string v = Trim(raw);
    if (v.empty()) return "";
    if (v.size() != 5 || v[2] != ':') return "";
    for (int i = 0; i < 5; ++i) {
        if (i == 2) continue;
        if (!std::isdigit((unsigned char)v[i])) return "";
    }
    return v;
}

// ---- calendar store (mirrors CalendarView's format) ----------------------

std::string CalendarKey(const std::string &date) { return "d_" + date; }

void CalendarIndexAdd(const std::string &date) {
    Settings s("calendar", true);
    std::string v = s.GetString("task_dates", "");
    std::istringstream iss(v);
    std::string token;
    while (std::getline(iss, token, ',')) if (token == date) return;
    if (!v.empty()) v += ",";
    v += date;
    s.SetString("task_dates", v);
}

// ---- reminders store (mirrors RemindersView's format) --------------------

std::string HexEncode(const std::string &value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 2);
    for (unsigned char c : value) {
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 0x0f]);
    }
    return out;
}

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string HexDecode(const std::string &value) {
    if (value.size() % 2 != 0) return {};
    std::string out;
    out.reserve(value.size() / 2);
    for (size_t i = 0; i < value.size(); i += 2) {
        const int hi = HexDigit(value[i]);
        const int lo = HexDigit(value[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back((char)((hi << 4) | lo));
    }
    return out;
}

struct Reminder {
    int id = 0;
    bool pinned = false;
    bool done = false;
    uint32_t color = 0x4dd0e1;
    std::string title;
    std::string info;
};

std::vector<Reminder> LoadReminders() {
    std::vector<Reminder> out;
    const std::string saved = Settings("reminders", false).GetString("items_v1", "");
    std::istringstream records(saved);
    std::string record;
    while (std::getline(records, record, '~')) {
        if (record.empty()) continue;
        std::string f[6];
        std::istringstream parts(record);
        bool valid = true;
        for (auto &field : f) if (!std::getline(parts, field, '|')) { valid = false; break; }
        if (!valid) continue;
        try {
            Reminder r;
            r.id = std::stoi(f[0]);
            r.pinned = f[1] == "1";
            r.done = f[2] == "1";
            r.color = (uint32_t)std::stoul(f[3], nullptr, 16);
            r.title = HexDecode(f[4]);
            r.info = HexDecode(f[5]);
            if (r.id <= 0 || r.title.empty()) continue;
            out.push_back(std::move(r));
        } catch (...) {
            // Skip a malformed record, keep the rest — same as RemindersView.
        }
    }
    return out;
}

void SaveReminders(const std::vector<Reminder> &list) {
    std::ostringstream out;
    for (size_t i = 0; i < list.size(); ++i) {
        const auto &r = list[i];
        if (i != 0) out << '~';
        char color[8];
        std::snprintf(color, sizeof(color), "%06X", r.color & 0xffffff);
        out << r.id << '|' << (r.pinned ? 1 : 0) << '|' << (r.done ? 1 : 0) << '|'
            << color << '|' << HexEncode(r.title) << '|' << HexEncode(r.info);
    }
    Settings("reminders", true).SetString("items_v1", out.str());
}

std::string CreatedNow() {
    std::tm t = LocalNow();
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d/%02d/%04d %02d:%02d",
                  t.tm_mday, t.tm_mon + 1, t.tm_year + 1900, t.tm_hour, t.tm_min);
    return buf;
}

} // namespace

// ---- device_status -------------------------------------------------------

DeviceStatusTool::DeviceStatusTool()
    : Tool("device_status",
           "Doc trang thai thiet bi ngay luc nay: ngay gio hien tai, pin va "
           "sac, Wi-Fi dang ket noi, am luong, bai nhac dang phat. Goi tool nay "
           "truoc khi tra loi bat cu cau hoi nao ve ngay/gio, pin, mang hay nhac.",
           R"({"type":"object","properties":{}})") {}

std::string DeviceStatusTool::Execute(const std::string &) {
    std::ostringstream out;

    std::tm t = LocalNow();
    char when[64];
    std::snprintf(when, sizeof(when), "%04d-%02d-%02d %02d:%02d",
                  t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
    out << "thoi_gian: " << when << "\n";

    int level = 0;
    bool charging = false, discharging = false;
    if (Board::GetInstance().GetBatteryLevel(level, charging, discharging)) {
        out << "pin: " << level << "%"
            << (charging ? " (dang sac)" : discharging ? " (dang dung pin)" : "") << "\n";
    } else {
        out << "pin: khong doc duoc (khong co module UPS)\n";
    }

    auto &wifi = WifiManager::Instance();
    if (!wifi.Available()) {
        out << "wifi: khong co adapter\n";
    } else if (!wifi.IsEnabled()) {
        out << "wifi: dang tat\n";
    } else {
        const std::string ssid = wifi.ActiveSsid();
        if (ssid.empty()) out << "wifi: bat nhung chua ket noi\n";
        else out << "wifi: da ket noi '" << ssid << "' (song " << wifi.ActiveSignal() << "%)\n";
    }

    Settings disp("display", false);
    out << "am_luong: " << disp.GetInt("volume", 50) << "%"
        << (disp.GetBool("muted", false) ? " (dang tat tieng)" : "") << "\n";

    const auto snap = music::PlayerController::Instance().Snapshot();
    if (snap.has_current) {
        const char *state = "khong ro";
        switch (snap.status) {
            case music::PlaybackStatus::Playing:   state = "dang phat"; break;
            case music::PlaybackStatus::Paused:    state = "dang tam dung"; break;
            case music::PlaybackStatus::Buffering: state = "dang tai"; break;
            case music::PlaybackStatus::Resolving: state = "dang tim nguon"; break;
            case music::PlaybackStatus::Ended:     state = "da het"; break;
            case music::PlaybackStatus::Error:     state = "loi"; break;
            case music::PlaybackStatus::Idle:      state = "chua phat"; break;
        }
        out << "nhac: " << state << " - " << snap.current.title
            << " (" << snap.current.artist << ")\n";
    } else {
        out << "nhac: chua co bai nao trong hang doi\n";
    }
    return out.str();
}

// ---- open_app ------------------------------------------------------------

namespace {
std::string OpenAppSchema() {
    // Enumerate the ids so the model picks from the real catalogue instead of
    // inventing "spotify". The label goes in the description for the same reason.
    json props;
    json app;
    app["type"] = "string";
    json ids = json::array();
    std::string desc = "Ung dung can mo. ";
    for (const auto &a : DeviceBridge::Apps()) {
        ids.push_back(a.id);
        desc += std::string(a.id) + "=" + a.label + ", ";
    }
    if (desc.size() > 2) desc.erase(desc.size() - 2);
    app["enum"] = ids;
    app["description"] = desc;
    props["app"] = app;

    json schema;
    schema["type"] = "object";
    schema["properties"] = props;
    schema["required"] = json::array({"app"});
    return schema.dump();
}
} // namespace

OpenAppTool::OpenAppTool()
    : Tool("open_app",
           "Mo mot ung dung tren man hinh thiet bi.",
           OpenAppSchema()) {}

std::string OpenAppTool::Execute(const std::string &arguments_json) {
    const json args = ParseArgs(arguments_json);
    const std::string name = Str(args, "app", Str(args, "name"));
    if (name.empty()) return "ERROR: thieu tham so 'app'";

    std::string label, err;
    if (!DeviceBridge::Instance().OpenApp(name, label, err)) return "ERROR: " + err;
    return "Da mo ung dung " + label + ".";
}

// ---- set_volume ----------------------------------------------------------

VolumeTool::VolumeTool()
    : Tool("set_volume",
           "Chinh am luong dau ra cua thiet bi (0-100) hoac bat/tat tieng. "
           "De doc am luong hien tai thi dung device_status.",
           R"({"type":"object","properties":{
                "level":{"type":"integer","minimum":0,"maximum":100,
                         "description":"Muc am luong 0-100. Bo trong neu chi muon bat/tat tieng."},
                "mute":{"type":"boolean","description":"true = tat tieng, false = bat tieng."}
              }})") {}

std::string VolumeTool::Execute(const std::string &arguments_json) {
    const json args = ParseArgs(arguments_json);

    Settings disp("display", false);
    const int current = disp.GetInt("volume", 50);
    const bool current_muted = disp.GetBool("muted", false);

    const bool has_level = args.contains("level") && !args["level"].is_null();
    const bool has_mute = args.contains("mute") && !args["mute"].is_null();
    if (!has_level && !has_mute) return "ERROR: can it nhat mot trong 'level' hoac 'mute'";

    int level = has_level ? std::max(0, std::min(100, Int(args, "level", current))) : current;
    // Asking for a level implies you want to hear it.
    bool muted = has_mute ? Bool(args, "mute", current_muted) : (has_level ? false : current_muted);

    if (!DeviceBridge::Instance().SetVolume(level, muted))
        return "ERROR: giao dien chua san sang";

    if (muted) return "Da tat tieng.";
    return "Da dat am luong " + std::to_string(level) + "%.";
}

// ---- set_brightness ------------------------------------------------------

BrightnessTool::BrightnessTool()
    : Tool("set_brightness",
           "Chinh do sang man hinh (10-100).",
           R"({"type":"object","properties":{
                "percent":{"type":"integer","minimum":10,"maximum":100,
                           "description":"Do sang 10-100."}
              },"required":["percent"]})") {}

std::string BrightnessTool::Execute(const std::string &arguments_json) {
    const json args = ParseArgs(arguments_json);
    if (!args.contains("percent")) return "ERROR: thieu tham so 'percent'";
    const int pct = std::max(10, std::min(100, Int(args, "percent", 100)));
    if (!DeviceBridge::Instance().SetBrightness(pct)) return "ERROR: giao dien chua san sang";
    return "Da dat do sang " + std::to_string(pct) + "%.";
}

// ---- wifi ----------------------------------------------------------------

WifiTool::WifiTool()
    : Tool("wifi_control",
           "Kiem tra va dieu khien Wi-Fi. 'status' xem trang thai, 'scan' liet ke "
           "mang xung quanh, 'on'/'off' bat tat song, 'connect' ket noi lai mot "
           "mang DA LUU truoc do (khong nhap duoc mat khau moi tu day - neu mang "
           "chua luu, hay bao nguoi dung mo ung dung Cai dat Wi-Fi), "
           "'disconnect' ngat ket noi.",
           R"({"type":"object","properties":{
                "action":{"type":"string","enum":["status","scan","on","off","connect","disconnect"],
                          "description":"Hanh dong can thuc hien."},
                "ssid":{"type":"string","description":"Ten mang, chi dung voi action=connect."}
              },"required":["action"]})") {}

std::string WifiTool::Execute(const std::string &arguments_json) {
    const json args = ParseArgs(arguments_json);
    const std::string action = Lower(Str(args, "action", "status"));
    auto &wifi = WifiManager::Instance();

    if (!wifi.Available()) return "ERROR: thiet bi khong co adapter Wi-Fi";

    if (action == "status") {
        if (!wifi.IsEnabled()) return "Wi-Fi dang tat.";
        const std::string ssid = wifi.ActiveSsid();
        if (ssid.empty()) return "Wi-Fi dang bat nhung chua ket noi mang nao.";
        return "Dang ket noi '" + ssid + "', song " + std::to_string(wifi.ActiveSignal()) + "%.";
    }

    if (action == "on" || action == "off") {
        const bool on = (action == "on");
        if (!wifi.Enable(on)) return "ERROR: " + wifi.LastError();
        return on ? "Da bat Wi-Fi." : "Da tat Wi-Fi.";
    }

    if (action == "scan") {
        if (!wifi.IsEnabled()) return "ERROR: Wi-Fi dang tat, bat len truoc da";
        auto nets = wifi.Scan();
        if (nets.empty()) return "Khong thay mang nao.";
        std::ostringstream out;
        out << "Cac mang tim thay (ten | song | da luu):\n";
        size_t shown = 0;
        for (const auto &n : nets) {
            if (n.ssid.empty()) continue;
            out << "- " << n.ssid << " | " << n.signal << "% | "
                << (n.known ? "da luu" : "chua luu")
                << (n.in_use ? " | DANG DUNG" : "") << "\n";
            if (++shown >= 12) break;   // keep the tool result inside a sane token budget
        }
        return out.str();
    }

    if (action == "disconnect") {
        if (!wifi.Disconnect()) return "ERROR: " + wifi.LastError();
        return "Da ngat ket noi Wi-Fi.";
    }

    if (action == "connect") {
        const std::string ssid = Trim(Str(args, "ssid"));
        if (ssid.empty()) return "ERROR: thieu 'ssid'";
        // Only reconnect to a profile NetworkManager already stores. Handing a
        // password through the model is not something this tool does.
        bool known = false;
        for (const auto &n : wifi.Scan()) {
            if (n.ssid == ssid) { known = n.known; break; }
        }
        if (!known)
            return "ERROR: mang '" + ssid + "' chua duoc luu tren may. Nguoi dung can "
                   "mo ung dung Cai dat Wi-Fi de nhap mat khau lan dau.";
        if (!wifi.Connect(ssid, "")) return "ERROR: " + wifi.LastError();
        return "Da ket noi '" + ssid + "'.";
    }

    return "ERROR: action khong hop le: " + action;
}

// ---- music ---------------------------------------------------------------

MusicTool::MusicTool()
    : Tool("music_control",
           "Dieu khien bai dang phat: play/pause/toggle/next/previous/stop, "
           "hoac 'status' de xem bai hien tai. De BAT DAU mot bai moi theo ten "
           "thi dung music_play, khong dung tool nay.",
           R"({"type":"object","properties":{
                "action":{"type":"string",
                          "enum":["play","pause","toggle","next","previous","stop","status"],
                          "description":"Hanh dong dieu khien phat nhac."}
              },"required":["action"]})") {}

std::string MusicTool::Execute(const std::string &arguments_json) {
    const json args = ParseArgs(arguments_json);
    const std::string action = Lower(Str(args, "action", "status"));
    auto &player = music::PlayerController::Instance();
    const auto snap = player.Snapshot();

    if (action == "status") {
        if (!snap.has_current) return "Chua co bai nao trong hang doi.";
        return std::string(snap.status == music::PlaybackStatus::Playing ? "Dang phat: " : "Da dung o: ") +
               snap.current.title + " - " + snap.current.artist;
    }

    if (!snap.has_current && action != "stop")
        return "ERROR: chua co bai nao trong hang doi. Dung music_play voi ten "
               "bai de bat dau phat.";

    if (action == "play")          player.Resume();
    else if (action == "pause")    player.Pause();
    else if (action == "toggle")   player.Toggle();
    else if (action == "next")     player.Next();
    else if (action == "previous") player.Previous();
    else if (action == "stop")     player.Stop();
    else return "ERROR: action khong hop le: " + action;

    if (action == "stop") return "Da dung nhac.";
    if (action == "pause") return "Da tam dung nhac.";
    // Next/previous resolve asynchronously, so the snapshot we have is the old
    // track — report the command rather than guessing at the new title.
    if (action == "next") return "Da chuyen sang bai tiep theo.";
    if (action == "previous") return "Da quay lai bai truoc.";
    return "Da tiep tuc phat: " + snap.current.title + " - " + snap.current.artist;
}

// ---- music_play ----------------------------------------------------------

MusicPlayTool::MusicPlayTool()
    : Tool("music_play",
           "Tim mot bai hat tren Zing MP3 theo ten (hoac 'ten bai - ca si') va "
           "phat ngay bai khop nhat. Cac ket qua con lai duoc xep vao hang doi "
           "phia sau, nen music_control voi action=next se chuyen sang ket qua "
           "ke tiep. Dung tool nay bat cu khi nao nguoi dung muon nghe mot bai "
           "cu the.",
           R"({"type":"object","properties":{
                "query":{"type":"string",
                         "description":"Ten bai hat, co the kem ten ca si."}
              },"required":["query"]})") {}

std::string MusicPlayTool::Execute(const std::string &arguments_json) {
    const json args = ParseArgs(arguments_json);
    const std::string query = Trim(Str(args, "query", Str(args, "name")));
    if (query.empty()) return "ERROR: thieu 'query'";

    ZingMusicClient client;
    std::vector<music::Track> found;
    std::string err;
    // Ask for more than we keep: premium hits are dropped below and would
    // otherwise leave the queue nearly empty.
    if (!client.SearchSongs(query, 10, found, err) || found.empty())
        return "ERROR: " + (err.empty() ? "khong tim thay bai nao" : err);

    /* Premium tracks have no playable stream without a Zing account, and
     * MusicView refuses to queue them for the same reason. Dropping them here
     * keeps the agent from announcing a song that will never make a sound. */
    std::vector<music::Track> results;
    int skipped_premium = 0;
    for (auto &track : found) {
        if (track.premium) { ++skipped_premium; continue; }
        results.push_back(std::move(track));
        if (results.size() >= 5) break;
    }
    if (results.empty())
        return "ERROR: chi tim thay ban premium cua '" + query +
               "', can tai khoan Zing MP3 Premium moi phat duoc. Thu ten bai khac.";

    /* Fetch the winning cover here rather than letting the now-playing island
     * render an empty frame: one extra request, and the artwork is on disk
     * before PlayQueue makes the track current. */
    if (!results.front().artwork_url.empty()) {
        std::string path, art_err;
        if (client.DownloadArtwork(results.front().artwork_url, path, art_err))
            results.front().artwork_path = path;
        else
            ESP_LOGW(TAG, "artwork download failed: %s", art_err.c_str());
    }

    const music::Track chosen = results.front();
    music::PlayerController::Instance().PlayQueue(results, 0);

    std::ostringstream out;
    out << "Dang phat: " << chosen.title;
    if (!chosen.artist.empty()) out << " - " << chosen.artist;
    if (skipped_premium > 0)
        out << "\n(" << skipped_premium << " ban premium da bi bo qua)";
    if (results.size() > 1) {
        out << "\nKet qua khac trong hang doi:";
        for (size_t i = 1; i < results.size(); ++i)
            out << "\n- " << results[i].title
                << (results[i].artist.empty() ? "" : " - " + results[i].artist);
    }
    return out.str();
}

// ---- calendar ------------------------------------------------------------

CalendarTool::CalendarTool(Op op)
    : Tool(op == Add ? "calendar_add" : "calendar_list",
           op == Add
               ? "Them mot su kien vao Lich cua thiet bi. Dung tool nay khi nguoi "
                 "dung muon dat lich hoac len ke hoach; goi nhieu lan de tao nhieu "
                 "muc cho mot ke hoach. Ngay theo dinh dang YYYY-MM-DD (lay ngay "
                 "hom nay tu device_status)."
               : "Liet ke cac su kien da luu trong Lich cua mot ngay.",
           op == Add
               ? R"({"type":"object","properties":{
                      "date":{"type":"string","description":"Ngay YYYY-MM-DD."},
                      "time":{"type":"string","description":"Gio HH:MM, bo trong neu ca ngay."},
                      "title":{"type":"string","description":"Noi dung su kien."}
                    },"required":["date","title"]})"
               : R"({"type":"object","properties":{
                      "date":{"type":"string","description":"Ngay YYYY-MM-DD. Bo trong = hom nay."}
                    }})"),
      op_(op) {}

std::string CalendarTool::Execute(const std::string &arguments_json) {
    const json args = ParseArgs(arguments_json);
    const std::string date = NormalizeDate(Str(args, "date"));
    if (date.empty()) return "ERROR: 'date' phai theo dinh dang YYYY-MM-DD";

    Settings read("calendar", false);
    const std::string existing = read.GetString(CalendarKey(date), "");

    if (op_ == List) {
        if (existing.empty()) return "Ngay " + date + " khong co su kien nao.";
        std::ostringstream out;
        out << "Su kien ngay " << date << ":\n";
        std::istringstream iss(existing);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.empty()) continue;
            const size_t p1 = line.find('|');
            if (p1 == std::string::npos) continue;
            const size_t p2 = line.find('|', p1 + 1);
            const std::string time = line.substr(0, p1);
            const bool done = line.substr(p1 + 1, 1) == "1";
            const std::string title = (p2 == std::string::npos) ? "" : line.substr(p2 + 1);
            out << "- " << (time.empty() ? "ca ngay" : time) << " " << title
                << (done ? " [xong]" : "") << "\n";
        }
        return out.str();
    }

    const std::string title = Trim(Str(args, "title"));
    if (title.empty()) return "ERROR: thieu 'title'";
    // The record is newline-delimited and '|'-separated, so a title carrying
    // either would corrupt the neighbouring rows when CalendarView parses them.
    std::string safe_title = title;
    std::replace(safe_title.begin(), safe_title.end(), '\n', ' ');
    std::replace(safe_title.begin(), safe_title.end(), '\r', ' ');

    const std::string raw_time = Str(args, "time");
    const std::string time = NormalizeTime(raw_time);
    if (!Trim(raw_time).empty() && time.empty())
        return "ERROR: 'time' phai theo dinh dang HH:MM";

    std::string updated = existing;
    if (!updated.empty() && updated.back() != '\n') updated += "\n";
    updated += time + "|0|" + safe_title;

    Settings("calendar", true).SetString(CalendarKey(date), updated);
    CalendarIndexAdd(date);
    DeviceBridge::Instance().ReloadCalendar();

    return "Da them vao Lich ngay " + date + (time.empty() ? "" : " luc " + time) +
           ": " + safe_title;
}

// ---- reminders -----------------------------------------------------------

ReminderTool::ReminderTool(Op op)
    : Tool(op == Add ? "reminder_add" : op == List ? "reminder_list" : "reminder_complete",
           op == Add
               ? "Them mot muc vao ung dung Nhac nho cua thiet bi (viec can lam, "
                 "khong gan vao ngay cu the). Neu viec co ngay gio ro rang thi "
                 "dung calendar_add thay vi tool nay."
               : op == List
                     ? "Liet ke cac muc trong ung dung Nhac nho."
                     : "Danh dau mot muc trong Nhac nho la da xong, theo id lay tu reminder_list.",
           op == Add
               ? R"({"type":"object","properties":{
                      "title":{"type":"string","description":"Noi dung can nho."}
                    },"required":["title"]})"
               : op == List
                     ? R"({"type":"object","properties":{}})"
                     : R"({"type":"object","properties":{
                            "id":{"type":"integer","description":"id cua muc, lay tu reminder_list."}
                          },"required":["id"]})"),
      op_(op) {}

std::string ReminderTool::Execute(const std::string &arguments_json) {
    const json args = ParseArgs(arguments_json);
    auto list = LoadReminders();

    if (op_ == List) {
        if (list.empty()) return "Chua co nhac nho nao.";
        std::ostringstream out;
        out << "Nhac nho:\n";
        for (const auto &r : list)
            out << "- id=" << r.id << " " << r.title << (r.done ? " [xong]" : "") << "\n";
        return out.str();
    }

    if (op_ == Complete) {
        const int id = Int(args, "id", 0);
        for (auto &r : list) {
            if (r.id != id) continue;
            r.done = true;
            SaveReminders(list);
            DeviceBridge::Instance().ReloadReminders();
            return "Da danh dau xong: " + r.title;
        }
        return "ERROR: khong co nhac nho id=" + std::to_string(id);
    }

    std::string title = Trim(Str(args, "title"));
    if (title.empty()) return "ERROR: thieu 'title'";
    // '|' and '~' are the record separators; hex encoding covers them, but a
    // newline would still break the on-screen row layout.
    std::replace(title.begin(), title.end(), '\n', ' ');
    std::replace(title.begin(), title.end(), '\r', ' ');

    int next_id = 1;
    for (const auto &r : list) next_id = std::max(next_id, r.id + 1);

    Reminder add;
    add.id = next_id;
    add.color = 0x4dd0e1;   // one of RemindersView's safe palette entries
    add.title = title;
    add.info = CreatedNow();
    list.push_back(std::move(add));

    SaveReminders(list);
    DeviceBridge::Instance().ReloadReminders();
    return "Da them nhac nho: " + title;
}

} // namespace jetson
