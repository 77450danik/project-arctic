/*
 * Arctic desktop composition engine
 *
 * dwm.exe hosts this library the way Windows' dwm.exe hosts dwmcore.dll.
 * The unix side owns the display (DRM/KMS) and serves window buffers; this
 * side reads Windows settings and lends it the thread.
 *
 * user32 is deliberately not used: its first window-related call would
 * start the desktop and the display driver before the compositor serves.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <stdio.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winreg.h"
#include "wine/debug.h"

#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(dwm);

BOOL WINAPI DllMain( HINSTANCE instance, DWORD reason, void *reserved )
{
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls( instance );
    return !__wine_init_unix_call();
}

/* HKCU\Control Panel\Colors\Background holds "R G B", as in Windows */
static COLORREF desktop_color(void)
{
    WCHAR value[32];
    DWORD size = sizeof(value);
    int r, g, b;

    if (!RegGetValueW( HKEY_CURRENT_USER, L"Control Panel\\Colors", L"Background", RRF_RT_REG_SZ,
                       NULL, value, &size ) &&
        swscanf( value, L"%d %d %d", &r, &g, &b ) == 3)
        return RGB( r, g, b );
    return RGB( 58, 110, 165 );
}

/* Takes the display and composes the desktop; returns only on failure. */
DWORD WINAPI DwmCoreRun(void)
{
    struct dwm_start_params params = { .background = desktop_color() };
    NTSTATUS status;

    if ((status = WINE_UNIX_CALL( unix_dwm_start, &params )))
    {
        ERR( "cannot take the display: %#lx\n", status );
        return status;
    }
    return WINE_UNIX_CALL( unix_dwm_run, NULL );
}
