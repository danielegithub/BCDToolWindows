#include "privilege.h"
#include <shlobj.h>     // IsUserAnAdmin
#include <cstdio>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

bool IsRunningAsAdmin()
{
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;

    if (AllocateAndInitializeSid(&ntAuthority, 2,
            SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0, &adminGroup))
    {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin != FALSE;
}

bool AcquirePrivilege(const wchar_t* privilegeName)
{
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return false;

    TOKEN_PRIVILEGES tp = {};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    if (!LookupPrivilegeValueW(nullptr, privilegeName, &tp.Privileges[0].Luid))
    {
        CloseHandle(hToken);
        return false;
    }

    BOOL ok = AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    DWORD err = GetLastError();
    CloseHandle(hToken);

    return ok && (err == ERROR_SUCCESS);
}

void EnsureAdminOrElevate(int argc, wchar_t* argv[])
{
    if (IsRunningAsAdmin())
        return;

    // Re-launch with UAC elevation
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    // Rebuild command line (skip argv[0])
    std::wstring args;
    for (int i = 1; i < argc; ++i)
    {
        if (i > 1) args += L" ";
        args += L"\"";
        args += argv[i];
        args += L"\"";
    }

    SHELLEXECUTEINFOW sei = {};
    sei.cbSize       = sizeof(sei);
    sei.lpVerb       = L"runas";
    sei.lpFile       = exePath;
    sei.lpParameters = args.empty() ? nullptr : args.c_str();
    sei.nShow        = SW_NORMAL;

    if (!ShellExecuteExW(&sei))
    {
        wprintf(L"Elevazione UAC annullata o fallita.\n");
        ExitProcess(1);
    }
    ExitProcess(0);
}

bool AcquireAllRequiredPrivileges()
{
    bool ok = true;
    ok &= AcquirePrivilege(SE_SYSTEM_ENVIRONMENT_NAME);    // UEFI vars
    ok &= AcquirePrivilege(SE_MANAGE_VOLUME_NAME);         // raw disk I/O
    ok &= AcquirePrivilege(SE_BACKUP_NAME);                // filesystem access
    return ok;
}
