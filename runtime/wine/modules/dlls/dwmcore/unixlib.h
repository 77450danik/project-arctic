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

enum dwm_funcs
{
    unix_dwm_start,
    unix_funcs_count
};

#endif
