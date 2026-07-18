#include "net/vpn_manager.h"

#include "esp_log.h"
#include "platform/shell_command.h"
#include "settings.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <utility>
#include <vector>

#define TAG "Vpn"

namespace jetson {
namespace {

constexpr const char *kSettingsNamespace = "network";
constexpr const char *kEnabledKey = "vpn_enabled";
constexpr const char *kDefaultExitNode = "jetsona-vpn";

bool ContainsJsonValue(const std::string &json, const char *compact,
                       const char *spaced) {
    return json.find(compact) != std::string::npos ||
           json.find(spaced) != std::string::npos;
}

bool BackendIsRunning(const std::string &json) {
    return ContainsJsonValue(json, "\"BackendState\":\"Running\"",
                             "\"BackendState\": \"Running\"");
}

bool BackendNeedsLogin(const std::string &json) {
    return ContainsJsonValue(json, "\"BackendState\":\"NeedsLogin\"",
                             "\"BackendState\": \"NeedsLogin\"") ||
           json.find("LoggedOut") != std::string::npos;
}

bool HasSelectedExitNode(const std::string &json) {
    // PeerStatus.ExitNode is true only for the peer currently selected as this
    // device's exit node. ExitNodeOption merely means a peer is eligible.
    return ContainsJsonValue(json, "\"ExitNode\":true", "\"ExitNode\": true");
}

std::string OneLineError(std::string output) {
    platform::TrimTrailingWhitespace(output);
    std::replace(output.begin(), output.end(), '\n', ' ');
    std::replace(output.begin(), output.end(), '\r', ' ');
    if (output.size() > 240) output.resize(240);
    return output;
}

bool CliInstalled() {
    auto result = platform::RunShellCommand("command -v tailscale");
    return result.Ok();
}

std::string JsonString(const nlohmann::json &object, const char *key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>()
                                                  : std::string{};
}

bool JsonBool(const nlohmann::json &object, const char *key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_boolean() && it->get<bool>();
}

std::string NormalizeNodeName(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.pop_back();
    size_t first = 0;
    while (first < value.size() &&
           std::isspace(static_cast<unsigned char>(value[first])))
        ++first;
    value.erase(0, first);
    while (!value.empty() && value.back() == '.') value.pop_back();
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool NodeNameMatches(const std::string &wanted, const std::string &candidate) {
    if (wanted.empty() || candidate.empty()) return false;
    const std::string normalized = NormalizeNodeName(candidate);
    if (wanted == normalized) return true;
    // Tailscale reports DNSName as host.tailnet.ts.net.; the configured value
    // is commonly just the MagicDNS base name (for example jetsona-vpn).
    const size_t dot = normalized.find('.');
    return wanted.find('.') == std::string::npos && dot != std::string::npos &&
           wanted == normalized.substr(0, dot);
}

struct ExitNodeCandidate {
    std::string hostname;
    std::string dns_name;
    std::string id;
    std::vector<std::string> ips;
    bool online = false;
};

struct ExitNodeResolution {
    std::string identifier;
    std::string display_name;
    std::string error;
};

std::string CandidateIdentifier(const ExitNodeCandidate &candidate) {
    // IPs are understood by old and new Tailscale clients and do not depend on
    // MagicDNS being enabled. Prefer the IPv4 address for maximum compatibility.
    for (const auto &ip : candidate.ips) {
        if (ip.rfind("100.", 0) == 0) return ip;
    }
    if (!candidate.ips.empty()) return candidate.ips.front();
    if (!candidate.id.empty()) return candidate.id;
    if (!candidate.dns_name.empty()) {
        std::string dns = candidate.dns_name;
        while (!dns.empty() && dns.back() == '.') dns.pop_back();
        return dns;
    }
    return candidate.hostname;
}

std::string CandidateDisplayName(const ExitNodeCandidate &candidate) {
    if (!candidate.hostname.empty()) return candidate.hostname;
    if (!candidate.dns_name.empty()) return NormalizeNodeName(candidate.dns_name);
    return CandidateIdentifier(candidate);
}

ExitNodeResolution ResolveExitNode(const std::string &status_json,
                                   const std::string &configured) {
    ExitNodeResolution resolution;
    // RunShellCommand combines stderr with stdout. Some Tailscale versions may
    // print a version warning before their JSON, so parse the JSON object rather
    // than rejecting an otherwise valid status response.
    const size_t json_begin = status_json.find('{');
    const size_t json_end = status_json.rfind('}');
    if (json_begin == std::string::npos || json_end == std::string::npos ||
        json_end < json_begin) {
        resolution.error = "Dữ liệu trạng thái Tailscale không hợp lệ";
        return resolution;
    }
    const auto root = nlohmann::json::parse(
        status_json.substr(json_begin, json_end - json_begin + 1), nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        resolution.error = "Dữ liệu trạng thái Tailscale không hợp lệ";
        return resolution;
    }

    const auto peers_it = root.find("Peer");
    if (peers_it == root.end() ||
        (!peers_it->is_object() && !peers_it->is_array())) {
        resolution.error = "Chưa có exit node nào được duyệt trong Tailscale Admin";
        return resolution;
    }

    std::vector<ExitNodeCandidate> candidates;
    auto add_candidate = [&candidates](const nlohmann::json &peer) {
        if (!peer.is_object() ||
            (!JsonBool(peer, "ExitNodeOption") && !JsonBool(peer, "ExitNode")))
            return;

        ExitNodeCandidate candidate;
        candidate.hostname = JsonString(peer, "HostName");
        candidate.dns_name = JsonString(peer, "DNSName");
        candidate.id = JsonString(peer, "ID");
        candidate.online = JsonBool(peer, "Online");
        const auto ips_it = peer.find("TailscaleIPs");
        if (ips_it != peer.end() && ips_it->is_array()) {
            for (const auto &ip : *ips_it) {
                if (ip.is_string()) candidate.ips.push_back(ip.get<std::string>());
            }
        }
        if (!CandidateIdentifier(candidate).empty())
            candidates.push_back(std::move(candidate));
    };

    if (peers_it->is_object()) {
        for (const auto &item : peers_it->items()) add_candidate(item.value());
    } else {
        for (const auto &peer : *peers_it) add_candidate(peer);
    }

    const std::string wanted = NormalizeNodeName(configured);
    const ExitNodeCandidate *offline_match = nullptr;
    for (const auto &candidate : candidates) {
        bool matches = NodeNameMatches(wanted, candidate.hostname) ||
                       NodeNameMatches(wanted, candidate.dns_name) ||
                       NodeNameMatches(wanted, candidate.id);
        for (const auto &ip : candidate.ips)
            matches = matches || NodeNameMatches(wanted, ip);
        if (!matches) continue;
        if (candidate.online) {
            resolution.identifier = CandidateIdentifier(candidate);
            resolution.display_name = CandidateDisplayName(candidate);
            return resolution;
        }
        if (!offline_match) offline_match = &candidate;
    }

    if (offline_match) {
        resolution.identifier = CandidateIdentifier(*offline_match);
        resolution.display_name = CandidateDisplayName(*offline_match);
        return resolution;
    }

    // A fresh install often still has the default name in config.yaml while
    // the administrator renamed the only approved exit node. Selecting the
    // sole eligible peer is deterministic and avoids passing an invalid name
    // to the CLI. Multiple eligible peers still require an explicit choice.
    if (candidates.size() == 1) {
        resolution.identifier = CandidateIdentifier(candidates.front());
        resolution.display_name = CandidateDisplayName(candidates.front());
        return resolution;
    }

    if (candidates.empty()) {
        resolution.error = "Chưa có exit node nào được duyệt trong Tailscale Admin";
    } else {
        resolution.error = "Không tìm thấy exit node \"" + configured +
                           "\"; hãy cấu hình tên hoặc IP Tailscale 100.x";
    }
    return resolution;
}

} // namespace

VpnManager &VpnManager::Instance() {
    static VpnManager manager;
    return manager;
}

VpnManager::VpnManager()
    : cached_enabled_(Settings(kSettingsNamespace, false).GetBool(kEnabledKey, false)) {}

std::string VpnManager::ExitNode() const {
    const char *configured = std::getenv("JETSON_VPN_EXIT_NODE");
    return configured && *configured ? configured : kDefaultExitNode;
}

void VpnManager::PersistEnabled(bool enabled) {
    cached_enabled_ = enabled;
    Settings(kSettingsNamespace, true).SetBool(kEnabledKey, enabled);
}

VpnStatus VpnManager::QueryStatusLocked(bool persist, std::string *status_json) {
    VpnStatus status;
    status.exit_node = ExitNode();
    if (status_json) status_json->clear();
    if (!CliInstalled()) {
        status.error = "Tailscale chưa được cài trên Jetson";
        if (persist) PersistEnabled(false);
        return status;
    }
    status.available = true;

    auto result = platform::RunShellCommand(
        "timeout --signal=TERM 8s tailscale status --json");
    if (!result.Ok()) {
        const std::string detail = OneLineError(std::move(result.output));
        status.error = detail.empty() ? "Không đọc được trạng thái Tailscale"
                                      : "Tailscale: " + detail;
        return status;
    }
    if (status_json) *status_json = result.output;

    if (BackendNeedsLogin(result.output)) {
        status.error = "Tailscale chưa đăng nhập vào tailnet";
        if (persist) PersistEnabled(false);
        return status;
    }
    status.authenticated = BackendIsRunning(result.output);
    if (!status.authenticated) {
        status.error = "Dịch vụ Tailscale chưa sẵn sàng";
        return status;
    }

    status.enabled = HasSelectedExitNode(result.output);
    if (persist) PersistEnabled(status.enabled);
    return status;
}

VpnStatus VpnManager::QueryStatus() {
    std::lock_guard<std::mutex> lock(mutex_);
    return QueryStatusLocked(true, nullptr);
}

VpnTransitionResult VpnManager::SetEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    VpnTransitionResult transition;

    if (!CliInstalled()) {
        if (!enabled) {
            PersistEnabled(false);
            transition.success = true;
            return transition;
        }
        transition.error = "Tailscale chưa được cài trên Jetson";
        transition.enabled = cached_enabled_.load();
        return transition;
    }

    // The firmware service runs as root. Starting an installed-but-stopped
    // daemon here makes the Settings toggle recover cleanly after service work.
    auto daemon = platform::RunShellCommand(
        "timeout --signal=TERM 4s systemctl is-active --quiet tailscaled");
    if (!daemon.Ok()) {
        daemon = platform::RunShellCommand(
            "timeout --signal=TERM 10s systemctl start tailscaled");
        if (!daemon.Ok() && enabled) {
            transition.error = "Không khởi động được dịch vụ tailscaled";
            transition.enabled = cached_enabled_.load();
            return transition;
        }
    }

    std::string before_status_json;
    const VpnStatus before = QueryStatusLocked(false, &before_status_json);
    if (enabled && !before.authenticated) {
        transition.error = before.error.empty()
                               ? "Tailscale chưa đăng nhập vào tailnet"
                               : before.error;
        transition.enabled = before.enabled;
        PersistEnabled(before.enabled);
        return transition;
    }
    if (!enabled && !before.authenticated) {
        // There cannot be an active exit-node route without a running,
        // authenticated backend. Clear a stale UI preference locally.
        PersistEnabled(false);
        transition.success = true;
        return transition;
    }

    ExitNodeResolution exit_node;
    if (enabled) {
        exit_node = ResolveExitNode(before_status_json, ExitNode());
        if (exit_node.identifier.empty()) {
            transition.error = exit_node.error.empty()
                                   ? "Không tìm thấy Tailscale exit node"
                                   : exit_node.error;
            transition.enabled = before.enabled;
            PersistEnabled(transition.enabled);
            ESP_LOGE(TAG, "%s", transition.error.c_str());
            return transition;
        }
        if (NormalizeNodeName(exit_node.display_name) !=
            NormalizeNodeName(ExitNode())) {
            ESP_LOGW(TAG, "configured exit node %s resolved to %s (%s)",
                     ExitNode().c_str(), exit_node.display_name.c_str(),
                     exit_node.identifier.c_str());
        }
    }

    std::string command = "timeout --signal=TERM 15s tailscale set --exit-node=";
    if (enabled) {
        command += platform::QuoteShellArgument(exit_node.identifier);
        command += " --exit-node-allow-lan-access=true";
    }

    auto result = platform::RunShellCommand(command);
    if (!result.Ok()) {
        transition.enabled = before.enabled;
        transition.error = OneLineError(std::move(result.output));
        if (transition.error.empty()) {
            transition.error = enabled ? "Không chọn được Tailscale exit node"
                                       : "Không bỏ được Tailscale exit node";
        }
        ESP_LOGE(TAG, "transition to %s failed: %s", enabled ? "on" : "off",
                 transition.error.c_str());
        PersistEnabled(transition.enabled);
        return transition;
    }

    if (enabled) {
        const std::string ping_command =
            "timeout --signal=TERM 8s tailscale ping --c=1 --timeout=5s "
            "--until-direct=false " +
            platform::QuoteShellArgument(exit_node.identifier);
        auto ping = platform::RunShellCommand(ping_command);
        if (!ping.Ok()) {
            // Do not leave a dead exit-node preference in place: Tailscale's
            // fail-closed behavior would otherwise cut off public traffic.
            auto rollback = platform::RunShellCommand(
                "timeout --signal=TERM 8s tailscale set --exit-node=");
            transition.error = "Không liên lạc được với VPN exit node";
            const std::string detail = OneLineError(std::move(ping.output));
            if (!detail.empty()) transition.error += ": " + detail;
            const VpnStatus rolled_back = QueryStatusLocked(false, nullptr);
            transition.enabled = rolled_back.enabled;
            if (!rollback.Ok() || transition.enabled) {
                transition.error += "; chưa gỡ được exit node";
            }
            PersistEnabled(transition.enabled);
            ESP_LOGE(TAG, "%s", transition.error.c_str());
            return transition;
        }
    }

    const VpnStatus after = QueryStatusLocked(false, nullptr);
    transition.enabled = after.enabled;
    transition.success = after.authenticated && after.enabled == enabled;
    if (!transition.success) {
        transition.error = after.error.empty()
                               ? (enabled ? "Exit node chưa được tailnet cho phép"
                                          : "VPN vẫn đang được chọn")
                               : after.error;
    }
    PersistEnabled(transition.enabled);
    if (transition.success) {
        ESP_LOGI(TAG, "VPN %s via exit node %s", enabled ? "enabled" : "disabled",
                 enabled ? exit_node.identifier.c_str() : ExitNode().c_str());
    }
    return transition;
}

} // namespace jetson
