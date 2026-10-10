/*
 * Network and Sharing Center: the TCP/IP settings of a connection
 *
 * Kept where the host reads them, /run/arctic/network/<interface>.conf
 * (unix.c), and applied by it at once: arctic-init sets the addresses of a
 * wired port, IPv6, turns the connection off; arctic-resolv takes the name
 * servers set by hand. A Wi-Fi adapter's addresses by hand go into iwd's
 * network files and the network is joined again. As in Windows, the
 * settings are also in Tcpip\Parameters\Interfaces\{GUID} for programs
 * that read them there.
 *
 *   enabled=1
 *   ipv4=dhcp | static | off       address=192.168.0.10/24  gateway=...
 *   dns=8.8.8.8 1.1.1.1            (empty: from DHCP)
 *   ipv6=auto | static | off       address6=2001:db8::5/64  gateway6=...
 *   dns6=...
 *   alternate=...                  netbios=0
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdio.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wlanapi.h>

#include "netcenter.h"
#include "commctrl.h"

/**********************************************************************
 *          Addresses
 */

BOOL valid_ipv4( const WCHAR *text )
{
    IN_ADDR addr;
    return text && text[0] && InetPtonW( AF_INET, text, &addr ) == 1;
}

BOOL valid_ipv6( const WCHAR *text )
{
    IN6_ADDR addr;
    return text && text[0] && InetPtonW( AF_INET6, text, &addr ) == 1;
}

UINT mask_to_prefix( const WCHAR *mask )
{
    IN_ADDR addr;
    ULONG bits;
    UINT prefix = 0;

    if (InetPtonW( AF_INET, mask, &addr ) != 1) return 24;
    bits = ntohl( addr.s_addr );
    while (bits & 0x80000000)
    {
        prefix++;
        bits <<= 1;
    }
    return prefix;
}

void prefix_to_mask( UINT prefix, WCHAR *mask, size_t count )
{
    ULONG bits = prefix ? 0xffffffff << (32 - min( prefix, 32 )) : 0;
    swprintf( mask, count, L"%u.%u.%u.%u", bits >> 24, (bits >> 16) & 0xff, (bits >> 8) & 0xff, bits & 0xff );
}

/* a SysIPAddress32 that is empty reads as nothing, not 0.0.0.0 */
void ipaddr_get( HWND dlg, UINT id, WCHAR *text, size_t count )
{
    HWND control = GetDlgItem( dlg, id );
    WCHAR cls[32];
    DWORD addr;

    text[0] = 0;
    if (!control) return;
    GetClassNameW( control, cls, ARRAY_SIZE(cls) );
    if (wcsicmp( cls, WC_IPADDRESSW ))
    {
        GetWindowTextW( control, text, count );
        return;
    }
    if (SendMessageW( control, IPM_ISBLANK, 0, 0 )) return;
    SendMessageW( control, IPM_GETADDRESS, 0, (LPARAM)&addr );
    swprintf( text, count, L"%u.%u.%u.%u", FIRST_IPADDRESS( addr ), SECOND_IPADDRESS( addr ),
              THIRD_IPADDRESS( addr ), FOURTH_IPADDRESS( addr ) );
}

void ipaddr_set( HWND dlg, UINT id, const WCHAR *text )
{
    HWND control = GetDlgItem( dlg, id );
    WCHAR cls[32];
    IN_ADDR addr;

    if (!control) return;
    GetClassNameW( control, cls, ARRAY_SIZE(cls) );
    if (wcsicmp( cls, WC_IPADDRESSW ))
    {
        SetWindowTextW( control, text ? text : L"" );
        return;
    }
    if (!text || InetPtonW( AF_INET, text, &addr ) != 1)
    {
        SendMessageW( control, IPM_CLEARADDRESS, 0, 0 );
        return;
    }
    SendMessageW( control, IPM_SETADDRESS, 0, ntohl( addr.s_addr ) );
}

/**********************************************************************
 *          Loading
 */

static void split_address( const char *value, WCHAR *addr, size_t addr_count, UINT *prefix )
{
    char text[64], *slash;

    lstrcpynA( text, value, sizeof(text) );
    if ((slash = strchr( text, '/' )))
    {
        *slash = 0;
        *prefix = atoi( slash + 1 );
    }
    MultiByteToWideChar( CP_UTF8, 0, text, -1, addr, addr_count );
}

static UINT split_list( const char *value, WCHAR list[][48], UINT width, UINT max )
{
    char text[512], *token, *context;
    UINT count = 0;

    lstrcpynA( text, value, sizeof(text) );
    for (token = strtok_s( text, " ,", &context ); token && count < max; token = strtok_s( NULL, " ,", &context ))
        MultiByteToWideChar( CP_UTF8, 0, token, -1, (WCHAR *)((char *)list + count++ * width), width / sizeof(WCHAR) );
    return count;
}

void settings_load( const struct connection *conn, struct settings *s )
{
    char text[4096], *line, *context;

    memset( s, 0, sizeof(*s) );
    s->enabled = s->ipv4_on = s->ipv6_on = TRUE;
    if (!host_read_settings( conn->ifname_a, text, sizeof(text) )) return;

    for (line = strtok_s( text, "\r\n", &context ); line; line = strtok_s( NULL, "\r\n", &context ))
    {
        char *value = strchr( line, '=' );

        if (!value || line[0] == '#') continue;
        *value++ = 0;
        if (!strcmp( line, "enabled" )) s->enabled = atoi( value ) != 0;
        else if (!strcmp( line, "ipv4" ))
        {
            s->static4 = !strcmp( value, "static" );
            s->ipv4_on = strcmp( value, "off" ) != 0;
        }
        else if (!strcmp( line, "ipv6" ))
        {
            s->static6 = !strcmp( value, "static" );
            s->ipv6_on = strcmp( value, "off" ) != 0;
        }
        else if (!strcmp( line, "address" ) && s->n4 < MAX_ADDRESSES)
        {
            UINT prefix = 24;
            split_address( value, s->addr4[s->n4], ARRAY_SIZE(s->addr4[0]), &prefix );
            prefix_to_mask( prefix, s->mask4[s->n4], ARRAY_SIZE(s->mask4[0]) );
            s->n4++;
        }
        else if (!strcmp( line, "gateway" ))
            MultiByteToWideChar( CP_UTF8, 0, value, -1, s->gw4, ARRAY_SIZE(s->gw4) );
        else if (!strcmp( line, "dns" ))
        {
            WCHAR list[MAX_ADDRESSES][48];
            s->ndns4 = split_list( value, list, sizeof(list[0]), MAX_ADDRESSES );
            for (UINT i = 0; i < s->ndns4; i++) lstrcpynW( s->dns4[i], list[i], ARRAY_SIZE(s->dns4[0]) );
            s->static_dns4 = s->ndns4 > 0;
        }
        else if (!strcmp( line, "address6" ) && s->n6 < MAX_ADDRESSES)
        {
            UINT prefix = 64;
            split_address( value, s->addr6[s->n6], ARRAY_SIZE(s->addr6[0]), &prefix );
            s->prefix6[s->n6++] = prefix;
        }
        else if (!strcmp( line, "gateway6" ))
            MultiByteToWideChar( CP_UTF8, 0, value, -1, s->gw6, ARRAY_SIZE(s->gw6) );
        else if (!strcmp( line, "dns6" ))
        {
            s->ndns6 = split_list( value, s->dns6, sizeof(s->dns6[0]), MAX_ADDRESSES );
            s->static_dns6 = s->ndns6 > 0;
        }
        else if (!strcmp( line, "alternate" ))
        {
            /* user ip mask gateway dns1 dns2, or apipa */
            WCHAR list[6][48];
            UINT n = split_list( value, list, sizeof(list[0]), 6 );
            s->alt_user = n && !wcscmp( list[0], L"user" );
            if (n > 1) lstrcpynW( s->alt_ip, list[1], ARRAY_SIZE(s->alt_ip) );
            if (n > 2) lstrcpynW( s->alt_mask, list[2], ARRAY_SIZE(s->alt_mask) );
            if (n > 3 && wcscmp( list[3], L"-" )) lstrcpynW( s->alt_gw, list[3], ARRAY_SIZE(s->alt_gw) );
            if (n > 4 && wcscmp( list[4], L"-" )) lstrcpynW( s->alt_dns1, list[4], ARRAY_SIZE(s->alt_dns1) );
            if (n > 5 && wcscmp( list[5], L"-" )) lstrcpynW( s->alt_dns2, list[5], ARRAY_SIZE(s->alt_dns2) );
        }
        else if (!strcmp( line, "netbios" )) s->netbios = atoi( value );
    }
}

/**********************************************************************
 *          Saving
 */

static void append( char *text, size_t size, const char *fmt, ... )
{
    size_t len = strlen( text );
    va_list args;

    va_start( args, fmt );
    vsnprintf( text + len, size - len, fmt, args );
    va_end( args );
}

static void utf8( const WCHAR *w, char *a, int size )
{
    if (!WideCharToMultiByte( CP_UTF8, 0, w, -1, a, size, NULL, NULL )) a[0] = 0;
}

static void settings_text( const struct settings *s, char *text, size_t size )
{
    char a[64], b[64];
    UINT i;

    text[0] = 0;
    append( text, size, "# TCP/IP of the connection, from its properties (netcenter.dll); arctic-init applies it\n" );
    append( text, size, "enabled=%d\n", s->enabled ? 1 : 0 );
    append( text, size, "ipv4=%s\n", !s->ipv4_on ? "off" : s->static4 ? "static" : "dhcp" );
    if (s->static4)
    {
        for (i = 0; i < s->n4; i++)
        {
            utf8( s->addr4[i], a, sizeof(a) );
            append( text, size, "address=%s/%u\n", a, mask_to_prefix( s->mask4[i] ) );
        }
        utf8( s->gw4, a, sizeof(a) );
        if (a[0]) append( text, size, "gateway=%s\n", a );
    }
    append( text, size, "dns=" );
    if (s->static_dns4 || s->static4)
        for (i = 0; i < s->ndns4; i++)
        {
            utf8( s->dns4[i], a, sizeof(a) );
            append( text, size, "%s%s", i ? " " : "", a );
        }
    append( text, size, "\n" );
    append( text, size, "ipv6=%s\n", !s->ipv6_on ? "off" : s->static6 ? "static" : "auto" );
    if (s->static6)
    {
        for (i = 0; i < s->n6; i++)
        {
            utf8( s->addr6[i], a, sizeof(a) );
            append( text, size, "address6=%s/%u\n", a, s->prefix6[i] ? s->prefix6[i] : 64 );
        }
        utf8( s->gw6, a, sizeof(a) );
        if (a[0]) append( text, size, "gateway6=%s\n", a );
    }
    append( text, size, "dns6=" );
    if (s->static_dns6 || s->static6)
        for (i = 0; i < s->ndns6; i++)
        {
            utf8( s->dns6[i], a, sizeof(a) );
            append( text, size, "%s%s", i ? " " : "", a );
        }
    append( text, size, "\n" );
    utf8( s->alt_ip, a, sizeof(a) );
    utf8( s->alt_mask, b, sizeof(b) );
    {
        char gw[64], d1[64], d2[64];
        utf8( s->alt_gw, gw, sizeof(gw) );
        utf8( s->alt_dns1, d1, sizeof(d1) );
        utf8( s->alt_dns2, d2, sizeof(d2) );
        append( text, size, "alternate=%s %s %s %s %s %s\n", s->alt_user ? "user" : "apipa", a[0] ? a : "-",
                b[0] ? b : "-", gw[0] ? gw : "-", d1[0] ? d1 : "-", d2[0] ? d2 : "-" );
    }
    append( text, size, "netbios=%u\n", s->netbios );
}

/* what iwd is to use on every network: its [IPv4] and [IPv6] sections */
static void iwd_sections( const struct settings *s, char *ipv4, size_t size4, char *ipv6, size_t size6 )
{
    char a[64];

    ipv4[0] = ipv6[0] = 0;
    if (s->ipv4_on && s->static4 && s->n4)
    {
        WCHAR mask[16];
        utf8( s->addr4[0], a, sizeof(a) );
        append( ipv4, size4, "Address=%s\n", a );
        prefix_to_mask( mask_to_prefix( s->mask4[0] ), mask, ARRAY_SIZE(mask) );
        utf8( mask, a, sizeof(a) );
        append( ipv4, size4, "Netmask=%s\n", a );
        utf8( s->gw4, a, sizeof(a) );
        if (a[0]) append( ipv4, size4, "Gateway=%s\n", a );
    }
    if (s->ipv6_on && s->static6 && s->n6)
    {
        utf8( s->addr6[0], a, sizeof(a) );
        append( ipv6, size6, "Address=%s/%u\n", a, s->prefix6[0] ? s->prefix6[0] : 64 );
        utf8( s->gw6, a, sizeof(a) );
        if (a[0]) append( ipv6, size6, "Gateway=%s\n", a );
    }
}

/* Wi-Fi takes its addresses when it joins: the network it is on, again */
static void wifi_rejoin( const struct connection *conn )
{
    WLAN_CONNECTION_ATTRIBUTES *attr = NULL;
    WLAN_CONNECTION_PARAMETERS params = { 0 };
    DWORD version, size = 0;
    WCHAR profile[256];
    HANDLE handle;
    GUID guid;
    DWORD hash = 5381;

    for (const char *p = conn->ifname_a; *p; p++) hash = hash * 33 + (unsigned char)*p;
    memset( &guid, 0, sizeof(guid) );
    guid.Data1 = hash;
    guid.Data2 = 0x4152;
    guid.Data3 = 0x4354;
    memcpy( guid.Data4, "WLANIFAC", 8 );

    if (WlanOpenHandle( 2, NULL, &version, &handle )) return;
    if (!WlanQueryInterface( handle, &guid, wlan_intf_opcode_current_connection, NULL, &size, (void **)&attr, NULL ) &&
        attr)
    {
        lstrcpynW( profile, attr->strProfileName, ARRAY_SIZE(profile) );
        WlanFreeMemory( attr );
        WlanDisconnect( handle, &guid, NULL );
        params.wlanConnectionMode = wlan_connection_mode_profile;
        params.strProfile = profile;
        params.dot11BssType = dot11_BSS_type_infrastructure;
        WlanConnect( handle, &guid, &params, NULL );
    }
    WlanCloseHandle( handle, NULL );
}

/* Tcpip\Parameters\Interfaces\{GUID}, as Windows keeps them */
static void mirror_registry( const struct connection *conn, const struct settings *s )
{
    WCHAR path[256], multi[512], *p;
    DWORD dhcp = !s->static4;
    HKEY key;
    UINT i;

    swprintf( path, ARRAY_SIZE(path), L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces\\%s",
              conn->guid );
    if (RegCreateKeyExW( HKEY_LOCAL_MACHINE, path, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL )) return;
    RegSetValueExW( key, L"EnableDHCP", 0, REG_DWORD, (BYTE *)&dhcp, sizeof(dhcp) );

    for (p = multi, i = 0; i < (s->static4 ? s->n4 : 0); i++) p += swprintf( p, 32, L"%s", s->addr4[i] ) + 1;
    if (p == multi) p += swprintf( p, 16, L"0.0.0.0" ) + 1;
    *p++ = 0;
    RegSetValueExW( key, L"IPAddress", 0, REG_MULTI_SZ, (BYTE *)multi, (p - multi) * sizeof(WCHAR) );
    for (p = multi, i = 0; i < (s->static4 ? s->n4 : 0); i++) p += swprintf( p, 32, L"%s", s->mask4[i] ) + 1;
    if (p == multi) p += swprintf( p, 16, L"0.0.0.0" ) + 1;
    *p++ = 0;
    RegSetValueExW( key, L"SubnetMask", 0, REG_MULTI_SZ, (BYTE *)multi, (p - multi) * sizeof(WCHAR) );
    p = multi;
    if (s->static4 && s->gw4[0]) p += swprintf( p, 32, L"%s", s->gw4 ) + 1;
    *p++ = 0;
    RegSetValueExW( key, L"DefaultGateway", 0, REG_MULTI_SZ, (BYTE *)multi, (p - multi) * sizeof(WCHAR) );
    multi[0] = 0;
    for (i = 0; i < (s->static_dns4 || s->static4 ? s->ndns4 : 0); i++)
    {
        if (i) lstrcatW( multi, L"," );
        lstrcatW( multi, s->dns4[i] );
    }
    RegSetValueExW( key, L"NameServer", 0, REG_SZ, (BYTE *)multi, (lstrlenW( multi ) + 1) * sizeof(WCHAR) );
    RegCloseKey( key );
}

BOOL settings_save( HWND owner, const struct connection *conn, const struct settings *s )
{
    char text[4096], ipv4[512], ipv6[512], old_ipv4[512], old_ipv6[512];
    struct settings old;

    settings_load( conn, &old );
    iwd_sections( &old, old_ipv4, sizeof(old_ipv4), old_ipv6, sizeof(old_ipv6) );
    settings_text( s, text, sizeof(text) );
    if (!host_write_settings( conn->ifname_a, text ))
    {
        MessageBoxW( owner, load_string( IDS_APPLY_FAILED ), conn->name, MB_OK | MB_ICONERROR );
        return FALSE;
    }
    mirror_registry( conn, s );
    if (conn->wireless)
    {
        iwd_sections( s, ipv4, sizeof(ipv4), ipv6, sizeof(ipv6) );
        host_iwd_addresses( conn->ifname_a, ipv4, ipv6 );
        /* the name servers apply as they are; addresses when the network is joined */
        if (conn->up && (strcmp( ipv4, old_ipv4 ) || strcmp( ipv6, old_ipv6 ))) wifi_rejoin( conn );
    }
    return TRUE;
}
