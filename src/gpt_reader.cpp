#include "gpt_reader.h"
#include <winioctl.h>
#include <cstdio>
#include <cstring>
#include <algorithm>

#pragma comment(lib, "advapi32.lib")

// ────────────────────────────────────────────────────────────────────────────
// Helpers
// ────────────────────────────────────────────────────────────────────────────

static HANDLE OpenDisk(int index, bool readOnly = true)
{
    wchar_t path[32];
    swprintf_s(path, 32, L"\\\\.\\PhysicalDrive%d", index);
    DWORD access = readOnly ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE);
    return CreateFileW(path, access,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING, nullptr);
}

static UINT32 GetBytesPerSector(HANDLE hDisk)
{
    DISK_GEOMETRY_EX geo = {};
    DWORD bytesReturned = 0;
    if (!DeviceIoControl(hDisk, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
            nullptr, 0, &geo, sizeof(geo), &bytesReturned, nullptr))
        return 512;  // fallback
    return geo.Geometry.BytesPerSector > 0 ? geo.Geometry.BytesPerSector : 512;
}

static UINT64 GetDiskSize(HANDLE hDisk)
{
    DISK_GEOMETRY_EX geo = {};
    DWORD bytesReturned = 0;
    if (!DeviceIoControl(hDisk, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
            nullptr, 0, &geo, sizeof(geo), &bytesReturned, nullptr))
        return 0;
    return (UINT64)geo.DiskSize.QuadPart;
}

// ────────────────────────────────────────────────────────────────────────────
// Disk enumeration using IOCTL_DISK_GET_DRIVE_LAYOUT_EX
// ────────────────────────────────────────────────────────────────────────────

static bool ReadDiskLayout(int diskIndex, DiskInfo& out)
{
    HANDLE hDisk = OpenDisk(diskIndex);
    if (hDisk == INVALID_HANDLE_VALUE)
        return false;

    out.DiskIndex      = diskIndex;
    out.BytesPerSector = GetBytesPerSector(hDisk);
    out.DiskSizeBytes  = GetDiskSize(hDisk);
    out.IsGPT          = false;

    // Friendly name from disk path (simplified)
    {
        wchar_t buf[64];
        swprintf_s(buf, 64, L"PhysicalDrive%d", diskIndex);
        out.FriendlyName = buf;
    }

    // Allocate a large buffer for the layout (128 partitions should be enough)
    const DWORD bufSize = sizeof(DRIVE_LAYOUT_INFORMATION_EX) +
                          128 * sizeof(PARTITION_INFORMATION_EX);
    std::vector<BYTE> layoutBuf(bufSize, 0);
    auto* layout = reinterpret_cast<DRIVE_LAYOUT_INFORMATION_EX*>(layoutBuf.data());

    DWORD bytesReturned = 0;
    BOOL ok = DeviceIoControl(hDisk, IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
        nullptr, 0, layout, bufSize, &bytesReturned, nullptr);

    CloseHandle(hDisk);

    if (!ok)
        return false;

    if (layout->PartitionStyle != PARTITION_STYLE_GPT)
        return true;  // Not GPT — disk exists but has no GPT partitions we care about

    out.IsGPT = true;

    for (DWORD i = 0; i < layout->PartitionCount; ++i)
    {
        const PARTITION_INFORMATION_EX& p = layout->PartitionEntry[i];
        if (p.PartitionStyle != PARTITION_STYLE_GPT)
            continue;
        if (p.PartitionNumber == 0)
            continue;  // Extended/unallocated entries reported with number 0

        PartitionInfo pi = {};
        pi.DiskIndex              = diskIndex;
        pi.PartitionNumber        = p.PartitionNumber;
        pi.StartingOffsetBytes    = (UINT64)p.StartingOffset.QuadPart;
        pi.PartitionLengthBytes   = (UINT64)p.PartitionLength.QuadPart;
        pi.BytesPerSector         = out.BytesPerSector;
        pi.StartLBA               = pi.StartingOffsetBytes / out.BytesPerSector;
        pi.SizeLBA                = pi.PartitionLengthBytes / out.BytesPerSector;

        // Copy GUIDs (GUID == EFI_GUID layout-compatible)
        static_assert(sizeof(EFI_GUID) == sizeof(GUID), "GUID size mismatch");
        memcpy(&pi.PartitionTypeGUID,   &p.Gpt.PartitionType, 16);
        memcpy(&pi.UniquePartitionGUID, &p.Gpt.PartitionId,   16);

        pi.PartitionName = p.Gpt.Name;  // null-padded UTF-16

        pi.IsESP = (pi.PartitionTypeGUID == ESP_PARTITION_TYPE_GUID);

        out.Partitions.push_back(pi);
    }

    return true;
}

// ────────────────────────────────────────────────────────────────────────────
// Public API
// ────────────────────────────────────────────────────────────────────────────

std::vector<DiskInfo> EnumerateDisks()
{
    std::vector<DiskInfo> result;
    for (int i = 0; i < 32; ++i)   // scan up to 32 physical drives
    {
        // Quick check: can we open the drive?
        HANDLE h = OpenDisk(i);
        if (h == INVALID_HANDLE_VALUE)
            break;  // No more drives beyond this index
        CloseHandle(h);

        DiskInfo di;
        if (ReadDiskLayout(i, di))
            result.push_back(di);
    }
    return result;
}

std::vector<PartitionInfo> FindAllESPs()
{
    std::vector<PartitionInfo> esps;
    for (const auto& disk : EnumerateDisks())
    {
        for (const auto& part : disk.Partitions)
        {
            if (part.IsESP)
                esps.push_back(part);
        }
    }
    return esps;
}
