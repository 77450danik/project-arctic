/*
 * Arctic web engine: cookies
 *
 * Kept in memory for the session: enough for the sites that send a browser
 * through a redirect with a cookie and back. On the disk, with the
 * expiry and the limits of RFC 6265, they go with stage 3.
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
    BOOL remove = FALSE;

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

            if (seconds <= 0) remove = TRUE;
            else cookie.expires = time( NULL ) + seconds;
        }
        part = attr_end;
    }

    AcquireSRWLockExclusive( &jar_lock );
    for (unsigned int i = 0; i < jar_count; i++)
    {
        if (strcmp( jar[i].name, cookie.name ) || _stricmp( jar[i].domain, cookie.domain ) ||
            strcmp( jar[i].path, cookie.path ))
            continue;
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
            ReleaseSRWLockExclusive( &jar_lock );
            return;
        }
    }
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
