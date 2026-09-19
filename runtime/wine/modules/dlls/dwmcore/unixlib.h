/*
 * Arctic desktop composition engine: calls into the unix side
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_DWMCORE_UNIXLIB_H
#define __WINE_DWMCORE_UNIXLIB_H

#include "wine/unixlib.h"

struct dwm_start_params
{
    UINT32 background;  /* COLORREF of the desktop */
};

struct dwm_dispatch_params
{
    UINT32 timeout_ms;
};

/* a toplevel window as wineserver sees it */
struct dwm_window
{
    UINT32 hwnd;
    UINT32 style;
    UINT32 ex_style;
    INT32  left, top, right, bottom;  /* visible rectangle, screen coordinates */
};

struct dwm_set_windows_params
{
    UINT32                   count;
    const struct dwm_window *windows;  /* topmost first */
};

struct dwm_set_cursor_params
{
    INT32 x, y;  /* screen coordinates */
};

enum dwm_funcs
{
    unix_dwm_start,        /* take the display, start serving buffers */
    unix_dwm_dispatch,     /* serve clients and compose, for up to timeout_ms */
    unix_dwm_set_windows,  /* the toplevel windows, from wineserver */
    unix_dwm_set_cursor,   /* the cursor position, from wineserver */
    unix_funcs_count
};

#endif
