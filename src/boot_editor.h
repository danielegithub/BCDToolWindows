#pragma once
#include "uefi_vars.h"
#include "os_detector.h"
#include <string>
#include <vector>

struct BootEntryFull {
    EFIBootEntry Entry;      // Parsed UEFI variable
    int          OrderIndex; // Position in BootOrder (-1 if not in BootOrder)
    bool         IsActive;   // LOAD_OPTION_ACTIVE flag set
    std::wstring DiskLabel;  // e.g. "PhysicalDrive0"
};

// List all boot entries in BootOrder sequence, plus orphaned entries
std::vector<BootEntryFull> ListAllBootEntries();

// Print a formatted table of boot entries to stdout
void PrintBootEntries(const std::vector<BootEntryFull>& entries);

// Add a new boot entry for a detected OS and prepend it to BootOrder.
// Returns the new BootXXXX number, or 0xFFFF on failure.
uint16_t AddBootEntry(const DetectedOS& os);

// Remove a boot entry and remove it from BootOrder
bool RemoveBootEntry(uint16_t bootNum);

// Move an entry up (lower index) in BootOrder
bool MoveEntryUp(uint16_t bootNum);

// Move an entry down (higher index) in BootOrder
bool MoveEntryDown(uint16_t bootNum);

// Set a specific entry as the next one-time boot target (BootNext)
bool SetBootNext(uint16_t bootNum);

// Enable or disable a boot entry (toggle LOAD_OPTION_ACTIVE)
bool SetEntryActive(uint16_t bootNum, bool active);

// Auto-configure: scan all disks, detect OSes, add missing entries,
// remove entries for bootloaders that no longer exist, and sort by OS type.
// Returns number of entries added.
int AutoConfigure();
