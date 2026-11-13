/*
 * Devices and Printers: "Додавання пристрою", the wizard of Windows 11
 *
 * As Windows 11's (Settings, Bluetooth and devices, Add device): first the
 * kind of device, Bluetooth or a printer; for Bluetooth the radio is
 * switched on, the PC made visible and the devices around listed as they
 * are found; a click pairs (BluetoothAuthenticateDeviceEx), the request of
 * the device answered here (BluetoothRegisterForAuthenticationEx): the PIN
 * to compare, the PIN to type on the device, the one shown on it, or a
 * legacy PIN; then "your device is ready to go". Headphones play at once:
 * winebth.sys connects their music after the pairing.
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
#include "dwmapi.h"
#include "shellapi.h"
#include "bthsdpdef.h"
#include "bluetoothapis.h"
#include "bthdef.h"
#include "wine/arctic_shell.h"
#include "wine/arctic_fluent.h"

#include "cpanel.h"
#include "devicescpl.h"

#define WM_AUTH_REQUEST   (WM_APP + 1)   /* wparam: the method, lparam: the number */
#define WM_PAIR_DONE      (WM_APP + 2)   /* wparam: the result */
#define TIMER_SCAN        1
#define TIMER_SPIN        2

#define WIDTH             px(500)
#define HEIGHT            px(580)
#define PAD               px(24)
#define FOOTER            px(80)
#define OPTION_HEIGHT     px(72)
#define ROW_HEIGHT        px(56)
#define BUTTON_HEIGHT     px(32)
#define MAX_FOUND         32

enum page { PAGE_CHOOSE, PAGE_SEARCH, PAGE_PAIR, PAGE_DONE, PAGE_FAILED, PAGE_NO_RADIO };
enum step { STEP_WAIT, STEP_COMPARE, STEP_SHOW_KEY, STEP_ENTER_KEY, STEP_ENTER_PIN };
enum hit { HIT_NONE, HIT_BLUETOOTH, HIT_PRINTER, HIT_CANCEL, HIT_PRIMARY, HIT_DEVICE };

struct wizard
{
    HWND hwnd, owner, edit;
    enum page page;
    enum step step;
    HANDLE radio;
    HBLUETOOTH_AUTHENTICATION_REGISTRATION auth;
    struct bt_device found[MAX_FOUND];
    UINT count;
    BLUETOOTH_DEVICE_INFO target;
    BLUETOOTH_AUTHENTICATION_METHOD method;
    UINT32 number;
    enum hit hot, pressed;
    int hot_device, pressed_device, scroll;
    DWORD spin_start;
    BOOL discovering, done;
    HFONT title, body, bold, caption, digits;
};

/**********************************************************************
 *          Colours, as Windows 11's dialogs have them
 */

struct colors
{
    COLORREF page, card, card_hot, edge, text, gray, footer, accent, on_accent;
};

static void get_colors( struct colors *c )
{
    BOOL dark = cp_dark();

    c->page = dark ? RGB( 0x20, 0x20, 0x20 ) : RGB( 0xf3, 0xf3, 0xf3 );
    c->card = dark ? RGB( 0x2b, 0x2b, 0x2b ) : RGB( 0xfb, 0xfb, 0xfb );
    c->card_hot = dark ? RGB( 0x32, 0x32, 0x32 ) : RGB( 0xf5, 0xf5, 0xf5 );
    c->edge = dark ? RGB( 0x1d, 0x1d, 0x1d ) : RGB( 0xe5, 0xe5, 0xe5 );
    c->text = dark ? RGB( 0xff, 0xff, 0xff ) : RGB( 0x1b, 0x1b, 0x1b );
    c->gray = dark ? RGB( 0xc5, 0xc5, 0xc5 ) : RGB( 0x5d, 0x5d, 0x5d );
    c->footer = dark ? RGB( 0x1b, 0x1b, 0x1b ) : RGB( 0xe9, 0xe9, 0xe9 );
    c->accent = FLUENT_ACCENT( !dark );
    c->on_accent = dark ? RGB( 0, 0, 0 ) : RGB( 0xff, 0xff, 0xff );
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

static int text_height( HDC hdc, HFONT font, const WCHAR *text, int width )
{
    RECT rect = { 0, 0, width, 0 };
    HGDIOBJ old = SelectObject( hdc, font );

    DrawTextW( hdc, text, -1, &rect, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX );
    SelectObject( hdc, old );
    return rect.bottom;
}

static void draw_text( HDC hdc, const WCHAR *text, RECT *rect, HFONT font, COLORREF color, UINT flags )
{
    HGDIOBJ old = SelectObject( hdc, font );

    SetTextColor( hdc, color );
    DrawTextW( hdc, text, -1, rect, flags | DT_NOPREFIX );
    SelectObject( hdc, old );
}

/* Windows 11's ProgressRing: an arc that grows and turns */
static void draw_spinner( HDC hdc, int cx, int cy, int radius, DWORD ms, COLORREF color )
{
    float turn = (ms % 1400) / 1400.f * 2 * 3.14159265f, sweep = 0.6f + 2.6f * (0.5f - 0.5f * cosf( ms / 700.f * 3.14159265f ));
    float w = max( 2.f, radius / 5.f );
    int steps = 16;

    for (int i = 0; i < steps; i++)
    {
        float a0 = turn + sweep * i / steps - 1.5708f, a1 = turn + sweep * (i + 1) / steps - 1.5708f;
        fluent_line( hdc, cx + radius * cosf( a0 ), cy + radius * sinf( a0 ), cx + radius * cosf( a1 ),
                     cy + radius * sinf( a1 ), w, color, 255 );
    }
}

/**********************************************************************
 *          Layout
 */

static void client_rect( struct wizard *w, RECT *rect )
{
    GetClientRect( w->hwnd, rect );
}

/* the footer's buttons, right aligned: the primary one rightmost */
static BOOL has_primary( struct wizard *w )
{
    return w->page == PAGE_DONE || w->page == PAGE_FAILED || w->page == PAGE_NO_RADIO ||
           (w->page == PAGE_PAIR && (w->step == STEP_COMPARE || w->step == STEP_ENTER_KEY || w->step == STEP_ENTER_PIN));
}

static BOOL has_cancel( struct wizard *w )
{
    return w->page != PAGE_DONE && w->page != PAGE_NO_RADIO;
}

static const WCHAR *primary_text( struct wizard *w )
{
    if (w->page == PAGE_PAIR) return load_string( IDS_WIZ_CONNECT );
    return load_string( IDS_WIZ_DONE );
}

static const WCHAR *cancel_text( struct wizard *w )
{
    return load_string( w->page == PAGE_FAILED ? IDS_WIZ_RETRY : IDS_WIZ_CANCEL );
}

static void footer_buttons( struct wizard *w, RECT *primary, RECT *cancel )
{
    RECT client;
    int width = px( 120 ), y;

    client_rect( w, &client );
    y = client.bottom - FOOTER + (FOOTER - BUTTON_HEIGHT) / 2;
    SetRect( primary, client.right - PAD - width, y, client.right - PAD, y + BUTTON_HEIGHT );
    *cancel = *primary;
    if (has_primary( w )) OffsetRect( cancel, -(width + px( 8 )), 0 );
    if (!has_primary( w )) SetRectEmpty( primary );
    if (!has_cancel( w )) SetRectEmpty( cancel );
}

/* where the list or the options start */
static int content_top( struct wizard *w, HDC hdc, const WCHAR *subtitle )
{
    RECT client;
    int top = PAD + px( 36 );

    client_rect( w, &client );
    if (subtitle && subtitle[0]) top += text_height( hdc, w->body, subtitle, client.right - 2 * PAD ) + px( 16 );
    return top;
}

static const WCHAR *subtitle_of( struct wizard *w )
{
    switch (w->page)
    {
    case PAGE_CHOOSE: return load_string( IDS_WIZ_CHOOSE );
    case PAGE_SEARCH: return load_string( IDS_WIZ_SEARCH );
    default: return NULL;
    }
}

static void option_rect( struct wizard *w, HDC hdc, int index, RECT *rect )
{
    RECT client;
    int top = content_top( w, hdc, subtitle_of( w ) ) + index * (OPTION_HEIGHT + px( 4 ));

    client_rect( w, &client );
    SetRect( rect, PAD, top, client.right - PAD, top + OPTION_HEIGHT );
}

static void device_rect( struct wizard *w, HDC hdc, int index, RECT *rect )
{
    RECT client;
    int top = content_top( w, hdc, subtitle_of( w ) ) + index * (ROW_HEIGHT + px( 4 )) - w->scroll;

    client_rect( w, &client );
    SetRect( rect, PAD, top, client.right - PAD, top + ROW_HEIGHT );
}

static enum hit hit_test( struct wizard *w, POINT pt, int *device )
{
    RECT rect, primary, cancel, client;
    HDC hdc = GetDC( w->hwnd );
    enum hit hit = HIT_NONE;

    *device = -1;
    client_rect( w, &client );
    footer_buttons( w, &primary, &cancel );
    if (PtInRect( &primary, pt )) hit = HIT_PRIMARY;
    else if (PtInRect( &cancel, pt )) hit = HIT_CANCEL;
    else if (w->page == PAGE_CHOOSE)
    {
        if (option_rect( w, hdc, 0, &rect ), PtInRect( &rect, pt )) hit = HIT_BLUETOOTH;
        else if (option_rect( w, hdc, 1, &rect ), PtInRect( &rect, pt )) hit = HIT_PRINTER;
    }
    else if (w->page == PAGE_SEARCH && pt.y < client.bottom - FOOTER)
    {
        for (UINT i = 0; i < w->count; i++)
        {
            device_rect( w, hdc, i, &rect );
            if (PtInRect( &rect, pt ))
            {
                hit = HIT_DEVICE;
                *device = i;
                break;
            }
        }
    }
    ReleaseDC( w->hwnd, hdc );
    return hit;
}

/**********************************************************************
 *          Painting
 */

static void paint_button( struct wizard *w, HDC hdc, const struct colors *c, const RECT *rect, const WCHAR *text,
                          BOOL primary, enum hit which )
{
    BOOL hot = w->hot == which, down = hot && w->pressed == which;
    RECT r = *rect;

    if (IsRectEmpty( rect )) return;
    if (primary)
        fluent_rect( hdc, rect, px( 4 ), c->accent, down ? 0xcc : hot ? 0xe6 : 0xff );
    else
    {
        fluent_rect( hdc, rect, px( 4 ), down ? c->card : hot ? c->card_hot : c->card, 255 );
        fluent_box( hdc, rect->left, rect->top, rect->right, rect->bottom, px( 4 ), 1.f,
                    cp_dark() ? RGB( 0x3a, 0x3a, 0x3a ) : RGB( 0xd5, 0xd5, 0xd5 ), 255 );
    }
    draw_text( hdc, text, &r, w->body, primary ? c->on_accent : c->text, DT_CENTER | DT_VCENTER | DT_SINGLELINE );
}

static void paint_option( struct wizard *w, HDC hdc, const struct colors *c, int index, enum device_glyph glyph,
                          UINT title, UINT hint, enum hit which )
{
    RECT rect, text;
    int glyph_size = px( 28 ), mid;

    option_rect( w, hdc, index, &rect );
    fluent_rect( hdc, &rect, px( 6 ), w->hot == which ? c->card_hot : c->card, 255 );
    fluent_box( hdc, rect.left, rect.top, rect.right, rect.bottom, px( 6 ), 1.f, c->edge, 255 );
    mid = (rect.top + rect.bottom) / 2;
    draw_device_glyph( hdc, glyph, rect.left + px( 20 ), mid - glyph_size / 2, glyph_size, c->text );
    SetRect( &text, rect.left + px( 68 ), mid - px( 20 ), rect.right - px( 16 ), mid );
    draw_text( hdc, load_string( title ), &text, w->bold, c->text, DT_LEFT | DT_BOTTOM | DT_SINGLELINE );
    SetRect( &text, rect.left + px( 68 ), mid + px( 2 ), rect.right - px( 16 ), mid + px( 22 ) );
    draw_text( hdc, load_string( hint ), &text, w->caption, c->gray, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS );
}

static void paint_search( struct wizard *w, HDC hdc, const struct colors *c, const RECT *client )
{
    int glyph_size = px( 24 ), top = content_top( w, hdc, subtitle_of( w ) );
    HRGN clip = CreateRectRgn( 0, top, client->right, client->bottom - FOOTER );
    RECT rect, text;

    SelectClipRgn( hdc, clip );
    for (UINT i = 0; i < w->count; i++)
    {
        const BLUETOOTH_DEVICE_INFO *info = &w->found[i].info;
        int mid;

        device_rect( w, hdc, i, &rect );
        fluent_rect( hdc, &rect, px( 6 ), w->hot == HIT_DEVICE && w->hot_device == (int)i ? c->card_hot : c->card, 255 );
        mid = (rect.top + rect.bottom) / 2;
        draw_device_glyph( hdc, glyph_of_class( info->ulClassofDevice ), rect.left + px( 16 ), mid - glyph_size / 2,
                           glyph_size, c->text );
        SetRect( &text, rect.left + px( 56 ), rect.top, rect.right - px( 16 ), rect.bottom );
        draw_text( hdc, info->szName, &text, w->body, c->text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS );
    }
    /* still looking */
    device_rect( w, hdc, w->count, &rect );
    draw_spinner( hdc, rect.left + px( 28 ), (rect.top + rect.bottom) / 2, px( 9 ), GetTickCount() - w->spin_start,
                  c->accent );
    SetRect( &text, rect.left + px( 56 ), rect.top, rect.right, rect.bottom );
    draw_text( hdc, load_string( IDS_WIZ_SEARCHING ), &text, w->body, c->gray, DT_LEFT | DT_VCENTER | DT_SINGLELINE );
    SelectClipRgn( hdc, NULL );
    DeleteObject( clip );
}

/* the device, large, its name under it, then what the pairing asks */
static void paint_pair( struct wizard *w, HDC hdc, const struct colors *c, const RECT *client )
{
    int glyph_size = px( 56 ), y = PAD + px( 56 ), cx = client->right / 2;
    const WCHAR *name = w->target.szName[0] ? w->target.szName : load_string( IDS_WIZ_UNNAMED );
    WCHAR digits[16];
    RECT text;

    draw_device_glyph( hdc, w->page == PAGE_FAILED ? GLYPH_BLUETOOTH : glyph_of_class( w->target.ulClassofDevice ),
                       cx - glyph_size / 2, y, glyph_size, c->text );
    y += glyph_size + px( 16 );
    SetRect( &text, PAD, y, client->right - PAD, y + px( 28 ) );
    draw_text( hdc, name, &text, w->bold, c->text, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS );
    y += px( 40 );

    if (w->page == PAGE_DONE)
    {
        SetRect( &text, PAD, y, client->right - PAD, y + px( 60 ) );
        draw_text( hdc, load_string( IDS_WIZ_READY ), &text, w->body, c->gray, DT_CENTER | DT_WORDBREAK );
        return;
    }
    if (w->page == PAGE_FAILED)
    {
        SetRect( &text, PAD, y, client->right - PAD, y + px( 28 ) );
        draw_text( hdc, load_string( IDS_WIZ_FAILED ), &text, w->bold, c->text, DT_CENTER | DT_SINGLELINE );
        y += px( 32 );
        SetRect( &text, PAD, y, client->right - PAD, y + px( 60 ) );
        draw_text( hdc, load_string( IDS_WIZ_FAILED_HINT ), &text, w->body, c->gray, DT_CENTER | DT_WORDBREAK );
        return;
    }

    switch (w->step)
    {
    case STEP_WAIT:
        draw_spinner( hdc, cx, y + px( 14 ), px( 12 ), GetTickCount() - w->spin_start, c->accent );
        SetRect( &text, PAD, y + px( 36 ), client->right - PAD, y + px( 64 ) );
        draw_text( hdc, load_string( IDS_WIZ_PAIRING ), &text, w->body, c->gray, DT_CENTER | DT_SINGLELINE );
        break;
    case STEP_COMPARE:
    case STEP_SHOW_KEY:
        SetRect( &text, PAD, y, client->right - PAD, y + px( 44 ) );
        draw_text( hdc, load_string( w->step == STEP_COMPARE ? IDS_WIZ_COMPARE : IDS_WIZ_TYPE_ON_DEVICE ), &text,
                   w->body, c->gray, DT_CENTER | DT_WORDBREAK );
        swprintf( digits, ARRAY_SIZE(digits), L"%03u %03u", w->number / 1000 % 1000, w->number % 1000 );
        SetRect( &text, PAD, y + px( 52 ), client->right - PAD, y + px( 104 ) );
        draw_text( hdc, digits, &text, w->digits, c->text, DT_CENTER | DT_SINGLELINE | DT_VCENTER );
        break;
    case STEP_ENTER_KEY:
    case STEP_ENTER_PIN:
        SetRect( &text, PAD, y, client->right - PAD, y + px( 44 ) );
        draw_text( hdc, load_string( w->step == STEP_ENTER_KEY ? IDS_WIZ_ENTER_PASSKEY : IDS_WIZ_ENTER_PIN ), &text,
                   w->body, c->gray, DT_CENTER | DT_WORDBREAK );
        break;
    }
}

static void on_paint( struct wizard *w )
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint( w->hwnd, &ps ), mem = CreateCompatibleDC( hdc );
    struct colors c;
    RECT client, rect, primary, cancel;
    HBITMAP bitmap;
    HBRUSH brush;

    get_colors( &c );
    client_rect( w, &client );
    bitmap = CreateCompatibleBitmap( hdc, client.right, client.bottom );
    SelectObject( mem, bitmap );
    brush = CreateSolidBrush( c.page );
    FillRect( mem, &client, brush );
    DeleteObject( brush );
    SetBkMode( mem, TRANSPARENT );

    if (w->page == PAGE_CHOOSE || w->page == PAGE_SEARCH || w->page == PAGE_NO_RADIO)
    {
        const WCHAR *subtitle = subtitle_of( w );

        SetRect( &rect, PAD, PAD, client.right - PAD, PAD + px( 36 ) );
        draw_text( mem, load_string( IDS_WIZ_TITLE ), &rect, w->title, c.text, DT_LEFT | DT_SINGLELINE );
        if (subtitle)
        {
            SetRect( &rect, PAD, PAD + px( 36 ), client.right - PAD, PAD + px( 36 ) + px( 80 ) );
            draw_text( mem, subtitle, &rect, w->body, c.text, DT_LEFT | DT_WORDBREAK );
        }
        if (w->page == PAGE_NO_RADIO)
        {
            SetRect( &rect, PAD, PAD + px( 52 ), client.right - PAD, PAD + px( 120 ) );
            draw_text( mem, load_string( IDS_WIZ_NO_RADIO ), &rect, w->body, c.gray, DT_LEFT | DT_WORDBREAK );
        }
    }
    if (w->page == PAGE_CHOOSE)
    {
        paint_option( w, mem, &c, 0, GLYPH_BLUETOOTH, IDS_BLUETOOTH, IDS_WIZ_BLUETOOTH_HINT, HIT_BLUETOOTH );
        paint_option( w, mem, &c, 1, GLYPH_PRINTER, IDS_WIZ_PRINTER, IDS_WIZ_PRINTER_HINT, HIT_PRINTER );
    }
    else if (w->page == PAGE_SEARCH) paint_search( w, mem, &c, &client );
    else if (w->page == PAGE_PAIR || w->page == PAGE_DONE || w->page == PAGE_FAILED) paint_pair( w, mem, &c, &client );

    /* the footer */
    SetRect( &rect, 0, client.bottom - FOOTER, client.right, client.bottom );
    brush = CreateSolidBrush( c.footer );
    FillRect( mem, &rect, brush );
    DeleteObject( brush );
    footer_buttons( w, &primary, &cancel );
    paint_button( w, mem, &c, &primary, primary_text( w ), TRUE, HIT_PRIMARY );
    paint_button( w, mem, &c, &cancel, cancel_text( w ), FALSE, HIT_CANCEL );

    BitBlt( hdc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY );
    DeleteDC( mem );
    DeleteObject( bitmap );
    EndPaint( w->hwnd, &ps );
}

/**********************************************************************
 *          Pairing
 */

static BOOL CALLBACK auth_callback( void *context, BLUETOOTH_AUTHENTICATION_CALLBACK_PARAMS *params )
{
    struct wizard *w = context;

    /* the device being paired only; the rest is not this wizard's */
    if (params->deviceInfo.Address.ullLong != w->target.Address.ullLong) return TRUE;
    PostMessageW( w->hwnd, WM_AUTH_REQUEST, params->authenticationMethod, params->Numeric_Value );
    return TRUE;
}

/* the pairing's own thread may outlive the wizard: it has copies */
struct pair_args
{
    HWND hwnd;
    BLUETOOTH_DEVICE_INFO info;
};

static DWORD WINAPI pair_proc( void *arg )
{
    struct pair_args *args = arg;
    HANDLE radio = bt_open_radio();
    DWORD ret;

    if (!radio) ret = ERROR_NOT_FOUND;
    else
    {
        ret = BluetoothAuthenticateDeviceEx( NULL, radio, &args->info, NULL, MITMProtectionNotRequired );
        CloseHandle( radio );
    }
    if (IsWindow( args->hwnd )) PostMessageW( args->hwnd, WM_PAIR_DONE, ret, 0 );
    free( args );
    return 0;
}

static void answer( struct wizard *w, BOOL yes )
{
    BLUETOOTH_AUTHENTICATE_RESPONSE response = {0};
    WCHAR text[32] = L"";

    response.bthAddressRemote = w->target.Address;
    response.authMethod = w->method;
    response.negativeResponse = !yes;
    if (w->edit) GetWindowTextW( w->edit, text, ARRAY_SIZE(text) );
    switch (w->method)
    {
    case BLUETOOTH_AUTHENTICATION_METHOD_NUMERIC_COMPARISON:
    case BLUETOOTH_AUTHENTICATION_METHOD_PASSKEY_NOTIFICATION:
        response.numericCompInfo.NumericValue = w->number;
        break;
    case BLUETOOTH_AUTHENTICATION_METHOD_PASSKEY:
        response.passkeyInfo.passkey = wcstoul( text, NULL, 10 );
        break;
    case BLUETOOTH_AUTHENTICATION_METHOD_LEGACY:
        for (int i = 0; text[i] && i < BTH_MAX_PIN_SIZE; i++) response.pinInfo.pin[response.pinInfo.pinLength++] = (UCHAR)text[i];
        break;
    default:
        break;
    }
    BluetoothSendAuthenticationResponseEx( NULL, &response );
    if (w->edit)
    {
        DestroyWindow( w->edit );
        w->edit = NULL;
    }
    w->step = STEP_WAIT;
    InvalidateRect( w->hwnd, NULL, FALSE );
}

static void show_edit( struct wizard *w, BOOL numbers )
{
    RECT client;
    int width = px( 220 ), y = PAD + px( 56 ) + px( 56 ) + px( 16 ) + px( 40 ) + px( 52 );

    client_rect( w, &client );
    w->edit = CreateWindowExW( 0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_CENTER |
                               (numbers ? ES_NUMBER : 0), (client.right - width) / 2, y, width, px( 32 ), w->hwnd,
                               (HMENU)1, NULL, NULL );
    SendMessageW( w->edit, WM_SETFONT, (WPARAM)w->body, TRUE );
    SendMessageW( w->edit, EM_LIMITTEXT, numbers ? 6 : 16, 0 );
    SetFocus( w->edit );
}

static void on_auth_request( struct wizard *w, BLUETOOTH_AUTHENTICATION_METHOD method, UINT32 number )
{
    w->method = method;
    w->number = number;
    if (w->edit)
    {
        DestroyWindow( w->edit );
        w->edit = NULL;
    }
    switch (method)
    {
    case BLUETOOTH_AUTHENTICATION_METHOD_NUMERIC_COMPARISON: w->step = STEP_COMPARE; break;
    case BLUETOOTH_AUTHENTICATION_METHOD_PASSKEY_NOTIFICATION: w->step = STEP_SHOW_KEY; break;
    case BLUETOOTH_AUTHENTICATION_METHOD_PASSKEY: w->step = STEP_ENTER_KEY; show_edit( w, TRUE ); break;
    case BLUETOOTH_AUTHENTICATION_METHOD_LEGACY: w->step = STEP_ENTER_PIN; show_edit( w, FALSE ); break;
    default: answer( w, FALSE ); return;
    }
    InvalidateRect( w->hwnd, NULL, FALSE );
}

/**********************************************************************
 *          Pages
 */

static void stop_search( struct wizard *w )
{
    KillTimer( w->hwnd, TIMER_SCAN );
    if (w->discovering) bt_discovery( w->radio, FALSE );
    w->discovering = FALSE;
}

static void refresh_found( struct wizard *w )
{
    struct bt_device list[MAX_FOUND];
    UINT count = bt_devices( w->radio, list, MAX_FOUND, FALSE );

    if (count != w->count || memcmp( list, w->found, count * sizeof(*list) ))
    {
        memcpy( w->found, list, count * sizeof(*list) );
        w->count = count;
        InvalidateRect( w->hwnd, NULL, FALSE );
    }
}

static void go_search( struct wizard *w )
{
    BOOL powered = FALSE;

    if (!w->radio && !(w->radio = bt_open_radio()))
    {
        w->page = PAGE_NO_RADIO;
        InvalidateRect( w->hwnd, NULL, FALSE );
        return;
    }
    /* as Windows: the radio switched on, the PC visible while the wizard is open */
    bt_radio_state( w->radio, &powered, NULL, NULL );
    if (!powered) bt_set_power( w->radio, TRUE );
    BluetoothEnableDiscovery( w->radio, TRUE );
    if (!w->auth) BluetoothRegisterForAuthenticationEx( NULL, &w->auth, auth_callback, w );
    w->page = PAGE_SEARCH;
    w->count = 0;
    w->scroll = 0;
    bt_discovery( w->radio, TRUE );
    w->discovering = TRUE;
    refresh_found( w );
    SetTimer( w->hwnd, TIMER_SCAN, 700, NULL );
    SetTimer( w->hwnd, TIMER_SPIN, 33, NULL );
    w->spin_start = GetTickCount();
    InvalidateRect( w->hwnd, NULL, FALSE );
}

static void go_pair( struct wizard *w, int index )
{
    struct pair_args *args = malloc( sizeof(*args) );
    HANDLE thread = NULL;

    stop_search( w );
    w->target = w->found[index].info;
    w->page = PAGE_PAIR;
    w->step = STEP_WAIT;
    w->spin_start = GetTickCount();
    SetTimer( w->hwnd, TIMER_SPIN, 33, NULL );
    if (args)
    {
        args->hwnd = w->hwnd;
        args->info = w->target;
        if (!(thread = CreateThread( NULL, 0, pair_proc, args, 0, NULL ))) free( args );
    }
    if (thread) CloseHandle( thread );
    else w->page = PAGE_FAILED;
    InvalidateRect( w->hwnd, NULL, FALSE );
}

static void on_click( struct wizard *w, enum hit hit, int device )
{
    switch (hit)
    {
    case HIT_BLUETOOTH:
        go_search( w );
        break;
    case HIT_PRINTER:
        ShellExecuteW( w->hwnd, NULL, L"explorer.exe",
                       L"::{26EE0668-A00A-44D7-9371-BEB064C98683}\\0\\::{2227A280-3AEA-1069-A2DE-08002B30309D}", NULL,
                       SW_SHOWNORMAL );
        DestroyWindow( w->hwnd );
        break;
    case HIT_DEVICE:
        if (device >= 0 && device < (int)w->count) go_pair( w, device );
        break;
    case HIT_PRIMARY:
        if (w->page == PAGE_PAIR) answer( w, TRUE );
        else DestroyWindow( w->hwnd );
        break;
    case HIT_CANCEL:
        if (w->page == PAGE_FAILED) go_search( w );
        else if (w->page == PAGE_PAIR && w->step != STEP_WAIT) answer( w, FALSE );
        else DestroyWindow( w->hwnd );
        break;
    default:
        break;
    }
}

static LRESULT CALLBACK wizard_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct wizard *w = (struct wizard *)GetWindowLongPtrW( hwnd, GWLP_USERDATA );

    if (msg == WM_NCCREATE)
    {
        w = ((CREATESTRUCTW *)lparam)->lpCreateParams;
        w->hwnd = hwnd;
        SetWindowLongPtrW( hwnd, GWLP_USERDATA, (LONG_PTR)w );
    }
    if (!w) return DefWindowProcW( hwnd, msg, wparam, lparam );

    switch (msg)
    {
    case WM_PAINT:
        on_paint( w );
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        if (wparam == TIMER_SCAN) refresh_found( w );
        else if (wparam == TIMER_SPIN) InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    case WM_MOUSEMOVE:
    {
        POINT pt = { (short)LOWORD( lparam ), (short)HIWORD( lparam ) };
        TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, hwnd };
        int device;
        enum hit hit = hit_test( w, pt, &device );

        if (hit != w->hot || device != w->hot_device)
        {
            w->hot = hit;
            w->hot_device = device;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        TrackMouseEvent( &track );
        return 0;
    }
    case WM_MOUSELEAVE:
        w->hot = w->pressed = HIT_NONE;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    case WM_LBUTTONDOWN:
    {
        POINT pt = { (short)LOWORD( lparam ), (short)HIWORD( lparam ) };
        w->pressed = hit_test( w, pt, &w->pressed_device );
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    }
    case WM_LBUTTONUP:
    {
        POINT pt = { (short)LOWORD( lparam ), (short)HIWORD( lparam ) };
        int device;
        enum hit hit = hit_test( w, pt, &device ), pressed = w->pressed;

        w->pressed = HIT_NONE;
        InvalidateRect( hwnd, NULL, FALSE );
        if (hit == pressed && device == w->pressed_device) on_click( w, hit, device );
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (w->page == PAGE_SEARCH)
        {
            int most = max( 0, (int)(w->count + 1) * (ROW_HEIGHT + px( 4 )) - px( 300 ) );
            w->scroll = max( 0, min( most, w->scroll - GET_WHEEL_DELTA_WPARAM( wparam ) / WHEEL_DELTA * ROW_HEIGHT ) );
            InvalidateRect( hwnd, NULL, FALSE );
        }
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) on_click( w, HIT_CANCEL, -1 );
        else if (wparam == VK_RETURN && has_primary( w )) on_click( w, HIT_PRIMARY, -1 );
        return 0;
    case WM_COMMAND:
        /* Enter in the PIN's box */
        return 0;
    case WM_AUTH_REQUEST:
        if (w->page == PAGE_PAIR) on_auth_request( w, (BLUETOOTH_AUTHENTICATION_METHOD)wparam, (UINT32)lparam );
        return 0;
    case WM_PAIR_DONE:
        if (w->page != PAGE_PAIR) return 0;
        if (w->edit)
        {
            DestroyWindow( w->edit );
            w->edit = NULL;
        }
        KillTimer( hwnd, TIMER_SPIN );
        w->page = wparam == ERROR_SUCCESS ? PAGE_DONE : PAGE_FAILED;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    case WM_CTLCOLOREDIT:
    {
        static HBRUSH brush;
        struct colors c;

        get_colors( &c );
        if (brush) DeleteObject( brush );
        brush = CreateSolidBrush( c.card );
        SetTextColor( (HDC)wparam, c.text );
        SetBkColor( (HDC)wparam, c.card );
        return (LRESULT)brush;
    }
    case WM_DESTROY:
        stop_search( w );
        KillTimer( hwnd, TIMER_SPIN );
        if (w->radio) BluetoothEnableDiscovery( w->radio, FALSE );
        if (w->auth) BluetoothUnregisterAuthentication( w->auth );
        w->auth = NULL;
        w->done = TRUE;
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

/* modal over the owner, as a dialog */
void add_device_wizard( HWND owner, BOOL bluetooth_only )
{
    WNDCLASSW cls = { .lpfnWndProc = wizard_proc, .hInstance = cp_instance, .hCursor = LoadCursorW( NULL, (LPCWSTR)IDC_ARROW ),
                      .lpszClassName = L"ArcticAddDevice" };
    struct wizard *w = calloc( 1, sizeof(*w) );
    RECT area, frame = { 0, 0, WIDTH, HEIGHT };
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU, dark = cp_dark();
    MSG msg;
    int x, y;

    if (!w) return;
    RegisterClassW( &cls );
    w->owner = owner;
    w->hot_device = w->pressed_device = -1;
    w->title = make_font( px( 26 ), FW_SEMIBOLD );
    w->body = make_font( px( 14 ), FW_NORMAL );
    w->bold = make_font( px( 16 ), FW_SEMIBOLD );
    w->caption = make_font( px( 12 ), FW_NORMAL );
    w->digits = make_font( px( 40 ), FW_SEMIBOLD );

    AdjustWindowRectEx( &frame, style, FALSE, WS_EX_DLGMODALFRAME );
    if (owner) GetWindowRect( owner, &area );
    else SystemParametersInfoW( SPI_GETWORKAREA, 0, &area, 0 );
    x = (area.left + area.right - (frame.right - frame.left)) / 2;
    y = (area.top + area.bottom - (frame.bottom - frame.top)) / 2;
    if (!CreateWindowExW( WS_EX_DLGMODALFRAME, cls.lpszClassName, load_string( IDS_WIZ_TITLE ), style, x, max( y, 0 ),
                          frame.right - frame.left, frame.bottom - frame.top, owner, NULL, cp_instance, w ))
    {
        free( w );
        return;
    }
    DwmSetWindowAttribute( w->hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark) );
    if (bluetooth_only) go_search( w );
    if (owner) EnableWindow( owner, FALSE );
    ShowWindow( w->hwnd, SW_SHOW );
    SetForegroundWindow( w->hwnd );

    while (!w->done && GetMessageW( &msg, NULL, 0, 0 ))
    {
        if (w->edit && msg.hwnd == w->edit && msg.message == WM_KEYDOWN &&
            (msg.wParam == VK_RETURN || msg.wParam == VK_ESCAPE))
        {
            SendMessageW( w->hwnd, WM_KEYDOWN, msg.wParam, msg.lParam );
            continue;
        }
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }
    if (owner)
    {
        EnableWindow( owner, TRUE );
        SetForegroundWindow( owner );
    }
    if (w->radio) CloseHandle( w->radio );
    DeleteObject( w->title );
    DeleteObject( w->body );
    DeleteObject( w->bold );
    DeleteObject( w->caption );
    DeleteObject( w->digits );
    /* the pairing thread may still post to the window, which is gone: it checks */
    free( w );
}
