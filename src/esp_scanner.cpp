#include "esp_scanner.h"
#include <winioctl.h>
#include <cstdio>
#include <cstring>
#include <algorithm>

#pragma comment(lib, "advapi32.lib")

// ────────────────────────────────────────────────────────────────────────────
// Match a volume (via IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS) to a partition
// ────────────────────────────────────────────────────────────────────────────

struct VolumeInfo {
    std::wstring VolumePath;    // \\?\Volume{GUID}
    std::wstring DriveLetter;   // "C:\\" or empty
    int          DiskNumber;
    UINT64       StartingOffset;
    UINT64       Length;
};

static std::vector<VolumeInfo> EnumerateVolumes()
{
    std::vector<VolumeInfo> result;

    wchar_t volName[MAX_PATH] = {};
    HANDLE hFind = FindFirstVolumeW(volName, MAX_PATH);
    if (hFind == INVALID_HANDLE_VALUE)
        return result;

    do
    {
        VolumeInfo vi;
        vi.VolumePath = volName;

        // Get drive letter(s) for this volume
        {
            wchar_t paths[512] = {};
            DWORD ret = 0;
            // Temporarily strip trailing backslash for GetVolumePathNamesForVolumeName
            std::wstring pathQuery = volName;
            if (!pathQuery.empty() && pathQuery.back() == L'\\')
                pathQuery.pop_back();

            if (GetVolumePathNamesForVolumeNameW(volName, paths, 512, &ret))
            {
                // paths is a double-null-terminated list of strings
                const wchar_t* p = paths;
                while (*p)
                {
                    if (wcslen(p) == 3)  // e.g. "C:\"
                    {
                        vi.DriveLetter = p;
                        break;
                    }
                    p += wcslen(p) + 1;
                }
            }
        }

        // Get disk extents
        {
            // Strip trailing backslash to open as a volume device
            std::wstring devPath = volName;
            if (!devPath.empty() && devPath.back() == L'\\')
                devPath.pop_back();

            HANDLE hVol = CreateFileW(devPath.c_str(), 0,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL, nullptr);

            if (hVol != INVALID_HANDLE_VALUE)
            {
                struct {
                    VOLUME_DISK_EXTENTS vde;
                    DISK_EXTENT extra[8];
                } extBuf = {};

                DWORD bytesRet = 0;
                if (DeviceIoControl(hVol, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                        nullptr, 0, &extBuf, sizeof(extBuf), &bytesRet, nullptr))
                {
                    if (extBuf.vde.NumberOfDiskExtents > 0)
                    {
                        vi.DiskNumber     = (int)extBuf.vde.Extents[0].DiskNumber;
                        vi.StartingOffset = (UINT64)extBuf.vde.Extents[0].StartingOffset.QuadPart;
                        vi.Length         = (UINT64)extBuf.vde.Extents[0].ExtentLength.QuadPart;
                        result.push_back(vi);
                    }
                }
                CloseHandle(hVol);
            }
        }

    } while (FindNextVolumeW(hFind, volName, MAX_PATH));

    FindVolumeClose(hFind);
    return result;
}

// ────────────────────────────────────────────────────────────────────────────
// Find a free drive letter
// ────────────────────────────────────────────────────────────────────────────

static wchar_t FindFreeDriveLetter()
{
    DWORD driveMask = GetLogicalDrives();
    for (wchar_t c = L'B'; c <= L'Z'; ++c)
    {
        if (!(driveMask & (1 << (c - L'A'))))
            return c;
    }
    return L'\0';
}

// ────────────────────────────────────────────────────────────────────────────
// Mount an ESP partition to a drive letter using DefineDosDevice
// ────────────────────────────────────────────────────────────────────────────

static bool MountVolumeToLetter(const std::wstring& volumePath, wchar_t letter)
{
    wchar_t drivePath[4] = { letter, L':', L'\0' };

    // volumePath should end with backslash for SetVolumeMountPoint
    std::wstring vp = volumePath;
    if (!vp.empty() && vp.back() != L'\\')
        vp += L'\\';

    wchar_t mountPoint[4] = { letter, L':', L'\\', L'\0' };

    return SetVolumeMountPointW(mountPoint, vp.c_str()) != FALSE;
}

bool ReleaseDriveLetter(const std::wstring& driveLetter)
{
    if (driveLetter.size() < 2)
        return false;
    wchar_t mountPoint[4] = { driveLetter[0], L':', L'\\', L'\0' };
    return DeleteVolumeMountPointW(mountPoint) != FALSE;
}

// ────────────────────────────────────────────────────────────────────────────
// Public API
// ────────────────────────────────────────────────────────────────────────────

std::vector<MountedESP> FindAndMountESPs(const std::vector<PartitionInfo>& espPartitions)
{
    auto volumes = EnumerateVolumes();
    std::vector<MountedESP> result;

    for (const auto& esp : espPartitions)
    {
        MountedESP me = {};
        me.Partition       = esp;
        me.WasMountedByUs  = false;

        // Try to match this ESP to a volume by disk number + offset
        bool found = false;
        for (const auto& vi : volumes)
        {
            if (vi.DiskNumber == esp.DiskIndex &&
                vi.StartingOffset == esp.StartingOffsetBytes)
            {
                me.VolumePath  = vi.VolumePath;
                me.DriveLetter = vi.DriveLetter;

                if (!vi.DriveLetter.empty())
                    me.AccessPath = vi.DriveLetter;
                else
                    me.AccessPath = vi.VolumePath;

                found = true;
                break;
            }
        }

        if (!found)
        {
            // ESP volume exists but wasn't found by offset — skip for now
            // (This can happen if the partition table offset doesn't match exactly)
            // Try to find any FAT volume on the right disk
            for (const auto& vi : volumes)
            {
                if (vi.DiskNumber == esp.DiskIndex)
                {
                    // Check if it has EFI directory
                    std::wstring testPath = vi.VolumePath + L"EFI";
                    DWORD attr = GetFileAttributesW(testPath.c_str());
                    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
                    {
                        me.VolumePath  = vi.VolumePath;
                        me.DriveLetter = vi.DriveLetter;
                        me.AccessPath  = vi.DriveLetter.empty() ? vi.VolumePath : vi.DriveLetter;
                        found = true;
                        break;
                    }
                }
            }
        }

        if (!found)
        {
            // ESP not mounted yet — find a free drive letter and mount it
            // First we need the volume path via FindFirstVolume comparison
            // Try to find the volume by its position on disk
            wchar_t volName[MAX_PATH] = {};
            HANDLE hFV = FindFirstVolumeW(volName, MAX_PATH);
            if (hFV != INVALID_HANDLE_VALUE)
            {
                do
                {
                    std::wstring devPath = volName;
                    if (!devPath.empty() && devPath.back() == L'\\')
                        devPath.pop_back();

                    HANDLE hVol = CreateFileW(devPath.c_str(), 0,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

                    if (hVol != INVALID_HANDLE_VALUE)
                    {
                        struct { VOLUME_DISK_EXTENTS vde; DISK_EXTENT extra[8]; } extBuf = {};
                        DWORD bytesRet = 0;
                        if (DeviceIoControl(hVol, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                                nullptr, 0, &extBuf, sizeof(extBuf), &bytesRet, nullptr))
                        {
                            if (extBuf.vde.NumberOfDiskExtents > 0 &&
                                (int)extBuf.vde.Extents[0].DiskNumber == esp.DiskIndex &&
                                (UINT64)extBuf.vde.Extents[0].StartingOffset.QuadPart
                                    == esp.StartingOffsetBytes)
                            {
                                me.VolumePath = volName;
                                wchar_t letter = FindFreeDriveLetter();
                                if (letter != L'\0' && MountVolumeToLetter(volName, letter))
                                {
                                    me.DriveLetter    = std::wstring(1, letter) + L":\\";
                                    me.AccessPath     = me.DriveLetter;
                                    me.WasMountedByUs = true;
                                }
                                else
                                {
                                    me.AccessPath = volName;
                                }
                                found = true;
                                CloseHandle(hVol);
                                break;
                            }
                        }
                        CloseHandle(hVol);
                    }
                } while (FindNextVolumeW(hFV, volName, MAX_PATH));
                FindVolumeClose(hFV);
            }
        }

        if (found || !me.AccessPath.empty())
            result.push_back(me);
    }

    return result;
}
