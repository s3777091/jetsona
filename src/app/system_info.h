#ifndef SYSTEM_INFO_H
#define SYSTEM_INFO_H

#include <string>

class SystemInfo {
public:
    static std::string GetMacAddress();     // first non-lo interface MAC
    static std::string GetUserAgent();      // "jetson-nano/<version>"
    static std::string GetChipModelName() { return "jetson-nano"; }
    static uint32_t GetFreeHeapSize() { return 0; }
};

#endif