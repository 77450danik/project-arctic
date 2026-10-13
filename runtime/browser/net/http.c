/*
 * Arctic web engine: fetching
 *
 * HTTP/1.1 over Winsock, TLS through Schannel (tls.c): what every Windows
 * has, so the engine runs on any. Connections are kept for the next request to\n * the same host; HTTP/2 and compression come with stage 6, so no\n * Accept-Encoding goes out and servers send the body as it is.
 *
 * The User-Agent says what the engine is and does not pretend to be
 * Chrome: a site sends a browser it does not know the plain page, which is
 * what an engine without JavaScript yet can show.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include "arctichtml_private.h"
#include "net.h"

#include <shlwapi.h>
#include <wincrypt.h>

#define MAX_BODY (64 * 1024 * 1024)
#define MAX_REDIRECTS 10
#define TIMEOUT_MS 30000

static void set_error( struct response *response, const char *format, ... )
{
    va_list args;

    va_start( args, format );
    vsnprintf( response->error, sizeof(response->error), format, args );
    va_end( args );
}

void response_free( struct response *response )
{
    free( response->url );
    free( response->content_type );
    free( response->body );
    memset( response, 0, sizeof(*response) );
}

UINT content_type_code_page( const char *content_type )
{
    const char *charset = content_type ? StrStrIA( content_type, "charset=" ) : NULL;
    size_t len;

    if (!charset) return CP_UTF8;
    charset += 8;
    if (*charset == '"') charset++;
    len = strcspn( charset, "\"; " );
    return charset_code_page( charset, len );
}

/* --- the body of a buffer --- */

struct buffer
{
    char   *data;
    size_t  len, size;
};

static BOOL append( struct buffer *buf, const char *data, size_t len )
{
    if (buf->len + len > MAX_BODY) return FALSE;
    if (buf->len + len + 1 > buf->size)
    {
        size_t size = max( buf->size * 2, buf->len + len + 65536 );
        char *grown = realloc( buf->data, size );

        if (!grown) return FALSE;
        buf->data = grown;
        buf->size = size;
    }
    memcpy( buf->data + buf->len, data, len );
    buf->len += len;
    buf->data[buf->len] = 0;
    return TRUE;
}

/* --- data: and file: --- */

static BOOL fetch_data( const char *url, struct response *response )
{
    const char *comma = strchr( url, ',' ), *meta = url + 5;
    BOOL base64;
    DWORD len = 0;

    if (!comma) { set_error( response, "адреса data: без коми" ); return FALSE; }
    base64 = comma - meta >= 7 && !_strnicmp( comma - 7, ";base64", 7 );
    response->content_type = malloc( comma - meta + 1 );
    if (response->content_type)
    {
        memcpy( response->content_type, meta, comma - meta );
        response->content_type[comma - meta] = 0;
    }
    comma++;
    if (base64)
    {
        if (!CryptStringToBinaryA( comma, 0, CRYPT_STRING_BASE64, NULL, &len, NULL, NULL ) ||
            !(response->body = malloc( len + 1 )) ||
            !CryptStringToBinaryA( comma, 0, CRYPT_STRING_BASE64, (BYTE *)response->body, &len, NULL, NULL ))
        {
            set_error( response, "зіпсований base64 в адресі data:" );
            return FALSE;
        }
        response->body[len] = 0;
        response->length = len;
    }
    else
    {
        size_t n = strlen( comma );

        if (!(response->body = malloc( n + 1 ))) return FALSE;
        memcpy( response->body, comma, n + 1 );
        UrlUnescapeA( response->body, NULL, NULL, URL_UNESCAPE_INPLACE );
        response->length = strlen( response->body );
    }
    response->status = 200;
    return TRUE;
}

static const char *type_by_extension( const WCHAR *path )
{
    static const struct { const WCHAR *ext; const char *type; } types[] =
    {
        { L".html", "text/html" }, { L".htm", "text/html" }, { L".xhtml", "text/html" }, { L".css", "text/css" },
        { L".png", "image/png" }, { L".jpg", "image/jpeg" }, { L".jpeg", "image/jpeg" }, { L".gif", "image/gif" },
        { L".bmp", "image/bmp" }, { L".ico", "image/x-icon" }, { L".svg", "image/svg+xml" },
        { L".txt", "text/plain" }, { L".js", "text/javascript" },
    };
    const WCHAR *ext = PathFindExtensionW( path );

    for (unsigned int i = 0; i < ARRAY_SIZE(types); i++) if (!_wcsicmp( ext, types[i].ext )) return types[i].type;
    return "application/octet-stream";
}

static BOOL fetch_file( const char *url, struct response *response )
{
    WCHAR wide[2084], path[MAX_PATH];
    DWORD len = MAX_PATH, got;
    LARGE_INTEGER size;
    HANDLE file;

    if (!MultiByteToWideChar( CP_UTF8, 0, url, -1, wide, ARRAY_SIZE(wide) )) wide[0] = 0;
    if (wcschr( wide, '#' )) *wcschr( wide, '#' ) = 0;     /* the fragment is no part of the file */
    if (!wide[0] || FAILED(PathCreateFromUrlW( wide, path, &len, 0 )))
    {
        set_error( response, "адреса файлу не читається" );
        return FALSE;
    }
    /* a share is the network: no page makes the system open one */
    if (path[0] == '\\' && path[1] == '\\')
    {
        set_error( response, "мережеві шляхи сторінки не відкривають" );
        return FALSE;
    }
    file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL );
    if (file == INVALID_HANDLE_VALUE)
    {
        set_error( response, "файлу немає або його не прочитати" );
        return FALSE;
    }
    if (!GetFileSizeEx( file, &size ) || size.QuadPart > MAX_BODY ||
        !(response->body = malloc( (size_t)size.QuadPart + 1 )))
    {
        CloseHandle( file );
        set_error( response, "файл завеликий" );
        return FALSE;
    }
    if (!ReadFile( file, response->body, (DWORD)size.QuadPart, &got, NULL )) got = 0;
    CloseHandle( file );
    response->body[got] = 0;
    response->length = got;
    response->content_type = _strdup( type_by_extension( path ) );
    response->status = 200;
    return TRUE;
}

/* --- http: and https: --- */

struct connection
{
    SOCKET      sock;
    struct tls *tls;
};

static BOOL connection_open( struct connection *conn, const struct url_parts *url, struct response *response )
{
    ADDRINFOA hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_protocol = IPPROTO_TCP }, *result, *ai;
    char port[16], host[256];
    DWORD timeout = TIMEOUT_MS;
    int status;

    conn->sock = INVALID_SOCKET;
    conn->tls = NULL;
    snprintf( port, sizeof(port), "%d", url->port );
    /* [::1] is an address, the brackets only mark it in the URL */
    lstrcpynA( host, url->host[0] == '[' ? url->host + 1 : url->host, sizeof(host) );
    if (url->host[0] == '[' && strchr( host, ']' )) *strchr( host, ']' ) = 0;

    if ((status = getaddrinfo( host, port, &hints, &result )))
    {
        set_error( response, "адресу %s не знайдено (DNS: %d)", url->host, status );
        return FALSE;
    }
    for (ai = result; ai; ai = ai->ai_next)
    {
        conn->sock = socket( ai->ai_family, ai->ai_socktype, ai->ai_protocol );
        if (conn->sock == INVALID_SOCKET) continue;
        setsockopt( conn->sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout) );
        setsockopt( conn->sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout) );
        if (!connect( conn->sock, ai->ai_addr, (int)ai->ai_addrlen )) break;
        closesocket( conn->sock );
        conn->sock = INVALID_SOCKET;
    }
    freeaddrinfo( result );
    if (conn->sock == INVALID_SOCKET)
    {
        set_error( response, "%s не відповідає (порт %d)", url->host, url->port );
        return FALSE;
    }
    if (!strcmp( url->scheme, "https" ) &&
        !(conn->tls = tls_connect( conn->sock, url->host, response->error, sizeof(response->error) )))
    {
        if (!response->error[0]) set_error( response, "не вдалося встановити TLS із %s", url->host );
        closesocket( conn->sock );
        conn->sock = INVALID_SOCKET;
        return FALSE;
    }
    return TRUE;
}

static void connection_close( struct connection *conn )
{
    tls_close( conn->tls );
    if (conn->sock != INVALID_SOCKET) closesocket( conn->sock );
}

static BOOL connection_send( struct connection *conn, const char *data, size_t len )
{
    if (conn->tls) return tls_send( conn->tls, data, len );
    while (len)
    {
        int sent = send( conn->sock, data, (int)min( len, 65536 ), 0 );

        if (sent <= 0) return FALSE;
        data += sent;
        len -= sent;
    }
    return TRUE;
}

static int connection_recv( struct connection *conn, char *data, size_t len )
{
    int got;

    if (conn->tls) return tls_recv( conn->tls, data, len );
    got = recv( conn->sock, data, (int)len, 0 );
    return got < 0 ? 0 : got;
}

static char *header_value( const char *headers, const char *name, const char **next );

/* --- connections kept open: a page asks one host for dozens of things --- */

#define POOL_SIZE    16
#define POOL_IDLE_MS 30000

struct pooled
{
    char              key[300];     /* scheme://host:port */
    struct connection conn;
    ULONGLONG         since;
};

static struct pooled pool[POOL_SIZE];
static SRWLOCK pool_lock = SRWLOCK_INIT;

static void pool_key( const struct url_parts *url, char *key, size_t size )
{
    snprintf( key, size, "%s://%s:%d", url->scheme, url->host, url->port );
}

static BOOL pool_take( const char *key, struct connection *conn )
{
    BOOL found = FALSE;
    ULONGLONG now = GetTickCount64();

    AcquireSRWLockExclusive( &pool_lock );
    for (unsigned int i = 0; i < POOL_SIZE; i++)
    {
        if (!pool[i].key[0]) continue;
        if (now - pool[i].since > POOL_IDLE_MS)
        {
            connection_close( &pool[i].conn );
            pool[i].key[0] = 0;
            continue;
        }
        if (!found && !strcmp( pool[i].key, key ))
        {
            *conn = pool[i].conn;
            pool[i].key[0] = 0;
            found = TRUE;
        }
    }
    ReleaseSRWLockExclusive( &pool_lock );
    return found;
}

static void pool_put( const char *key, struct connection *conn )
{
    unsigned int oldest = 0;

    AcquireSRWLockExclusive( &pool_lock );
    for (unsigned int i = 0; i < POOL_SIZE; i++)
    {
        if (!pool[i].key[0]) { oldest = i; break; }
        if (pool[i].since < pool[oldest].since) oldest = i;
    }
    if (pool[oldest].key[0]) connection_close( &pool[oldest].conn );
    lstrcpynA( pool[oldest].key, key, sizeof(pool[oldest].key) );
    pool[oldest].conn = *conn;
    pool[oldest].since = GetTickCount64();
    ReleaseSRWLockExclusive( &pool_lock );
}

/* One response off the connection, its body exactly as long as it says -
 * Content-Length, or chunks, or until the server closes. FALSE when not a
 * byte came: a kept connection the server had closed meanwhile. */
static BOOL read_response( struct connection *conn, struct buffer *reply, BOOL head_only, BOOL *reusable )
{
    char buffer[16384], *head_end;
    size_t head_len, body_start;
    int got, status = 0;

    *reusable = FALSE;
    while (!reply->data || !(head_end = strstr( reply->data, "\r\n\r\n" )))
    {
        if ((got = connection_recv( conn, buffer, sizeof(buffer) )) <= 0) return reply->len > 0;
        if (!append( reply, buffer, got )) return TRUE;
    }
    head_len = head_end - reply->data;
    body_start = head_len + 4;
    sscanf( reply->data, "HTTP/%*d.%*d %d", &status );
    {
        /* the headers that frame the body, read off a copy so the block stays as it came */
        char *head = malloc( head_len + 1 ), *value;
        const char *next = NULL;
        BOOL chunked = FALSE, closes, has_length = FALSE;
        unsigned long long length = 0;

        if (!head) return TRUE;
        memcpy( head, reply->data, head_len );
        head[head_len] = 0;
        if ((value = header_value( head, "Transfer-Encoding", &next ))) { chunked = !!StrStrIA( value, "chunked" ); free( value ); }
        next = NULL;
        if ((value = header_value( head, "Content-Length", &next ))) { has_length = TRUE; length = _strtoui64( value, NULL, 10 ); free( value ); }
        next = NULL;
        closes = !strncmp( head, "HTTP/1.0", 8 );
        if ((value = header_value( head, "Connection", &next )))
        {
            closes = !!StrStrIA( value, "close" ) || (closes && !StrStrIA( value, "keep-alive" ));
            free( value );
        }
        free( head );

        if (head_only || status == 204 || status == 304 || (status >= 100 && status < 200))
        {
            *reusable = !closes;
            return TRUE;
        }
        if (chunked)
        {
            size_t pos = body_start;

            for (;;)
            {
                char *line_end;
                unsigned long size;

                while (!(line_end = strstr( reply->data + pos, "\r\n" )))
                {
                    if ((got = connection_recv( conn, buffer, sizeof(buffer) )) <= 0 || !append( reply, buffer, got ))
                        return TRUE;
                }
                size = strtoul( reply->data + pos, NULL, 16 );
                pos = line_end - reply->data + 2;
                if (!size)
                {
                    /* the trailer, if any, ends with an empty line */
                    while (!strstr( reply->data + pos - 2, "\r\n\r\n" ))
                    {
                        if ((got = connection_recv( conn, buffer, sizeof(buffer) )) <= 0 || !append( reply, buffer, got ))
                            return TRUE;
                    }
                    break;
                }
                while (reply->len < pos + size + 2)
                {
                    if ((got = connection_recv( conn, buffer, sizeof(buffer) )) <= 0 || !append( reply, buffer, got ))
                        return TRUE;
                }
                pos += size + 2;
            }
            *reusable = !closes;
        }
        else if (has_length)
        {
            while (reply->len < body_start + length)
            {
                if ((got = connection_recv( conn, buffer, sizeof(buffer) )) <= 0 || !append( reply, buffer, got ))
                    return TRUE;
            }
            *reusable = !closes;
        }
        else
        {
            /* to the end: the server says nothing of the length */
            while ((got = connection_recv( conn, buffer, sizeof(buffer) )) > 0)
                if (!append( reply, buffer, got )) break;
        }
    }
    return TRUE;
}

/* the value of a header in a block of them, or NULL; the caller frees */
static char *header_value( const char *headers, const char *name, const char **next )
{
    size_t name_len = strlen( name );
    const char *line = *next ? *next : headers;

    while (line && *line)
    {
        const char *end = strstr( line, "\r\n" ), *value;
        char *copy;

        if (!end) end = line + strlen( line );
        if ((size_t)(end - line) > name_len && line[name_len] == ':' && !_strnicmp( line, name, name_len ))
        {
            value = line + name_len + 1;
            while (*value == ' ' || *value == '\t') value++;
            if (!(copy = malloc( end - value + 1 ))) return NULL;
            memcpy( copy, value, end - value );
            copy[end - value] = 0;
            *next = *end ? end + 2 : end;
            return copy;
        }
        line = *end ? end + 2 : NULL;
    }
    *next = NULL;
    return NULL;
}

/* a body of Transfer-Encoding: chunked, in place */
static size_t dechunk( char *body, size_t len )
{
    size_t in = 0, out = 0;

    while (in < len)
    {
        char *end;
        unsigned long chunk = strtoul( body + in, &end, 16 );
        size_t line_end = (end - body);

        while (line_end + 1 < len && !(body[line_end] == '\r' && body[line_end + 1] == '\n')) line_end++;
        in = line_end + 2;
        if (!chunk || in > len) break;
        chunk = (unsigned long)min( chunk, len - in );
        memmove( body + out, body + in, chunk );
        out += chunk;
        in += chunk + 2;
    }
    body[out] = 0;
    return out;
}

static BOOL fetch_http( const char *url, const char *page, enum request_type type, const char *method,
                        const char *headers, const char *post, size_t post_len, struct response *response )
{
    char *current = _strdup( url );

    for (int redirect = 0; current && redirect <= MAX_REDIRECTS; redirect++)
    {
        struct url_parts parts;
        struct connection conn;
        struct buffer request = { 0 }, reply = { 0 };
        char *cookies, *location = NULL, *head_end, line[512], *value, key[300];
        const char *next = NULL, *accept;
        BOOL reused, reusable;

        if (!url_parse( current, &parts ) || !parts.host[0])
        {
            set_error( response, "адреса не читається" );
            break;
        }
        /* what the user goes to is never filtered: only what a page loads */
        if (page && type != REQUEST_DOCUMENT && filter_blocks( current, &parts, page, type ))
        {
            response->blocked = TRUE;
            set_error( response, "заблоковано фільтром" );
            break;
        }
        pool_key( &parts, key, sizeof(key) );
        reused = pool_take( key, &conn );
        if (!reused && !connection_open( &conn, &parts, response )) break;

        switch (type)
        {
        case REQUEST_DOCUMENT:   accept = "text/html,application/xhtml+xml;q=0.9,*/*;q=0.8"; break;
        case REQUEST_STYLESHEET: accept = "text/css,*/*;q=0.1"; break;
        case REQUEST_IMAGE:      accept = "image/png,image/jpeg,image/gif,image/*;q=0.8,*/*;q=0.5"; break;
        default:                 accept = "*/*"; break;
        }
        if (method) { append( &request, method, strlen( method ) ); append( &request, " ", 1 ); }
        else if (post) append( &request, "POST ", 5 );
        else append( &request, "GET ", 4 );
        append( &request, parts.path, parts.path_len );
        snprintf( line, sizeof(line), " HTTP/1.1\r\nHost: %s", parts.host );
        append( &request, line, strlen( line ) );
        if ((!strcmp( parts.scheme, "https" ) && parts.port != 443) || (!strcmp( parts.scheme, "http" ) && parts.port != 80))
        {
            snprintf( line, sizeof(line), ":%d", parts.port );
            append( &request, line, strlen( line ) );
        }
        snprintf( line, sizeof(line), "\r\nUser-Agent: %s\r\nAccept: %s\r\nAccept-Language: uk,en;q=0.7\r\n"
                  "Accept-Encoding: identity\r\nConnection: keep-alive\r\n", USER_AGENT, accept );
        append( &request, line, strlen( line ) );
        if ((cookies = cookies_get( &parts )))
        {
            append( &request, "Cookie: ", 8 );
            append( &request, cookies, strlen( cookies ) );
            append( &request, "\r\n", 2 );
            free( cookies );
        }
        if (page && !_strnicmp( page, "http", 4 ))
        {
            append( &request, "Referer: ", 9 );
            append( &request, page, strcspn( page, "#" ) );
            append( &request, "\r\n", 2 );
        }
        if (post)
        {
            /* a form of the page: where it came from, what it sends */
            if (page && (!_strnicmp( page, "http:", 5 ) || !_strnicmp( page, "https:", 6 )))
            {
                const char *host = strstr( page, "//" ), *host_end;

                host = host ? host + 2 : page;
                host_end = host + strcspn( host, "/?#" );
                append( &request, "Origin: ", 8 );
                append( &request, page, host_end - page );
                append( &request, "\r\n", 2 );
            }
            if (!headers || !StrStrIA( headers, "content-type:" ))
                append( &request, "Content-Type: application/x-www-form-urlencoded\r\n", 49 );
            snprintf( line, sizeof(line), "Content-Length: %lu\r\n", (unsigned long)post_len );
            append( &request, line, strlen( line ) );
        }
        /* a script's own headers: "Name: value\r\n" each */
        if (headers) append( &request, headers, strlen( headers ) );
        append( &request, "\r\n", 2 );
        if (post) append( &request, post, post_len );

        /* a kept connection the server closed meanwhile takes nothing: once more on a new one */
        if (!connection_send( &conn, request.data, request.len ) || !read_response( &conn, &reply, FALSE, &reusable ))
        {
            connection_close( &conn );
            free( reply.data );
            memset( &reply, 0, sizeof(reply) );
            if (!reused || !connection_open( &conn, &parts, response ) ||
                !connection_send( &conn, request.data, request.len ) || !read_response( &conn, &reply, FALSE, &reusable ))
            {
                if (!response->error[0]) set_error( response, "%s не прийняв запит", parts.host );
                free( request.data );
                if (reused) connection_close( &conn );
                break;
            }
        }
        free( request.data );
        if (reusable) pool_put( key, &conn );
        else connection_close( &conn );

        if (!reply.data || !(head_end = strstr( reply.data, "\r\n\r\n" )) ||
            sscanf( reply.data, "HTTP/%*d.%*d %d", &response->status ) != 1)
        {
            set_error( response, "%s відповів не по-HTTP або не відповів", parts.host );
            free( reply.data );
            break;
        }
        *head_end = 0;

        while ((value = header_value( reply.data, "Set-Cookie", &next )))
        {
            cookies_set( &parts, value, strlen( value ) );
            free( value );
        }
        next = NULL;
        if (response->status >= 300 && response->status < 400 && response->status != 304 &&
            (location = header_value( reply.data, "Location", &next )))
        {
            char *target = url_resolve( current, location, strlen( location ) );

            free( location );
            free( reply.data );
            free( current );
            current = target;
            if (response->status != 307 && response->status != 308) post = NULL, method = NULL;
            continue;
        }

        next = NULL;
        response->content_type = header_value( reply.data, "Content-Type", &next );
        {
            const char *body = head_end + 4;
            size_t length = reply.len - (body - reply.data);
            char *encoding, *declared;

            next = NULL;
            if ((encoding = header_value( reply.data, "Transfer-Encoding", &next )) && StrStrIA( encoding, "chunked" ))
                length = dechunk( (char *)body, length );
            free( encoding );
            next = NULL;
            if ((declared = header_value( reply.data, "Content-Length", &next )))
            {
                length = min( length, (size_t)_strtoui64( declared, NULL, 10 ) );
                free( declared );
            }
            memmove( reply.data, body, length );
            reply.data[length] = 0;
            response->body = reply.data;
            response->length = length;
        }
        response->url = current;
        return TRUE;
    }
    if (current && !response->error[0]) set_error( response, "забагато переадресацій" );
    free( current );
    return FALSE;
}

static INIT_ONCE winsock_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK start_winsock( INIT_ONCE *once, void *param, void **context )
{
    WSADATA data;

    WSAStartup( MAKEWORD(2, 2), &data );
    filter_init();
    return TRUE;
}

BOOL net_fetch( const char *url, const char *page, enum request_type type, struct response *response )
{
    memset( response, 0, sizeof(*response) );
    if (!url) return FALSE;
    InitOnceExecuteOnce( &winsock_once, start_winsock, NULL, NULL );

    if (!_strnicmp( url, "data:", 5 )) return fetch_data( url, response );
    if (!_strnicmp( url, "file:", 5 ))
    {
        /* a page from the network never reads the disk */
        if (page && _strnicmp( page, "file:", 5 ))
        {
            set_error( response, "сторінка з мережі не читає файлів диска" );
            return FALSE;
        }
        if (fetch_file( url, response ))
        {
            response->url = _strdup( url );
            return TRUE;
        }
        return FALSE;
    }
    if (!_strnicmp( url, "http:", 5 ) || !_strnicmp( url, "https:", 6 ))
        return fetch_http( url, page, type, NULL, NULL, NULL, 0, response );
    set_error( response, "рушій не знає такої адреси" );
    return FALSE;
}

BOOL net_post( const char *url, const char *page, const char *body, size_t len, struct response *response )
{
    memset( response, 0, sizeof(*response) );
    if (!url) return FALSE;
    InitOnceExecuteOnce( &winsock_once, start_winsock, NULL, NULL );
    if (_strnicmp( url, "http:", 5 ) && _strnicmp( url, "https:", 6 ))
    {
        set_error( response, "форма шле дані лише на http і https" );
        return FALSE;
    }
    return fetch_http( url, page, REQUEST_DOCUMENT, NULL, NULL, body ? body : "", len, response );
}

BOOL net_request( const char *method, const char *url, const char *page, const char *headers, const char *body,
                  size_t len, struct response *response )
{
    memset( response, 0, sizeof(*response) );
    if (!url) return FALSE;
    InitOnceExecuteOnce( &winsock_once, start_winsock, NULL, NULL );
    if (_strnicmp( url, "http:", 5 ) && _strnicmp( url, "https:", 6 ))
        return net_fetch( url, page, REQUEST_OTHER, response );
    /* a POST with nothing to send still says so: Content-Length: 0 */
    if (!body && method && (!_stricmp( method, "POST" ) || !_stricmp( method, "PUT" ))) body = "", len = 0;
    return fetch_http( url, page, REQUEST_OTHER, method, headers, body, body ? len : 0, response );
}