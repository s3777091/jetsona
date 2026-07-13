#include "system_info.h"
#include "esp_log.h"

#include <cstdio>
#include <cstring>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define TAG "sysinfo"

std::string SystemInfo::GetMacAddress() {
    struct ifaddrs *ifap = nullptr;
    if (getifaddrs(&ifap) != 0) return "000000000000";
    std::string mac = "000000000000";
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    for (struct ifaddrs *ifa = ifap; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_name) continue;
        std::string name = ifa->ifa_name;
        if (name == "lo") continue;
        if (!(ifa->ifa_flags & IFF_UP)) continue;
        if (sock < 0) break;
        struct ifreq req;
        std::strncpy(req.ifr_name, ifa->ifa_name, IFNAMSIZ - 1);
        req.ifr_name[IFNAMSIZ - 1] = 0;
        if (ioctl(sock, SIOCGIFHWADDR, &req) == 0) {
            char buf[24];
            const unsigned char *m = (const unsigned char *)req.ifr_hwaddr.sa_data;
            std::snprintf(buf, sizeof(buf), "%02x%02x%02x%02x%02x%02x",
                          m[0], m[1], m[2], m[3], m[4], m[5]);
            mac = buf;
            break;
        }
    }
    if (sock >= 0) close(sock);
    freeifaddrs(ifap);
    return mac;
}

std::string SystemInfo::GetUserAgent() {
    return std::string("jetson-nano/0.1.0 (") + GetChipModelName() + ")";
}