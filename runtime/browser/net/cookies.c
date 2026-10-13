/*
 * Arctic web engine: cookies
 *
 * Kept in memory, and those that outlive the session - Expires, Max-Age -
 * on the disk too, in %LOCALAPPDATA%\Arctic\Internet Explorer\cookies.txt,
 * written again whenever one of them changes: a site asks its questions
 * once, as in any browser.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

#include "arctichtml_private.h"
#include "net.h"

#define MAX_COOKIES 3000

struct cookie
{
    char  *name, *value;
    char   domain[256];
    char   path[256];
    BOOL   host_only, secure;
    time_t expires;         /* 0: when the session ends */
};

static struct cookie *jar;
static unsigned int jar_count, jar_capacity;
static SRWLOCK jar_lock = SRWLOCK_INIT;
static INIT_ONCE jar_loaded = INIT_ONCE_STATIC_INIT;

static void cookie_free( struct cookie *cookie );

/* --- on the disk --- */

static BOOL jar_path( WCHAR *path, DWORD size, BOOL create )
{
    WCHAR base[MAX_PATH];
    DWORD len = GetEnvironmentVariableW( L"LOCALAPPDATA", base, ARRAY_SIZE(base) );

    if (!len || len >= ARRAY_SIZE(base)) return FALSE;
    if (create)
    {
        swprintf( path, size, L"%s\\Arctic", base );
        CreateDirectoryW( path, NULL );
        swprintf( path, size, L"%s\\Arctic\\Internet Explorer", base );
        CreateDirectoryW( path, NULL );
    }
    swprintf( path, size, L"%s\\Arctic\\Internet Explorer\\cookies.txt", base );
    return TRUE;
}

/* a line: domain, host only, path, secure, expiry, name, value - by tabs */
static BOOL parse_line( char *line, struct cookie *cookie )
{
    char *fields[7], *next = line;

    for (int i = 0; i < 7; i++)
    {
        fields[i] = next;
        if (i < 6)
        {
            if (!(next = strchr( next, '\t' ))) return FALSE;
            *next++ = 0;
        }
    }
    memset( cookie, 0, sizeof(*cookie) );
    lstrcpynA( cookie->domain, fields[0], sizeof(cookie->domain) );
    cookie->host_only = fields[1][0] == '1';
    lstrcpynA( cookie->path, fields[2], sizeof(cookie->path) );
    cookie->secure = fields[3][0] == '1';
    cookie->expires = (time_t)_strtoi64( fields[4], NULL, 10 );
    cookie->name = _strdup( fields[5] );
    cookie->value = _strdup( fields[6] );
    if (!cookie->name || !cookie->value || cookie->expires <= time( NULL ))
    {
        free( cookie->name );
        free( cookie->value );
        return FALSE;
    }
    return TRUE;
}

static BOOL CALLBACK load_jar( INIT_ONCE *once, void *param, void **context )
{
    WCHAR path[MAX_PATH];
    HANDLE file;
    LARGE_INTEGER size;
    char *data, *line, *next;
    DWORD read;

    if (!jar_path( path, ARRAY_SIZE(path), FALSE )) return TRUE;
    file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                        OPEN_EXISTING, 0, NULL );
    if (file == INVALID_HANDLE_VALUE) return TRUE;
    if (GetFileSizeEx( file, &size ) && size.QuadPart < 16 * 1024 * 1024 &&
        (data = malloc( (size_t)size.QuadPart + 1 )))
    {
        if (ReadFile( file, data, (DWORD)size.QuadPart, &read, NULL ))
        {
            data[read] = 0;
            for (line = data; line && *line; line = next)
            {
                struct cookie cookie;

                if ((next = strchr( line, '\n' ))) *next++ = 0;
                if (next && next - line >= 2 && next[-2] == '\r') next[-2] = 0;
                if (jar_count >= MAX_COOKIES || !parse_line( line, &cookie )) continue;
                if (jar_count == jar_capacity)
                {
                    unsigned int capacity = jar_capacity ? jar_capacity * 2 : 64;
                    struct cookie *grown = realloc( jar, capacity * sizeof(*grown) );

                    if (!grown) { cookie_free( &cookie ); break; }
                    jar = grown;
                    jar_capacity = capacity;
                }
                jar[jar_count++] = cookie;
            }
        }
        free( data );
    }
    CloseHandle( file );
    return TRUE;
}

/* the cookies that outlive the session, written whole: called with the jar locked */
static void save_jar(void)
{
    WCHAR path[MAX_PATH], temp[MAX_PATH + 8];
    time_t now = time( NULL );
    size_t size = 4096, len = 0;
    char *text = malloc( size );
    HANDLE file;
    DWORD written;

    if (!text || !jar_path( path, ARRAY_SIZE(path), TRUE )) { free( text ); return; }
    for (unsigned int i = 0; i < jar_count; i++)
    {
        const struct cookie *cookie = &jar[i];
        size_t need;

        if (!cookie->expires || cookie->expires <= now) continue;
        /* a tab or a line break in them would break the file: such a one stays in memory */
        if (strpbrk( cookie->name, "\t\r\n" ) || strpbrk( cookie->value, "\t\r\n" )) continue;
        need = strlen( cookie->domain ) + strlen( cookie->path ) + strlen( cookie->name ) +
               strlen( cookie->value ) + 40;
        if (len + need > size)
        {
            char *grown = realloc( text, size = max( size * 2, len + need ) );

            if (!grown) { free( text ); return; }
            text = grown;
        }
        len += sprintf( text + len, "%s\t%d\t%s\t%d\t%I64d\t%s\t%s\r\n", cookie->domain, cookie->host_only,
                        cookie->path, cookie->secure, (__int64)cookie->expires, cookie->name, cookie->value );
    }
    swprintf( temp, ARRAY_SIZE(temp), L"%s.new", path );
    file = CreateFileW( temp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
    if (file != INVALID_HANDLE_VALUE)
    {
        BOOL ok = WriteFile( file, text, (DWORD)len, &written, NULL ) && written == len;

        CloseHandle( file );
        if (!ok || !MoveFileExW( temp, path, MOVEFILE_REPLACE_EXISTING )) DeleteFileW( temp );
    }
    free( text );
}

/* Expires=Wed, 21 Oct 2027 07:28:00 GMT, and the older forms with dashes and two-digit years */
static time_t parse_expires( const char *s, const char *end )
{
    static const char months[] = "janfebmaraprmayjunjulaugsepoctnovdec";
    struct tm tm = { 0 };
    int day = -1, month = -1, year = -1, hour = -1, minute = 0, second = 0;

    while (s < end)
    {
        const char *word = s;
        size_t len;

        while (s < end && *s != ' ' && *s != '-' && *s != ',') s++;
        len = s - word;
        if (len)
        {
            if (strchr( "0123456789", *word ) && memchr( word, ':', len ) && hour < 0)
                sscanf( word, "%d:%d:%d", &hour, &minute, &second );
            else if (*word >= '0' && *word <= '9')
            {
                int number = atoi( word );

                if (day < 0 && len <= 2) day = number;
                else if (year < 0) year = len <= 2 ? number + (number < 70 ? 2000 : 1900) : number;
            }
            else if (len >= 3 && month < 0)
            {
                for (int m = 0; m < 12; m++)
                    if (!_strnicmp( word, months + m * 3, 3 )) { month = m; break; }
            }
        }
        while (s < end && (*s == ' ' || *s == '-' || *s == ',')) s++;
    }
    if (day < 1 || month < 0 || year < 1970) return 0;
    tm.tm_mday = day;
    tm.tm_mon = month;
    tm.tm_year = year - 1900;
    tm.tm_hour = max( hour, 0 );
    tm.tm_min = minute;
    tm.tm_sec = second;
    return _mkgmtime( &tm );
}

static BOOL domain_matches( const char *host, const char *domain, BOOL host_only )
{
    size_t host_len = strlen( host ), domain_len = strlen( domain );

    if (!_stricmp( host, domain )) return TRUE;
    if (host_only || host_len <= domain_len) return FALSE;
    return host[host_len - domain_len - 1] == '.' && !_stricmp( host + host_len - domain_len, domain );
}

static BOOL path_matches( const char *request, size_t request_len, const char *path )
{
    size_t len = strlen( path );

    if (len > request_len || strncmp( request, path, len )) return FALSE;
    return len == request_len || path[len - 1] == '/' || request[len] == '/' || request[len] == '?';
}

static void cookie_free( struct cookie *cookie )
{
    free( cookie->name );
    free( cookie->value );
}

static char *copy_trimmed( const char *s, size_t len )
{
    char *copy;

    while (len && (*s == ' ' || *s == '\t')) s++, len--;
    while (len && (s[len - 1] == ' ' || s[len - 1] == '\t')) len--;
    if (!(copy = malloc( len + 1 ))) return NULL;
    memcpy( copy, s, len );
    copy[len] = 0;
    return copy;
}

void cookies_set( const struct url_parts *url, const char *header, size_t len )
{
    struct cookie cookie = { 0 };
    const char *end = header + len, *part = header, *eq;
    size_t pair_len = strcspn( header, ";" );
    BOOL remove = FALSE, max_age = FALSE, persistent = FALSE;

    InitOnceExecuteOnce( &jar_loaded, load_jar, NULL, NULL );

    if (pair_len > len) pair_len = len;
    if (!(eq = memchr( header, '=', pair_len ))) return;
    cookie.name = copy_trimmed( header, eq - header );
    cookie.value = copy_trimmed( eq + 1, header + pair_len - eq - 1 );
    if (!cookie.name || !cookie.value || !cookie.name[0]) goto fail;

    lstrcpynA( cookie.domain, url->host, sizeof(cookie.domain) );
    cookie.host_only = TRUE;
    /* the default path: the folder of the request */
    {
        size_t path_len = strcspn( url->path, "?" );

        while (path_len > 1 && url->path[path_len - 1] != '/') path_len--;
        if (path_len > 1) path_len--;
        lstrcpynA( cookie.path, url->path, (int)min( path_len + 1, sizeof(cookie.path) ) );
        if (!cookie.path[0]) strcpy( cookie.path, "/" );
    }

    for (part = header + pair_len; part < end; )
    {
        const char *attr, *attr_end, *value;

        part++;
        attr = part;
        attr_end = attr + strcspn( attr, ";" );
        if (attr_end > end) attr_end = end;
        while (attr < attr_end && *attr == ' ') attr++;
        value = memchr( attr, '=', attr_end - attr );
        if (value) value++;

        if (!_strnicmp( attr, "domain=", 7 ) && value)
        {
            char *domain = copy_trimmed( value, attr_end - value );

            if (domain)
            {
                const char *d = domain[0] == '.' ? domain + 1 : domain;

                /* a site sets cookies for itself and its parents, never for another */
                if (!domain_matches( url->host, d, FALSE ) || !strchr( d, '.' ))
                {
                    free( domain );
                    goto fail;
                }
                lstrcpynA( cookie.domain, d, sizeof(cookie.domain) );
                cookie.host_only = FALSE;
                free( domain );
            }
        }
        else if (!_strnicmp( attr, "path=", 5 ) && value && *value == '/')
            lstrcpynA( cookie.path, value, (int)min( (size_t)(attr_end - value) + 1, sizeof(cookie.path) ) );
        else if (!_strnicmp( attr, "secure", 6 ))
            cookie.secure = TRUE;
        else if (!_strnicmp( attr, "max-age=", 8 ) && value)
        {
            long seconds = strtol( value, NULL, 10 );

            /* Max-Age wins over Expires, wherever they stand */
            max_age = TRUE;
            remove = seconds <= 0;
            if (seconds > 0) cookie.expires = time( NULL ) + seconds;
        }
        else if (!_strnicmp( attr, "expires=", 8 ) && value && !max_age)
        {
            time_t expires = parse_expires( value, attr_end );

            if (expires && expires <= time( NULL )) remove = TRUE;
            else if (expires) cookie.expires = expires;
        }
        part = attr_end;
    }

    AcquireSRWLockExclusive( &jar_lock );
    persistent = cookie.expires != 0;
    for (unsigned int i = 0; i < jar_count; i++)
    {
        if (strcmp( jar[i].name, cookie.name ) || _stricmp( jar[i].domain, cookie.domain ) ||
            strcmp( jar[i].path, cookie.path ))
            continue;
        /* the same again changes nothing on the disk */
        if (!remove && jar[i].expires && persistent && !strcmp( jar[i].value, cookie.value ) &&
            llabs( (long long)(jar[i].expires - cookie.expires) ) < 3600)
            persistent = FALSE;
        else if (jar[i].expires)
            persistent = TRUE;
        cookie_free( &jar[i] );
        jar[i] = jar[--jar_count];
        break;
    }
    if (!remove && jar_count < MAX_COOKIES)
    {
        if (jar_count == jar_capacity)
        {
            unsigned int capacity = jar_capacity ? jar_capacity * 2 : 64;
            struct cookie *grown = realloc( jar, capacity * sizeof(*grown) );

            if (grown) { jar = grown; jar_capacity = capacity; }
        }
        if (jar_count < jar_capacity)
        {
            jar[jar_count++] = cookie;
            if (persistent) save_jar();
            ReleaseSRWLockExclusive( &jar_lock );
            return;
        }
    }
    if (persistent) save_jar();
    ReleaseSRWLockExclusive( &jar_lock );
fail:
    cookie_free( &cookie );
}

char *cookies_get( const struct url_parts *url )
{
    size_t size = 0, len = 0;
    char *header = NULL;
    time_t now = time( NULL );
    BOOL secure = !strcmp( url->scheme, "https" );

    InitOnceExecuteOnce( &jar_loaded, load_jar, NULL, NULL );
    AcquireSRWLockShared( &jar_lock );
    for (unsigned int i = 0; i < jar_count; i++)
    {
        const struct cookie *cookie = &jar[i];
        size_t need;

        if ((cookie->secure && !secure) || (cookie->expires && cookie->expires < now)) continue;
        if (!domain_matches( url->host, cookie->domain, cookie->host_only )) continue;
        if (!path_matches( url->path, url->path_len, cookie->path )) continue;

        need = strlen( cookie->name ) + strlen( cookie->value ) + 3;
        if (len + need + 1 > size)
        {
            char *grown = realloc( header, size = max( size * 2, len + need + 256 ) );

            if (!grown) break;
            header = grown;
        }
        len += sprintf( header + len, "%s%s=%s", len ? "; " : "", cookie->name, cookie->value );
    }
    ReleaseSRWLockShared( &jar_lock );
    return header;
}
