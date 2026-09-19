/*
 * Client Server Runtime Process
 *
 * Owns the desktop through winsrv.dll, as in Windows, and dispatches the
 * messages of the desktop window. Started by arctic-init, or by win32u
 * when a program needs the desktop before it exists.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include <windows.h>
#include <winternl.h>

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(csrss);

int WINAPI wWinMain( HINSTANCE instance, HINSTANCE prev, WCHAR *cmdline, int show )
{
    HMODULE winsrv = LoadLibraryW( L"winsrv.dll" );
    NTSTATUS (WINAPI *init)(void *) = winsrv ? (void *)GetProcAddress( winsrv, "UserServerDllInitialization" ) : NULL;
    NTSTATUS status;
    MSG msg;

    if (!init)
    {
        ERR( "winsrv.dll is missing\n" );
        return 1;
    }
    if ((status = init( NULL ))) return status == STATUS_OBJECT_NAME_EXISTS ? 0 : 1;
    while (GetMessageW( &msg, 0, 0, 0 )) DispatchMessageW( &msg );
    return 0;
}
