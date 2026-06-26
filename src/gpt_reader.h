#pragma once
#include "efi_types.h"
#include <string>
#include <vector>

struct PartitionInfo {
    int          DiskIndex;         // 0-based physical disk number
    UINT32       PartitionNumber;   // 1-based
    EFI_GUID     PartitionTypeGUID;
    EFI_GUID     UniquePartitionGUID;
    UINT64       StartingOffsetBytes;
    UINT64       PartitionLengthBytes;
    UINT64       StartLBA;          // StartingOffsetBytes / BytesPerSector
    UINT64       SizeLBA;           // PartitionLengthBytes / BytesPerSector
    UINT32       BytesPerSector;
    std::wstring PartitionName;     // GPT partition label
    bool         IsESP;
};

struct DiskInfo {
    int                        DiskIndex;
    UINT64                     DiskSizeBytes;
    UINT32                     BytesPerSector;
    std::wstring               FriendlyName;
    std::vector<PartitionInfo> Partitions;
    bool                       IsGPT;
};

// Enumerate all physical disks (PhysicalDrive0 … PhysicalDriveN)
std::vector<DiskInfo> EnumerateDisks();

// Return only the ESP partitions found across all disks
std::vector<PartitionInfo> FindAllESPs();
