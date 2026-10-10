/*
 * Windows Start-Up Application
 *
 * Brings the session up in order: the compositor (dwm.exe), the desktop
 * (csrss.exe), then winlogon.exe. arctic-init starts it once wineserver and
 * wineboot are up and acts on how it ends: its exit code says whether the
 * machine restarts, shuts down or stops on a critical process.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <windows.h>

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(wininit);

/* exit codes, read by arctic-init */
#define EXIT_DWM_FAILED     1
#define EXIT_CSRSS_ENDED    2
#define EXIT_RESTART        3  /* winlogon.exe's own codes, passed on */
#define EXIT_SHUTDOWN       4
#define EXIT_WINLOGON_ENDED 5

static HANDLE start( const WCHAR *name )
{
    /* wininit.exe itself runs in session 0; what it starts is the session */
    STARTUPINFOW si = { .cb = sizeof(si), .lpDesktop = (WCHAR *)L"WinSta0\\Default" };
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

/* the graphics cards programs are given (powrprof, docs/power.md): before
 * csrss.exe, whose desktop draws with them, and after dwm.exe, which waited
 * for the cards' drivers; programs run on the card of the monitors */
static void publish_gpus(void)
{
    HMODULE powrprof = LoadLibraryW( L"powrprof.dll" );
    BOOL (WINAPI *publish)(void) = powrprof ? (void *)GetProcAddress( powrprof, "ArcticGpuPublish" ) : NULL;

    if (!publish || !publish()) WARN( "the graphics cards are not published\n" );
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
    HANDLE dwm, csrss, winlogon, handles[3];
    DWORD code;

    if (!(dwm = start( L"dwm.exe" )) || !wait_ready( L"__arctic_dwm_ready", dwm, 60000 )) return EXIT_DWM_FAILED;
    publish_gpus();
    if (!(csrss = start( L"csrss.exe" )) || !wait_ready( L"__arctic_desktop_ready", csrss, 60000 ))
        return EXIT_CSRSS_ENDED;
    if (!(winlogon = start( L"winlogon.exe" ))) return EXIT_WINLOGON_ENDED;

    for (;;)
    {
        handles[0] = csrss;
        handles[1] = winlogon;
        handles[2] = dwm;
        switch (WaitForMultipleObjects( 3, handles, FALSE, INFINITE ))
        {
        case WAIT_OBJECT_0 + 1:
            GetExitCodeProcess( winlogon, &code );
            if (code == EXIT_RESTART || code == EXIT_SHUTDOWN) return code;
            ERR( "winlogon.exe ended (%lu)\n", code );
            return EXIT_WINLOGON_ENDED;
        case WAIT_OBJECT_0 + 2:
            ERR( "dwm.exe ended, starting it again\n" );
            CloseHandle( dwm );
            if (!(dwm = start( L"dwm.exe" ))) return EXIT_DWM_FAILED;
            Sleep( 1000 );
            break;
        default:
            ERR( "csrss.exe ended\n" );
            return EXIT_CSRSS_ENDED;
        }
    }
}
