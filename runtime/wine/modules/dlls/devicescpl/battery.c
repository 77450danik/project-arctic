/*
 * Devices and Printers: the batteries of a device as rings
 *
 * As a phone shows earbuds: a ring for the left one, one for the right one,
 * one for their case, each with its level in it and its name under it; a
 * headset that tells only its own level gets one ring. The arc is the
 * accent's colour, red at a fifth and less, green while charging. The page's
 * cards have them in a control of their own (ArcticBatteryRings); the
 * network flyout of the taskbar draws them through ArcticDrawBatteryRings.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "cpanel.h"
#include "bthsdpdef.h"
#include "bluetoothapis.h"
#include "wine/arctic_shell.h"
#include "wine/arctic_fluent.h"
#include "devicescpl.h"

struct ring
{
    BYTE level;
    BOOL charging;
    UINT caption;     /* a string, 0: none */
};

static UINT battery_ring_list( const struct bt_battery *battery, struct ring rings[3] )
{
    UINT count = 0;

    if (battery->left != BT_BATTERY_UNKNOWN || battery->right != BT_BATTERY_UNKNOWN)
    {
        if (battery->left != BT_BATTERY_UNKNOWN)
            rings[count++] = (struct ring){ battery->left, !!(battery->charging & 2), IDS_BATTERY_LEFT };
        if (battery->right != BT_BATTERY_UNKNOWN)
            rings[count++] = (struct ring){ battery->right, !!(battery->charging & 4), IDS_BATTERY_RIGHT };
        if (battery->case_level != BT_BATTERY_UNKNOWN)
            rings[count++] = (struct ring){ battery->case_level, !!(battery->charging & 8), IDS_BATTERY_CASE };
    }
    else if (battery->level != BT_BATTERY_UNKNOWN)
        rings[count++] = (struct ring){ battery->level, !!(battery->charging & 1), 0 };
    return count;
}

static float scale(void)
{
    return GetDpiForSystem() / 96.0f;
}

static int item_width( BOOL compact )
{
    return (int)((compact ? 36 : 46) * scale() + 0.5f);
}

int battery_rings_width( const struct bt_battery *battery )
{
    struct ring rings[3];
    UINT count = battery_ring_list( battery, rings );

    return count ? count * item_width( FALSE ) : 0;
}

void battery_rings_text( const struct bt_battery *battery, WCHAR *text, UINT size )
{
    swprintf( text, size, L"%u,%u,%u,%u,%u", battery->level, battery->left, battery->right, battery->case_level,
              battery->charging );
}

static void parse_rings_text( const WCHAR *text, struct bt_battery *battery )
{
    unsigned int v[5] = { BT_BATTERY_UNKNOWN, BT_BATTERY_UNKNOWN, BT_BATTERY_UNKNOWN, BT_BATTERY_UNKNOWN, 0 };

    swscanf( text, L"%u,%u,%u,%u,%u", &v[0], &v[1], &v[2], &v[3], &v[4] );
    battery->level = v[0];
    battery->left = v[1];
    battery->right = v[2];
    battery->case_level = v[3];
    battery->charging = v[4];
}

void draw_battery_rings( HDC hdc, const struct bt_battery *battery, const RECT *rect, BOOL dark, BOOL compact,
                         HFONT number, HFONT caption )
{
    struct ring rings[3];
    UINT count = battery_ring_list( battery, rings );
    float unit = scale(), diameter = (compact ? 26 : 32) * unit, stroke = (compact ? 2.5f : 3) * unit;
    int width = item_width( compact ), caption_height = 0, total, top, left;
    COLORREF text = dark ? RGB( 0xff, 0xff, 0xff ) : RGB( 0x1b, 0x1b, 0x1b );
    COLORREF gray = dark ? RGB( 0xa0, 0xa0, 0xa0 ) : RGB( 0x6d, 0x6d, 0x6d );
    HFONT old;
    TEXTMETRICW tm;

    if (!count) return;
    old = SelectObject( hdc, caption );
    GetTextMetricsW( hdc, &tm );
    if (rings[0].caption) caption_height = tm.tmHeight + (int)(2 * unit);
    total = (int)diameter + caption_height;
    top = rect->top + (rect->bottom - rect->top - total) / 2;
    left = rect->right - count * width;   /* against the right edge */
    SetBkMode( hdc, TRANSPARENT );

    for (UINT i = 0; i < count; i++)
    {
        float cx = left + i * width + width / 2.0f, cy = top + diameter / 2;
        COLORREF arc = rings[i].charging ? (dark ? RGB( 0x6c, 0xcb, 0x5f ) : RGB( 0x0f, 0x7b, 0x0f )) :
                       rings[i].level <= 20 ? (dark ? RGB( 0xff, 0x99, 0xa4 ) : RGB( 0xc4, 0x2b, 0x1c )) :
                       FLUENT_ACCENT( !dark );
        WCHAR digits[8];
        RECT r;

        fluent_ring( hdc, cx, cy, diameter / 2 - stroke / 2, stroke, rings[i].level / 100.0f,
                     dark ? RGB( 0xff, 0xff, 0xff ) : RGB( 0, 0, 0 ), dark ? 0x30 : 0x22, arc, 0xff );
        swprintf( digits, ARRAY_SIZE(digits), L"%u", rings[i].level );
        SelectObject( hdc, number );
        SetTextColor( hdc, text );
        SetRect( &r, (int)(cx - diameter / 2), (int)(cy - diameter / 2), (int)(cx + diameter / 2), (int)(cy + diameter / 2) );
        DrawTextW( hdc, digits, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX );
        if (rings[i].caption)
        {
            SelectObject( hdc, caption );
            SetTextColor( hdc, gray );
            SetRect( &r, left + i * width, top + (int)diameter + (int)(2 * unit), left + (i + 1) * width, rect->bottom );
            DrawTextW( hdc, load_string( rings[i].caption ), -1, &r, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX );
        }
    }
    SelectObject( hdc, old );
}

/* the fonts of the rings, from the page's body font */
static void ring_fonts( HFONT base, BOOL compact, HFONT *number, HFONT *caption )
{
    LOGFONTW lf;
    float unit = scale();

    if (!base || !GetObjectW( base, sizeof(lf), &lf )) GetObjectW( GetStockObject( DEFAULT_GUI_FONT ), sizeof(lf), &lf );
    lf.lfHeight = -(int)((compact ? 10 : 11) * unit + 0.5f);
    lf.lfWeight = FW_SEMIBOLD;
    lf.lfQuality = CLEARTYPE_QUALITY;
    *number = CreateFontIndirectW( &lf );
    lf.lfWeight = FW_NORMAL;
    *caption = CreateFontIndirectW( &lf );
}

/* the taskbar's network flyout (pnidui.dll): levels as the text has them */
void WINAPI ArcticDrawBatteryRings( HDC hdc, const BYTE levels[5], const RECT *rect, BOOL dark, HFONT base )
{
    struct bt_battery battery = { levels[0], levels[1], levels[2], levels[3], levels[4] };
    HFONT number, caption;

    ring_fonts( base, TRUE, &number, &caption );
    draw_battery_rings( hdc, &battery, rect, dark, TRUE, number, caption );
    DeleteObject( number );
    DeleteObject( caption );
}

int WINAPI ArcticBatteryRingsWidth( const BYTE levels[5] )
{
    struct bt_battery battery = { levels[0], levels[1], levels[2], levels[3], levels[4] };
    struct ring rings[3];

    return battery_ring_list( &battery, rings ) * item_width( TRUE );
}

static LRESULT CALLBACK rings_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    switch (msg)
    {
    case WM_SETFONT:
        SetPropW( hwnd, L"ArcticFont", (HANDLE)wparam );
        return 0;
    case WM_SETTEXT:
    {
        LRESULT ret = DefWindowProcW( hwnd, msg, wparam, lparam );
        InvalidateRect( hwnd, NULL, TRUE );
        return ret;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint( hwnd, &ps ), mem;
        WCHAR text[64];
        struct bt_battery battery;
        HFONT number, caption;
        HBITMAP bitmap;
        RECT rect;
        BOOL dark = cp_dark();

        GetClientRect( hwnd, &rect );
        GetWindowTextW( hwnd, text, ARRAY_SIZE(text) );
        parse_rings_text( text, &battery );
        /* drawn off screen, over the card's colour: the rings blend their edges */
        mem = CreateCompatibleDC( hdc );
        bitmap = CreateCompatibleBitmap( hdc, rect.right, rect.bottom );
        SelectObject( mem, bitmap );
        FillRect( mem, &rect, view_brush( COLOR_CARD ) );
        ring_fonts( GetPropW( hwnd, L"ArcticFont" ), FALSE, &number, &caption );
        draw_battery_rings( mem, &battery, &rect, dark, FALSE, number, caption );
        BitBlt( hdc, 0, 0, rect.right, rect.bottom, mem, 0, 0, SRCCOPY );
        DeleteObject( number );
        DeleteObject( caption );
        DeleteDC( mem );
        DeleteObject( bitmap );
        EndPaint( hwnd, &ps );
        return 0;
    }
    case WM_DESTROY:
        RemovePropW( hwnd, L"ArcticFont" );
        break;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

void battery_rings_register( void )
{
    WNDCLASSW cls = { 0 };

    cls.lpfnWndProc = rings_proc;
    cls.hInstance = cp_instance;
    cls.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_ARROW );
    cls.lpszClassName = BATTERY_RINGS_CLASS;
    RegisterClassW( &cls );
}
