/*
 * Arctic WordPad: buffers, ZIP packages and XML
 *
 * A DOCX is a ZIP of XML parts. The ZIP is read and written here with
 * zlib's deflate; the XML is read by a fixed pull reader, enough for what
 * Word and other word processors write (no DTDs, entities of XML's own and
 * numeric ones).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdio.h>
#include <ctype.h>
#include <zlib.h>
#include "wordpad.h"

/**********************************************************************
 *          Buffers
 */

void buf_add( struct buffer *b, const void *data, size_t len )
{
    if (b->len + len + 1 > b->size)
    {
        size_t size = max( b->size * 2, b->len + len + 256 );
        char *n = realloc( b->data, size );

        if (!n) return;
        b->data = n;
        b->size = size;
    }
    memcpy( b->data + b->len, data, len );
    b->len += len;
    b->data[b->len] = 0;
}

void buf_str( struct buffer *b, const char *s )
{
    buf_add( b, s, strlen( s ) );
}

void buf_printf( struct buffer *b, const char *format, ... )
{
    char fixed[512];
    va_list args;
    int n;

    va_start( args, format );
    n = vsnprintf( fixed, sizeof(fixed), format, args );
    va_end( args );
    if (n < 0) return;
    if (n < (int)sizeof(fixed))
    {
        buf_add( b, fixed, n );
        return;
    }
    {
        char *big = malloc( n + 1 );
        if (!big) return;
        va_start( args, format );
        vsnprintf( big, n + 1, format, args );
        va_end( args );
        buf_add( b, big, n );
        free( big );
    }
}

void buf_wstr_utf8( struct buffer *b, const WCHAR *s, int len )
{
    int n;
    char *utf8;

    if (len < 0) len = lstrlenW( s );
    if (!len) return;
    n = WideCharToMultiByte( CP_UTF8, 0, s, len, NULL, 0, NULL, NULL );
    if (!(utf8 = malloc( n ))) return;
    WideCharToMultiByte( CP_UTF8, 0, s, len, utf8, n, NULL, NULL );
    buf_add( b, utf8, n );
    free( utf8 );
}

void buf_free( struct buffer *b )
{
    free( b->data );
    memset( b, 0, sizeof(*b) );
}

void hex_bytes( struct buffer *b, const BYTE *data, size_t len )
{
    static const char digits[] = "0123456789abcdef";
    char line[130];

    for (size_t i = 0; i < len; )
    {
        int n = 0;
        for (; n < 128 && i < len; i++)
        {
            line[n++] = digits[data[i] >> 4];
            line[n++] = digits[data[i] & 15];
        }
        line[n++] = '\n';
        buf_add( b, line, n );
    }
}

/**********************************************************************
 *          Reading ZIP
 */

/* zlib as Wine builds it (Z_SOLO) has no allocator of its own */
static voidpf z_alloc( voidpf opaque, uInt items, uInt size ) { return calloc( items, size ); }
static void z_free( voidpf opaque, voidpf p ) { free( p ); }

struct zip_entry
{
    char   name[260];
    DWORD  method, crc, packed, size, offset;
};

struct zip
{
    const BYTE *data;
    size_t size;
    struct zip_entry *entries;
    int count;
};

static DWORD le32( const BYTE *p ) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((DWORD)p[3] << 24); }
static WORD le16( const BYTE *p ) { return p[0] | (p[1] << 8); }

struct zip *zip_open( const BYTE *data, size_t size )
{
    struct zip *zip;
    const BYTE *end = NULL, *p;
    DWORD count, dir_offset;

    if (size < 22) return NULL;
    /* the end of the central directory, from the end, past a comment */
    for (size_t i = size - 22; ; i--)
    {
        if (le32( data + i ) == 0x06054b50)
        {
            end = data + i;
            break;
        }
        if (!i || size - i > 65557) break;
    }
    if (!end) return NULL;
    count = le16( end + 10 );
    dir_offset = le32( end + 16 );
    if (dir_offset >= size) return NULL;
    if (!(zip = calloc( 1, sizeof(*zip) ))) return NULL;
    zip->data = data;
    zip->size = size;
    zip->entries = calloc( max( count, 1 ), sizeof(*zip->entries) );
    p = data + dir_offset;
    for (DWORD i = 0; i < count && zip->entries; i++)
    {
        struct zip_entry *e = &zip->entries[zip->count];
        WORD name_len, extra_len, comment_len;

        if (p + 46 > data + size || le32( p ) != 0x02014b50) break;
        e->method = le16( p + 10 );
        e->crc = le32( p + 16 );
        e->packed = le32( p + 20 );
        e->size = le32( p + 24 );
        name_len = le16( p + 28 );
        extra_len = le16( p + 30 );
        comment_len = le16( p + 32 );
        e->offset = le32( p + 42 );
        if (p + 46 + name_len > data + size) break;
        memcpy( e->name, p + 46, min( name_len, sizeof(e->name) - 1 ) );
        e->name[min( name_len, sizeof(e->name) - 1 )] = 0;
        p += 46 + name_len + extra_len + comment_len;
        zip->count++;
    }
    return zip;
}

void zip_close( struct zip *zip )
{
    if (!zip) return;
    free( zip->entries );
    free( zip );
}

int zip_count( struct zip *zip ) { return zip ? zip->count : 0; }
const char *zip_name( struct zip *zip, int index ) { return zip->entries[index].name; }

static BYTE *unpack( struct zip *zip, struct zip_entry *e, size_t *len )
{
    const BYTE *local = zip->data + e->offset, *packed;
    BYTE *out;

    if (e->offset + 30 > zip->size || le32( local ) != 0x04034b50) return NULL;
    packed = local + 30 + le16( local + 26 ) + le16( local + 28 );
    if (packed + e->packed > zip->data + zip->size) return NULL;
    if (!(out = malloc( (size_t)e->size + 1 ))) return NULL;
    if (e->method == 0)
    {
        memcpy( out, packed, e->size );
    }
    else if (e->method == 8)
    {
        z_stream z = { 0 };

        z.zalloc = z_alloc;
        z.zfree = z_free;
        z.next_in = (Bytef *)packed;
        z.avail_in = e->packed;
        z.next_out = out;
        z.avail_out = e->size;
        if (inflateInit2( &z, -MAX_WBITS ) != Z_OK)
        {
            free( out );
            return NULL;
        }
        inflate( &z, Z_FINISH );
        inflateEnd( &z );
        if (z.total_out != e->size)
        {
            free( out );
            return NULL;
        }
    }
    else
    {
        free( out );
        return NULL;
    }
    out[e->size] = 0;
    *len = e->size;
    return out;
}

/* a part by its name (without a leading /), case as the package has it or not */
BYTE *zip_read( struct zip *zip, const char *name, size_t *len )
{
    if (!zip) return NULL;
    if (name[0] == '/') name++;
    for (int i = 0; i < zip->count; i++)
        if (!strcmp( zip->entries[i].name, name )) return unpack( zip, &zip->entries[i], len );
    for (int i = 0; i < zip->count; i++)
        if (!_stricmp( zip->entries[i].name, name )) return unpack( zip, &zip->entries[i], len );
    return NULL;
}

/**********************************************************************
 *          Writing ZIP
 */

struct zip_writer
{
    struct buffer data, dir;
    int count;
};

struct zip_writer *zipw_create(void)
{
    return calloc( 1, sizeof(struct zip_writer) );
}

static void put16( struct buffer *b, WORD v ) { BYTE x[2] = { v & 0xff, v >> 8 }; buf_add( b, x, 2 ); }
static void put32( struct buffer *b, DWORD v ) { BYTE x[4] = { v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, v >> 24 }; buf_add( b, x, 4 ); }

void zipw_add( struct zip_writer *z, const char *name, const void *data, size_t len, BOOL compress )
{
    DWORD crc = crc32( 0, data, (uInt)len ), offset = (DWORD)z->data.len, packed_len = (DWORD)len;
    BYTE *packed = NULL;
    WORD method = 0, name_len = (WORD)strlen( name );
    /* 1980-01-01, as most writers of DOCX put */
    WORD time = 0, date = (0 << 9) | (1 << 5) | 1;

    if (compress && len > 64)
    {
        uLong bound = (uLong)len + (uLong)len / 8 + 256;
        z_stream s = { 0 };

        s.zalloc = z_alloc;
        s.zfree = z_free;
        if ((packed = malloc( bound )) && deflateInit2( &s, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8,
                                                        Z_DEFAULT_STRATEGY ) == Z_OK)
        {
            s.next_in = (Bytef *)data;
            s.avail_in = (uInt)len;
            s.next_out = packed;
            s.avail_out = bound;
            if (deflate( &s, Z_FINISH ) == Z_STREAM_END && s.total_out < len)
            {
                method = 8;
                packed_len = s.total_out;
            }
            deflateEnd( &s );
        }
    }
    put32( &z->data, 0x04034b50 );
    put16( &z->data, 20 );
    put16( &z->data, 0x0800 );   /* names in UTF-8 */
    put16( &z->data, method );
    put16( &z->data, time );
    put16( &z->data, date );
    put32( &z->data, crc );
    put32( &z->data, packed_len );
    put32( &z->data, (DWORD)len );
    put16( &z->data, name_len );
    put16( &z->data, 0 );
    buf_add( &z->data, name, name_len );
    buf_add( &z->data, method ? packed : data, packed_len );

    put32( &z->dir, 0x02014b50 );
    put16( &z->dir, 20 );
    put16( &z->dir, 20 );
    put16( &z->dir, 0x0800 );
    put16( &z->dir, method );
    put16( &z->dir, time );
    put16( &z->dir, date );
    put32( &z->dir, crc );
    put32( &z->dir, packed_len );
    put32( &z->dir, (DWORD)len );
    put16( &z->dir, name_len );
    put16( &z->dir, 0 );
    put16( &z->dir, 0 );
    put16( &z->dir, 0 );
    put16( &z->dir, 0 );
    put32( &z->dir, 0 );
    put32( &z->dir, offset );
    buf_add( &z->dir, name, name_len );
    z->count++;
    free( packed );
}

BYTE *zipw_finish( struct zip_writer *z, size_t *len )
{
    DWORD dir_offset = (DWORD)z->data.len;
    BYTE *out;

    buf_add( &z->data, z->dir.data, z->dir.len );
    put32( &z->data, 0x06054b50 );
    put16( &z->data, 0 );
    put16( &z->data, 0 );
    put16( &z->data, z->count );
    put16( &z->data, z->count );
    put32( &z->data, (DWORD)z->dir.len );
    put32( &z->data, dir_offset );
    put16( &z->data, 0 );
    out = (BYTE *)z->data.data;
    *len = z->data.len;
    buf_free( &z->dir );
    free( z );
    return out;
}

/**********************************************************************
 *          XML
 */

void xml_init( struct xml *x, const char *data, size_t len )
{
    memset( x, 0, sizeof(*x) );
    x->p = data;
    x->end = data + len;
    if (len >= 3 && !memcmp( data, "\xef\xbb\xbf", 3 )) x->p += 3;
}

void xml_free( struct xml *x )
{
    buf_free( &x->text );
}

static void put_utf8( struct buffer *b, unsigned int c )
{
    char s[4];
    int n;

    if (c < 0x80) { s[0] = c; n = 1; }
    else if (c < 0x800) { s[0] = 0xc0 | (c >> 6); s[1] = 0x80 | (c & 0x3f); n = 2; }
    else if (c < 0x10000) { s[0] = 0xe0 | (c >> 12); s[1] = 0x80 | ((c >> 6) & 0x3f); s[2] = 0x80 | (c & 0x3f); n = 3; }
    else { s[0] = 0xf0 | (c >> 18); s[1] = 0x80 | ((c >> 12) & 0x3f); s[2] = 0x80 | ((c >> 6) & 0x3f); s[3] = 0x80 | (c & 0x3f); n = 4; }
    buf_add( b, s, n );
}

/* XML's entities and numeric references into UTF-8 */
static void decode( struct buffer *b, const char *p, const char *end )
{
    while (p < end)
    {
        const char *amp = memchr( p, '&', end - p );

        if (!amp)
        {
            buf_add( b, p, end - p );
            break;
        }
        buf_add( b, p, amp - p );
        p = amp + 1;
        if (end - p >= 3 && !memcmp( p, "lt;", 3 )) { buf_add( b, "<", 1 ); p += 3; }
        else if (end - p >= 3 && !memcmp( p, "gt;", 3 )) { buf_add( b, ">", 1 ); p += 3; }
        else if (end - p >= 4 && !memcmp( p, "amp;", 4 )) { buf_add( b, "&", 1 ); p += 4; }
        else if (end - p >= 5 && !memcmp( p, "quot;", 5 )) { buf_add( b, "\"", 1 ); p += 5; }
        else if (end - p >= 5 && !memcmp( p, "apos;", 5 )) { buf_add( b, "'", 1 ); p += 5; }
        else if (p < end && *p == '#')
        {
            unsigned int c = 0;
            p++;
            if (p < end && (*p == 'x' || *p == 'X'))
            {
                for (p++; p < end && isxdigit( (unsigned char)*p ); p++)
                    c = c * 16 + (isdigit( (unsigned char)*p ) ? *p - '0' : (tolower( *p ) - 'a' + 10));
            }
            else for (; p < end && isdigit( (unsigned char)*p ); p++) c = c * 10 + *p - '0';
            if (p < end && *p == ';') p++;
            put_utf8( b, c );
        }
        else buf_add( b, "&", 1 );
    }
}

static BOOL is_name_char( char c )
{
    return c && !strchr( " \t\r\n/>=\"'", c );
}

enum xml_token xml_next( struct xml *x )
{
    if (x->pending_close)
    {
        x->pending_close = FALSE;
        x->depth--;
        return XML_CLOSE;
    }
    for (;;)
    {
        const char *lt;

        if (x->p >= x->end) return XML_END;
        if (*x->p != '<')
        {
            lt = memchr( x->p, '<', x->end - x->p );
            if (!lt) lt = x->end;
            x->text.len = 0;
            decode( &x->text, x->p, lt );
            if (x->text.data) x->text.data[x->text.len] = 0;
            x->p = lt;
            return XML_TEXT;
        }
        if (x->end - x->p >= 4 && !memcmp( x->p, "<!--", 4 ))
        {
            const char *e = x->p + 4;
            while (e + 3 <= x->end && memcmp( e, "-->", 3 )) e++;
            x->p = min( e + 3, x->end );
            continue;
        }
        if (x->end - x->p >= 9 && !memcmp( x->p, "<![CDATA[", 9 ))
        {
            const char *s = x->p + 9, *e = s;
            while (e + 3 <= x->end && memcmp( e, "]]>", 3 )) e++;
            x->text.len = 0;
            buf_add( &x->text, s, e - s );
            x->p = min( e + 3, x->end );
            return XML_TEXT;
        }
        if (x->p + 1 < x->end && (x->p[1] == '?' || x->p[1] == '!'))
        {
            const char *gt = memchr( x->p, '>', x->end - x->p );
            x->p = gt ? gt + 1 : x->end;
            continue;
        }
        if (x->p + 1 < x->end && x->p[1] == '/')
        {
            const char *gt = memchr( x->p, '>', x->end - x->p );
            const char *s = x->p + 2;
            int n = 0;

            while (s < x->end && is_name_char( *s ) && n < (int)sizeof(x->qname) - 1) x->qname[n++] = *s++;
            x->qname[n] = 0;
            x->p = gt ? gt + 1 : x->end;
            {
                const char *colon = strchr( x->qname, ':' );
                lstrcpynA( x->name, colon ? colon + 1 : x->qname, sizeof(x->name) );
            }
            x->depth--;
            return XML_CLOSE;
        }
        /* an element */
        {
            const char *s = x->p + 1, *q;
            int n = 0;
            char quote = 0;

            while (s < x->end && is_name_char( *s ) && n < (int)sizeof(x->qname) - 1) x->qname[n++] = *s++;
            while (s < x->end && is_name_char( *s )) s++;
            x->qname[n] = 0;
            {
                const char *colon = strchr( x->qname, ':' );
                lstrcpynA( x->name, colon ? colon + 1 : x->qname, sizeof(x->name) );
            }
            x->attrs = s;
            for (q = s; q < x->end; q++)
            {
                if (quote) { if (*q == quote) quote = 0; }
                else if (*q == '"' || *q == '\'') quote = *q;
                else if (*q == '>') break;
            }
            x->attrs_end = q;
            x->empty = q > s && q[-1] == '/';
            if (x->empty) x->attrs_end = q - 1;
            x->p = q < x->end ? q + 1 : x->end;
            x->depth++;
            if (x->empty) x->pending_close = TRUE;
            return XML_OPEN;
        }
    }
}

BOOL xml_is( struct xml *x, const char *qname )
{
    return !strcmp( x->qname, qname );
}

/* an attribute by its full name ("w:val") or, without a prefix, its local name */
BOOL xml_attr( struct xml *x, const char *name, char *value, int size )
{
    const char *p = x->attrs;
    size_t name_len = strlen( name );
    BOOL local = !strchr( name, ':' );

    while (p < x->attrs_end)
    {
        const char *n, *v, *e;
        char quote;
        size_t len;

        while (p < x->attrs_end && strchr( " \t\r\n", *p )) p++;
        n = p;
        while (p < x->attrs_end && *p != '=' && !strchr( " \t\r\n", *p )) p++;
        len = p - n;
        while (p < x->attrs_end && *p != '"' && *p != '\'') p++;
        if (p >= x->attrs_end) break;
        quote = *p++;
        v = p;
        e = memchr( p, quote, x->attrs_end - p );
        if (!e) break;
        p = e + 1;
        {
            const char *colon = local ? memchr( n, ':', len ) : NULL;
            const char *cmp = colon ? colon + 1 : n;
            size_t cmp_len = colon ? len - (colon + 1 - n) : len;

            if (cmp_len == name_len && !memcmp( cmp, name, name_len ))
            {
                struct buffer b = { 0 };

                decode( &b, v, e );
                if (b.data) lstrcpynA( value, b.data, size );
                else value[0] = 0;
                buf_free( &b );
                return TRUE;
            }
        }
    }
    if (size) value[0] = 0;
    return FALSE;
}

int xml_attr_int( struct xml *x, const char *name, int def )
{
    char v[32];

    if (!xml_attr( x, name, v, sizeof(v) ) || !v[0]) return def;
    return atoi( v );
}

/* w:b, w:b w:val="true", "1", "on" are on; "false", "0", "off" off */
BOOL xml_attr_bool( struct xml *x, const char *name, BOOL def )
{
    char v[16];

    if (!xml_attr( x, name, v, sizeof(v) )) return def;
    return !(!strcmp( v, "false" ) || !strcmp( v, "0" ) || !strcmp( v, "off" ) || !strcmp( v, "none" ));
}

/* into the children of the element just opened: FALSE, and past it, when it has none */
BOOL xml_enter( struct xml *x )
{
    if (!x->empty) return TRUE;
    xml_next( x );
    return FALSE;
}

void xml_skip( struct xml *x )
{
    int depth = x->depth;

    for (;;)
    {
        enum xml_token t = xml_next( x );
        if (t == XML_END) return;
        if (t == XML_CLOSE && x->depth < depth) return;
    }
}
