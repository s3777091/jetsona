#include "net/vpn_manager.h"

#include "esp_log.h"
#include "platform/shell_command.h"
#include "settings.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

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

VpnStatus VpnManager::QueryStatusLocked(bool persist) {
    VpnStatus status;
    status.exit_node = ExitNode();
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
    return QueryStatusLocked(true);
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

    const VpnStatus before = QueryStatusLocked(false);
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

    std::string command = "timeout --signal=TERM 15s tailscale set --exit-node=";
    if (enabled) {
        command += platform::QuoteShellArgument(ExitNode());
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
            platform::QuoteShellArgument(ExitNode());
        auto ping = platform::RunShellCommand(ping_command);
        if (!ping.Ok()) {
            // Do not leave a dead exit-node preference in place: Tailscale's
            // fail-closed behavior would otherwise cut off public traffic.
            auto rollback = platform::RunShellCommand(
                "timeout --signal=TERM 8s tailscale set --exit-node=");
            transition.error = "Không liên lạc được với VPN exit node";
            const std::string detail = OneLineError(std::move(ping.output));
            if (!detail.empty()) transition.error += ": " + detail;
            const VpnStatus rolled_back = QueryStatusLocked(false);
            transition.enabled = rolled_back.enabled;
            if (!rollback.Ok() || transition.enabled) {
                transition.error += "; chưa gỡ được exit node";
            }
            PersistEnabled(transition.enabled);
            ESP_LOGE(TAG, "%s", transition.error.c_str());
            return transition;
        }
    }

    const VpnStatus after = QueryStatusLocked(false);
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
                 ExitNode().c_str());
    }
    return transition;
}

} // namespace jetson
