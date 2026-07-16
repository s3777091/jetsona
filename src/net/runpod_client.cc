#include "runpod_client.h"
#include "settings.h"
#include "esp_log.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <mutex>

#define TAG "RunpodClient"

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

// desiredStatus of pods the account still owns (TERMINATED pods are gone).
bool IsLivePod(const std::string &status) { return status != "TERMINATED"; }

void ParsePod(const nlohmann::json &j, RunpodPod &p) {
    p.id     = j.value("id", "");
    p.name   = j.value("name", "");
    p.image  = j.value("image", "");
    p.status = j.value("desiredStatus", "");
    if (j.contains("gpu") && j["gpu"].is_object()) {
        const auto &g = j["gpu"];
        p.gpu_name  = g.value("displayName", g.value("id", ""));
        p.gpu_count = g.value("count", 0);
    }
    p.cost_per_hr = j.value("costPerHr", 0.0);
    if (j.contains("publicIp") && j["publicIp"].is_string())
        p.public_ip = j["publicIp"].get<std::string>();
    p.memory_gb = j.value("memoryInGb", 0.0);
    p.vcpu      = j.value("vcpuCount", 0.0);
    p.volume_gb = j.value("volumeInGb", 0);
    if (j.contains("ports") && j["ports"].is_array())
        for (const auto &port : j["ports"])
            if (port.is_string()) p.ports.push_back(port.get<std::string>());
    if (j.contains("portMappings") && j["portMappings"].is_object())
        for (auto it = j["portMappings"].begin(); it != j["portMappings"].end(); ++it)
            if (it.value().is_number())
                p.port_mappings[std::atoi(it.key().c_str())] = it.value().get<int>();
}
} // namespace

int RunpodPod::HttpPort() const {
    // Common web-IDE ports first: code-server(8080/8443), Jupyter(8888).
    static const int kPreferred[] = {8443, 8080, 8888, 3000, 7860};
    std::vector<int> http_ports;
    for (const auto &port : ports) {
        if (port.find("/http") == std::string::npos) continue;
        http_ports.push_back(std::atoi(port.c_str()));
    }
    if (http_ports.empty()) return 0;
    for (int pref : kPreferred)
        if (std::find(http_ports.begin(), http_ports.end(), pref) != http_ports.end())
            return pref;
    return http_ports.front();
}

std::string RunpodPod::SshCommand() const {
    if (public_ip.empty()) return "";
    auto it = port_mappings.find(22);
    if (it == port_mappings.end()) return "";
    return "ssh root@" + public_ip + " -p " + std::to_string(it->second);
}

std::string RunpodClient::ProxyUrl(const std::string &pod_id, int port) {
    return "https://" + pod_id + "-" + std::to_string(port) + ".proxy.runpod.net";
}

RunpodClient::RunpodClient() {
    EnsureCurlInit();
    ConfigureFromSettings();
}

void RunpodClient::ConfigureFromSettings() {
    Settings s("runpod", false);
    base_url_    = EnvOr("RUNPOD_BASE_URL", s.GetString("base_url", "https://rest.runpod.io/v1"));
    graphql_url_ = EnvOr("RUNPOD_GRAPHQL_URL", "https://api.runpod.io/graphql");
    api_key_     = EnvOr("RUNPOD_API_KEY", s.GetString("api_key", ""));
}

bool RunpodClient::Request(const char *method, const std::string &url,
                           const std::string &body, std::string &out, std::string &err) {
    CURL *curl = curl_easy_init();
    if (!curl) { err = "curl_easy_init failed"; return false; }

    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    std::string auth = "Authorization: Bearer " + api_key_;
    headers = curl_slist_append(headers, auth.c_str());

    std::string resp;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    if (!body.empty())
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    // Pod creation can take a while server-side; everything else is quick.
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 90L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        err = std::string("HTTP loi: ") + curl_easy_strerror(rc);
        return false;
    }
    if (http_code < 200 || http_code >= 300) {
        // RunPod errors carry {"error": "..."} — surface just the message.
        std::string detail = resp.substr(0, 240);
        try {
            auto j = nlohmann::json::parse(resp);
            if (j.contains("error") && j["error"].is_string())
                detail = j["error"].get<std::string>();
        } catch (...) {}
        err = "HTTP " + std::to_string(http_code) + ": " + detail;
        return false;
    }
    out = std::move(resp);
    return true;
}

bool RunpodClient::ListPods(std::vector<RunpodPod> &out, std::string &err) {
    if (!Configured()) { err = "Chua cau hinh RUNPOD_API_KEY"; return false; }
    std::string raw;
    if (!Request("GET", base_url_ + "/pods", "", raw, err)) return false;
    try {
        auto j = nlohmann::json::parse(raw);
        // GET /pods returns a bare array; tolerate {"pods":[...]} too.
        const nlohmann::json *arr = &j;
        if (j.is_object() && j.contains("pods")) arr = &j["pods"];
        if (!arr->is_array()) { err = "phan hoi khong phai danh sach pod"; return false; }
        for (const auto &item : *arr) {
            RunpodPod p;
            ParsePod(item, p);
            if (!p.id.empty() && IsLivePod(p.status)) out.push_back(std::move(p));
        }
    } catch (const std::exception &ex) {
        err = std::string("parse loi: ") + ex.what();
        return false;
    }
    return true;
}

bool RunpodClient::GetPod(const std::string &pod_id, RunpodPod &out, std::string &err) {
    if (!Configured()) { err = "Chua cau hinh RUNPOD_API_KEY"; return false; }
    std::string raw;
    if (!Request("GET", base_url_ + "/pods/" + pod_id, "", raw, err)) return false;
    try {
        ParsePod(nlohmann::json::parse(raw), out);
    } catch (const std::exception &ex) {
        err = std::string("parse loi: ") + ex.what();
        return false;
    }
    return true;
}

bool RunpodClient::CreatePod(const RunpodCreateOptions &opt, RunpodPod &out, std::string &err) {
    if (!Configured()) { err = "Chua cau hinh RUNPOD_API_KEY"; return false; }
    nlohmann::json body;
    body["name"]       = opt.name.empty() ? "jetsona-pod" : opt.name;
    body["imageName"]  = opt.image;
    body["cloudType"]  = opt.cloud_type;
    body["gpuTypeIds"] = nlohmann::json::array({opt.gpu_type_id});
    body["gpuCount"]   = opt.gpu_count;
    body["containerDiskInGb"] = opt.container_disk_gb;
    body["volumeInGb"] = opt.volume_gb;
    body["interruptible"] = opt.interruptible;
    body["ports"] = opt.ports.empty()
        ? nlohmann::json::array({"8888/http", "22/tcp"})
        : nlohmann::json(opt.ports);
    if (!opt.env.empty()) body["env"] = opt.env;

    std::string raw;
    if (!Request("POST", base_url_ + "/pods", body.dump(), raw, err)) return false;
    try {
        ParsePod(nlohmann::json::parse(raw), out);
    } catch (const std::exception &ex) {
        err = std::string("parse loi: ") + ex.what();
        return false;
    }
    return true;
}

bool RunpodClient::StartPod(const std::string &pod_id, std::string &err) {
    if (!Configured()) { err = "Chua cau hinh RUNPOD_API_KEY"; return false; }
    std::string raw;
    return Request("POST", base_url_ + "/pods/" + pod_id + "/start", "", raw, err);
}

bool RunpodClient::StopPod(const std::string &pod_id, std::string &err) {
    if (!Configured()) { err = "Chua cau hinh RUNPOD_API_KEY"; return false; }
    std::string raw;
    return Request("POST", base_url_ + "/pods/" + pod_id + "/stop", "", raw, err);
}

bool RunpodClient::TerminatePod(const std::string &pod_id, std::string &err) {
    if (!Configured()) { err = "Chua cau hinh RUNPOD_API_KEY"; return false; }
    std::string raw;
    return Request("DELETE", base_url_ + "/pods/" + pod_id, "", raw, err);
}

bool RunpodClient::ListGpuTypes(std::vector<RunpodGpuType> &out, std::string &err) {
    // Offline fallback so the rent sheet always has options; prices refresh
    // from GraphQL when the call succeeds.
    static const RunpodGpuType kFallback[] = {
        {"NVIDIA GeForce RTX 4090",      "RTX 4090",  24, 0, 0},
        {"NVIDIA GeForce RTX 5090",      "RTX 5090",  32, 0, 0},
        {"NVIDIA RTX A5000",             "RTX A5000", 24, 0, 0},
        {"NVIDIA RTX A6000",             "RTX A6000", 48, 0, 0},
        {"NVIDIA L40S",                  "L40S",      48, 0, 0},
        {"NVIDIA A100 80GB PCIe",        "A100 80GB", 80, 0, 0},
        {"NVIDIA H100 80GB HBM3",        "H100 SXM",  80, 0, 0},
    };

    err.clear();
    if (Configured()) {
        nlohmann::json body;
        body["query"] =
            "query GpuTypes { gpuTypes { id displayName memoryInGb "
            "securePrice communityPrice } }";
        std::string raw;
        if (Request("POST", graphql_url_, body.dump(), raw, err)) {
            try {
                auto j = nlohmann::json::parse(raw);
                for (const auto &g : j.at("data").at("gpuTypes")) {
                    RunpodGpuType t;
                    t.id           = g.value("id", "");
                    t.display_name = g.value("displayName", t.id);
                    t.memory_gb    = g.value("memoryInGb", 0);
                    if (g.contains("securePrice") && g["securePrice"].is_number())
                        t.secure_price = g["securePrice"].get<double>();
                    if (g.contains("communityPrice") && g["communityPrice"].is_number())
                        t.community_price = g["communityPrice"].get<double>();
                    // Skip the "unknown" placeholder and CPU-ish entries.
                    if (t.id.empty() || t.id == "unknown" || t.memory_gb <= 0) continue;
                    out.push_back(std::move(t));
                }
                // Cheapest secure price first; unpriced (unavailable) last.
                std::sort(out.begin(), out.end(),
                          [](const RunpodGpuType &a, const RunpodGpuType &b) {
                              double pa = a.secure_price > 0 ? a.secure_price : 1e9;
                              double pb = b.secure_price > 0 ? b.secure_price : 1e9;
                              return pa < pb;
                          });
            } catch (const std::exception &ex) {
                err = std::string("parse gpuTypes loi: ") + ex.what();
                out.clear();
            }
        }
    } else {
        err = "Chua cau hinh RUNPOD_API_KEY";
    }

    if (out.empty())
        out.assign(std::begin(kFallback), std::end(kFallback));
    return true;
}

} // namespace jetson
