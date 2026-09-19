/*
 * Windows Start-Up Application
 *
 * Brings the session up in order: the compositor (dwm.exe), the desktop
 * (csrss.exe), then winlogon.exe. arctic-init starts it once wineserver and
 * wineboot are up and stops the system when it ends, so it ends only when
 * the session cannot go on.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <windows.h>

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(wininit);

static HANDLE start( const WCHAR *name )
{
    STARTUPINFOW si = { .cb = sizeof(si) };
    PROCESS_INFORMATION pi;
    WCHAR path[MAX_PATH];

    GetSystemDirectoryW( path, MAX_PATH );
    lstrcatW( path, L"\\" );
    lstrcatW( path, name );
    if (!CreateProcessW( path, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi ))
    {
        ERR( "cannot start %s: %lu\n", debugstr_w(name), GetLastError() );
        return NULL;
    }
    CloseHandle( pi.hThread );
    return pi.hProcess;
}

/* the process sets the named event once it serves; FALSE if it ended first */
static BOOL wait_ready( const WCHAR *event_name, HANDLE process, DWORD timeout )
{
    HANDLE handles[2] = { CreateEventW( NULL, TRUE, FALSE, event_name ), process };
    DWORD ret = WaitForMultipleObjects( 2, handles, FALSE, timeout );

    CloseHandle( handles[0] );
    if (ret == WAIT_TIMEOUT) WARN( "%s: no signal after %lu ms\n", debugstr_w(event_name), timeout );
    return ret != WAIT_OBJECT_0 + 1 && ret != WAIT_FAILED;
}

int WINAPI wWinMain( HINSTANCE instance, HINSTANCE prev, WCHAR *cmdline, int show )
{
    HANDLE dwm, csrss, winlogon, handles[2];

    if (!(dwm = start( L"dwm.exe" )) || !wait_ready( L"__arctic_dwm_ready", dwm, 10000 )) return 1;
    if (!(csrss = start( L"csrss.exe" )) || !wait_ready( L"__arctic_desktop_ready", csrss, 30000 )) return 2;
    if ((winlogon = start( L"winlogon.exe" ))) CloseHandle( winlogon );

    for (;;)
    {
        handles[0] = csrss;
        handles[1] = dwm;
        switch (WaitForMultipleObjects( 2, handles, FALSE, INFINITE ))
        {
        case WAIT_OBJECT_0 + 1:
            ERR( "dwm.exe ended, starting it again\n" );
            CloseHandle( dwm );
            if (!(dwm = start( L"dwm.exe" ))) return 1;
            Sleep( 1000 );
            break;
        default:
            ERR( "csrss.exe ended\n" );
            return 2;
        }
    }
}
