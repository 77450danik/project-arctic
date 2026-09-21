/*
 * Arctic Desktop Window Manager
 *
 * Started by wininit.exe; lives as long as the session. All the work is
 * done by dwmcore.dll, as in Windows.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <windows.h>

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(dwm);

int WINAPI wWinMain( HINSTANCE instance, HINSTANCE prev, WCHAR *cmdline, int show )
{
    HMODULE core;
    DWORD (WINAPI *run)(void);

    /* A process joins the desktop it was started on when user32 sets it up;
     * dwm composes the windows of the session's desktop, which wininit.exe
     * named, not of the session 0 desktop it would otherwise inherit. */
    if (!GetProcessWindowStation()) ERR( "no window station: %lu\n", GetLastError() );

    core = LoadLibraryW( L"dwmcore.dll" );
    run = core ? (void *)GetProcAddress( core, "DwmCoreRun" ) : NULL;

    if (!run)
    {
        ERR( "dwmcore.dll is missing\n" );
        return 1;
    }
    return run();
}
