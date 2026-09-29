/*
 * Task switching of the shell
 *
 * In Windows 10, Alt+Tab and the thumbnails over taskbar buttons are
 * explorer.exe's, drawn by twinui.dll with live pictures of the windows
 * from DWM. Here explorer.exe loads this twinui.dll the same way: it starts
 * the task switcher and tells it which button the pointer is over.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(twinui);

HINSTANCE twinui_instance;

BOOL WINAPI SetWindowCompositionAttribute( HWND hwnd, void *data );

BOOL WINAPI DllMain( HINSTANCE instance, DWORD reason, void *reserved )
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        twinui_instance = instance;
        DisableThreadLibraryCalls( instance );
    }
    return TRUE;
}

/***********************************************************************
 *              ArcticStartTaskSwitcher (TWINUI.@)
 *
 * Alt+Tab from now on, as long as the calling process lives.
 */
BOOL WINAPI ArcticStartTaskSwitcher(void)
{
    return switcher_start();
}

/***********************************************************************
 *              ArcticTaskbarHover (TWINUI.@)
 *
 * The pointer is over the taskbar button of task (NULL: over none), whose
 * screen rectangle is button. flags 1: at once, without the usual delay.
 */
void WINAPI ArcticTaskbarHover( HWND task, const RECT *button, DWORD flags )
{
    flyout_hover( task, button, flags );
}

/* the shell's dark acrylic behind the window, as the taskbar has */
void set_acrylic( HWND hwnd )
{
    struct arctic_accent_policy policy = { ARCTIC_ACCENT_ENABLE_ACRYLICBLURBEHIND, 0, SHELL_ACRYLIC_TINT, 0 };
    struct { DWORD attrib; void *data; SIZE_T size; } attr = { 19 /* WCA_ACCENT_POLICY */, &policy, sizeof(policy) };

    SetWindowCompositionAttribute( hwnd, &attr );
}

void set_transition( HWND hwnd, DWORD transition )
{
    DwmSetWindowAttribute( hwnd, DWMWA_ARCTIC_TRANSITION, &transition, sizeof(transition) );
}

void set_cloaked( HWND hwnd, BOOL cloaked )
{
    DwmSetWindowAttribute( hwnd, DWMWA_CLOAK, &cloaked, sizeof(cloaked) );
}

/* the message font of the theme, at a height of our own */
HFONT shell_font( int height, int weight )
{
    NONCLIENTMETRICSW metrics = { sizeof(metrics) };

    SystemParametersInfoW( SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0 );
    metrics.lfMessageFont.lfHeight = -height;
    metrics.lfMessageFont.lfWeight = weight;
    metrics.lfMessageFont.lfQuality = CLEARTYPE_QUALITY;
    return CreateFontIndirectW( &metrics.lfMessageFont );
}

HICON window_icon( HWND hwnd, BOOL big )
{
    DWORD_PTR icon = 0;

    if (SendMessageTimeoutW( hwnd, WM_GETICON, big ? ICON_BIG : ICON_SMALL2, 0, SMTO_ABORTIFHUNG, 100, &icon ) &&
        icon)
        return (HICON)icon;
    if (!big && SendMessageTimeoutW( hwnd, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 100, &icon ) && icon)
        return (HICON)icon;
    if ((icon = GetClassLongPtrW( hwnd, big ? GCLP_HICON : GCLP_HICONSM ))) return (HICON)icon;
    if ((icon = GetClassLongPtrW( hwnd, big ? GCLP_HICONSM : GCLP_HICON ))) return (HICON)icon;
    return LoadIconW( NULL, (const WCHAR *)IDI_APPLICATION );
}

/* The windows Alt+Tab and the taskbar offer, as Windows picks them: a
 * window stands for its owner chain when it is the last active popup of it
 * that is visible; tool windows and cloaked windows are left out. */
BOOL is_task_window( HWND hwnd )
{
    LONG ex_style = GetWindowLongW( hwnd, GWL_EXSTYLE );
    HWND walk, next;
    WCHAR name[64];
    DWORD cloaked = 0;
    RECT rect;

    if (!IsWindowVisible( hwnd ) || hwnd == GetShellWindow()) return FALSE;
    if (!(ex_style & WS_EX_APPWINDOW))
    {
        if (ex_style & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return FALSE;
        walk = GetAncestor( hwnd, GA_ROOTOWNER );
        while ((next = GetLastActivePopup( walk )) != walk)
        {
            if (IsWindowVisible( next )) break;
            walk = next;
        }
        if (walk != hwnd) return FALSE;
    }
    if (SUCCEEDED(DwmGetWindowAttribute( hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked) )) && cloaked) return FALSE;
    GetClassNameW( hwnd, name, ARRAY_SIZE(name) );
    if (!wcscmp( name, L"Shell_TrayWnd" ) || !wcscmp( name, L"Progman" ) || !wcscmp( name, L"WorkerW" ) ||
        !wcsncmp( name, L"Arctic", 6 ))
        return FALSE;
    if (!IsIconic( hwnd ) && GetWindowRect( hwnd, &rect ) && (rect.right <= rect.left || rect.bottom <= rect.top))
        return FALSE;
    return TRUE;
}

/* a colour with its own alpha, which dwm keeps over the backdrop */
void fill_alpha( HDC hdc, const RECT *rect, COLORREF color, BYTE alpha )
{
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    BITMAPINFO info = { { sizeof(info.bmiHeader), 1, 1, 1, 32, BI_RGB } };
    DWORD *bits, pixel;
    HDC mem = CreateCompatibleDC( hdc );
    HBITMAP bitmap = CreateDIBSection( hdc, &info, DIB_RGB_COLORS, (void **)&bits, NULL, 0 );
    HGDIOBJ old;

    if (!bitmap)
    {
        DeleteDC( mem );
        return;
    }
    pixel = ((DWORD)alpha << 24) | ((GetRValue( color ) * alpha / 255) << 16) |
            ((GetGValue( color ) * alpha / 255) << 8) | (GetBValue( color ) * alpha / 255);
    *bits = pixel;
    old = SelectObject( mem, bitmap );
    GdiAlphaBlend( hdc, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, mem, 0, 0, 1, 1,
                   blend );
    SelectObject( mem, old );
    DeleteObject( bitmap );
    DeleteDC( mem );
}

/* the X of Windows 10: two strokes, a pixel wide, 10 pixels across */
void draw_glyph_close( HDC hdc, const RECT *rect, COLORREF color )
{
    int cx = (rect->left + rect->right) / 2, cy = (rect->top + rect->bottom) / 2;
    HPEN pen = CreatePen( PS_SOLID, 1, color ), old = SelectObject( hdc, pen );

    MoveToEx( hdc, cx - 5, cy - 5, NULL );
    LineTo( hdc, cx + 5, cy + 5 );
    MoveToEx( hdc, cx + 4, cy - 5, NULL );
    LineTo( hdc, cx - 6, cy + 5 );
    SelectObject( hdc, old );
    DeleteObject( pen );
}
