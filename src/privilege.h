#pragma once
#include <windows.h>
#include <string>

// Returns true if the current process has Administrator rights
bool IsRunningAsAdmin();

// Acquires the named privilege (e.g. SE_SYSTEM_ENVIRONMENT_NAME)
// Returns true on success
bool AcquirePrivilege(const wchar_t* privilegeName);

// If not already Admin, relaunches the current exe with UAC elevation and exits.
// Call this at the very start of main().
void EnsureAdminOrElevate(int argc, wchar_t* argv[]);

// Acquires all privileges needed by this tool:
//   SE_SYSTEM_ENVIRONMENT_PRIVILEGE  (read/write UEFI vars)
//   SE_MANAGE_VOLUME_PRIVILEGE       (raw disk I/O)
bool AcquireAllRequiredPrivileges();
