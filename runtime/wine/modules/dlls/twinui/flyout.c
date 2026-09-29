/*
 * The thumbnails over a taskbar button, as in Windows 10
 *
 * When the pointer rests on a taskbar button, a flyout of the shell's
 * acrylic rises from the taskbar over it: for every window of the button
 * (one, or all of a group) its icon, its title and a live thumbnail from
 * DWM, and under the thumbnail the buttons the program put there
 * (ITaskbarList3::ThumbBarAddButtons: play, pause, next...). The X closes a
 * window, a click on it switches to it, a click on a button tells the
 * program (WM_COMMAND, THBN_CLICKED). It goes a moment after the pointer
 * leaves both the taskbar button and the flyout.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "private.h"
#include "commctrl.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(twinui);

#define TIMER_SHOW    1
#define TIMER_HIDE    2
#define TIMER_REFRESH 3
#define SHOW_DELAY    400
#define HIDE_DELAY    300

#define MAX_ITEMS     10
#define PADDING       8
#define TITLE_HEIGHT  30
#define ICON_SIZE     16
#define THUMB_WIDTH   200
#define THUMB_HEIGHT  120
#define BAR_HEIGHT    30     /* the row of the program's buttons */
#define BUTTON_SIZE   26

#ifndef THBN_CLICKED
#define THBN_CLICKED  0x1800
#endif

struct flyout_button
{
    RECT   rect;
    UINT32 id, flags;
    BOOL   has_icon;
    UINT32 icon[ARCTIC_TASKBAR_ICON * ARCTIC_TASKBAR_ICON];
    WCHAR  tip[64];
};

struct item
{
    HWND        hwnd;
    HTHUMBNAIL  thumbnail;
    RECT        rect, thumb, close;
    UINT        button_count;
    struct flyout_button buttons[ARCTIC_THUMB_BUTTONS];
};

static HWND        flyout, tooltip;
static struct item items[MAX_ITEMS];
static UINT        count;
static HWND        pending[MAX_ITEMS];
static UINT        pending_count;
static RECT        pending_button;
static int         hot = -1, hot_button = -1;   /* item and button under the pointer */
static BOOL        hot_close, pressed;
static HFONT       font;
static LONG        buttons_serial = -1;
static BOOL        with_bar;                    /* some window has buttons */

static void load_buttons(void)
{
    struct arctic_taskbar_shared *shared = get_taskbar_shared();

    with_bar = FALSE;
    for (UINT i = 0; i < count; i++) items[i].button_count = 0;
    if (!shared) return;
    buttons_serial = shared->serial;
    for (UINT i = 0; i < count; i++)
    {
        const struct arctic_taskbar_window *row = find_taskbar_row( shared, items[i].hwnd );
        UINT n = 0;

        if (!row) continue;
        for (UINT k = 0; k < row->button_count && k < ARCTIC_THUMB_BUTTONS; k++)
        {
            struct flyout_button *b = &items[i].buttons[n];

            if (row->buttons[k].flags & THBF_HIDDEN) continue;
            b->id = row->buttons[k].id;
            b->flags = row->buttons[k].flags;
            b->has_icon = row->buttons[k].has_icon;
            memcpy( b->icon, row->buttons[k].icon, sizeof(b->icon) );
            memcpy( b->tip, row->buttons[k].tip, sizeof(b->tip) );
            n++;
        }
        items[i].button_count = n;
        if (n) with_bar = TRUE;
    }
}

static void drop_thumbnails(void)
{
    for (UINT i = 0; i < count; i++)
    {
        if (items[i].thumbnail) DwmUnregisterThumbnail( items[i].thumbnail );
        items[i].thumbnail = NULL;
    }
}

static void hide_flyout(void)
{
    KillTimer( flyout, TIMER_SHOW );
    KillTimer( flyout, TIMER_HIDE );
    KillTimer( flyout, TIMER_REFRESH );
    drop_thumbnails();
    count = 0;
    hot = hot_button = -1;
    hot_close = pressed = FALSE;
    SendMessageW( tooltip, TTM_ACTIVATE, FALSE, 0 );
    ShowWindow( flyout, SW_HIDE );
}

static void update_tooltips(void)
{
    TTTOOLINFOW info = { sizeof(info) };

    /* one tool per button, the same ids every time */
    for (UINT id = 1; id <= MAX_ITEMS * ARCTIC_THUMB_BUTTONS; id++)
    {
        info.hwnd = flyout;
        info.uId = id;
        SendMessageW( tooltip, TTM_DELTOOLW, 0, (LPARAM)&info );
    }
    for (UINT i = 0; i < count; i++)
    {
        for (UINT k = 0; k < items[i].button_count; k++)
        {
            memset( &info, 0, sizeof(info) );
            info.cbSize = sizeof(info);
            info.uFlags = TTF_SUBCLASS;
            info.hwnd = flyout;
            info.uId = 1 + i * ARCTIC_THUMB_BUTTONS + k;
            info.rect = items[i].buttons[k].rect;
            info.lpszText = items[i].buttons[k].tip;
            if (items[i].buttons[k].tip[0]) SendMessageW( tooltip, TTM_ADDTOOLW, 0, (LPARAM)&info );
        }
    }
    SendMessageW( tooltip, TTM_ACTIVATE, TRUE, 0 );
}

/* thumbnails side by side, each as wide as its window's shape asks, the row
 * narrowed to fit the monitor */
static void layout( const RECT *monitor )
{
    int thumb_width = THUMB_WIDTH, item_height, x = 0;

    while (thumb_width > 80 && (int)count * (thumb_width + 2 * PADDING) > monitor->right - monitor->left)
        thumb_width -= 20;
    item_height = 2 * PADDING + TITLE_HEIGHT + THUMB_HEIGHT + (with_bar ? BAR_HEIGHT : 0);

    for (UINT i = 0; i < count; i++)
    {
        struct item *item = &items[i];
        WINDOWPLACEMENT placement = { sizeof(placement) };
        RECT rect;
        int width, height, tw, th, bx;

        if (IsIconic( item->hwnd ) && GetWindowPlacement( item->hwnd, &placement )) rect = placement.rcNormalPosition;
        else GetWindowRect( item->hwnd, &rect );
        width = max( 1, rect.right - rect.left );
        height = max( 1, rect.bottom - rect.top );
        tw = thumb_width;
        th = MulDiv( tw, height, width );
        if (th > THUMB_HEIGHT)
        {
            th = THUMB_HEIGHT;
            tw = MulDiv( th, width, height );
        }

        SetRect( &item->rect, x, 0, x + thumb_width + 2 * PADDING, item_height );
        SetRect( &item->thumb, x + PADDING + (thumb_width - tw) / 2, PADDING + TITLE_HEIGHT + (THUMB_HEIGHT - th) / 2,
                 x + PADDING + (thumb_width - tw) / 2 + tw, PADDING + TITLE_HEIGHT + (THUMB_HEIGHT - th) / 2 + th );
        SetRect( &item->close, item->rect.right - TITLE_HEIGHT, 0, item->rect.right, TITLE_HEIGHT );

        /* the program's buttons, in a row under the thumbnail */
        bx = x + (thumb_width + 2 * PADDING - (int)item->button_count * BUTTON_SIZE) / 2;
        for (UINT k = 0; k < item->button_count; k++)
            SetRect( &item->buttons[k].rect, bx + k * BUTTON_SIZE,
                     PADDING + TITLE_HEIGHT + THUMB_HEIGHT + (BAR_HEIGHT - BUTTON_SIZE) / 2,
                     bx + (k + 1) * BUTTON_SIZE, PADDING + TITLE_HEIGHT + THUMB_HEIGHT + (BAR_HEIGHT + BUTTON_SIZE) / 2 );
        x += thumb_width + 2 * PADDING;
    }
}

static void show_flyout( const HWND *hwnds, UINT n, const RECT *button )
{
    MONITORINFO info = { sizeof(info) };
    int width, height, x, y;

    drop_thumbnails();
    count = 0;
    for (UINT i = 0; i < n && count < MAX_ITEMS; i++)
    {
        if (!IsWindow( hwnds[i] )) continue;
        memset( &items[count], 0, sizeof(items[count]) );
        items[count++].hwnd = hwnds[i];
    }
    if (!count)
    {
        hide_flyout();
        return;
    }
    load_buttons();

    GetMonitorInfoW( MonitorFromRect( button, MONITOR_DEFAULTTONEAREST ), &info );
    layout( &info.rcMonitor );
    width = items[count - 1].rect.right;
    height = items[0].rect.bottom;

    /* over the button, on the taskbar's edge, inside the monitor */
    x = (button->left + button->right - width) / 2;
    x = max( info.rcMonitor.left, min( x, info.rcMonitor.right - width ) );
    if (button->top > (info.rcMonitor.top + info.rcMonitor.bottom) / 2) y = button->top - height;
    else y = button->bottom;
    SetWindowPos( flyout, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE );

    for (UINT i = 0; i < count; i++)
    {
        DWM_THUMBNAIL_PROPERTIES props = { DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY };

        if (FAILED(DwmRegisterThumbnail( flyout, items[i].hwnd, &items[i].thumbnail ))) continue;
        props.rcDestination = items[i].thumb;
        props.fVisible = TRUE;
        props.opacity = 255;
        DwmUpdateThumbnailProperties( items[i].thumbnail, &props );
    }
    update_tooltips();
    SetTimer( flyout, TIMER_REFRESH, 250, NULL );
    InvalidateRect( flyout, NULL, TRUE );
    ShowWindow( flyout, SW_SHOWNOACTIVATE );
}

/* premultiplied pixels of a program's icon, faded when its button is disabled */
static void draw_pixels( HDC hdc, int x, int y, const UINT32 *pixels, BYTE opacity )
{
    BITMAPINFO info = {{ sizeof(info.bmiHeader), ARCTIC_TASKBAR_ICON, -ARCTIC_TASKBAR_ICON, 1, 32, BI_RGB }};
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, opacity, AC_SRC_ALPHA };
    HDC mem = CreateCompatibleDC( hdc );
    HBITMAP bitmap;
    void *bits;

    if ((bitmap = CreateDIBSection( hdc, &info, DIB_RGB_COLORS, &bits, NULL, 0 )))
    {
        HGDIOBJ old = SelectObject( mem, bitmap );
        memcpy( bits, pixels, ARCTIC_TASKBAR_ICON * ARCTIC_TASKBAR_ICON * 4 );
        GdiAlphaBlend( hdc, x, y, ARCTIC_TASKBAR_ICON, ARCTIC_TASKBAR_ICON, mem, 0, 0, ARCTIC_TASKBAR_ICON,
                       ARCTIC_TASKBAR_ICON, blend );
        SelectObject( mem, old );
        DeleteObject( bitmap );
    }
    DeleteDC( mem );
}

static void paint( HDC hdc, const RECT *client )
{
    FillRect( hdc, client, GetStockObject( BLACK_BRUSH ) );
    SelectObject( hdc, font );
    SetBkMode( hdc, TRANSPARENT );
    SetTextColor( hdc, RGB(255, 255, 255) );

    for (UINT i = 0; i < count; i++)
    {
        const struct item *item = &items[i];
        BOOL is_hot = (int)i == hot;
        WCHAR title[256];
        RECT text;
        HICON icon;

        if (is_hot && hot_button < 0) fill_alpha( hdc, &item->rect, RGB(255, 255, 255), 0x1a );

        icon = window_icon( item->hwnd, FALSE );
        DrawIconEx( hdc, item->rect.left + PADDING, (TITLE_HEIGHT - ICON_SIZE) / 2 + PADDING / 2, icon,
                    ICON_SIZE, ICON_SIZE, 0, NULL, DI_NORMAL );
        if (!InternalGetWindowText( item->hwnd, title, ARRAY_SIZE(title) ))
            GetWindowTextW( item->hwnd, title, ARRAY_SIZE(title) );
        SetRect( &text, item->rect.left + PADDING + ICON_SIZE + 8, PADDING / 2,
                 is_hot ? item->close.left : item->rect.right - PADDING, TITLE_HEIGHT + PADDING / 2 );
        DrawTextW( hdc, title, -1, &text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX );

        /* under the thumbnail, for a window that has none */
        icon = window_icon( item->hwnd, TRUE );
        DrawIconEx( hdc, (item->thumb.left + item->thumb.right) / 2 - 16, (item->thumb.top + item->thumb.bottom) / 2 - 16,
                    icon, 32, 32, 0, NULL, DI_NORMAL );

        if (is_hot)
        {
            if (hot_close) fill_alpha( hdc, &item->close, RGB(0xe8, 0x11, 0x23), 255 );
            draw_glyph_close( hdc, &item->close, RGB(255, 255, 255) );
        }

        for (UINT k = 0; k < item->button_count; k++)
        {
            const struct flyout_button *b = &item->buttons[k];
            BOOL disabled = b->flags & THBF_DISABLED;

            if (!(b->flags & THBF_NOBACKGROUND) && is_hot && (int)k == hot_button && !disabled &&
                !(b->flags & THBF_NONINTERACTIVE))
                fill_alpha( hdc, &b->rect, RGB(255, 255, 255), pressed ? 0x0f : 0x26 );
            if (b->has_icon)
                draw_pixels( hdc, (b->rect.left + b->rect.right - ARCTIC_TASKBAR_ICON) / 2,
                             (b->rect.top + b->rect.bottom - ARCTIC_TASKBAR_ICON) / 2, b->icon, disabled ? 0x66 : 0xff );
        }
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

static void hit_test( POINT pt, int *item, int *button, BOOL *on_close )
{
    *item = *button = -1;
    *on_close = FALSE;
    for (UINT i = 0; i < count; i++)
    {
        if (!PtInRect( &items[i].rect, pt )) continue;
        *item = i;
        *on_close = PtInRect( &items[i].close, pt );
        for (UINT k = 0; k < items[i].button_count; k++)
            if (PtInRect( &items[i].buttons[k].rect, pt )) *button = k;
        return;
    }
}

static LRESULT CALLBACK flyout_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    switch (msg)
    {
    case WM_TIMER:
        if (wparam == TIMER_REFRESH)
        {
            struct arctic_taskbar_shared *shared = get_taskbar_shared();

            /* a program changed its buttons (play became pause) */
            if (shared && shared->serial != buttons_serial)
            {
                BOOL had_bar = with_bar;
                load_buttons();
                if (had_bar != with_bar && pending_count) show_flyout( pending, pending_count, &pending_button );
                else
                {
                    MONITORINFO info = { sizeof(info) };
                    GetMonitorInfoW( MonitorFromWindow( hwnd, MONITOR_DEFAULTTONEAREST ), &info );
                    layout( &info.rcMonitor );
                    update_tooltips();
                    InvalidateRect( hwnd, NULL, TRUE );
                }
            }
            return 0;
        }
        KillTimer( hwnd, wparam );
        if (wparam == TIMER_SHOW && pending_count) show_flyout( pending, pending_count, &pending_button );
        else if (wparam == TIMER_HIDE) hide_flyout();
        return 0;

    case WM_MOUSEMOVE:
    {
        POINT pt = { (short)LOWORD( lparam ), (short)HIWORD( lparam ) };
        TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, hwnd };
        int item, button;
        BOOL on_close;

        KillTimer( hwnd, TIMER_HIDE );
        hit_test( pt, &item, &button, &on_close );
        if (item != hot || button != hot_button || on_close != hot_close)
        {
            hot = item;
            hot_button = button;
            hot_close = on_close;
            InvalidateRect( hwnd, NULL, TRUE );
        }
        TrackMouseEvent( &track );
        return 0;
    }

    case WM_MOUSELEAVE:
        hot = hot_button = -1;
        hot_close = pressed = FALSE;
        InvalidateRect( hwnd, NULL, TRUE );
        SetTimer( hwnd, TIMER_HIDE, HIDE_DELAY, NULL );
        return 0;

    case WM_LBUTTONDOWN:
        pressed = TRUE;
        InvalidateRect( hwnd, NULL, TRUE );
        return 0;

    case WM_LBUTTONUP:
    {
        POINT pt = { (short)LOWORD( lparam ), (short)HIWORD( lparam ) };
        int item, button;
        BOOL on_close;
        HWND target;

        pressed = FALSE;
        hit_test( pt, &item, &button, &on_close );
        if (item < 0) return 0;
        target = items[item].hwnd;
        if (button >= 0)
        {
            const struct flyout_button *b = &items[item].buttons[button];

            if (b->flags & (THBF_DISABLED | THBF_NONINTERACTIVE)) return 0;
            PostMessageW( target, WM_COMMAND, MAKEWPARAM( b->id, THBN_CLICKED ), 0 );
            if (b->flags & THBF_DISMISSONCLICK) hide_flyout();
            else InvalidateRect( hwnd, NULL, TRUE );
            return 0;
        }
        hide_flyout();
        if (on_close) PostMessageW( target, WM_SYSCOMMAND, SC_CLOSE, 0 );
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
    tooltip = CreateWindowExW( WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                               0, 0, 0, 0, flyout, NULL, twinui_instance, NULL );
    font = shell_font( 12, FW_NORMAL );
    set_acrylic( flyout );
    set_transition( flyout, ARCTIC_TRANSITION_FADE );
    return TRUE;
}

void flyout_hover( const HWND *hwnds, UINT n, const RECT *button, DWORD flags )
{
    if (!create_flyout()) return;
    if (n && hwnds && button)
    {
        BOOL same = count == n && IsWindowVisible( flyout );

        for (UINT i = 0; same && i < n; i++) same = items[i].hwnd == hwnds[i];
        KillTimer( flyout, TIMER_HIDE );
        pending_count = min( n, MAX_ITEMS );
        memcpy( pending, hwnds, pending_count * sizeof(*pending) );
        pending_button = *button;
        if (same) return;
        if (IsWindowVisible( flyout ) || (flags & 1)) show_flyout( pending, pending_count, button );
        else SetTimer( flyout, TIMER_SHOW, SHOW_DELAY, NULL );
        return;
    }
    pending_count = 0;
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
