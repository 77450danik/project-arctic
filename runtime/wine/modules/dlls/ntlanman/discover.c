/*
 * The computers of the network, found as Windows finds them: WS-Discovery
 * (Windows 7 and later, and what answers for a NAS), NetBIOS names asked of
 * everybody on the wire (older Windows, Samba), and the SMB service of mDNS
 * (macOS, Linux).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "ntlanman.h"
#include "iphlpapi.h"
#include "bcrypt.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(ntlanman);

#define LISTEN_MS       1300
#define CACHE_MS        10000
#define MAX_INTERFACES  16
#define MAX_GETS        48

struct iface
{
    struct in_addr address;
    struct in_addr broadcast;
};

static CRITICAL_SECTION found_cs;
static CRITICAL_SECTION_DEBUG found_cs_debug =
{
    0, 0, &found_cs, { &found_cs_debug.ProcessLocksList, &found_cs_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)(__FILE__ ": found_cs") }
};
static CRITICAL_SECTION found_cs = { &found_cs_debug, -1, 0, 0, 0, 0 };

static CRITICAL_SECTION scan_cs;
static CRITICAL_SECTION_DEBUG scan_cs_debug =
{
    0, 0, &scan_cs, { &scan_cs_debug.ProcessLocksList, &scan_cs_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)(__FILE__ ": scan_cs") }
};
static CRITICAL_SECTION scan_cs = { &scan_cs_debug, -1, 0, 0, 0, 0 };

static struct computer *found;
static UINT found_count, found_size;
static ULONGLONG found_at;
static struct iface ifaces[MAX_INTERFACES];
static UINT iface_count;

BOOL discovery_enabled(void)
{
    DWORD value = 1, size = sizeof(value);

    RegGetValueW( HKEY_LOCAL_MACHINE, L"SOFTWARE\\Arctic\\Network", L"NetworkDiscovery", RRF_RT_REG_DWORD, NULL,
                  &value, &size );
    return value != 0;
}

static void add_computer( const WCHAR *name, const WCHAR *comment, const struct in_addr *address )
{
    WCHAR clean[64];
    UINT i, len;

    while (*name == ' ') name++;
    /* a computer has a name: one opened by its address is not listed */
    if (!address && name[wcsspn( name, L"0123456789." )] == 0) return;
    lstrcpynW( clean, name, ARRAY_SIZE(clean) );
    for (len = wcslen( clean ); len && clean[len - 1] == ' '; len--) clean[len - 1] = 0;
    if (!len || wcspbrk( clean, L"\\/:*?\"<>|" )) return;

    EnterCriticalSection( &found_cs );
    for (i = 0; i < found_count; i++)
    {
        if (wcsicmp( found[i].name, clean )) continue;
        if (comment && comment[0] && !found[i].comment[0])
            lstrcpynW( found[i].comment, comment, ARRAY_SIZE(found[i].comment) );
        if (address && !found[i].address[0])
            inet_ntop( AF_INET, (void *)address, found[i].address, sizeof(found[i].address) );
        LeaveCriticalSection( &found_cs );
        return;
    }
    if (found_count == found_size)
    {
        struct computer *grown = realloc( found, (found_size + 32) * sizeof(*found) );
        if (!grown)
        {
            LeaveCriticalSection( &found_cs );
            return;
        }
        found = grown;
        found_size += 32;
    }
    memset( &found[found_count], 0, sizeof(*found) );
    wcscpy( found[found_count].name, clean );
    if (comment) lstrcpynW( found[found_count].comment, comment, ARRAY_SIZE(found[found_count].comment) );
    if (address) inet_ntop( AF_INET, (void *)address, found[found_count].address, sizeof(found[found_count].address) );
    found_count++;
    LeaveCriticalSection( &found_cs );
    TRACE( "%s\n", debugstr_w( clean ) );
}

/* the IPv4 networks this computer is on */
static void list_interfaces(void)
{
    IP_ADAPTER_ADDRESSES *adapters, *adapter;
    ULONG size = 16384;

    iface_count = 0;
    if (!(adapters = malloc( size ))) return;
    if (GetAdaptersAddresses( AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                              NULL, adapters, &size ) == ERROR_BUFFER_OVERFLOW)
    {
        free( adapters );
        if (!(adapters = malloc( size ))) return;
        if (GetAdaptersAddresses( AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                  NULL, adapters, &size ))
        {
            free( adapters );
            return;
        }
    }
    for (adapter = adapters; adapter; adapter = adapter->Next)
    {
        IP_ADAPTER_UNICAST_ADDRESS *unicast;

        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next)
        {
            struct sockaddr_in *sin = (struct sockaddr_in *)unicast->Address.lpSockaddr;
            UINT prefix = unicast->OnLinkPrefixLength, mask;

            if (!sin || sin->sin_family != AF_INET || iface_count == MAX_INTERFACES) continue;
            if (!prefix || prefix > 32) prefix = 24;
            mask = prefix == 32 ? 0xffffffff : ~(0xffffffffu >> prefix);
            ifaces[iface_count].address = sin->sin_addr;
            ifaces[iface_count].broadcast.s_addr = sin->sin_addr.s_addr | htonl( ~mask );
            iface_count++;
        }
    }
    free( adapters );
}

static SOCKET udp_socket(void)
{
    struct sockaddr_in any = { .sin_family = AF_INET };
    SOCKET sock = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    BOOL on = TRUE;

    if (sock == INVALID_SOCKET) return sock;
    setsockopt( sock, SOL_SOCKET, SO_BROADCAST, (const char *)&on, sizeof(on) );
    bind( sock, (struct sockaddr *)&any, sizeof(any) );
    return sock;
}

/* what comes within the time left, or 0 */
static int receive( SOCKET sock, ULONGLONG until, char *buf, int size, struct sockaddr_in *from )
{
    ULONGLONG now = GetTickCount64();
    struct timeval wait;
    int from_len = sizeof(*from), n;
    fd_set readable;

    if (now >= until) return 0;
    wait.tv_sec = (until - now) / 1000;
    wait.tv_usec = ((until - now) % 1000) * 1000;
    FD_ZERO( &readable );
    FD_SET( sock, &readable );
    if (select( 0, &readable, NULL, NULL, &wait ) != 1) return 0;
    n = recvfrom( sock, buf, size, 0, (struct sockaddr *)from, &from_len );
    return n > 0 ? n : -1;
}

/**********************************************************************
 *          NetBIOS
 */

static void netbios_ask( SOCKET sock, const struct in_addr *to, BOOL status, BOOL broadcast )
{
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons( 137 ), .sin_addr = *to };
    BYTE packet[50] = { 0 };
    UINT i;

    BCryptGenRandom( NULL, packet, 2, BCRYPT_USE_SYSTEM_PREFERRED_RNG );
    if (broadcast) packet[3] = 0x10;
    if (!status) packet[2] = 0x01;
    packet[5] = 1;
    packet[12] = 32;
    packet[13] = 'C';       /* the name "*", in halves of bytes */
    packet[14] = 'K';
    for (i = 2; i < 32; i++) packet[13 + i] = 'A';
    packet[47] = status ? 0x21 : 0x20;
    packet[49] = 1;
    sendto( sock, (const char *)packet, sizeof(packet), 0, (struct sockaddr *)&addr, sizeof(addr) );
}

static DWORD WINAPI netbios_thread( void *arg )
{
    ULONGLONG until = GetTickCount64() + LISTEN_MS;
    struct in_addr asked[128];
    UINT asked_count = 0, i;
    struct sockaddr_in from;
    SOCKET sock = udp_socket();
    BYTE buf[2048];
    int n;

    if (sock == INVALID_SOCKET) return 0;
    for (i = 0; i < iface_count; i++)
    {
        netbios_ask( sock, &ifaces[i].broadcast, FALSE, TRUE );
        netbios_ask( sock, &ifaces[i].broadcast, TRUE, TRUE );
    }
    while ((n = receive( sock, until, (char *)buf, sizeof(buf), &from )))
    {
        UINT type, names;

        if (n < 57 || !(buf[2] & 0x80)) continue;
        type = buf[46] << 8 | buf[47];
        if (type == 0x20)
        {
            /* it has the name "*": what is it called? */
            for (i = 0; i < asked_count; i++)
                if (asked[i].s_addr == from.sin_addr.s_addr) break;
            if (i < asked_count || asked_count == ARRAY_SIZE(asked)) continue;
            asked[asked_count++] = from.sin_addr;
            netbios_ask( sock, &from.sin_addr, TRUE, FALSE );
            continue;
        }
        if (type != 0x21) continue;
        names = buf[56];
        for (i = 0; i < names && 57 + (i + 1) * 18 <= (UINT)n; i++)
        {
            const BYTE *entry = buf + 57 + i * 18;
            WCHAR name[32];
            int len;

            /* the file server's name, not a group's */
            if (entry[15] != 0x20 || (entry[16] & 0x80)) continue;
            len = MultiByteToWideChar( CP_OEMCP, 0, (const char *)entry, 15, name, ARRAY_SIZE(name) - 1 );
            name[len] = 0;
            add_computer( name, NULL, &from.sin_addr );
            break;
        }
    }
    closesocket( sock );
    return 0;
}

/**********************************************************************
 *          mDNS
 */

/* a name of a DNS message, its shortcuts followed; where the record goes on */
static int dns_name( const BYTE *msg, int len, int pos, char *out, int size )
{
    int end = -1, jumps = 0, used = 0;

    for (;;)
    {
        int label;

        if (pos >= len) return -1;
        label = msg[pos];
        if (!label)
        {
            if (end < 0) end = pos + 1;
            break;
        }
        if ((label & 0xc0) == 0xc0)
        {
            if (pos + 1 >= len || ++jumps > 16) return -1;
            if (end < 0) end = pos + 2;
            pos = (label & 0x3f) << 8 | msg[pos + 1];
            continue;
        }
        if (pos + 1 + label > len || used + label + 2 > size) return -1;
        if (used) out[used++] = '.';
        memcpy( out + used, msg + pos + 1, label );
        used += label;
        pos += 1 + label;
    }
    out[used] = 0;
    return end;
}

static DWORD WINAPI mdns_thread( void *arg )
{
    static const BYTE query[] =
    {
        0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0,
        4, '_', 's', 'm', 'b', 4, '_', 't', 'c', 'p', 5, 'l', 'o', 'c', 'a', 'l', 0,
        0, 12, 0x80, 1,     /* the pointers to the service, answered to us alone */
    };
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons( 5353 ) }, from;
    ULONGLONG until = GetTickCount64() + LISTEN_MS;
    SOCKET sock = udp_socket();
    BYTE buf[4096];
    UINT i;
    int n;

    if (sock == INVALID_SOCKET) return 0;
    inet_pton( AF_INET, "224.0.0.251", &to.sin_addr );
    for (i = 0; i < iface_count; i++)
    {
        setsockopt( sock, IPPROTO_IP, IP_MULTICAST_IF, (const char *)&ifaces[i].address, sizeof(struct in_addr) );
        sendto( sock, (const char *)query, sizeof(query), 0, (struct sockaddr *)&to, sizeof(to) );
    }
    while ((n = receive( sock, until, (char *)buf, sizeof(buf), &from )))
    {
        char name[256], instance[256] = "", target[256] = "";
        int pos = 12, records, questions;
        WCHAR wide[64];
        char *dot;

        if (n < 12 || !(buf[2] & 0x80)) continue;
        questions = buf[4] << 8 | buf[5];
        records = (buf[6] << 8 | buf[7]) + (buf[8] << 8 | buf[9]) + (buf[10] << 8 | buf[11]);
        while (questions-- > 0 && pos > 0)
        {
            pos = dns_name( buf, n, pos, name, sizeof(name) );
            if (pos > 0) pos += 4;
        }
        while (records-- > 0 && pos > 0 && pos + 10 <= n)
        {
            int type, rdlen;

            if ((pos = dns_name( buf, n, pos, name, sizeof(name) )) < 0 || pos + 10 > n) break;
            type = buf[pos] << 8 | buf[pos + 1];
            rdlen = buf[pos + 8] << 8 | buf[pos + 9];
            pos += 10;
            if (pos + rdlen > n) break;
            if (type == 12 && !stricmp( name, "_smb._tcp.local" ))
                dns_name( buf, n, pos, instance, sizeof(instance) );
            else if (type == 33 && rdlen > 6 && strstr( name, "_smb._tcp" ))
                dns_name( buf, n, pos + 6, target, sizeof(target) );
            pos += rdlen;
        }
        if (!instance[0]) continue;
        /* the computer's own name where the answer tells it, the service's otherwise */
        if (!target[0]) lstrcpynA( target, instance, sizeof(target) );
        if ((dot = strchr( target, '.' ))) *dot = 0;
        MultiByteToWideChar( CP_UTF8, 0, target, -1, wide, ARRAY_SIZE(wide) );
        wide[ARRAY_SIZE(wide) - 1] = 0;
        add_computer( wide, NULL, &from.sin_addr );
    }
    closesocket( sock );
    return 0;
}

/**********************************************************************
 *          WS-Discovery
 */

struct wsd_device
{
    struct in_addr address;
    UINT port;
    char path[160];
    char endpoint[96];
};

static void new_uuid( char *out )
{
    BYTE raw[16];

    BCryptGenRandom( NULL, raw, sizeof(raw), BCRYPT_USE_SYSTEM_PREFERRED_RNG );
    sprintf( out, "%02x%02x%02x%02x-%02x%02x-4%01x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", raw[0], raw[1], raw[2],
             raw[3], raw[4], raw[5], raw[6] & 15, raw[7], (raw[8] & 0x3f) | 0x80, raw[9], raw[10], raw[11], raw[12],
             raw[13], raw[14], raw[15] );
}

/* the text of an element, whatever prefix its namespace has here */
static BOOL element_text( const char *xml, const char *tag, char *out, UINT size )
{
    UINT tag_len = strlen( tag );
    const char *p = xml;

    while ((p = strstr( p, tag )))
    {
        const char *start = p + tag_len, *end;

        /* <prefix:Tag> or <Tag>, not the closing one and not a longer name */
        if (p > xml && (p[-1] == ':' || p[-1] == '<') && *start == '>' && (end = strchr( ++start, '<' )) &&
            end > start)
        {
            const char *open = p;

            while (open > xml && open[-1] != '<' && open[-1] != '/') open--;
            if (open > xml && open[-1] == '<')
            {
                while (start < end && isspace( (unsigned char)*start )) start++;
                while (end > start && isspace( (unsigned char)end[-1] )) end--;
                lstrcpynA( out, start, min( size, (UINT)(end - start) + 1 ) );
                return out[0] != 0;
            }
        }
        p += tag_len;
    }
    return FALSE;
}

/* the device tells what it is: <pub:Computer>NAME/Workgroup:HOME</pub:Computer> */
static DWORD WINAPI wsd_get_thread( void *arg )
{
    static const char envelope[] =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<soap:Envelope xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\">"
        "<soap:Header><wsa:To>%s</wsa:To>"
        "<wsa:Action>http://schemas.xmlsoap.org/ws/2004/09/transfer/Get</wsa:Action>"
        "<wsa:MessageID>urn:uuid:%s</wsa:MessageID>"
        "<wsa:ReplyTo><wsa:Address>http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</wsa:Address>"
        "</wsa:ReplyTo></soap:Header><soap:Body/></soap:Envelope>";
    struct wsd_device *device = arg;
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons( device->port ), .sin_addr = device->address };
    struct timeval wait = { 1, 200000 };
    char body[1024], request[1600], *answer = NULL, uuid[40], text[200], host[32], *sep;
    DWORD timeout = 1500;
    u_long on = 1, off = 0;
    fd_set writable;
    WCHAR name[64];
    SOCKET sock;
    int len, total = 0, n;

    if ((sock = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP )) == INVALID_SOCKET) goto done;
    ioctlsocket( sock, FIONBIO, &on );
    if (connect( sock, (struct sockaddr *)&to, sizeof(to) ))
    {
        FD_ZERO( &writable );
        FD_SET( sock, &writable );
        if (WSAGetLastError() != WSAEWOULDBLOCK || select( 0, NULL, &writable, NULL, &wait ) != 1) goto done;
    }
    ioctlsocket( sock, FIONBIO, &off );
    setsockopt( sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout) );
    setsockopt( sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout) );

    new_uuid( uuid );
    inet_ntop( AF_INET, &device->address, host, sizeof(host) );
    len = snprintf( body, sizeof(body), envelope, device->endpoint, uuid );
    len = snprintf( request, sizeof(request),
                    "POST %s HTTP/1.1\r\nHost: %s:%u\r\nContent-Type: application/soap+xml\r\n"
                    "User-Agent: WSDAPI\r\nConnection: close\r\nContent-Length: %d\r\n\r\n%s",
                    device->path, host, device->port, len, body );
    if (send( sock, request, len, 0 ) != len || !(answer = malloc( 32768 ))) goto done;
    while (total < 32767 && (n = recv( sock, answer + total, 32767 - total, 0 )) > 0)
    {
        total += n;
        answer[total] = 0;
        if (strstr( answer, "Envelope>" )) break;
    }
    answer[total] = 0;
    if (!element_text( answer, "Computer", text, sizeof(text) )) goto done;
    if ((sep = strpbrk( text, "/\\" ))) *sep = 0;
    MultiByteToWideChar( CP_UTF8, 0, text, -1, name, ARRAY_SIZE(name) );
    name[ARRAY_SIZE(name) - 1] = 0;
    add_computer( name, NULL, &device->address );
done:
    if (sock != INVALID_SOCKET) closesocket( sock );
    free( answer );
    free( device );
    return 0;
}

static DWORD WINAPI wsd_thread( void *arg )
{
    static const char probe[] =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<soap:Envelope xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\""
        " xmlns:wsd=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\""
        " xmlns:wsdp=\"http://schemas.xmlsoap.org/ws/2006/02/devprof\">"
        "<soap:Header><wsa:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</wsa:To>"
        "<wsa:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</wsa:Action>"
        "<wsa:MessageID>urn:uuid:%s</wsa:MessageID></soap:Header>"
        "<soap:Body><wsd:Probe><wsd:Types>wsdp:Device</wsd:Types></wsd:Probe></soap:Body></soap:Envelope>";
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons( 3702 ) }, from;
    ULONGLONG until = GetTickCount64() + LISTEN_MS;
    struct in_addr seen[MAX_GETS];
    HANDLE threads[MAX_GETS];
    UINT thread_count = 0, i;
    SOCKET sock = udp_socket();
    char message[1200], uuid[40], *buf;
    int len, n;

    if (sock == INVALID_SOCKET) return 0;
    if (!(buf = malloc( 16384 )))
    {
        closesocket( sock );
        return 0;
    }
    new_uuid( uuid );
    len = snprintf( message, sizeof(message), probe, uuid );
    inet_pton( AF_INET, "239.255.255.250", &to.sin_addr );
    for (i = 0; i < iface_count; i++)
    {
        setsockopt( sock, IPPROTO_IP, IP_MULTICAST_IF, (const char *)&ifaces[i].address, sizeof(struct in_addr) );
        sendto( sock, message, len, 0, (struct sockaddr *)&to, sizeof(to) );
        sendto( sock, message, len, 0, (struct sockaddr *)&to, sizeof(to) );
    }
    while ((n = receive( sock, until, buf, 16383, &from )))
    {
        struct wsd_device *device;
        char types[400], addresses[600], endpoint[96], *url, *slash, *colon;

        if (n < 0 || thread_count == MAX_GETS) continue;
        buf[n] = 0;
        if (!strstr( buf, "ProbeMatch" )) continue;
        /* computers, not printers and cameras */
        if (!element_text( buf, "Types", types, sizeof(types) ) || !strstr( types, "Computer" )) continue;
        for (i = 0; i < thread_count; i++)
            if (seen[i].s_addr == from.sin_addr.s_addr) break;
        if (i < thread_count) continue;
        if (!element_text( buf, "Address", endpoint, sizeof(endpoint) )) continue;

        if (!(device = calloc( 1, sizeof(*device) ))) continue;
        device->address = from.sin_addr;
        device->port = 5357;
        lstrcpynA( device->endpoint, endpoint, sizeof(device->endpoint) );
        /* where it serves its description; Windows' own place when it does not say */
        snprintf( device->path, sizeof(device->path), "/%s", !strncmp( endpoint, "urn:uuid:", 9 ) ? endpoint + 9
                                                                                                  : endpoint );
        if (element_text( buf, "XAddrs", addresses, sizeof(addresses) ) && (url = strstr( addresses, "http://" )) &&
            url[7] != '[')
        {
            char *end = strpbrk( url, " \t\r\n" );

            if (end) *end = 0;
            url += 7;
            if ((slash = strchr( url, '/' )))
            {
                lstrcpynA( device->path, slash, sizeof(device->path) );
                *slash = 0;
            }
            if ((colon = strchr( url, ':' ))) device->port = atoi( colon + 1 );
        }
        seen[thread_count] = from.sin_addr;
        if ((threads[thread_count] = CreateThread( NULL, 0, wsd_get_thread, device, 0, NULL ))) thread_count++;
        else free( device );
    }
    closesocket( sock );
    free( buf );
    if (thread_count) WaitForMultipleObjects( thread_count, threads, TRUE, 4000 );
    for (i = 0; i < thread_count; i++) CloseHandle( threads[i] );
    return 0;
}

/**********************************************************************
 *          The list
 */

static int compare_computers( const void *a, const void *b )
{
    return wcsicmp( ((const struct computer *)a)->name, ((const struct computer *)b)->name );
}

/* the computers whose folders are open here, and those of the network drives */
static void add_known(void)
{
    char *reply, *line, *next;
    WCHAR name[64], key_name[16], remote[MAX_PATH], server[64];
    DWORD index, size;
    HKEY network, key;

    if ((reply = malloc( 16384 )))
    {
        if (!host_command( "LIST", NULL, NULL, NULL, NULL, reply, 16384 ))
        {
            for (line = reply; *line; line = next)
            {
                char *space;

                if ((next = strchr( line, '\n' ))) *next++ = 0;
                else next = line + strlen( line );
                if (!(space = strchr( line, ' ' ))) continue;
                *space = 0;
                MultiByteToWideChar( CP_UTF8, 0, line, -1, name, ARRAY_SIZE(name) );
                name[ARRAY_SIZE(name) - 1] = 0;
                CharUpperW( name );
                add_computer( name, NULL, NULL );
            }
        }
        free( reply );
    }
    if (RegOpenKeyExW( HKEY_CURRENT_USER, L"Network", 0, KEY_READ, &network )) return;
    for (index = 0; ; index++)
    {
        size = ARRAY_SIZE(key_name);
        if (RegEnumKeyExW( network, index, key_name, &size, NULL, NULL, NULL, NULL )) break;
        if (RegOpenKeyExW( network, key_name, 0, KEY_READ, &key )) continue;
        size = sizeof(remote);
        if (!RegGetValueW( key, NULL, L"RemotePath", RRF_RT_REG_SZ, NULL, remote, &size ) &&
            split_unc( remote, server, ARRAY_SIZE(server), NULL, 0, NULL ))
        {
            CharUpperW( server );
            add_computer( server, NULL, NULL );
        }
        RegCloseKey( key );
    }
    RegCloseKey( network );
}

/* the address the last look at the network found for a name */
BOOL discovered_address( const WCHAR *server, char *address, UINT size )
{
    BOOL ret = FALSE;
    UINT i;

    EnterCriticalSection( &found_cs );
    for (i = 0; i < found_count && !ret; i++)
    {
        if (wcsicmp( found[i].name, server ) || !found[i].address[0]) continue;
        lstrcpynA( address, found[i].address, size );
        ret = TRUE;
    }
    LeaveCriticalSection( &found_cs );
    return ret;
}

/* the list is the caller's to free */
UINT discover_computers( struct computer **list, BOOL fresh )
{
    HANDLE threads[3];
    UINT count = 0, i;

    EnterCriticalSection( &scan_cs );
    if (fresh || !found_at || GetTickCount64() - found_at > CACHE_MS)
    {
        EnterCriticalSection( &found_cs );
        found_count = 0;
        LeaveCriticalSection( &found_cs );
        if (discovery_enabled())
        {
            list_interfaces();
            threads[count] = CreateThread( NULL, 0, wsd_thread, NULL, 0, NULL );
            if (threads[count]) count++;
            threads[count] = CreateThread( NULL, 0, netbios_thread, NULL, 0, NULL );
            if (threads[count]) count++;
            threads[count] = CreateThread( NULL, 0, mdns_thread, NULL, 0, NULL );
            if (threads[count]) count++;
            if (count) WaitForMultipleObjects( count, threads, TRUE, 8000 );
            for (i = 0; i < count; i++) CloseHandle( threads[i] );
        }
        add_known();
        EnterCriticalSection( &found_cs );
        qsort( found, found_count, sizeof(*found), compare_computers );
        LeaveCriticalSection( &found_cs );
        found_at = GetTickCount64();
    }
    EnterCriticalSection( &found_cs );
    count = found_count;
    if ((*list = malloc( (count + 1) * sizeof(**list) ))) memcpy( *list, found, count * sizeof(**list) );
    else count = 0;
    LeaveCriticalSection( &found_cs );
    LeaveCriticalSection( &scan_cs );
    return count;
}
