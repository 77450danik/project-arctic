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

enum item { ITEM_COMMAND_PROMPT, ITEM_TASK_MANAGER, ITEM_RESTART, ITEM_SHUT_DOWN, ITEM_CANCEL, ITEM_LANGUAGE, ITEM_COUNT };

static const UINT item_text_ids[ITEM_COUNT] =
{
    IDS_COMMAND_PROMPT, IDS_TASK_MANAGER, IDS_RESTART, IDS_SHUT_DOWN, IDS_CANCEL, 0
};

static const WCHAR class_name[] = L"Arctic security options";
static const WCHAR flyout_class_name[] = L"Arctic input flyout";

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
    /* the input indicator, bottom right, as LogonUI's system buttons are */
    BOOL     has_language;        /* more than one input language */
    HKL      language;
    HFONT    small_font;
    HWND     flyout;
} screen = { .hot = -1, .pressed = -1 };

/* the list of languages the indicator opens, as the taskbar's: 300 wide on
 * #1E1E1E, 60 a language, its abbreviation in a 74 wide column (12pt
 * semibold), its name and keyboard (11pt) beside it, the current one on the
 * accent colour, #19FFFFFF under the pointer */
#define FLYOUT_WIDTH  300
#define FLYOUT_TILE   60
#define FLYOUT_COLUMN 74

static struct
{
    HKL   layouts[16];
    int   count;
    int   hot;
    HFONT abbr_font, name_font;
} flyout = { .hot = -1 };

/* the first three letters of the language's own name: ENG, УКР, РУС */
static void language_abbreviation( HKL hkl, WCHAR *abbr, int count )
{
    WCHAR name[80];

    abbr[0] = 0;
    if (!GetLocaleInfoW( MAKELCID( LOWORD(hkl), SORT_DEFAULT ), LOCALE_SNATIVELANGNAME, name, ARRAY_SIZE(name) ))
        return;
    lstrcpynW( abbr, name, min( count, 4 ) );
    CharUpperW( abbr );
}

static void language_names( HKL hkl, WCHAR *language, int language_count, WCHAR *keyboard, int keyboard_count )
{
    WCHAR key[128], layout[128] = L"", format[64];
    DWORD size = sizeof(layout);

    if (!GetLocaleInfoW( MAKELCID( LOWORD(hkl), SORT_DEFAULT ), LOCALE_SLANGUAGE, language, language_count ))
        language[0] = 0;
    wsprintfW( key, L"SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts\\%08X",
               (HIWORD(hkl) & 0xf000) == 0xf000 ? LOWORD(hkl) : HIWORD(hkl) );
    if (RegGetValueW( HKEY_LOCAL_MACHINE, key, L"Layout Text", RRF_RT_REG_SZ, NULL, layout, &size ))
        layout[0] = 0;
    LoadStringW( GetModuleHandleW( NULL ), IDS_INPUT_KEYBOARD, format, ARRAY_SIZE(format) );
    wsprintfW( keyboard, format, layout );
}

/* the language of the program that was in front */
static HKL current_language(void)
{
    if (screen.previous && IsWindow( screen.previous ))
        return GetKeyboardLayout( GetWindowThreadProcessId( screen.previous, NULL ) );
    return GetKeyboardLayout( 0 );
}

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

    /* LogonUI's system buttons: bottom right, 32 and 22 in, 48 high, at
     * least 48 wide with 8 either side of the text */
    SetRectEmpty( &screen.rects[ITEM_LANGUAGE] );
    if (screen.has_language)
    {
        SelectObject( dc, screen.small_font );
        GetTextExtentPoint32W( dc, screen.text[ITEM_LANGUAGE], lstrlenW( screen.text[ITEM_LANGUAGE] ), &size );
        size.cx = max( size.cx + 2 * scaled( height, 8 ), scaled( height, 48 ) );
        SetRect( &screen.rects[ITEM_LANGUAGE], width - scaled( height, 32 ) - size.cx,
                 height - scaled( height, 22 ) - scaled( height, 48 ),
                 width - scaled( height, 32 ), height - scaled( height, 22 ) );
    }

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

    for (int i = 0; i < ITEM_LANGUAGE; i++)
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

    /* the input indicator: #21FFFFFF and text at #CCFFFFFF under the pointer,
     * #66FFFFFF pressed */
    if (screen.has_language)
    {
        RECT r = screen.rects[ITEM_LANGUAGE];
        BOOL hot = screen.hot == ITEM_LANGUAGE, pressed = hot && screen.pressed == ITEM_LANGUAGE;
        COLORREF back = pressed ? blend( screen.background, 40 ) : hot ? blend( screen.background, 13 ) : screen.background;

        brush = CreateSolidBrush( back );
        FillRect( dc, &r, brush );
        DeleteObject( brush );
        if (ITEM_LANGUAGE == screen.focus && screen.focus_shown) frame( dc, &r, scaled( height, 2 ), RGB( 255, 255, 255 ) );
        SelectObject( dc, screen.small_font );
        SetTextColor( dc, hot && !pressed ? RGB( GetRValue(back) + (255 - GetRValue(back)) * 80 / 100,
                                                 GetGValue(back) + (255 - GetGValue(back)) * 80 / 100,
                                                 GetBValue(back) + (255 - GetBValue(back)) * 80 / 100 )
                                          : RGB( 255, 255, 255 ) );
        DrawTextW( dc, screen.text[ITEM_LANGUAGE], -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX );
        SetTextColor( dc, RGB( 255, 255, 255 ) );
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

/* ends the session and tells wininit.exe what to do with the machine */
void end_session( int exit_code )
{
    EnumWindows( end_session_window, 0 );
    if (exit_code == WINLOGON_EXIT_LOGOFF)
    {
        run_userinit(); /* one user and no logon screen: the shell starts again */
        return;
    }
    ExitProcess( exit_code );
}

static int flyout_height(void)
{
    return 5 + flyout.count * FLYOUT_TILE + 5;
}

static void close_flyout(void)
{
    if (screen.flyout) DestroyWindow( screen.flyout );
    screen.flyout = 0;
}

static void choose_language( HKL hkl )
{
    PostMessageW( GetDesktopWindow(), RegisterWindowMessageW( L"ArcticInputLanguage" ), 0, (LPARAM)hkl );
    screen.language = hkl;
    language_abbreviation( hkl, screen.text[ITEM_LANGUAGE], ARRAY_SIZE(screen.text[ITEM_LANGUAGE]) );
    close_flyout();
    if (screen.hwnd)
    {
        RECT client;
        GetClientRect( screen.hwnd, &client );
        layout( client.right, client.bottom );
        InvalidateRect( screen.hwnd, NULL, FALSE );
    }
}

static void fill_alpha( HDC dc, const RECT *rect, BYTE alpha )
{
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, alpha, 0 };
    HDC white = CreateCompatibleDC( dc );
    HBITMAP bitmap = CreateCompatibleBitmap( dc, 1, 1 );
    HGDIOBJ old = SelectObject( white, bitmap );

    SetPixel( white, 0, 0, RGB( 255, 255, 255 ) );
    GdiAlphaBlend( dc, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, white, 0, 0, 1, 1, bf );
    SelectObject( white, old );
    DeleteObject( bitmap );
    DeleteDC( white );
}

static void paint_flyout( HWND hwnd, HDC dc )
{
    HBRUSH brush;
    RECT client, tile, text;

    GetClientRect( hwnd, &client );
    brush = CreateSolidBrush( RGB( 0x1e, 0x1e, 0x1e ) );
    FillRect( dc, &client, brush );
    DeleteObject( brush );
    SetBkMode( dc, TRANSPARENT );
    SetTextColor( dc, RGB( 255, 255, 255 ) );
    for (int i = 0; i < flyout.count; i++)
    {
        WCHAR abbr[8], language[80], keyboard[128];
        HGDIOBJ old;

        SetRect( &tile, 0, 5 + i * FLYOUT_TILE, client.right, 5 + (i + 1) * FLYOUT_TILE );
        if (flyout.layouts[i] == screen.language)
        {
            brush = CreateSolidBrush( RGB( 0x00, 0x78, 0xd7 ) );
            FillRect( dc, &tile, brush );
            DeleteObject( brush );
        }
        if (i == flyout.hot) fill_alpha( dc, &tile, 0x19 );
        language_abbreviation( flyout.layouts[i], abbr, ARRAY_SIZE(abbr) );
        language_names( flyout.layouts[i], language, ARRAY_SIZE(language), keyboard, ARRAY_SIZE(keyboard) );
        old = SelectObject( dc, flyout.abbr_font );
        SetRect( &text, 0, tile.top, FLYOUT_COLUMN - 14, tile.top + 30 );
        DrawTextW( dc, abbr, -1, &text, DT_CENTER | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX );
        SelectObject( dc, flyout.name_font );
        SetRect( &text, FLYOUT_COLUMN, tile.top, client.right - 22, tile.top + 30 );
        DrawTextW( dc, language, -1, &text, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS );
        SetRect( &text, FLYOUT_COLUMN, tile.top + 30, client.right - 22, tile.bottom );
        DrawTextW( dc, keyboard, -1, &text, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS );
        SelectObject( dc, old );
    }
}

static int flyout_tile_at( POINT pt )
{
    if (pt.y < 5 || pt.y >= 5 + flyout.count * FLYOUT_TILE) return -1;
    return (pt.y - 5) / FLYOUT_TILE;
}

static LRESULT WINAPI flyout_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
    int tile;

    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint( hwnd, &ps );
        paint_flyout( hwnd, dc );
        EndPaint( hwnd, &ps );
        return 0;
    }
    case WM_ERASEBKGND:
        return TRUE;
    case WM_MOUSEMOVE:
        if ((tile = flyout_tile_at( pt )) != flyout.hot)
        {
            TRACKMOUSEEVENT tme = { .cbSize = sizeof(tme), .dwFlags = TME_LEAVE, .hwndTrack = hwnd };
            TrackMouseEvent( &tme );
            flyout.hot = tile;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        return 0;
    case WM_MOUSELEAVE:
        flyout.hot = -1;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    case WM_LBUTTONUP:
        if ((tile = flyout_tile_at( pt )) >= 0) choose_language( flyout.layouts[tile] );
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) close_flyout();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) PostMessageW( hwnd, WM_CLOSE, 0, 0 );
        return 0;
    case WM_CLOSE:
        close_flyout();
        return 0;
    case WM_DESTROY:
        if (flyout.abbr_font) DeleteObject( flyout.abbr_font );
        if (flyout.name_font) DeleteObject( flyout.name_font );
        flyout.abbr_font = flyout.name_font = 0;
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wp, lp );
}

/* above the indicator, its right edge on the indicator's */
static void open_flyout(void)
{
    static BOOL registered;
    RECT r = screen.rects[ITEM_LANGUAGE];
    POINT corner;

    if (screen.flyout) return;
    if (!registered)
    {
        WNDCLASSW class = { .style = CS_DROPSHADOW, .lpfnWndProc = flyout_proc,
                            .hInstance = GetModuleHandleW( NULL ), .lpszClassName = flyout_class_name };
        if (!RegisterClassW( &class )) return;
        registered = TRUE;
    }
    flyout.count = GetKeyboardLayoutList( ARRAY_SIZE(flyout.layouts), flyout.layouts );
    flyout.hot = -1;
    flyout.abbr_font = CreateFontW( -16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                    CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI" );
    flyout.name_font = CreateFontW( -15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                    CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI" );
    corner.x = r.right;
    corner.y = r.top;
    ClientToScreen( screen.hwnd, &corner );
    screen.flyout = CreateWindowExW( WS_EX_TOPMOST | WS_EX_TOOLWINDOW, flyout_class_name, NULL, WS_POPUP,
                                     corner.x - FLYOUT_WIDTH, corner.y - flyout_height(), FLYOUT_WIDTH, flyout_height(),
                                     screen.hwnd, 0, GetModuleHandleW( NULL ), NULL );
    if (!screen.flyout) return;
    ShowWindow( screen.flyout, SW_SHOW );
    SetForegroundWindow( screen.flyout );
}

static void activate( int item )
{
    WCHAR cmd[] = L"cmd.exe", taskmgr[] = L"taskmgr.exe";

    switch (item)
    {
    case ITEM_LANGUAGE:
        open_flyout();
        break;
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
    int count = screen.has_language ? ITEM_COUNT : ITEM_LANGUAGE;

    if (screen.focus_shown) screen.focus = (item + count) % count;
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
        close_flyout();
        if (screen.font) DeleteObject( screen.font );
        if (screen.small_font) DeleteObject( screen.small_font );
        screen.font = screen.small_font = 0;
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
    /* LogonUI's buttons are XAML's 14 */
    screen.small_font = CreateFontW( -scaled( screen_height, 14 ), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH, L"Segoe UI" );
    screen.hot = screen.pressed = -1;
    screen.focus = 0;
    screen.focus_shown = FALSE;
    screen.tracking = FALSE;
    screen.previous = GetForegroundWindow();
    screen.has_language = GetKeyboardLayoutList( 0, NULL ) > 1;
    screen.language = current_language();
    language_abbreviation( screen.language, screen.text[ITEM_LANGUAGE], ARRAY_SIZE(screen.text[ITEM_LANGUAGE]) );

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
