/*
 * Devices and Printers: "Надсилання файлів Bluetooth" (fsquirt.exe)
 *
 * Send to → Bluetooth device, or the page's "Надіслати файли": the paired
 * device to send to, then each file over OBEX Object Push (winebth.sys),
 * with how far it is, until the device has taken them all; the device's
 * owner says yes on it.
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

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "winreg.h"
#include "winioctl.h"
#include "commdlg.h"
#include "dwmapi.h"
#include "shellapi.h"
#include "bthsdpdef.h"
#include "bluetoothapis.h"
#include "bthdef.h"
#include "bthledef.h"
#include "bthioctl.h"
#include "wine/winebth.h"
#include "wine/arctic_shell.h"
#include "wine/arctic_fluent.h"

#include "cpanel.h"
#include "devicescpl.h"

#define WM_SEND_DONE      (WM_APP + 1)   /* wparam: the first error, 0 */
#define WM_SEND_FILE      (WM_APP + 2)   /* wparam: the file being sent */
#define TIMER_PROGRESS    1

#define WIDTH             px(460)
#define HEIGHT            px(460)
#define PAD               px(24)
#define FOOTER            px(80)
#define ROW_HEIGHT        px(56)
#define BUTTON_HEIGHT     px(32)

enum page { PAGE_DEVICES, PAGE_SENDING, PAGE_DONE, PAGE_FAILED, PAGE_NONE };
enum hit { HIT_NONE, HIT_DEVICE, HIT_PRIMARY, HIT_CANCEL };

struct sender
{
    HWND hwnd;
    enum page page;
    WCHAR **files;
    UINT file_count, current;
    struct bt_device devices[32];
    UINT device_count;
    BLUETOOTH_DEVICE_INFO target;
    ULONGLONG sent, total;
    enum hit hot, pressed;
    int hot_device, pressed_device;
    DWORD error;
    BOOL done;
    HFONT title, body, bold;
};

static void colors( COLORREF *page, COLORREF *card, COLORREF *card_hot, COLORREF *text, COLORREF *gray, COLORREF *footer )
{
    BOOL dark = cp_dark();

    *page = dark ? RGB( 0x20, 0x20, 0x20 ) : RGB( 0xf3, 0xf3, 0xf3 );
    *card = dark ? RGB( 0x2b, 0x2b, 0x2b ) : RGB( 0xfb, 0xfb, 0xfb );
    *card_hot = dark ? RGB( 0x32, 0x32, 0x32 ) : RGB( 0xf5, 0xf5, 0xf5 );
    *text = dark ? RGB( 0xff, 0xff, 0xff ) : RGB( 0x1b, 0x1b, 0x1b );
    *gray = dark ? RGB( 0xc5, 0xc5, 0xc5 ) : RGB( 0x5d, 0x5d, 0x5d );
    *footer = dark ? RGB( 0x1b, 0x1b, 0x1b ) : RGB( 0xe9, 0xe9, 0xe9 );
}

static HFONT make_font( int height, int weight )
{
    NONCLIENTMETRICSW metrics = { sizeof(metrics) };

    SystemParametersInfoW( SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0 );
    metrics.lfMessageFont.lfHeight = -height;
    metrics.lfMessageFont.lfWeight = weight;
    metrics.lfMessageFont.lfQuality = CLEARTYPE_QUALITY;
    return CreateFontIndirectW( &metrics.lfMessageFont );
}

static void draw_text( HDC hdc, const WCHAR *text, RECT *rect, HFONT font, COLORREF color, UINT flags )
{
    HGDIOBJ old = SelectObject( hdc, font );

    SetTextColor( hdc, color );
    DrawTextW( hdc, text, -1, rect, flags | DT_NOPREFIX );
    SelectObject( hdc, old );
}

static void footer_buttons( struct sender *s, RECT *primary, RECT *cancel )
{
    RECT client;
    int width = px( 120 ), y;

    GetClientRect( s->hwnd, &client );
    y = client.bottom - FOOTER + (FOOTER - BUTTON_HEIGHT) / 2;
    SetRect( primary, client.right - PAD - width, y, client.right - PAD, y + BUTTON_HEIGHT );
    *cancel = *primary;
    if (s->page == PAGE_DONE || s->page == PAGE_FAILED || s->page == PAGE_NONE) SetRectEmpty( cancel );
    else SetRectEmpty( primary );
}

static void device_rect( struct sender *s, int i, RECT *rect )
{
    RECT client;
    int top = PAD + px( 36 ) + px( 32 ) + i * (ROW_HEIGHT + px( 4 ));

    GetClientRect( s->hwnd, &client );
    SetRect( rect, PAD, top, client.right - PAD, top + ROW_HEIGHT );
}

static enum hit hit_test( struct sender *s, POINT pt, int *device )
{
    RECT primary, cancel, rect;

    *device = -1;
    footer_buttons( s, &primary, &cancel );
    if (PtInRect( &primary, pt )) return HIT_PRIMARY;
    if (PtInRect( &cancel, pt )) return HIT_CANCEL;
    if (s->page != PAGE_DEVICES) return HIT_NONE;
    for (UINT i = 0; i < s->device_count; i++)
    {
        device_rect( s, i, &rect );
        if (PtInRect( &rect, pt ))
        {
            *device = i;
            return HIT_DEVICE;
        }
    }
    return HIT_NONE;
}

static void paint_button( struct sender *s, HDC hdc, const RECT *rect, const WCHAR *text, BOOL primary, enum hit which,
                          COLORREF card, COLORREF card_hot, COLORREF ink )
{
    BOOL hot = s->hot == which, down = hot && s->pressed == which, dark = cp_dark();
    RECT r = *rect;

    if (IsRectEmpty( rect )) return;
    if (primary) fluent_rect( hdc, rect, px( 4 ), FLUENT_ACCENT( !dark ), down ? 0xcc : hot ? 0xe6 : 0xff );
    else
    {
        fluent_rect( hdc, rect, px( 4 ), hot ? card_hot : card, 255 );
        fluent_box( hdc, rect->left, rect->top, rect->right, rect->bottom, px( 4 ), 1.f,
                    dark ? RGB( 0x3a, 0x3a, 0x3a ) : RGB( 0xd5, 0xd5, 0xd5 ), 255 );
    }
    draw_text( hdc, text, &r, s->body, primary ? FLUENT_ON_ACCENT( !dark ) : ink, DT_CENTER | DT_VCENTER | DT_SINGLELINE );
}

static void on_paint( struct sender *s )
{
    COLORREF page, card, card_hot, ink, gray, footer;
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint( s->hwnd, &ps ), mem = CreateCompatibleDC( hdc );
    RECT client, rect, primary, cancel;
    HBITMAP bitmap;
    HBRUSH brush;
    int y;

    colors( &page, &card, &card_hot, &ink, &gray, &footer );
    GetClientRect( s->hwnd, &client );
    bitmap = CreateCompatibleBitmap( hdc, client.right, client.bottom );
    SelectObject( mem, bitmap );
    brush = CreateSolidBrush( page );
    FillRect( mem, &client, brush );
    DeleteObject( brush );
    SetBkMode( mem, TRANSPARENT );

    SetRect( &rect, PAD, PAD, client.right - PAD, PAD + px( 36 ) );
    draw_text( mem, load_string( IDS_SEND_TITLE ), &rect, s->title, ink, DT_LEFT | DT_SINGLELINE );
    y = PAD + px( 36 );

    switch (s->page)
    {
    case PAGE_DEVICES:
        SetRect( &rect, PAD, y, client.right - PAD, y + px( 24 ) );
        draw_text( mem, load_string( IDS_SEND_CHOOSE ), &rect, s->body, ink, DT_LEFT | DT_SINGLELINE );
        for (UINT i = 0; i < s->device_count; i++)
        {
            const BLUETOOTH_DEVICE_INFO *info = &s->devices[i].info;
            int size = px( 24 ), mid;

            device_rect( s, i, &rect );
            fluent_rect( mem, &rect, px( 6 ), s->hot == HIT_DEVICE && s->hot_device == (int)i ? card_hot : card, 255 );
            mid = (rect.top + rect.bottom) / 2;
            draw_device_glyph( mem, glyph_of_class( info->ulClassofDevice ), rect.left + px( 16 ), mid - size / 2, size, ink );
            rect.left += px( 56 );
            draw_text( mem, info->szName, &rect, s->body, ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS );
        }
        break;
    case PAGE_NONE:
        SetRect( &rect, PAD, y, client.right - PAD, y + px( 80 ) );
        draw_text( mem, load_string( IDS_SEND_NO_DEVICES ), &rect, s->body, gray, DT_LEFT | DT_WORDBREAK );
        break;
    case PAGE_SENDING:
    case PAGE_DONE:
    case PAGE_FAILED:
    {
        const WCHAR *file = s->current < s->file_count ? s->files[s->current] : L"";
        const WCHAR *base = wcsrchr( file, '\\' );
        int size = px( 48 ), cx = client.right / 2;
        float done = s->total ? (float)s->sent / s->total : 0;

        y += px( 16 );
        draw_device_glyph( mem, glyph_of_class( s->target.ulClassofDevice ), cx - size / 2, y, size, ink );
        y += size + px( 12 );
        SetRect( &rect, PAD, y, client.right - PAD, y + px( 24 ) );
        draw_text( mem, s->target.szName, &rect, s->bold, ink, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS );
        y += px( 36 );
        if (s->page == PAGE_SENDING)
        {
            SetRect( &rect, PAD, y, client.right - PAD, y + px( 22 ) );
            draw_text( mem, base ? base + 1 : file, &rect, s->body, gray, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS );
            y += px( 32 );
            /* ProgressBar: a 4 high rounded track, the accent over its done part */
            fluent_box( mem, PAD, y, client.right - PAD, y + px( 4 ), px( 2 ), 0, ink, 0x40 );
            if (done > 0)
                fluent_box( mem, PAD, y, PAD + (client.right - 2 * PAD) * done, y + px( 4 ), px( 2 ), 0,
                            FLUENT_ACCENT( !cp_dark() ), 255 );
            y += px( 16 );
            SetRect( &rect, PAD, y, client.right - PAD, y + px( 22 ) );
            draw_text( mem, format_string( IDS_SEND_PROGRESS, s->current + 1, s->file_count, (int)(done * 100 + 0.5f) ),
                       &rect, s->body, gray, DT_CENTER | DT_SINGLELINE );
        }
        else
        {
            SetRect( &rect, PAD, y, client.right - PAD, y + px( 60 ) );
            draw_text( mem, load_string( s->page == PAGE_DONE ? IDS_SEND_DONE : IDS_SEND_FAILED ), &rect, s->body, gray,
                       DT_CENTER | DT_WORDBREAK );
        }
        break;
    }
    }

    SetRect( &rect, 0, client.bottom - FOOTER, client.right, client.bottom );
    brush = CreateSolidBrush( footer );
    FillRect( mem, &rect, brush );
    DeleteObject( brush );
    footer_buttons( s, &primary, &cancel );
    paint_button( s, mem, &primary, load_string( IDS_WIZ_DONE ), TRUE, HIT_PRIMARY, card, card_hot, ink );
    paint_button( s, mem, &cancel, load_string( IDS_WIZ_CANCEL ), FALSE, HIT_CANCEL, card, card_hot, ink );

    BitBlt( hdc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY );
    DeleteDC( mem );
    DeleteObject( bitmap );
    EndPaint( s->hwnd, &ps );
}

/**********************************************************************
 *          Sending
 */

struct send_args
{
    HWND hwnd;
    BTH_ADDR address;
    WCHAR **files;
    UINT count;
};

static DWORD WINAPI send_proc( void *arg )
{
    struct send_args *args = arg;
    char *(CDECL *unix_name)( const WCHAR * ) =
        (void *)GetProcAddress( GetModuleHandleW( L"kernel32.dll" ), "wine_get_unix_file_name" );
    HANDLE radio = bt_open_radio();
    DWORD error = 0, bytes;

    for (UINT i = 0; i < args->count && !error; i++)
    {
        struct winebth_radio_obex_send_params params = { .address = args->address };
        char *path;

        PostMessageW( args->hwnd, WM_SEND_FILE, i, 0 );
        if (!radio || !unix_name || !(path = unix_name( args->files[i] )))
        {
            error = ERROR_FILE_NOT_FOUND;
            break;
        }
        lstrcpynA( params.unix_path, path, sizeof(params.unix_path) );
        HeapFree( GetProcessHeap(), 0, path );
        if (!DeviceIoControl( radio, IOCTL_WINEBTH_RADIO_OBEX_SEND, &params, sizeof(params), NULL, 0, &bytes, NULL ))
            error = GetLastError();
    }
    if (radio) CloseHandle( radio );
    if (IsWindow( args->hwnd )) PostMessageW( args->hwnd, WM_SEND_DONE, error, 0 );
    free( args );
    return 0;
}

static void start_sending( struct sender *s, int device )
{
    struct send_args *args = malloc( sizeof(*args) );
    HANDLE thread = NULL;

    s->target = s->devices[device].info;
    s->page = PAGE_SENDING;
    s->current = 0;
    s->sent = s->total = 0;
    if (args)
    {
        args->hwnd = s->hwnd;
        args->address = s->target.Address.ullLong;
        args->files = s->files;
        args->count = s->file_count;
        if (!(thread = CreateThread( NULL, 0, send_proc, args, 0, NULL ))) free( args );
    }
    if (thread) CloseHandle( thread );
    else s->page = PAGE_FAILED;
    SetTimer( s->hwnd, TIMER_PROGRESS, 200, NULL );
    InvalidateRect( s->hwnd, NULL, FALSE );
}

static void cancel_sending(void)
{
    HANDLE radio = bt_open_radio();
    DWORD bytes;

    if (!radio) return;
    DeviceIoControl( radio, IOCTL_WINEBTH_RADIO_OBEX_CANCEL, NULL, 0, NULL, 0, &bytes, NULL );
    CloseHandle( radio );
}

static void poll_progress( struct sender *s )
{
    struct winebth_radio_obex_progress progress;
    HANDLE radio = bt_open_radio();
    DWORD bytes;

    if (!radio) return;
    if (DeviceIoControl( radio, IOCTL_WINEBTH_RADIO_OBEX_PROGRESS, NULL, 0, &progress, sizeof(progress), &bytes, NULL ))
    {
        s->sent = progress.sent;
        s->total = progress.total;
        InvalidateRect( s->hwnd, NULL, FALSE );
    }
    CloseHandle( radio );
}

static LRESULT CALLBACK sender_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct sender *s = (struct sender *)GetWindowLongPtrW( hwnd, GWLP_USERDATA );

    if (msg == WM_NCCREATE)
    {
        s = ((CREATESTRUCTW *)lparam)->lpCreateParams;
        s->hwnd = hwnd;
        SetWindowLongPtrW( hwnd, GWLP_USERDATA, (LONG_PTR)s );
    }
    if (!s) return DefWindowProcW( hwnd, msg, wparam, lparam );

    switch (msg)
    {
    case WM_PAINT:
        on_paint( s );
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        poll_progress( s );
        return 0;
    case WM_MOUSEMOVE:
    {
        POINT pt = { (short)LOWORD( lparam ), (short)HIWORD( lparam ) };
        TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, hwnd };
        int device;
        enum hit hit = hit_test( s, pt, &device );

        if (hit != s->hot || device != s->hot_device)
        {
            s->hot = hit;
            s->hot_device = device;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        TrackMouseEvent( &track );
        return 0;
    }
    case WM_MOUSELEAVE:
        s->hot = s->pressed = HIT_NONE;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    case WM_LBUTTONDOWN:
    {
        POINT pt = { (short)LOWORD( lparam ), (short)HIWORD( lparam ) };
        s->pressed = hit_test( s, pt, &s->pressed_device );
        return 0;
    }
    case WM_LBUTTONUP:
    {
        POINT pt = { (short)LOWORD( lparam ), (short)HIWORD( lparam ) };
        int device;
        enum hit hit = hit_test( s, pt, &device ), pressed = s->pressed;

        s->pressed = HIT_NONE;
        if (hit != pressed || device != s->pressed_device) return 0;
        if (hit == HIT_DEVICE && s->page == PAGE_DEVICES) start_sending( s, device );
        else if (hit == HIT_PRIMARY) DestroyWindow( hwnd );
        else if (hit == HIT_CANCEL)
        {
            if (s->page == PAGE_SENDING) cancel_sending();
            else DestroyWindow( hwnd );
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE)
        {
            if (s->page == PAGE_SENDING) cancel_sending();
            else DestroyWindow( hwnd );
        }
        return 0;
    case WM_SEND_FILE:
        s->current = wparam;
        s->sent = s->total = 0;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    case WM_SEND_DONE:
        KillTimer( hwnd, TIMER_PROGRESS );
        s->error = wparam;
        s->page = wparam ? PAGE_FAILED : PAGE_DONE;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    case WM_DESTROY:
        KillTimer( hwnd, TIMER_PROGRESS );
        s->done = TRUE;
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

/* the files to send: those given, or chosen in the Open dialog */
static UINT choose_files( HWND owner, WCHAR ***files )
{
    WCHAR *buffer = calloc( 32768, sizeof(WCHAR) ), *p;
    OPENFILENAMEW ofn = { sizeof(ofn) };
    UINT count = 0;

    if (!buffer) return 0;
    ofn.hwndOwner = owner;
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = 32768;
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST;
    if (!GetOpenFileNameW( &ofn ) || !(*files = calloc( 256, sizeof(WCHAR *) )))
    {
        free( buffer );
        return 0;
    }
    p = buffer + wcslen( buffer ) + 1;
    if (!*p) (*files)[count++] = _wcsdup( buffer );   /* one file: the whole path */
    for (; *p && count < 256; p += wcslen( p ) + 1)
    {
        WCHAR *path = malloc( (wcslen( buffer ) + wcslen( p ) + 2) * sizeof(WCHAR) );
        if (!path) break;
        swprintf( path, wcslen( buffer ) + wcslen( p ) + 2, L"%s\\%s", buffer, p );
        (*files)[count++] = path;
    }
    free( buffer );
    return count;
}

void WINAPI ArcticSendFiles( HWND owner, int argc, WCHAR **argv )
{
    WNDCLASSW cls = { .lpfnWndProc = sender_proc, .hInstance = cp_instance, .hCursor = LoadCursorW( NULL, (LPCWSTR)IDC_ARROW ),
                      .lpszClassName = L"ArcticSendFiles" };
    struct sender *s = calloc( 1, sizeof(*s) );
    RECT area, frame = { 0, 0, WIDTH, HEIGHT };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, dark = cp_dark();
    HANDLE radio;
    MSG msg;

    if (!s) return;
    if (argc > 0)
    {
        s->files = argv;
        s->file_count = argc;
    }
    else if (!(s->file_count = choose_files( owner, &s->files )))
    {
        free( s );
        return;
    }
    if ((radio = bt_open_radio()))
    {
        BOOL powered = FALSE;

        bt_radio_state( radio, &powered, NULL, NULL );
        if (!powered) bt_set_power( radio, TRUE );
        s->device_count = bt_devices( radio, s->devices, ARRAY_SIZE(s->devices), TRUE );
        CloseHandle( radio );
    }
    s->page = s->device_count ? PAGE_DEVICES : PAGE_NONE;
    s->hot_device = s->pressed_device = -1;
    s->title = make_font( px( 26 ), FW_SEMIBOLD );
    s->body = make_font( px( 14 ), FW_NORMAL );
    s->bold = make_font( px( 16 ), FW_SEMIBOLD );

    RegisterClassW( &cls );
    AdjustWindowRectEx( &frame, style, FALSE, 0 );
    SystemParametersInfoW( SPI_GETWORKAREA, 0, &area, 0 );
    if (!CreateWindowExW( 0, cls.lpszClassName, load_string( IDS_SEND_TITLE ), style,
                          (area.left + area.right - (frame.right - frame.left)) / 2,
                          (area.top + area.bottom - (frame.bottom - frame.top)) / 2,
                          frame.right - frame.left, frame.bottom - frame.top, owner, NULL, cp_instance, s ))
    {
        free( s );
        return;
    }
    DwmSetWindowAttribute( s->hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark) );
    ShowWindow( s->hwnd, SW_SHOW );
    SetForegroundWindow( s->hwnd );
    while (!s->done && GetMessageW( &msg, NULL, 0, 0 ))
    {
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }
    DeleteObject( s->title );
    DeleteObject( s->body );
    DeleteObject( s->bold );
    /* the files the dialog chose: the sending thread has copies of nothing else, it is done or cancelled */
    free( s );
}
