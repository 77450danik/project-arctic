/*
 * Arctic desktop composition engine
 *
 * dwm.exe hosts this library the way Windows' dwm.exe hosts dwmcore.dll.
 * The unix side owns the display (DRM/KMS) and serves window buffers; this
 * side reads Windows settings, asks wineserver where the windows are and
 * lends the unix side the thread in between.
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
#include <stdlib.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winreg.h"
#include "winternl.h"
#include "wine/server.h"
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

static struct composition_window *list;
static struct dwm_window           *windows;
static int                          capacity;
static UINT64                       serial = ~(UINT64)0;

static BOOL grow( int count )
{
    struct composition_window *new_list;
    struct dwm_window *new_windows;

    if (count <= capacity) return TRUE;
    count = max( count, capacity * 2 );
    if (!(new_list = realloc( list, count * sizeof(*list) ))) return FALSE;
    list = new_list;
    if (!(new_windows = realloc( windows, count * sizeof(*windows) ))) return FALSE;
    windows = new_windows;
    capacity = count;
    return TRUE;
}

/* the toplevel windows of our desktop, topmost first, as wineserver has them */
static void update_windows(void)
{
    struct dwm_set_windows_params params;
    UINT64 new_serial;
    int count, got;
    NTSTATUS status;

    for (;;)
    {
        SERVER_START_REQ( get_composition_list )
        {
            wine_server_set_reply( req, list, capacity * sizeof(*list) );
            status = wine_server_call( req );
            new_serial = reply->serial;
            count = reply->count;
            got = wine_server_reply_size( reply ) / sizeof(*list);
        }
        SERVER_END_REQ;
        if (status || count <= got || !grow( count )) break;
    }
    if (status || new_serial == serial) return;
    serial = new_serial;

    for (int i = 0; i < got; i++)
    {
        windows[i].hwnd     = list[i].handle;
        windows[i].style    = list[i].style;
        windows[i].ex_style = list[i].ex_style;
        windows[i].left     = list[i].visible_rect.left;
        windows[i].top      = list[i].visible_rect.top;
        windows[i].right    = list[i].visible_rect.right;
        windows[i].bottom   = list[i].visible_rect.bottom;
    }
    params.count = got;
    params.windows = windows;
    WINE_UNIX_CALL( unix_dwm_set_windows, &params );
}

/* Takes the display and composes the desktop; returns only on failure. */
DWORD WINAPI DwmCoreRun(void)
{
    struct dwm_start_params params = { .background = desktop_color() };
    struct dwm_dispatch_params dispatch = { .timeout_ms = 8 };
    NTSTATUS status;

    if ((status = WINE_UNIX_CALL( unix_dwm_start, &params )))
    {
        ERR( "cannot take the display: %#lx\n", status );
        return status;
    }
    grow( 256 );
    for (;;)
    {
        if ((status = WINE_UNIX_CALL( unix_dwm_dispatch, &dispatch ))) return status;
        update_windows();
    }
}
