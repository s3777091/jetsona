#include "llm_client.h"
#include "settings.h"
#include "esp_log.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cctype>
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

/* Ekko is the firmware's own operator, not a general chatbot: the tools are the
 * only way it can touch the device, so the prompt pushes it to act first and
 * narrate afterwards. Kept short — every turn of the tool loop resends it. */
constexpr const char *kDefaultSystemPrompt =
    "Ban la Ekko, tro ly da nang song ben trong firmware cua thiet bi Jetson nay. "
    "Ban dieu khien thiet bi that: mo ung dung, chinh am luong, xem pin/wifi, "
    "phat nhac, tao lich va nhac nho. Khi nguoi dung yeu cau mot hanh dong, "
    "GOI TOOL ngay thay vi hoi lai hoac mo ta cach lam thu cong; chi hoi lai khi "
    "that su thieu thong tin bat buoc. Ngay hom nay lay tu device_status. "
    "Khi can thong tin moi tren mang, dung web_search; muon doc ky mot trang ket "
    "qua thi dung web_open voi URL do. Sau khi tool chay xong, tra loi ngan gon "
    "bang tieng Viet tu nhien ve ket qua that su da xay ra, khong bia them.";
} // namespace

LlmClient::LlmClient() {
    EnsureCurlInit();
    ConfigureFromSettings();
}

LlmClient::~LlmClient() = default;

void LlmClient::ConfigureFromSettings() {
    Settings s("llm", false);
    provider_ = EnvOr("LLM_PROVIDER", s.GetString("provider", "ollama"));
    for (auto &c : provider_) c = (char)std::tolower((unsigned char)c);

    // Per-provider defaults, then the provider's own env vars, then a
    // provider-agnostic LLM_* override for one-off experiments.
    const bool openrouter = (provider_ == "openrouter");
    const char *base_env  = openrouter ? "OPENROUTER_BASE_URL" : "OLLAMA_BASE_URL";
    const char *key_env   = openrouter ? "OPENROUTER_API_KEY"  : "OLLAMA_API_KEY";
    const char *model_env = openrouter ? "OPENROUTER_MODEL"    : "OLLAMA_MODEL";
    const char *base_def  = openrouter ? "https://openrouter.ai/api/v1" : "https://ollama.com/v1";
    const char *model_def = openrouter ? "google/gemini-2.5-flash" : "qwen2.5:7b";

    base_url_ = EnvOr("LLM_BASE_URL", EnvOr(base_env,  s.GetString("base_url", base_def)));
    api_key_  = EnvOr("LLM_API_KEY",  EnvOr(key_env,   s.GetString("api_key", "")));
    model_    = EnvOr("LLM_MODEL",    EnvOr(model_env, s.GetString("model", model_def)));
    // A stale Settings base_url/model from the other provider would silently
    // point OpenRouter at ollama.com; fall back to this provider's default.
    if (openrouter && base_url_.find("ollama.com") != std::string::npos) base_url_ = base_def;
    while (!base_url_.empty() && base_url_.back() == '/') base_url_.pop_back();

    system_prompt_ = s.GetString("system_prompt", kDefaultSystemPrompt);
    temperature_ = (double)s.GetInt("temperature_x100", 70) / 100.0; // 70 -> 0.7

    ESP_LOGI(TAG, "provider=%s base=%s model=%s key=%s", provider_.c_str(),
             base_url_.c_str(), model_.c_str(), api_key_.empty() ? "MISSING" : "set");
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
    if (url.find("openrouter.ai") != std::string::npos) {
        // OpenRouter attributes requests by these two headers; without them the
        // call still works but shows up unlabelled on the dashboard.
        headers = curl_slist_append(headers, "HTTP-Referer: https://github.com/s3777091/jetsona");
        headers = curl_slist_append(headers, "X-Title: jetsona");
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
    if (!Configured()) {
        r.error = "LLM chua cau hinh (provider=" + provider_ + ", thieu base_url/model)";
        return r;
    }
    if (api_key_.empty()) {
        r.error = "Thieu API key cho provider '" + provider_ + "' — dat " +
                  (provider_ == "openrouter" ? "OPENROUTER_API_KEY" : "OLLAMA_API_KEY") +
                  " trong .env";
        return r;
    }

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