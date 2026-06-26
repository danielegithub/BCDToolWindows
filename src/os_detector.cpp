#include "os_detector.h"
#include <cstdio>
#include <algorithm>
#include <cctype>

// ────────────────────────────────────────────────────────────────────────────
// Database of known EFI bootloader paths and their OS names
// Path is relative to ESP root, always starts with backslash, lowercased.
// Priority: 0 = highest (Windows boot manager), higher = lower priority.
// ────────────────────────────────────────────────────────────────────────────

struct KnownBootloader {
    const wchar_t* Path;
    const wchar_t* OSName;
    const wchar_t* Vendor;
    int            Priority;
};

static const KnownBootloader KNOWN_BOOTLOADERS[] = {
    // Windows
    { L"\\efi\\microsoft\\boot\\bootmgfw.efi", L"Windows Boot Manager",  L"Microsoft",  10 },
    { L"\\efi\\microsoft\\boot\\bootx64.efi",  L"Windows Boot Manager",  L"Microsoft",  10 },

    // Ubuntu / Debian family
    { L"\\efi\\ubuntu\\grubx64.efi",           L"Ubuntu",                L"Canonical",  20 },
    { L"\\efi\\ubuntu\\shimx64.efi",           L"Ubuntu (Secure Boot)",  L"Canonical",  20 },
    { L"\\efi\\debian\\grubx64.efi",           L"Debian",                L"Debian",     20 },
    { L"\\efi\\debian\\shimx64.efi",           L"Debian (Secure Boot)",  L"Debian",     20 },
    { L"\\efi\\linuxmint\\grubx64.efi",        L"Linux Mint",            L"Linux Mint", 20 },
    { L"\\efi\\pop\\grubx64.efi",              L"Pop!_OS",               L"System76",   20 },
    { L"\\efi\\pop\\shimx64.efi",              L"Pop!_OS (Secure Boot)", L"System76",   20 },
    { L"\\efi\\elementary\\grubx64.efi",       L"elementary OS",         L"elementary", 20 },

    // Fedora / RHEL family
    { L"\\efi\\fedora\\grubx64.efi",           L"Fedora",                L"Red Hat",    20 },
    { L"\\efi\\fedora\\shimx64.efi",           L"Fedora (Secure Boot)",  L"Red Hat",    20 },
    { L"\\efi\\centos\\grubx64.efi",           L"CentOS",                L"Red Hat",    20 },
    { L"\\efi\\rhel\\grubx64.efi",             L"Red Hat Enterprise Linux", L"Red Hat", 20 },
    { L"\\efi\\almalinux\\grubx64.efi",        L"AlmaLinux",             L"AlmaLinux",  20 },
    { L"\\efi\\rocky\\grubx64.efi",            L"Rocky Linux",           L"Rocky",      20 },

    // openSUSE / SUSE
    { L"\\efi\\opensuse\\grubx64.efi",         L"openSUSE",              L"SUSE",       20 },
    { L"\\efi\\opensuse\\shimx64.efi",         L"openSUSE (Secure Boot)","SUSE",        20 },
    { L"\\efi\\sles\\grubx64.efi",             L"SUSE Linux Enterprise", L"SUSE",       20 },

    // Arch Linux
    { L"\\efi\\arch\\grubx64.efi",             L"Arch Linux",            L"Arch",       20 },
    { L"\\efi\\arch_grub\\grubx64.efi",        L"Arch Linux",            L"Arch",       20 },
    { L"\\efi\\endeavouros\\grubx64.efi",      L"EndeavourOS",           L"EndeavourOS",20 },
    { L"\\efi\\manjaro\\grubx64.efi",          L"Manjaro",               L"Manjaro",    20 },
    { L"\\efi\\garuda\\grubx64.efi",           L"Garuda Linux",          L"Garuda",     20 },

    // Gentoo
    { L"\\efi\\gentoo\\grubx64.efi",           L"Gentoo",                L"Gentoo",     20 },

    // NixOS
    { L"\\efi\\nixos\\grubx64.efi",            L"NixOS",                 L"NixOS",      20 },
    { L"\\efi\\nixos\\grub\\grubx64.efi",      L"NixOS",                 L"NixOS",      20 },

    // Void Linux
    { L"\\efi\\void\\grubx64.efi",             L"Void Linux",            L"Void",       20 },

    // Kali Linux
    { L"\\efi\\kali\\grubx64.efi",             L"Kali Linux",            L"Offensive Security", 20 },

    // systemd-boot (any distro)
    { L"\\efi\\systemd\\systemd-bootx64.efi",  L"systemd-boot",          L"systemd",    30 },
    { L"\\efi\\boot\\systemd-bootx64.efi",     L"systemd-boot",          L"systemd",    30 },

    // rEFInd
    { L"\\efi\\refind\\refind_x64.efi",        L"rEFInd Boot Manager",   L"rEFInd",     15 },

    // FreeBSD / OpenBSD / NetBSD
    { L"\\efi\\freebsd\\loader.efi",           L"FreeBSD",               L"FreeBSD",    20 },
    { L"\\efi\\freebsd\\boot1.efi",            L"FreeBSD",               L"FreeBSD",    20 },
    { L"\\efi\\openbsd\\bootx64.efi",          L"OpenBSD",               L"OpenBSD",    20 },
    { L"\\efi\\netbsd\\bootx64.efi",           L"NetBSD",                L"NetBSD",     20 },

    // macOS (Hackintosh / real Mac)
    { L"\\efi\\apple\\firmware\\boot.efi",     L"macOS",                 L"Apple",      20 },
    { L"\\efi\\clover\\cloverx64.efi",         L"Clover Bootloader",     L"Clover",     25 },
    { L"\\efi\\oc\\opencore.efi",              L"OpenCore Bootloader",   L"OpenCore",   15 },

    // Fallback bootloaders
    { L"\\efi\\boot\\bootx64.efi",             L"Generic EFI Bootloader",L"Unknown",    50 },
    { L"\\efi\\boot\\grubx64.efi",             L"GRUB (fallback)",       L"Unknown",    50 },
};

static const int KNOWN_BOOTLOADERS_COUNT =
    (int)(sizeof(KNOWN_BOOTLOADERS) / sizeof(KNOWN_BOOTLOADERS[0]));

// ────────────────────────────────────────────────────────────────────────────
// File existence check (works with both drive letter and volume GUID paths)
// ────────────────────────────────────────────────────────────────────────────

static bool FileExists(const std::wstring& fullPath)
{
    DWORD attr = GetFileAttributesW(fullPath.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES) && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring ToLower(std::wstring s)
{
    for (auto& c : s)
        c = (wchar_t)towlower(c);
    return s;
}

// ────────────────────────────────────────────────────────────────────────────
// Public API
// ────────────────────────────────────────────────────────────────────────────

std::vector<DetectedOS> DetectOSesInESP(const MountedESP& esp)
{
    std::vector<DetectedOS> result;

    if (esp.AccessPath.empty())
        return result;

    // Build the base path for file access (strip trailing backslash)
    std::wstring base = esp.AccessPath;
    if (!base.empty() && base.back() == L'\\')
        base.pop_back();

    for (int i = 0; i < KNOWN_BOOTLOADERS_COUNT; ++i)
    {
        const auto& kb = KNOWN_BOOTLOADERS[i];

        // kb.Path starts with \, e.g. \efi\ubuntu\grubx64.efi
        // Full path = base + path (with correct backslashes)
        std::wstring relPath = kb.Path;   // already has leading backslash
        std::wstring fullPath = base + relPath;

        if (!FileExists(fullPath))
            continue;

        // Check we haven't already added this exact path for this ESP
        bool duplicate = false;
        for (const auto& existing : result)
        {
            if (ToLower(existing.BootloaderPath) == ToLower(relPath) &&
                existing.ESPPartition.DiskIndex == esp.Partition.DiskIndex &&
                existing.ESPPartition.PartitionNumber == esp.Partition.PartitionNumber)
            {
                duplicate = true;
                break;
            }
        }
        if (duplicate)
            continue;

        DetectedOS os;
        os.Name           = kb.OSName;
        os.Vendor         = kb.Vendor;
        os.BootloaderPath = relPath;        // relative, e.g. \efi\ubuntu\grubx64.efi
        os.ESPAccessPath  = esp.AccessPath;
        os.ESPPartition   = esp.Partition;
        os.Priority       = kb.Priority;

        // For Windows, try to identify the version from the BCD store or version file
        if (wcsstr(kb.OSName, L"Windows") != nullptr)
        {
            // Could parse BCD here; for now use the generic name
            os.Name = L"Windows Boot Manager";
        }

        result.push_back(os);
    }

    return result;
}

std::vector<DetectedOS> DetectAllOSes(const std::vector<MountedESP>& esps)
{
    std::vector<DetectedOS> all;
    for (const auto& esp : esps)
    {
        auto found = DetectOSesInESP(esp);
        all.insert(all.end(), found.begin(), found.end());
    }

    // Sort by priority (lower number = higher priority)
    std::sort(all.begin(), all.end(),
        [](const DetectedOS& a, const DetectedOS& b) {
            return a.Priority < b.Priority;
        });

    return all;
}
