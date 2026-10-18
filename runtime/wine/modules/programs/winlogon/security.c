/*
 * Windows Logon Application: the security options screen (Ctrl+Alt+Del)
 *
 * A full-screen black page with the options in a column and a Cancel
 * button, as in Windows 11: 20 pixel text, 44 a row, the row under the
 * pointer a rounded lighter field; Cancel a rounded button of 32 with a thin
 * border. It is laid out in effective pixels at the screen's scale, in a
 * window that knows the DPI, so nothing is stretched. It belongs to
 * winlogon.exe, which the system cannot run without.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <windows.h>
#include <math.h>

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
    UINT     dpi;                 /* of the screen: the window knows it, nothing is stretched */
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
#define FLYOUT_WIDTH  px(300)
#define FLYOUT_TILE   px(60)
#define FLYOUT_COLUMN px(74)

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

/* the session's language, which csrss keeps; before one is chosen, the
 * language of the program that was in front */
static HKL current_language(void)
{
    DWORD_PTR hkl = 0;

    if (SendMessageTimeoutW( GetDesktopWindow(), RegisterWindowMessageW( L"ArcticInputLanguage" ), 1, 0,
                             SMTO_ABORTIFHUNG, 200, &hkl ) && hkl)
        return (HKL)hkl;
    if (screen.previous && IsWindow( screen.previous ))
        return GetKeyboardLayout( GetWindowThreadProcessId( screen.previous, NULL ) );
    return GetKeyboardLayout( 0 );
}

/* Windows 11's dark theme over the black page: the controls' fills and
 * strokes as its resources give them (ControlFillColorDefault, ...Secondary,
 * ...Tertiary; ControlStrokeColorDefault), as straight white at an alpha */
#define FILL_REST       0x0f   /* #0FFFFFFF */
#define FILL_HOVER      0x15   /* #15FFFFFF */
#define FILL_PRESSED    0x08   /* #08FFFFFF */
#define STROKE          0x17   /* #17FFFFFF */
#define ROW_HOVER       0x0f   /* SubtleFillColorSecondary */
#define ROW_PRESSED     0x0a   /* SubtleFillColorTertiary */

/* effective pixels at the screen's scale */
static int px( int n )
{
    return MulDiv( n, screen.dpi, 96 );
}

static void layout( int width, int height )
{
    HDC dc = GetDC( screen.hwnd );
    HFONT old = SelectObject( dc, screen.font );
    int pad_x = px( 16 ), item_h = px( 44 ), column_w = px( 220 ), top;
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

    /* Cancel: a standard button, 32 high, 120 wide at least, 12 either side of its text */
    SelectObject( dc, screen.small_font );
    GetTextExtentPoint32W( dc, screen.text[ITEM_CANCEL], lstrlenW( screen.text[ITEM_CANCEL] ), &size );
    size.cx = max( size.cx + 2 * px( 12 ), px( 120 ) );
    SetRect( &screen.rects[ITEM_CANCEL], (width - size.cx) / 2, height - px( 104 ) - px( 32 ),
             (width + size.cx) / 2, height - px( 104 ) );

    /* LogonUI's system buttons: bottom right, 32 and 22 in, 48 high, at
     * least 48 wide with 8 either side of the text */
    SetRectEmpty( &screen.rects[ITEM_LANGUAGE] );
    if (screen.has_language)
    {
        GetTextExtentPoint32W( dc, screen.text[ITEM_LANGUAGE], lstrlenW( screen.text[ITEM_LANGUAGE] ), &size );
        size.cx = max( size.cx + 2 * px( 8 ), px( 48 ) );
        SetRect( &screen.rects[ITEM_LANGUAGE], width - px( 32 ) - size.cx, height - px( 22 ) - px( 48 ),
                 width - px( 32 ), height - px( 22 ) );
    }

    SelectObject( dc, old );
    ReleaseDC( screen.hwnd, dc );
}

/* a rounded rectangle of white at an alpha over the opaque page, its edges
 * antialiased (4 x 4 samples a pixel); only its edge, ring wide, when ring >= 0 */
static void round_rect( UINT32 *bits, int stride, int rows, const RECT *rect, float radius, BYTE alpha, float ring )
{
    for (int y = max( (int)rect->top, 0 ); y < min( (int)rect->bottom, rows ); y++)
    {
        for (int x = max( (int)rect->left, 0 ); x < min( (int)rect->right, stride ); x++)
        {
            int hits = 0;
            UINT32 a, p;

            for (int j = 0; j < 4; j++)
            {
                for (int i = 0; i < 4; i++)
                {
                    float sx = x + (i + 0.5f) / 4 - rect->left, sy = y + (j + 0.5f) / 4 - rect->top;
                    float w = rect->right - rect->left, h = rect->bottom - rect->top;
                    float dx = fmaxf( fmaxf( radius - sx, sx - (w - radius) ), 0 );
                    float dy = fmaxf( fmaxf( radius - sy, sy - (h - radius) ), 0 );
                    float d = sqrtf( dx * dx + dy * dy );
                    BOOL in = d <= radius;

                    if (in && ring >= 0)
                    {
                        /* inside the shape shrunk by the ring's width: not on it */
                        float ix = sx - ring, iy = sy - ring, iw = w - 2 * ring, ih = h - 2 * ring, ir = fmaxf( radius - ring, 0 );
                        float ex = fmaxf( fmaxf( ir - ix, ix - (iw - ir) ), 0 );
                        float ey = fmaxf( fmaxf( ir - iy, iy - (ih - ir) ), 0 );
                        if (ix >= 0 && iy >= 0 && ix <= iw && iy <= ih && sqrtf( ex * ex + ey * ey ) <= ir) in = FALSE;
                    }
                    hits += in;
                }
            }
            if (!hits) continue;
            a = alpha * hits / 16;
            p = bits[y * stride + x];
            bits[y * stride + x] = (((p >> 16) & 0xff) * (255 - a) / 255 + a) << 16 |
                                   (((p >> 8) & 0xff) * (255 - a) / 255 + a) << 8 |
                                   ((p & 0xff) * (255 - a) / 255 + a);
        }
    }
}

static void paint( HDC target )
{
    BITMAPINFO info = { { sizeof(info.bmiHeader), 0, 0, 1, 32, BI_RGB } };
    RECT client;
    HDC dc;
    HBITMAP bitmap, old_bitmap;
    HFONT old_font;
    UINT32 *bits;
    int pad_x = px( 16 );

    GetClientRect( screen.hwnd, &client );
    info.bmiHeader.biWidth = client.right;
    info.bmiHeader.biHeight = -client.bottom;
    dc = CreateCompatibleDC( target );
    if (!(bitmap = CreateDIBSection( target, &info, DIB_RGB_COLORS, (void **)&bits, NULL, 0 )))
    {
        DeleteDC( dc );
        return;
    }
    old_bitmap = SelectObject( dc, bitmap );
    /* black, as Windows 11's page in the dark */
    memset( bits, 0, (size_t)client.right * client.bottom * 4 );

    /* the fields first, then the text over them */
    for (int i = 0; i < ITEM_COUNT; i++)
    {
        RECT r = screen.rects[i], f;
        BOOL hot = screen.hot == i, pressed = hot && screen.pressed == i;

        if (IsRectEmpty( &r )) continue;
        if (i == ITEM_CANCEL)
        {
            round_rect( bits, client.right, client.bottom, &r, px( 4 ), pressed ? FILL_PRESSED : hot ? FILL_HOVER : FILL_REST, -1 );
            round_rect( bits, client.right, client.bottom, &r, px( 4 ), STROKE, 1 );
        }
        else if (hot || pressed)
            round_rect( bits, client.right, client.bottom, &r, px( 4 ), pressed ? ROW_PRESSED : ROW_HOVER, -1 );
        if (i == screen.focus && screen.focus_shown)
        {
            /* the focus visual: a white ring of 2, 3 outside */
            f = r;
            InflateRect( &f, px( 3 ), px( 3 ) );
            round_rect( bits, client.right, client.bottom, &f, px( 7 ), 255, px( 2 ) );
        }
    }
    GdiFlush();

    old_font = SelectObject( dc, screen.font );
    SetBkMode( dc, TRANSPARENT );
    SetTextColor( dc, RGB( 255, 255, 255 ) );
    for (int i = 0; i < ITEM_CANCEL; i++)
    {
        RECT r = screen.rects[i];
        r.left += pad_x;
        DrawTextW( dc, screen.text[i], -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX );
    }
    SelectObject( dc, screen.small_font );
    {
        RECT r = screen.rects[ITEM_CANCEL];
        /* TextFillColorSecondary pressed */
        if (screen.pressed == ITEM_CANCEL && screen.hot == ITEM_CANCEL) SetTextColor( dc, RGB( 0xc5, 0xc5, 0xc5 ) );
        DrawTextW( dc, screen.text[ITEM_CANCEL], -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX );
        SetTextColor( dc, RGB( 255, 255, 255 ) );
    }
    if (screen.has_language)
        DrawTextW( dc, screen.text[ITEM_LANGUAGE], -1, &screen.rects[ITEM_LANGUAGE],
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX );

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

static int px( int n );

static int flyout_height(void)
{
    return px( 5 ) + flyout.count * FLYOUT_TILE + px( 5 );
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
    /* Windows 11's flyouts in the dark: #2C2C2C */
    brush = CreateSolidBrush( RGB( 0x2c, 0x2c, 0x2c ) );
    FillRect( dc, &client, brush );
    DeleteObject( brush );
    SetBkMode( dc, TRANSPARENT );
    SetTextColor( dc, RGB( 255, 255, 255 ) );
    for (int i = 0; i < flyout.count; i++)
    {
        WCHAR abbr[8], language[80], keyboard[128];
        HGDIOBJ old;

        SetRect( &tile, 0, px( 5 ) + i * FLYOUT_TILE, client.right, px( 5 ) + (i + 1) * FLYOUT_TILE );
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
        SetRect( &text, 0, tile.top, FLYOUT_COLUMN - px( 14 ), tile.top + px( 30 ) );
        DrawTextW( dc, abbr, -1, &text, DT_CENTER | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX );
        SelectObject( dc, flyout.name_font );
        SetRect( &text, FLYOUT_COLUMN, tile.top, client.right - px( 22 ), tile.top + px( 30 ) );
        DrawTextW( dc, language, -1, &text, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS );
        SetRect( &text, FLYOUT_COLUMN, tile.top + px( 30 ), client.right - px( 22 ), tile.bottom );
        DrawTextW( dc, keyboard, -1, &text, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS );
        SelectObject( dc, old );
    }
}

static int flyout_tile_at( POINT pt )
{
    if (pt.y < px( 5 ) || pt.y >= px( 5 ) + flyout.count * FLYOUT_TILE) return -1;
    return (pt.y - px( 5 )) / FLYOUT_TILE;
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
    DPI_AWARENESS_CONTEXT old_context;
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
    flyout.abbr_font = CreateFontW( -px( 16 ), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                    CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI" );
    flyout.name_font = CreateFontW( -px( 15 ), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                    CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI" );
    corner.x = r.right;
    corner.y = r.top;
    ClientToScreen( screen.hwnd, &corner );
    /* in physical pixels, as the page */
    old_context = SetThreadDpiAwarenessContext( DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 );
    screen.flyout = CreateWindowExW( WS_EX_TOPMOST | WS_EX_TOOLWINDOW, flyout_class_name, NULL, WS_POPUP,
                                     corner.x - FLYOUT_WIDTH, corner.y - flyout_height(), FLYOUT_WIDTH, flyout_height(),
                                     screen.hwnd, 0, GetModuleHandleW( NULL ), NULL );
    SetThreadDpiAwarenessContext( old_context );
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
    DPI_AWARENESS_CONTEXT old_context = SetThreadDpiAwarenessContext( DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 );
    int x = GetSystemMetrics( SM_XVIRTUALSCREEN ), y = GetSystemMetrics( SM_YVIRTUALSCREEN );
    int width = GetSystemMetrics( SM_CXVIRTUALSCREEN ), height = GetSystemMetrics( SM_CYVIRTUALSCREEN );

    if (screen.hwnd)
    {
        SetForegroundWindow( screen.hwnd );
        SetThreadDpiAwarenessContext( old_context );
        return;
    }
    if (!registered)
    {
        WNDCLASSW class = { .lpfnWndProc = screen_proc, .hInstance = GetModuleHandleW( NULL ),
                            .lpszClassName = class_name };
        if (!RegisterClassW( &class ))
        {
            SetThreadDpiAwarenessContext( old_context );
            return;
        }
        registered = TRUE;
    }

    for (int i = 0; i < ITEM_COUNT; i++)
        LoadStringW( GetModuleHandleW( NULL ), item_text_ids[i], screen.text[i], ARRAY_SIZE(screen.text[i]) );
    screen.dpi = GetDpiForSystem();
    /* the options in Windows 11's Subtitle size, 20; the buttons in Body, 14 */
    screen.font = CreateFontW( -px( 20 ), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH, L"Segoe UI" );
    screen.small_font = CreateFontW( -px( 14 ), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
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
        SetThreadDpiAwarenessContext( old_context );
        return;
    }
    ShowWindow( screen.hwnd, SW_SHOW );
    SetForegroundWindow( screen.hwnd );
    SetThreadDpiAwarenessContext( old_context );
}
