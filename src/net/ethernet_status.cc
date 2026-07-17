#include "net/ethernet_status.h"

#include <dirent.h>
#include <cstring>
#include <fstream>
#include <string>

namespace jetson {

bool IsEthernetConnected() {
    DIR *dir = opendir("/sys/class/net");
    if (!dir) return false;
    bool connected = false;
    while (dirent *entry = readdir(dir)) {
        const char *name = entry->d_name;
        // Wired NICs only: the Jetson onboard port is eth0, USB adapters show
        // up as eth1/enx... . wl* radios, lo and virtual bridges are skipped.
        if (std::strncmp(name, "eth", 3) != 0 &&
            std::strncmp(name, "en", 2) != 0)
            continue;
        // carrier reads "1" while the cable is plugged; the read fails while
        // the interface is administratively down, which counts as no link.
        std::ifstream carrier(std::string("/sys/class/net/") + name + "/carrier");
        int link = 0;
        if (carrier >> link && link == 1) {
            connected = true;
            break;
        }
    }
    closedir(dir);
    return connected;
}

} // namespace jetson
