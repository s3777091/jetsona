#pragma once

#include <map>
#include <string>
#include <vector>

namespace jetson {

/* One rentable GPU model on RunPod (from the GraphQL gpuTypes catalog). */
struct RunpodGpuType {
    std::string id;            // e.g. "NVIDIA GeForce RTX 4090"
    std::string display_name;  // e.g. "RTX 4090"
    int memory_gb = 0;         // VRAM
    double secure_price = 0;   // $/hr on Secure Cloud (0 = unknown)
    double community_price = 0;// $/hr on Community Cloud (0 = unknown)
};

/* One pod (GPU VM) on the account, as returned by GET /pods. */
struct RunpodPod {
    std::string id;
    std::string name;
    std::string image;
    std::string status;        // desiredStatus: RUNNING | EXITED | TERMINATED
    std::string gpu_name;      // gpu.displayName (falls back to gpu.id)
    int gpu_count = 0;
    double cost_per_hr = 0;
    std::string public_ip;
    std::vector<std::string> ports;      // e.g. "8888/http", "22/tcp"
    std::map<int, int> port_mappings;    // internal -> public (tcp ports)
    double memory_gb = 0;
    double vcpu = 0;
    int volume_gb = 0;

    bool running() const { return status == "RUNNING"; }
    // First exposed HTTP port (prefers common IDE/Jupyter ports), 0 if none.
    int HttpPort() const;
    // Public SSH command via the direct TCP mapping, "" when port 22 is not
    // exposed or the pod has no public IP yet.
    std::string SshCommand() const;
};

/* Everything POST /pods needs to rent a new GPU pod. */
struct RunpodCreateOptions {
    std::string name;
    std::string image;
    std::string gpu_type_id;
    int gpu_count = 1;
    std::string cloud_type = "SECURE";   // SECURE | COMMUNITY
    int container_disk_gb = 50;
    int volume_gb = 20;
    bool interruptible = false;          // spot pricing
    std::vector<std::string> ports;      // default set in CreatePod
    std::map<std::string, std::string> env;
};

/* Minimal RunPod client for the Pods drawer app.
 *
 * Pods CRUD goes through the REST API (https://rest.runpod.io/v1, Bearer
 * RUNPOD_API_KEY). The GPU-type catalog is only served by the legacy GraphQL
 * API (https://api.runpod.io/graphql), same key; when that call fails a small
 * built-in catalog of common GPUs is returned so the rent sheet still works.
 *
 * Blocking API (libcurl): call on a worker thread, never the LVGL thread.
 *
 * Config (highest priority first):
 *   env RUNPOD_API_KEY / RUNPOD_BASE_URL
 *   Settings namespace "runpod": api_key, base_url */
class RunpodClient {
public:
    RunpodClient();

    void ConfigureFromSettings();
    bool Configured() const { return !api_key_.empty(); }

    bool ListPods(std::vector<RunpodPod> &out, std::string &err);
    bool GetPod(const std::string &pod_id, RunpodPod &out, std::string &err);
    bool CreatePod(const RunpodCreateOptions &opt, RunpodPod &out, std::string &err);
    bool StartPod(const std::string &pod_id, std::string &err);
    bool StopPod(const std::string &pod_id, std::string &err);
    bool TerminatePod(const std::string &pod_id, std::string &err); // DELETE

    // GPU catalog with $/hr pricing. Never fails hard: on network/API errors
    // fills `out` with the built-in fallback list and still returns true
    // (err carries the reason so the UI can mention stale pricing).
    bool ListGpuTypes(std::vector<RunpodGpuType> &out, std::string &err);

    /* RunPod's HTTP proxy for a pod's exposed /http port:
     * https://{podId}-{port}.proxy.runpod.net */
    static std::string ProxyUrl(const std::string &pod_id, int port);

private:
    bool Request(const char *method, const std::string &path,
                 const std::string &body, std::string &out, std::string &err);

    std::string base_url_;   // https://rest.runpod.io/v1
    std::string graphql_url_;// https://api.runpod.io/graphql
    std::string api_key_;
};

} // namespace jetson
