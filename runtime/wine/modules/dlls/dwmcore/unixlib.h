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
#include "wine/arctic_dwm.h"

#define DWM_MAX_OUTPUTS 8
#define DWM_MAX_MODES   160
#define DWM_MAX_EDID    2048

struct dwm_start_params
{
    UINT32 background;  /* COLORREF of the desktop */
};

/* what happened while dispatching, for the Windows side to act on */
#define DWM_EVENT_HOTPLUG 0x1  /* monitors came or went: they need a layout */
#define DWM_EVENT_SAVE    0x2  /* a program applied a layout to be kept */

struct dwm_dispatch_params
{
    UINT32 timeout_ms;
    UINT32 events;      /* out: DWM_EVENT_* */
};

/* a toplevel window as wineserver sees it */
struct dwm_window
{
    UINT32 hwnd;
    UINT32 style;
    UINT32 ex_style;
    INT32  left, top, right, bottom;  /* visible rectangle, screen coordinates */
    INT32  client_left, client_top;   /* where the client area starts, screen coordinates */
    INT32  client_right, client_bottom;
    UINT32 owner;
    UINT32 class_atom;                /* 0x8000: a menu */
    UINT32 band;                      /* the taskbar's band, over program windows */
    UINT32 dpi;                       /* DPI of the window's own coordinates */
    UINT32 raw_dpi;                   /* DPI of its monitor's pixels, which the rectangles above are in */
};

struct dwm_set_windows_params
{
    UINT32                   count;
    const struct dwm_window *windows;  /* topmost first */
    UINT32                   snap_window;  /* dragged to a screen edge (Aero Snap), 0 if none */
    INT32                    snap_left, snap_top, snap_right, snap_bottom;  /* where it would go */
};

struct dwm_set_cursor_params
{
    INT32  x, y;    /* screen coordinates */
    UINT32 hidden;  /* the window under it hides it */
};

#define DWM_MODE_PREFERRED 0x1  /* the monitor's native mode */

struct dwm_mode
{
    UINT32 width, height;
    UINT32 refresh;     /* mHz */
    UINT32 flags;       /* DWM_MODE_* */
};

/* a connected monitor */
struct dwm_output
{
    UINT32          id;           /* connector id */
    char            name[32];     /* connector, e.g. HDMI-A-1 */
    UINT32          internal;     /* a laptop's own panel */
    UINT32          enabled;
    INT32           x, y;         /* in the virtual screen, when enabled */
    UINT32          mode;         /* index into modes, when enabled */
    UINT32          mode_count;
    struct dwm_mode modes[DWM_MAX_MODES];
    UINT32          edid_len;
    BYTE            edid[DWM_MAX_EDID];
    UINT32          vrr_capable;  /* the monitor and the card take a variable refresh */
};

struct dwm_get_outputs_params
{
    UINT32             count;     /* out */
    struct dwm_output *outputs;   /* DWM_MAX_OUTPUTS of them */
};

/* what one monitor should show */
struct dwm_output_config
{
    UINT32 id;
    UINT32 enabled;
    INT32  x, y;
    UINT32 width, height;
    UINT32 refresh;     /* mHz */
};

struct dwm_set_config_params
{
    UINT32                          count;
    const struct dwm_output_config *configs;  /* monitors not named keep theirs */
};

struct dwm_set_options_params
{
    UINT32 vrr;         /* variable refresh for games that fill a monitor */
    UINT32 vrr_off_count;
    UINT32 vrr_off[DWM_MAX_OUTPUTS];  /* connectors whose monitor the user turned it off for */
};

/* what programs asked of their windows through dwmapi (wine/arctic_dwm.h) */
struct dwm_set_attributes_params
{
    UINT32                         count;
    const struct arctic_dwm_entry *entries;  /* the rows in use */
};

enum dwm_funcs
{
    unix_dwm_start,        /* take the display, start serving buffers */
    unix_dwm_dispatch,     /* serve clients and compose, for up to timeout_ms */
    unix_dwm_set_windows,  /* the toplevel windows, from wineserver */
    unix_dwm_set_cursor,   /* the cursor position, from wineserver */
    unix_dwm_get_outputs,  /* the connected monitors with their modes */
    unix_dwm_set_config,   /* modes and positions of the monitors */
    unix_dwm_set_options,  /* Windows settings the compositor follows */
    unix_dwm_set_attributes, /* window attributes and thumbnails from dwmapi */
    unix_funcs_count
};

#endif
