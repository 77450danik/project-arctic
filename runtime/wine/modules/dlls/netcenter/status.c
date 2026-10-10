/*
 * Network and Sharing Center: the status of a connection
 *
 * Windows' "Стан" (netshell.dll's dialogs 23500, Wi-Fi 24050): what IPv4 and
 * IPv6 reach, the medium, for how long, at what speed, the network and its
 * signal for Wi-Fi, the bytes sent and received, refreshed every second;
 * "Докладно..." (24300) lists the addresses. "Вимкнути" turns the connection
 * off on the host, "Діагностика" asks DHCP again or joins Wi-Fi again.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdio.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include "netcenter.h"
#include "commctrl.h"

#define TIMER_REFRESH 1

struct status
{
    struct connection conn;
    HICON signal;
};

static void format_bytes( ULONG64 bytes, WCHAR *text, size_t count )
{
    WCHAR raw[32];
    NUMBERFMTW format = { 0, 0, 3, (WCHAR *)L",", (WCHAR *)L" ", 1 };

    swprintf( raw, ARRAY_SIZE(raw), L"%I64u", bytes );
    if (!GetNumberFormatEx( LOCALE_NAME_USER_DEFAULT, 0, raw, &format, text, count )) lstrcpynW( text, raw, count );
}

static void format_speed( ULONG64 bits, WCHAR *text, size_t count )
{
    if (bits >= 1000000000 && !(bits % 100000000))
        swprintf( text, count, load_string( IDS_GBPS ), (UINT)(bits / 1000000000), (UINT)(bits / 100000000 % 10) );
    else
        swprintf( text, count, load_string( IDS_MBPS ), (UINT)(bits / 1000000), (UINT)(bits / 100000 % 10) );
}

static void format_duration( ULONG seconds, WCHAR *text, size_t count )
{
    if (seconds >= 86400)
        swprintf( text, count, L"%u %s %02u:%02u:%02u", seconds / 86400, load_string( IDS_DAYS ), seconds / 3600 % 24,
                  seconds / 60 % 60, seconds % 60 );
    else
        swprintf( text, count, L"%02u:%02u:%02u", seconds / 3600, seconds / 60 % 60, seconds % 60 );
}

static const WCHAR *reach_text( const struct connection *conn, BOOL address, BOOL gateway )
{
    if (!conn->up || !address) return load_string( IDS_NO_NETWORK );
    return load_string( gateway ? IDS_INTERNET_ACCESS : IDS_NO_INTERNET );
}

static void refresh( HWND dlg, struct status *st )
{
    struct connection now;
    MIB_IF_ROW2 row = { 0 };
    WCHAR text[64];
    ULONG64 speed = 0;
    ULONG duration = 0;

    if (connection_find( st->conn.guid, &now )) st->conn = now;
    SetDlgItemTextW( dlg, IDC_IPV4_STATE, reach_text( &st->conn, st->conn.ipv4, st->conn.gateway4 ) );
    SetDlgItemTextW( dlg, IDC_IPV6_STATE, reach_text( &st->conn, st->conn.ipv6, st->conn.gateway6 ) );
    SetDlgItemTextW( dlg, IDC_MEDIA_STATE, load_string( !st->conn.enabled ? IDS_DISABLED :
                                                        st->conn.up ? IDS_CONNECTED : IDS_DISCONNECTED ) );
    host_link_info( st->conn.ifname_a, NULL, 0, &speed, &duration );
    format_duration( st->conn.up ? duration : 0, text, ARRAY_SIZE(text) );
    SetDlgItemTextW( dlg, IDC_DURATION, text );
    format_speed( st->conn.up ? speed : 0, text, ARRAY_SIZE(text) );
    SetDlgItemTextW( dlg, IDC_SPEED, text );
    if (st->conn.wireless)
    {
        HICON icon;
        int bars = st->conn.up ? min( 5, (st->conn.signal + 19) / 20 ) : 0;

        SetDlgItemTextW( dlg, IDC_SSID, st->conn.ssid );
        icon = load_icon( IDI_SIGNAL0 + bars, px( 24 ) );
        if (icon != st->signal)
        {
            st->signal = icon;
            SendDlgItemMessageW( dlg, IDC_SIGNAL, STM_SETICON, (WPARAM)icon, 0 );
        }
    }
    row.InterfaceLuid.Value = st->conn.luid;
    if (!GetIfEntry2( &row ))
    {
        format_bytes( row.OutOctets, text, ARRAY_SIZE(text) );
        SetDlgItemTextW( dlg, IDC_SENT, text );
        format_bytes( row.InOctets, text, ARRAY_SIZE(text) );
        SetDlgItemTextW( dlg, IDC_RECEIVED, text );
        swprintf( text, ARRAY_SIZE(text), L"%I64u", row.OutErrors );
        SetDlgItemTextW( dlg, 1012, text );
        swprintf( text, ARRAY_SIZE(text), L"%I64u", row.InErrors );
        SetDlgItemTextW( dlg, 1013, text );
    }
    SetDlgItemTextW( dlg, IDC_DISABLE, load_string( st->conn.enabled ? IDS_DISABLE : IDS_ENABLE ) );
}

/* "Вимкнути" / "Увімкнути": the connection on the host */
static void toggle_enabled( HWND dlg, struct status *st )
{
    struct settings s;

    settings_load( &st->conn, &s );
    s.enabled = !st->conn.enabled;
    if (settings_save( dlg, &st->conn, &s ) && !s.enabled) prop_press( dlg, PSBTN_CANCEL );
    else refresh( dlg, st );
}

/* "Діагностика": DHCP once more (a new "repair" in the settings makes
 * arctic-init start it again), or the Wi-Fi network joined again */
static void repair( HWND dlg, struct status *st )
{
    HCURSOR old = SetCursor( LoadCursorW( NULL, (const WCHAR *)IDC_WAIT ) );
    char text[4096], *line;
    struct settings s;

    settings_load( &st->conn, &s );
    if (!s.enabled)
    {
        s.enabled = TRUE;
        settings_save( dlg, &st->conn, &s );
    }
    else
    {
        if (!host_read_settings( st->conn.ifname_a, text, sizeof(text) ))
            lstrcpyA( text, "enabled=1\nipv4=dhcp\ndns=\nipv6=auto\ndns6=\n" );
        if ((line = strstr( text, "repair=" ))) *line = 0;
        snprintf( text + strlen( text ), sizeof(text) - strlen( text ), "repair=%lu\n", GetTickCount() );
        host_write_settings( st->conn.ifname_a, text );
        if (st->conn.wireless) settings_save( dlg, &st->conn, &s );
    }
    Sleep( 3000 );
    SetCursor( old );
    refresh( dlg, st );
}

static INT_PTR CALLBACK status_proc( HWND dlg, UINT msg, WPARAM wp, LPARAM lp )
{
    struct status *st = (struct status *)GetWindowLongPtrW( dlg, DWLP_USER );

    switch (msg)
    {
    case WM_INITDIALOG:
        st = (struct status *)((PROPSHEETPAGEW *)lp)->lParam;
        SetWindowLongPtrW( dlg, DWLP_USER, (LONG_PTR)st );
        SendDlgItemMessageW( dlg, IDC_ACTIVITY_ICON, STM_SETICON, (WPARAM)load_icon( IDI_NETWORK, px( 32 ) ), 0 );
        if (st->conn.wireless)
        {
            /* the bars where Windows draws its own */
            HWND signal = GetDlgItem( dlg, IDC_SIGNAL );
            RECT rect;

            SetWindowLongW( signal, GWL_STYLE, WS_CHILD | WS_VISIBLE | SS_ICON | SS_REALSIZEIMAGE );
            GetWindowRect( signal, &rect );
            MapWindowPoints( NULL, dlg, (POINT *)&rect, 2 );
            SetWindowPos( signal, NULL, rect.left - px( 4 ), rect.top - px( 8 ), px( 24 ), px( 24 ), SWP_NOZORDER );
        }
        refresh( dlg, st );
        SetTimer( dlg, TIMER_REFRESH, 1000, NULL );
        return TRUE;
    case WM_TIMER:
        if (wp == TIMER_REFRESH) refresh( dlg, st );
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD( wp ))
        {
        case IDC_DETAILS:
            show_details( dlg, &st->conn );
            return TRUE;
        case IDC_PROPERTIES:
            show_properties( dlg, &st->conn );
            refresh( dlg, st );
            return TRUE;
        case IDC_DISABLE:
            toggle_enabled( dlg, st );
            return TRUE;
        case IDC_DIAGNOSE:
            repair( dlg, st );
            return TRUE;
        case IDC_WIRELESS_PROPS:
            cp_run( L"rundll32.exe", L"pnidui.dll,ShowNetworkList" );
            return TRUE;
        }
        break;
    case WM_DESTROY:
        KillTimer( dlg, TIMER_REFRESH );
        break;
    }
    return FALSE;
}

void show_status( HWND owner, const struct connection *conn )
{
    PROPSHEETPAGEW page = { sizeof(page) };
    PROPSHEETHEADERW header = { sizeof(header) };
    struct status st = { *conn };

    page.hInstance = cp_instance;
    page.pszTemplate = MAKEINTRESOURCEW( conn->wireless ? IDD_STATUS_WLAN : IDD_STATUS_LAN );
    page.pfnDlgProc = status_proc;
    page.lParam = (LPARAM)&st;
    header.dwFlags = PSH_PROPSHEETPAGE | PSH_NOAPPLYNOW | PSH_NOCONTEXTHELP | PSH_USEHICON;
    header.hwndParent = owner;
    header.hInstance = cp_instance;
    header.hIcon = load_icon( IDI_NETCENTER, GetSystemMetrics( SM_CXSMICON ) );
    header.pszCaption = format_string( IDS_STATUS_TITLE, conn->name );
    header.nPages = 1;
    header.ppsp = &page;
    PropertySheetW( &header );
}

/**********************************************************************
 *          Відомості про мережеве підключення
 */

static void add_row( HWND list, UINT label, const WCHAR *value )
{
    LVITEMW item = { LVIF_TEXT };

    item.iItem = SendMessageW( list, LVM_GETITEMCOUNT, 0, 0 );
    item.pszText = label ? load_string( label ) : (WCHAR *)L"";
    item.iItem = SendMessageW( list, LVM_INSERTITEMW, 0, (LPARAM)&item );
    item.iSubItem = 1;
    item.pszText = (WCHAR *)value;
    SendMessageW( list, LVM_SETITEMTEXTW, item.iItem, (LPARAM)&item );
}

static void sockaddr_text( const SOCKADDR *sa, WCHAR *text, DWORD count )
{
    text[0] = 0;
    if (sa->sa_family == AF_INET)
        InetNtopW( AF_INET, (void *)&((const SOCKADDR_IN *)sa)->sin_addr, text, count );
    else if (sa->sa_family == AF_INET6)
        InetNtopW( AF_INET6, (void *)&((const SOCKADDR_IN6 *)sa)->sin6_addr, text, count );
}

/* the name servers: the host's for the interface, which follow its settings
 * at once; Wine's list is the process's own, read when it started */
static void add_dns_rows( HWND list, const struct connection *conn, IP_ADAPTER_ADDRESSES *a, BOOL v6 )
{
    WCHAR servers[512], text[128], *token, *context;
    BOOL first = TRUE;

    if (host_name_servers( conn->ifname_a, servers, ARRAY_SIZE(servers) ))
    {
        for (token = wcstok( servers, L" ", &context ); token; token = wcstok( NULL, L" ", &context ))
        {
            if ((wcschr( token, ':' ) != NULL) != v6) continue;
            add_row( list, first ? (v6 ? IDS_DET_DNS6 : IDS_DET_DNS4) : 0, token );
            first = FALSE;
        }
        return;
    }
    for (IP_ADAPTER_DNS_SERVER_ADDRESS *d = a->FirstDnsServerAddress; d; d = d->Next)
    {
        if (d->Address.lpSockaddr->sa_family != (v6 ? AF_INET6 : AF_INET)) continue;
        sockaddr_text( d->Address.lpSockaddr, text, ARRAY_SIZE(text) );
        add_row( list, first ? (v6 ? IDS_DET_DNS6 : IDS_DET_DNS4) : 0, text );
        first = FALSE;
    }
}

static void fill_details( HWND list, const struct connection *conn )
{
    ULONG size = 0x10000;
    IP_ADAPTER_ADDRESSES *addresses = malloc( size ), *a;
    struct settings s;
    WCHAR text[128];
    BOOL first;

    settings_load( conn, &s );
    if (!addresses || GetAdaptersAddresses( AF_UNSPEC, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_INCLUDE_PREFIX, NULL,
                                            addresses, &size ))
    {
        free( addresses );
        return;
    }
    for (a = addresses; a; a = a->Next)
    {
        WCHAR guid[40];

        MultiByteToWideChar( CP_ACP, 0, a->AdapterName, -1, guid, ARRAY_SIZE(guid) );
        if (wcsicmp( guid, conn->guid )) continue;
        add_row( list, IDS_DET_SUFFIX, a->DnsSuffix ? a->DnsSuffix : L"" );
        add_row( list, IDS_DET_DESCRIPTION, conn->adapter[0] ? conn->adapter : conn->ifname );
        swprintf( text, ARRAY_SIZE(text), L"%02X-%02X-%02X-%02X-%02X-%02X", a->PhysicalAddress[0], a->PhysicalAddress[1],
                  a->PhysicalAddress[2], a->PhysicalAddress[3], a->PhysicalAddress[4], a->PhysicalAddress[5] );
        add_row( list, IDS_DET_PHYSICAL, text );
        add_row( list, IDS_DET_DHCP, load_string( s.static4 ? IDS_NO : IDS_YES ) );
        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next)
        {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            sockaddr_text( u->Address.lpSockaddr, text, ARRAY_SIZE(text) );
            add_row( list, IDS_DET_IPV4, text );
            prefix_to_mask( u->OnLinkPrefixLength, text, ARRAY_SIZE(text) );
            add_row( list, IDS_DET_MASK, text );
        }
        first = TRUE;
        for (IP_ADAPTER_GATEWAY_ADDRESS *g = a->FirstGatewayAddress; g; g = g->Next)
        {
            if (g->Address.lpSockaddr->sa_family != AF_INET) continue;
            sockaddr_text( g->Address.lpSockaddr, text, ARRAY_SIZE(text) );
            add_row( list, first ? IDS_DET_GATEWAY4 : 0, text );
            first = FALSE;
        }
        add_dns_rows( list, conn, a, FALSE );
        add_row( list, IDS_DET_NETBIOS, load_string( s.netbios == 2 ? IDS_NO : IDS_YES ) );
        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next)
        {
            const SOCKADDR_IN6 *in6 = (const SOCKADDR_IN6 *)u->Address.lpSockaddr;

            if (in6->sin6_family != AF_INET6) continue;
            sockaddr_text( u->Address.lpSockaddr, text, ARRAY_SIZE(text) );
            add_row( list, IN6_IS_ADDR_LINKLOCAL( &in6->sin6_addr ) ? IDS_DET_LINK_LOCAL : IDS_DET_IPV6, text );
        }
        first = TRUE;
        for (IP_ADAPTER_GATEWAY_ADDRESS *g = a->FirstGatewayAddress; g; g = g->Next)
        {
            if (g->Address.lpSockaddr->sa_family != AF_INET6) continue;
            sockaddr_text( g->Address.lpSockaddr, text, ARRAY_SIZE(text) );
            add_row( list, first ? IDS_DET_GATEWAY6 : 0, text );
            first = FALSE;
        }
        add_dns_rows( list, conn, a, TRUE );
        break;
    }
    free( addresses );
}

static INT_PTR CALLBACK details_proc( HWND dlg, UINT msg, WPARAM wp, LPARAM lp )
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        HWND list = GetDlgItem( dlg, IDC_DETAILS_LIST );
        LVCOLUMNW column = { LVCF_TEXT | LVCF_WIDTH };
        RECT rect;

        GetClientRect( list, &rect );
        SendMessageW( list, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT );
        column.cx = rect.right * 45 / 100;
        column.pszText = load_string( IDS_DETAILS_PROPERTY );
        SendMessageW( list, LVM_INSERTCOLUMNW, 0, (LPARAM)&column );
        column.cx = rect.right - column.cx - GetSystemMetrics( SM_CXVSCROLL );
        column.pszText = load_string( IDS_DETAILS_VALUE );
        SendMessageW( list, LVM_INSERTCOLUMNW, 1, (LPARAM)&column );
        fill_details( list, (const struct connection *)lp );
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD( wp ) == IDOK || LOWORD( wp ) == IDCANCEL) EndDialog( dlg, IDOK );
        return TRUE;
    }
    return FALSE;
}

void show_details( HWND owner, const struct connection *conn )
{
    DialogBoxParamW( cp_instance, MAKEINTRESOURCEW( IDD_DETAILS ), owner, details_proc, (LPARAM)conn );
}
