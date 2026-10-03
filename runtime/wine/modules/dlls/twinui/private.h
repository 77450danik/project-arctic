/*
 * Task switching of the shell: Alt+Tab and the taskbar's thumbnails
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __TWINUI_PRIVATE_H
#define __TWINUI_PRIVATE_H

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "dwmapi.h"
#include "shobjidl.h"
#include "wine/arctic_dwm.h"
#include "wine/arctic_taskbar.h"

/* the dark acrylic of the shell in Windows 10: its tint, 0xAABBGGRR */
#define SHELL_ACRYLIC_TINT 0xcc1f1f1f

extern HINSTANCE twinui_instance;

/* main.c */
void set_acrylic( HWND hwnd );
void set_transition( HWND hwnd, DWORD transition );
void set_cloaked( HWND hwnd, BOOL cloaked );
int px( int n );
HFONT shell_font( int height, int weight );
HICON window_icon( HWND hwnd, BOOL big );
BOOL is_task_window( HWND hwnd );
void fill_alpha( HDC hdc, const RECT *rect, COLORREF color, BYTE alpha );
void draw_glyph_close( HDC hdc, const RECT *rect, COLORREF color );
struct arctic_taskbar_shared *get_taskbar_shared(void);
const struct arctic_taskbar_window *find_taskbar_row( const struct arctic_taskbar_shared *shared, HWND hwnd );

/* switcher.c */
BOOL switcher_start(void);

/* flyout.c */
void flyout_hover( const HWND *hwnds, UINT count, const RECT *button, DWORD flags );

#endif
