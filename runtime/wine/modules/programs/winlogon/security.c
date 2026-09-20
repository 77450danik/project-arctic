/*
 * Windows Logon Application: the security options screen (Ctrl+Alt+Del)
 *
 * A full-screen page in the accent colour with the options in a column and
 * a Cancel button, as in Windows 10. It belongs to winlogon.exe, which the
 * system cannot run without.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <windows.h>

#include "wine/debug.h"

#include "winlogon.h"

WINE_DEFAULT_DEBUG_CHANNEL(winlogon);

enum item { ITEM_COMMAND_PROMPT, ITEM_TASK_MANAGER, ITEM_RESTART, ITEM_SHUT_DOWN, ITEM_CANCEL, ITEM_COUNT };

static const UINT item_text_ids[ITEM_COUNT] =
{
    IDS_COMMAND_PROMPT, IDS_TASK_MANAGER, IDS_RESTART, IDS_SHUT_DOWN, IDS_CANCEL
};

static const WCHAR class_name[] = L"Arctic security options";

static struct
{
    HWND     hwnd;
    HWND     previous;            /* the foreground window before */
    HFONT    font;
    COLORREF background;
    WCHAR    text[ITEM_COUNT][64];
    RECT     rects[ITEM_COUNT];
    int      hot;                 /* under the pointer, or -1 */
    int      pressed;             /* left button went down on it, or -1 */
    int      focus;               /* keyboard selection */
    BOOL     focus_shown;         /* only once the keyboard is used, as in Windows */
    BOOL     tracking;
} screen = { .hot = -1, .pressed = -1 };

/* HKCU\Software\Microsoft\Windows\DWM\AccentColor is 0xAABBGGRR */
static COLORREF accent_color(void)
{
    DWORD value, size = sizeof(value);

    if (!RegGetValueW( HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor",
                       RRF_RT_REG_DWORD, NULL, &value, &size ))
        return value & 0xffffff;
    return RGB( 0, 120, 215 );
}

static COLORREF blend( COLORREF color, int white_percent )
{
    return RGB( GetRValue(color) + (255 - GetRValue(color)) * white_percent / 100,
                GetGValue(color) + (255 - GetGValue(color)) * white_percent / 100,
                GetBValue(color) + (255 - GetBValue(color)) * white_percent / 100 );
}

/* sizes follow the screen height, as the page is laid out for 1080 lines */
static int scaled( int height, int value_at_1080 )
{
    return max( 1, MulDiv( value_at_1080, height, 1080 ) );
}

static void layout( int width, int height )
{
    HDC dc = GetDC( screen.hwnd );
    HFONT old = SelectObject( dc, screen.font );
    int pad_x = scaled( height, 16 ), item_h = scaled( height, 48 ), column_w = 0, top;
    SIZE size;

    for (int i = 0; i < ITEM_CANCEL; i++)
    {
        GetTextExtentPoint32W( dc, screen.text[i], lstrlenW( screen.text[i] ), &size );
        column_w = max( column_w, size.cx + 2 * pad_x );
    }
    top = (height - ITEM_CANCEL * item_h) / 2;
    for (int i = 0; i < ITEM_CANCEL; i++)
        SetRect( &screen.rects[i], (width - column_w) / 2, top + i * item_h,
                 (width + column_w) / 2, top + (i + 1) * item_h );

    GetTextExtentPoint32W( dc, screen.text[ITEM_CANCEL], lstrlenW( screen.text[ITEM_CANCEL] ), &size );
    size.cx = max( size.cx + 2 * pad_x, scaled( height, 140 ) );
    SetRect( &screen.rects[ITEM_CANCEL], (width - size.cx) / 2, height - scaled( height, 160 ),
             (width + size.cx) / 2, height - scaled( height, 160 ) + scaled( height, 44 ) );

    SelectObject( dc, old );
    ReleaseDC( screen.hwnd, dc );
}

static void frame( HDC dc, const RECT *rect, int thickness, COLORREF color )
{
    HBRUSH brush = CreateSolidBrush( color );
    RECT r;

    SetRect( &r, rect->left, rect->top, rect->right, rect->top + thickness );
    FillRect( dc, &r, brush );
    SetRect( &r, rect->left, rect->bottom - thickness, rect->right, rect->bottom );
    FillRect( dc, &r, brush );
    SetRect( &r, rect->left, rect->top, rect->left + thickness, rect->bottom );
    FillRect( dc, &r, brush );
    SetRect( &r, rect->right - thickness, rect->top, rect->right, rect->bottom );
    FillRect( dc, &r, brush );
    DeleteObject( brush );
}

static void paint( HDC target )
{
    RECT client;
    HDC dc;
    HBITMAP bitmap, old_bitmap;
    HFONT old_font;
    HBRUSH brush;
    int height, pad_x;

    GetClientRect( screen.hwnd, &client );
    height = client.bottom;
    pad_x = scaled( height, 16 );
    dc = CreateCompatibleDC( target );
    bitmap = CreateCompatibleBitmap( target, client.right, client.bottom );
    old_bitmap = SelectObject( dc, bitmap );

    brush = CreateSolidBrush( screen.background );
    FillRect( dc, &client, brush );
    DeleteObject( brush );

    old_font = SelectObject( dc, screen.font );
    SetBkMode( dc, TRANSPARENT );
    SetTextColor( dc, RGB( 255, 255, 255 ) );

    for (int i = 0; i < ITEM_COUNT; i++)
    {
        RECT r = screen.rects[i];
        int shade = screen.pressed == i && screen.hot == i ? 30 : screen.hot == i ? 15 : 0;

        if (shade)
        {
            brush = CreateSolidBrush( blend( screen.background, shade ) );
            FillRect( dc, &r, brush );
            DeleteObject( brush );
        }
        if (i == ITEM_CANCEL) frame( dc, &r, scaled( height, 2 ), RGB( 255, 255, 255 ) );
        if (i == screen.focus && screen.focus_shown)
        {
            RECT f = r;
            if (i == ITEM_CANCEL) InflateRect( &f, scaled( height, 3 ), scaled( height, 3 ) );
            frame( dc, &f, scaled( height, 2 ), RGB( 255, 255, 255 ) );
        }
        if (i == ITEM_CANCEL)
            DrawTextW( dc, screen.text[i], -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX );
        else
        {
            r.left += pad_x;
            DrawTextW( dc, screen.text[i], -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX );
        }
    }

    BitBlt( target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY );
    SelectObject( dc, old_font );
    SelectObject( dc, old_bitmap );
    DeleteObject( bitmap );
    DeleteDC( dc );
}

static int item_at( POINT pt )
{
    for (int i = 0; i < ITEM_COUNT; i++)
        if (PtInRect( &screen.rects[i], pt )) return i;
    return -1;
}

static void close_screen( BOOL restore_foreground )
{
    HWND previous = screen.previous;

    DestroyWindow( screen.hwnd );
    screen.hwnd = 0;
    if (restore_foreground && previous && IsWindow( previous )) SetForegroundWindow( previous );
}

/* Tells every program the session ends, as ExitWindowsEx does, without
 * letting one of them stop it. */
static BOOL CALLBACK end_session_window( HWND hwnd, LPARAM lparam )
{
    DWORD_PTR result;
    DWORD pid;

    GetWindowThreadProcessId( hwnd, &pid );
    if (pid == GetCurrentProcessId()) return TRUE;
    SendMessageTimeoutW( hwnd, WM_QUERYENDSESSION, 0, 0, SMTO_ABORTIFHUNG, 5000, &result );
    SendMessageTimeoutW( hwnd, WM_ENDSESSION, TRUE, 0, SMTO_ABORTIFHUNG, 5000, &result );
    return TRUE;
}

static void end_session( int exit_code )
{
    EnumWindows( end_session_window, 0 );
    ExitProcess( exit_code );
}

static void activate( int item )
{
    WCHAR cmd[] = L"cmd.exe", taskmgr[] = L"taskmgr.exe";

    switch (item)
    {
    case ITEM_COMMAND_PROMPT:
        close_screen( FALSE );
        run( cmd );
        break;
    case ITEM_TASK_MANAGER:
        close_screen( FALSE );
        run( taskmgr );
        break;
    case ITEM_RESTART:
        end_session( WINLOGON_EXIT_RESTART );
        break;
    case ITEM_SHUT_DOWN:
        end_session( WINLOGON_EXIT_SHUTDOWN );
        break;
    case ITEM_CANCEL:
        close_screen( TRUE );
        break;
    }
}

static void set_focus_item( int item )
{
    if (screen.focus_shown) screen.focus = (item + ITEM_COUNT) % ITEM_COUNT;
    screen.focus_shown = TRUE;
    InvalidateRect( screen.hwnd, NULL, FALSE );
}

static LRESULT WINAPI screen_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    POINT pt;
    int item;

    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint( hwnd, &ps );
        paint( dc );
        EndPaint( hwnd, &ps );
        return 0;
    }
    case WM_ERASEBKGND:
        return TRUE;
    case WM_SIZE:
        layout( LOWORD(lp), HIWORD(lp) );
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    case WM_SETCURSOR:
        SetCursor( LoadCursorW( 0, (LPCWSTR)IDC_ARROW ) );
        return TRUE;
    case WM_MOUSEMOVE:
        if (!screen.tracking)
        {
            TRACKMOUSEEVENT tme = { .cbSize = sizeof(tme), .dwFlags = TME_LEAVE, .hwndTrack = hwnd };
            screen.tracking = TrackMouseEvent( &tme );
        }
        pt.x = (short)LOWORD(lp);
        pt.y = (short)HIWORD(lp);
        if ((item = item_at( pt )) != screen.hot)
        {
            screen.hot = item;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        return 0;
    case WM_MOUSELEAVE:
        screen.tracking = FALSE;
        screen.hot = -1;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    case WM_LBUTTONDOWN:
        pt.x = (short)LOWORD(lp);
        pt.y = (short)HIWORD(lp);
        screen.pressed = item_at( pt );
        SetCapture( hwnd );
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    case WM_LBUTTONUP:
        ReleaseCapture();
        pt.x = (short)LOWORD(lp);
        pt.y = (short)HIWORD(lp);
        item = screen.pressed;
        screen.pressed = -1;
        InvalidateRect( hwnd, NULL, FALSE );
        if (item >= 0 && item == item_at( pt )) activate( item );
        return 0;
    case WM_KEYDOWN:
        switch (wp)
        {
        case VK_UP:     set_focus_item( screen.focus - 1 ); break;
        case VK_DOWN:   set_focus_item( screen.focus + 1 ); break;
        case VK_TAB:    set_focus_item( screen.focus + (GetKeyState( VK_SHIFT ) < 0 ? -1 : 1) ); break;
        case VK_RETURN:
        case VK_SPACE:  if (screen.focus_shown) activate( screen.focus ); break;
        case VK_ESCAPE: activate( ITEM_CANCEL ); break;
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_CLOSE) return 0; /* Alt+F4 does not close it */
        break;
    case WM_CLOSE:
        return 0;
    case WM_DESTROY:
        if (screen.font) DeleteObject( screen.font );
        screen.font = 0;
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wp, lp );
}

void show_security_options(void)
{
    static BOOL registered;
    int x = GetSystemMetrics( SM_XVIRTUALSCREEN ), y = GetSystemMetrics( SM_YVIRTUALSCREEN );
    int width = GetSystemMetrics( SM_CXVIRTUALSCREEN ), height = GetSystemMetrics( SM_CYVIRTUALSCREEN );
    int screen_height = GetSystemMetrics( SM_CYSCREEN );

    if (screen.hwnd)
    {
        SetForegroundWindow( screen.hwnd );
        return;
    }
    if (!registered)
    {
        WNDCLASSW class = { .lpfnWndProc = screen_proc, .hInstance = GetModuleHandleW( NULL ),
                            .lpszClassName = class_name };
        if (!RegisterClassW( &class )) return;
        registered = TRUE;
    }

    for (int i = 0; i < ITEM_COUNT; i++)
        LoadStringW( GetModuleHandleW( NULL ), item_text_ids[i], screen.text[i], ARRAY_SIZE(screen.text[i]) );
    screen.background = accent_color();
    screen.font = CreateFontW( -scaled( screen_height, 27 ), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH, L"Segoe UI" );
    screen.hot = screen.pressed = -1;
    screen.focus = 0;
    screen.focus_shown = FALSE;
    screen.tracking = FALSE;
    screen.previous = GetForegroundWindow();

    screen.hwnd = CreateWindowExW( WS_EX_TOPMOST | WS_EX_TOOLWINDOW, class_name, NULL, WS_POPUP,
                                   x, y, width, height, 0, 0, GetModuleHandleW( NULL ), NULL );
    if (!screen.hwnd)
    {
        ERR( "cannot show the security options: %lu\n", GetLastError() );
        return;
    }
    ShowWindow( screen.hwnd, SW_SHOW );
    SetForegroundWindow( screen.hwnd );
}
