#pragma once
#include "gpt_reader.h"
#include <string>
#include <vector>

struct MountedESP {
    std::wstring  VolumePath;      // \\?\Volume{GUID}\  (always available)
    std::wstring  DriveLetter;     // e.g. L"S:\\"  (empty if not mounted)
    std::wstring  AccessPath;      // Best path to use for file access
    PartitionInfo Partition;       // From GPT reader
    bool          WasMountedByUs;  // True if we assigned the drive letter
};

// Find all mounted volumes that correspond to an ESP partition.
// Also mounts ESPs that aren't already mounted (assigns a temp drive letter).
std::vector<MountedESP> FindAndMountESPs(const std::vector<PartitionInfo>& espPartitions);

// Release a drive letter we assigned
bool ReleaseDriveLetter(const std::wstring& driveLetter);
