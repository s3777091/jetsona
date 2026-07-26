#include "agent/assistant_tools.h"

#include "audio/voice_loop.h"
#include "esp_log.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#define TAG "AssistantTools"

namespace jetson {

namespace {

using json = nlohmann::json;

json ParseArgs(const std::string &text) {
    if (text.empty()) return json::object();
    auto parsed = json::parse(text, nullptr, false);
    return parsed.is_object() ? parsed : json::object();
}

std::string Str(const json &args, const char *key) {
    if (!args.contains(key)) return "";
    const auto &value = args[key];
    return value.is_string() ? value.get<std::string>() : "";
}

std::string Lower(std::string text) {
    for (char &c : text)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}

std::string StateDir() {
    const char *dir = std::getenv("JETSON_STATE_DIR");
    return (dir && *dir) ? dir : "/var/lib/jetson-fw";
}

// ---- Timer ---------------------------------------------------------------

struct TimerState {
    std::mutex mutex;
    std::atomic<uint64_t> generation{0};
    bool armed = false;
    std::string label;
    std::chrono::steady_clock::time_point due{};
};

TimerState &Timers() {
    static TimerState state;
    return state;
}

std::string DescribeRemaining(int seconds) {
    if (seconds <= 0) return "sắp xong";
    const int minutes = seconds / 60;
    const int rest = seconds % 60;
    char buffer[96];
    if (minutes > 0 && rest > 0)
        std::snprintf(buffer, sizeof(buffer), "%d phút %d giây", minutes, rest);
    else if (minutes > 0)
        std::snprintf(buffer, sizeof(buffer), "%d phút", minutes);
    else
        std::snprintf(buffer, sizeof(buffer), "%d giây", rest);
    return buffer;
}

// ---- Memory --------------------------------------------------------------

std::string MemoryPath() { return StateDir() + "/memory.json"; }

std::mutex &MemoryMutex() {
    static std::mutex mutex;
    return mutex;
}

json LoadMemory() {
    std::ifstream file(MemoryPath());
    if (!file) return json::array();
    auto parsed = json::parse(file, nullptr, false);
    return parsed.is_array() ? parsed : json::array();
}

bool SaveMemory(const json &items) {
    std::ofstream file(MemoryPath(), std::ios::trunc);
    if (!file) return false;
    file << items.dump(2);
    return static_cast<bool>(file);
}

} // namespace

// ---- TimerTool -----------------------------------------------------------

TimerTool::TimerTool()
    : Tool("timer",
           "Hen gio dem nguoc, khac voi bao thuc theo gio dong ho. Dung khi "
           "nguoi dung noi 'hen gio 10 phut', 'nhac toi sau 5 phut nua', "
           "'canh 30 giay'. action='set' can 'minutes' hoac 'seconds'; "
           "'cancel' de huy; 'status' de xem con bao lau. Khi het gio Nova tu "
           "noi ra, nguoi dung khong phai hoi lai.",
           R"({"type":"object","properties":{
                "action":{"type":"string","enum":["set","cancel","status"],
                          "description":"Hanh dong voi hen gio."},
                "minutes":{"type":"integer","description":"So phut dem nguoc."},
                "seconds":{"type":"integer","description":"So giay dem nguoc."},
                "label":{"type":"string","description":"Viec can nhac, vi du 'luoc trung'."}
              },"required":["action"]})") {}

std::string TimerTool::Execute(const std::string &arguments_json) {
    const json args = ParseArgs(arguments_json);
    const std::string action = Lower(Str(args, "action"));
    auto &state = Timers();

    if (action == "cancel") {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (!state.armed) return "Khong co hen gio nao dang chay.";
        state.armed = false;
        state.generation.fetch_add(1);   // invalidates the sleeping thread
        return "Da huy hen gio.";
    }

    if (action == "status") {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (!state.armed) return "Khong co hen gio nao dang chay.";
        const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
            state.due - std::chrono::steady_clock::now()).count();
        return "Con " + DescribeRemaining(static_cast<int>(remaining)) +
               (state.label.empty() ? "." : " (" + state.label + ").");
    }

    if (action != "set") return "ERROR: action phai la set, cancel hoac status.";

    int total = args.value("minutes", 0) * 60 + args.value("seconds", 0);
    if (total <= 0) return "ERROR: can minutes hoac seconds lon hon 0.";
    if (total > 12 * 3600) return "ERROR: hen gio toi da 12 tieng.";

    const std::string label = Str(args, "label");
    uint64_t generation;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.armed = true;
        state.label = label;
        state.due = std::chrono::steady_clock::now() +
                    std::chrono::seconds(total);
        generation = state.generation.fetch_add(1) + 1;
    }

    /* Detached rather than a scheduler: one timer at a time is what people
     * actually ask for out loud, and the generation counter makes a cancelled
     * or replaced timer wake up, notice it is stale, and exit quietly. */
    std::thread([total, label, generation] {
        std::this_thread::sleep_for(std::chrono::seconds(total));
        auto &state = Timers();
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (!state.armed || state.generation.load() != generation) return;
            state.armed = false;
        }
        const std::string spoken =
            label.empty() ? "Hết giờ rồi." : "Hết giờ " + label + " rồi.";
        ESP_LOGI(TAG, "timer fired: %s", spoken.c_str());
        jetson::audio::VoiceLoop::Instance().Speak(spoken);
    }).detach();

    return "Da hen gio " + DescribeRemaining(total) +
           (label.empty() ? "." : " cho " + label + ".");
}

// ---- MemoryTool ----------------------------------------------------------

MemoryTool::MemoryTool(Op op)
    : Tool(op == Remember ? "remember"
                          : (op == Recall ? "recall" : "forget"),
           op == Remember
               ? "Ghi nho lau dai mot thong tin ve nguoi dung de dung cho cac "
                 "lan sau: ten nguoi than, noi lam viec, so thich, thoi quen. "
                 "Dung khi nguoi dung noi 'nho giup minh...', 'tu gio biet la...'. "
                 "Khac add_note: note la danh sach de doc lai, con day la dieu "
                 "Nova phai nho de tra loi dung ve sau."
           : (op == Recall
                  ? "Doc lai nhung dieu da ghi nho ve nguoi dung. Dung truoc khi "
                    "hoi lai mot thong tin ma nguoi dung co the da noi roi."
                  : "Xoa mot dieu da ghi nho, theo id lay tu recall."),
           op == Remember
               ? R"({"type":"object","properties":{"text":{"type":"string","description":"Dieu can nho, viet ngan gon o ngoi thu ba."}},"required":["text"]})"
           : (op == Recall
                  ? R"({"type":"object","properties":{"query":{"type":"string","description":"Tu khoa loc, bo trong de lay tat ca."}}})"
                  : R"({"type":"object","properties":{"id":{"type":"integer","description":"id lay tu recall."}},"required":["id"]})")),
      op_(op) {}

std::string MemoryTool::Execute(const std::string &arguments_json) {
    const json args = ParseArgs(arguments_json);
    std::lock_guard<std::mutex> lock(MemoryMutex());
    json items = LoadMemory();

    if (op_ == Remember) {
        const std::string text = Str(args, "text");
        if (text.empty()) return "ERROR: thieu noi dung can nho.";
        int next_id = 1;
        for (const auto &item : items)
            next_id = std::max(next_id, item.value("id", 0) + 1);
        items.push_back({{"id", next_id}, {"text", text}});
        if (!SaveMemory(items)) return "ERROR: khong ghi duoc bo nho.";
        ESP_LOGI(TAG, "remembered #%d: %s", next_id, text.c_str());
        return "Da nho.";
    }

    if (op_ == Forget) {
        const int id = args.value("id", 0);
        json kept = json::array();
        bool removed = false;
        for (const auto &item : items) {
            if (item.value("id", 0) == id) { removed = true; continue; }
            kept.push_back(item);
        }
        if (!removed) return "Khong tim thay dieu can xoa.";
        if (!SaveMemory(kept)) return "ERROR: khong ghi duoc bo nho.";
        return "Da quen.";
    }

    const std::string query = Lower(Str(args, "query"));
    std::string out;
    for (const auto &item : items) {
        const std::string text = item.value("text", "");
        if (!query.empty() && Lower(text).find(query) == std::string::npos)
            continue;
        out += "#" + std::to_string(item.value("id", 0)) + " " + text + "\n";
    }
    return out.empty() ? "Chua nho dieu gi ve nguoi dung." : out;
}

std::string MemoryTool::Summary() {
    std::lock_guard<std::mutex> lock(MemoryMutex());
    const json items = LoadMemory();
    if (items.empty()) return "";
    std::string out;
    for (const auto &item : items) out += "- " + item.value("text", "") + "\n";
    return out;
}

} // namespace jetson
