#include "llm_client.h"
#include "settings.h"
#include "esp_log.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#define TAG "LlmClient"

namespace jetson {

namespace {
std::once_flag g_curl_init_once;

void EnsureCurlInit() {
    std::call_once(g_curl_init_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

size_t CurlWriteCb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *out = static_cast<std::string *>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string EnvOr(const char *name, const std::string &fallback) {
    const char *v = std::getenv(name);
    if (v && v[0]) return std::string(v);
    return fallback;
}
} // namespace

LlmClient::LlmClient() {
    EnsureCurlInit();
    ConfigureFromSettings();
}

LlmClient::~LlmClient() = default;

void LlmClient::ConfigureFromSettings() {
    Settings s("llm", false);
    base_url_      = EnvOr("OLLAMA_BASE_URL", s.GetString("base_url", "https://ollama.com/v1"));
    api_key_       = EnvOr("OLLAMA_API_KEY",  s.GetString("api_key", ""));
    model_         = EnvOr("OLLAMA_MODEL",   s.GetString("model", "qwen2.5:7b"));
    system_prompt_ = s.GetString("system_prompt",
        "Ban la tro ly AI tieng Viet, huu ich va thuc tinh. "
        "Khi nguoi dung muon ghi nho/cong viec/nhac nho, dung tool de luu va quan ly. "
        "Khi can thong tin moi, dung web_search; muon doc chi tiet mot trang "
        "ket qua, dung web_open voi URL do. Tra loi ngan gon, tieng Viet tu nhien.");
    temperature_ = (double)s.GetInt("temperature_x100", 70) / 100.0; // 70 -> 0.7
}

// ---- Shared HTTP POST ----------------------------------------------------

static ChatResult DoPost(const std::string &url, const std::string &api_key,
                         const std::string &payload, std::string &raw_out) {
    ChatResult r;
    CURL *curl = curl_easy_init();
    if (!curl) { r.error = "curl_easy_init failed"; return r; }

    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (!api_key.empty()) {
        std::string auth = "Authorization: Bearer " + api_key;
        headers = curl_slist_append(headers, auth.c_str());
    }

    std::string resp;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        r.error = std::string("HTTP loi: ") + curl_easy_strerror(rc);
        return r;
    }
    if (http_code < 200 || http_code >= 300) {
        r.error = "HTTP " + std::to_string(http_code) + ": " + resp.substr(0, 240);
        return r;
    }
    raw_out = resp;
    r.ok = true;
    return r;
}

// ---- Plain chat ----------------------------------------------------------

bool LlmClient::Chat(const std::vector<ChatMessage> &messages,
                     std::string &out_reply, std::string &out_err) {
    ChatResult r = ChatWithTools(messages, {});
    if (!r.ok) { out_err = r.error; return false; }
    out_reply = r.content;
    return true;
}

// ---- Chat with tools -----------------------------------------------------

ChatResult LlmClient::ChatWithTools(const std::vector<ChatMessage> &messages,
                                    const std::vector<ToolDef> &tools) {
    ChatResult r;
    if (!Configured()) { r.error = "LLM chua cau hinh (OLLAMA_API_KEY/MODEL)"; return r; }

    nlohmann::json body;
    body["model"] = model_;
    body["stream"] = false;
    body["temperature"] = temperature_;

    nlohmann::json msgs = nlohmann::json::array();
    bool has_system = false;
    for (const auto &m : messages) if (m.role == "system") { has_system = true; break; }
    if (!has_system && !system_prompt_.empty()) {
        nlohmann::json sys;
        sys["role"] = "system";
        sys["content"] = system_prompt_;
        msgs.push_back(std::move(sys));
    }
    for (const auto &m : messages) {
        nlohmann::json msg;
        msg["role"] = m.role;
        if (m.role == "tool") {
            msg["content"] = m.content;
            if (!m.tool_call_id.empty()) msg["tool_call_id"] = m.tool_call_id;
        } else if (m.role == "assistant" && !m.tool_calls_json.empty()) {
            // Replay the assistant turn that carried tool_calls verbatim.
            try {
                msg = nlohmann::json::parse(m.tool_calls_json);
            } catch (...) {
                msg["role"] = "assistant";
                msg["content"] = m.content;
            }
        } else {
            msg["content"] = m.content;
        }
        msgs.push_back(std::move(msg));
    }
    body["messages"] = msgs;

    if (!tools.empty()) {
        nlohmann::json tools_arr = nlohmann::json::array();
        for (const auto &t : tools) {
            nlohmann::json fn;
            fn["type"] = "function";
            nlohmann::json func;
            func["name"] = t.name;
            func["description"] = t.description;
            try {
                func["parameters"] = nlohmann::json::parse(t.parameters_json);
            } catch (...) {
                func["parameters"] = nlohmann::json::object();
            }
            fn["function"] = func;
            tools_arr.push_back(std::move(fn));
        }
        body["tools"] = tools_arr;
    }

    const std::string payload = body.dump();
    const std::string url = base_url_ + "/chat/completions";

    std::string raw;
    r = DoPost(url, api_key_, payload, raw);
    if (!r.ok) return r;

    try {
        auto j = nlohmann::json::parse(raw);
        auto &choice = j.at("choices").at(0).at("message");
        // tool_calls?
        if (choice.contains("tool_calls") && !choice["tool_calls"].is_null()) {
            // Keep the whole assistant message (role+content+tool_calls) so we
            // can replay it verbatim in the next request.
            nlohmann::json replay = choice;
            replay["role"] = "assistant";
            r.tool_calls_json = replay.dump();
            for (auto &tc : choice["tool_calls"]) {
                ToolCall call;
                call.id = tc.value("id", "");
                if (tc.contains("function")) {
                    call.name = tc["function"].value("name", "");
                    call.arguments_json = tc["function"].value("arguments", std::string("{}"));
                    if (call.arguments_json.empty()) call.arguments_json = "{}";
                }
                r.tool_calls.push_back(std::move(call));
            }
            r.content = choice.value("content", "");
        } else {
            r.content = choice.value("content", "");
        }
    } catch (const std::exception &ex) {
        r.ok = false;
        r.error = std::string("parse reply loi: ") + ex.what() + " | body: " + raw.substr(0, 240);
        return r;
    }
    return r;
}

} // namespace jetson