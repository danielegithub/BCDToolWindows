#pragma once
#include "uefi_vars.h"
#include <string>
#include <vector>

struct UEFISnapshot {
    std::vector<uint16_t>                            BootOrder;
    std::vector<std::pair<uint16_t,
                          std::vector<uint8_t>>>     BootEntries; // num → raw blob
    std::wstring                                     Timestamp;
    std::wstring                                     Note;
};

// Save the complete current UEFI boot state to a JSON-like text file.
// Returns true on success.
bool SaveSnapshot(const std::wstring& filePath, const std::wstring& note = L"");

// Load a previously saved snapshot from file.
bool LoadSnapshot(const std::wstring& filePath, UEFISnapshot& out);

// Apply a snapshot to the UEFI variables (full restore).
// WARNING: This replaces ALL BootXXXX variables and BootOrder.
bool ApplySnapshot(const UEFISnapshot& snap);

// Return the default snapshot file path next to the exe.
std::wstring DefaultSnapshotPath();

// List all snapshot files found next to the exe.
std::vector<std::wstring> ListSnapshots();
