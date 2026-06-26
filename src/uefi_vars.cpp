#include "uefi_vars.h"
#include <cstring>
#include <cstdio>
#include <algorithm>

// EFI Global Variable GUID as a wide-string for the Windows API
// {8BE4DF61-93CA-11D2-AA0D-00E098032B8C}
static const wchar_t* EFI_GLOBAL_GUID_STR =
    L"{8BE4DF61-93CA-11D2-AA0D-00E098032B8C}";

// ────────────────────────────────────────────────────────────────────────────
// Low-level UEFI variable access (Windows wraps these via firmware calls)
// ────────────────────────────────────────────────────────────────────────────

static std::vector<uint8_t> ReadUEFIVar(const wchar_t* varName)
{
    // First call: get required size
    DWORD needed = GetFirmwareEnvironmentVariableW(varName, EFI_GLOBAL_GUID_STR,
        nullptr, 0);
    DWORD err = GetLastError();

    // If needed==0 and err==ERROR_INSUFFICIENT_BUFFER we got the size.
    // Some firmware reports directly the needed size; others require a guess.
    if (needed == 0 && err == ERROR_INSUFFICIENT_BUFFER)
    {
        // Retry with 0 doesn't give size on all implementations — use a large buf
        needed = 4096;
    }
    else if (needed == 0)
    {
        // Variable doesn't exist or access denied
        return {};
    }

    std::vector<uint8_t> buf(needed, 0);
    DWORD got = GetFirmwareEnvironmentVariableW(varName, EFI_GLOBAL_GUID_STR,
        buf.data(), (DWORD)buf.size());

    if (got == 0)
    {
        err = GetLastError();
        if (err == ERROR_INSUFFICIENT_BUFFER)
        {
            // Grow and retry once
            buf.resize(buf.size() * 4, 0);
            got = GetFirmwareEnvironmentVariableW(varName, EFI_GLOBAL_GUID_STR,
                buf.data(), (DWORD)buf.size());
            if (got == 0)
                return {};
        }
        else
            return {};
    }

    buf.resize(got);
    return buf;
}

static bool WriteUEFIVar(const wchar_t* varName, const void* data, DWORD size)
{
    return SetFirmwareEnvironmentVariableW(varName, EFI_GLOBAL_GUID_STR,
        const_cast<void*>(data), size) != FALSE;
}

static bool DeleteUEFIVar(const wchar_t* varName)
{
    // Writing zero bytes deletes the variable
    return SetFirmwareEnvironmentVariableW(varName, EFI_GLOBAL_GUID_STR,
        nullptr, 0) != FALSE;
}

// ────────────────────────────────────────────────────────────────────────────
// BootOrder
// ────────────────────────────────────────────────────────────────────────────

std::vector<uint16_t> GetBootOrder()
{
    auto raw = ReadUEFIVar(L"BootOrder");
    if (raw.size() < 2)
        return {};

    std::vector<uint16_t> order;
    order.reserve(raw.size() / 2);
    for (size_t i = 0; i + 1 < raw.size(); i += 2)
    {
        uint16_t num = (uint16_t)(raw[i] | (raw[i + 1] << 8));
        order.push_back(num);
    }
    return order;
}

bool SetBootOrder(const std::vector<uint16_t>& order)
{
    if (order.empty())
        return DeleteUEFIVar(L"BootOrder");

    std::vector<uint8_t> raw(order.size() * 2);
    for (size_t i = 0; i < order.size(); ++i)
    {
        raw[i * 2]     = (uint8_t)(order[i] & 0xFF);
        raw[i * 2 + 1] = (uint8_t)(order[i] >> 8);
    }
    return WriteUEFIVar(L"BootOrder", raw.data(), (DWORD)raw.size());
}

// ────────────────────────────────────────────────────────────────────────────
// Parse a raw EFI_LOAD_OPTION blob
// ────────────────────────────────────────────────────────────────────────────

static EFIBootEntry ParseLoadOption(uint16_t bootNum, const std::vector<uint8_t>& raw)
{
    EFIBootEntry e = {};
    e.BootNum = bootNum;
    e.Valid   = false;

    if (raw.size() < 6)
        return e;

    const uint8_t* p   = raw.data();
    const uint8_t* end = p + raw.size();

    // Attributes (4 bytes)
    e.Attributes = *(const uint32_t*)p;
    p += 4;

    // FilePathListLength (2 bytes)
    uint16_t fpLen = *(const uint16_t*)p;
    p += 2;

    // Description (null-terminated UTF-16)
    const wchar_t* descStart = reinterpret_cast<const wchar_t*>(p);
    size_t descMaxChars = (end - p) / 2;
    size_t descLen = 0;
    while (descLen < descMaxChars && descStart[descLen] != L'\0')
        ++descLen;

    if (descLen == descMaxChars)
        return e;  // No null terminator found

    e.Description = std::wstring(descStart, descLen);
    p += (descLen + 1) * 2;

    // Device path list (fpLen bytes)
    if ((size_t)(end - p) < fpLen)
        return e;

    const uint8_t* dpStart = p;
    const uint8_t* dpEnd   = p + fpLen;

    e.RawDevicePath.assign(dpStart, dpEnd);

    // Walk device path nodes
    const uint8_t* dp = dpStart;
    while (dp + 4 <= dpEnd)
    {
        const EFI_DEVICE_PATH_PROTOCOL* node =
            reinterpret_cast<const EFI_DEVICE_PATH_PROTOCOL*>(dp);

        uint16_t nodeLen = node->Length;
        if (nodeLen < 4)
            break;
        if (dp + nodeLen > dpEnd)
            break;

        if (node->Type == END_DEVICE_PATH_TYPE)
            break;

        if (node->Type == MEDIA_DEVICE_PATH)
        {
            if (node->SubType == MEDIA_HARDDRIVE_DP && nodeLen == 42)
            {
                if (!e.HasHDPath)
                {
                    const HARDDRIVE_DEVICE_PATH* hd =
                        reinterpret_cast<const HARDDRIVE_DEVICE_PATH*>(dp);

                    e.HasHDPath          = true;
                    e.HD_PartitionNumber = hd->PartitionNumber;
                    e.HD_PartitionStart  = hd->PartitionStart;
                    e.HD_PartitionSize   = hd->PartitionSize;
                    e.HD_MBRType         = hd->MBRType;
                    e.HD_SignatureType    = hd->SignatureType;

                    if (hd->SignatureType == SIGNATURE_TYPE_GUID)
                        memcpy(&e.HD_PartitionGUID, hd->Signature, 16);
                }
            }
            else if (node->SubType == MEDIA_FILEPATH_DP && nodeLen >= 4)
            {
                if (e.FilePath.empty())
                {
                    const wchar_t* pathStr =
                        reinterpret_cast<const wchar_t*>(dp + 4);
                    size_t maxChars = (nodeLen - 4) / 2;
                    size_t pathLen = 0;
                    while (pathLen < maxChars && pathStr[pathLen] != L'\0')
                        ++pathLen;
                    e.FilePath = std::wstring(pathStr, pathLen);
                }
            }
        }

        dp += nodeLen;
    }

    e.Valid = true;
    return e;
}

// ────────────────────────────────────────────────────────────────────────────
// Build a new device path blob
// ────────────────────────────────────────────────────────────────────────────

std::vector<uint8_t> BuildDevicePath(
    const EFI_GUID&     partitionGUID,
    uint64_t            partitionStartLBA,
    uint64_t            partitionSizeLBA,
    uint32_t            partitionNumber,
    const std::wstring& filePath)
{
    std::vector<uint8_t> buf;

    // ── HARDDRIVE node (42 bytes) ────────────────────────────────────────────
    {
        HARDDRIVE_DEVICE_PATH hd = {};
        hd.Header.Type    = MEDIA_DEVICE_PATH;
        hd.Header.SubType = MEDIA_HARDDRIVE_DP;
        hd.Header.Length  = 42;
        hd.PartitionNumber = partitionNumber;
        hd.PartitionStart  = partitionStartLBA;
        hd.PartitionSize   = partitionSizeLBA;
        hd.MBRType         = MBR_TYPE_GPT;
        hd.SignatureType    = SIGNATURE_TYPE_GUID;
        memcpy(hd.Signature, &partitionGUID, 16);

        const uint8_t* p = reinterpret_cast<const uint8_t*>(&hd);
        buf.insert(buf.end(), p, p + 42);
    }

    // ── FILEPATH node ────────────────────────────────────────────────────────
    {
        // Normalize backslashes
        std::wstring path = filePath;
        for (auto& c : path)
            if (c == L'/') c = L'\\';
        // Ensure leading backslash
        if (path.empty() || path[0] != L'\\')
            path = L"\\" + path;

        size_t pathBytes = (path.size() + 1) * sizeof(wchar_t); // +1 for null
        uint16_t nodeLen = (uint16_t)(4 + pathBytes);

        buf.push_back(MEDIA_DEVICE_PATH);
        buf.push_back(MEDIA_FILEPATH_DP);
        buf.push_back((uint8_t)(nodeLen & 0xFF));
        buf.push_back((uint8_t)(nodeLen >> 8));
        const uint8_t* ps = reinterpret_cast<const uint8_t*>(path.c_str());
        buf.insert(buf.end(), ps, ps + pathBytes);
    }

    // ── END node (4 bytes) ───────────────────────────────────────────────────
    buf.push_back(END_DEVICE_PATH_TYPE);
    buf.push_back(END_ENTIRE_DEVICE_PATH);
    buf.push_back(4);
    buf.push_back(0);

    return buf;
}

// ────────────────────────────────────────────────────────────────────────────
// Serialize an EFIBootEntry to a raw EFI_LOAD_OPTION blob
// ────────────────────────────────────────────────────────────────────────────

std::vector<uint8_t> SerializeBootEntry(const EFIBootEntry& entry)
{
    std::vector<uint8_t> buf;

    // Attributes (4 bytes)
    uint32_t attr = entry.Attributes;
    buf.push_back((uint8_t)(attr & 0xFF));
    buf.push_back((uint8_t)((attr >> 8) & 0xFF));
    buf.push_back((uint8_t)((attr >> 16) & 0xFF));
    buf.push_back((uint8_t)((attr >> 24) & 0xFF));

    // FilePathListLength (2 bytes)
    uint16_t fpLen = (uint16_t)entry.RawDevicePath.size();
    buf.push_back((uint8_t)(fpLen & 0xFF));
    buf.push_back((uint8_t)(fpLen >> 8));

    // Description (null-terminated UTF-16)
    const uint8_t* dp = reinterpret_cast<const uint8_t*>(entry.Description.c_str());
    size_t descBytes   = (entry.Description.size() + 1) * sizeof(wchar_t);
    buf.insert(buf.end(), dp, dp + descBytes);

    // Device path
    buf.insert(buf.end(), entry.RawDevicePath.begin(), entry.RawDevicePath.end());

    return buf;
}

// ────────────────────────────────────────────────────────────────────────────
// Public API
// ────────────────────────────────────────────────────────────────────────────

EFIBootEntry ReadBootEntry(uint16_t bootNum)
{
    wchar_t varName[16];
    swprintf_s(varName, 16, L"Boot%04X", bootNum);

    auto raw = ReadUEFIVar(varName);
    if (raw.empty())
    {
        EFIBootEntry e = {};
        e.BootNum = bootNum;
        e.Valid   = false;
        return e;
    }
    return ParseLoadOption(bootNum, raw);
}

bool WriteBootEntry(uint16_t bootNum, const EFIBootEntry& entry)
{
    wchar_t varName[16];
    swprintf_s(varName, 16, L"Boot%04X", bootNum);

    auto blob = SerializeBootEntry(entry);
    return WriteUEFIVar(varName, blob.data(), (DWORD)blob.size());
}

bool DeleteBootEntry(uint16_t bootNum)
{
    wchar_t varName[16];
    swprintf_s(varName, 16, L"Boot%04X", bootNum);
    return DeleteUEFIVar(varName);
}

std::vector<uint16_t> GetExistingBootNums()
{
    std::vector<uint16_t> result;
    for (int i = 0; i <= 0xFFFF; ++i)
    {
        wchar_t varName[16];
        swprintf_s(varName, 16, L"Boot%04X", i);

        // Zero-size probe
        DWORD r = GetFirmwareEnvironmentVariableW(varName, EFI_GLOBAL_GUID_STR,
            nullptr, 0);
        DWORD err = GetLastError();

        if (r > 0 || err == ERROR_INSUFFICIENT_BUFFER)
            result.push_back((uint16_t)i);
    }
    return result;
}

uint16_t FindFreeBootSlot()
{
    auto existing = GetExistingBootNums();
    std::sort(existing.begin(), existing.end());

    for (int i = 0; i <= 0xFFFF; ++i)
    {
        if (!std::binary_search(existing.begin(), existing.end(), (uint16_t)i))
            return (uint16_t)i;
    }
    return 0xFFFF; // Should never happen
}
