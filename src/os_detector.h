#pragma once
#include "esp_scanner.h"
#include <string>
#include <vector>

struct DetectedOS {
    std::wstring  Name;          // e.g. "Ubuntu 22.04", "Windows 11", "Fedora"
    std::wstring  Vendor;        // e.g. "Canonical", "Microsoft", "Red Hat"
    std::wstring  BootloaderPath; // Relative path inside ESP, e.g. \EFI\ubuntu\grubx64.efi
    std::wstring  ESPAccessPath;  // Full path prefix, e.g. S:\
    PartitionInfo ESPPartition;
    int           Priority;      // Suggested priority (lower = higher priority; Windows=0)
};

// Scan an ESP and return all detected OSes
std::vector<DetectedOS> DetectOSesInESP(const MountedESP& esp);

// Scan all ESPs and return every OS found
std::vector<DetectedOS> DetectAllOSes(const std::vector<MountedESP>& esps);
