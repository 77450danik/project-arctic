/*
 * Network and Sharing Center: the connections
 *
 * A connection is an Ethernet or Wi-Fi adapter, as Network Connections
 * (netshell.dll, ReactOS patch 0017) lists them: from GetAdaptersAddresses,
 * named "Підключення по локальній мережі", "Бездротове мережне підключення"
 * and a number after the first, or as the user renamed it
 * (Control\Network\{4D36E972-…}\{GUID}\Connection\Name). What Wi-Fi is
 * joined to comes from wlanapi.dll.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <wlanapi.h>

#include "netcenter.h"

static const WCHAR connection_key[] =
    L"SYSTEM\\CurrentControlSet\\Control\\Network\\{4D36E972-E325-11CE-BFC1-08002BE10318}\\%s\\Connection";

/* wlanapi.dll's GUID of an interface: from its host name */
static void wlan_guid( const char *name, GUID *guid )
{
    DWORD hash = 5381;

    for (const char *p = name; *p; p++) hash = hash * 33 + (unsigned char)*p;
    memset( guid, 0, sizeof(*guid) );
    guid->Data1 = hash;
    guid->Data2 = 0x4152;
    guid->Data3 = 0x4354;
    memcpy( guid->Data4, "WLANIFAC", 8 );
}

static void wireless_info( struct connection *conn )
{
    WLAN_CONNECTION_ATTRIBUTES *attr = NULL;
    DWORD version, size = 0;
    HANDLE handle;
    GUID guid;

    if (WlanOpenHandle( 2, NULL, &version, &handle )) return;
    wlan_guid( conn->ifname_a, &guid );
    if (!WlanQueryInterface( handle, &guid, wlan_intf_opcode_current_connection, NULL, &size, (void **)&attr,
                             NULL ) && attr)
    {
        DOT11_SSID *ssid = &attr->wlanAssociationAttributes.dot11Ssid;
        char text[33];

        memcpy( text, ssid->ucSSID, min( ssid->uSSIDLength, 32 ) );
        text[min( ssid->uSSIDLength, 32 )] = 0;
        MultiByteToWideChar( CP_UTF8, 0, text, -1, conn->ssid, ARRAY_SIZE(conn->ssid) );
        conn->signal = attr->wlanAssociationAttributes.wlanSignalQuality;
        WlanFreeMemory( attr );
    }
    WlanCloseHandle( handle, NULL );
}

static BOOL usable_ipv4( const SOCKADDR *sa )
{
    const SOCKADDR_IN *in = (const SOCKADDR_IN *)sa;
    return sa->sa_family == AF_INET && (ntohl( in->sin_addr.s_addr ) >> 16) != 0xa9fe; /* not 169.254 */
}

static BOOL global_ipv6( const SOCKADDR *sa )
{
    const SOCKADDR_IN6 *in6 = (const SOCKADDR_IN6 *)sa;
    return sa->sa_family == AF_INET6 && !IN6_IS_ADDR_LINKLOCAL( &in6->sin6_addr ) &&
           !IN6_IS_ADDR_LOOPBACK( &in6->sin6_addr );
}

static void connection_name( struct connection *conn, UINT number )
{
    WCHAR path[256], name[128];
    DWORD size = sizeof(name);

    swprintf( path, ARRAY_SIZE(path), connection_key, conn->guid );
    if (!RegGetValueW( HKEY_LOCAL_MACHINE, path, L"Name", RRF_RT_REG_SZ, NULL, name, &size ) && name[0])
    {
        lstrcpynW( conn->name, name, ARRAY_SIZE(conn->name) );
        return;
    }
    lstrcpynW( conn->name, load_string( conn->wireless ? IDS_WIRELESS_CONNECTION : IDS_LAN_CONNECTION ),
               ARRAY_SIZE(conn->name) );
    if (number > 1)
    {
        WCHAR suffix[16];
        swprintf( suffix, ARRAY_SIZE(suffix), L" %u", number );
        lstrcatW( conn->name, suffix );
    }
}

UINT connections_list( struct connection *list, UINT max )
{
    ULONG size = 0x10000;
    IP_ADAPTER_ADDRESSES *addresses = malloc( size ), *a;
    UINT count = 0, lan = 0, wlan = 0;

    if (!addresses) return 0;
    if (GetAdaptersAddresses( AF_UNSPEC, GAA_FLAG_INCLUDE_GATEWAYS, NULL, addresses, &size ) == ERROR_BUFFER_OVERFLOW)
    {
        free( addresses );
        if (!(addresses = malloc( size ))) return 0;
        if (GetAdaptersAddresses( AF_UNSPEC, GAA_FLAG_INCLUDE_GATEWAYS, NULL, addresses, &size ))
            size = 0;
    }
    for (a = size ? addresses : NULL; a && count < max; a = a->Next)
    {
        struct connection *conn = &list[count];
        MIB_IF_ROW2 row = { 0 };
        char settings[4096];

        if (a->IfType != IF_TYPE_ETHERNET_CSMACD && a->IfType != IF_TYPE_IEEE80211) continue;
        memset( conn, 0, sizeof(*conn) );
        lstrcpynW( conn->ifname, a->Description, ARRAY_SIZE(conn->ifname) );
        WideCharToMultiByte( CP_UTF8, 0, conn->ifname, -1, conn->ifname_a, sizeof(conn->ifname_a), NULL, NULL );
        MultiByteToWideChar( CP_ACP, 0, a->AdapterName, -1, conn->guid, ARRAY_SIZE(conn->guid) );
        conn->wireless = a->IfType == IF_TYPE_IEEE80211;
        conn->luid = a->Luid.Value;
        conn->speed = a->TransmitLinkSpeed;
        row.InterfaceLuid = a->Luid;
        conn->up = !GetIfEntry2( &row ) ? row.MediaConnectState == MediaConnectStateConnected
                                        : a->OperStatus == IfOperStatusUp;
        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next)
        {
            if (usable_ipv4( u->Address.lpSockaddr )) conn->ipv4 = TRUE;
            if (global_ipv6( u->Address.lpSockaddr )) conn->ipv6 = TRUE;
        }
        for (IP_ADAPTER_GATEWAY_ADDRESS *g = a->FirstGatewayAddress; g; g = g->Next)
        {
            if (g->Address.lpSockaddr->sa_family == AF_INET) conn->gateway4 = TRUE;
            if (g->Address.lpSockaddr->sa_family == AF_INET6) conn->gateway6 = TRUE;
        }
        connection_name( conn, conn->wireless ? ++wlan : ++lan );
        conn->enabled = !host_read_settings( conn->ifname_a, settings, sizeof(settings) ) ||
                        !strstr( settings, "enabled=0" );
        host_link_info( conn->ifname_a, conn->adapter, ARRAY_SIZE(conn->adapter), NULL, NULL );
        if (conn->wireless) wireless_info( conn );
        count++;
    }
    free( addresses );
    return count;
}

/* by the adapter's GUID, Network Connections' name for it, or the host's */
BOOL connection_find( const WCHAR *id, struct connection *conn )
{
    struct connection list[16];
    UINT count = connections_list( list, ARRAY_SIZE(list) );
    WCHAR want[64];

    if (!id) return FALSE;
    while (*id == ' ' || *id == '"') id++;
    lstrcpynW( want, id, ARRAY_SIZE(want) );
    for (WCHAR *p = want + wcslen( want ); p > want && (p[-1] == ' ' || p[-1] == '"'); ) *--p = 0;
    for (UINT i = 0; i < count; i++)
    {
        if (wcsicmp( list[i].guid, want ) && wcsicmp( list[i].ifname, want ) && wcsicmp( list[i].name, want )) continue;
        *conn = list[i];
        return TRUE;
    }
    /* none given: the first one that is connected, as the tray's icon means */
    for (UINT i = 0; i < count; i++)
    {
        if (want[0] || !list[i].up) continue;
        *conn = list[i];
        return TRUE;
    }
    return FALSE;
}

/* a default route on a connection that is up: Windows' NCSI would check a
 * server too; a gateway is what Arctic's tray icon goes by as well */
BOOL internet_reachable(void)
{
    struct connection list[16];
    UINT count = connections_list( list, ARRAY_SIZE(list) );

    for (UINT i = 0; i < count; i++)
        if (list[i].up && list[i].enabled && (list[i].gateway4 || list[i].gateway6)) return TRUE;
    return FALSE;
}
