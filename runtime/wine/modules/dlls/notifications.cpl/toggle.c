/*
 * Notifications applet: the toggle switch of Windows 10
 *
 * The switch of the Settings app (ToggleSwitch, light theme) as a Win32
 * control: a pill 44x20 with its knob, the accent colour when on, then
 * "Увімк." or "Вимк."; BM_GETCHECK / BM_SETCHECK and BN_CLICKED as a check
 * box has them. The shape is drawn with its edges smoothed (4x4 samples a
 * pixel) and blended over the dialog's background.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <math.h>
#include <stdlib.h>

#include "notifications.h"
#include "uxtheme.h"

struct toggle
{
    BOOL on, hot, pressed;
    HFONT font;
};

static int scale( HWND hwnd, int value )
{
    HDC hdc = GetDC( hwnd );
    int dpi = GetDeviceCaps( hdc, LOGPIXELSY );

    ReleaseDC( hwnd, hdc );
    return MulDiv( value, dpi, 96 );
}

/* how much of the pixel at (x, y) the pill and the knob cover */
static float pill_cover( float x, float y, float w, float h, float inset )
{
    float r = h / 2 - inset, cx = x < h / 2 ? h / 2 : x > w - h / 2 ? w - h / 2 : x, dx = x - cx, dy = y - h / 2;
    return sqrtf( dx * dx + dy * dy ) <= r ? 1.0f : 0.0f;
}

/* the accent colour of Windows (DWM's, as ABGR), its blue when none is chosen */
static COLORREF accent_color(void)
{
    DWORD abgr, size = sizeof(abgr);

    if (RegGetValueW( HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD,
                      NULL, &abgr, &size ))
        return RGB( 0, 120, 215 );
    return abgr & 0xffffff;
}

static void draw_switch( HDC hdc, int left, int top, int w, int h, const struct toggle *t, BOOL enabled )
{
    COLORREF accent = accent_color(), dark = t->hot ? RGB( 0, 0, 0 ) : RGB( 51, 51, 51 );
    COLORREF fill, ring, knob;
    float border = h / 10.0f, knob_r = h * 0.25f, knob_x;
    BITMAPINFO bi = { { sizeof(bi.bmiHeader), w, -h, 1, 32, BI_RGB } };
    UINT32 *bits;
    HBITMAP bitmap;
    HDC mem;

    if (!enabled) { fill = ring = knob = RGB( 204, 204, 204 ); if (t->on) knob = RGB( 255, 255, 255 ); }
    else if (t->on) { fill = ring = t->pressed ? RGB( 153, 153, 153 ) : accent; knob = RGB( 255, 255, 255 ); }
    else { fill = RGB( 255, 255, 255 ); ring = knob = t->pressed ? RGB( 153, 153, 153 ) : dark; }
    knob_x = t->on ? w - h / 2.0f : h / 2.0f;

    if (!(bitmap = CreateDIBSection( hdc, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0 ))) return;
    for (int y = 0; y < h; y++)
    {
        for (int x = 0; x < w; x++)
        {
            float outer = 0, inner = 0, dot = 0, r, g, b, a;

            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++)
                {
                    float px = x + (sx + 0.5f) / 4, py = y + (sy + 0.5f) / 4, dx = px - knob_x, dy = py - h / 2.0f;

                    outer += pill_cover( px, py, w, h, 0 );
                    inner += pill_cover( px, py, w, h, border );
                    dot += sqrtf( dx * dx + dy * dy ) <= knob_r;
                }
            outer /= 16; inner /= 16; dot /= 16;
            /* ring, then the inside, then the knob over them */
            r = GetRValue( ring ) * (outer - inner) + GetRValue( fill ) * inner;
            g = GetGValue( ring ) * (outer - inner) + GetGValue( fill ) * inner;
            b = GetBValue( ring ) * (outer - inner) + GetBValue( fill ) * inner;
            a = outer;
            r = r * (1 - dot) + GetRValue( knob ) * dot;
            g = g * (1 - dot) + GetGValue( knob ) * dot;
            b = b * (1 - dot) + GetBValue( knob ) * dot;
            if (dot > a) a = dot;
            bits[y * w + x] = (UINT32)(a * 255) << 24 | (UINT32)r << 16 | (UINT32)g << 8 | (UINT32)b;
        }
    }
    mem = CreateCompatibleDC( hdc );
    SelectObject( mem, bitmap );
    {
        BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        GdiAlphaBlend( hdc, left, top, w, h, mem, 0, 0, w, h, blend );
    }
    DeleteDC( mem );
    DeleteObject( bitmap );
}

static void paint( HWND hwnd, struct toggle *t )
{
    PAINTSTRUCT ps;
    RECT rc, text;
    HDC hdc = BeginPaint( hwnd, &ps );
    int w = scale( hwnd, 44 ), h = scale( hwnd, 20 );
    BOOL enabled = IsWindowEnabled( hwnd );
    HGDIOBJ old;

    GetClientRect( hwnd, &rc );
    if (FAILED( DrawThemeParentBackground( hwnd, hdc, &rc ) ))
        FillRect( hdc, &rc, (HBRUSH)SendMessageW( GetParent( hwnd ), WM_CTLCOLORSTATIC, (WPARAM)hdc, (LPARAM)hwnd ) );
    draw_switch( hdc, rc.left, (rc.top + rc.bottom - h) / 2, w, h, t, enabled );

    old = SelectObject( hdc, t->font ? t->font : GetStockObject( DEFAULT_GUI_FONT ) );
    SetBkMode( hdc, TRANSPARENT );
    SetTextColor( hdc, GetSysColor( enabled ? COLOR_WINDOWTEXT : COLOR_GRAYTEXT ) );
    text = rc;
    text.left += w + scale( hwnd, 12 );
    DrawTextW( hdc, load_string( t->on ? IDS_ON : IDS_OFF ), -1, &text, DT_SINGLELINE | DT_VCENTER | DT_LEFT );
    if (GetFocus() == hwnd && !(SendMessageW( hwnd, WM_QUERYUISTATE, 0, 0 ) & UISF_HIDEFOCUS))
    {
        RECT focus = text;
        DrawTextW( hdc, load_string( t->on ? IDS_ON : IDS_OFF ), -1, &focus, DT_SINGLELINE | DT_CALCRECT );
        OffsetRect( &focus, 0, (text.bottom - focus.bottom) / 2 );
        InflateRect( &focus, 2, 1 );
        DrawFocusRect( hdc, &focus );
    }
    SelectObject( hdc, old );
    EndPaint( hwnd, &ps );
}

static void flip( HWND hwnd, struct toggle *t )
{
    t->on = !t->on;
    InvalidateRect( hwnd, NULL, FALSE );
    SendMessageW( GetParent( hwnd ), WM_COMMAND, MAKEWPARAM( GetDlgCtrlID( hwnd ), BN_CLICKED ), (LPARAM)hwnd );
}

static LRESULT CALLBACK toggle_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    struct toggle *t = (struct toggle *)GetWindowLongPtrW( hwnd, 0 );

    switch (msg)
    {
    case WM_NCCREATE:
        if (!(t = calloc( 1, sizeof(*t) ))) return FALSE;
        SetWindowLongPtrW( hwnd, 0, (LONG_PTR)t );
        break;
    case WM_NCDESTROY:
        free( t );
        SetWindowLongPtrW( hwnd, 0, 0 );
        break;
    case WM_PAINT:
        paint( hwnd, t );
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SETFONT:
        t->font = (HFONT)wp;
        if (LOWORD( lp )) InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    case WM_GETFONT:
        return (LRESULT)t->font;
    case BM_GETCHECK:
        return t->on ? BST_CHECKED : BST_UNCHECKED;
    case BM_SETCHECK:
        t->on = wp == BST_CHECKED;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    case WM_GETDLGCODE:
        return DLGC_BUTTON;
    case WM_LBUTTONDOWN:
        SetFocus( hwnd );
        SetCapture( hwnd );
        t->pressed = TRUE;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == hwnd)
        {
            POINT pt = { (short)LOWORD( lp ), (short)HIWORD( lp ) };
            RECT rc;

            ReleaseCapture();
            t->pressed = FALSE;
            GetClientRect( hwnd, &rc );
            if (PtInRect( &rc, pt )) flip( hwnd, t );
            else InvalidateRect( hwnd, NULL, FALSE );
        }
        return 0;
    case WM_MOUSEMOVE:
        if (!t->hot)
        {
            TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, hwnd };
            t->hot = TRUE;
            TrackMouseEvent( &track );
            InvalidateRect( hwnd, NULL, FALSE );
        }
        return 0;
    case WM_MOUSELEAVE:
        t->hot = FALSE;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    case WM_KEYUP:
        if (wp == VK_SPACE) flip( hwnd, t );
        return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
    case WM_UPDATEUISTATE:
        InvalidateRect( hwnd, NULL, FALSE );
        break;
    }
    return DefWindowProcW( hwnd, msg, wp, lp );
}

void register_toggle(void)
{
    WNDCLASSW wc = { 0 };

    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = toggle_proc;
    wc.cbWndExtra = sizeof(void *);
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_ARROW );
    wc.lpszClassName = TOGGLE_CLASS;
    RegisterClassW( &wc );
}
