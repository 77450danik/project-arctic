/*
 * Alt+Tab, as in Windows 10
 *
 * Alt+Tab and Alt+Shift+Tab are hotkeys of the switcher's own thread. On
 * the first one the switcher takes the foreground at once, still cloaked,
 * so that the Alt that is let go reaches it and not the program (which
 * would open its menu). A quick Alt+Tab switches without showing anything;
 * when Alt is held longer, the switcher shows over the shell's acrylic:
 * every window with its icon, title and a live thumbnail (DWM), the one
 * that will be switched to in a white frame. Tab and the arrows move the
 * frame, Enter or letting go of Alt switches, Esc goes back, Delete or the
 * X closes a window, and a click switches to the window clicked.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <limits.h>
#include <stdlib.h>

#include "private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(twinui);

#define HOTKEY_NEXT     1
#define HOTKEY_PREVIOUS 2
#define TIMER_POLL      1
#define SHOW_DELAY      150   /* ms Alt is held before the switcher shows */
#define MAX_ITEMS       64

/* the Windows 10 switcher's layout, at 96 dpi */
#define PANEL_PADDING   px(16)
#define ITEM_PADDING    px(8)
#define ITEM_GAP        px(8)
#define TITLE_HEIGHT    px(30)
#define ICON_SIZE       px(16)
#define THUMB_HEIGHT    px(160)
#define THUMB_MIN_WIDTH px(100)
#define FRAME           px(3)     /* the frame of the selected window */

struct item
{
    HWND       hwnd;
    WCHAR      title[256];
    HICON      icon, big_icon;
    int        width, height;  /* the window's own size */
    RECT       rect;           /* the item, client coordinates */
    RECT       box;            /* the thumbnail's box */
    RECT       thumb;          /* the thumbnail, the window's shape in the box */
    RECT       close;          /* its X */
    HTHUMBNAIL thumbnail;
};

static HWND        switcher;
static struct item items[MAX_ITEMS];
static int         count, selected, hot = -1;
static BOOL        hot_close;
static BOOL        active, revealed;
static DWORD       started;
static HWND        previous;
static HFONT       font;

static BOOL CALLBACK add_window( HWND hwnd, LPARAM lparam )
{
    struct item *item;
    WINDOWPLACEMENT placement = { sizeof(placement) };
    RECT rect;

    if (count == MAX_ITEMS) return FALSE;
    if (hwnd == switcher || !is_task_window( hwnd )) return TRUE;
    item = &items[count++];
    memset( item, 0, sizeof(*item) );
    item->hwnd = hwnd;
    InternalGetWindowText( hwnd, item->title, ARRAY_SIZE(item->title) );
    item->icon = window_icon( hwnd, FALSE );
    item->big_icon = window_icon( hwnd, TRUE );
    if (IsIconic( hwnd ) && GetWindowPlacement( hwnd, &placement )) rect = placement.rcNormalPosition;
    else GetWindowRect( hwnd, &rect );
    item->width = max( 1, rect.right - rect.left );
    item->height = max( 1, rect.bottom - rect.top );
    return TRUE;
}

/* rows of items, as few as fit in 90% of the monitor's width; thumbnails
 * shrink when there would be more than three rows */
static SIZE layout( const RECT *work )
{
    int max_width = (work->right - work->left) * 9 / 10 - 2 * PANEL_PADDING;
    int thumb_height = THUMB_HEIGHT, rows, widest, row_start[MAX_ITEMS + 1], row_width[MAX_ITEMS];
    int item_height, i, r, x, y;
    SIZE size;

    for (;;)
    {
        rows = 0;
        widest = 0;
        x = 0;
        for (i = 0; i < count; i++)
        {
            int box = min( max( thumb_height * items[i].width / items[i].height, THUMB_MIN_WIDTH ), thumb_height * 2 );
            int width = box + 2 * ITEM_PADDING;

            if (!i || x + ITEM_GAP + width > max_width)
            {
                if (i) row_width[rows - 1] = x;
                row_start[rows++] = i;
                x = width;
            }
            else x += ITEM_GAP + width;
            items[i].box.left = 0;
            items[i].box.right = box;
        }
        row_width[rows - 1] = x;
        row_start[rows] = count;
        for (r = 0; r < rows; r++) widest = max( widest, row_width[r] );
        if (rows <= 3 || thumb_height <= px( 64 )) break;
        thumb_height -= px( 16 );
    }

    item_height = 2 * ITEM_PADDING + TITLE_HEIGHT + thumb_height;
    for (r = 0; r < rows; r++)
    {
        x = PANEL_PADDING + (widest - row_width[r]) / 2;
        y = PANEL_PADDING + r * (item_height + ITEM_GAP);
        for (i = row_start[r]; i < row_start[r + 1]; i++)
        {
            struct item *item = &items[i];
            int box = item->box.right, width, height;

            SetRect( &item->rect, x, y, x + box + 2 * ITEM_PADDING, y + item_height );
            SetRect( &item->box, x + ITEM_PADDING, y + ITEM_PADDING + TITLE_HEIGHT,
                     x + ITEM_PADDING + box, y + ITEM_PADDING + TITLE_HEIGHT + thumb_height );
            /* the window's shape, as large as the box takes */
            width = box;
            height = MulDiv( width, item->height, item->width );
            if (height > thumb_height)
            {
                height = thumb_height;
                width = MulDiv( height, item->width, item->height );
            }
            SetRect( &item->thumb, item->box.left + (box - width) / 2, item->box.top + (thumb_height - height) / 2,
                     item->box.left + (box - width) / 2 + width, item->box.top + (thumb_height - height) / 2 + height );
            SetRect( &item->close, item->rect.right - ITEM_PADDING - TITLE_HEIGHT, y + ITEM_PADDING,
                     item->rect.right - ITEM_PADDING, y + ITEM_PADDING + TITLE_HEIGHT );
            x += box + 2 * ITEM_PADDING + ITEM_GAP;
        }
    }
    size.cx = widest + 2 * PANEL_PADDING;
    size.cy = rows * item_height + (rows - 1) * ITEM_GAP + 2 * PANEL_PADDING;
    return size;
}

static void place(void)
{
    MONITORINFO info = { sizeof(info) };
    HMONITOR monitor = MonitorFromWindow( previous, MONITOR_DEFAULTTOPRIMARY );
    SIZE size;

    GetMonitorInfoW( monitor, &info );
    size = layout( &info.rcWork );
    SetWindowPos( switcher, HWND_TOPMOST,
                  (info.rcWork.left + info.rcWork.right - size.cx) / 2,
                  (info.rcWork.top + info.rcWork.bottom - size.cy) / 2,
                  size.cx, size.cy, SWP_NOACTIVATE );
}

static void update_thumbnails(void)
{
    for (int i = 0; i < count; i++)
    {
        DWM_THUMBNAIL_PROPERTIES props = { DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY |
                                           DWM_TNP_SOURCECLIENTAREAONLY };

        if (!items[i].thumbnail && FAILED(DwmRegisterThumbnail( switcher, items[i].hwnd, &items[i].thumbnail )))
            continue;
        props.rcDestination = items[i].thumb;
        props.fVisible = TRUE;
        props.opacity = 255;
        props.fSourceClientAreaOnly = FALSE;
        DwmUpdateThumbnailProperties( items[i].thumbnail, &props );
    }
}

static void drop_thumbnails(void)
{
    for (int i = 0; i < count; i++)
    {
        if (items[i].thumbnail) DwmUnregisterThumbnail( items[i].thumbnail );
        items[i].thumbnail = NULL;
    }
}

static void frame_rect( HDC hdc, const RECT *rect, int width, COLORREF color )
{
    HBRUSH brush = CreateSolidBrush( color );
    RECT side;

    SetRect( &side, rect->left, rect->top, rect->right, rect->top + width );
    FillRect( hdc, &side, brush );
    SetRect( &side, rect->left, rect->bottom - width, rect->right, rect->bottom );
    FillRect( hdc, &side, brush );
    SetRect( &side, rect->left, rect->top + width, rect->left + width, rect->bottom - width );
    FillRect( hdc, &side, brush );
    SetRect( &side, rect->right - width, rect->top + width, rect->right, rect->bottom - width );
    FillRect( hdc, &side, brush );
    DeleteObject( brush );
}

/* Black lets the acrylic through; white GDI text and frames stay white
 * over it (dwm keys GDI pixels by their brightness). */
static void paint( HDC hdc, const RECT *client )
{
    FillRect( hdc, client, GetStockObject( BLACK_BRUSH ) );
    SelectObject( hdc, font );
    SetBkMode( hdc, TRANSPARENT );
    SetTextColor( hdc, RGB(255, 255, 255) );

    for (int i = 0; i < count; i++)
    {
        const struct item *item = &items[i];
        BOOL with_close = i == hot || i == selected;
        RECT text;

        if (i == selected) frame_rect( hdc, &item->rect, FRAME, RGB(255, 255, 255) );
        else if (i == hot) frame_rect( hdc, &item->rect, 2, RGB(128, 128, 128) );

        DrawIconEx( hdc, item->rect.left + ITEM_PADDING, item->rect.top + ITEM_PADDING + (TITLE_HEIGHT - ICON_SIZE) / 2,
                    item->icon, ICON_SIZE, ICON_SIZE, 0, NULL, DI_NORMAL );
        SetRect( &text, item->rect.left + ITEM_PADDING + ICON_SIZE + px( 8 ), item->rect.top + ITEM_PADDING,
                 with_close ? item->close.left - px( 4 ) : item->rect.right - ITEM_PADDING, item->rect.top + ITEM_PADDING + TITLE_HEIGHT );
        DrawTextW( hdc, item->title, -1, &text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX );

        /* under the thumbnail, for a window that has none (minimized before dwm saw it) */
        DrawIconEx( hdc, (item->box.left + item->box.right) / 2 - px( 24 ), (item->box.top + item->box.bottom) / 2 - px( 24 ),
                    item->big_icon, px( 48 ), px( 48 ), 0, NULL, DI_NORMAL );

        if (with_close)
        {
            if (i == hot && hot_close) fill_alpha( hdc, &item->close, RGB(0xe8, 0x11, 0x23), 255 );
            draw_glyph_close( hdc, &item->close, RGB(255, 255, 255) );
        }
    }
}

static void on_paint(void)
{
    PAINTSTRUCT ps;
    RECT client;
    HDC hdc = BeginPaint( switcher, &ps ), mem;
    BITMAPINFO info = { { sizeof(info.bmiHeader), 0, 0, 1, 32, BI_RGB } };
    HBITMAP bitmap;
    void *bits;

    GetClientRect( switcher, &client );
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
    EndPaint( switcher, &ps );
}

static void hide(void)
{
    BOOL off = TRUE;

    KillTimer( switcher, TIMER_POLL );
    /* Windows 10 takes it away at once */
    DwmSetWindowAttribute( switcher, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof(off) );
    ShowWindow( switcher, SW_HIDE );
    drop_thumbnails();
    count = 0;
    hot = -1;
    revealed = FALSE;
}

static void commit(void)
{
    HWND target = selected >= 0 && selected < count ? items[selected].hwnd : NULL;

    if (!active) return;
    active = FALSE;
    if (target && IsWindow( target )) SwitchToThisWindow( target, TRUE );
    else if (IsWindow( previous )) SetForegroundWindow( previous );
    hide();
}

static void cancel( BOOL restore )
{
    if (!active) return;
    active = FALSE;
    if (restore && IsWindow( previous )) SetForegroundWindow( previous );
    hide();
}

static void reveal(void)
{
    BOOL on = FALSE;

    if (revealed) return;
    revealed = TRUE;
    update_thumbnails();
    DwmSetWindowAttribute( switcher, DWMWA_TRANSITIONS_FORCEDISABLED, &on, sizeof(on) );
    set_cloaked( switcher, FALSE );
    InvalidateRect( switcher, NULL, TRUE );
}

static void select_item( int index )
{
    if (!count) return;
    selected = (index % count + count) % count;
    InvalidateRect( switcher, NULL, TRUE );
}

/* the nearest item in the row above or below */
static void select_vertical( int direction )
{
    const RECT *from = &items[selected].rect;
    int best = -1, best_distance = INT_MAX, cx = (from->left + from->right) / 2;

    for (int i = 0; i < count; i++)
    {
        const RECT *r = &items[i].rect;
        int distance;

        if (direction < 0 ? r->top >= from->top : r->top <= from->top) continue;
        distance = abs( r->top - from->top ) * 4 + abs( (r->left + r->right) / 2 - cx );
        if (distance < best_distance)
        {
            best_distance = distance;
            best = i;
        }
    }
    if (best >= 0) select_item( best );
}

static void close_item( int index )
{
    if (index < 0 || index >= count) return;
    PostMessageW( items[index].hwnd, WM_SYSCOMMAND, SC_CLOSE, 0 );
    if (items[index].thumbnail) DwmUnregisterThumbnail( items[index].thumbnail );
    memmove( &items[index], &items[index + 1], (count - index - 1) * sizeof(*items) );
    if (!--count)
    {
        cancel( FALSE );
        return;
    }
    if (selected >= count) selected = count - 1;
    hot = -1;
    place();
    if (revealed) update_thumbnails();
    InvalidateRect( switcher, NULL, TRUE );
}

static void start( BOOL backwards )
{
    count = 0;
    previous = GetForegroundWindow();
    EnumWindows( add_window, 0 );
    if (!count) return;

    active = TRUE;
    revealed = FALSE;
    hot = -1;
    started = GetTickCount();
    /* the window after the active one; the desktop being active, the first */
    selected = backwards ? count - 1 : (count > 1 && items[0].hwnd == previous ? 1 : 0);

    set_cloaked( switcher, TRUE );
    place();
    ShowWindow( switcher, SW_SHOW );
    SetForegroundWindow( switcher );
    SetTimer( switcher, TIMER_POLL, 15, NULL );
}

static void on_hotkey( BOOL backwards )
{
    if (!active)
    {
        start( backwards );
        return;
    }
    select_item( selected + (backwards ? -1 : 1) );
}

static int hit_test( POINT pt, BOOL *on_close )
{
    for (int i = 0; i < count; i++)
    {
        if (!PtInRect( &items[i].rect, pt )) continue;
        *on_close = PtInRect( &items[i].close, pt );
        return i;
    }
    *on_close = FALSE;
    return -1;
}

static LRESULT CALLBACK switcher_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    switch (msg)
    {
    case WM_HOTKEY:
        on_hotkey( wparam == HOTKEY_PREVIOUS );
        return 0;

    case WM_TIMER:
        if (!active) break;
        if (!(GetAsyncKeyState( VK_MENU ) & 0x8000)) commit();
        else if (!revealed && GetTickCount() - started >= SHOW_DELAY) reveal();
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (!active) break;
        switch (wparam)
        {
        case VK_TAB: select_item( selected + (GetKeyState( VK_SHIFT ) < 0 ? -1 : 1) ); break;
        case VK_LEFT: select_item( selected - 1 ); break;
        case VK_RIGHT: select_item( selected + 1 ); break;
        case VK_UP: select_vertical( -1 ); break;
        case VK_DOWN: select_vertical( 1 ); break;
        case VK_RETURN: commit(); break;
        case VK_ESCAPE: cancel( TRUE ); break;
        case VK_DELETE: close_item( selected ); break;
        }
        return 0;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (wparam == VK_MENU) commit();
        return 0;

    case WM_SYSCOMMAND:
        if ((wparam & 0xfff0) == SC_KEYMENU) return 0;
        break;

    case WM_MOUSEMOVE:
    {
        POINT pt = { (short)LOWORD( lparam ), (short)HIWORD( lparam ) };
        TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, hwnd };
        BOOL on_close;
        int i = hit_test( pt, &on_close );

        if (i != hot || on_close != hot_close)
        {
            hot = i;
            hot_close = on_close;
            InvalidateRect( hwnd, NULL, TRUE );
        }
        TrackMouseEvent( &track );
        return 0;
    }

    case WM_MOUSELEAVE:
        if (hot != -1)
        {
            hot = -1;
            InvalidateRect( hwnd, NULL, TRUE );
        }
        return 0;

    case WM_LBUTTONUP:
    {
        POINT pt = { (short)LOWORD( lparam ), (short)HIWORD( lparam ) };
        BOOL on_close;
        int i = hit_test( pt, &on_close );

        if (i < 0) return 0;
        if (on_close) close_item( i );
        else
        {
            selected = i;
            commit();
        }
        return 0;
    }

    case WM_ACTIVATE:
        /* something else took the foreground */
        if (LOWORD( wparam ) == WA_INACTIVE && active) cancel( FALSE );
        return 0;

    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        on_paint();
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

static DWORD WINAPI switcher_thread( void *arg )
{
    WNDCLASSW class = { 0 };
    HANDLE ready = arg;
    MSG msg;

    class.lpfnWndProc = switcher_proc;
    class.hInstance = twinui_instance;
    class.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_ARROW );
    class.lpszClassName = L"ArcticTaskSwitcher";
    RegisterClassW( &class );
    switcher = CreateWindowExW( WS_EX_TOOLWINDOW | WS_EX_TOPMOST, class.lpszClassName, L"", WS_POPUP,
                                0, 0, 1, 1, NULL, NULL, twinui_instance, NULL );
    if (switcher)
    {
        font = shell_font( px( 12 ), FW_NORMAL );
        set_acrylic( switcher );
        set_transition( switcher, ARCTIC_TRANSITION_FADE );
        if (!RegisterHotKey( switcher, HOTKEY_NEXT, MOD_ALT, VK_TAB ) ||
            !RegisterHotKey( switcher, HOTKEY_PREVIOUS, MOD_ALT | MOD_SHIFT, VK_TAB ))
            ERR( "Alt+Tab is taken: %lu\n", GetLastError() );
    }
    SetEvent( ready );
    if (!switcher) return 1;

    while (GetMessageW( &msg, NULL, 0, 0 ))
    {
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }
    return 0;
}

BOOL switcher_start(void)
{
    HANDLE ready, thread;

    if (switcher) return TRUE;
    ready = CreateEventW( NULL, TRUE, FALSE, NULL );
    if (!(thread = CreateThread( NULL, 0, switcher_thread, ready, 0, NULL )))
    {
        CloseHandle( ready );
        return FALSE;
    }
    WaitForSingleObject( ready, 5000 );
    CloseHandle( ready );
    CloseHandle( thread );
    return switcher != NULL;
}
