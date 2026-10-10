/*
 * Network icon of the notification area: the icon and its flyout
 *
 * One thread inside explorer keeps the icon up to date (every few seconds
 * and on WLAN notifications) and shows the flyout of Windows 10 on a click
 * (flyout.c): the connections, the wireless networks in range, and the
 * quick actions.
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

/* sizes are given at 96 DPI and drawn at the DPI of the system: the shell's */
int px( int n )
{
    return MulDiv( n, GetDpiForSystem(), 96 );
}

#define FLYOUT_WIDTH  px(300)
#define ROW_HEIGHT    px(30)
#define ROW_EXPANDED  px(64)
#define HEADER_HEIGHT px(26)



static HANDLE thread;
static HWND tray_hwnd;
HANDLE wlan;
GUID wifi_guid;
BOOL have_wifi;
static UINT taskbar_created;
static HICON current_icon;
static WCHAR current_tip[128];
WCHAR connecting_ssid[64];

WCHAR *load_string( UINT id )
{
    static WCHAR buffers[8][256];
    static int next;
    WCHAR *buf = buffers[next++ % ARRAY_SIZE(buffers)];

    if (!LoadStringW( pnidui_instance, id, buf, ARRAY_SIZE(buffers[0]) )) buf[0] = 0;
    return buf;
}

void ssid_to_text( const DOT11_SSID *ssid, WCHAR *text, int size )
{
    int len = MultiByteToWideChar( CP_UTF8, 0, (const char *)ssid->ucSSID, min( ssid->uSSIDLength, 32 ),
                                   text, size - 1 );
    text[max( len, 0 )] = 0;
}

/**********************************************************************
 *          What the PC is connected to
 */


void read_wired( struct state *state )
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
        else if (a->OperStatus == IfOperStatusUp)
            state->wired_limited = TRUE;
    }
    free( addresses );
}

void read_wifi( struct state *state )
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

/* The icon is painted as Vista and Windows 7 paint theirs: glossy, with
 * gradients and dark outlines. Shapes are laid out on a 16 unit grid and
 * sampled 4 x 4 per pixel, so they stay smooth at every size. */

#define SUPERSAMPLE 4

struct canvas
{
    int    size;      /* pixels */
    float  scale;     /* samples per unit */
    float *px;        /* premultiplied RGBA, (size * SUPERSAMPLE)^2 samples */
};

struct rgba { float r, g, b, a; };

static struct rgba color( UINT32 rgb, float a )
{
    struct rgba c = { ((rgb >> 16) & 0xff) / 255.0f, ((rgb >> 8) & 0xff) / 255.0f, (rgb & 0xff) / 255.0f, a };
    return c;
}

static void blend_sample( struct canvas *c, int i, struct rgba s )
{
    float *d = c->px + i * 4;

    d[0] = s.r * s.a + d[0] * (1 - s.a);
    d[1] = s.g * s.a + d[1] * (1 - s.a);
    d[2] = s.b * s.a + d[2] * (1 - s.a);
    d[3] = s.a + d[3] * (1 - s.a);
}

/* a rounded rectangle, from one colour at its top to another at its bottom */
static void paint_rect( struct canvas *c, float x0, float y0, float x1, float y1, float radius,
                        struct rgba top, struct rgba bottom )
{
    int n = c->size * SUPERSAMPLE;

    for (int j = 0; j < n; j++)
    {
        float y = (j + 0.5f) / c->scale, t = (y - y0) / (y1 - y0);
        struct rgba s;

        if (y < y0 || y > y1) continue;
        s.r = top.r + (bottom.r - top.r) * t;
        s.g = top.g + (bottom.g - top.g) * t;
        s.b = top.b + (bottom.b - top.b) * t;
        s.a = top.a + (bottom.a - top.a) * t;
        for (int i = 0; i < n; i++)
        {
            float x = (i + 0.5f) / c->scale, dx, dy;

            if (x < x0 || x > x1) continue;
            dx = max( max( x0 + radius - x, x - (x1 - radius) ), 0.0f );
            dy = max( max( y0 + radius - y, y - (y1 - radius) ), 0.0f );
            if (dx * dx + dy * dy > radius * radius) continue;
            blend_sample( c, j * n + i, s );
        }
    }
}

static void paint_circle( struct canvas *c, float cx, float cy, float radius, struct rgba top, struct rgba bottom )
{
    int n = c->size * SUPERSAMPLE;

    for (int j = 0; j < n; j++)
    {
        float y = (j + 0.5f) / c->scale, t = (y - (cy - radius)) / (2 * radius);
        struct rgba s = { top.r + (bottom.r - top.r) * t, top.g + (bottom.g - top.g) * t,
                          top.b + (bottom.b - top.b) * t, top.a + (bottom.a - top.a) * t };

        for (int i = 0; i < n; i++)
        {
            float x = (i + 0.5f) / c->scale;
            if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= radius * radius) blend_sample( c, j * n + i, s );
        }
    }
}

static void paint_stroke( struct canvas *c, float x0, float y0, float x1, float y1, float width, struct rgba s )
{
    int n = c->size * SUPERSAMPLE;
    float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;

    for (int j = 0; j < n; j++)
    {
        for (int i = 0; i < n; i++)
        {
            float x = (i + 0.5f) / c->scale, y = (j + 0.5f) / c->scale;
            float t = len2 ? ((x - x0) * dx + (y - y0) * dy) / len2 : 0, ex, ey;

            t = max( 0.0f, min( 1.0f, t ) );
            ex = x0 + t * dx - x;
            ey = y0 + t * dy - y;
            if (ex * ex + ey * ey <= width * width / 4) blend_sample( c, j * n + i, s );
        }
    }
}

/* a computer's screen on its stand, the wired connection of Windows 7 */
static void paint_monitor( struct canvas *c )
{
    paint_rect( c, 4.2f, 12.6f, 11.8f, 15.0f, 0.9f, color( 0x2b3036, 1 ), color( 0x2b3036, 1 ) );
    paint_rect( c, 4.8f, 13.1f, 11.2f, 14.4f, 0.5f, color( 0xd4d9df, 1 ), color( 0x7b838d, 1 ) );
    paint_rect( c, 6.8f, 11.2f, 9.2f, 13.2f, 0, color( 0x5b626b, 1 ), color( 0x3a4047, 1 ) );
    paint_rect( c, 0.8f, 1.2f, 15.2f, 11.8f, 1.2f, color( 0x1f2328, 1 ), color( 0x1f2328, 1 ) );
    paint_rect( c, 1.4f, 1.8f, 14.6f, 11.2f, 0.8f, color( 0xb8c0ca, 1 ), color( 0x4d545d, 1 ) );
    paint_rect( c, 2.6f, 3.0f, 13.4f, 10.0f, 0, color( 0x6cc0ff, 1 ), color( 0x0a3f9c, 1 ) );
    /* the glass: a gloss over its upper half */
    paint_rect( c, 2.6f, 3.0f, 13.4f, 6.2f, 0, color( 0xffffff, 0.45f ), color( 0xffffff, 0.08f ) );
}

/* five bars rising to the right; those the signal reaches are lit */
static void paint_bars( struct canvas *c, UINT bars )
{
    for (UINT i = 0; i < 5; i++)
    {
        float x = 0.8f + i * 3.0f, top = 11.8f - i * 2.5f;
        BOOL lit = i < bars;

        paint_rect( c, x, top, x + 2.6f, 15.0f, 0.5f, color( 0x1f2328, lit ? 1 : 0.45f ),
                    color( 0x1f2328, lit ? 1 : 0.45f ) );
        if (lit) paint_rect( c, x + 0.5f, top + 0.5f, x + 2.1f, 14.5f, 0.3f, color( 0xffffff, 1 ), color( 0x9fb3c8, 1 ) );
        else paint_rect( c, x + 0.5f, top + 0.5f, x + 2.1f, 14.5f, 0.3f, color( 0xffffff, 0.35f ), color( 0xffffff, 0.2f ) );
    }
}

/* the badge in the corner: a red disc with a white cross (no connection)
 * or an amber one with a spark (connections are available) */
static void paint_badge( struct canvas *c, BOOL cross )
{
    const float cx = 11.8f, cy = 11.8f;

    paint_circle( c, cx, cy, 4.2f, color( 0x3a0d05, 0.9f ), color( 0x3a0d05, 0.9f ) );
    if (cross)
    {
        paint_circle( c, cx, cy, 3.5f, color( 0xff7a66, 1 ), color( 0xb3120a, 1 ) );
        paint_stroke( c, cx - 1.6f, cy - 1.6f, cx + 1.6f, cy + 1.6f, 1.2f, color( 0xffffff, 1 ) );
        paint_stroke( c, cx + 1.6f, cy - 1.6f, cx - 1.6f, cy + 1.6f, 1.2f, color( 0xffffff, 1 ) );
    }
    else
    {
        paint_circle( c, cx, cy, 3.5f, color( 0xffe98a, 1 ), color( 0xe29b00, 1 ) );
        paint_stroke( c, cx, cy - 2.2f, cx, cy + 2.2f, 0.9f, color( 0xffffff, 0.95f ) );
        paint_stroke( c, cx - 2.2f, cy, cx + 2.2f, cy, 0.9f, color( 0xffffff, 0.95f ) );
    }
    /* the gloss on its upper half */
    paint_rect( c, cx - 2.6f, cy - 3.2f, cx + 2.6f, cy - 0.4f, 1.3f, color( 0xffffff, 0.5f ), color( 0xffffff, 0.05f ) );
}

enum icon_kind { ICON_WIRELESS, ICON_WIRED, ICON_NONE };

static HICON make_icon( enum icon_kind kind, UINT bars, BOOL cross, BOOL star )
{
    int size = GetSystemMetrics( SM_CXSMICON ), n = size * SUPERSAMPLE;
    BITMAPINFO info = { .bmiHeader = { .biSize = sizeof(info.bmiHeader), .biWidth = size, .biHeight = -size,
                                       .biPlanes = 1, .biBitCount = 32, .biCompression = BI_RGB } };
    struct canvas canvas = { size, n / 16.0f, NULL };
    ICONINFO icon = { .fIcon = TRUE };
    UINT32 *bits;
    HICON ret;

    if (!(canvas.px = calloc( n * n * 4, sizeof(float) ))) return NULL;
    if (!(icon.hbmColor = CreateDIBSection( NULL, &info, DIB_RGB_COLORS, (void **)&bits, NULL, 0 )))
    {
        free( canvas.px );
        return NULL;
    }

    if (kind == ICON_WIRELESS) paint_bars( &canvas, bars );
    else paint_monitor( &canvas );
    if (cross || star) paint_badge( &canvas, cross );

    /* each pixel is the mean of its samples; icons carry straight alpha */
    for (int y = 0; y < size; y++)
    {
        for (int x = 0; x < size; x++)
        {
            float sum[4] = { 0 };

            for (int j = 0; j < SUPERSAMPLE; j++)
                for (int i = 0; i < SUPERSAMPLE; i++)
                    for (int k = 0; k < 4; k++)
                        sum[k] += canvas.px[((y * SUPERSAMPLE + j) * n + x * SUPERSAMPLE + i) * 4 + k];
            for (int k = 0; k < 4; k++) sum[k] /= SUPERSAMPLE * SUPERSAMPLE;
            if (sum[3] < 1.0f / 255)
            {
                bits[y * size + x] = 0;
                continue;
            }
            bits[y * size + x] = ((UINT32)(sum[3] * 255 + 0.5f) << 24) |
                                 ((UINT32)(min( sum[0] / sum[3], 1.0f ) * 255 + 0.5f) << 16) |
                                 ((UINT32)(min( sum[1] / sum[3], 1.0f ) * 255 + 0.5f) << 8) |
                                 (UINT32)(min( sum[2] / sum[3], 1.0f ) * 255 + 0.5f);
        }
    }
    free( canvas.px );

    icon.hbmMask = CreateBitmap( size, size, 1, 1, NULL );
    ret = CreateIconIndirect( &icon );
    DeleteObject( icon.hbmColor );
    DeleteObject( icon.hbmMask );
    return ret;
}

static HICON load_icon( UINT id )
{
    return LoadImageW( pnidui_instance, MAKEINTRESOURCEW(id), IMAGE_ICON, GetSystemMetrics( SM_CXSMICON ),
                       GetSystemMetrics( SM_CYSMICON ), 0 );
}

UINT quality_bars( UINT quality )
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
    else if (state.wired_limited)
    {
        key = 250;
        swprintf( data.szTip, ARRAY_SIZE(data.szTip), L"%s\n%s", load_string( IDS_NETWORK ),
                  load_string( IDS_NO_INTERNET ) );
    }
    else
    {
        key = 400;
        lstrcpynW( data.szTip, load_string( IDS_NOT_CONNECTED ), ARRAY_SIZE(data.szTip) );
    }

    if (key != last_key || !current_icon)
    {
        /* a wired network has the pictures of Windows 7 (the two screens);
         * Wi-Fi has its bars */
        HICON icon = key >= 100 && key < 200 ? make_icon( ICON_WIRELESS, key - 100, FALSE, FALSE ) :
                     key == 200 ? load_icon( IDI_NETWORK_OK ) :
                     key == 250 ? load_icon( IDI_NETWORK_WARNING ) :
                     key == 300 ? make_icon( ICON_WIRELESS, 0, FALSE, TRUE ) :
                     load_icon( IDI_NETWORK_ABSENT );
        if (current_icon) DestroyIcon( current_icon );
        current_icon = icon;
        last_key = key;
    }
    else if (!wcscmp( data.szTip, current_tip )) return;

    data.hIcon = current_icon;
    lstrcpynW( current_tip, data.szTip, ARRAY_SIZE(current_tip) );
    if (!Shell_NotifyIconW( NIM_MODIFY, &data )) Shell_NotifyIconW( NIM_ADD, &data );
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
    MessageBoxW( NULL, text,
                 load_string( IDS_WIRELESS_GROUP ), MB_OK | MB_ICONWARNING );
}

/**********************************************************************
 *          Windows
 */

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

BOOL connections_folder_exists(void)
{
    WCHAR path[MAX_PATH];

    GetSystemDirectoryW( path, ARRAY_SIZE(path) );
    lstrcatW( path, L"\\netshell.dll" );
    return GetFileAttributesW( path ) != INVALID_FILE_ATTRIBUTES;
}

/* My Computer \ Control Panel \ Network Connections, as ncpa.cpl opens it */
void open_connections(void)
{
    flyout_hide();
    ShellExecuteW( NULL, NULL, L"explorer.exe",
                   L"/n,::{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\::{21EC2020-3AEA-1069-A2DD-08002B30309D}"
                   L"\\::{7007ACC7-3202-11D1-AAD2-00805FC1270E}", NULL, SW_SHOWNORMAL );
}

/* the Network and Sharing Center of the Control Panel (netcenter.dll) */
void open_network_center(void)
{
    flyout_hide();
    ShellExecuteW( NULL, NULL, L"rundll32.exe", L"netcenter.dll,ShowNetworkCenter", NULL, SW_SHOWNORMAL );
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
        if (!connecting_ssid[0]) flyout_refresh();
        return 0;
    case WM_TRAY_ICON:
        if (lp == WM_LBUTTONUP) flyout_toggle();
        else if (lp == WM_RBUTTONUP)
        {
            /* the menu of Windows 7's icon */
            HMENU menu = CreatePopupMenu();
            POINT pt;
            UINT cmd;

            AppendMenuW( menu, MF_STRING, 2, load_string( IDS_OPEN_NETWORK_CENTER ) );
            if (connections_folder_exists()) AppendMenuW( menu, MF_STRING, 1, load_string( IDS_OPEN_CONNECTIONS ) );
            GetCursorPos( &pt );
            SetForegroundWindow( hwnd );
            cmd = TrackPopupMenu( menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL );
            if (cmd == 1) open_connections();
            if (cmd == 2) open_network_center();
            DestroyMenu( menu );
        }
        return 0;
    case WM_SHOW_LIST:
        if (!flyout_visible()) flyout_toggle();
        return 0;
    case WM_WLAN_NOTIFY:
        if (wp == ACM_CONNECTION_COMPLETE) connecting_ssid[0] = 0;
        else if (wp == ACM_CONNECTION_ATTEMPT_FAIL) connection_failed( lp );
        update_icon();
        flyout_refresh();
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
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }

    if (wlan)
    {
        WlanRegisterNotification( wlan, 0, TRUE, NULL, NULL, NULL, NULL );
        WlanCloseHandle( wlan, NULL );
        wlan = NULL;
    }
    flyout_destroy();
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
