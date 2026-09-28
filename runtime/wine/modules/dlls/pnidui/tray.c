/*
 * Network icon of the notification area: the icon and its flyout
 *
 * One thread inside explorer keeps the icon up to date (every few seconds
 * and on WLAN notifications) and shows the flyout of Windows 7 on a click:
 * the connections in use, then the wireless networks in range; the one
 * picked gets a Connect or Disconnect button, and a secured network asks
 * for its key once, which is kept as a WLAN profile.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <stdlib.h>

#include "winsock2.h"
#include "ws2ipdef.h"
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "winnls.h"
#include "shellapi.h"
#include "iphlpapi.h"
#include "wlanapi.h"
#include "wine/debug.h"

#include "pnidui.h"

WINE_DEFAULT_DEBUG_CHANNEL(pnidui);

#define WM_TRAY_ICON     (WM_APP + 1)
#define WM_WLAN_NOTIFY   (WM_APP + 2)
#define WM_SHOW_LIST     (WM_APP + 3)
#define TIMER_REFRESH    1
#define ICON_ID          1

/* WLAN notifications this icon follows */
#define ACM_SCAN_COMPLETE             7
#define ACM_CONNECTION_COMPLETE       10
#define ACM_CONNECTION_ATTEMPT_FAIL   11
#define ACM_DISCONNECTED              21
#define REASON_KEY_MISMATCH           0x00048014

#define FLYOUT_WIDTH  300
#define ROW_HEIGHT    30
#define ROW_EXPANDED  64
#define HEADER_HEIGHT 26

enum row_kind { ROW_HEADER, ROW_CURRENT, ROW_WIRELESS, ROW_MESSAGE };

struct row
{
    enum row_kind kind;
    WCHAR text[64];          /* SSID, connection name or header */
    WCHAR detail[64];        /* second text, e.g. Internet access */
    DOT11_SSID ssid;
    WCHAR profile[WLAN_MAX_NAME_LENGTH];
    UINT quality;            /* 0-100, wireless */
    BOOL secured, connected, has_profile, connecting;
};

static HANDLE thread;
static HWND tray_hwnd, flyout_hwnd, list_hwnd, button_hwnd, link_hwnd;
static HANDLE wlan;
static GUID wifi_guid;
static BOOL have_wifi;
static UINT taskbar_created;
static HICON current_icon;
static WCHAR current_tip[128];
static struct row *rows;
static UINT row_count;
static HFONT font, bold_font;
static WCHAR connecting_ssid[64];

static WCHAR *load_string( UINT id )
{
    static WCHAR buffers[8][256];
    static int next;
    WCHAR *buf = buffers[next++ % ARRAY_SIZE(buffers)];

    if (!LoadStringW( pnidui_instance, id, buf, ARRAY_SIZE(buffers[0]) )) buf[0] = 0;
    return buf;
}

static void ssid_to_text( const DOT11_SSID *ssid, WCHAR *text, int size )
{
    int len = MultiByteToWideChar( CP_UTF8, 0, (const char *)ssid->ucSSID, min( ssid->uSSIDLength, 32 ),
                                   text, size - 1 );
    text[max( len, 0 )] = 0;
}

/**********************************************************************
 *          What the PC is connected to
 */

struct state
{
    BOOL wired_present, wired_connected;
    BOOL wifi_connected, wifi_connecting;
    WCHAR wifi_ssid[64];
    UINT wifi_quality;
    BOOL networks_available;
};

static void read_wired( struct state *state )
{
    IP_ADAPTER_ADDRESSES *addresses, *a;
    ULONG size = 16384;

    if (!(addresses = malloc( size ))) return;
    if (GetAdaptersAddresses( AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_DNS_SERVER, NULL,
                              addresses, &size ) == ERROR_BUFFER_OVERFLOW)
    {
        free( addresses );
        if (!(addresses = malloc( size ))) return;
        if (GetAdaptersAddresses( AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_DNS_SERVER, NULL,
                                  addresses, &size ))
            size = 0;
    }
    for (a = size ? addresses : NULL; a; a = a->Next)
    {
        if (a->IfType != IF_TYPE_ETHERNET_CSMACD) continue;
        state->wired_present = TRUE;
        if (a->OperStatus == IfOperStatusUp && a->FirstUnicastAddress && a->FirstGatewayAddress)
            state->wired_connected = TRUE;
    }
    free( addresses );
}

static void read_wifi( struct state *state )
{
    WLAN_INTERFACE_INFO_LIST *interfaces = NULL;
    WLAN_AVAILABLE_NETWORK_LIST *networks = NULL;
    WLAN_CONNECTION_ATTRIBUTES *attr = NULL;
    DWORD size;

    have_wifi = FALSE;
    if (!wlan || WlanEnumInterfaces( wlan, NULL, &interfaces ) || !interfaces->dwNumberOfItems)
    {
        if (interfaces) WlanFreeMemory( interfaces );
        return;
    }
    have_wifi = TRUE;
    wifi_guid = interfaces->InterfaceInfo[0].InterfaceGuid;
    state->wifi_connecting = interfaces->InterfaceInfo[0].isState == wlan_interface_state_associating ||
                             interfaces->InterfaceInfo[0].isState == wlan_interface_state_authenticating;
    WlanFreeMemory( interfaces );

    if (!WlanQueryInterface( wlan, &wifi_guid, wlan_intf_opcode_current_connection, NULL, &size,
                             (void **)&attr, NULL ))
    {
        if (attr->isState == wlan_interface_state_connected)
        {
            state->wifi_connected = TRUE;
            ssid_to_text( &attr->wlanAssociationAttributes.dot11Ssid, state->wifi_ssid, ARRAY_SIZE(state->wifi_ssid) );
            state->wifi_quality = attr->wlanAssociationAttributes.wlanSignalQuality;
        }
        WlanFreeMemory( attr );
    }
    if (!WlanGetAvailableNetworkList( wlan, &wifi_guid, 0, NULL, &networks ))
    {
        state->networks_available = networks->dwNumberOfItems > 0;
        WlanFreeMemory( networks );
    }
}

/**********************************************************************
 *          The icon, drawn at the size of the notification area
 */

static void fill( UINT32 *bits, int size, int x0, int y0, int x1, int y1, UINT32 color )
{
    for (int y = max( y0, 0 ); y <= min( y1, size - 1 ); y++)
        for (int x = max( x0, 0 ); x <= min( x1, size - 1 ); x++)
            bits[y * size + x] = color;
}

/* premultiplied ARGB */
static UINT32 argb( BYTE a, BYTE r, BYTE g, BYTE b )
{
    return (a << 24) | ((r * a / 255) << 16) | ((g * a / 255) << 8) | (b * a / 255);
}

enum icon_kind { ICON_WIRELESS, ICON_WIRED, ICON_NONE };

static HICON make_icon( enum icon_kind kind, UINT bars, BOOL cross, BOOL star )
{
    int size = GetSystemMetrics( SM_CXSMICON ), unit = max( size / 16, 1 );
    BITMAPINFO info = { .bmiHeader = { .biSize = sizeof(info.bmiHeader), .biWidth = size, .biHeight = -size,
                                       .biPlanes = 1, .biBitCount = 32, .biCompression = BI_RGB } };
    UINT32 *bits, white = argb( 255, 255, 255, 255 ), dim = argb( 90, 255, 255, 255 );
    ICONINFO icon = { .fIcon = TRUE };
    HICON ret;

    if (!(icon.hbmColor = CreateDIBSection( NULL, &info, DIB_RGB_COLORS, (void **)&bits, NULL, 0 ))) return NULL;
    memset( bits, 0, size * size * 4 );

    if (kind == ICON_WIRELESS)
    {
        /* five bars rising to the right */
        for (UINT i = 0; i < 5; i++)
        {
            int x = (1 + i * 3) * unit, h = (3 + i * 2) * unit;
            fill( bits, size, x, 14 * unit - h + 1, x + 2 * unit - 1, 14 * unit, i < bars ? white : dim );
        }
    }
    else
    {
        /* a monitor on its stand */
        fill( bits, size, 2 * unit, 2 * unit, 13 * unit, 2 * unit, white );
        fill( bits, size, 2 * unit, 10 * unit, 13 * unit, 10 * unit, white );
        fill( bits, size, 2 * unit, 2 * unit, 2 * unit, 10 * unit, white );
        fill( bits, size, 13 * unit, 2 * unit, 13 * unit, 10 * unit, white );
        fill( bits, size, 3 * unit, 3 * unit, 12 * unit, 9 * unit, dim );
        fill( bits, size, 7 * unit, 11 * unit, 8 * unit, 12 * unit, white );
        fill( bits, size, 5 * unit, 13 * unit, 10 * unit, 13 * unit, white );
    }

    if (cross || star)
    {
        int x0 = size - 7 * unit, y0 = size - 7 * unit;
        UINT32 color = cross ? argb( 255, 214, 48, 49 ) : argb( 255, 242, 190, 30 );

        /* a round badge in the corner */
        for (int y = 0; y < 7 * unit; y++)
            for (int x = 0; x < 7 * unit; x++)
            {
                int dx = 2 * x - (7 * unit - 1), dy = 2 * y - (7 * unit - 1);
                if (dx * dx + dy * dy <= 49 * unit * unit) bits[(y0 + y) * size + x0 + x] = color;
            }
        if (cross)
            for (int i = 2 * unit; i < 5 * unit; i++)
            {
                bits[(y0 + i) * size + x0 + i] = white;
                bits[(y0 + i) * size + x0 + 7 * unit - 1 - i] = white;
            }
    }

    icon.hbmMask = CreateBitmap( size, size, 1, 1, NULL );
    ret = CreateIconIndirect( &icon );
    DeleteObject( icon.hbmColor );
    DeleteObject( icon.hbmMask );
    return ret;
}

static UINT quality_bars( UINT quality )
{
    return quality >= 80 ? 5 : quality >= 60 ? 4 : quality >= 40 ? 3 : quality >= 20 ? 2 : quality ? 1 : 0;
}

static void update_icon(void)
{
    NOTIFYICONDATAW data = { .cbSize = sizeof(data), .hWnd = tray_hwnd, .uID = ICON_ID,
                             .uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE, .uCallbackMessage = WM_TRAY_ICON };
    struct state state = { 0 };
    static int last_key = -1;
    int key;

    read_wired( &state );
    read_wifi( &state );

    if (state.wifi_connected)
    {
        key = 100 + quality_bars( state.wifi_quality );
        swprintf( data.szTip, ARRAY_SIZE(data.szTip), L"%s\n%s", state.wifi_ssid, load_string( IDS_INTERNET_ACCESS ) );
    }
    else if (state.wired_connected)
    {
        key = 200;
        swprintf( data.szTip, ARRAY_SIZE(data.szTip), L"%s\n%s", load_string( IDS_NETWORK ),
                  load_string( IDS_INTERNET_ACCESS ) );
    }
    else if (state.networks_available)
    {
        key = 300;
        lstrcpynW( data.szTip, load_string( IDS_CONNECTIONS_AVAILABLE ), ARRAY_SIZE(data.szTip) );
    }
    else
    {
        key = 400 + have_wifi;
        lstrcpynW( data.szTip, load_string( IDS_NOT_CONNECTED ), ARRAY_SIZE(data.szTip) );
    }

    if (key != last_key || !current_icon)
    {
        HICON icon = key >= 100 && key < 200 ? make_icon( ICON_WIRELESS, key - 100, FALSE, FALSE ) :
                     key == 200 ? make_icon( ICON_WIRED, 0, FALSE, FALSE ) :
                     key == 300 ? make_icon( ICON_WIRELESS, 0, FALSE, TRUE ) :
                     make_icon( have_wifi ? ICON_WIRELESS : ICON_WIRED, 0, TRUE, FALSE );
        if (current_icon) DestroyIcon( current_icon );
        current_icon = icon;
        last_key = key;
    }
    else if (!wcscmp( data.szTip, current_tip )) return;

    data.hIcon = current_icon;
    lstrcpynW( current_tip, data.szTip, ARRAY_SIZE(current_tip) );
    if (!Shell_NotifyIconW( NIM_MODIFY, &data )) Shell_NotifyIconW( NIM_ADD, &data );
}

/**********************************************************************
 *          The flyout
 */

static struct row *add_row( enum row_kind kind, const WCHAR *text )
{
    struct row *grown = realloc( rows, (row_count + 1) * sizeof(*rows) );

    if (!grown) return NULL;
    rows = grown;
    memset( &rows[row_count], 0, sizeof(*rows) );
    rows[row_count].kind = kind;
    lstrcpynW( rows[row_count].text, text, ARRAY_SIZE(rows[0].text) );
    return &rows[row_count++];
}

static void fill_rows(void)
{
    WLAN_AVAILABLE_NETWORK_LIST *networks = NULL;
    struct state state = { 0 };
    struct row *row;

    free( rows );
    rows = NULL;
    row_count = 0;
    read_wired( &state );
    read_wifi( &state );

    /* the connections in use */
    if (state.wired_connected && (row = add_row( ROW_CURRENT, load_string( IDS_NETWORK ) )))
        lstrcpyW( row->detail, load_string( IDS_INTERNET_ACCESS ) );
    if (state.wifi_connected && (row = add_row( ROW_CURRENT, state.wifi_ssid )))
        lstrcpyW( row->detail, load_string( IDS_INTERNET_ACCESS ) );
    if (state.wired_present && !state.wired_connected)
    {
        add_row( ROW_HEADER, load_string( IDS_WIRED_GROUP ) );
        add_row( ROW_MESSAGE, load_string( IDS_CABLE_UNPLUGGED ) );
    }

    /* the wireless networks in range, each once, strongest first */
    if (have_wifi && !WlanGetAvailableNetworkList( wlan, &wifi_guid, 0, NULL, &networks ))
    {
        add_row( ROW_HEADER, load_string( IDS_WIRELESS_GROUP ) );
        for (DWORD i = 0; i < networks->dwNumberOfItems; i++)
        {
            const WLAN_AVAILABLE_NETWORK *net = &networks->Network[i];
            WCHAR ssid[64];

            if (!net->dot11Ssid.uSSIDLength) continue;
            ssid_to_text( &net->dot11Ssid, ssid, ARRAY_SIZE(ssid) );
            if (!(row = add_row( ROW_WIRELESS, ssid ))) break;
            row->ssid = net->dot11Ssid;
            row->quality = net->wlanSignalQuality;
            row->secured = net->bSecurityEnabled;
            row->connected = !!(net->dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED);
            row->has_profile = !!(net->dwFlags & WLAN_AVAILABLE_NETWORK_HAS_PROFILE);
            row->connecting = !wcscmp( ssid, connecting_ssid );
            lstrcpynW( row->profile, net->strProfileName, ARRAY_SIZE(row->profile) );
            if (row->connected) lstrcpyW( row->detail, load_string( IDS_CONNECTED ) );
            else if (row->connecting) lstrcpyW( row->detail, load_string( IDS_CONNECTING ) );
        }
        if (!networks->dwNumberOfItems) add_row( ROW_MESSAGE, load_string( IDS_NO_NETWORKS ) );
        WlanFreeMemory( networks );
    }
    if (!row_count) add_row( ROW_MESSAGE, load_string( IDS_NO_NETWORKS ) );
}

static int selected_row(void)
{
    LRESULT sel = SendMessageW( list_hwnd, LB_GETCURSEL, 0, 0 );
    return sel == LB_ERR ? -1 : (int)sel;
}

static int row_height( UINT i, BOOL selected )
{
    if (rows[i].kind == ROW_HEADER) return HEADER_HEIGHT;
    if (rows[i].kind == ROW_WIRELESS && selected) return ROW_EXPANDED;
    return ROW_HEIGHT + (rows[i].kind == ROW_CURRENT ? 6 : 0);
}

/* the Connect / Disconnect button sits in the row picked */
static void place_button(void)
{
    int sel = selected_row();
    RECT rect;

    if (sel < 0 || rows[sel].kind != ROW_WIRELESS ||
        SendMessageW( list_hwnd, LB_GETITEMRECT, sel, (LPARAM)&rect ) == LB_ERR)
    {
        ShowWindow( button_hwnd, SW_HIDE );
        return;
    }
    MapWindowPoints( list_hwnd, flyout_hwnd, (POINT *)&rect, 2 );
    SetWindowTextW( button_hwnd, load_string( rows[sel].connected ? IDS_DISCONNECT : IDS_CONNECT ) );
    EnableWindow( button_hwnd, !rows[sel].connecting );
    SetWindowPos( button_hwnd, HWND_TOP, rect.right - 110, rect.bottom - 30, 100, 24, SWP_SHOWWINDOW );
}

/* the rows again, row sel picked (-1: none), and the flyout sized to them */
static void layout_flyout( int sel, BOOL reposition )
{
    int height = 0, list_height, link_height = link_hwnd ? 30 : 0;
    RECT work, rect;
    POINT pt;

    SendMessageW( list_hwnd, WM_SETREDRAW, FALSE, 0 );
    SendMessageW( list_hwnd, LB_RESETCONTENT, 0, 0 );
    for (UINT i = 0; i < row_count; i++)
    {
        SendMessageW( list_hwnd, LB_ADDSTRING, 0, i );
        SendMessageW( list_hwnd, LB_SETITEMHEIGHT, i, row_height( i, (int)i == sel ) );
        height += row_height( i, (int)i == sel );
    }
    if (sel >= 0 && sel < (int)row_count) SendMessageW( list_hwnd, LB_SETCURSEL, sel, 0 );
    SendMessageW( list_hwnd, WM_SETREDRAW, TRUE, 0 );

    list_height = min( height + 4, 420 );
    SystemParametersInfoW( SPI_GETWORKAREA, 0, &work, 0 );
    GetWindowRect( flyout_hwnd, &rect );
    if (reposition)
    {
        GetCursorPos( &pt );
        rect.left = min( max( pt.x - FLYOUT_WIDTH / 2, work.left + 8 ), work.right - FLYOUT_WIDTH - 8 );
        rect.bottom = work.bottom - 8;
    }
    rect.top = rect.bottom - (list_height + link_height + 2);
    SetWindowPos( flyout_hwnd, HWND_TOPMOST, rect.left, rect.top, FLYOUT_WIDTH, rect.bottom - rect.top, 0 );
    MoveWindow( list_hwnd, 0, 0, FLYOUT_WIDTH - 2, list_height, TRUE );
    if (link_hwnd) MoveWindow( link_hwnd, 12, list_height + 7, FLYOUT_WIDTH - 24, 18, TRUE );
    InvalidateRect( list_hwnd, NULL, TRUE );
    place_button();
}

static void refresh_flyout( BOOL reposition )
{
    WCHAR keep[64] = L"";
    int sel = selected_row();

    /* the network picked stays picked when the list changes around it */
    if (sel >= 0 && sel < (int)row_count) lstrcpyW( keep, rows[sel].text );
    fill_rows();
    sel = -1;
    for (UINT i = 0; keep[0] && i < row_count && sel < 0; i++)
        if (rows[i].kind == ROW_WIRELESS && !wcscmp( rows[i].text, keep )) sel = i;
    layout_flyout( sel, reposition );
}

static void draw_bars( HDC hdc, int right, int bottom, UINT quality, BOOL secured )
{
    UINT bars = quality_bars( quality );
    HBRUSH on = CreateSolidBrush( RGB( 64, 64, 64 ) ), off = CreateSolidBrush( RGB( 200, 200, 200 ) );

    for (UINT i = 0; i < 5; i++)
    {
        RECT bar = { right - 20 + i * 4, bottom - 4 - (int)(3 + i * 3), right - 17 + i * 4, bottom - 4 };
        FillRect( hdc, &bar, i < bars ? on : off );
    }
    if (secured)
    {
        /* a padlock under the bars' left end */
        HBRUSH gold = CreateSolidBrush( RGB( 176, 136, 40 ) );
        RECT body = { right - 27, bottom - 9, right - 21, bottom - 4 }, arc = { right - 26, bottom - 13, right - 22, bottom - 9 };
        HPEN pen = CreatePen( PS_SOLID, 1, RGB( 176, 136, 40 ) ), old_pen = SelectObject( hdc, pen );
        HBRUSH old_brush = SelectObject( hdc, GetStockObject( NULL_BRUSH ) );

        FillRect( hdc, &body, gold );
        Arc( hdc, arc.left, arc.top, arc.right, arc.bottom + 4, arc.right, arc.top + 3, arc.left, arc.top + 3 );
        SelectObject( hdc, old_pen );
        SelectObject( hdc, old_brush );
        DeleteObject( pen );
        DeleteObject( gold );
    }
    DeleteObject( on );
    DeleteObject( off );
}

static void draw_row( const DRAWITEMSTRUCT *draw )
{
    const struct row *row;
    RECT rect = draw->rcItem, text;
    HDC hdc = draw->hDC;
    BOOL selected = (draw->itemState & ODS_SELECTED) != 0;
    HFONT old;

    if (draw->itemID >= row_count) return;
    row = &rows[draw->itemID];
    FillRect( hdc, &rect, GetStockObject( WHITE_BRUSH ) );
    SetBkMode( hdc, TRANSPARENT );

    switch (row->kind)
    {
    case ROW_HEADER:
    {
        HPEN pen = CreatePen( PS_SOLID, 1, RGB( 220, 225, 232 ) ), old_pen = SelectObject( hdc, pen );

        old = SelectObject( hdc, bold_font );
        SetTextColor( hdc, RGB( 30, 57, 91 ) );
        text = rect;
        text.left += 10;
        DrawTextW( hdc, row->text, -1, &text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX );
        MoveToEx( hdc, rect.left + 10, rect.bottom - 2, NULL );
        LineTo( hdc, rect.right - 10, rect.bottom - 2 );
        SelectObject( hdc, old_pen );
        DeleteObject( pen );
        SelectObject( hdc, old );
        break;
    }
    case ROW_CURRENT:
        old = SelectObject( hdc, bold_font );
        SetTextColor( hdc, RGB( 0, 0, 0 ) );
        text = rect;
        text.left += 10;
        text.bottom = rect.top + ROW_HEIGHT / 2 + 4;
        DrawTextW( hdc, row->text, -1, &text, DT_SINGLELINE | DT_BOTTOM | DT_END_ELLIPSIS | DT_NOPREFIX );
        SelectObject( hdc, font );
        SetTextColor( hdc, RGB( 100, 100, 100 ) );
        text.top = text.bottom;
        text.bottom = rect.bottom;
        DrawTextW( hdc, row->detail, -1, &text, DT_SINGLELINE | DT_TOP | DT_END_ELLIPSIS | DT_NOPREFIX );
        SelectObject( hdc, old );
        break;
    case ROW_MESSAGE:
        old = SelectObject( hdc, font );
        SetTextColor( hdc, RGB( 100, 100, 100 ) );
        text = rect;
        text.left += 10;
        DrawTextW( hdc, row->text, -1, &text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX );
        SelectObject( hdc, old );
        break;
    case ROW_WIRELESS:
    {
        RECT inner = { rect.left + 4, rect.top + 1, rect.right - 4, rect.bottom - 1 };

        if (selected)
        {
            HBRUSH back = CreateSolidBrush( RGB( 229, 243, 251 ) ), frame = CreateSolidBrush( RGB( 112, 192, 231 ) );
            FillRect( hdc, &inner, back );
            FrameRect( hdc, &inner, frame );
            DeleteObject( back );
            DeleteObject( frame );
        }
        old = SelectObject( hdc, font );
        SetTextColor( hdc, RGB( 0, 0, 0 ) );
        text = rect;
        text.left += 12;
        text.right -= 110;
        text.bottom = rect.top + ROW_HEIGHT;
        DrawTextW( hdc, row->text, -1, &text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX );
        if (row->detail[0])
        {
            text.left = rect.right - 110;
            text.right = rect.right - 34;
            SetTextColor( hdc, RGB( 100, 100, 100 ) );
            DrawTextW( hdc, row->detail, -1, &text, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_END_ELLIPSIS | DT_NOPREFIX );
        }
        draw_bars( hdc, rect.right - 8, rect.top + ROW_HEIGHT - 4, row->quality, row->secured );
        SelectObject( hdc, old );
        break;
    }
    }
}

/**********************************************************************
 *          Connecting
 */

static void xml_escape( const WCHAR *in, WCHAR *out, int size )
{
    int len = 0;

    for (; *in && len < size - 7; in++)
    {
        const WCHAR *rep = *in == '&' ? L"&amp;" : *in == '<' ? L"&lt;" : *in == '>' ? L"&gt;" :
                           *in == '"' ? L"&quot;" : *in == '\'' ? L"&apos;" : NULL;
        if (rep) len += swprintf( out + len, size - len, L"%s", rep );
        else out[len++] = *in;
    }
    out[len] = 0;
}

static INT_PTR CALLBACK key_dialog_proc( HWND dlg, UINT msg, WPARAM wp, LPARAM lp )
{
    static WCHAR *key;

    switch (msg)
    {
    case WM_INITDIALOG:
        key = (WCHAR *)lp;
        CheckDlgButton( dlg, IDC_HIDE_KEY, BST_CHECKED );
        SendDlgItemMessageW( dlg, IDC_KEY, EM_LIMITTEXT, 63, 0 );
        SetForegroundWindow( dlg );
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD( wp ))
        {
        case IDC_HIDE_KEY:
            SendDlgItemMessageW( dlg, IDC_KEY, EM_SETPASSWORDCHAR,
                                 IsDlgButtonChecked( dlg, IDC_HIDE_KEY ) ? 0x25cf : 0, 0 );
            InvalidateRect( GetDlgItem( dlg, IDC_KEY ), NULL, TRUE );
            return TRUE;
        case IDOK:
            GetDlgItemTextW( dlg, IDC_KEY, key, 64 );
            /* WPA takes 8 to 63 characters */
            if (wcslen( key ) < 8) return TRUE;
            EndDialog( dlg, IDOK );
            return TRUE;
        case IDCANCEL:
            EndDialog( dlg, IDCANCEL );
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void connect_row( UINT index )
{
    struct row *row = &rows[index];
    WLAN_CONNECTION_PARAMETERS params = { .dot11BssType = dot11_BSS_type_infrastructure };
    WCHAR key[64] = L"", name[128], material[160], xml[2048];
    DWORD reason, ret;

    TRACE( "%s connected %d profile %d\n", debugstr_w( row->text ), row->connected, row->has_profile );
    if (row->connected)
    {
        WlanDisconnect( wlan, &wifi_guid, NULL );
        refresh_flyout( FALSE );
        return;
    }

    if (row->has_profile && row->profile[0])
    {
        params.wlanConnectionMode = wlan_connection_mode_profile;
        params.strProfile = row->profile;
    }
    else if (row->has_profile)
    {
        /* iwd knows it, the registry does not */
        params.wlanConnectionMode = wlan_connection_mode_discovery_secure;
        params.pDot11Ssid = &row->ssid;
    }
    else
    {
        if (row->secured &&
            DialogBoxParamW( pnidui_instance, MAKEINTRESOURCEW(IDD_NETWORK_KEY), flyout_hwnd, key_dialog_proc,
                             (LPARAM)key ) != IDOK)
            return;
        xml_escape( row->text, name, ARRAY_SIZE(name) );
        xml_escape( key, material, ARRAY_SIZE(material) );
        swprintf( xml, ARRAY_SIZE(xml),
                  L"<?xml version=\"1.0\"?>\n"
                  L"<WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\">\n"
                  L"<name>%s</name>\n<SSIDConfig><SSID><name>%s</name></SSID></SSIDConfig>\n"
                  L"<connectionType>ESS</connectionType>\n<connectionMode>auto</connectionMode>\n"
                  L"<MSM><security><authEncryption><authentication>%s</authentication>"
                  L"<encryption>%s</encryption><useOneX>false</useOneX></authEncryption>%s%s%s</security></MSM>\n"
                  L"</WLANProfile>\n",
                  name, name, row->secured ? L"WPA2PSK" : L"open", row->secured ? L"AES" : L"none",
                  row->secured ? L"<sharedKey><keyType>passPhrase</keyType><protected>false</protected><keyMaterial>" : L"",
                  row->secured ? material : L"", row->secured ? L"</keyMaterial></sharedKey>" : L"" );
        SecureZeroMemory( key, sizeof(key) );
        SecureZeroMemory( material, sizeof(material) );
        ret = WlanSetProfile( wlan, &wifi_guid, 0, xml, NULL, TRUE, NULL, &reason );
        SecureZeroMemory( xml, sizeof(xml) );
        if (ret) return;
        lstrcpynW( row->profile, row->text, ARRAY_SIZE(row->profile) );
        params.wlanConnectionMode = wlan_connection_mode_profile;
        params.strProfile = row->profile;
    }

    lstrcpynW( connecting_ssid, row->text, ARRAY_SIZE(connecting_ssid) );
    if (WlanConnect( wlan, &wifi_guid, &params, NULL )) connecting_ssid[0] = 0;
    refresh_flyout( FALSE );
}

static void connection_failed( DWORD reason )
{
    WCHAR text[512], ssid[64];

    lstrcpynW( ssid, connecting_ssid, ARRAY_SIZE(ssid) );
    connecting_ssid[0] = 0;
    swprintf( text, ARRAY_SIZE(text), load_string( IDS_CONNECT_FAILED ), ssid );
    if (reason == REASON_KEY_MISMATCH)
    {
        /* the key is asked again next time */
        WlanDeleteProfile( wlan, &wifi_guid, ssid, NULL );
        lstrcatW( text, L"\n" );
        lstrcatW( text, load_string( IDS_KEY_MISMATCH ) );
    }
    MessageBoxW( flyout_hwnd && IsWindowVisible( flyout_hwnd ) ? flyout_hwnd : NULL, text,
                 load_string( IDS_WIRELESS_GROUP ), MB_OK | MB_ICONWARNING );
}

static void WINAPI wlan_callback( WLAN_NOTIFICATION_DATA *data, void *context )
{
    DWORD reason = 0;

    if (data->NotificationCode == ACM_CONNECTION_ATTEMPT_FAIL && data->pData &&
        data->dwDataSize >= sizeof(WLAN_CONNECTION_MODE) + WLAN_MAX_NAME_LENGTH * sizeof(WCHAR) +
                            sizeof(DOT11_SSID) + sizeof(DOT11_BSS_TYPE) + sizeof(BOOL) + sizeof(DWORD))
    {
        const BYTE *p = data->pData;
        reason = *(const DWORD *)(p + sizeof(WLAN_CONNECTION_MODE) + WLAN_MAX_NAME_LENGTH * sizeof(WCHAR) +
                                  sizeof(DOT11_SSID) + sizeof(DOT11_BSS_TYPE) + sizeof(BOOL));
    }
    PostMessageW( tray_hwnd, WM_WLAN_NOTIFY, data->NotificationCode, reason );
}

/**********************************************************************
 *          Windows
 */

static BOOL connections_folder_exists(void)
{
    WCHAR path[MAX_PATH];

    GetSystemDirectoryW( path, ARRAY_SIZE(path) );
    lstrcatW( path, L"\\netshell.dll" );
    return GetFileAttributesW( path ) != INVALID_FILE_ATTRIBUTES;
}

/* My Computer \ Control Panel \ Network Connections, as ncpa.cpl opens it */
static void open_connections(void)
{
    ShowWindow( flyout_hwnd, SW_HIDE );
    ShellExecuteW( NULL, NULL, L"explorer.exe",
                   L"/n,::{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\::{21EC2020-3AEA-1069-A2DD-08002B30309D}"
                   L"\\::{7007ACC7-3202-11D1-AAD2-00805FC1270E}", NULL, SW_SHOWNORMAL );
}

static LRESULT WINAPI flyout_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    switch (msg)
    {
    case WM_MEASUREITEM:
    {
        MEASUREITEMSTRUCT *measure = (MEASUREITEMSTRUCT *)lp;
        if (measure->itemID < row_count) measure->itemHeight = row_height( measure->itemID, FALSE );
        return TRUE;
    }
    case WM_DRAWITEM:
        draw_row( (const DRAWITEMSTRUCT *)lp );
        return TRUE;
    case WM_COMMAND:
        if ((HWND)lp == list_hwnd && HIWORD( wp ) == LBN_SELCHANGE)
        {
            int sel = selected_row();
            /* only networks can be picked */
            layout_flyout( sel >= 0 && rows[sel].kind == ROW_WIRELESS ? sel : -1, FALSE );
        }
        else if ((HWND)lp == list_hwnd && HIWORD( wp ) == LBN_DBLCLK)
        {
            int sel = selected_row();
            if (sel >= 0 && rows[sel].kind == ROW_WIRELESS && !rows[sel].connected) connect_row( sel );
        }
        else if ((HWND)lp == button_hwnd && HIWORD( wp ) == BN_CLICKED)
        {
            int sel = selected_row();
            if (sel >= 0 && rows[sel].kind == ROW_WIRELESS) connect_row( sel );
        }
        else if ((HWND)lp == link_hwnd && HIWORD( wp ) == STN_CLICKED)
            open_connections();
        return 0;
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == link_hwnd)
        {
            SetTextColor( (HDC)wp, RGB( 0, 102, 204 ) );
            SetBkColor( (HDC)wp, RGB( 241, 245, 251 ) );
            SetDCBrushColor( (HDC)wp, RGB( 241, 245, 251 ) );
            return (LRESULT)GetStockObject( DC_BRUSH );
        }
        break;
    case WM_ERASEBKGND:
    {
        RECT rect;
        GetClientRect( hwnd, &rect );
        SetDCBrushColor( (HDC)wp, RGB( 241, 245, 251 ) );
        FillRect( (HDC)wp, &rect, GetStockObject( DC_BRUSH ) );
        return TRUE;
    }
    case WM_ACTIVATE:
        /* a click anywhere else closes it, as in Windows; not the key dialog it opened */
        if (LOWORD( wp ) == WA_INACTIVE && (HWND)lp != NULL && GetWindow( (HWND)lp, GW_OWNER ) == hwnd) break;
        if (LOWORD( wp ) == WA_INACTIVE) ShowWindow( hwnd, SW_HIDE );
        break;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) ShowWindow( hwnd, SW_HIDE );
        break;
    }
    return DefWindowProcW( hwnd, msg, wp, lp );
}

static void create_flyout(void)
{
    WNDCLASSW cls = { .lpfnWndProc = flyout_proc, .hInstance = pnidui_instance,
                      .hCursor = LoadCursorW( 0, (LPCWSTR)IDC_ARROW ), .lpszClassName = L"PniduiFlyout" };
    NONCLIENTMETRICSW metrics = { .cbSize = sizeof(metrics) };
    LOGFONTW bold;

    SystemParametersInfoW( SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0 );
    font = CreateFontIndirectW( &metrics.lfMessageFont );
    bold = metrics.lfMessageFont;
    bold.lfWeight = FW_BOLD;
    bold_font = CreateFontIndirectW( &bold );

    RegisterClassW( &cls );
    flyout_hwnd = CreateWindowExW( WS_EX_TOOLWINDOW | WS_EX_TOPMOST, cls.lpszClassName, NULL, WS_POPUP | WS_BORDER,
                                   0, 0, FLYOUT_WIDTH, 200, NULL, NULL, pnidui_instance, NULL );
    list_hwnd = CreateWindowExW( 0, L"LISTBOX", NULL, WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_OWNERDRAWVARIABLE |
                                 LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, 0, 0, FLYOUT_WIDTH - 2, 200, flyout_hwnd,
                                 NULL, pnidui_instance, NULL );
    button_hwnd = CreateWindowExW( 0, L"BUTTON", NULL, WS_CHILD | BS_PUSHBUTTON, 0, 0, 100, 24, flyout_hwnd,
                                   NULL, pnidui_instance, NULL );
    SendMessageW( button_hwnd, WM_SETFONT, (WPARAM)font, FALSE );
    if (connections_folder_exists())
    {
        link_hwnd = CreateWindowExW( 0, L"STATIC", load_string( IDS_OPEN_CONNECTIONS ), WS_CHILD | WS_VISIBLE |
                                     SS_NOTIFY | SS_LEFT, 0, 0, 100, 18, flyout_hwnd, NULL, pnidui_instance, NULL );
        SendMessageW( link_hwnd, WM_SETFONT, (WPARAM)font, FALSE );
    }
}

static void toggle_flyout(void)
{
    if (!flyout_hwnd) create_flyout();
    if (IsWindowVisible( flyout_hwnd ))
    {
        ShowWindow( flyout_hwnd, SW_HIDE );
        return;
    }
    if (have_wifi) WlanScan( wlan, &wifi_guid, NULL, NULL, NULL );
    SendMessageW( list_hwnd, LB_SETCURSEL, -1, 0 );
    refresh_flyout( TRUE );
    ShowWindow( flyout_hwnd, SW_SHOW );
    SetForegroundWindow( flyout_hwnd );
    SetFocus( list_hwnd );
}

static LRESULT WINAPI tray_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    if (msg == taskbar_created && taskbar_created)
    {
        /* explorer restarted: the icon goes back */
        current_tip[0] = 0;
        if (current_icon) DestroyIcon( current_icon );
        current_icon = NULL;
        update_icon();
        return 0;
    }
    switch (msg)
    {
    case WM_TIMER:
        update_icon();
        if (flyout_hwnd && IsWindowVisible( flyout_hwnd ) && !connecting_ssid[0]) refresh_flyout( FALSE );
        return 0;
    case WM_TRAY_ICON:
        if (lp == WM_LBUTTONUP) toggle_flyout();
        else if (lp == WM_RBUTTONUP && connections_folder_exists())
        {
            HMENU menu = CreatePopupMenu();
            POINT pt;

            AppendMenuW( menu, MF_STRING, 1, load_string( IDS_OPEN_CONNECTIONS ) );
            GetCursorPos( &pt );
            SetForegroundWindow( hwnd );
            if (TrackPopupMenu( menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL ) == 1)
                open_connections();
            DestroyMenu( menu );
        }
        return 0;
    case WM_SHOW_LIST:
        if (!flyout_hwnd || !IsWindowVisible( flyout_hwnd )) toggle_flyout();
        return 0;
    case WM_WLAN_NOTIFY:
        if (wp == ACM_CONNECTION_COMPLETE) connecting_ssid[0] = 0;
        else if (wp == ACM_CONNECTION_ATTEMPT_FAIL) connection_failed( lp );
        update_icon();
        if (flyout_hwnd && IsWindowVisible( flyout_hwnd )) refresh_flyout( FALSE );
        return 0;
    case WM_CLOSE:
        DestroyWindow( hwnd );
        return 0;
    case WM_DESTROY:
    {
        NOTIFYICONDATAW data = { .cbSize = sizeof(data), .hWnd = hwnd, .uID = ICON_ID };
        Shell_NotifyIconW( NIM_DELETE, &data );
        PostQuitMessage( 0 );
        return 0;
    }
    }
    return DefWindowProcW( hwnd, msg, wp, lp );
}

static DWORD WINAPI tray_thread( void *arg )
{
    WNDCLASSW cls = { .lpfnWndProc = tray_proc, .hInstance = pnidui_instance, .lpszClassName = L"PniduiTray" };
    DWORD version;
    MSG msg;

    taskbar_created = RegisterWindowMessageW( L"TaskbarCreated" );
    RegisterClassW( &cls );
    tray_hwnd = CreateWindowExW( 0, cls.lpszClassName, NULL, WS_POPUP, 0, 0, 0, 0, NULL, NULL, pnidui_instance, NULL );
    if (WlanOpenHandle( 2, NULL, &version, &wlan )) wlan = NULL;
    if (wlan) WlanRegisterNotification( wlan, 0x0000ffff, TRUE, wlan_callback, NULL, NULL, NULL );

    update_icon();
    SetTimer( tray_hwnd, TIMER_REFRESH, 3000, NULL );
    while (GetMessageW( &msg, NULL, 0, 0 ))
    {
        if (flyout_hwnd && msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE && IsChild( flyout_hwnd, msg.hwnd ))
            ShowWindow( flyout_hwnd, SW_HIDE );
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }

    if (wlan)
    {
        WlanRegisterNotification( wlan, 0, TRUE, NULL, NULL, NULL, NULL );
        WlanCloseHandle( wlan, NULL );
        wlan = NULL;
    }
    if (flyout_hwnd) DestroyWindow( flyout_hwnd );
    flyout_hwnd = list_hwnd = button_hwnd = link_hwnd = NULL;
    tray_hwnd = NULL;
    return 0;
}

void tray_start(void)
{
    if (thread) return;
    thread = CreateThread( NULL, 0, tray_thread, NULL, 0, NULL );
}

void tray_stop(void)
{
    if (!thread) return;
    if (tray_hwnd) PostMessageW( tray_hwnd, WM_CLOSE, 0, 0 );
    WaitForSingleObject( thread, 5000 );
    CloseHandle( thread );
    thread = NULL;
}

/***********************************************************************
 *          ShowNetworkList (PNIDUI.@)
 *
 * rundll32 pnidui.dll,ShowNetworkList: the icon's list of networks, opened
 * from elsewhere (the Network Connections folder).
 */
void WINAPI ShowNetworkList( HWND hwnd, HINSTANCE instance, char *cmdline, int show )
{
    HWND tray = FindWindowW( L"PniduiTray", NULL );

    if (!tray) return;
    AllowSetForegroundWindow( ASFW_ANY );
    PostMessageW( tray, WM_SHOW_LIST, 0, 0 );
}
