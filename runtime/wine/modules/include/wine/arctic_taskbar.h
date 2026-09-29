/*
 * What programs put on their taskbar buttons (ITaskbarList3)
 *
 * Progress, an overlay icon and the buttons under the thumbnail belong to
 * the taskbar, which lives in explorer.exe. explorerframe.dll, in the
 * program's process, writes them into a table in a shared section; the
 * taskbar and twinui.dll (the thumbnail) read it. Icons travel as pixels:
 * a handle means nothing in another process.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_ARCTIC_TASKBAR_H
#define __WINE_ARCTIC_TASKBAR_H

#define ARCTIC_TASKBAR_SECTION L"Global\\__arctic_taskbar"
#define ARCTIC_TASKBAR_VERSION 1
#define ARCTIC_TASKBAR_WINDOWS 64
#define ARCTIC_THUMB_BUTTONS   7     /* as many as Windows takes */
#define ARCTIC_TASKBAR_ICON    16    /* icons are kept at this size, premultiplied ARGB */

/* registered messages to the taskbar's task list (MSTaskSwWClass):
 * wparam is the window whose button changed */
#define ARCTIC_TASKBAR_CHANGED L"ArcticTaskbarChanged"
/* ITaskbarList::AddTab (lparam 1) and DeleteTab (lparam 0) */
#define ARCTIC_TASKBAR_TAB     L"ArcticTaskbarTab"

struct arctic_thumb_button
{
    UINT32 id;
    UINT32 flags;                    /* THBF_* */
    UINT32 has_icon;
    UINT32 icon[ARCTIC_TASKBAR_ICON * ARCTIC_TASKBAR_ICON];
    WCHAR  tip[64];
};

struct arctic_taskbar_window
{
    UINT32 hwnd;                     /* 0: a free row */
    UINT32 process;
    UINT32 progress_state;           /* TBPF_* */
    UINT32 progress;                 /* 0-10000 */
    UINT32 has_overlay;
    UINT32 overlay[ARCTIC_TASKBAR_ICON * ARCTIC_TASKBAR_ICON];
    UINT32 button_count;
    struct arctic_thumb_button buttons[ARCTIC_THUMB_BUTTONS];
};

struct arctic_taskbar_shared
{
    UINT32        version;
    volatile LONG lock;
    volatile LONG serial;            /* moves after every change */
    UINT32        reserved;
    struct arctic_taskbar_window windows[ARCTIC_TASKBAR_WINDOWS];
};

#endif  /* __WINE_ARCTIC_TASKBAR_H */
