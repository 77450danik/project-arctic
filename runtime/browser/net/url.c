/*
 * Arctic web engine: addresses
 *
 * What a page names - "img/a.png", "/x", "//host/y", "?q", "https://..." - is
 * resolved against the address of the page as RFC 3986 says (section 5.2),
 * dot segments removed. What goes out is ASCII: a host of letters other than
 * Latin goes through IdnToAscii, which every Windows has, and a byte above
 * 127 or a space in a path or a query is percent-encoded, as the URL
 * standard does for UTF-8 pages.
 *
 * Addresses are UTF-8 in the engine, as lexbor gives them.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "arctichtml_private.h"
#include "net.h"

#include <shlwapi.h>

static BOOL is_scheme_char( char c, BOOL first )
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return TRUE;
    return !first && ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.');
}

/* the length of "scheme:" at the start, 0 when there is none */
static size_t scheme_length( const char *s, size_t len )
{
    size_t i;

    if (!len || !is_scheme_char( s[0], TRUE )) return 0;
    for (i = 1; i < len && is_scheme_char( s[i], FALSE ); i++);
    if (i >= len || s[i] != ':') return 0;
    if (i == 1) return 0;   /* C:\x is a drive, not a scheme */
    return i + 1;
}

BOOL url_parse( const char *url, struct url_parts *parts )
{
    size_t len = strlen( url ), scheme = scheme_length( url, len ), i;
    const char *rest, *authority, *authority_end, *host_start, *colon;

    memset( parts, 0, sizeof(*parts) );
    if (!scheme || scheme > sizeof(parts->scheme)) return FALSE;
    for (i = 0; i < scheme - 1; i++) parts->scheme[i] = (char)tolower( (unsigned char)url[i] );

    rest = url + scheme;
    if (rest[0] == '/' && rest[1] == '/')
    {
        authority = rest + 2;
        authority_end = authority + strcspn( authority, "/?#" );
        host_start = authority;
        /* user:password@ is never sent: it only names the host */
        for (const char *p = authority; p < authority_end; p++) if (*p == '@') host_start = p + 1;
        colon = NULL;
        if (*host_start == '[')     /* an IPv6 literal */
        {
            const char *close = memchr( host_start, ']', authority_end - host_start );

            if (!close) return FALSE;
            if (close + 1 < authority_end && close[1] == ':') colon = close + 1;
            i = close + 1 - host_start;
        }
        else
        {
            colon = memchr( host_start, ':', authority_end - host_start );
            i = (colon ? colon : authority_end) - host_start;
        }
        if (i >= sizeof(parts->host)) return FALSE;
        for (size_t k = 0; k < i; k++) parts->host[k] = (char)tolower( (unsigned char)host_start[k] );
        if (colon && colon + 1 < authority_end) parts->port = atoi( colon + 1 );
        rest = authority_end;
    }
    parts->path = rest[0] && rest[0] != '#' ? rest : "/";
    parts->path_len = strcspn( parts->path, "#" );
    if (!parts->path_len) { parts->path = "/"; parts->path_len = 1; }
    if (!parts->port)
    {
        if (!strcmp( parts->scheme, "https" )) parts->port = 443;
        else if (!strcmp( parts->scheme, "http" )) parts->port = 80;
    }
    return TRUE;
}

/* --- building an address --- */

struct out
{
    char  *data;
    size_t len, size;
};

static BOOL put( struct out *out, const char *s, size_t len )
{
    if (out->len + len + 1 > out->size)
    {
        size_t size = max( out->size * 2, out->len + len + 128 );
        char *grown = realloc( out->data, size );

        if (!grown) return FALSE;
        out->data = grown;
        out->size = size;
    }
    memcpy( out->data + out->len, s, len );
    out->len += len;
    out->data[out->len] = 0;
    return TRUE;
}

/* a path or a query as it may go out: no byte above 127, no space */
static void put_encoded( struct out *out, const char *s, size_t len )
{
    static const char hex[] = "0123456789ABCDEF";

    for (size_t i = 0; i < len; i++)
    {
        unsigned char c = s[i];

        if (c > 127 || c <= ' ' || c == '"' || c == '<' || c == '>' || c == '`' || c == '\\')
        {
            char escaped[3] = { '%', hex[c >> 4], hex[c & 15] };

            if (c == '\\') { put( out, "/", 1 ); continue; }    /* as browsers read C:\ in a file: URL */
            put( out, escaped, 3 );
        }
        else put( out, (const char *)&c, 1 );
    }
}

/* RFC 3986 5.2.4: "a/./b/../c" -> "a/c" */
static void remove_dot_segments( char *path )
{
    size_t len = strlen( path ), o = 0;
    char *copy = malloc( len + 1 ), *in = copy;

    if (!copy) return;
    memcpy( copy, path, len + 1 );
    while (*in)
    {
        if (!strncmp( in, "../", 3 )) in += 3;
        else if (!strncmp( in, "./", 2 )) in += 2;
        else if (!strncmp( in, "/./", 3 )) in += 2;
        else if (!strcmp( in, "/." )) in[1] = 0;        /* becomes "/" */
        else if (!strncmp( in, "/../", 4 ))
        {
            in += 3;                                    /* "/rest" */
            while (o && path[--o] != '/');              /* the last segment of the output goes */
        }
        else if (!strcmp( in, "/.." ))
        {
            in[1] = 0;                                  /* becomes "/" */
            while (o && path[--o] != '/');
        }
        else if (!strcmp( in, "." ) || !strcmp( in, ".." )) in += strlen( in );
        else do path[o++] = *in++; while (*in && *in != '/');
    }
    path[o] = 0;
    free( copy );
}

/* the host, in ASCII: a name in Cyrillic and the like goes through IDNA */
static BOOL put_host( struct out *out, const char *host, size_t len )
{
    WCHAR wide[256], ascii[256];
    char narrow[256];
    int n;
    BOOL plain = TRUE;

    for (size_t i = 0; i < len; i++) if ((unsigned char)host[i] > 127) plain = FALSE;
    if (plain)
    {
        for (size_t i = 0; i < len; i++)
        {
            char c = (char)tolower( (unsigned char)host[i] );

            put( out, &c, 1 );
        }
        return TRUE;
    }
    if (!(n = MultiByteToWideChar( CP_UTF8, 0, host, (int)len, wide, ARRAY_SIZE(wide) - 1 ))) return FALSE;
    if (!(n = IdnToAscii( 0, wide, n, ascii, ARRAY_SIZE(ascii) ))) return FALSE;
    n = WideCharToMultiByte( CP_ACP, 0, ascii, n, narrow, sizeof(narrow), NULL, NULL );
    return n && put( out, narrow, n );
}

/* "scheme://authority" of an absolute address, written out; scheme is the
 * length of "scheme:" with its colon */
static BOOL put_origin( struct out *out, const char *url, size_t scheme, const char **after )
{
    const char *authority = url + scheme + 2, *end = authority + strcspn( authority, "/?#" );
    const char *at = NULL, *host = authority, *colon;

    for (const char *p = authority; p < end; p++) if (*p == '@') at = p;
    if (at) host = at + 1;
    for (size_t i = 0; i < scheme; i++)
    {
        char c = (char)tolower( (unsigned char)url[i] );

        put( out, &c, 1 );
    }
    put( out, "//", 2 );
    if (at) put( out, authority, host - authority );
    colon = *host == '[' ? NULL : memchr( host, ':', end - host );
    if (!put_host( out, host, (colon ? colon : end) - host )) return FALSE;
    if (colon) put( out, colon, end - colon );
    *after = end;
    return TRUE;
}

char *url_resolve( const char *base, const char *ref, size_t ref_len )
{
    struct out out = { 0 };
    size_t scheme, base_len = base ? strlen( base ) : 0, base_scheme = base ? scheme_length( base, base_len ) : 0;
    const char *after, *base_path, *base_end, *query;
    char *path;

    /* what a page writes around an address does not belong to it */
    while (ref_len && (unsigned char)*ref <= ' ') ref++, ref_len--;
    while (ref_len && (unsigned char)ref[ref_len - 1] <= ' ') ref_len--;

    if ((scheme = scheme_length( ref, ref_len )))
    {
        /* an absolute address: http://, https://, file://, data:, about:... */
        if (ref_len > scheme + 1 && ref[scheme] == '/' && ref[scheme + 1] == '/')
        {
            char *copy = malloc( ref_len + 1 );

            if (!copy) return NULL;
            memcpy( copy, ref, ref_len );
            copy[ref_len] = 0;
            if (!put_origin( &out, copy, scheme, &after ))
            {
                free( copy );
                free( out.data );
                return NULL;
            }
            put_encoded( &out, after, copy + ref_len - after );
            free( copy );
        }
        else put( &out, ref, ref_len );     /* data:, about:, mailto: as they are */
        return out.data;
    }
    if (!base_scheme || base_len < base_scheme + 2 || base[base_scheme] != '/' || base[base_scheme + 1] != '/')
        return NULL;    /* nothing to be relative to */

    if (ref_len >= 2 && ref[0] == '/' && ref[1] == '/')
    {
        /* //host/path: the scheme of the page */
        char *copy = malloc( base_scheme + ref_len + 1 );

        if (!copy) return NULL;
        memcpy( copy, base, base_scheme );
        memcpy( copy + base_scheme, ref, ref_len );
        copy[base_scheme + ref_len] = 0;
        if (put_origin( &out, copy, base_scheme, &after )) put_encoded( &out, after, strlen( after ) );
        free( copy );
        return out.data;
    }

    /* the origin of the page, then a path made from both */
    base_path = base + base_scheme + 2;
    base_path += strcspn( base_path, "/?#" );
    put( &out, base, base_path - base );
    base_end = base_path + strcspn( base_path, "#" );

    if (!ref_len || ref[0] == '#')
    {
        put( &out, base_path, base_end - base_path );
        put_encoded( &out, ref, ref_len );
        return out.data;
    }
    if (ref[0] == '?')
    {
        query = base_path + strcspn( base_path, "?#" );
        put( &out, base_path, query - base_path );
        put_encoded( &out, ref, ref_len );
        return out.data;
    }

    if (!(path = malloc( (base_end - base_path) + ref_len + 2 ))) return out.data;
    if (ref[0] == '/')
    {
        memcpy( path, ref, ref_len );
        path[ref_len] = 0;
    }
    else
    {
        /* merge: the page's folder, then the reference */
        size_t folder = strcspn( base_path, "?#" );

        while (folder && base_path[folder - 1] != '/') folder--;
        if (!folder) { path[0] = '/'; folder = 1; }
        else memcpy( path, base_path, folder );
        memcpy( path + folder, ref, ref_len );
        path[folder + ref_len] = 0;
    }
    {
        /* the dot segments of the path only: a query keeps its dots */
        char *split = strpbrk( path, "?#" );

        if (split)
        {
            char saved = *split;
            size_t path_len;

            *split = 0;
            remove_dot_segments( path );
            path_len = strlen( path );
            *split = saved;
            if (path + path_len != split) memmove( path + path_len, split, strlen( split ) + 1 );
        }
        else remove_dot_segments( path );
    }
    if (!path[0]) strcpy( path, "/" );
    put_encoded( &out, path, strlen( path ) );
    free( path );
    return out.data;
}

/* what the user typed in the address bar, as an address */
char *url_from_typed( const WCHAR *typed )
{
    WCHAR file_url[2084];
    DWORD len = ARRAY_SIZE(file_url);
    int n;
    char *utf8, *url;

    while (*typed == ' ') typed++;
    /* C:\x.html and \\? are files: the file: address of them */
    if (((typed[0] >= 'a' && typed[0] <= 'z') || (typed[0] >= 'A' && typed[0] <= 'Z')) && typed[1] == ':' &&
        (typed[2] == '\\' || typed[2] == '/'))
    {
        if (FAILED(UrlCreateFromPathW( typed, file_url, &len, 0 ))) return NULL;
        typed = file_url;
    }
    n = WideCharToMultiByte( CP_UTF8, 0, typed, -1, NULL, 0, NULL, NULL );
    if (!(utf8 = malloc( n ))) return NULL;
    WideCharToMultiByte( CP_UTF8, 0, typed, -1, utf8, n, NULL, NULL );
    url = url_resolve( NULL, utf8, strlen( utf8 ) );
    free( utf8 );
    return url;
}
