/*
 * The search box and the Task View button of the taskbar
 *
 * As in Windows 11, they stand right after Start: explorer places them
 * (traywnd.cpp, ArcticPlaceGroup) and indents the task buttons after them.
 * The search shows as Windows 11 lets it: hidden, an icon, an icon with its
 * label, or a box (HKCU\...\CurrentVersion\Search, SearchboxTaskbarMode, the
 * box until the user says otherwise); in the buttons of Windows 10 it is
 * Windows 10's box or icon. A click, or Win+S, opens the search (search11.c).
 * The Task View button (HKCU\...\Explorer\Advanced, ShowTaskViewButton, on
 * until the user says otherwise) opens Task View (twinui.dll). A right click
 * on either is a right click on the taskbar, whose menu chooses them
 * (taskbtn.c).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <math.h>
#include <stdlib.h>

#include "startui.h"
#include "winreg.h"

#define SEARCH_KEY      L"Software\\Microsoft\\Windows\\CurrentVersion\\Search"
#define ADVANCED_KEY    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced"
#define HOTKEY_SEARCH   1

/* as explorer passes them to ArcticTaskbarPaintButton */
#define BUTTON_HOT      1
#define BUTTON_PRESSED  2
#define BUTTON_START    16

BOOL WINAPI ArcticTaskbarIsLight(void);
void WINAPI ArcticTaskbarPaintButton( HDC hdc, const RECT *button, DWORD flags );

static HWND tray, search_button, taskview_button;
static BOOL search_hot, search_down, taskview_hot, taskview_down, taskview_open;
static HFONT font_box;

static DWORD get_dword( const WCHAR *key, const WCHAR *name, DWORD value )
{
    DWORD size = sizeof(value);

    RegGetValueW( HKEY_CURRENT_USER, key, name, RRF_RT_REG_DWORD, NULL, &value, &size );
    return value;
}

static void set_dword( const WCHAR *key, const WCHAR *name, DWORD value )
{
    RegSetKeyValueW( HKEY_CURRENT_USER, key, name, REG_DWORD, &value, sizeof(value) );
}

/* explorer places the buttons anew: the widths changed */
static void changed(void)
{
    if (search_button) InvalidateRect( search_button, NULL, FALSE );
    if (taskview_button) InvalidateRect( taskview_button, NULL, FALSE );
    if (tray) PostMessageW( tray, RegisterWindowMessageW( L"ArcticWidgetChanged" ), 0, 0 );
}

DWORD search_mode(void)
{
    DWORD mode = get_dword( SEARCH_KEY, L"SearchboxTaskbarMode", SEARCH_BOX );

    if (mode > SEARCH_ICON_LABEL) mode = SEARCH_BOX;
    /* Windows 10's buttons have no label beside the icon */
    if (!taskbar_style11() && mode == SEARCH_ICON_LABEL) mode = SEARCH_ICON;
    return mode;
}

void search_set_mode( DWORD mode )
{
    set_dword( SEARCH_KEY, L"SearchboxTaskbarMode", mode );
    if (mode == SEARCH_HIDDEN) search11_hide();
    changed();
}

BOOL taskview_button_shown(void)
{
    return get_dword( ADVANCED_KEY, L"ShowTaskViewButton", 1 ) != 0;
}

void taskview_button_show( BOOL show )
{
    set_dword( ADVANCED_KEY, L"ShowTaskViewButton", show != 0 );
    changed();
}

static HFONT box_font(void)
{
    if (!font_box) font_box = shell_font( px( 14 ), FW_NORMAL );
    return font_box;
}

static int text_width( HWND hwnd, const WCHAR *text )
{
    HDC hdc = GetDC( hwnd );
    HGDIOBJ old = SelectObject( hdc, box_font() );
    SIZE size = { 0 };

    GetTextExtentPoint32W( hdc, text, lstrlenW( text ), &size );
    SelectObject( hdc, old );
    ReleaseDC( hwnd, hdc );
    return size.cx;
}

/**********************************************************************
 *          Widths, for explorer
 */

int WINAPI ArcticSearchWidth(void)
{
    BOOL win11 = taskbar_style11();

    if (!search_button) return 0;
    switch (search_mode())
    {
    case SEARCH_ICON:
        return px( win11 ? 44 : 48 );
    case SEARCH_ICON_LABEL:
        return px( 12 + 20 + 8 + 14 ) + text_width( search_button, load_string( IDS_SEARCH_BOX ) );
    case SEARCH_BOX:
        return px( win11 ? 216 : 344 );
    default:
        return 0;
    }
}

int WINAPI ArcticTaskViewWidth(void)
{
    if (!taskview_button || !taskview_button_shown()) return 0;
    return px( taskbar_style11() ? 44 : 48 );
}

/**********************************************************************
 *          Painting
 */


/* the signed distance of a point from a rounded rectangle: below 0 inside */
static float round_distance( float x, float y, const float *rect, float radius )
{
    float cx = (rect[0] + rect[2]) / 2, cy = (rect[1] + rect[3]) / 2;
    float qx = fabsf( x - cx ) - ((rect[2] - rect[0]) / 2 - radius);
    float qy = fabsf( y - cy ) - ((rect[3] - rect[1]) / 2 - radius);
    float ox = max( qx, 0.0f ), oy = max( qy, 0.0f );

    return sqrtf( ox * ox + oy * oy ) + min( max( qx, qy ), 0.0f ) - radius;
}

/* Windows 11's Task View, as its glyph is: a dark window in front, darkening
 * from its top left corner, a white one behind it with a soft shadow, and
 * where they overlap a grey that darkens downwards, as through tinted glass.
 * Drawn in 24 units, every pixel sampled 4x4 (2026-11-18, from the user's
 * picture of it). */
static void draw_taskview_glyph( HDC hdc, const RECT *button, BOOL light, BOOL win11 )
{
    static const float front[4] = { 2.8f, 7.8f, 16.3f, 21.3f }, back[4] = { 7.1f, 2.6f, 20.6f, 16.1f };
    static const float radius = 1.4f;
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    BITMAPINFO info = { { sizeof(info.bmiHeader), 0, 0, 1, 32, BI_RGB } };
    int size = px( win11 ? 24 : 20 ), x0 = (button->left + button->right - size) / 2;
    int y0 = (button->top + button->bottom - size) / 2;
    float u = 24.0f / size;
    HBITMAP bitmap;
    HDC mem;
    DWORD *bits;

    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    mem = CreateCompatibleDC( hdc );
    if (!(bitmap = CreateDIBSection( hdc, &info, DIB_RGB_COLORS, (void **)&bits, NULL, 0 )))
    {
        DeleteDC( mem );
        return;
    }
    for (int y = 0; y < size; y++)
    {
        for (int x = 0; x < size; x++)
        {
            float a = 0, c = 0;

            for (int sy = 0; sy < 4; sy++)
            {
                for (int sx = 0; sx < 4; sx++)
                {
                    float gx = (x + (sx + 0.5f) / 4) * u, gy = (y + (sy + 0.5f) / 4) * u;
                    float d_front = round_distance( gx, gy, front, radius );
                    float d_back = round_distance( gx, gy, back, radius );

                    if (d_front <= 0 && d_back <= 0)
                    {
                        /* the overlap: grey, darker downwards */
                        float t = (gy - front[1]) / (back[3] - front[1]);
                        c += 0x93 + (0x44 - 0x93) * t;
                        a += 1;
                    }
                    else if (d_front <= 0)
                    {
                        float t = ((gx - front[0]) + (gy - front[1])) / ((front[2] - front[0]) + (front[3] - front[1]));
                        float ink = 0x2e + (0x05 - 0x2e) * t;

                        /* over a dark taskbar its edge shows faintly */
                        if (!light && d_front > -0.6f * u) ink = max( ink, 0x58 );
                        c += ink;
                        a += 1;
                    }
                    else if (d_back <= 0)
                    {
                        float t = ((gx - back[0]) + (gy - back[1])) / ((back[2] - back[0]) + (back[3] - back[1]));
                        c += 0xff + (0xec - 0xff) * t;
                        a += 1;
                    }
                    else
                    {
                        /* the shadow of the white one, down and to the right */
                        float shifted[4] = { back[0] + 0.3f, back[1] + 0.5f, back[2] + 0.3f, back[3] + 0.5f };
                        float d = round_distance( gx, gy, shifted, radius );

                        if (d < 1.0f) a += 0.28f * (1 - max( d, 0.0f ));
                    }
                }
            }
            a /= 16;
            c /= 16;  /* premultiplied already: the shadow adds alpha, no colour */
            bits[y * size + x] = ((DWORD)lroundf( a * 255 ) << 24) | ((DWORD)lroundf( c ) << 16) |
                                 ((DWORD)lroundf( c ) << 8) | (DWORD)lroundf( c );
        }
    }
    GdiFlush();
    {
        HGDIOBJ old = SelectObject( mem, bitmap );
        GdiAlphaBlend( hdc, x0, y0, size, size, mem, 0, 0, size, size, blend );
        SelectObject( mem, old );
    }
    DeleteObject( bitmap );
    DeleteDC( mem );
}

static void paint_taskview(void)
{
    struct palette p;
    PAINTSTRUCT ps;
    HBITMAP bitmap;
    HGDIOBJ old;
    RECT client;
    BOOL win11 = taskbar_style11();
    DWORD flags = BUTTON_START;
    HDC mem;

    palette_init( &p, ArcticTaskbarIsLight() );
    if (!(mem = paint_begin( taskview_button, &ps, &p, &bitmap, &old ))) return;
    GetClientRect( taskview_button, &client );
    if (taskview_hot || taskview_open) flags |= BUTTON_HOT;
    if (taskview_down) flags |= BUTTON_PRESSED;
    if (flags != BUTTON_START)
    {
        if (win11) ArcticTaskbarPaintButton( mem, &client, flags );
        else fill_alpha( mem, &client, p.ink, taskview_down ? 0x14 : 0x20 );
    }
    draw_taskview_glyph( mem, &client, p.light, win11 );
    paint_end( taskview_button, &ps, mem, bitmap, old );
}

static void paint_search(void)
{
    struct palette p;
    PAINTSTRUCT ps;
    HBITMAP bitmap;
    HGDIOBJ old;
    RECT client, r, text;
    BOOL win11 = taskbar_style11(), open = search11_visible();
    DWORD mode = search_mode();
    HDC mem;

    palette_init( &p, ArcticTaskbarIsLight() );
    if (!(mem = paint_begin( search_button, &ps, &p, &bitmap, &old ))) return;
    GetClientRect( search_button, &client );

    if (mode == SEARCH_BOX && win11)
    {
        /* a pill, lit under the pointer, round at its ends */
        int height = min( (int)client.bottom - px( 12 ), px( 36 ) );
        float radius = height / 2.0f;
        RECT inner;

        SetRect( &r, px( 4 ), (client.bottom - height) / 2, client.right - px( 4 ), (client.bottom + height) / 2 );
        inner = r;
        InflateRect( &inner, -1, -1 );
        if (p.light)
        {
            fill_round_rect( mem, &r, radius, RGB( 0xd9, 0xd9, 0xd9 ), 255 );
            fill_round_rect( mem, &inner, radius - 1, search_hot || open ? RGB( 0xff, 0xff, 0xff ) : RGB( 0xf9, 0xf9, 0xf9 ), 255 );
        }
        else
        {
            fill_round_rect( mem, &r, radius, p.ink, 0x1e );
            fill_round_rect( mem, &inner, radius - 1, RGB( 0x2c, 0x2c, 0x2c ), search_hot || open ? 0xf0 : 0xd8 );
        }
        draw_glyph( mem, GLYPH_SEARCH, r.left + px( 14 ), (r.top + r.bottom - px( 16 )) / 2, px( 16 ), p.ink, 255 );
        SetRect( &text, r.left + px( 14 + 16 + 10 ), r.top, r.right - px( 14 ), r.bottom );
        paint_text( mem, load_string( IDS_SEARCH_BOX ), &text, box_font(), p.dim, DT_LEFT );
        if (p.light)
        {
            InflateRect( &inner, -(int)radius / 2, -1 );
            make_opaque( mem, &inner );
        }
    }
    else if (mode == SEARCH_BOX)
    {
        /* Windows 10's: the whole height, square, light over a light taskbar */
        SetRect( &r, 0, 0, client.right, client.bottom );
        if (p.light) fill_alpha( mem, &r, search_hot || open ? RGB( 0xff, 0xff, 0xff ) : RGB( 0xf3, 0xf3, 0xf3 ), 255 );
        else fill_alpha( mem, &r, search_hot || open ? RGB( 0x4c, 0x4c, 0x4c ) : RGB( 0x3a, 0x3a, 0x3a ), 255 );
        draw_glyph( mem, GLYPH_SEARCH, px( 12 ), (client.bottom - px( 16 )) / 2, px( 16 ), p.ink, 255 );
        SetRect( &text, px( 12 + 16 + 12 ), 0, client.right - px( 8 ), client.bottom );
        paint_text( mem, load_string( IDS_SEARCH_WIN10 ), &text, box_font(), p.dim, DT_LEFT );
        make_opaque( mem, &r );
    }
    else
    {
        DWORD flags = BUTTON_START;

        if (search_hot || open) flags |= BUTTON_HOT;
        if (search_down) flags |= BUTTON_PRESSED;
        if (flags != BUTTON_START)
        {
            if (win11) ArcticTaskbarPaintButton( mem, &client, flags );
            else fill_alpha( mem, &client, p.ink, search_down ? 0x14 : 0x20 );
        }
        if (mode == SEARCH_ICON_LABEL)
        {
            draw_glyph( mem, GLYPH_SEARCH, px( 12 ), (client.bottom - px( 20 )) / 2, px( 20 ), p.ink, 255 );
            SetRect( &text, px( 12 + 20 + 8 ), 0, client.right - px( 4 ), client.bottom );
            paint_text( mem, load_string( IDS_SEARCH_BOX ), &text, box_font(), p.text, DT_LEFT );
        }
        else
        {
            int size = px( win11 ? 20 : 16 );
            draw_glyph( mem, GLYPH_SEARCH, (client.right - size) / 2, (client.bottom - size) / 2, size, p.ink, 255 );
        }
    }
    paint_end( search_button, &ps, mem, bitmap, old );
}

/**********************************************************************
 *          The windows
 */

static void open_search(void)
{
    RECT rect;

    GetWindowRect( search_button && IsWindowVisible( search_button ) ? search_button : tray, &rect );
    search11_toggle( tray, &rect );
    InvalidateRect( search_button, NULL, FALSE );
}

static void toggle_taskview(void)
{
    HMODULE twinui = GetModuleHandleW( L"twinui.dll" );
    BOOL (WINAPI *toggle)(void);

    if (!twinui) twinui = LoadLibraryW( L"twinui.dll" );
    if (twinui && (toggle = (void *)GetProcAddress( twinui, "ArcticTaskViewToggle" ))) toggle();
}

/* what a right click on the taskbar shows, where it was clicked */
static void taskbar_menu( HWND hwnd, LPARAM lparam )
{
    POINT pt = { (short)LOWORD( lparam ), (short)HIWORD( lparam ) };

    ClientToScreen( hwnd, &pt );
    PostMessageW( tray, WM_CONTEXTMENU, (WPARAM)tray, MAKELPARAM( pt.x, pt.y ) );
}

static LRESULT CALLBACK button_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    BOOL is_search = hwnd == search_button;
    BOOL *hot = is_search ? &search_hot : &taskview_hot, *down = is_search ? &search_down : &taskview_down;

    switch (msg)
    {
    case WM_MOUSEMOVE:
        if (!*hot)
        {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };

            TrackMouseEvent( &tme );
            *hot = TRUE;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        return 0;

    case WM_MOUSELEAVE:
        *hot = *down = FALSE;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;

    case WM_LBUTTONDOWN:
        *down = TRUE;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;

    case WM_LBUTTONUP:
        if (!*down) return 0;
        *down = FALSE;
        InvalidateRect( hwnd, NULL, FALSE );
        if (is_search) open_search();
        else toggle_taskview();
        return 0;

    case WM_RBUTTONUP:
        taskbar_menu( hwnd, lparam );
        return 0;

    case WM_HOTKEY:
        if (wparam == HOTKEY_SEARCH) open_search();
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        if (is_search) paint_search();
        else paint_taskview();
        return 0;
    }
    if (msg == RegisterWindowMessageW( L"ArcticTaskViewChanged" ))
    {
        taskview_open = wparam != 0;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    }
    if (msg == RegisterWindowMessageW( L"ArcticShellLookChanged" ))
    {
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

static HWND create_button( HWND tray_window, const WCHAR *name )
{
    static BOOL registered[2];
    BOOL is_search = !wcscmp( name, L"ArcticSearchButton" );
    WNDCLASSW cls = { 0 };

    tray = tray_window;
    if (!registered[is_search])
    {
        cls.lpfnWndProc = button_proc;
        cls.hInstance = startui_instance;
        cls.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_ARROW );
        cls.lpszClassName = name;
        RegisterClassW( &cls );
        registered[is_search] = TRUE;
    }
    return CreateWindowExW( 0, name, load_string( is_search ? IDS_SEARCH_BOX : IDS_TASKVIEW ),
                            WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, tray, NULL, startui_instance, NULL );
}

/* the search opened or closed: the box shows it */
void search_button_refresh(void)
{
    if (search_button) InvalidateRect( search_button, NULL, FALSE );
}

/* for explorer: the buttons in the taskbar, which it places */
HWND WINAPI ArcticSearchCreate( HWND tray_window )
{
    if (search_button) return search_button;
    if ((search_button = create_button( tray_window, L"ArcticSearchButton" )))
    {
        /* Win+S, as in Windows */
        if (!RegisterHotKey( search_button, HOTKEY_SEARCH, MOD_WIN | 0x4000 /* MOD_NOREPEAT */, 'S' ))
            RegisterHotKey( search_button, HOTKEY_SEARCH, MOD_WIN, 'S' );
    }
    return search_button;
}

HWND WINAPI ArcticTaskViewCreate( HWND tray_window )
{
    if (!taskview_button) taskview_button = create_button( tray_window, L"ArcticTaskViewButton" );
    return taskview_button;
}

/* the search, from elsewhere (Win+S in explorer, the Start button's menu) */
void WINAPI ArcticSearchToggle(void)
{
    if (tray) open_search();
}
