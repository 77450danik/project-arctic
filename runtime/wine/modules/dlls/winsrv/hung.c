/*
 * Arctic window server: programs that do not respond
 *
 * As in Windows: a window whose thread has not taken its messages for five
 * seconds (wineserver's word for it, IsHungAppWindow) gets a ghost laid over
 * it, whitened, with "(Не відповідає)" after its title. A click on the ghost
 * asks whether to close the program or to wait for it, and nothing is ended
 * without that answer. The ghost goes as soon as the program answers again.
 *
 * Nothing here sends the hung window a message, which would hang us too:
 * where it is, its title and its process all come from wineserver.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "shellscalingapi.h"
#include "wine/debug.h"

#include "winsrv_private.h"
#include "resource.h"

WINE_DEFAULT_DEBUG_CHANNEL(winsrv);

#define MAX_GHOSTS 32
#define SCAN_MS    1000
/* the ghost whitens what the program last drew, premultiplied white */
#define WASH_ALPHA 0xa0
#define FRAME      0xff999999

#define ID_INSTRUCTION 10
#define ID_CONTENT     11
#define ID_CLOSE       12
#define ID_WAIT        13

#define DIALOG_STYLE    (WS_POPUP | WS_CAPTION | WS_SYSMENU)
#define DIALOG_EX_STYLE (WS_EX_DLGMODALFRAME | WS_EX_TOPMOST)

static const WCHAR ghost_class[]  = L"Ghost";
static const WCHAR dialog_class[] = L"ArcticHungAppDialog";

struct ghost
{
    HWND  target;     /* the window that does not respond */
    HWND  hwnd;       /* ours, over it */
    RECT  rect;       /* where it was when the ghost was drawn */
    UINT  dpi;
    WCHAR title[256];
};

static struct ghost ghosts[MAX_GHOSTS];
static HINSTANCE    instance;
static HWND         pending;   /* a ghost was clicked: ask about its window */

/* the question, while it is asked */
static struct
{
    HWND  hwnd;
    HWND  target;
    BOOL  close;
    UINT  dpi;
    HFONT font, big_font;
    WCHAR instruction[320], content[256];
    RECT  instruction_rect, content_rect, close_rect, wait_rect;
} question;

static int scale( int value, UINT dpi )
{
    return MulDiv( value, dpi, USER_DEFAULT_SCREEN_DPI );
}

/* the shell's own windows stand for the desktop, not for a program */
static BOOL is_shell_window( HWND hwnd )
{
    static const WCHAR *const classes[] = { L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd" };
    WCHAR name[64];

    if (!GetClassNameW( hwnd, name, ARRAY_SIZE(name) )) return TRUE;
    for (UINT i = 0; i < ARRAY_SIZE(classes); i++)
        if (!lstrcmpiW( name, classes[i] )) return TRUE;
    return FALSE;
}

/* a program's window on the screen, as the taskbar would show it */
static BOOL can_ghost( HWND hwnd )
{
    LONG style = GetWindowLongW( hwnd, GWL_STYLE ), ex_style = GetWindowLongW( hwnd, GWL_EXSTYLE );
    DWORD pid = 0;
    RECT rect;

    if (!(style & WS_VISIBLE) || (style & (WS_CHILD | WS_MINIMIZE))) return FALSE;
    if (ex_style & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT)) return FALSE;
    if (!GetWindowThreadProcessId( hwnd, &pid ) || pid == GetCurrentProcessId()) return FALSE;
    if (!GetWindowRect( hwnd, &rect ) || IsRectEmpty( &rect )) return FALSE;
    return !is_shell_window( hwnd );
}

/* its title, or the name of its program when it has none */
static void program_name( HWND hwnd, WCHAR *name, int size )
{
    WCHAR path[MAX_PATH], *file;
    DWORD pid = 0, len = ARRAY_SIZE(path);
    HANDLE process;

    if (InternalGetWindowText( hwnd, name, size ) > 0) return;
    name[0] = 0;
    GetWindowThreadProcessId( hwnd, &pid );
    if (!(process = OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid ))) return;
    if (QueryFullProcessImageNameW( process, 0, path, &len ))
    {
        file = wcsrchr( path, '\\' );
        lstrcpynW( name, file ? file + 1 : path, size );
    }
    CloseHandle( process );
}

static struct ghost *find_ghost( HWND target )
{
    for (UINT i = 0; i < MAX_GHOSTS; i++)
        if (ghosts[i].target == target) return &ghosts[i];
    return NULL;
}

/* the caption of the window, down to its client area; a program that draws
 * its own (a browser) gets the height of a standard one */
static int caption_height( const struct ghost *g )
{
    int standard = GetSystemMetricsForDpi( SM_CYCAPTION, g->dpi ) + GetSystemMetricsForDpi( SM_CYSIZEFRAME, g->dpi ) +
                   GetSystemMetricsForDpi( SM_CXPADDEDBORDER, g->dpi );
    POINT client = { 0, 0 };
    int height;

    if (!ClientToScreen( g->target, &client )) return standard;
    height = client.y - g->rect.top;
    return height > standard / 2 && height < standard * 2 ? height : standard;
}

static void draw_ghost( struct ghost *g )
{
    int width = g->rect.right - g->rect.left, height = g->rect.bottom - g->rect.top;
    BITMAPINFO info = { .bmiHeader = { sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB } };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    NONCLIENTMETRICSW metrics = { .cbSize = sizeof(metrics) };
    POINT origin = { 0, 0 }, pos = { g->rect.left, g->rect.top };
    SIZE size = { width, height };
    WCHAR format[64], text[ARRAY_SIZE(g->title) + 64];
    HFONT font = 0, old_font = 0;
    HBITMAP bitmap, old_bitmap;
    HPEN pen, old_pen;
    UINT32 *bits;
    int band, pad, button, cx, cy, arm;
    RECT rc;
    HDC dc;

    if (width <= 0 || height <= 0 || !(dc = CreateCompatibleDC( 0 ))) return;
    if (!(bitmap = CreateDIBSection( dc, &info, DIB_RGB_COLORS, (void **)&bits, NULL, 0 )))
    {
        DeleteDC( dc );
        return;
    }
    old_bitmap = SelectObject( dc, bitmap );
    band = min( caption_height( g ), height );
    pad = scale( 8, g->dpi );
    button = scale( 46, g->dpi );

    /* the caption, opaque, with the title and a close button */
    SetRect( &rc, 0, 0, width, band );
    FillRect( dc, &rc, GetStockObject( WHITE_BRUSH ) );
    if (SystemParametersInfoForDpi( SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, g->dpi ) &&
        (font = CreateFontIndirectW( &metrics.lfCaptionFont )))
        old_font = SelectObject( dc, font );
    LoadStringW( instance, IDS_NOT_RESPONDING, format, ARRAY_SIZE(format) );
    swprintf( text, ARRAY_SIZE(text), format, g->title );
    SetBkMode( dc, TRANSPARENT );
    SetTextColor( dc, RGB( 0, 0, 0 ) );
    SetRect( &rc, pad, 0, max( pad, width - button - pad ), band );
    DrawTextW( dc, text, -1, &rc, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX );
    if (width > button * 2)
    {
        cx = width - button / 2;
        cy = band / 2;
        arm = scale( 5, g->dpi );
        pen = CreatePen( PS_SOLID, max( 1, scale( 1, g->dpi ) ), RGB( 0, 0, 0 ) );
        old_pen = SelectObject( dc, pen );
        MoveToEx( dc, cx - arm, cy - arm, NULL );
        LineTo( dc, cx + arm + 1, cy + arm + 1 );
        MoveToEx( dc, cx + arm, cy - arm, NULL );
        LineTo( dc, cx - arm - 1, cy + arm + 1 );
        SelectObject( dc, old_pen );
        DeleteObject( pen );
    }
    if (font)
    {
        SelectObject( dc, old_font );
        DeleteObject( font );
    }
    GdiFlush();

    /* GDI leaves alpha at 0: the caption is made opaque here, the rest is
     * the white wash with a thin frame round it */
    for (int y = 0; y < height; y++)
    {
        UINT32 *row = bits + (SIZE_T)y * width;

        for (int x = 0; x < width; x++)
        {
            if (y < band) row[x] |= 0xff000000;
            else if (x == 0 || x == width - 1 || y == height - 1) row[x] = FRAME;
            else row[x] = WASH_ALPHA * 0x01010101u;
        }
    }
    if (!UpdateLayeredWindow( g->hwnd, NULL, &pos, &size, dc, &origin, 0, &blend, ULW_ALPHA ))
        WARN( "cannot draw the ghost of %p: %lu\n", g->target, GetLastError() );
    SelectObject( dc, old_bitmap );
    DeleteObject( bitmap );
    DeleteDC( dc );
}

/* right over its window, whatever came over that since */
static void place_ghost( struct ghost *g )
{
    HWND above = GetWindow( g->target, GW_HWNDPREV ), after;
    BOOL topmost = (GetWindowLongW( g->target, GWL_EXSTYLE ) & WS_EX_TOPMOST) != 0;

    if (above == g->hwnd && IsWindowVisible( g->hwnd )) return;
    if (above && (topmost || !(GetWindowLongW( above, GWL_EXSTYLE ) & WS_EX_TOPMOST))) after = above;
    else after = topmost ? HWND_TOPMOST : HWND_TOP;
    SetWindowPos( g->hwnd, after, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW );
}

/* the monitor's: a program that knows nothing of DPI has 96 for its windows,
 * and the ghost and the question would come out small on a 125 % screen */
static UINT monitor_dpi( HWND hwnd )
{
    UINT x = USER_DEFAULT_SCREEN_DPI, y;

    GetDpiForMonitor( MonitorFromWindow( hwnd, MONITOR_DEFAULTTONEAREST ), MDT_EFFECTIVE_DPI, &x, &y );
    return x;
}

static void update_ghost( struct ghost *g )
{
    WCHAR title[ARRAY_SIZE(g->title)];
    UINT dpi = monitor_dpi( g->target );
    RECT rect;

    GetWindowRect( g->target, &rect );
    program_name( g->target, title, ARRAY_SIZE(title) );
    if (!EqualRect( &rect, &g->rect ) || dpi != g->dpi || wcscmp( title, g->title ))
    {
        g->rect = rect;
        g->dpi = dpi ? dpi : USER_DEFAULT_SCREEN_DPI;
        lstrcpynW( g->title, title, ARRAY_SIZE(g->title) );
        draw_ghost( g );
    }
    place_ghost( g );
}

static void remove_ghost( struct ghost *g )
{
    TRACE( "%p answers again\n", g->target );
    DestroyWindow( g->hwnd );
    memset( g, 0, sizeof(*g) );
}

static BOOL CALLBACK find_hung( HWND hwnd, LPARAM param )
{
    struct ghost *g;

    if (find_ghost( hwnd ) || !can_ghost( hwnd ) || !IsHungAppWindow( hwnd )) return TRUE;
    if (!(g = find_ghost( NULL ))) return FALSE;

    g->target = hwnd;
    if (!(g->hwnd = CreateWindowExW( WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, ghost_class, NULL, WS_POPUP,
                                     0, 0, 0, 0, 0, 0, instance, NULL )))
    {
        g->target = 0;
        return TRUE;
    }
    SetWindowLongPtrW( g->hwnd, GWLP_USERDATA, (LONG_PTR)hwnd );
    update_ghost( g );
    MESSAGE( "csrss: %s (%p) does not respond\n", debugstr_w(g->title), hwnd );
    return TRUE;
}

static void CALLBACK scan( HWND hwnd, UINT msg, UINT_PTR id, DWORD time )
{
    for (UINT i = 0; i < MAX_GHOSTS; i++)
    {
        struct ghost *g = &ghosts[i];

        if (!g->target) continue;
        if (!IsWindow( g->target ) || !can_ghost( g->target ) || !IsHungAppWindow( g->target )) remove_ghost( g );
        else update_ghost( g );
    }
    EnumWindows( find_hung, 0 );
    /* the program answered, or is gone, while it was asked about */
    if (question.hwnd && !find_ghost( question.target )) DestroyWindow( question.hwnd );
}

/**********************************************************************
 *          The question
 */

static void create_child( HWND parent, const WCHAR *class, const WCHAR *text, DWORD style, const RECT *rect,
                          UINT id, HFONT font )
{
    HWND child = CreateWindowExW( 0, class, text, WS_CHILD | WS_VISIBLE | style, rect->left, rect->top,
                                  rect->right - rect->left, rect->bottom - rect->top, parent,
                                  (HMENU)(UINT_PTR)id, instance, NULL );

    if (child) SendMessageW( child, WM_SETFONT, (WPARAM)font, FALSE );
}

static LRESULT CALLBACK dialog_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    WCHAR text[64];

    switch (msg)
    {
    case WM_CREATE:
        create_child( hwnd, L"STATIC", question.instruction, SS_LEFT | SS_NOPREFIX, &question.instruction_rect,
                      ID_INSTRUCTION, question.big_font );
        create_child( hwnd, L"STATIC", question.content, SS_LEFT | SS_NOPREFIX, &question.content_rect,
                      ID_CONTENT, question.font );
        LoadStringW( instance, IDS_HUNG_CLOSE, text, ARRAY_SIZE(text) );
        create_child( hwnd, L"BUTTON", text, BS_PUSHBUTTON | WS_TABSTOP, &question.close_rect, ID_CLOSE,
                      question.font );
        LoadStringW( instance, IDS_HUNG_WAIT, text, ARRAY_SIZE(text) );
        create_child( hwnd, L"BUTTON", text, BS_DEFPUSHBUTTON | WS_TABSTOP, &question.wait_rect, ID_WAIT,
                      question.font );
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkColor( (HDC)wp, RGB( 255, 255, 255 ) );
        /* the main instruction of a Windows task dialog */
        if (GetDlgCtrlID( (HWND)lp ) == ID_INSTRUCTION) SetTextColor( (HDC)wp, RGB( 0, 51, 153 ) );
        return (LRESULT)GetStockObject( WHITE_BRUSH );
    case WM_COMMAND:
        if (HIWORD( wp ) > 1) break;
        question.close = LOWORD( wp ) == ID_CLOSE;
        DestroyWindow( hwnd );
        return 0;
    case WM_CLOSE:
        DestroyWindow( hwnd );
        return 0;
    case WM_DESTROY:
        question.hwnd = 0;
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wp, lp );
}

static void end_program( HWND target )
{
    DWORD pid = 0;
    HANDLE process;

    GetWindowThreadProcessId( target, &pid );
    if (!pid || pid == GetCurrentProcessId()) return;
    if (!(process = OpenProcess( PROCESS_TERMINATE, FALSE, pid )))
    {
        ERR( "cannot end process %04lx: %lu\n", pid, GetLastError() );
        return;
    }
    MESSAGE( "csrss: the user ended process %04lx, which did not respond\n", pid );
    TerminateProcess( process, 1 );
    CloseHandle( process );
}

/* "<program> does not respond": close it, or wait for it */
static void ask( HWND target )
{
    struct ghost *g = find_ghost( target );
    NONCLIENTMETRICSW metrics = { .cbSize = sizeof(metrics) };
    MONITORINFO monitor = { .cbSize = sizeof(monitor) };
    WCHAR format[128];
    int width, margin, button, y, w, h, x;
    RECT frame;
    HDC dc;
    MSG msg;

    if (!g || question.hwnd) return;
    memset( &question, 0, sizeof(question) );
    question.target = target;
    question.dpi = g->dpi;
    SystemParametersInfoForDpi( SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, question.dpi );
    question.font = CreateFontIndirectW( &metrics.lfMessageFont );
    metrics.lfMessageFont.lfHeight = metrics.lfMessageFont.lfHeight * 4 / 3;
    question.big_font = CreateFontIndirectW( &metrics.lfMessageFont );
    LoadStringW( instance, IDS_HUNG_INSTRUCTION, format, ARRAY_SIZE(format) );
    swprintf( question.instruction, ARRAY_SIZE(question.instruction), format, g->title );
    LoadStringW( instance, IDS_HUNG_CONTENT, question.content, ARRAY_SIZE(question.content) );

    width = scale( 440, question.dpi );
    margin = scale( 20, question.dpi );
    button = scale( 34, question.dpi );
    dc = GetDC( 0 );
    SelectObject( dc, question.big_font );
    SetRect( &question.instruction_rect, margin, margin, width - margin, margin );
    DrawTextW( dc, question.instruction, -1, &question.instruction_rect, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX );
    y = question.instruction_rect.bottom + scale( 12, question.dpi );
    SelectObject( dc, question.font );
    SetRect( &question.content_rect, margin, y, width - margin, y );
    DrawTextW( dc, question.content, -1, &question.content_rect, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX );
    ReleaseDC( 0, dc );
    y = question.content_rect.bottom + margin;
    SetRect( &question.close_rect, margin, y, width - margin, y + button );
    y += button + scale( 8, question.dpi );
    SetRect( &question.wait_rect, margin, y, width - margin, y + button );
    y += button + margin;

    /* over the window it asks about, inside that window's monitor */
    SetRect( &frame, 0, 0, width, y );
    AdjustWindowRectExForDpi( &frame, DIALOG_STYLE, FALSE, DIALOG_EX_STYLE, question.dpi );
    w = frame.right - frame.left;
    h = frame.bottom - frame.top;
    GetMonitorInfoW( MonitorFromRect( &g->rect, MONITOR_DEFAULTTONEAREST ), &monitor );
    x = max( monitor.rcWork.left, min( (g->rect.left + g->rect.right - w) / 2, monitor.rcWork.right - w ) );
    y = max( monitor.rcWork.top, min( (g->rect.top + g->rect.bottom - h) / 2, monitor.rcWork.bottom - h ) );

    question.hwnd = CreateWindowExW( DIALOG_EX_STYLE, dialog_class, g->title, DIALOG_STYLE, x, y, w, h, 0, 0,
                                     instance, NULL );
    if (question.hwnd)
    {
        ShowWindow( question.hwnd, SW_SHOWNORMAL );
        SetForegroundWindow( question.hwnd );
        SetFocus( GetDlgItem( question.hwnd, ID_WAIT ) );
    }
    while (question.hwnd)
    {
        if (!GetMessageW( &msg, 0, 0, 0 ))
        {
            PostQuitMessage( msg.wParam );
            break;
        }
        if (IsDialogMessageW( question.hwnd, &msg )) continue;
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }
    DeleteObject( question.font );
    DeleteObject( question.big_font );
    if (!question.close) return;
    end_program( target );
    if ((g = find_ghost( target ))) remove_ghost( g );
}

/**********************************************************************
 *          The ghosts
 */

static LRESULT CALLBACK ghost_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    switch (msg)
    {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_SETCURSOR:
        SetCursor( LoadCursorW( 0, (LPCWSTR)IDC_WAIT ) );
        return TRUE;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        /* asked from the thread's own loop: the ghost may go while it is */
        if (question.hwnd) SetForegroundWindow( question.hwnd );
        else pending = (HWND)GetWindowLongPtrW( hwnd, GWLP_USERDATA );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wp, lp );
}

static DWORD WINAPI hung_app_thread( void *arg )
{
    WNDCLASSW ghost = { .lpfnWndProc = ghost_proc, .hInstance = instance, .lpszClassName = ghost_class };
    WNDCLASSW dialog = { .lpfnWndProc = dialog_proc, .hInstance = instance,
                         .hCursor = LoadCursorW( 0, (LPCWSTR)IDC_ARROW ),
                         .hbrBackground = GetStockObject( WHITE_BRUSH ), .lpszClassName = dialog_class };
    HWND target;
    MSG msg;

    SetThreadDescription( GetCurrentThread(), L"HungAppThread" );
    SetThreadDpiAwarenessContext( DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 );
    if (!RegisterClassW( &ghost ) || !RegisterClassW( &dialog ))
    {
        ERR( "cannot register the ghost classes: %lu\n", GetLastError() );
        return 0;
    }
    SetTimer( NULL, 0, SCAN_MS, scan );
    while (GetMessageW( &msg, 0, 0, 0 ))
    {
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
        if ((target = pending))
        {
            pending = 0;
            ask( target );
        }
    }
    return 0;
}

void start_hung_app_thread(void)
{
    HANDLE thread;

    instance = GetModuleHandleW( L"winsrv.dll" );
    if ((thread = CreateThread( NULL, 0, hung_app_thread, NULL, 0, NULL ))) CloseHandle( thread );
}
