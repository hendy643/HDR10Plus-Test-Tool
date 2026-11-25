#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

struct Display {
    std::string description;
    bool supported = false;
    HMONITOR monitor = nullptr;    // null if the vendor path does not map it (Intel)
    unsigned maxLuminance = 0;     // standard HDR max in nits, 0 if unknown
    std::function<bool()> enable;  // false on failure, after printing why
    std::function<void()> restore; // undoes a successful enable
    std::function<bool()> reset;   // clears settings that outlive the process (Intel only)
};

// Every attached NVIDIA output, then every attached Intel output.
std::vector<Display> enumerateDisplays();
