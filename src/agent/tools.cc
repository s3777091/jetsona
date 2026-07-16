#include "tools.h"
#include "settings.h"
#include "esp_log.h"

#include <nlohmann/json.hpp>
#include <curl/curl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#define TAG "Tools"

namespace jetson {

namespace {
using json = nlohmann::json;

std::string StrEnv(const char *name, const std::string &fallback) {
    const char *v = std::getenv(name);
    if (v && v[0]) return std::string(v);
    return fallback;
}

std::string Jstring(const json &j, const char *key, const std::string &def = "") {
    if (!j.is_object() || !j.contains(key)) return def;
    const json &v = j[key];
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_integer()) return std::to_string(v.get<int>());
    if (v.is_number_float()) return std::to_string(v.get<double>());
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    return def;
}

std::string UrlEncode(const std::string &s) {
    std::string out;
    out.reserve(s.size() * 3);
    static const char *hex = "0123456789ABCDEF";
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
            || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back((char)c);
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

size_t CurlWriteCb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *out = static_cast<std::string *>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}
} // namespace

// ---- ToolRegistry --------------------------------------------------------

void ToolRegistry::Register(std::unique_ptr<Tool> tool) {
    std::lock_guard<std::mutex> lk(mtx_);
    tools_.push_back(std::move(tool));
}

Tool *ToolRegistry::Find(const std::string &name) const {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto &t : tools_) if (t->name() == name) return t.get();
    return nullptr;
}

std::vector<ToolDef> ToolRegistry::Definitions() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<ToolDef> out;
    out.reserve(tools_.size());
    for (auto &t : tools_) out.push_back({t->name(), t->description(), t->parameters_json()});
    return out;
}

// ---- TaskStore -----------------------------------------------------------

TaskStore &TaskStore::Instance() { static TaskStore inst; return inst; }

TaskStore::TaskStore() {
    const char *home = std::getenv("HOME");
    std::string dir = home ? std::string(home) + "/.jetson-fw" : "/tmp/jetson-fw";
    path_ = dir + "/tasks.json";
    Load();
}

void TaskStore::Load() {
    std::ifstream in(path_);
    if (in) {
        std::stringstream ss; ss << in.rdbuf();
        data_json_ = ss.str();
    }
    if (data_json_.empty()) data_json_ = "{\"tasks\":[],\"notes\":[],\"next_id\":1}";
    try {
        auto j = json::parse(data_json_);
        next_id_ = j.value("next_id", 1);
        if (next_id_ < 1) next_id_ = 1;
    } catch (...) {
        data_json_ = "{\"tasks\":[],\"notes\":[],\"next_id\":1}";
        next_id_ = 1;
    }
}

void TaskStore::Save() {
    // Ensure dir exists (best-effort).
    std::string dir = path_.substr(0, path_.rfind('/'));
    std::string mkdir = "mkdir -p '" + dir + "' 2>/dev/null";
    int _ = std::system(mkdir.c_str()); (void)_;
    std::ofstream out(path_, std::ios::trunc);
    if (out) out << data_json_;
}

std::string TaskStore::CreateTask(const std::string &title, const std::string &due) {
    std::lock_guard<std::mutex> lk(mtx_);
    json j;
    try { j = json::parse(data_json_); } catch (...) { j = json::object(); }
    if (!j.contains("tasks") || !j["tasks"].is_array()) j["tasks"] = json::array();
    json t;
    int id = next_id_++;
    t["id"] = id;
    t["title"] = title;
    t["due"] = due;
    t["done"] = false;
    j["tasks"].push_back(t);
    j["next_id"] = next_id_;
    data_json_ = j.dump();
    Save();
    return "Da tao cong viec #" + std::to_string(id) + ": " + title +
           (due.empty() ? "" : " (han: " + due + ")");
}

std::string TaskStore::ListTasks() const {
    std::lock_guard<std::mutex> lk(mtx_);
    json j;
    try { j = json::parse(data_json_); } catch (...) { j = json::object(); }
    auto &arr = j["tasks"];
    if (!arr.is_array() || arr.empty()) return "Chua co cong viec nao.";
    std::string out = "Danh sach cong viec:\n";
    for (auto &t : arr) {
        bool done = t.value("done", false);
        out += std::to_string(t.value("id", 0)) + ". [" + (done ? "x" : " ") + "] " +
               t.value("title", std::string()) +
               (t.value("due", std::string()).empty() ? "" : " (han: " + t.value("due", std::string()) + ")") +
               "\n";
    }
    return out;
}

std::string TaskStore::CompleteTask(int id) {
    std::lock_guard<std::mutex> lk(mtx_);
    json j;
    try { j = json::parse(data_json_); } catch (...) { j = json::object(); }
    if (!j["tasks"].is_array()) return "ERROR: khong co cong viec";
    for (auto &t : j["tasks"]) {
        if (t.value("id", 0) == id) {
            t["done"] = true;
            data_json_ = j.dump();
            Save();
            return "Da danh dau hoan thanh cong viec #" + std::to_string(id);
        }
    }
    return "ERROR: khong tim thay cong viec #" + std::to_string(id);
}

std::string TaskStore::DeleteTask(int id) {
    std::lock_guard<std::mutex> lk(mtx_);
    json j;
    try { j = json::parse(data_json_); } catch (...) { j = json::object(); }
    if (!j["tasks"].is_array()) return "ERROR: khong co cong viec";
    json keep = json::array();
    bool found = false;
    for (auto &t : j["tasks"]) {
        if (t.value("id", 0) == id) { found = true; continue; }
        keep.push_back(t);
    }
    if (!found) return "ERROR: khong tim thay cong viec #" + std::to_string(id);
    j["tasks"] = keep;
    data_json_ = j.dump();
    Save();
    return "Da xoa cong viec #" + std::to_string(id);
}

std::string TaskStore::AddNote(const std::string &text) {
    std::lock_guard<std::mutex> lk(mtx_);
    json j;
    try { j = json::parse(data_json_); } catch (...) { j = json::object(); }
    if (!j.contains("notes") || !j["notes"].is_array()) j["notes"] = json::array();
    json n;
    int id = next_id_++;
    n["id"] = id;
    n["text"] = text;
    j["notes"].push_back(n);
    j["next_id"] = next_id_;
    data_json_ = j.dump();
    Save();
    return "Da ghi chu #" + std::to_string(id);
}

std::string TaskStore::ListNotes() const {
    std::lock_guard<std::mutex> lk(mtx_);
    json j;
    try { j = json::parse(data_json_); } catch (...) { j = json::object(); }
    auto &arr = j["notes"];
    if (!arr.is_array() || arr.empty()) return "Chua co ghi chu nao.";
    std::string out = "Danh sach ghi chu:\n";
    for (auto &n : arr) {
        out += "#" + std::to_string(n.value("id", 0)) + ": " + n.value("text", std::string()) + "\n";
    }
    return out;
}

// ---- TaskTool / NoteTool -------------------------------------------------

TaskTool::TaskTool(Op op)
    : Tool(
        op == Create  ? "create_task" :
        op == List    ? "list_tasks" :
        op == Complete? "complete_task" : "delete_task",
        op == Create  ? "Tao cong viec moi (can phan loai/luu viec can lam)." :
        op == List    ? "Liet ke tat ca cong viec dang co." :
        op == Complete? "Danh dau hoan thanh mot cong viec theo id." :
                        "Xoa mot cong viec theo id.",
        op == Create  ? R"({"type":"object","properties":{"title":{"type":"string","description":"Tieu de cong viec"},"due":{"type":"string","description":"Han chot, bo trong neu khong co"}},"required":["title"]})" :
        op == List    ? R"({"type":"object","properties":{}})" :
        op == Complete? R"({"type":"object","properties":{"id":{"type":"integer","description":"id cong viec"}},"required":["id"]})" :
                        R"({"type":"object","properties":{"id":{"type":"integer","description":"id cong viec"}},"required":["id"]})"),
      op_(op) {}

std::string TaskTool::Execute(const std::string &args_json) {
    json a;
    try { a = json::parse(args_json.empty() ? "{}" : args_json); } catch (...) { a = json::object(); }
    switch (op_) {
    case Create:   return TaskStore::Instance().CreateTask(Jstring(a, "title"), Jstring(a, "due"));
    case List:     return TaskStore::Instance().ListTasks();
    case Complete: return TaskStore::Instance().CompleteTask(a.value("id", 0));
    case Delete:   return TaskStore::Instance().DeleteTask(a.value("id", 0));
    }
    return "ERROR: op khong ro";
}

NoteTool::NoteTool(Op op)
    : Tool(
        op == Add ? "add_note" : "list_notes",
        op == Add ? "Ghi lai mot ghi chu/note ngan." : "Liet ke tat ca ghi chu da luu.",
        op == Add ? R"({"type":"object","properties":{"text":{"type":"string","description":"Noi dung ghi chu"}},"required":["text"]})"
                  : R"({"type":"object","properties":{}})"),
      op_(op) {}

std::string NoteTool::Execute(const std::string &args_json) {
    json a;
    try { a = json::parse(args_json.empty() ? "{}" : args_json); } catch (...) { a = json::object(); }
    switch (op_) {
    case Add:  return TaskStore::Instance().AddNote(Jstring(a, "text"));
    case List: return TaskStore::Instance().ListNotes();
    }
    return "ERROR: op khong ro";
}

// ---- WebSearchTool -------------------------------------------------------

WebSearchTool::WebSearchTool()
    : Tool("web_search",
           "Tim kiem tren web de lay thong tin moi (tin tuc, thoi tiet, kien thuc). Tra ve tong hop ket qua.",
           R"({"type":"object","properties":{"query":{"type":"string","description":"Tu khoa tim kiem"}},"required":["query"]})") {}

std::string WebSearchTool::Execute(const std::string &args_json) {
    json a;
    try { a = json::parse(args_json.empty() ? "{}" : args_json); } catch (...) { a = json::object(); }
    std::string query = Jstring(a, "query");
    if (query.empty()) return "ERROR: thieu query";

    std::string base = StrEnv("LIGHTPANDA_SEARCH_URL", "");
    if (base.empty()) return "web search chua cau hinh (LIGHTPANDA_SEARCH_URL).";
    std::string token = StrEnv("LIGHTPANDA_SEARCH_TOKEN", "");

    std::string url = base;
    url += (url.find('?') == std::string::npos ? "?" : "&");
    url += "q=" + UrlEncode(query);

    CURL *curl = curl_easy_init();
    if (!curl) return "ERROR: curl init";
    std::string resp;
    struct curl_slist *headers = nullptr;
    if (!token.empty()) {
        std::string auth = "Authorization: Bearer " + token;
        headers = curl_slist_append(headers, auth.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 40L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) return std::string("ERROR: ") + curl_easy_strerror(rc);
    if (http_code < 200 || http_code >= 300) return "ERROR: HTTP " + std::to_string(http_code) + " " + resp.substr(0, 160);

    // Expect JSON: {"results":[{"title","url","snippet"}]}
    try {
        auto j = json::parse(resp);
        auto &arr = j["results"];
        if (arr.is_array() && !arr.empty()) {
            std::string out = "Ket qua tim kiem cho \"" + query + "\":\n";
            int n = 0;
            for (auto &r : arr) {
                if (++n > 5) break;
                out += std::to_string(n) + ". " + r.value("title", std::string()) +
                       "\n   " + r.value("snippet", std::string()) +
                       "\n   " + r.value("url", std::string()) + "\n";
            }
            return out;
        }
        // Maybe a direct text field.
        if (j.contains("text")) return j["text"].get<std::string>();
    } catch (...) {
        // not JSON — return raw text truncated
    }
    if (resp.empty()) return "ERROR: khong co ket qua";
    return resp.substr(0, 1200);
}

// ---- WebOpenTool ---------------------------------------------------------

WebOpenTool::WebOpenTool()
    : Tool("web_open",
           "Mo mot URL (thuong tu ket qua web_search) va doc noi dung trang duoi dang text. "
           "Dung khi can chi tiet thay vi chi snippet.",
           R"({"type":"object","properties":{"url":{"type":"string","description":"URL http(s) can doc"}},"required":["url"]})") {}

std::string WebOpenTool::Execute(const std::string &args_json) {
    json a;
    try { a = json::parse(args_json.empty() ? "{}" : args_json); } catch (...) { a = json::object(); }
    std::string page_url = Jstring(a, "url");
    if (page_url.rfind("http://", 0) != 0 && page_url.rfind("https://", 0) != 0)
        return "ERROR: url phai bat dau bang http(s)://";

    // Same gateway as web_search; /fetch lives next to /search.
    std::string fetch_url = StrEnv("LIGHTPANDA_FETCH_URL", "");
    if (fetch_url.empty()) {
        std::string base = StrEnv("LIGHTPANDA_SEARCH_URL", "");
        if (base.empty()) return "web_open chua cau hinh (LIGHTPANDA_SEARCH_URL).";
        const std::string suffix = "/search";
        if (base.size() >= suffix.size() &&
            base.compare(base.size() - suffix.size(), suffix.size(), suffix) == 0)
            base.erase(base.size() - suffix.size());
        while (!base.empty() && base.back() == '/') base.pop_back();
        fetch_url = base + "/fetch";
    }
    std::string token = StrEnv("LIGHTPANDA_SEARCH_TOKEN", "");

    std::string url = fetch_url;
    url += (url.find('?') == std::string::npos ? "?" : "&");
    url += "url=" + UrlEncode(page_url);

    CURL *curl = curl_easy_init();
    if (!curl) return "ERROR: curl init";
    std::string resp;
    struct curl_slist *headers = nullptr;
    if (!token.empty()) {
        std::string auth = "Authorization: Bearer " + token;
        headers = curl_slist_append(headers, auth.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    // The gateway may drive a browser render + fallback fetch: allow longer
    // than web_search before giving up.
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 90L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) return std::string("ERROR: ") + curl_easy_strerror(rc);
    if (http_code < 200 || http_code >= 300)
        return "ERROR: HTTP " + std::to_string(http_code) + " " + resp.substr(0, 160);

    // Expect JSON: {"url","source","title","text"}
    try {
        auto j = json::parse(resp);
        std::string title = j.value("title", std::string());
        std::string text = j.value("text", std::string());
        if (text.empty()) return "ERROR: trang khong co noi dung doc duoc";
        // Keep the tool reply bounded; the LLM context is small.
        if (text.size() > 4000) text = text.substr(0, 4000) + "\n[...cat bot...]";
        std::string out;
        if (!title.empty()) out += "Tieu de: " + title + "\n\n";
        out += text;
        return out;
    } catch (...) {}
    if (resp.empty()) return "ERROR: khong co noi dung";
    return resp.substr(0, 4000);
}

// ---- Default registry ----------------------------------------------------

std::shared_ptr<ToolRegistry> BuildDefaultToolRegistry() {
    auto reg = std::make_shared<ToolRegistry>();
    reg->Register(std::make_unique<TaskTool>(TaskTool::Create));
    reg->Register(std::make_unique<TaskTool>(TaskTool::List));
    reg->Register(std::make_unique<TaskTool>(TaskTool::Complete));
    reg->Register(std::make_unique<TaskTool>(TaskTool::Delete));
    reg->Register(std::make_unique<NoteTool>(NoteTool::Add));
    reg->Register(std::make_unique<NoteTool>(NoteTool::List));
    reg->Register(std::make_unique<WebSearchTool>());
    reg->Register(std::make_unique<WebOpenTool>());
    return reg;
}

} // namespace jetson