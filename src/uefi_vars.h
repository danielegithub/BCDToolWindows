#pragma once
#include "efi_types.h"
#include <string>
#include <vector>

// ─── Parsed boot entry ───────────────────────────────────────────────────────

struct EFIBootEntry {
    uint16_t     BootNum;           // e.g. 0x0001  → "Boot0001"
    uint32_t     Attributes;
    std::wstring Description;       // Human-readable label
    bool         Valid;             // false = variable exists but is malformed

    // Device path info (first HD() node found)
    bool         HasHDPath;
    uint32_t     HD_PartitionNumber;
    uint64_t     HD_PartitionStart; // LBA
    uint64_t     HD_PartitionSize;  // LBAs
    EFI_GUID     HD_PartitionGUID;
    uint8_t      HD_MBRType;
    uint8_t      HD_SignatureType;

    // File path (first File() node found)
    std::wstring FilePath;          // e.g.  \EFI\ubuntu\grubx64.efi

    // Raw device path bytes (for re-serialization)
    std::vector<uint8_t> RawDevicePath;
};

// ─── UEFI variable I/O ───────────────────────────────────────────────────────

// Read BootOrder → list of boot numbers in priority order
std::vector<uint16_t> GetBootOrder();

// Write BootOrder
bool SetBootOrder(const std::vector<uint16_t>& order);

// Read one BootXXXX entry; returns {BootNum=n, Valid=false} on error
EFIBootEntry ReadBootEntry(uint16_t bootNum);

// Write (create or overwrite) a BootXXXX entry
bool WriteBootEntry(uint16_t bootNum, const EFIBootEntry& entry);

// Delete a BootXXXX variable entirely
bool DeleteBootEntry(uint16_t bootNum);

// Return the list of BootXXXX numbers that actually exist
std::vector<uint16_t> GetExistingBootNums();

// Find the first unused BootXXXX slot (0x0000 … 0xFFFF)
uint16_t FindFreeBootSlot();

// Build a complete EFI_LOAD_OPTION blob from an EFIBootEntry and return it.
// The entry must have RawDevicePath already filled (use BuildDevicePath).
std::vector<uint8_t> SerializeBootEntry(const EFIBootEntry& entry);

// Build a HARDDRIVE + FILEPATH device path blob from partition info and a file path
// e.g. partGUID, 0x800, 0x82000, 1, L"\\EFI\\ubuntu\\grubx64.efi"
std::vector<uint8_t> BuildDevicePath(
    const EFI_GUID& partitionGUID,
    uint64_t        partitionStartLBA,
    uint64_t        partitionSizeLBA,
    uint32_t        partitionNumber,
    const std::wstring& filePath);
