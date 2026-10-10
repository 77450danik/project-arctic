/*
 * Network and Sharing Center (netcenter.dll)
 *
 * Windows 7's Network and Sharing Center ({8E908FC9-BECC-40f6-915B-
 * F4CA0E70D03D}), a page of the Control Panel inside Explorer
 * (page_center.c), and what it opens: the status of a connection
 * (status.c), its properties (props.c), the properties of IPv4 and IPv6
 * (tcpip.c). The settings go to the host (settings.c, unix.c), which applies
 * them at once, as Windows does. docs/M5-network.md.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS

#include "netcenter.h"
#include "commctrl.h"
#include "unixlib.h"

HINSTANCE cp_instance;

const CLSID cp_clsid = { 0x8e908fc9, 0xbecc, 0x40f6, { 0x91, 0x5b, 0xf4, 0xca, 0x0e, 0x70, 0xd0, 0x3d } };
const WCHAR cp_class_name[] = L"NetworkCenter";

const struct cp_page cp_pages[] =
{
    { IDS_NETCENTER, L"pageNetworkCenter", center_page, NULL, center_timer },
    { IDS_SHARING_PAGE, L"pageAdvancedSharing", sharing_page, sharing_command, NULL },
};
const UINT cp_page_count = ARRAY_SIZE(cp_pages);

static BOOL unix_ready;

static NTSTATUS host_call( enum netcenter_funcs code, void *params )
{
    if (!unix_ready) return 0xc00000bb; /* STATUS_NOT_SUPPORTED */
    return WINE_UNIX_CALL( code, params );
}

BOOL host_read_settings( const char *ifname, char *text, UINT size )
{
    struct settings_params params;

    lstrcpynA( params.ifname, ifname, sizeof(params.ifname) );
    if (host_call( unix_read_settings, &params )) return FALSE;
    lstrcpynA( text, params.text, size );
    return TRUE;
}

BOOL host_write_settings( const char *ifname, const char *text )
{
    struct settings_params params;

    lstrcpynA( params.ifname, ifname, sizeof(params.ifname) );
    lstrcpynA( params.text, text, sizeof(params.text) );
    return !host_call( unix_write_settings, &params );
}

/* the name servers of the interface as the host has them now */
BOOL host_name_servers( const char *ifname, WCHAR *servers, UINT count )
{
    struct link_info_params params = { 0 };

    servers[0] = 0;
    lstrcpynA( params.ifname, ifname, sizeof(params.ifname) );
    if (host_call( unix_link_info, &params )) return FALSE;
    MultiByteToWideChar( CP_UTF8, 0, params.dns, -1, servers, count );
    return servers[0] != 0;
}

BOOL host_link_info( const char *ifname, WCHAR *adapter, UINT count, ULONG64 *speed, ULONG *connected_for )
{
    struct link_info_params params = { 0 };

    lstrcpynA( params.ifname, ifname, sizeof(params.ifname) );
    if (host_call( unix_link_info, &params )) return FALSE;
    if (adapter) MultiByteToWideChar( CP_UTF8, 0, params.adapter, -1, adapter, count );
    if (speed) *speed = (ULONG64)params.speed_kbps * 1000;
    if (connected_for) *connected_for = params.connected_for;
    return TRUE;
}

BOOL host_iwd_addresses( const char *ifname, const char *ipv4, const char *ipv6 )
{
    struct iwd_params params;

    lstrcpynA( params.ifname, ifname, sizeof(params.ifname) );
    lstrcpynA( params.ipv4, ipv4, sizeof(params.ipv4) );
    lstrcpynA( params.ipv6, ipv6, sizeof(params.ipv6) );
    return !host_call( unix_iwd_addresses, &params );
}

HICON load_icon( UINT id, int size )
{
    return LoadImageW( cp_instance, MAKEINTRESOURCEW( id ), IMAGE_ICON, size, size, LR_SHARED );
}

void open_adapter_settings(void)
{
    /* Network Connections, the folder of the connections (netshell.dll) */
    cp_run( L"explorer.exe", L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\::{21EC2020-3AEA-1069-A2DD-08002B30309D}"
                             L"\\::{7007ACC7-3202-11D1-AAD2-00805FC1270E}" );
}

static void init_common_controls(void)
{
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_INTERNET_CLASSES | ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS };
    InitCommonControlsEx( &icc );
}

static void system_aware(void)
{
    /* rundll32 is not DPI aware: the dialogs draw at the screen's scale, as in Windows */
    SetThreadDpiAwarenessContext( DPI_AWARENESS_CONTEXT_SYSTEM_AWARE );
}

/* rundll32 netcenter.dll,ShowNetworkCenter */
void WINAPI ShowNetworkCenter( HWND hwnd, HINSTANCE instance, LPSTR cmdline, int show )
{
    cp_open( 0, 0 );
}

/* rundll32 netcenter.dll,ShowConnectionStatus {GUID} or a host name (wlan0) */
void WINAPI ShowConnectionStatusW( HWND hwnd, HINSTANCE instance, LPWSTR cmdline, int show )
{
    struct connection conn;

    system_aware();
    init_common_controls();
    if (connection_find( cmdline, &conn )) show_status( hwnd, &conn );
}

void WINAPI ShowConnectionPropertiesW( HWND hwnd, HINSTANCE instance, LPWSTR cmdline, int show )
{
    struct connection conn;

    system_aware();
    init_common_controls();
    if (connection_find( cmdline, &conn )) show_properties( hwnd, &conn );
}

HRESULT WINAPI DllGetClassObject( REFCLSID clsid, REFIID riid, void **out )
{
    return cp_class_object( clsid, riid, out );
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    return cp_can_unload();
}

BOOL WINAPI DllMain( HINSTANCE instance, DWORD reason, void *reserved )
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        cp_instance = instance;
        DisableThreadLibraryCalls( instance );
        unix_ready = !__wine_init_unix_call();
    }
    return TRUE;
}
