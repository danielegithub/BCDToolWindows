#include "boot_editor.h"
#include "backup.h"
#include "gpt_reader.h"
#include "esp_scanner.h"
#include <cstdio>
#include <algorithm>
#include <cwctype>

// ────────────────────────────────────────────────────────────────────────────
// Helpers
// ────────────────────────────────────────────────────────────────────────────

static std::wstring ToLower(std::wstring s)
{
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

// ────────────────────────────────────────────────────────────────────────────
// List all entries
// ────────────────────────────────────────────────────────────────────────────

std::vector<BootEntryFull> ListAllBootEntries()
{
    auto order   = GetBootOrder();
    auto allNums = GetExistingBootNums();

    // Build ordered list first
    std::vector<BootEntryFull> result;
    result.reserve(order.size() + allNums.size());

    for (int idx = 0; idx < (int)order.size(); ++idx)
    {
        uint16_t num = order[idx];
        auto entry   = ReadBootEntry(num);
        if (!entry.Valid && entry.Description.empty())
            continue;  // Variable disappeared

        BootEntryFull full;
        full.Entry      = entry;
        full.OrderIndex = idx;
        full.IsActive   = (entry.Attributes & LOAD_OPTION_ACTIVE) != 0;
        result.push_back(full);
    }

    // Append orphaned entries (exist as BootXXXX but not in BootOrder)
    for (uint16_t num : allNums)
    {
        bool inOrder = false;
        for (const auto& r : result)
            if (r.Entry.BootNum == num) { inOrder = true; break; }

        if (!inOrder)
        {
            auto entry = ReadBootEntry(num);
            BootEntryFull full;
            full.Entry      = entry;
            full.OrderIndex = -1;
            full.IsActive   = (entry.Attributes & LOAD_OPTION_ACTIVE) != 0;
            result.push_back(full);
        }
    }

    return result;
}

// ────────────────────────────────────────────────────────────────────────────
// Print table
// ────────────────────────────────────────────────────────────────────────────

void PrintBootEntries(const std::vector<BootEntryFull>& entries)
{
    if (entries.empty())
    {
        wprintf(L"  (nessuna voce di boot trovata)\n");
        return;
    }

    wprintf(L"\n");
    wprintf(L"  %-6s %-8s %-4s %-32s %-40s\n",
        L"Ordine", L"Boot#", L"Att", L"Nome", L"File");
    wprintf(L"  %s\n", std::wstring(96, L'-').c_str());

    for (const auto& f : entries)
    {
        wchar_t orderStr[8];
        if (f.OrderIndex >= 0)
            swprintf_s(orderStr, 8, L"%d", f.OrderIndex + 1);
        else
            wcscpy_s(orderStr, 8, L"(orfano)");

        wchar_t numStr[8];
        swprintf_s(numStr, 8, L"%04X", f.Entry.BootNum);

        // Truncate description if too long
        std::wstring desc = f.Entry.Description;
        if (desc.size() > 30) desc = desc.substr(0, 28) + L"..";

        // Show file path (may be empty for non-file entries)
        std::wstring fp = f.Entry.FilePath;
        if (fp.size() > 38) fp = L".." + fp.substr(fp.size() - 36);

        wprintf(L"  %-6s %-8s %-4s %-32s %-40s\n",
            orderStr,
            numStr,
            f.IsActive ? L"[*]" : L"[ ]",
            desc.c_str(),
            fp.empty() ? L"(non-file)" : fp.c_str());
    }
    wprintf(L"\n");
}

// ────────────────────────────────────────────────────────────────────────────
// Add a boot entry
// ────────────────────────────────────────────────────────────────────────────

uint16_t AddBootEntry(const DetectedOS& os)
{
    uint16_t slot = FindFreeBootSlot();

    EFIBootEntry entry = {};
    entry.BootNum      = slot;
    entry.Attributes   = LOAD_OPTION_ACTIVE;
    entry.Description  = os.Name;
    entry.Valid        = true;
    entry.HasHDPath    = true;

    // Build device path
    entry.RawDevicePath = BuildDevicePath(
        os.ESPPartition.UniquePartitionGUID,
        os.ESPPartition.StartLBA,
        os.ESPPartition.SizeLBA,
        os.ESPPartition.PartitionNumber,
        os.BootloaderPath);

    if (!WriteBootEntry(slot, entry))
        return 0xFFFF;

    // Prepend to BootOrder
    auto order = GetBootOrder();
    order.insert(order.begin(), slot);
    if (!SetBootOrder(order))
    {
        DeleteBootEntry(slot);
        return 0xFFFF;
    }

    return slot;
}

// ────────────────────────────────────────────────────────────────────────────
// Remove a boot entry
// ────────────────────────────────────────────────────────────────────────────

bool RemoveBootEntry(uint16_t bootNum)
{
    bool ok = DeleteBootEntry(bootNum);

    // Remove from BootOrder
    auto order = GetBootOrder();
    order.erase(std::remove(order.begin(), order.end(), bootNum), order.end());
    SetBootOrder(order);

    // Remove from BootNext if set
    wchar_t bnVar[16];
    swprintf_s(bnVar, 16, L"Boot%04X", bootNum);
    // (BootNext is a separate variable — leave it; firmware will ignore missing entry)

    return ok;
}

// ────────────────────────────────────────────────────────────────────────────
// Reorder
// ────────────────────────────────────────────────────────────────────────────

bool MoveEntryUp(uint16_t bootNum)
{
    auto order = GetBootOrder();
    auto it = std::find(order.begin(), order.end(), bootNum);
    if (it == order.end() || it == order.begin())
        return false;
    std::swap(*it, *std::prev(it));
    return SetBootOrder(order);
}

bool MoveEntryDown(uint16_t bootNum)
{
    auto order = GetBootOrder();
    auto it = std::find(order.begin(), order.end(), bootNum);
    if (it == order.end() || std::next(it) == order.end())
        return false;
    std::swap(*it, *std::next(it));
    return SetBootOrder(order);
}

// ────────────────────────────────────────────────────────────────────────────
// BootNext (one-time override)
// ────────────────────────────────────────────────────────────────────────────

bool SetBootNext(uint16_t bootNum)
{
    static const wchar_t* EFI_GLOBAL = L"{8BE4DF61-93CA-11D2-AA0D-00E098032B8C}";
    uint8_t data[2] = { (uint8_t)(bootNum & 0xFF), (uint8_t)(bootNum >> 8) };
    return SetFirmwareEnvironmentVariableW(L"BootNext", EFI_GLOBAL, data, 2) != FALSE;
}

// ────────────────────────────────────────────────────────────────────────────
// Enable / disable
// ────────────────────────────────────────────────────────────────────────────

bool SetEntryActive(uint16_t bootNum, bool active)
{
    auto entry = ReadBootEntry(bootNum);
    if (!entry.Valid)
        return false;

    if (active)
        entry.Attributes |= LOAD_OPTION_ACTIVE;
    else
        entry.Attributes &= ~LOAD_OPTION_ACTIVE;

    return WriteBootEntry(bootNum, entry);
}

// ────────────────────────────────────────────────────────────────────────────
// Auto-configure
// ────────────────────────────────────────────────────────────────────────────

int AutoConfigure()
{
    wprintf(L"\n  [*] Scansione dischi in corso...\n");
    auto esps = FindAllESPs();
    if (esps.empty())
    {
        wprintf(L"  [!] Nessuna partizione EFI System trovata.\n");
        return 0;
    }
    wprintf(L"  [+] Trovate %zu partizione/i ESP.\n", esps.size());

    wprintf(L"  [*] Montaggio ESP e rilevamento sistemi operativi...\n");
    auto mounted = FindAndMountESPs(esps);

    auto detectedOSes = DetectAllOSes(mounted);

    if (detectedOSes.empty())
    {
        wprintf(L"  [!] Nessun sistema operativo rilevato nelle ESP.\n");

        // Unmount what we mounted
        for (const auto& m : mounted)
            if (m.WasMountedByUs)
                ReleaseDriveLetter(m.DriveLetter);
        return 0;
    }

    wprintf(L"  [+] Rilevati %zu sistema/i operativo/i:\n", detectedOSes.size());
    for (const auto& os : detectedOSes)
        wprintf(L"      - %s (%s)\n", os.Name.c_str(), os.BootloaderPath.c_str());

    // Get existing entries to avoid duplicates
    auto existing = ListAllBootEntries();

    int added = 0;

    // Add missing entries (sorted by priority: Windows first, then Linux)
    for (const auto& os : detectedOSes)
    {
        // Check if there's already an entry pointing to the same bootloader
        // on the same partition
        bool alreadyPresent = false;
        for (const auto& ex : existing)
        {
            if (!ex.Entry.Valid)
                continue;
            bool samePartition = (
                ex.Entry.HasHDPath &&
                ex.Entry.HD_PartitionGUID == os.ESPPartition.UniquePartitionGUID);
            bool samePath = ToLower(ex.Entry.FilePath) == ToLower(os.BootloaderPath);
            if (samePartition && samePath)
            {
                alreadyPresent = true;
                break;
            }
        }

        if (!alreadyPresent)
        {
            wprintf(L"  [+] Aggiunta voce: %s\n", os.Name.c_str());
            uint16_t slot = AddBootEntry(os);
            if (slot != 0xFFFF)
            {
                wprintf(L"      → Boot%04X\n", slot);
                ++added;
            }
            else
            {
                wprintf(L"  [!] Fallita aggiunta di %s\n", os.Name.c_str());
            }
        }
        else
        {
            wprintf(L"  [=] Già presente: %s\n", os.Name.c_str());
        }
    }

    // Re-read and sort boot order: Windows first, then others by priority
    auto finalEntries = ListAllBootEntries();
    auto order = GetBootOrder();

    // Sort order: put Windows entries first
    std::stable_sort(order.begin(), order.end(),
        [&finalEntries](uint16_t a, uint16_t b) {
            auto findPriority = [&](uint16_t num) -> int {
                for (const auto& f : finalEntries)
                    if (f.Entry.BootNum == num)
                    {
                        // Windows entry: priority 0
                        std::wstring desc = f.Entry.Description;
                        for (auto& c : desc) c = (wchar_t)towlower(c);
                        if (desc.find(L"windows") != std::wstring::npos)
                            return 0;
                        return 1;
                    }
                return 2;
            };
            return findPriority(a) < findPriority(b);
        });

    SetBootOrder(order);

    // Unmount what we mounted
    for (const auto& m : mounted)
        if (m.WasMountedByUs)
            ReleaseDriveLetter(m.DriveLetter);

    return added;
}
