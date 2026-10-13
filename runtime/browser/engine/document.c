/*
 * Arctic web engine: a document, from its address to lexbor's tree
 *
 * Stage 1 of docs/browser.md reads pages from the disk (file: and plain
 * paths) and the engine's own pages (about:); the network is stage 2, and
 * until then an http address gets a page that says so.
 *
 * lexbor parses the HTML, and with its style module on it also parses every
 * <style> as it meets it and matches the rules to the elements; the sheets
 * <link> names are read here and attached after.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <shlwapi.h>

#include "arctichtml_private.h"

#define MAX_DOCUMENT_SIZE (64 * 1024 * 1024)

static char *read_file( const WCHAR *path, size_t *len )
{
    HANDLE file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, NULL );
    LARGE_INTEGER size;
    DWORD got;
    char *data;

    if (file == INVALID_HANDLE_VALUE) return NULL;
    if (!GetFileSizeEx( file, &size ) || size.QuadPart > MAX_DOCUMENT_SIZE ||
        !(data = malloc( (size_t)size.QuadPart + 1 )))
    {
        CloseHandle( file );
        return NULL;
    }
    if (!ReadFile( file, data, (DWORD)size.QuadPart, &got, NULL )) got = 0;
    CloseHandle( file );
    data[got] = 0;
    *len = got;
    return data;
}

/* The code pages of the charsets Ukrainian and Western pages are written in;
 * anything else is taken as UTF-8, as the encoding standard does for new
 * pages. */
static UINT charset_code_page( const char *name, size_t len )
{
    static const struct { const char *name; UINT cp; } charsets[] =
    {
        { "windows-1251", 1251 }, { "cp1251", 1251 },
        { "windows-1252", 1252 }, { "iso-8859-1", 1252 }, { "latin1", 1252 }, { "us-ascii", 1252 },
        { "koi8-r", 20866 }, { "koi8-u", 21866 }, { "iso-8859-5", 28595 }, { "windows-1250", 1250 },
    };

    for (unsigned int i = 0; i < ARRAY_SIZE(charsets); i++)
        if (strlen( charsets[i].name ) == len && !_strnicmp( charsets[i].name, name, len ))
            return charsets[i].cp;
    return CP_UTF8;
}

/* <meta charset=...> or charset= in an http-equiv, in the first kilobytes */
static UINT sniff_code_page( const char *data, size_t len )
{
    size_t limit = min( len, 4096 );

    for (size_t i = 0; i + 8 < limit; i++)
    {
        size_t start, end;

        if (_strnicmp( data + i, "charset=", 8 )) continue;
        start = i + 8;
        while (start < limit && (data[start] == '"' || data[start] == '\'')) start++;
        end = start;
        while (end < limit && data[end] != '"' && data[end] != '\'' && data[end] != '>' &&
               data[end] != ';' && data[end] != ' ' && data[end] != '/')
            end++;
        return charset_code_page( data + start, end - start );
    }
    return CP_UTF8;
}

/* lexbor wants UTF-8: anything else is converted before it parses */
static char *to_utf8( char *data, size_t *len )
{
    UINT cp;
    int wide_len, out_len;
    WCHAR *wide;
    char *out;

    if (*len >= 3 && (BYTE)data[0] == 0xef && (BYTE)data[1] == 0xbb && (BYTE)data[2] == 0xbf)
    {
        memmove( data, data + 3, *len - 3 + 1 );
        *len -= 3;
        return data;
    }
    if (*len >= 2 && (BYTE)data[0] == 0xff && (BYTE)data[1] == 0xfe)
    {
        wide = (WCHAR *)(data + 2);
        wide_len = (int)((*len - 2) / 2);
    }
    else
    {
        if ((cp = sniff_code_page( data, *len )) == CP_UTF8) return data;
        wide_len = MultiByteToWideChar( cp, 0, data, (int)*len, NULL, 0 );
        if (!(wide = malloc( (wide_len + 1) * sizeof(WCHAR) ))) return data;
        MultiByteToWideChar( cp, 0, data, (int)*len, wide, wide_len );
    }
    out_len = WideCharToMultiByte( CP_UTF8, 0, wide, wide_len, NULL, 0, NULL, NULL );
    if ((out = malloc( out_len + 1 )))
    {
        WideCharToMultiByte( CP_UTF8, 0, wide, wide_len, out, out_len, NULL, NULL );
        out[out_len] = 0;
        *len = out_len;
    }
    if (wide != (WCHAR *)(data + 2)) free( wide );
    if (!out) return data;
    free( data );
    return out;
}

/* "C:\x.html", "C:/x.html", "file:///C:/x.html" - a path on this machine */
static BOOL url_to_path( const WCHAR *url, WCHAR path[MAX_PATH] )
{
    DWORD len = MAX_PATH;

    if (((url[0] >= 'a' && url[0] <= 'z') || (url[0] >= 'A' && url[0] <= 'Z')) && url[1] == ':' &&
        (url[2] == '\\' || url[2] == '/'))
    {
        lstrcpynW( path, url, MAX_PATH );
        for (WCHAR *p = path; *p; p++) if (*p == '/') *p = '\\';
        return TRUE;
    }
    if (_wcsnicmp( url, L"file:", 5 )) return FALSE;
    return SUCCEEDED(PathCreateFromUrlW( url, path, &len, 0 ));
}

/* a path a page names: relative to the page's folder, or absolute */
static BOOL resolve_path( const struct document *doc, const char *ref, size_t len, WCHAR path[MAX_PATH] )
{
    WCHAR *wide, combined[MAX_PATH];
    BOOL ok;

    if (!len || !(wide = utf8_to_wide( ref, len, NULL ))) return FALSE;
    /* //host/x is the network, and so is a UNC path: a page never makes the
     * system open a share - that would hand the user's NTLM hash to its host */
    if ((wide[0] == '/' || wide[0] == '\\') && (wide[1] == '/' || wide[1] == '\\'))
    {
        free( wide );
        return FALSE;
    }
    if (url_to_path( wide, path ))
    {
        free( wide );
        return path[0] != '\\';
    }
    if (!doc->base_dir[0] || wcschr( wide, ':' ))     /* another scheme: http, data, ... */
    {
        free( wide );
        return FALSE;
    }
    for (WCHAR *p = wide; *p; p++)
    {
        if (*p == '/') *p = '\\';
        if (*p == '?' || *p == '#') *p = 0;     /* a query means nothing on a disk */
    }
    ok = PathCombineW( combined, doc->base_dir, wide ) != NULL && combined[0] != '\\';
    free( wide );
    if (ok) lstrcpynW( path, combined, MAX_PATH );
    return ok;
}

static void add_sheet( struct document *doc, const char *original, size_t original_len )
{
    lxb_css_stylesheet_t *sheet;
    size_t len;
    char *css;

    /* with the longhands of the shorthands lexbor drops (css.c) */
    if (!(css = css_rewrite( original, original_len, &len ))) return;

    if (!doc->css_parser)
    {
        doc->css_parser = lxb_css_parser_create();
        if (lxb_css_parser_init( doc->css_parser, NULL ) != LXB_STATUS_OK) goto done;
    }
    if (doc->sheet_count == doc->sheet_capacity)
    {
        unsigned int capacity = doc->sheet_capacity ? doc->sheet_capacity * 2 : 8;
        lxb_css_stylesheet_t **grown = realloc( doc->sheets, capacity * sizeof(*grown) );

        if (!grown) goto done;
        doc->sheets = grown;
        doc->sheet_capacity = capacity;
    }
    if (!(sheet = lxb_css_stylesheet_create( NULL ))) goto done;
    if (lxb_css_stylesheet_parse( sheet, doc->css_parser, (const lxb_char_t *)css, len ) != LXB_STATUS_OK ||
        lxb_html_document_stylesheet_attach( doc->html, sheet ) != LXB_STATUS_OK)
    {
        WARN( "a style sheet of %s did not parse\n", debugstr_w(doc->url) );
        lxb_css_stylesheet_destroy( sheet, true );
        goto done;
    }
    doc->sheets[doc->sheet_count++] = sheet;
done:
    free( css );
}

static BOOL attribute_has_word( lxb_dom_element_t *element, const char *name, const char *word )
{
    size_t len, word_len = strlen( word );
    const char *value = (const char *)lxb_dom_element_get_attribute( element, (const lxb_char_t *)name,
                                                                     strlen( name ), &len );

    for (size_t i = 0; value && i + word_len <= len; i++)
    {
        if ((i && value[i - 1] != ' ') || _strnicmp( value + i, word, word_len )) continue;
        if (i + word_len == len || value[i + word_len] == ' ') return TRUE;
    }
    return FALSE;
}

/* The sheets of <style> and <link rel=stylesheet>, in document order, so
 * that a later one wins a tie. lexbor parsed every <style> as it met it,
 * without the shorthands it drops: that sheet goes, the rewritten one takes
 * its place. A style="" keeps what lexbor parsed and gets the longhands of
 * its shorthands on top. */
static void load_sheets( struct document *doc, lxb_dom_node_t *node )
{
    for (node = lxb_dom_node_first_child( node ); node; node = lxb_dom_node_next( node ))
    {
        lxb_dom_element_t *element;
        const char *text;
        WCHAR path[MAX_PATH] = L"";
        size_t len, css_len;
        char *css;

        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        element = lxb_dom_interface_element( node );
        switch (lxb_dom_element_tag_id( element ))
        {
        case LXB_TAG_STYLE:
            if (lxb_html_interface_style( element )->stylesheet)
                lxb_html_style_element_remove( lxb_html_interface_style( element ) );
            if ((text = (const char *)lxb_dom_node_text_content( node, &len )))
                add_sheet( doc, text, len );
            continue;   /* nothing inside a <style> is an element */

        case LXB_TAG_LINK:
            if (!attribute_has_word( element, "rel", "stylesheet" )) break;
            text = (const char *)lxb_dom_element_get_attribute( element, (const lxb_char_t *)"href", 4, &len );
            if (text && resolve_path( doc, text, len, path ) && (css = read_file( path, &css_len )))
            {
                css = to_utf8( css, &css_len );
                add_sheet( doc, css, css_len );
                free( css );
            }
            else WARN( "no style sheet %s\n", debugstr_w(path) );
            break;

        default:
            break;
        }
        if ((text = (const char *)lxb_dom_element_get_attribute( element, (const lxb_char_t *)"style", 5, &len )) &&
            (css = css_expand_inline( text, len, &css_len )))
        {
            lxb_dom_element_style_parse( element, (const lxb_char_t *)css, css_len );
            free( css );
        }
        load_sheets( doc, node );
    }
}

static BOOL parse( struct document *doc, const char *html, size_t len )
{
    const lxb_char_t *title;
    size_t title_len;
    WCHAR *wide;

    ULONGLONG start = GetTickCount64(), parsed;

    if (!(doc->html = lxb_html_document_create())) return FALSE;
    if (lxb_style_init( doc->html ) != LXB_STATUS_OK ||
        lxb_html_document_parse( doc->html, (const lxb_char_t *)html, len ) != LXB_STATUS_OK)
    {
        ERR( "lexbor did not parse %s\n", debugstr_w(doc->url) );
        return FALSE;
    }
    parsed = GetTickCount64();
    load_sheets( doc, lxb_dom_interface_node( doc->html ) );
    TRACE( "%Iu bytes: parsed in %I64u ms, sheets in %I64u ms\n", len, parsed - start, GetTickCount64() - parsed );

    if ((title = lxb_html_document_title( doc->html, &title_len )) && title_len &&
        (wide = utf8_to_wide( (const char *)title, title_len, NULL )))
    {
        lstrcpynW( doc->title, wide, ARRAY_SIZE(doc->title) );
        free( wide );
    }
    return TRUE;
}

struct document *document_load( const WCHAR *url )
{
    struct document *doc = calloc( 1, sizeof(*doc) );
    WCHAR path[MAX_PATH];
    char *html = NULL;
    size_t len = 0;

    if (!doc) return NULL;
    lstrcpynW( doc->url, url, ARRAY_SIZE(doc->url) );

    if (!_wcsicmp( url, L"about:arctic" ) || !_wcsicmp( url, L"about:home" ))
        html = page_about( url, &len );
    else if (!_wcsicmp( url, L"about:blank" ))
    {
        html = _strdup( "<!DOCTYPE html><html><head><title></title></head><body></body></html>" );
        len = html ? strlen( html ) : 0;
    }
    else if (url_to_path( url, path ))
    {
        if ((html = read_file( path, &len )))
        {
            WCHAR *name;

            html = to_utf8( html, &len );
            lstrcpynW( doc->base_dir, path, ARRAY_SIZE(doc->base_dir) );
            if ((name = wcsrchr( doc->base_dir, '\\' ))) name[1] = 0;
        }
        else html = page_error( url, L"Файл не знайдено або його не прочитати.", &len );
    }
    else if (!_wcsnicmp( url, L"http:", 5 ) || !_wcsnicmp( url, L"https:", 6 ))
        html = page_error( url, L"Мережа — етап 2 рушія (docs/browser.md): поки він відкриває сторінки з диска "
                                L"(file:///…) і власні (about:arctic).", &len );
    else
        html = page_error( url, L"Рушій не знає такої адреси.", &len );

    if (!html || !parse( doc, html, len ))
    {
        free( html );
        document_free( doc );
        return NULL;
    }
    free( html );
    if (!doc->title[0])
    {
        const WCHAR *name = wcsrchr( url, '/' );

        lstrcpynW( doc->title, name && name[1] ? name + 1 : url, ARRAY_SIZE(doc->title) );
    }
    TRACE( "%s: %u linked sheets, title %s\n", debugstr_w(url), doc->sheet_count, debugstr_w(doc->title) );
    return doc;
}

void document_free( struct document *doc )
{
    if (!doc) return;
    if (doc->html)
    {
        lxb_style_destroy( doc->html );
        lxb_html_document_destroy( doc->html );
    }
    for (unsigned int i = 0; i < doc->sheet_count; i++) lxb_css_stylesheet_destroy( doc->sheets[i], true );
    free( doc->sheets );
    if (doc->css_parser) lxb_css_parser_destroy( doc->css_parser, true );
    for (unsigned int i = 0; i < doc->image_count; i++)
    {
        if (doc->images[i]->bitmap) DeleteObject( doc->images[i]->bitmap );
        free( doc->images[i] );
    }
    free( doc->images );
    free( doc );
}

/* the decoded image for an <img src>, once per path */
const struct image *document_image( struct document *doc, const char *src, size_t len )
{
    WCHAR path[MAX_PATH];
    struct image *image;

    if (!resolve_path( doc, src, len, path )) return NULL;
    for (unsigned int i = 0; i < doc->image_count; i++)
        if (!_wcsicmp( doc->images[i]->path, path )) return doc->images[i]->failed ? NULL : doc->images[i];

    if (doc->image_count == doc->image_capacity)
    {
        unsigned int capacity = doc->image_capacity ? doc->image_capacity * 2 : 16;
        struct image **grown = realloc( doc->images, capacity * sizeof(*grown) );

        if (!grown) return NULL;
        doc->images = grown;
        doc->image_capacity = capacity;
    }
    if (!(image = calloc( 1, sizeof(*image) ))) return NULL;
    lstrcpynW( image->path, path, MAX_PATH );
    image->failed = !image_decode( image );
    doc->images[doc->image_count++] = image;
    return image->failed ? NULL : image;
}
