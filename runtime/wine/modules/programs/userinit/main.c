/*
 * Userinit Logon Application
 *
 * Runs once per logon: starts the shell named in Winlogon\Shell (the
 * user's value first, then the machine's) in the user's profile, and ends.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <windows.h>

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(userinit);

static const WCHAR winlogon_key[] = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon";

int WINAPI wWinMain( HINSTANCE instance, HINSTANCE prev, WCHAR *cmdline, int show )
{
    WCHAR shell[MAX_PATH] = L"explorer.exe", profile[MAX_PATH];
    STARTUPINFOW si = { .cb = sizeof(si) };
    PROCESS_INFORMATION pi;
    DWORD size = sizeof(shell);

    if (RegGetValueW( HKEY_CURRENT_USER, winlogon_key, L"Shell", RRF_RT_REG_SZ, NULL, shell, &size ))
    {
        size = sizeof(shell);
        RegGetValueW( HKEY_LOCAL_MACHINE, winlogon_key, L"Shell", RRF_RT_REG_SZ, NULL, shell, &size );
    }
    if (!GetEnvironmentVariableW( L"USERPROFILE", profile, MAX_PATH )) GetWindowsDirectoryW( profile, MAX_PATH );

    if (!CreateProcessW( NULL, shell, NULL, NULL, FALSE, 0, NULL, profile, &si, &pi ))
    {
        ERR( "cannot start the shell %s: %lu\n", debugstr_w(shell), GetLastError() );
        return 1;
    }
    CloseHandle( pi.hThread );
    CloseHandle( pi.hProcess );
    return 0;
}
