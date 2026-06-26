#include "backup.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <shlobj.h>

// We store snapshots as a simple binary format:
//
// Header: "BCDTOOL_SNAP\0" (13 bytes)
// Version: uint32 = 1
// Timestamp UTF-16 length: uint32 (chars including null)
// Timestamp UTF-16 data
// Note UTF-16 length: uint32
// Note UTF-16 data
// BootOrder entry count: uint32
// BootOrder entries: uint16 × count
// BootEntry count: uint32
// For each entry:
//   BootNum: uint16
//   BlobSize: uint32
//   Blob: uint8 × BlobSize

static const char SNAP_MAGIC[] = "BCDTOOL_SNAP\0";
static const uint32_t SNAP_VERSION = 1;

// ────────────────────────────────────────────────────────────────────────────
// Helper: read all raw bytes of a BootXXXX variable
// ────────────────────────────────────────────────────────────────────────────

static const wchar_t* EFI_GLOBAL = L"{8BE4DF61-93CA-11D2-AA0D-00E098032B8C}";

static std::vector<uint8_t> RawReadBootVar(uint16_t num)
{
    wchar_t name[16];
    swprintf_s(name, 16, L"Boot%04X", num);

    DWORD needed = 4096;
    std::vector<uint8_t> buf(needed, 0);
    DWORD got = GetFirmwareEnvironmentVariableW(name, EFI_GLOBAL,
        buf.data(), (DWORD)buf.size());

    if (got == 0)
    {
        DWORD err = GetLastError();
        if (err == ERROR_INSUFFICIENT_BUFFER)
        {
            buf.resize(buf.size() * 4, 0);
            got = GetFirmwareEnvironmentVariableW(name, EFI_GLOBAL,
                buf.data(), (DWORD)buf.size());
        }
        if (got == 0)
            return {};
    }
    buf.resize(got);
    return buf;
}

static bool RawWriteBootVar(uint16_t num, const std::vector<uint8_t>& blob)
{
    wchar_t name[16];
    swprintf_s(name, 16, L"Boot%04X", num);
    return SetFirmwareEnvironmentVariableW(name, EFI_GLOBAL,
        const_cast<uint8_t*>(blob.data()), (DWORD)blob.size()) != FALSE;
}

static bool RawDeleteBootVar(uint16_t num)
{
    wchar_t name[16];
    swprintf_s(name, 16, L"Boot%04X", num);
    return SetFirmwareEnvironmentVariableW(name, EFI_GLOBAL, nullptr, 0) != FALSE;
}

// ────────────────────────────────────────────────────────────────────────────
// Binary write helpers
// ────────────────────────────────────────────────────────────────────────────

static void WriteU32(FILE* f, uint32_t v)
{
    fwrite(&v, 4, 1, f);
}

static void WriteU16(FILE* f, uint16_t v)
{
    fwrite(&v, 2, 1, f);
}

static void WriteWStr(FILE* f, const std::wstring& s)
{
    uint32_t len = (uint32_t)(s.size() + 1);
    WriteU32(f, len);
    fwrite(s.c_str(), sizeof(wchar_t), len, f);
}

static void WriteBytes(FILE* f, const std::vector<uint8_t>& v)
{
    uint32_t sz = (uint32_t)v.size();
    WriteU32(f, sz);
    if (sz > 0)
        fwrite(v.data(), 1, sz, f);
}

// ────────────────────────────────────────────────────────────────────────────
// Binary read helpers
// ────────────────────────────────────────────────────────────────────────────

static bool ReadU32(FILE* f, uint32_t& v)
{
    return fread(&v, 4, 1, f) == 1;
}

static bool ReadU16(FILE* f, uint16_t& v)
{
    return fread(&v, 2, 1, f) == 1;
}

static bool ReadWStr(FILE* f, std::wstring& s)
{
    uint32_t len = 0;
    if (!ReadU32(f, len) || len == 0 || len > 4096)
        return false;
    std::vector<wchar_t> buf(len);
    if (fread(buf.data(), sizeof(wchar_t), len, f) != len)
        return false;
    buf.back() = L'\0';
    s = buf.data();
    return true;
}

static bool ReadBytes(FILE* f, std::vector<uint8_t>& v)
{
    uint32_t sz = 0;
    if (!ReadU32(f, sz))
        return false;
    v.resize(sz);
    if (sz > 0 && fread(v.data(), 1, sz, f) != sz)
        return false;
    return true;
}

// ────────────────────────────────────────────────────────────────────────────
// Timestamp helper
// ────────────────────────────────────────────────────────────────────────────

static std::wstring CurrentTimestamp()
{
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    wchar_t buf[64];
    swprintf_s(buf, 64, L"%04d-%02d-%02d_%02d-%02d-%02d",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// ────────────────────────────────────────────────────────────────────────────
// Public API
// ────────────────────────────────────────────────────────────────────────────

std::wstring DefaultSnapshotPath()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    // Replace extension with _snapshot_TIMESTAMP.bcdtool
    std::wstring path = exePath;
    size_t dot = path.rfind(L'.');
    size_t slash = path.rfind(L'\\');
    if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash))
        path = path.substr(0, dot);

    return path + L"_snapshot_" + CurrentTimestamp() + L".bcdtool";
}

std::vector<std::wstring> ListSnapshots()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    std::wstring dir = exePath;
    size_t slash = dir.rfind(L'\\');
    if (slash != std::wstring::npos)
        dir = dir.substr(0, slash + 1);

    std::vector<std::wstring> result;
    WIN32_FIND_DATAW fd = {};
    std::wstring pattern = dir + L"*.bcdtool";
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return result;
    do
    {
        result.push_back(dir + fd.cFileName);
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
    return result;
}

bool SaveSnapshot(const std::wstring& filePath, const std::wstring& note)
{
    // Collect current boot state
    UEFISnapshot snap;
    snap.Timestamp  = CurrentTimestamp();
    snap.Note       = note.empty() ? L"Manual backup" : note;
    snap.BootOrder  = GetBootOrder();

    // Scan all BootXXXX (0x0000 to 0xFFFF is too slow — use known list + BootOrder)
    auto existing = GetExistingBootNums();
    for (uint16_t num : existing)
    {
        auto raw = RawReadBootVar(num);
        if (!raw.empty())
            snap.BootEntries.emplace_back(num, raw);
    }

    FILE* f = nullptr;
    if (_wfopen_s(&f, filePath.c_str(), L"wb") != 0 || !f)
        return false;

    // Magic + version
    fwrite(SNAP_MAGIC, 1, sizeof(SNAP_MAGIC), f);
    WriteU32(f, SNAP_VERSION);

    // Metadata
    WriteWStr(f, snap.Timestamp);
    WriteWStr(f, snap.Note);

    // BootOrder
    WriteU32(f, (uint32_t)snap.BootOrder.size());
    for (uint16_t n : snap.BootOrder)
        WriteU16(f, n);

    // BootXXXX entries
    WriteU32(f, (uint32_t)snap.BootEntries.size());
    for (const auto& [num, blob] : snap.BootEntries)
    {
        WriteU16(f, num);
        WriteBytes(f, blob);
    }

    fclose(f);
    return true;
}

bool LoadSnapshot(const std::wstring& filePath, UEFISnapshot& out)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, filePath.c_str(), L"rb") != 0 || !f)
        return false;

    // Verify magic
    char magic[sizeof(SNAP_MAGIC)] = {};
    if (fread(magic, 1, sizeof(SNAP_MAGIC), f) != sizeof(SNAP_MAGIC))
    { fclose(f); return false; }
    if (memcmp(magic, SNAP_MAGIC, sizeof(SNAP_MAGIC)) != 0)
    { fclose(f); return false; }

    uint32_t ver = 0;
    if (!ReadU32(f, ver) || ver != SNAP_VERSION)
    { fclose(f); return false; }

    if (!ReadWStr(f, out.Timestamp) || !ReadWStr(f, out.Note))
    { fclose(f); return false; }

    // BootOrder
    uint32_t orderCount = 0;
    if (!ReadU32(f, orderCount))
    { fclose(f); return false; }
    out.BootOrder.resize(orderCount);
    for (uint32_t i = 0; i < orderCount; ++i)
    {
        uint16_t n = 0;
        if (!ReadU16(f, n)) { fclose(f); return false; }
        out.BootOrder[i] = n;
    }

    // BootEntries
    uint32_t entryCount = 0;
    if (!ReadU32(f, entryCount))
    { fclose(f); return false; }
    out.BootEntries.reserve(entryCount);
    for (uint32_t i = 0; i < entryCount; ++i)
    {
        uint16_t num = 0;
        if (!ReadU16(f, num)) { fclose(f); return false; }
        std::vector<uint8_t> blob;
        if (!ReadBytes(f, blob)) { fclose(f); return false; }
        out.BootEntries.emplace_back(num, blob);
    }

    fclose(f);
    return true;
}

bool ApplySnapshot(const UEFISnapshot& snap)
{
    // 1. Delete all current BootXXXX variables
    auto current = GetExistingBootNums();
    for (uint16_t num : current)
        RawDeleteBootVar(num);

    // 2. Write all snapshot entries
    bool ok = true;
    for (const auto& [num, blob] : snap.BootEntries)
    {
        if (!RawWriteBootVar(num, blob))
        {
            wprintf(L"  [!] Impossibile ripristinare Boot%04X\n", num);
            ok = false;
        }
    }

    // 3. Restore BootOrder
    if (!SetBootOrder(snap.BootOrder))
    {
        wprintf(L"  [!] Impossibile ripristinare BootOrder\n");
        ok = false;
    }

    return ok;
}
