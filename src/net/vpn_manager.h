#pragma once

#include <atomic>
#include <mutex>
#include <string>

namespace jetson {

struct VpnStatus {
    bool available = false;
    bool authenticated = false;
    bool enabled = false;
    std::string exit_node;
    std::string error;
};

struct VpnTransitionResult {
    bool success = false;
    bool enabled = false;
    std::string error;
};

/* Controls the Tailscale exit-node preference used as Jetsona's VPN.
 *
 * The exit node is selected by its Tailscale machine name/IP, not by the VM's
 * public SSH address. Set JETSON_VPN_EXIT_NODE in config.yaml; the deployment
 * helper names the intended VM "jetsona-vpn", which is also the safe default.
 * Calls that touch the CLI are blocking and must run on a worker thread. */
class VpnManager {
public:
    static VpnManager &Instance();

    std::string ExitNode() const;
    bool CachedEnabled() const { return cached_enabled_.load(); }

    VpnStatus QueryStatus();
    VpnTransitionResult SetEnabled(bool enabled);

private:
    VpnManager();

    VpnStatus QueryStatusLocked(bool persist, std::string *status_json);
    void PersistEnabled(bool enabled);

    mutable std::mutex mutex_;
    std::atomic<bool> cached_enabled_{false};
};

} // namespace jetson
