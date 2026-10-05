/*
 * The Start menu: the user's picture over its top edge
 *
 * A circle of 64 over the right column, half above the menu, as on the
 * reference picture: grey with the white figure of a person, ringed by
 * #FFFFFF at 40%. The menu cannot draw outside itself, so it is a layered
 * window of its own, owned by the menu and never activated; a click opens
 * the user's folder.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "startui.h"
#include "dwmapi.h"
#include "wine/arctic_dwm.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(startui);

#define AVATAR_SIZE  px(64)
#define RING         px(2)

static HWND avatar, avatar_owner;

HWND avatar_window(void)
{
    return avatar;
}

static LRESULT WINAPI avatar_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    switch (msg)
    {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_LBUTTONUP:
        if (avatar_owner) PostMessageW( avatar_owner, WM_APP, 0, 0 );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

static void draw( HDC hdc, int size )
{
    RECT rect;
    int glyph = size * 5 / 8;

    SetRect( &rect, 0, 0, size, size );
    fill_round_rect( hdc, &rect, size / 2.0f, RGB( 255, 255, 255 ), 0x66 );
    InflateRect( &rect, -RING, -RING );
    fill_round_rect( hdc, &rect, (size - 2 * RING) / 2.0f, RGB( 0x5a, 0x5a, 0x5a ), 255 );
    draw_glyph( hdc, GLYPH_PERSON, (size - glyph) / 2, (size - glyph) / 2 + size / 32, glyph, RGB( 255, 255, 255 ), 255 );
}

void avatar_show( HWND owner, int center_x, int center_y )
{
    BITMAPINFO info = { { sizeof(info.bmiHeader), 0, 0, 1, 32, BI_RGB } };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    int size = AVATAR_SIZE;
    POINT pos = { center_x - size / 2, center_y - size / 2 }, src = { 0, 0 };
    SIZE extent = { size, size };
    HDC screen, mem;
    HBITMAP bitmap;
    void *bits;

    if (!avatar || avatar_owner != owner)
    {
        WNDCLASSW cls = { 0 };
        DWORD transition = ARCTIC_TRANSITION_SLIDE_UP;

        if (avatar) DestroyWindow( avatar );
        cls.lpfnWndProc = avatar_proc;
        cls.hInstance = startui_instance;
        cls.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_HAND );
        cls.lpszClassName = L"ArcticStartMenuAvatar";
        RegisterClassW( &cls );
        avatar = CreateWindowExW( WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                                  cls.lpszClassName, L"", WS_POPUP, pos.x, pos.y, size, size, owner, NULL,
                                  startui_instance, NULL );
        if (!avatar) return;
        avatar_owner = owner;
        DwmSetWindowAttribute( avatar, DWMWA_ARCTIC_TRANSITION, &transition, sizeof(transition) );
    }

    SetWindowPos( avatar, HWND_TOPMOST, pos.x, pos.y, size, size, SWP_NOACTIVATE | SWP_SHOWWINDOW );
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    screen = GetDC( NULL );
    mem = CreateCompatibleDC( screen );
    if ((bitmap = CreateDIBSection( screen, &info, DIB_RGB_COLORS, &bits, NULL, 0 )))
    {
        HGDIOBJ old = SelectObject( mem, bitmap );

        memset( bits, 0, size * size * 4 );
        draw( mem, size );
        if (!UpdateLayeredWindow( avatar, screen, &pos, &extent, mem, &src, 0, &blend, ULW_ALPHA ))
            WARN( "UpdateLayeredWindow failed: %lu\n", GetLastError() );
        SelectObject( mem, old );
        DeleteObject( bitmap );
    }
    DeleteDC( mem );
    ReleaseDC( NULL, screen );
}

void avatar_hide(void)
{
    if (avatar) ShowWindow( avatar, SW_HIDE );
}
