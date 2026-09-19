/*
 * Windows Logon Application
 *
 * There is one user and no logon screen: winlogon runs the Userinit
 * programs for that user at once, then stays for the keys only the system
 * handles.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <windows.h>

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(winlogon);

#define HOTKEY_TASKMGR 1

#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif

static const WCHAR winlogon_key[] = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon";

static void run( WCHAR *cmdline )
{
    STARTUPINFOW si = { .cb = sizeof(si) };
    PROCESS_INFORMATION pi;
    WCHAR dir[MAX_PATH];

    GetSystemDirectoryW( dir, MAX_PATH );
    if (!CreateProcessW( NULL, cmdline, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi ))
    {
        ERR( "cannot start %s: %lu\n", debugstr_w(cmdline), GetLastError() );
        return;
    }
    CloseHandle( pi.hThread );
    CloseHandle( pi.hProcess );
}

/* Winlogon\Userinit is a comma-separated list, "userinit.exe," by default */
static void run_userinit(void)
{
    WCHAR list[1024] = L"C:\\Windows\\system32\\userinit.exe,", *item, *next;
    DWORD size = sizeof(list);

    RegGetValueW( HKEY_LOCAL_MACHINE, winlogon_key, L"Userinit", RRF_RT_REG_SZ, NULL, list, &size );
    for (item = list; item; item = next)
    {
        if ((next = wcschr( item, ',' ))) *next++ = 0;
        while (*item == ' ') item++;
        if (*item) run( item );
    }
}

int WINAPI wWinMain( HINSTANCE instance, HINSTANCE prev, WCHAR *cmdline, int show )
{
    WCHAR taskmgr[] = L"taskmgr.exe";
    MSG msg;

    if (!RegisterHotKey( NULL, HOTKEY_TASKMGR, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_ESCAPE ))
        WARN( "Ctrl+Shift+Esc is taken: %lu\n", GetLastError() );
    run_userinit();

    while (GetMessageW( &msg, 0, 0, 0 ))
    {
        if (msg.message == WM_HOTKEY && msg.wParam == HOTKEY_TASKMGR) run( taskmgr );
        DispatchMessageW( &msg );
    }
    return 0;
}
