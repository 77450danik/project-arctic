/*
 * What programs tell dwm.exe about their windows
 *
 * In Windows, dwmapi.dll talks to the Desktop Window Manager over ALPC. In
 * Arctic, dwm.exe keeps a table in a shared section instead: dwmapi.dll and
 * user32.dll write window attributes (DwmSetWindowAttribute,
 * SetWindowCompositionAttribute) and thumbnails (DwmRegisterThumbnail) into
 * it, and dwm.exe reads it whenever the serial moves. Rows of windows that
 * are gone are dropped by dwm.exe.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_ARCTIC_DWM_H
#define __WINE_ARCTIC_DWM_H

#define ARCTIC_DWM_SECTION     L"Global\\__arctic_dwm"
#define ARCTIC_DWM_VERSION     1
#define ARCTIC_DWM_MAX_ENTRIES 512

/* how a window comes and goes, beyond what dwm.exe picks from its style */
#define ARCTIC_TRANSITION_DEFAULT  0
#define ARCTIC_TRANSITION_NONE     1
#define ARCTIC_TRANSITION_FADE     2  /* opacity only */
#define ARCTIC_TRANSITION_SLIDE_UP 3  /* from below, as the Start menu */

/* the private DWMWINDOWATTRIBUTE that sets it */
#define DWMWA_ARCTIC_TRANSITION    0x1000
/* the private DWMWINDOWATTRIBUTE the taskbar sets on each window: the screen
 * rectangle of its button, where the window goes when it is minimized */
#define DWMWA_ARCTIC_MINIMIZE_RECT 0x1001
/* the private DWMWINDOWATTRIBUTE through which user32 hands over
 * SetWindowCompositionAttribute(WCA_ACCENT_POLICY): an ACCENT_POLICY */
#define DWMWA_ARCTIC_ACCENT        0x1002

struct arctic_accent_policy
{
    UINT32 state;            /* ARCTIC_ACCENT_* */
    UINT32 flags;
    UINT32 gradient_color;   /* 0xAABBGGRR */
    UINT32 animation_id;
};

/* ACCENT_STATE of SetWindowCompositionAttribute(WCA_ACCENT_POLICY) */
#define ARCTIC_ACCENT_DISABLED                 0
#define ARCTIC_ACCENT_ENABLE_GRADIENT          1
#define ARCTIC_ACCENT_ENABLE_TRANSPARENTGRADIENT 2
#define ARCTIC_ACCENT_ENABLE_BLURBEHIND        3
#define ARCTIC_ACCENT_ENABLE_ACRYLICBLURBEHIND 4
#define ARCTIC_ACCENT_ENABLE_HOSTBACKDROP      5

enum arctic_dwm_entry_type
{
    ARCTIC_DWM_FREE,
    ARCTIC_DWM_WINDOW,      /* attributes of one window */
    ARCTIC_DWM_THUMBNAIL,   /* a live picture of a window in another one */
};

struct arctic_dwm_window
{
    UINT32 accent;           /* ARCTIC_ACCENT_* */
    UINT32 accent_color;     /* 0xAABBGGRR, of the accent policy */
    UINT32 backdrop;         /* DWMSBT_*, DWMWA_SYSTEMBACKDROP_TYPE */
    UINT32 blur_behind;      /* DwmEnableBlurBehindWindow */
    UINT32 corner;           /* DWMWCP_*, DWMWA_WINDOW_CORNER_PREFERENCE */
    UINT32 transitions_off;  /* DWMWA_TRANSITIONS_FORCEDISABLED */
    UINT32 transition;       /* ARCTIC_TRANSITION_* */
    UINT32 cloaked;          /* DWMWA_CLOAK */
    INT32  minimize_left, minimize_top, minimize_right, minimize_bottom;  /* empty: none */
};

struct arctic_dwm_thumbnail
{
    UINT32 source;
    INT32  dest_left, dest_top, dest_right, dest_bottom;          /* client coordinates of the destination */
    INT32  source_left, source_top, source_right, source_bottom;  /* of the source window; empty: all of it */
    UINT32 opacity;          /* 0-255 */
    UINT32 visible;
    UINT32 client_only;      /* the source's client area only */
};

struct arctic_dwm_entry
{
    UINT32 type;             /* enum arctic_dwm_entry_type */
    UINT32 hwnd;             /* the window; for a thumbnail, the one it is drawn in */
    UINT32 id;               /* HTHUMBNAIL of a thumbnail */
    UINT32 process;          /* who wrote it */
    union
    {
        struct arctic_dwm_window    window;
        struct arctic_dwm_thumbnail thumbnail;
    } u;
};

struct arctic_dwm_shared
{
    UINT32          version;
    volatile LONG   lock;    /* a spin lock around every change */
    volatile LONG   serial;  /* moves after every change */
    UINT32          next_id;
    struct arctic_dwm_entry entries[ARCTIC_DWM_MAX_ENTRIES];
};

/* Screenshots: the desktop as dwm.exe composes it (PrintScreen, BitBlt from
 * the screen). A client takes the lock, creates the section with the
 * request and room for the pixels, sets the request event and waits for the
 * done event; dwm.exe fills the pixels and closes its view before it says
 * so. dwm.exe owns the events. */
#define ARCTIC_CAPTURE_LOCK    L"Global\\__arctic_dwm_capture_lock"
#define ARCTIC_CAPTURE_SECTION L"Global\\__arctic_dwm_capture"
#define ARCTIC_CAPTURE_REQUEST L"Global\\__arctic_dwm_capture_request"
#define ARCTIC_CAPTURE_DONE    L"Global\\__arctic_dwm_capture_done"

struct arctic_capture
{
    LONG   x, y, width, height;  /* in the virtual screen, in its pixels */
    LONG   status;               /* 0 once the pixels are there */
    UINT32 pixels[1];            /* width * height, BGRA, top row first, opaque */
};

#endif  /* __WINE_ARCTIC_DWM_H */
