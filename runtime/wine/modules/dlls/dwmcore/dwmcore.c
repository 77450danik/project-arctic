/*
 * Arctic desktop composition engine
 *
 * dwm.exe hosts this library the way Windows' dwm.exe hosts dwmcore.dll.
 * The unix side owns the display (DRM/KMS); this side reads Windows
 * settings and keeps the process alive.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "wine/debug.h"

#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(dwm);

BOOL WINAPI DllMain( HINSTANCE instance, DWORD reason, void *reserved )
{
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls( instance );
    return !__wine_init_unix_call();
}

/* Takes the display and composes the desktop; returns only on failure. */
DWORD WINAPI DwmCoreRun(void)
{
    struct dwm_start_params params = { .background = GetSysColor( COLOR_DESKTOP ) };
    NTSTATUS status;

    if ((status = WINE_UNIX_CALL( unix_dwm_start, &params )))
    {
        ERR( "cannot take the display: %#lx\n", status );
        return status;
    }
    for (;;) Sleep( INFINITE );
}
