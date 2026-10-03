/*
 * Volume icon of the notification area: the overlay of the volume and media keys
 *
 * As Windows 11 does, the volume keys bring up a small overlay at the bottom
 * of the screen, over the taskbar and centred: the speaker and a bar of the
 * level in the accent colour, which the pointer can also drag. Its corners
 * are rounded less than Windows 11's (DWMWCP_ROUNDSMALL, 4 pixels). While a
 * program plays something (SystemMediaTransportControls), the overlay is
 * wider and carries the program's card above the level, the one of the
 * flyout (media.c): what plays, by whom, and previous / play-pause / next,
 * which the media keys press too. It never takes the focus, and goes two and
 * a half seconds after the last key unless the pointer is on it.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "sndvolsso.h"
#include "dwmapi.h"
#include "wine/arctic_dwm.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(sndvolsso);

#define COMPACT_WIDTH  220
#define ROW_HEIGHT     48
#define MARGIN         24      /* over the taskbar */
#define GLYPH          20
#define TIMER_HIDE     1
#define TIMER_POLL     2
#define SHOWN_MS       2500

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#define CORNER_ROUND_SMALL 3

BOOL WINAPI SetWindowCompositionAttribute( HWND hwnd, void *data );

static HWND osd;
static struct media_info media;
static float level;
static BOOL muted, have_device, over, dragging;
static int hot = -1;
static BOOL pressed;

static int osd_width(void)
{
    return media.present ? MEDIA_CARD_WIDTH : COMPACT_WIDTH;
}

static int osd_height(void)
{
    return (media.present ? MEDIA_CARD_HEIGHT + 1 : 0) + ROW_HEIGHT;
}

static int row_top(void)
{
    return media.present ? MEDIA_CARD_HEIGHT + 1 : 0;
}

static void bar_extent( int *left, int *right )
{
    *left = 16 + GLYPH + 16;
    *right = osd_width() - 20;
}

static void reload(void)
{
    struct endpoint def;

    have_device = audio_state( &def, &level, &muted );
    media_current( &media );
}

/* centred over the taskbar of the monitor the pointer is on */
static void place(void)
{
    MONITORINFO info = { sizeof(info) };
    POINT pt;
    int width = osd_width(), height = osd_height();

    GetCursorPos( &pt );
    GetMonitorInfoW( MonitorFromPoint( pt, MONITOR_DEFAULTTOPRIMARY ), &info );
    SetWindowPos( osd, HWND_TOPMOST, (info.rcWork.left + info.rcWork.right - width) / 2,
                  info.rcWork.bottom - MARGIN - height, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW );
}

static void paint( HDC hdc, const RECT *client )
{
    RECT card = { 0, 0, MEDIA_CARD_WIDTH, MEDIA_CARD_HEIGHT }, rect;
    int top = row_top(), left, right, fill;

    FillRect( hdc, client, GetStockObject( BLACK_BRUSH ) );
    SetBkMode( hdc, TRANSPARENT );
    if (media.present)
    {
        media_paint( hdc, &card, &media, hot, pressed, 255 );
        SetRect( &rect, 12, MEDIA_CARD_HEIGHT, client->right - 12, MEDIA_CARD_HEIGHT + 1 );
        fill_alpha( hdc, &rect, RGB( 255, 255, 255 ), 0x33 );
    }

    draw_speaker( hdc, 16, top + (ROW_HEIGHT - GLYPH) / 2, GLYPH, level, muted || !have_device, 0xff, 0 );
    bar_extent( &left, &right );
    SetRect( &rect, left, top + ROW_HEIGHT / 2 - 2, right, top + ROW_HEIGHT / 2 + 2 );
    fill_round_rect( hdc, &rect, 2.f, RGB( 255, 255, 255 ), 0x66 );
    fill = left + (int)((right - left) * (have_device && !muted ? level : 0) + 0.5f);
    if (fill > left)
    {
        rect.right = max( fill, left + 4 );
        fill_round_rect( hdc, &rect, 2.f, ACCENT_COLOR, 0xff );
    }
}

static void on_paint( HWND hwnd )
{
    PAINTSTRUCT ps;
    RECT client;
    HDC hdc = BeginPaint( hwnd, &ps ), mem;
    BITMAPINFO info = { { sizeof(info.bmiHeader), 0, 0, 1, 32, BI_RGB } };
    HBITMAP bitmap;
    void *bits;

    GetClientRect( hwnd, &client );
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
    EndPaint( hwnd, &ps );
}

static void set_level_at( int x )
{
    int left, right;
    float value;

    bar_extent( &left, &right );
    value = max( 0.f, min( 1.f, (float)(x - left) / (right - left) ) );
    value = (int)(value * 100 + 0.5f) / 100.f;
    if (value == level && !muted) return;
    level = value;
    audio_set_level( level );
    if (muted)
    {
        muted = FALSE;
        audio_set_mute( FALSE );
    }
    tray_update();
    InvalidateRect( osd, NULL, FALSE );
}

static BOOL on_bar( POINT pt )
{
    int left, right, top = row_top();

    bar_extent( &left, &right );
    return pt.y >= top && pt.y < top + ROW_HEIGHT && pt.x >= left - 8 && pt.x < right + 8;
}

static LRESULT CALLBACK osd_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    switch (msg)
    {
    case WM_TIMER:
        if (wparam == TIMER_HIDE)
        {
            if (!over && !dragging) osd_hide();
        }
        else if (wparam == TIMER_POLL && !dragging)
        {
            BOOL had_media = media.present;
            reload();
            if (had_media != media.present) place();
            InvalidateRect( hwnd, NULL, FALSE );
        }
        return 0;

    case WM_MOUSEMOVE:
    {
        POINT pt = { (short)LOWORD( lparam ), (short)HIWORD( lparam ) };
        RECT card = { 0, 0, MEDIA_CARD_WIDTH, MEDIA_CARD_HEIGHT };
        TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, hwnd };
        int hit;

        if (dragging)
        {
            set_level_at( pt.x );
            return 0;
        }
        over = TRUE;
        TrackMouseEvent( &track );
        hit = media_hit( &card, pt, &media );
        if (hit != hot)
        {
            hot = hit;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        over = FALSE;
        hot = -1;
        pressed = FALSE;
        InvalidateRect( hwnd, NULL, FALSE );
        SetTimer( hwnd, TIMER_HIDE, SHOWN_MS, NULL );
        return 0;

    case WM_LBUTTONDOWN:
    {
        POINT pt = { (short)LOWORD( lparam ), (short)HIWORD( lparam ) };

        if (have_device && on_bar( pt ))
        {
            dragging = TRUE;
            SetCapture( hwnd );
            set_level_at( pt.x );
            return 0;
        }
        pressed = hot >= 0;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    }

    case WM_LBUTTONUP:
    {
        POINT pt = { (short)LOWORD( lparam ), (short)HIWORD( lparam ) };
        RECT card = { 0, 0, MEDIA_CARD_WIDTH, MEDIA_CARD_HEIGHT };

        if (dragging)
        {
            dragging = FALSE;
            ReleaseCapture();
            return 0;
        }
        if (pressed && media_hit( &card, pt, &media ) == hot) media_press( &media, hot );
        pressed = FALSE;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    }

    case WM_CAPTURECHANGED:
        dragging = FALSE;
        return 0;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        on_paint( hwnd );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

static BOOL create_osd(void)
{
    struct arctic_accent_policy policy = { ARCTIC_ACCENT_ENABLE_ACRYLICBLURBEHIND, 0, ACRYLIC_TINT, 0 };
    struct { DWORD attrib; void *data; SIZE_T size; } attr = { 19 /* WCA_ACCENT_POLICY */, &policy, sizeof(policy) };
    DWORD transition = ARCTIC_TRANSITION_FADE, corner = CORNER_ROUND_SMALL;
    WNDCLASSW cls = { 0 };

    if (osd) return TRUE;
    cls.lpfnWndProc = osd_proc;
    cls.hInstance = sndvolsso_instance;
    cls.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_ARROW );
    cls.lpszClassName = L"ArcticVolumeOverlay";
    RegisterClassW( &cls );
    osd = CreateWindowExW( WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, cls.lpszClassName, L"", WS_POPUP,
                           0, 0, COMPACT_WIDTH, ROW_HEIGHT, NULL, NULL, sndvolsso_instance, NULL );
    if (!osd) return FALSE;
    SetWindowCompositionAttribute( osd, &attr );
    DwmSetWindowAttribute( osd, DWMWA_ARCTIC_TRANSITION, &transition, sizeof(transition) );
    DwmSetWindowAttribute( osd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner) );
    return TRUE;
}

/* a key went down: up it comes, or stays a while longer */
void osd_show(void)
{
    if (!create_osd()) return;
    reload();
    place();
    InvalidateRect( osd, NULL, FALSE );
    SetTimer( osd, TIMER_HIDE, SHOWN_MS, NULL );
    SetTimer( osd, TIMER_POLL, 150, NULL );
}

void osd_hide(void)
{
    if (!osd || !IsWindowVisible( osd )) return;
    KillTimer( osd, TIMER_HIDE );
    KillTimer( osd, TIMER_POLL );
    if (dragging) ReleaseCapture();
    dragging = over = pressed = FALSE;
    hot = -1;
    ShowWindow( osd, SW_HIDE );
}
