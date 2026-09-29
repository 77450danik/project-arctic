/*
 * The thumbnail over a taskbar button, as in Windows 10
 *
 * When the pointer rests on a taskbar button, a flyout of the shell's
 * acrylic rises from the taskbar over it: the window's icon and title, and
 * a live thumbnail of it from DWM. The X closes the window, a click
 * switches to it. It goes a moment after the pointer leaves both the button
 * and the flyout.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(twinui);

#define TIMER_SHOW    1
#define TIMER_HIDE    2
#define SHOW_DELAY    400
#define HIDE_DELAY    300

#define PADDING       8
#define TITLE_HEIGHT  30
#define ICON_SIZE     16
#define THUMB_WIDTH   200
#define THUMB_HEIGHT  120

static HWND       flyout;
static HWND       task, pending;
static RECT       pending_button;
static HTHUMBNAIL thumbnail;
static RECT       item, thumb, close_box;
static BOOL       hot, hot_close;
static HFONT      font;

static void hide_flyout(void)
{
    KillTimer( flyout, TIMER_SHOW );
    KillTimer( flyout, TIMER_HIDE );
    if (thumbnail) DwmUnregisterThumbnail( thumbnail );
    thumbnail = NULL;
    task = NULL;
    hot = hot_close = FALSE;
    ShowWindow( flyout, SW_HIDE );
}

static void show_flyout( HWND hwnd, const RECT *button )
{
    DWM_THUMBNAIL_PROPERTIES props = { DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY };
    MONITORINFO info = { sizeof(info) };
    WINDOWPLACEMENT placement = { sizeof(placement) };
    int width, height, tw, th, x, y;
    RECT rect;

    if (!IsWindow( hwnd ))
    {
        hide_flyout();
        return;
    }
    if (thumbnail) DwmUnregisterThumbnail( thumbnail );
    thumbnail = NULL;
    task = hwnd;

    if (IsIconic( hwnd ) && GetWindowPlacement( hwnd, &placement )) rect = placement.rcNormalPosition;
    else GetWindowRect( hwnd, &rect );
    width = max( 1, rect.right - rect.left );
    height = max( 1, rect.bottom - rect.top );
    tw = THUMB_WIDTH;
    th = MulDiv( tw, height, width );
    if (th > THUMB_HEIGHT)
    {
        th = THUMB_HEIGHT;
        tw = MulDiv( th, width, height );
    }

    SetRect( &item, 0, 0, THUMB_WIDTH + 2 * PADDING, TITLE_HEIGHT + THUMB_HEIGHT + 2 * PADDING );
    SetRect( &thumb, PADDING + (THUMB_WIDTH - tw) / 2, PADDING + TITLE_HEIGHT + (THUMB_HEIGHT - th) / 2,
             PADDING + (THUMB_WIDTH - tw) / 2 + tw, PADDING + TITLE_HEIGHT + (THUMB_HEIGHT - th) / 2 + th );
    SetRect( &close_box, item.right - TITLE_HEIGHT, 0, item.right, TITLE_HEIGHT );

    /* over the button, on the taskbar's edge, inside the monitor */
    GetMonitorInfoW( MonitorFromRect( button, MONITOR_DEFAULTTONEAREST ), &info );
    x = (button->left + button->right - item.right) / 2;
    x = max( info.rcMonitor.left, min( x, info.rcMonitor.right - item.right ) );
    if (button->top > (info.rcMonitor.top + info.rcMonitor.bottom) / 2) y = button->top - item.bottom;
    else y = button->bottom;
    SetWindowPos( flyout, HWND_TOPMOST, x, y, item.right, item.bottom, SWP_NOACTIVATE );

    if (SUCCEEDED(DwmRegisterThumbnail( flyout, hwnd, &thumbnail )))
    {
        props.rcDestination = thumb;
        props.fVisible = TRUE;
        props.opacity = 255;
        DwmUpdateThumbnailProperties( thumbnail, &props );
    }
    InvalidateRect( flyout, NULL, TRUE );
    ShowWindow( flyout, SW_SHOWNOACTIVATE );
}

static void paint( HDC hdc, const RECT *client )
{
    WCHAR title[256];
    RECT text;
    HICON icon;

    FillRect( hdc, client, GetStockObject( BLACK_BRUSH ) );
    if (!task) return;
    if (hot) fill_alpha( hdc, &item, RGB(255, 255, 255), 0x1a );

    icon = window_icon( task, FALSE );
    DrawIconEx( hdc, PADDING, (TITLE_HEIGHT - ICON_SIZE) / 2 + PADDING / 2, icon, ICON_SIZE, ICON_SIZE, 0, NULL,
                DI_NORMAL );
    if (!InternalGetWindowText( task, title, ARRAY_SIZE(title) )) GetWindowTextW( task, title, ARRAY_SIZE(title) );
    TRACE( "%p %s, icon %p, font %p\n", task, debugstr_w(title), icon, font );
    SelectObject( hdc, font );
    SetBkMode( hdc, TRANSPARENT );
    SetTextColor( hdc, RGB(255, 255, 255) );
    SetRect( &text, PADDING + ICON_SIZE + 8, PADDING / 2, hot ? close_box.left : item.right - PADDING,
             TITLE_HEIGHT + PADDING / 2 );
    DrawTextW( hdc, title, -1, &text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX );

    icon = window_icon( task, TRUE );
    DrawIconEx( hdc, (thumb.left + thumb.right) / 2 - 16, (thumb.top + thumb.bottom) / 2 - 16, icon, 32, 32, 0,
                NULL, DI_NORMAL );

    if (hot)
    {
        if (hot_close) fill_alpha( hdc, &close_box, RGB(0xe8, 0x11, 0x23), 255 );
        draw_glyph_close( hdc, &close_box, RGB(255, 255, 255) );
    }
}

static void on_paint(void)
{
    PAINTSTRUCT ps;
    RECT client;
    HDC hdc = BeginPaint( flyout, &ps ), mem;
    BITMAPINFO info = { { sizeof(info.bmiHeader), 0, 0, 1, 32, BI_RGB } };
    HBITMAP bitmap;
    void *bits;

    GetClientRect( flyout, &client );
    info.bmiHeader.biWidth = client.right;
    info.bmiHeader.biHeight = -client.bottom;
    mem = CreateCompatibleDC( hdc );
    if ((bitmap = CreateDIBSection( hdc, &info, DIB_RGB_COLORS, &bits, NULL, 0 )))
    {
        HGDIOBJ old = SelectObject( mem, bitmap );
        paint( mem, &client );
        BitBlt( hdc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY );
        SelectObject( mem, old );
        DeleteObject( bitmap );
    }
    DeleteDC( mem );
    EndPaint( flyout, &ps );
}

static LRESULT CALLBACK flyout_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    switch (msg)
    {
    case WM_TIMER:
        KillTimer( hwnd, wparam );
        if (wparam == TIMER_SHOW && pending) show_flyout( pending, &pending_button );
        else if (wparam == TIMER_HIDE) hide_flyout();
        return 0;

    case WM_MOUSEMOVE:
    {
        POINT pt = { (short)LOWORD( lparam ), (short)HIWORD( lparam ) };
        TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, hwnd };
        BOOL on_close = PtInRect( &close_box, pt );

        KillTimer( hwnd, TIMER_HIDE );
        if (!hot || on_close != hot_close)
        {
            hot = TRUE;
            hot_close = on_close;
            InvalidateRect( hwnd, NULL, TRUE );
        }
        TrackMouseEvent( &track );
        return 0;
    }

    case WM_MOUSELEAVE:
        hot = hot_close = FALSE;
        InvalidateRect( hwnd, NULL, TRUE );
        SetTimer( hwnd, TIMER_HIDE, HIDE_DELAY, NULL );
        return 0;

    case WM_LBUTTONUP:
    {
        POINT pt = { (short)LOWORD( lparam ), (short)HIWORD( lparam ) };
        HWND target = task;

        hide_flyout();
        if (!target) return 0;
        if (PtInRect( &close_box, pt )) PostMessageW( target, WM_SYSCOMMAND, SC_CLOSE, 0 );
        else SwitchToThisWindow( target, TRUE );
        return 0;
    }

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        on_paint();
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

static BOOL create_flyout(void)
{
    WNDCLASSW class = { 0 };

    if (flyout) return TRUE;
    class.lpfnWndProc = flyout_proc;
    class.hInstance = twinui_instance;
    class.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_ARROW );
    class.lpszClassName = L"ArcticTaskFlyout";
    RegisterClassW( &class );
    flyout = CreateWindowExW( WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, class.lpszClassName, L"",
                              WS_POPUP, 0, 0, 1, 1, NULL, NULL, twinui_instance, NULL );
    if (!flyout) return FALSE;
    font = shell_font( 12, FW_NORMAL );
    set_acrylic( flyout );
    set_transition( flyout, ARCTIC_TRANSITION_FADE );
    return TRUE;
}

void flyout_hover( HWND hwnd, const RECT *button, DWORD flags )
{
    if (!create_flyout()) return;
    if (hwnd && button)
    {
        KillTimer( flyout, TIMER_HIDE );
        if (task == hwnd && IsWindowVisible( flyout )) return;
        pending = hwnd;
        pending_button = *button;
        if (IsWindowVisible( flyout ) || (flags & 1)) show_flyout( hwnd, button );
        else SetTimer( flyout, TIMER_SHOW, SHOW_DELAY, NULL );
        return;
    }
    pending = NULL;
    KillTimer( flyout, TIMER_SHOW );
    if (flags & 1) hide_flyout();
    else if (IsWindowVisible( flyout ))
    {
        POINT pt;
        RECT rect;

        /* the pointer went from the button onto the flyout: it stays, and
         * goes once the pointer leaves it */
        GetCursorPos( &pt );
        GetWindowRect( flyout, &rect );
        if (!PtInRect( &rect, pt )) SetTimer( flyout, TIMER_HIDE, HIDE_DELAY, NULL );
    }
}
