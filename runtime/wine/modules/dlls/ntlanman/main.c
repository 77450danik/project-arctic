/*
 * Microsoft Windows Network, the network provider (ntlanman.dll)
 *
 * What mpr.dll asks about the network of Windows computers: who is on it
 * (discover.c), what each shares (smb2.c), and connecting to a share. The
 * files of a share are the host's work: arctic-smb mounts \\server\share
 * where Wine looks for it, the first time a program names it, and this
 * provider tells it the name and password to use. docs/M5-network.md.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "ntlanman.h"
#include "winioctl.h"
#include "lmcons.h"
#define WINE_MOUNTMGR_EXTENSIONS
#include "ddk/mountmgr.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(ntlanman);

HINSTANCE ntlanman_instance;

static WCHAR provider_name[128] = L"Microsoft Windows Network";

static INIT_ONCE init_once = INIT_ONCE_STATIC_INIT;

static BOOL WINAPI init_sockets( INIT_ONCE *once, void *param, void **context )
{
    DWORD size = sizeof(provider_name);
    WSADATA data;

    WSAStartup( MAKEWORD( 2, 2 ), &data );
    RegGetValueW( HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\LanmanWorkstation\\NetworkProvider",
                  L"Name", RRF_RT_REG_SZ, NULL, provider_name, &size );
    return TRUE;
}

static void init(void)
{
    InitOnceExecuteOnce( &init_once, init_sockets, NULL, NULL );
}

/**********************************************************************
 *          The host's service
 */

static BOOL session( UINT *port, char *token, UINT size )
{
    WCHAR path[MAX_PATH];
    char text[128], *space, *end;
    DWORD read = 0;
    HANDLE file;

    if (!ExpandEnvironmentStringsW( L"%ProgramData%\\Arctic\\Smb\\session", path, ARRAY_SIZE(path) )) return FALSE;
    file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                        OPEN_EXISTING, 0, NULL );
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    ReadFile( file, text, sizeof(text) - 1, &read, NULL );
    CloseHandle( file );
    text[read] = 0;
    if (!(space = strchr( text, ' ' ))) return FALSE;
    *space++ = 0;
    if ((end = strpbrk( space, "\r\n" ))) *end = 0;
    *port = atoi( text );
    lstrcpynA( token, space, size );
    return *port != 0;
}

static void append_hex( char *out, UINT size, const WCHAR *text )
{
    char utf8[1024];
    UINT len = strlen( out ), i, n;

    if (len + 2 >= size) return;
    out[len++] = ' ';
    n = text ? WideCharToMultiByte( CP_UTF8, 0, text, -1, utf8, sizeof(utf8), NULL, NULL ) : 0;
    if (n) n--;
    if (!n) out[len++] = '-';
    for (i = 0; i < n && len + 3 < size; i++) len += sprintf( out + len, "%02x", (BYTE)utf8[i] );
    out[len] = 0;
}

static void from_hex( const char *hex, WCHAR *out, UINT count )
{
    char utf8[1024];
    UINT i;

    for (i = 0; i < sizeof(utf8) - 1 && isxdigit( (BYTE)hex[i * 2] ) && isxdigit( (BYTE)hex[i * 2 + 1] ); i++)
    {
        char byte[3] = { hex[i * 2], hex[i * 2 + 1], 0 };
        utf8[i] = strtoul( byte, NULL, 16 );
    }
    utf8[i] = 0;
    if (!MultiByteToWideChar( CP_UTF8, 0, utf8, -1, out, count )) out[0] = 0;
    out[count - 1] = 0;
}

/* 0, the errno the service answered, or -1 when it is not there; what it said
 * before "OK" and on that line goes to reply */
DWORD host_command( const char *command, const WCHAR *a, const WCHAR *b, const WCHAR *c, const WCHAR *d,
                    char *reply, UINT size )
{
    struct sockaddr_in to = { .sin_family = AF_INET };
    char token[80], *request, *answer, *line, *next;
    DWORD timeout = 45000, ret = ~0u;
    UINT port, total = 0;
    SOCKET sock;
    int n;

    init();
    if (reply && size) reply[0] = 0;
    if (!session( &port, token, sizeof(token) )) return ret;
    if (!(request = malloc( 8192 ))) return ret;
    if (!(answer = malloc( 32768 )))
    {
        free( request );
        return ret;
    }
    snprintf( request, 8192, "%s\n%s", token, command );
    if (a || b || c || d) append_hex( request, 8190, a );
    if (b || c || d) append_hex( request, 8190, b );
    if (c || d) append_hex( request, 8190, c );
    if (d) append_hex( request, 8190, d );
    strcat( request, "\n" );

    to.sin_port = htons( port );
    to.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
    if ((sock = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP )) == INVALID_SOCKET) goto done;
    setsockopt( sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout) );
    if (connect( sock, (struct sockaddr *)&to, sizeof(to) ) ||
        send( sock, request, strlen( request ), 0 ) != (int)strlen( request ))
    {
        closesocket( sock );
        goto done;
    }
    shutdown( sock, SD_SEND );
    while (total < 32767 && (n = recv( sock, answer + total, 32767 - total, 0 )) > 0) total += n;
    closesocket( sock );
    answer[total] = 0;

    for (line = answer; *line; line = next)
    {
        if ((next = strchr( line, '\n' ))) *next++ = 0;
        else next = line + strlen( line );
        if (!strncmp( line, "ERR ", 4 ))
        {
            ret = atoi( line + 4 );
            if (!ret) ret = ~0u;
            break;
        }
        if (!strncmp( line, "OK", 2 ) && (!line[2] || line[2] == ' '))
        {
            line += line[2] ? 3 : 2;
            ret = 0;
        }
        if (reply && strlen( reply ) + strlen( line ) + 2 < size)
        {
            strcat( reply, line );
            if (!ret) break;
            strcat( reply, "\n" );
        }
        if (!ret) break;
    }
done:
    SecureZeroMemory( request, 8192 );
    free( request );
    free( answer );
    TRACE( "%s %s %s: %lu\n", command, debugstr_w( a ), debugstr_w( b ), ret );
    return ret;
}

/* an errno of the kernel's SMB client, as Windows would say it */
static DWORD error_of_errno( DWORD err )
{
    switch (err)
    {
    case 0: return WN_SUCCESS;
    case 1:    /* EPERM */
    case 13:   /* EACCES */
    case 126:  /* ENOKEY */
    case 127:  /* EKEYEXPIRED */
    case 129:  /* EKEYREJECTED */
        return ERROR_ACCESS_DENIED;
    case 2:    /* ENOENT */
    case 6:    /* ENXIO */
    case 22:   /* EINVAL */
        return ERROR_BAD_NET_NAME;
    case 16:   /* EBUSY */
        return ERROR_SHARING_VIOLATION;
    case 95:   /* EOPNOTSUPP */
        return ERROR_NOT_SUPPORTED;
    case ~0u: return ERROR_NO_NETWORK;
    default:   /* the host is down, not reachable, refuses, does not answer */
        return ERROR_BAD_NETPATH;
    }
}

BOOL host_resolve( const WCHAR *server, char *address, UINT size )
{
    struct in_addr numeric;
    char name[128], reply[128];

    WideCharToMultiByte( CP_UTF8, 0, server, -1, name, sizeof(name), NULL, NULL );
    name[sizeof(name) - 1] = 0;
    if (inet_pton( AF_INET, name, &numeric ) == 1)
    {
        lstrcpynA( address, name, size );
        return TRUE;
    }
    /* what the last look at the network found */
    if (discovered_address( server, address, size )) return TRUE;
    if (host_command( "RESOLVE", server, NULL, NULL, NULL, reply, sizeof(reply) ) || !reply[0]) return FALSE;
    lstrcpynA( address, reply, size );
    return TRUE;
}

void host_tell_address( const WCHAR *server, const char *address )
{
    WCHAR wide[64];

    MultiByteToWideChar( CP_UTF8, 0, address, -1, wide, ARRAY_SIZE(wide) );
    host_command( "HOST", server, wide, NULL, NULL, NULL, 0 );
}

BOOL host_get_logon( const WCHAR *server, struct logon *logon )
{
    char reply[4096], *user, *domain, *password;

    memset( logon, 0, sizeof(*logon) );
    if (host_command( "GETCRED", server, NULL, NULL, NULL, reply, sizeof(reply) )) return FALSE;
    user = reply;
    if (!(domain = strchr( user, ' ' ))) return FALSE;
    *domain++ = 0;
    if (!(password = strchr( domain, ' ' ))) return FALSE;
    *password++ = 0;
    from_hex( user, logon->user, ARRAY_SIZE(logon->user) );
    from_hex( domain, logon->domain, ARRAY_SIZE(logon->domain) );
    from_hex( password, logon->password, ARRAY_SIZE(logon->password) );
    SecureZeroMemory( reply, sizeof(reply) );
    return logon->user[0] != 0;
}

static void forget_shares( const WCHAR *server );

void host_set_logon( const WCHAR *server, const struct logon *logon )
{
    host_command( "CRED", server, logon->user, logon->domain, logon->password, NULL, 0 );
    forget_shares( server );
}

/**********************************************************************
 *          Names and drives
 */

/* \\server, \\server\share, \\server\share\the\rest */
BOOL split_unc( const WCHAR *path, WCHAR *server, UINT server_count, WCHAR *share, UINT share_count,
                const WCHAR **rest )
{
    const WCHAR *start, *end;

    if (share && share_count) share[0] = 0;
    if (rest) *rest = NULL;
    if (!path || path[0] != '\\' || path[1] != '\\' || !path[2] || path[2] == '\\') return FALSE;
    start = path + 2;
    if (!(end = wcschr( start, '\\' ))) end = start + wcslen( start );
    if ((UINT)(end - start) >= server_count) return FALSE;
    memcpy( server, start, (end - start) * sizeof(WCHAR) );
    server[end - start] = 0;
    if (!*end || !end[1]) return TRUE;
    start = end + 1;
    if (!(end = wcschr( start, '\\' ))) end = start + wcslen( start );
    if (share)
    {
        if ((UINT)(end - start) >= share_count) return FALSE;
        memcpy( share, start, (end - start) * sizeof(WCHAR) );
        share[end - start] = 0;
    }
    if (rest && *end && end[1]) *rest = end;
    return TRUE;
}

static HANDLE mount_manager(void)
{
    return CreateFileW( MOUNTMGR_DOS_DEVICE_NAME, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, 0, 0 );
}

/* \\server\share of a network drive */
BOOL drive_remote( WCHAR letter, WCHAR *remote, UINT count )
{
    char buffer[1024];
    struct mountmgr_unix_drive *data = (struct mountmgr_unix_drive *)buffer;
    HANDLE manager = mount_manager();
    BOOL ret = FALSE;
    DWORD size;

    if (manager == INVALID_HANDLE_VALUE) return FALSE;
    memset( data, 0, sizeof(*data) );
    data->letter = letter;
    if (DeviceIoControl( manager, IOCTL_MOUNTMGR_QUERY_UNIX_DRIVE, data, sizeof(*data), data, sizeof(buffer),
                         &size, NULL ) && data->mount_point_offset)
    {
        char *mount = buffer + data->mount_point_offset, *p;

        if (!strncmp( mount, "unc/", 4 ))
        {
            mount += 2;
            mount[0] = '\\';
            for (p = mount; *p; p++) if (*p == '/') *p = '\\';
            ret = MultiByteToWideChar( CP_UTF8, 0, mount, -1, remote, count ) != 0;
        }
    }
    CloseHandle( manager );
    return ret;
}

/* the folder of a share as the host has it: the names in lower case */
static void unix_folder( char *out, UINT size, const WCHAR *server, const WCHAR *share )
{
    WCHAR path[400];
    char *p;

    swprintf( path, ARRAY_SIZE(path), L"unc/%s/%s", server, share );
    WideCharToMultiByte( CP_UTF8, 0, path, -1, out, size, NULL, NULL );
    out[size - 1] = 0;
    for (p = out; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 'a' - 'A';
}

DWORD drive_define( WCHAR letter, const WCHAR *server, const WCHAR *share )
{
    char buffer[1024];
    struct mountmgr_unix_drive *data = (struct mountmgr_unix_drive *)buffer;
    HANDLE manager = mount_manager();
    DWORD ret = WN_SUCCESS;

    if (manager == INVALID_HANDLE_VALUE) return ERROR_NO_NETWORK;
    memset( data, 0, sizeof(*data) );
    data->type = DRIVE_REMOTE;
    data->letter = letter;
    data->mount_point_offset = sizeof(*data);
    unix_folder( buffer + sizeof(*data), sizeof(buffer) - sizeof(*data), server, share );
    data->size = sizeof(*data) + strlen( buffer + sizeof(*data) ) + 1;
    if (!DeviceIoControl( manager, IOCTL_MOUNTMGR_DEFINE_UNIX_DRIVE, data, data->size, NULL, 0, NULL, NULL ))
        ret = GetLastError();
    CloseHandle( manager );
    TRACE( "%c: is %s: %lu\n", letter, buffer + sizeof(*data), ret );
    return ret;
}

DWORD drive_remove( WCHAR letter )
{
    struct mountmgr_unix_drive data = { 0 };
    HANDLE manager = mount_manager();
    DWORD ret = WN_SUCCESS;

    if (manager == INVALID_HANDLE_VALUE) return ERROR_NO_NETWORK;
    data.size = sizeof(data);
    data.type = DRIVE_NO_ROOT_DIR;
    data.letter = letter;
    if (!DeviceIoControl( manager, IOCTL_MOUNTMGR_DEFINE_UNIX_DRIVE, &data, sizeof(data), NULL, 0, NULL, NULL ))
        ret = GetLastError();
    CloseHandle( manager );
    return ret;
}

/**********************************************************************
 *          The shares of a computer
 */

static CRITICAL_SECTION shares_cs;
static CRITICAL_SECTION_DEBUG shares_cs_debug =
{
    0, 0, &shares_cs, { &shares_cs_debug.ProcessLocksList, &shares_cs_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)(__FILE__ ": shares_cs") }
};
static CRITICAL_SECTION shares_cs = { &shares_cs_debug, -1, 0, 0, 0, 0 };

static struct
{
    WCHAR server[64];
    struct share *shares;
    UINT count;
    ULONGLONG at;
} remembered[16];

static void forget_shares( const WCHAR *server )
{
    UINT i;

    EnterCriticalSection( &shares_cs );
    for (i = 0; i < ARRAY_SIZE(remembered); i++)
    {
        if (wcsicmp( remembered[i].server, server )) continue;
        free( remembered[i].shares );
        memset( &remembered[i], 0, sizeof(remembered[i]) );
    }
    LeaveCriticalSection( &shares_cs );
}

static BOOL recall_shares( const WCHAR *server, struct share **shares, UINT *count )
{
    BOOL ret = FALSE;
    UINT i;

    EnterCriticalSection( &shares_cs );
    for (i = 0; i < ARRAY_SIZE(remembered); i++)
    {
        if (wcsicmp( remembered[i].server, server ) || GetTickCount64() - remembered[i].at > 15000) continue;
        if ((*shares = malloc( (remembered[i].count + 1) * sizeof(**shares) )))
        {
            memcpy( *shares, remembered[i].shares, remembered[i].count * sizeof(**shares) );
            *count = remembered[i].count;
            ret = TRUE;
        }
        break;
    }
    LeaveCriticalSection( &shares_cs );
    return ret;
}

static void remember_shares( const WCHAR *server, const struct share *shares, UINT count )
{
    UINT i, slot = 0;

    EnterCriticalSection( &shares_cs );
    for (i = 0; i < ARRAY_SIZE(remembered); i++)
    {
        if (!wcsicmp( remembered[i].server, server )) { slot = i; break; }
        if (remembered[i].at < remembered[slot].at) slot = i;
    }
    free( remembered[slot].shares );
    memset( &remembered[slot], 0, sizeof(remembered[slot]) );
    if ((remembered[slot].shares = malloc( (count + 1) * sizeof(*shares) )))
    {
        memcpy( remembered[slot].shares, shares, count * sizeof(*shares) );
        remembered[slot].count = count;
        remembered[slot].at = GetTickCount64();
        lstrcpynW( remembered[slot].server, server, ARRAY_SIZE(remembered[slot].server) );
    }
    LeaveCriticalSection( &shares_cs );
}

/* the folders a computer shares: with the logon the user gave for it, or as
 * Windows comes to a computer nobody gave a password for */
DWORD list_shares( const WCHAR *server, struct share **shares, UINT *count )
{
    struct logon logon;
    struct share *all = NULL;
    char address[64];
    UINT total = 0, i, kept = 0;
    DWORD err;

    init();
    *shares = NULL;
    *count = 0;
    if (recall_shares( server, shares, count )) return WN_SUCCESS;
    if (!host_resolve( server, address, sizeof(address) )) return ERROR_BAD_NETPATH;

    if (host_get_logon( server, &logon ) || (saved_logon( server, &logon ) && (host_set_logon( server, &logon ), 1)))
    {
        err = smb_enum_shares( server, address, &logon, &all, &total );
    }
    else
    {
        DWORD size = ARRAY_SIZE(logon.user);

        /* the user's own name without a password, a guest, nobody */
        memset( &logon, 0, sizeof(logon) );
        if (!GetUserNameW( logon.user, &size )) wcscpy( logon.user, L"Guest" );
        err = smb_enum_shares( server, address, &logon, &all, &total );
        if (err == ERROR_LOGON_FAILURE || err == ERROR_ACCESS_DENIED)
        {
            wcscpy( logon.user, L"Guest" );
            err = smb_enum_shares( server, address, &logon, &all, &total );
        }
        if (err == ERROR_LOGON_FAILURE || err == ERROR_ACCESS_DENIED)
        {
            DWORD null_err = smb_enum_shares( server, address, NULL, &all, &total );
            if (!null_err) err = 0;
        }
        if (err == ERROR_LOGON_FAILURE) err = ERROR_ACCESS_DENIED;
    }
    SecureZeroMemory( &logon, sizeof(logon) );
    if (err) return err;

    /* folders, without those Windows hides (C$, ADMIN$) */
    for (i = 0; i < total; i++)
    {
        UINT len = wcslen( all[i].name );

        if ((all[i].type & 0xff) || (all[i].type & 0x80000000) || !len || all[i].name[len - 1] == '$') continue;
        all[kept++] = all[i];
    }
    remember_shares( server, all, kept );
    *shares = all;
    *count = kept;
    return WN_SUCCESS;
}

static DWORD mount_share( const WCHAR *server, const WCHAR *share )
{
    char address[64];

    /* the address the neighbours' answers gave, which the host may not find by itself */
    if (host_resolve( server, address, sizeof(address) )) host_tell_address( server, address );
    return error_of_errno( host_command( "MOUNT", server, share, NULL, NULL, NULL, 0 ) );
}

static void split_user( const WCHAR *name, struct logon *logon )
{
    const WCHAR *sep;

    if ((sep = wcschr( name, '\\' )))
    {
        lstrcpynW( logon->domain, name, min( ARRAY_SIZE(logon->domain), (UINT)(sep - name) + 1 ) );
        lstrcpynW( logon->user, sep + 1, ARRAY_SIZE(logon->user) );
    }
    else
    {
        logon->domain[0] = 0;
        lstrcpynW( logon->user, name, ARRAY_SIZE(logon->user) );
    }
}

/* \\server\share is open (and is a drive, when a letter is given); the user
 * is asked for a name and password when the computer wants them */
DWORD connect_resource( HWND owner, const WCHAR *remote, const WCHAR *local, const WCHAR *user,
                        const WCHAR *password, DWORD flags )
{
    WCHAR server[64], share[84], letter = 0;
    struct logon logon;
    struct share *shares;
    BOOL asked = FALSE, save = FALSE;
    UINT count;
    DWORD err;

    init();
    if (!split_unc( remote, server, ARRAY_SIZE(server), share, ARRAY_SIZE(share), NULL )) return WN_BAD_NETNAME;
    if (local && local[0])
    {
        letter = towlower( local[0] );
        if (letter < 'a' || letter > 'z' || local[1] != ':') return WN_BAD_LOCALNAME;
        if (!share[0]) return WN_BAD_NETNAME;
        if (GetLogicalDrives() & (1u << (letter - 'a'))) return WN_ALREADY_CONNECTED;
    }
    memset( &logon, 0, sizeof(logon) );
    if (user && user[0])
    {
        split_user( user, &logon );
        if (password) lstrcpynW( logon.password, password, ARRAY_SIZE(logon.password) );
        host_set_logon( server, &logon );
    }
    else if (!host_get_logon( server, &logon ) && saved_logon( server, &logon ))
        host_set_logon( server, &logon );

    if ((flags & (CONNECT_INTERACTIVE | CONNECT_PROMPT)) == (CONNECT_INTERACTIVE | CONNECT_PROMPT))
    {
        if (!ask_logon( owner, server, &logon, FALSE, &save )) return WN_CANCEL;
        host_set_logon( server, &logon );
        asked = TRUE;
    }
    for (;;)
    {
        if (share[0] && wcsicmp( share, L"IPC$" )) err = mount_share( server, share );
        else
        {
            forget_shares( server );
            err = list_shares( server, &shares, &count );
            free( shares );
        }
        if (!err) break;
        if ((err != ERROR_ACCESS_DENIED && err != ERROR_LOGON_FAILURE) || !(flags & CONNECT_INTERACTIVE)) break;
        if (!ask_logon( owner, server, &logon, asked, &save ))
        {
            err = WN_CANCEL;
            break;
        }
        host_set_logon( server, &logon );
        asked = TRUE;
    }
    if (!err && asked && save) save_logon( server, &logon );
    SecureZeroMemory( &logon, sizeof(logon) );
    if (!err && letter) err = drive_define( letter, server, share );
    return err;
}

/**********************************************************************
 *          What mpr.dll asks
 */

struct entry
{
    WCHAR remote[MAX_PATH];
    WCHAR comment[260];
    DWORD display;
};

struct enumerator
{
    struct entry *entries;
    UINT count;
    UINT pos;
};

DWORD WINAPI NPGetCaps( DWORD index )
{
    switch (index)
    {
    case WNNC_SPEC_VERSION:   return WNNC_SPEC_VERSION51;
    case WNNC_NET_TYPE:       return WNNC_NET_LANMAN;
    case WNNC_DRIVER_VERSION: return 0x0a00;
    case WNNC_USER:           return WNNC_USR_GETUSER;
    case WNNC_CONNECTION:     return WNNC_CON_ADDCONNECTION | WNNC_CON_ADDCONNECTION3 | WNNC_CON_CANCELCONNECTION |
                                     WNNC_CON_GETCONNECTIONS;
    case WNNC_DIALOG:         return WNNC_DLG_GETRESOURCEPARENT | WNNC_DLG_GETRESOURCEINFORMATION;
    case WNNC_ENUMERATION:    return WNNC_ENUM_GLOBAL | WNNC_ENUM_LOCAL | WNNC_ENUM_CONTEXT;
    case WNNC_START:          return 1;
    }
    return 0;
}

DWORD WINAPI NPOpenEnum( DWORD scope, DWORD type, DWORD usage, NETRESOURCEW *resource, HANDLE *handle )
{
    struct enumerator *enumerator;
    WCHAR server[64], share[84];
    UINT i;

    TRACE( "scope %#lx type %#lx usage %#lx %s\n", scope, type, usage,
           debugstr_w( resource ? resource->lpRemoteName : NULL ) );

    init();
    if (!handle) return WN_BAD_POINTER;
    if (scope != RESOURCE_GLOBALNET && scope != RESOURCE_CONTEXT) return WN_NOT_SUPPORTED;
    if (!(enumerator = calloc( 1, sizeof(*enumerator) ))) return WN_OUT_OF_MEMORY;

    if (scope == RESOURCE_CONTEXT || !resource || !resource->lpRemoteName || !resource->lpRemoteName[0] ||
        !wcsicmp( resource->lpRemoteName, provider_name ))
    {
        struct computer *computers;
        UINT count = discover_computers( &computers, FALSE );

        if ((enumerator->entries = calloc( count + 1, sizeof(*enumerator->entries) )))
        {
            for (i = 0; i < count; i++)
            {
                swprintf( enumerator->entries[i].remote, MAX_PATH, L"\\\\%s", computers[i].name );
                wcscpy( enumerator->entries[i].comment, computers[i].comment );
                enumerator->entries[i].display = RESOURCEDISPLAYTYPE_SERVER;
            }
            enumerator->count = count;
        }
        free( computers );
    }
    else if (split_unc( resource->lpRemoteName, server, ARRAY_SIZE(server), share, ARRAY_SIZE(share), NULL ))
    {
        struct share *shares;
        UINT count;
        DWORD err;

        if (share[0])
        {
            free( enumerator );
            return WN_NOT_CONTAINER;
        }
        if ((err = list_shares( server, &shares, &count )))
        {
            free( enumerator );
            return err;
        }
        if (type && !(type & RESOURCETYPE_DISK)) count = 0;
        if ((enumerator->entries = calloc( count + 1, sizeof(*enumerator->entries) )))
        {
            for (i = 0; i < count; i++)
            {
                swprintf( enumerator->entries[i].remote, MAX_PATH, L"\\\\%s\\%s", server, shares[i].name );
                lstrcpynW( enumerator->entries[i].comment, shares[i].remark, 260 );
                enumerator->entries[i].display = RESOURCEDISPLAYTYPE_SHARE;
            }
            enumerator->count = count;
        }
        free( shares );
    }
    else
    {
        free( enumerator );
        return WN_BAD_NETNAME;
    }
    *handle = enumerator;
    return WN_SUCCESS;
}

static void fill_resource( NETRESOURCEW *resource, WCHAR **strings, const WCHAR *remote, const WCHAR *comment,
                           DWORD display )
{
    UINT len;

    memset( resource, 0, sizeof(*resource) );
    resource->dwScope = RESOURCE_GLOBALNET;
    resource->dwDisplayType = display;
    if (display == RESOURCEDISPLAYTYPE_SHARE)
    {
        resource->dwType = RESOURCETYPE_DISK;
        resource->dwUsage = RESOURCEUSAGE_CONNECTABLE;
    }
    else
    {
        resource->dwType = RESOURCETYPE_ANY;
        resource->dwUsage = RESOURCEUSAGE_CONTAINER;
    }
    if (remote)
    {
        len = wcslen( remote ) + 1;
        *strings -= len;
        memcpy( *strings, remote, len * sizeof(WCHAR) );
        resource->lpRemoteName = *strings;
    }
    if (comment && comment[0])
    {
        len = wcslen( comment ) + 1;
        *strings -= len;
        memcpy( *strings, comment, len * sizeof(WCHAR) );
        resource->lpComment = *strings;
    }
    len = wcslen( provider_name ) + 1;
    *strings -= len;
    memcpy( *strings, provider_name, len * sizeof(WCHAR) );
    resource->lpProvider = *strings;
}

static UINT resource_size( const WCHAR *remote, const WCHAR *comment )
{
    UINT chars = wcslen( provider_name ) + 1;

    if (remote) chars += wcslen( remote ) + 1;
    if (comment && comment[0]) chars += wcslen( comment ) + 1;
    return sizeof(NETRESOURCEW) + chars * sizeof(WCHAR);
}

DWORD WINAPI NPEnumResource( HANDLE handle, DWORD *count, void *buffer, DWORD *size )
{
    struct enumerator *enumerator = handle;
    NETRESOURCEW *resources = buffer;
    WCHAR *strings;
    DWORD wanted, done = 0, used = 0;

    if (!enumerator || !count || !size || !buffer) return WN_BAD_POINTER;
    if (enumerator->pos >= enumerator->count) return WN_NO_MORE_ENTRIES;
    wanted = *count;
    strings = (WCHAR *)((BYTE *)buffer + (*size & ~1));
    while (done < wanted && enumerator->pos < enumerator->count)
    {
        const struct entry *entry = &enumerator->entries[enumerator->pos];
        UINT need = resource_size( entry->remote, entry->comment );

        if (used + need > *size)
        {
            if (done) break;
            *size = need;
            return WN_MORE_DATA;
        }
        fill_resource( &resources[done], &strings, entry->remote, entry->comment, entry->display );
        used += need;
        done++;
        enumerator->pos++;
    }
    *count = done;
    return WN_SUCCESS;
}

DWORD WINAPI NPCloseEnum( HANDLE handle )
{
    struct enumerator *enumerator = handle;

    if (!enumerator) return WN_BAD_HANDLE;
    free( enumerator->entries );
    free( enumerator );
    return WN_SUCCESS;
}

DWORD WINAPI NPGetResourceInformation( NETRESOURCEW *resource, void *buffer, DWORD *size, WCHAR **system )
{
    WCHAR server[64], share[84], remote[MAX_PATH], *strings;
    const WCHAR *rest = NULL;
    char address[64];
    UINT need;

    TRACE( "%s\n", debugstr_w( resource ? resource->lpRemoteName : NULL ) );

    init();
    if (!resource || !size) return WN_BAD_POINTER;
    if (!split_unc( resource->lpRemoteName, server, ARRAY_SIZE(server), share, ARRAY_SIZE(share), &rest ))
        return WN_BAD_NETNAME;
    if (!host_resolve( server, address, sizeof(address) )) return WN_BAD_NETNAME;
    if (share[0]) swprintf( remote, ARRAY_SIZE(remote), L"\\\\%s\\%s", server, share );
    else swprintf( remote, ARRAY_SIZE(remote), L"\\\\%s", server );

    need = resource_size( remote, NULL ) + (rest ? (wcslen( rest ) + 1) * sizeof(WCHAR) : 0);
    if (!buffer || *size < need)
    {
        *size = need;
        return WN_MORE_DATA;
    }
    strings = (WCHAR *)((BYTE *)buffer + (*size & ~1));
    fill_resource( buffer, &strings, remote, NULL, share[0] ? RESOURCEDISPLAYTYPE_SHARE : RESOURCEDISPLAYTYPE_SERVER );
    if (system)
    {
        *system = NULL;
        if (rest)
        {
            strings -= wcslen( rest ) + 1;
            wcscpy( strings, rest );
            *system = strings;
        }
    }
    return WN_SUCCESS;
}

DWORD WINAPI NPGetResourceParent( NETRESOURCEW *resource, void *buffer, DWORD *size )
{
    WCHAR server[64], share[84], remote[MAX_PATH], *strings;
    UINT need;

    if (!resource || !size) return WN_BAD_POINTER;
    if (!split_unc( resource->lpRemoteName, server, ARRAY_SIZE(server), share, ARRAY_SIZE(share), NULL ))
        return WN_BAD_NETNAME;
    swprintf( remote, ARRAY_SIZE(remote), L"\\\\%s", server );
    need = resource_size( share[0] ? remote : NULL, NULL );
    if (!buffer || *size < need)
    {
        *size = need;
        return WN_MORE_DATA;
    }
    strings = (WCHAR *)((BYTE *)buffer + (*size & ~1));
    if (share[0]) fill_resource( buffer, &strings, remote, NULL, RESOURCEDISPLAYTYPE_SERVER );
    else fill_resource( buffer, &strings, NULL, NULL, RESOURCEDISPLAYTYPE_NETWORK );
    return WN_SUCCESS;
}

DWORD WINAPI NPAddConnection3( HWND owner, NETRESOURCEW *resource, WCHAR *password, WCHAR *user, DWORD flags )
{
    TRACE( "%s as %s, drive %s, flags %#lx\n", debugstr_w( resource ? resource->lpRemoteName : NULL ),
           debugstr_w( user ), debugstr_w( resource ? resource->lpLocalName : NULL ), flags );

    if (!resource || !resource->lpRemoteName) return WN_BAD_NETNAME;
    if (resource->dwType == RESOURCETYPE_PRINT) return WN_BAD_DEV_TYPE;
    return connect_resource( owner, resource->lpRemoteName, resource->lpLocalName, user, password, flags );
}

DWORD WINAPI NPAddConnection( NETRESOURCEW *resource, WCHAR *password, WCHAR *user )
{
    return NPAddConnection3( NULL, resource, password, user, 0 );
}

DWORD WINAPI NPCancelConnection( WCHAR *name, BOOL force )
{
    WCHAR server[64], share[84], remote[MAX_PATH];

    TRACE( "%s\n", debugstr_w( name ) );

    if (!name) return WN_BAD_POINTER;
    if (name[0] && name[1] == ':')
    {
        if (!drive_remote( name[0], remote, ARRAY_SIZE(remote) )) return WN_NOT_CONNECTED;
        return drive_remove( name[0] );
    }
    if (!split_unc( name, server, ARRAY_SIZE(server), share, ARRAY_SIZE(share), NULL )) return WN_BAD_NETNAME;
    host_command( "UMOUNT", server, share, NULL, NULL, NULL, 0 );
    return WN_SUCCESS;
}

DWORD WINAPI NPGetConnection( WCHAR *local, WCHAR *remote, DWORD *size )
{
    WCHAR path[MAX_PATH];
    UINT len;

    if (!local || !size) return WN_BAD_POINTER;
    if (!local[0] || local[1] != ':' || !drive_remote( local[0], path, ARRAY_SIZE(path) )) return WN_NOT_CONNECTED;
    len = wcslen( path ) + 1;
    if (!remote || *size < len)
    {
        *size = len;
        return WN_MORE_DATA;
    }
    wcscpy( remote, path );
    return WN_SUCCESS;
}

DWORD WINAPI NPGetUser( WCHAR *name, WCHAR *user, DWORD *size )
{
    WCHAR server[64], remote[MAX_PATH], text[UNLEN + 260];
    struct logon logon;
    DWORD len = UNLEN + 1;

    if (!size) return WN_BAD_POINTER;
    text[0] = 0;
    if (name && name[0] && name[1] == ':' && drive_remote( name[0], remote, ARRAY_SIZE(remote) )) name = remote;
    if (name && split_unc( name, server, ARRAY_SIZE(server), NULL, 0, NULL ) && host_get_logon( server, &logon ))
    {
        if (logon.domain[0]) swprintf( text, ARRAY_SIZE(text), L"%s\\%s", logon.domain, logon.user );
        else wcscpy( text, logon.user );
        SecureZeroMemory( &logon, sizeof(logon) );
    }
    if (!text[0]) GetUserNameW( text, &len );
    len = wcslen( text ) + 1;
    if (!user || *size < len)
    {
        *size = len;
        return WN_MORE_DATA;
    }
    wcscpy( user, text );
    return WN_SUCCESS;
}

/* rundll32 ntlanman.dll,RestoreConnections: at logon the network drives are
 * drives again, and the host learns the passwords the user had kept */
void WINAPI RestoreConnectionsW( HWND hwnd, HINSTANCE instance, WCHAR *cmdline, int show )
{
    WCHAR name[16], remote[MAX_PATH], server[64], share[84];
    struct logon logon;
    DWORD index, size;
    HKEY network, key;

    init();
    memset( &logon, 0, sizeof(logon) );
    push_saved_logons();
    if (RegOpenKeyExW( HKEY_CURRENT_USER, L"Network", 0, KEY_READ, &network )) return;
    for (index = 0; ; index++)
    {
        size = ARRAY_SIZE(name);
        if (RegEnumKeyExW( network, index, name, &size, NULL, NULL, NULL, NULL )) break;
        if (!name[0] || name[1] || RegOpenKeyExW( network, name, 0, KEY_READ, &key )) continue;
        size = sizeof(remote);
        if (!RegGetValueW( key, NULL, L"RemotePath", RRF_RT_REG_SZ, NULL, remote, &size ) &&
            split_unc( remote, server, ARRAY_SIZE(server), share, ARRAY_SIZE(share), NULL ) && share[0])
        {
            if (!host_get_logon( server, &logon ) && saved_logon( server, &logon )) host_set_logon( server, &logon );
            if (!(GetLogicalDrives() & (1u << (towlower( name[0] ) - 'a'))))
                drive_define( towlower( name[0] ), server, share );
        }
        RegCloseKey( key );
    }
    RegCloseKey( network );
    SecureZeroMemory( &logon, sizeof(logon) );
}

BOOL WINAPI DllMain( HINSTANCE instance, DWORD reason, void *reserved )
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        ntlanman_instance = instance;
        DisableThreadLibraryCalls( instance );
    }
    return TRUE;
}
