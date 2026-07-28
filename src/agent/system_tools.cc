#include "system_tools.h"

#include "device_bridge.h"

#include "board.h"
#include "settings.h"
#include "esp_log.h"
#include "media/player_controller.h"
#include "media/user_library.h"
#include "net/weather_client.h"
#include "net/zing_music_client.h"
#include "platform/shell_command.h"

#include <nlohmann/json.hpp>

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <fstream>
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
           "sac, am luong, bai nhac dang phat. Goi tool nay truoc khi tra loi "
           "bat cu cau hoi nao ve ngay/gio, pin hay nhac.",
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
    // Ask for more than we keep: premium hits are handled separately below and
    // would otherwise leave the queue nearly empty.
    if (!client.SearchSongs(query, 10, found, err) || found.empty())
        return "ERROR: " + (err.empty() ? "khong tim thay bai nao" : err);

    /* VIP flow: if the best match is premium (VIP), it has no playable stream
     * without a Zing account, so we tell the user that and play the nearest
     * non-premium result instead -- "nhac do la nhac VIP, mo bai khac". Only
     * when *every* hit is premium do we give up and suggest names. */
    const music::Track &best = found.front();
    if (best.premium) {
        std::vector<music::Track> alts;
        for (auto &t : found) {
            if (!t.premium) alts.push_back(std::move(t));
            if (alts.size() >= 5) break;
        }
        std::ostringstream out;
        out << "Bai \"" << best.title << "\""
            << (best.artist.empty() ? "" : " - " + best.artist)
            << " la ban VIP (Premium) nen khong phat duoc.";
        if (alts.empty()) {
            out << " Hay thu ten bai khac.";
            return out.str();
        }
        // Play the first alternative; keep the rest in the queue behind it.
        if (!alts.front().artwork_url.empty()) {
            std::string path, art_err;
            if (client.DownloadArtwork(alts.front().artwork_url, path, art_err))
                alts.front().artwork_path = path;
        }
        const music::Track chosen = alts.front();
        music::PlayerController::Instance().PlayQueue(alts, 0);
        out << " Minh mo bai khac: " << chosen.title
            << (chosen.artist.empty() ? "" : " - " + chosen.artist) << ".";
        if (alts.size() > 1) {
            out << " Con trong hang doi:";
            for (size_t i = 1; i < alts.size(); ++i)
                out << "\n- " << alts[i].title
                    << (alts[i].artist.empty() ? "" : " - " + alts[i].artist);
        }
        return out.str();
    }

    /* Non-VIP: play the best match, with the remaining non-premium hits queued
     * behind it so music_control action=next walks the alternatives. */
    std::vector<music::Track> results;
    for (auto &track : found) {
        if (track.premium) continue;
        results.push_back(std::move(track));
        if (results.size() >= 5) break;
    }

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

// ---- pc_power (WoL) ------------------------------------------------------

PcPowerTool::PcPowerTool()
    : Tool("pc_power",
           "Bat PC o nha bang Wake-on-LAN. Goi tool nay khi nguoi dung muon bat "
           "may tinh. PC se khoi dong trong vai giay sau khi lenh duoc gui.",
           R"({"type":"object","properties":{}})") {}

std::string PcPowerTool::Execute(const std::string & /*arguments_json*/) {
    const char *url = std::getenv("JETSON_WOL_URL");
    if (!url || !*url)
        return "ERROR: chua cau hinh JETSON_WOL_URL (diem cuoi /wake cua Jetson "
               "hoac /api/wol/wake cua jetsona-ui)";
    const char *token = std::getenv("JETSON_WOL_TOKEN");

    CURL *curl = curl_easy_init();
    if (!curl) return "ERROR: khoi tao curl that bai";

    struct curl_slist *hdr = nullptr;
    hdr = curl_slist_append(hdr, "Content-Type: application/json");
    if (token && *token) {
        std::string t = std::string("X-WoL-Token: ") + token;
        hdr = curl_slist_append(hdr, t.c_str());
    }

    std::string resp;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "{}");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                     +[](char *p, size_t s, size_t n, void *u) {
                         static_cast<std::string *>(u)->append(p, s * n);
                         return s * n;
                     });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hdr);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK)
        return "ERROR: khong ket noi duoc WoL: " + std::string(curl_easy_strerror(rc));
    if (code >= 200 && code < 300)
        return "Da gui lenh bat PC. PC se khoi dong trong vai giay.";
    return "ERROR: WoL tra ma " + std::to_string(code) +
           (resp.empty() ? "" : ": " + resp);
}

// ---- air_conditioner -----------------------------------------------------

namespace {

/* JETSON_AC_URL is the bridge's base ("http://127.0.0.1:46003"), not a single
 * endpoint like JETSON_WOL_URL: this tool talks to several routes. */
std::string AcBaseUrl() {
    const char *url = std::getenv("JETSON_AC_URL");
    std::string base = (url && *url) ? url : "";
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base;
}

/* One request to the AC bridge; empty `body` means GET. On success fills
 * out_json; on failure fills out_err with a ready-to-return "ERROR: ..."
 * string. The bridge answers JSON even for failures, and writes its "error"
 * text to be spoken, so it is passed through rather than reworded here. */
bool AcRequest(const std::string &path, const std::string &body,
               json &out_json, std::string &out_err) {
    const std::string base = AcBaseUrl();
    if (base.empty()) {
        out_err = "ERROR: chua cau hinh JETSON_AC_URL (vi du http://127.0.0.1:46003)";
        return false;
    }

    CURL *curl = curl_easy_init();
    if (!curl) { out_err = "ERROR: khoi tao curl that bai"; return false; }

    struct curl_slist *hdr = nullptr;
    hdr = curl_slist_append(hdr, "Content-Type: application/json");
    const char *token = std::getenv("JETSON_AC_TOKEN");
    if (token && *token) {
        std::string t = std::string("X-AC-Token: ") + token;
        hdr = curl_slist_append(hdr, t.c_str());
    }

    const std::string url = base + path;
    std::string resp;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    if (!body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdr);
    /* Generous next to the 5s WoL timeout: every call round-trips to LG's
     * cloud, and /comfort does a read, up to three writes, and a read back. */
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                     +[](char *p, size_t s, size_t n, void *u) {
                         static_cast<std::string *>(u)->append(p, s * n);
                         return s * n;
                     });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hdr);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        out_err = "ERROR: khong goi duoc dich vu dieu hoa: " +
                  std::string(curl_easy_strerror(rc)) +
                  " (kiem tra systemctl status jetsona-ac)";
        return false;
    }

    json parsed = json::parse(resp, nullptr, false);
    if (parsed.is_discarded()) parsed = json::object();

    if (code < 200 || code >= 300) {
        std::string message = parsed.value("error", std::string());
        out_err = "ERROR: " + (message.empty()
                                   ? "dich vu dieu hoa tra ma " + std::to_string(code)
                                   : message);
        return false;
    }
    out_json = std::move(parsed);
    return true;
}

/* Pull the spoken sentence out of a bridge reply. Every route sets "summary";
 * falling back to the raw body only matters if the two drift apart. */
std::string AcSummary(const json &j) {
    std::string summary = j.value("summary", std::string());
    return summary.empty() ? std::string("Da thuc hien.") : summary;
}

std::string Upper(std::string s) {
    for (auto &c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

/* The model does not reliably emit the enum spellings, and the transcript it
 * works from is itself lossy, so map the obvious synonyms instead of failing. */
std::string NormalizeJobMode(const std::string &raw) {
    const std::string v = Lower(Trim(raw));
    if (v == "cool" || v == "lanh" || v == "lam lanh" || v == "cooling") return "COOL";
    if (v == "air_dry" || v == "dry" || v == "hut am" || v == "am") return "AIR_DRY";
    if (v == "fan" || v == "quat" || v == "quat gio") return "FAN";
    return Upper(Trim(raw));
}

std::string NormalizeFanSpeed(const std::string &raw) {
    const std::string v = Lower(Trim(raw));
    if (v == "low" || v == "thap" || v == "nhe" || v == "yeu") return "LOW";
    if (v == "mid" || v == "medium" || v == "vua" || v == "trung binh") return "MID";
    if (v == "high" || v == "cao" || v == "manh") return "HIGH";
    if (v == "auto" || v == "tu dong") return "AUTO";
    return Upper(Trim(raw));
}

/* Accepts the number under any of the keys the model reaches for. */
bool ReadCelsius(const json &args, double &out) {
    for (const char *key : {"celsius", "temperature", "temp", "value"}) {
        if (!args.contains(key)) continue;
        const json &v = args[key];
        if (v.is_number()) { out = v.get<double>(); return true; }
        if (v.is_string()) {
            try { out = std::stod(v.get<std::string>()); return true; } catch (...) {}
        }
    }
    return false;
}

} // namespace

AirConditionerTool::AirConditionerTool()
    : Tool("air_conditioner",
           "Dieu khien dieu hoa trong phong. Dung action='comfort' kem "
           "feeling khi nguoi dung noi CAM GIAC thay vi con so -- 'lanh qua' "
           "-> feeling='cold', 'nong qua' -> 'hot', 'nom/am qua' -> 'humid', "
           "'bi qua, ngot ngat' -> 'stuffy'; them intensity='very' neu ho nhan "
           "manh (rat lanh, lanh cong), 'slight' neu hoi hoi. Comfort tu doc "
           "trang thai may roi chinh cho hop ly, KHONG can goi status truoc. "
           "Cac action khac: 'status' xem trang thai, 'on'/'off' bat tat, "
           "'set_temp' voi celsius 16-30 khi nguoi dung noi ro so, 'mode' "
           "(COOL/AIR_DRY/FAN), 'fan' (LOW/MID/HIGH/AUTO).",
           R"({"type":"object","properties":{"action":{"type":"string","enum":["comfort","status","on","off","set_temp","mode","fan"],"description":"Viec can lam"},"feeling":{"type":"string","enum":["cold","hot","humid","stuffy","ok"],"description":"Cam giac cua nguoi dung, bat buoc khi action='comfort'"},"intensity":{"type":"string","enum":["slight","normal","very"],"description":"Muc do cam giac, mac dinh 'normal'"},"celsius":{"type":"number","description":"Nhiet do muc tieu 16-30, dung voi action='set_temp'"},"mode":{"type":"string","enum":["COOL","AIR_DRY","FAN"],"description":"Che do, dung voi action='mode'"},"fan_speed":{"type":"string","enum":["LOW","MID","HIGH","AUTO"],"description":"Toc do quat, dung voi action='fan'"}},"required":["action"]})") {}

std::string AirConditionerTool::Execute(const std::string &arguments_json) {
    const json args = ParseArgs(arguments_json);
    std::string action = Lower(Trim(Str(args, "action")));

    /* A feeling with no action (or a made-up one) is still unambiguously a
     * comfort request; treating it as such beats bouncing an error back. */
    const bool has_feeling = !Trim(Str(args, "feeling")).empty();
    if (action.empty()) action = has_feeling ? "comfort" : "status";
    if (action == "feel" || action == "adjust" || action == "auto") action = "comfort";
    if (action == "temp" || action == "temperature" || action == "set_temperature")
        action = "set_temp";
    if (action == "turn_on" || action == "power_on") action = "on";
    if (action == "turn_off" || action == "power_off") action = "off";
    if (action == "fan_speed" || action == "wind") action = "fan";

    json reply;
    std::string err;

    if (action == "status") {
        if (!AcRequest("/status", "", reply, err)) return err;
        return AcSummary(reply);
    }

    if (action == "on" || action == "off") {
        json body;
        body["on"] = (action == "on");
        if (!AcRequest("/power", body.dump(), reply, err)) return err;
        return AcSummary(reply);
    }

    if (action == "comfort") {
        std::string feeling = Lower(Trim(Str(args, "feeling")));
        if (feeling.empty())
            return "ERROR: thieu 'feeling' (cold/hot/humid/stuffy/ok)";
        json body;
        body["feeling"] = feeling;
        const std::string intensity = Lower(Trim(Str(args, "intensity")));
        if (!intensity.empty()) body["intensity"] = intensity;
        if (!AcRequest("/comfort", body.dump(), reply, err)) return err;
        /* State after the change, so the model can answer a follow-up
         * ("giờ bao nhiêu độ?") without another call. Diacritics here, unlike
         * the ASCII error strings above: this text is glued onto the bridge's
         * own Vietnamese and ends up spoken. */
        std::string out = AcSummary(reply);
        const std::string state = reply.value("state_summary", std::string());
        if (!state.empty()) out += " Hiện tại: " + state + ".";
        return out;
    }

    if (action == "set_temp") {
        double celsius = 0.0;
        if (!ReadCelsius(args, celsius))
            return "ERROR: thieu 'celsius' (so tu 16 den 30)";
        json body;
        body["celsius"] = celsius;
        if (!AcRequest("/temperature", body.dump(), reply, err)) return err;
        return AcSummary(reply);
    }

    if (action == "mode") {
        const std::string mode = NormalizeJobMode(Str(args, "mode"));
        if (mode.empty()) return "ERROR: thieu 'mode' (COOL/AIR_DRY/FAN)";
        json body;
        body["mode"] = mode;
        if (!AcRequest("/mode", body.dump(), reply, err)) return err;
        return AcSummary(reply);
    }

    if (action == "fan") {
        std::string speed = NormalizeFanSpeed(Str(args, "fan_speed"));
        if (speed.empty()) speed = NormalizeFanSpeed(Str(args, "speed"));
        if (speed.empty()) return "ERROR: thieu 'fan_speed' (LOW/MID/HIGH/AUTO)";
        json body;
        body["speed"] = speed;
        if (!AcRequest("/fan", body.dump(), reply, err)) return err;
        return AcSummary(reply);
    }

    return "ERROR: action khong hop le: " + action +
           " (comfort/status/on/off/set_temp/mode/fan)";
}

// ---- weather -------------------------------------------------------------

WeatherTool::WeatherTool()
    : Tool("weather",
           "Lay thoi tiet hien tai tai vi tri cua thiet bi (Da Nang). Tra ve "
           "nhiet do, trang thai va do am. Dung khi nguoi dung hoi thoi tiet, "
           "hoac de bao thoi tiet buoi sang khi bao thuc.",
           R"({"type":"object","properties":{}})") {}

std::string WeatherTool::Execute(const std::string & /*arguments_json*/) {
    WeatherInfo w;
    std::string err;
    if (!WeatherClient::Fetch(w, err))
        return "ERROR: " + (err.empty() ? "khong lay duoc thoi tiet" : err);
    return WeatherClient::FormatLine(w);
}

// ---- ringtone ------------------------------------------------------------

namespace {
std::string RingtoneDir() { return std::string(JETSON_ASSETS_DIR) + "/ringtones"; }

bool HasAudioExt(const std::string &name) {
    auto pos = name.rfind('.');
    if (pos == std::string::npos) return false;
    std::string ext = name.substr(pos);
    for (auto &c : ext) c = (char)std::tolower((unsigned char)c);
    return ext == ".mp3" || ext == ".wav" || ext == ".ogg" || ext == ".m4a";
}

std::vector<std::string> ListRingtones() {
    std::vector<std::string> out;
    DIR *d = opendir(RingtoneDir().c_str());
    if (!d) return out;
    while (struct dirent *e = readdir(d)) {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        if (HasAudioExt(name)) out.push_back(name);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

bool RingtoneExists(const std::string &name) {
    const auto all = ListRingtones();
    return std::find(all.begin(), all.end(), name) != all.end();
}
} // namespace

RingtoneTool::RingtoneTool()
    : Tool("ringtone",
           "Chon nhac bao thuc: action 'list' de liet ke cac ringtone, 'set' de "
           "dat ringtone active (can 'name'), 'preview' de phat thu (co the kem "
           "'name', mac dinh la ringtone dang dung). Ringtone duoc sync tu S3.",
           R"({"type":"object","properties":{
                "action":{"type":"string","enum":["list","set","preview"],
                          "description":"Hanh dong can thuc hien."},
                "name":{"type":"string","description":"Ten file ringtone (tu list)."}
              },"required":["action"]})") {}

std::string RingtoneTool::Execute(const std::string &arguments_json) {
    const json args = ParseArgs(arguments_json);
    const std::string action = Lower(Str(args, "action", "list"));

    if (action == "list") {
        const auto tones = ListRingtones();
        if (tones.empty()) return "Chua co ringtone nao (chua sync tu S3).";
        const std::string active = Settings("alarm", false).GetString("ringtone", "");
        std::ostringstream out;
        out << "Cac ringtone:";
        for (const auto &t : tones)
            out << "\n- " << t << (t == active ? " [dang dung]" : "");
        return out.str();
    }

    if (action == "set") {
        std::string name = Trim(Str(args, "name"));
        if (name.empty()) return "ERROR: thieu 'name'. Goi ringtone voi action=list truoc.";
        if (!RingtoneExists(name))
            return "ERROR: khong co ringtone '" + name + "'. Goi action=list de xem danh sach.";
        Settings("alarm", true).SetString("ringtone", name);
        return "Da dat ringtone bao thuc: " + name;
    }

    if (action == "preview") {
        std::string name = Trim(Str(args, "name"));
        if (name.empty()) name = Settings("alarm", false).GetString("ringtone", "");
        if (name.empty() || !RingtoneExists(name))
            return "ERROR: chua chon ringtone, hoac ten khong ton tai. Goi action=list.";
        std::string path = RingtoneDir() + "/" + name;
        // Fire-and-forget so the tool returns immediately while mpv plays.
        const char *device = std::getenv("JETSON_MUSIC_DEVICE");
        if (!device || !*device) device = std::getenv("JETSON_VOICE_OUT");
        std::string cmd =
            "mpv --no-video --really-quiet --no-terminal --force-window=no";
        if (device && *device)
            cmd += " " + jetson::platform::QuoteShellArgument(
                               std::string("--audio-device=alsa/") + device);
        cmd += " -- " + jetson::platform::QuoteShellArgument(path) +
               " >/dev/null 2>&1 &";
        jetson::platform::RunShellCommand(cmd);
        return "Dang phat thu: " + name;
    }

    return "ERROR: action khong hop le: " + action;
}

// ---- music_album ---------------------------------------------------------

MusicAlbumTool::MusicAlbumTool()
    : Tool("music_album",
           "Quan ly album rieng cua nguoi dung (Album cua toi). action='save' de "
           "luu bai dang phat vao album (co the kem 'name' de chi ro album, mac "
           "dinh la 'Album cua toi'); 'list' de liet ke cac album va bai trong do; "
           "'play' de phat mot album (kem 'name', mac dinh la album dau tien). "
           "Dung sau music_play khi nguoi dung bao 'luu bai nay di' hoac 'choi "
           "nhac trong album'.",
           R"({"type":"object","properties":{
                "action":{"type":"string","enum":["save","list","play"],
                          "description":"Hanh dong voi album."},
                "name":{"type":"string",
                        "description":"Ten album (cho save/play). Bo trong = Album cua toi."}
              },"required":["action"]})") {}

namespace {
/* Find an album by display name (case-insensitive), or return nullptr. The
 * UserLibrary ids are opaque ints, but the user only ever names albums in
 * speech, so we match on name here. */
music::UserAlbum *FindAlbum(std::vector<music::UserAlbum> &albums,
                            const std::string &name) {
    std::string want = Lower(name);
    for (auto &a : albums)
        if (Lower(a.name) == want) return &a;
    return nullptr;
}
} // namespace

std::string MusicAlbumTool::Execute(const std::string &arguments_json) {
    const json args = ParseArgs(arguments_json);
    const std::string action = Lower(Str(args, "action"));
    auto &lib = music::UserLibrary::Instance();

    if (action == "save") {
        const auto snap = music::PlayerController::Instance().Snapshot();
        if (!snap.has_current)
            return "ERROR: chua co bai nao dang phat. Dung music_play truoc, "
                   "roi moi luu.";
        std::string name = Trim(Str(args, "name"));
        if (name.empty()) name = "Album của tôi";

        // Reuse an existing album of the same name, else create one.
        auto albums = lib.Albums();
        std::string album_id;
        if (music::UserAlbum *a = FindAlbum(albums, name))
            album_id = a->id;
        else
            album_id = lib.CreateAlbum(name);

        if (lib.Contains(album_id, snap.current.id))
            return "Bai \"" + snap.current.title + "\" da co trong album \"" +
                   name + "\" roi.";
        if (!lib.AddTrack(album_id, snap.current))
            return "ERROR: khong luu duoc (album khong con ton tai).";
        return "Da luu bai \"" + snap.current.title + "\" vao album \"" + name +
               "\".";
    }

    if (action == "list") {
        auto albums = lib.Albums();
        if (albums.empty()) return "Chua co album nao. Goi music_album action=save sau khi phat mot bai.";
        std::ostringstream out;
        out << "Cac album:";
        for (const auto &a : albums) {
            out << "\n- " << a.name << " (" << a.tracks.size() << " bai)";
            for (const auto &t : a.tracks)
                out << "\n   * " << t.title
                    << (t.artist.empty() ? "" : " - " + t.artist);
        }
        return out.str();
    }

    if (action == "play") {
        std::string name = Trim(Str(args, "name"));
        auto albums = lib.Albums();
        if (albums.empty())
            return "ERROR: chua co album nao de phat. Luu bai truoc voi music_album action=save.";
        music::UserAlbum *a = nullptr;
        if (name.empty())
            a = &albums.front();
        else {
            a = FindAlbum(albums, name);
            if (!a)
                return "ERROR: khong co album \"" + name + "\". Goi action=list de xem.";
        }
        if (a->tracks.empty())
            return "ERROR: album \"" + a->name + "\" chua co bai nao.";
        std::vector<music::Track> queue = a->tracks;
        music::PlayerController::Instance().PlayQueue(queue, 0);
        std::ostringstream out;
        out << "Dang phat album \"" << a->name << "\" (" << queue.size()
            << " bai), bat dau: " << queue.front().title
            << (queue.front().artist.empty() ? "" : " - " + queue.front().artist);
        return out.str();
    }

    return "ERROR: action khong hop le: " + action;
}

// ---- alarm ---------------------------------------------------------------

AlarmTool::AlarmTool()
    : Tool("alarm",
           "Bao thuc buoi sang. action='set' (can 'time' HH:MM) de dat gio bao "
           "thuc; 'cancel' de tat lich bao thuc; 'status' de xem; 'stop' de tat "
           "tieng chuong dang reo. Luc reo, Ekko phat ringtone, doc thoi tiet Da "
           "Nang va cac ghi chu 'mai bao toi'.",
           R"({"type":"object","properties":{
                "action":{"type":"string","enum":["set","cancel","status","stop"],
                          "description":"Hanh dong voi bao thuc."},
                "time":{"type":"string","description":"Gio bao thuc HH:MM (cho set)."}
              },"required":["action"]})") {}

std::string AlarmTool::Execute(const std::string &arguments_json) {
    const json args = ParseArgs(arguments_json);
    const std::string action = Lower(Str(args, "action"));

    if (action == "set") {
        std::string t = NormalizeTime(Str(args, "time"));
        if (t.empty())
            return "ERROR: 'time' phai theo dinh dang HH:MM (vi du 06:30).";
        Settings("alarm", true).SetString("time", t);
        Settings("alarm", true).SetString("enabled", "1");
        return "Da dat bao thuc luc " + t +
               ". Sang reo se phat ringtone va bao thoi tiet Da Nang.";
    }
    if (action == "cancel") {
        Settings("alarm", true).SetString("enabled", "0");
        return "Da tat lich bao thuc.";
    }
    if (action == "status") {
        Settings a("alarm", false);
        std::string t = a.GetString("time", "");
        std::string e = a.GetString("enabled", "0");
        if (t.empty()) return "Chua dat bao thuc.";
        std::string ring = a.GetString("ringtone", "");
        std::string out = "Bao thuc " + t + " (" + (e == "1" ? "bat" : "tat") + ")";
        if (!ring.empty()) out += ", ringtone: " + ring;
        return out + ".";
    }
    if (action == "stop") {
        // Kill the ringing alarm ringtone (pidfile written by the scheduler).
        const std::string pidfile = AlarmPidFile();
        std::ifstream pf(pidfile);
        std::string pid;
        if (pf) std::getline(pf, pid);
        if (!pid.empty()) {
            jetson::platform::RunShellCommand("kill " + pid + " 2>/dev/null");
            std::remove(pidfile.c_str());
        }
        return "Da tat bao thuc. Chuc ngay moi tot lanh.";
    }
    return "ERROR: action khong hop le: " + action;
}

} // namespace jetson
