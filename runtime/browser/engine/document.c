/*
 * Arctic web engine: a document, from its address to lexbor's tree
 *
 * Everything the page needs is fetched here, off the window's thread (the
 * view's loader): the page, its style sheets and its images, so the window
 * only lays out and draws. The network is net/ (stage 2); the engine's own
 * pages are about:.
 *
 * lexbor parses the HTML, and with its style module on it also parses every
 * <style> as it meets it and matches the rules to the elements. Those
 * sheets lack the shorthands lexbor drops (css.c), so they are swapped for
 * rewritten ones, in document order with the sheets of <link>.
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

#include "arctichtml_private.h"
#include "../net/net.h"

#define IMAGE_THREADS 6
#define MAX_IMAGES 400

/* The code pages of the charsets Ukrainian and Western pages are written in;
 * anything else is taken as UTF-8, as the encoding standard does for new
 * pages. */
UINT charset_code_page( const char *name, size_t len )
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

/* lexbor wants UTF-8: anything else is converted before it parses. The
 * byte order mark wins, then what the server said, then the page's <meta>. */
static char *to_utf8( char *data, size_t *len, UINT declared )
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
        cp = declared != CP_UTF8 ? declared : sniff_code_page( data, *len );
        if (cp == CP_UTF8) return data;
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

char *document_resolve( const struct document *doc, const char *ref, size_t len )
{
    return url_resolve( doc->base ? doc->base : doc->address, ref, len );
}

/* --- style sheets --- */

static void add_sheet( struct document *doc, const struct css_context *ctx, const char *original, size_t original_len )
{
    lxb_css_stylesheet_t *sheet;
    size_t len;
    char *css;

    ULONGLONG start = GetTickCount64(), rewritten;

    /* @media evaluated, var() replaced, shorthands expanded (css.c) */
    if (!(css = css_rewrite( ctx, original, original_len, &len ))) return;
    rewritten = GetTickCount64();
    {
        /* ARCTIC_HTML_DUMP_CSS=<folder>: what lexbor is given, to see why a page looks as it does */
        WCHAR folder[MAX_PATH], path[MAX_PATH];
        static LONG dumped;
        HANDLE file;
        DWORD written;

        if (GetEnvironmentVariableW( L"ARCTIC_HTML_DUMP_CSS", folder, ARRAY_SIZE(folder) ))
        {
            swprintf( path, ARRAY_SIZE(path), L"%s\\sheet-%ld.css", folder, InterlockedIncrement( &dumped ) );
            file = CreateFileW( path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL );
            if (file != INVALID_HANDLE_VALUE)
            {
                WriteFile( file, css, (DWORD)len, &written, NULL );
                CloseHandle( file );
            }
        }
    }
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
        WARN( "a style sheet of %s did not parse\n", doc->address );
        lxb_css_stylesheet_destroy( sheet, true );
        goto done;
    }
    doc->sheets[doc->sheet_count++] = sheet;
    TRACE( "sheet of %Iu bytes: rewritten in %I64u ms, parsed and matched in %I64u ms\n", original_len,
           rewritten - start, GetTickCount64() - rewritten );
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

static const char *attribute( lxb_dom_element_t *element, const char *name, size_t *len )
{
    return (const char *)lxb_dom_element_get_attribute( element, (const lxb_char_t *)name, strlen( name ), len );
}

/* the text of every sheet, in document order */
struct sheet_texts
{
    char        **texts;
    size_t       *lengths;
    char        **urls;     /* of a <link>, to fetch; NULL for a <style> */
    unsigned int  count, capacity;
    struct document *doc;
    LONG          next;     /* the next link a fetching thread takes */
};

static void keep_sheet( struct sheet_texts *sheets, char *text, size_t len, char *url )
{
    if (sheets->count == sheets->capacity)
    {
        unsigned int capacity = sheets->capacity ? sheets->capacity * 2 : 16;
        char **texts = realloc( sheets->texts, capacity * sizeof(*texts) );
        size_t *lengths = texts ? realloc( sheets->lengths, capacity * sizeof(*lengths) ) : NULL;
        char **urls = lengths ? realloc( sheets->urls, capacity * sizeof(*urls) ) : NULL;

        if (texts) sheets->texts = texts;
        if (lengths) sheets->lengths = lengths;
        if (!texts || !lengths || !urls) { free( text ); free( url ); return; }
        sheets->urls = urls;
        sheets->capacity = capacity;
    }
    sheets->texts[sheets->count] = text;
    sheets->urls[sheets->count] = url;
    sheets->lengths[sheets->count++] = len;
}

static BOOL media_holds( lxb_dom_element_t *element, const struct css_context *ctx )
{
    size_t len;
    const char *media = attribute( element, "media", &len );

    return !media || css_media_matches( ctx, media, len );
}

/* the sheets of <link>, fetched side by side: a page may have thirty */
static DWORD WINAPI sheet_thread( void *arg )
{
    struct sheet_texts *sheets = arg;
    LONG i;

    while ((i = InterlockedIncrement( &sheets->next ) - 1) < (LONG)sheets->count)
    {
        struct response response;
        ULONGLONG start = GetTickCount64();

        if (!sheets->urls[i]) continue;
        if (net_fetch( sheets->urls[i], sheets->doc->address, REQUEST_STYLESHEET, &response ) &&
            response.status < 400 && response.body)
        {
            sheets->texts[i] = to_utf8( response.body, &response.length,
                                        content_type_code_page( response.content_type ) );
            sheets->lengths[i] = response.length;
            response.body = NULL;
        }
        else if (response.blocked) InterlockedIncrement( (LONG *)&sheets->doc->blocked );
        else WARN( "no style sheet %s: %d %s\n", sheets->urls[i], response.status, response.error );
        TRACE( "%s in %I64u ms\n", sheets->urls[i], GetTickCount64() - start );
        response_free( &response );
    }
    return 0;
}

static void fetch_sheets( struct sheet_texts *sheets )
{
    HANDLE threads[6];
    unsigned int links = 0, count = 0;

    for (unsigned int i = 0; i < sheets->count; i++) if (sheets->urls[i]) links++;
    if (!links) return;
    sheets->next = 0;
    for (unsigned int i = 0; i < min( links, ARRAY_SIZE(threads) ); i++)
        if ((threads[count] = CreateThread( NULL, 0, sheet_thread, sheets, 0, NULL ))) count++;
    if (!count) sheet_thread( sheets );
    WaitForMultipleObjects( count, threads, TRUE, INFINITE );
    for (unsigned int i = 0; i < count; i++) CloseHandle( threads[i] );
}

static void link_sheet( struct document *doc, struct sheet_texts *sheets, const char *href, size_t href_len )
{
    char *url = document_resolve( doc, href, href_len );

    if (!url) return;
    /* the same sheet twice changes nothing: GitHub links some six times */
    for (unsigned int i = 0; i < sheets->count; i++)
        if (sheets->urls[i] && !strcmp( sheets->urls[i], url ))
        {
            free( url );
            return;
        }
    keep_sheet( sheets, NULL, 0, url );
}
/* The sheets of <style> and <link rel=stylesheet>, in document order, so
 * that a later one wins a tie. lexbor parsed every <style> as it met it, but
 * its style module cannot take @media, var() and some shorthands (css.c):
 * that sheet goes, a rewritten one takes its place. */
static void gather_sheets( struct document *doc, const struct css_context *ctx, struct sheet_texts *sheets,
                           lxb_dom_node_t *node, BOOL fetch )
{
    for (node = lxb_dom_node_first_child( node ); node; node = lxb_dom_node_next( node ))
    {
        lxb_dom_element_t *element;
        const char *text;
        size_t len;
        char *copy;

        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        element = lxb_dom_interface_element( node );
        switch (lxb_dom_element_tag_id( element ))
        {
        case LXB_TAG_STYLE:
            if (lxb_html_interface_style( element )->stylesheet)
                lxb_html_style_element_remove( lxb_html_interface_style( element ) );
            if (media_holds( element, ctx ) && (text = (const char *)lxb_dom_node_text_content( node, &len )) &&
                (copy = malloc( len + 1 )))
            {
                memcpy( copy, text, len );
                copy[len] = 0;
                keep_sheet( sheets, copy, len, NULL );
            }
            continue;   /* nothing inside a <style> is an element */

        case LXB_TAG_LINK:
            if (fetch && attribute_has_word( element, "rel", "stylesheet" ) &&
                !attribute_has_word( element, "rel", "alternate" ) && media_holds( element, ctx ) &&
                (text = attribute( element, "href", &len )))
                link_sheet( doc, sheets, text, len );
            break;

        default:
            break;
        }
        gather_sheets( doc, ctx, sheets, node, fetch );
    }
}

/* a style="" keeps what lexbor parsed and gets what it lacked on top */
static void inline_styles( const struct css_context *ctx, lxb_dom_node_t *node )
{
    for (node = lxb_dom_node_first_child( node ); node; node = lxb_dom_node_next( node ))
    {
        const char *text;
        size_t len, css_len;
        char *css;

        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if ((text = attribute( lxb_dom_interface_element( node ), "style", &len )) &&
            (css = css_expand_inline( ctx, text, len, &css_len )))
        {
            lxb_dom_element_style_parse( lxb_dom_interface_element( node ), (const lxb_char_t *)css, css_len );
            free( css );
        }
        inline_styles( ctx, node );
    }
}

/* the classes and ids of the document, for css.c to leave out the rules of others */
static void collect_names( struct css_context *ctx, lxb_dom_node_t *node )
{
    for (node = lxb_dom_node_first_child( node ); node; node = lxb_dom_node_next( node ))
    {
        lxb_dom_element_t *element;
        const char *value;
        size_t len;

        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        element = lxb_dom_interface_element( node );
        if ((value = attribute( element, "id", &len ))) css_context_add_name( ctx, value, len );
        if ((value = attribute( element, "class", &len )))
        {
            for (size_t i = 0; i < len; )
            {
                size_t start;

                while (i < len && (value[i] == ' ' || value[i] == '\t' || value[i] == '\n' || value[i] == '\r')) i++;
                start = i;
                while (i < len && value[i] != ' ' && value[i] != '\t' && value[i] != '\n' && value[i] != '\r') i++;
                css_context_add_name( ctx, value + start, i - start );
            }
        }
        collect_names( ctx, node );
    }
}

static void load_sheets( struct document *doc, int width, int height, BOOL fetch )
{
    struct sheet_texts sheets = { 0 };
    struct css_context ctx;
    lxb_dom_node_t *root = lxb_dom_interface_node( doc->html );

    css_context_init( &ctx, width, height );
    collect_names( &ctx, root );
    sheets.doc = doc;
    gather_sheets( doc, &ctx, &sheets, root, fetch );
    fetch_sheets( &sheets );
    /* the custom properties of all of them first: a sheet uses what a later one sets */
    for (unsigned int i = 0; i < sheets.count; i++)
        if (sheets.texts[i]) css_collect( &ctx, sheets.texts[i], sheets.lengths[i] );
    for (unsigned int i = 0; i < sheets.count; i++)
    {
        if (sheets.texts[i]) add_sheet( doc, &ctx, sheets.texts[i], sheets.lengths[i] );
        free( sheets.texts[i] );
        free( sheets.urls[i] );
    }
    free( sheets.urls );
    inline_styles( &ctx, root );
    TRACE( "%u sheets, %u custom properties\n", sheets.count, ctx.var_count );
    free( sheets.texts );
    free( sheets.lengths );
    css_context_free( &ctx );
}

/* --- images, fetched side by side --- */

struct image_job
{
    struct image   *image;
    struct response response;
};

struct image_jobs
{
    struct document  *doc;
    struct image_job *jobs;
    unsigned int      count;
    LONG              next;
};

static struct image *find_image( struct document *doc, const char *url )
{
    for (unsigned int i = 0; i < doc->image_count; i++)
        if (!strcmp( doc->images[i]->url, url )) return doc->images[i];
    return NULL;
}

static struct image *add_image( struct document *doc, char *url )
{
    struct image *image;

    if (doc->image_count == doc->image_capacity)
    {
        unsigned int capacity = doc->image_capacity ? doc->image_capacity * 2 : 16;
        struct image **grown = realloc( doc->images, capacity * sizeof(*grown) );

        if (!grown) return NULL;
        doc->images = grown;
        doc->image_capacity = capacity;
    }
    if (!(image = calloc( 1, sizeof(*image) ))) return NULL;
    image->url = url;
    image->failed = TRUE;   /* until it comes */
    doc->images[doc->image_count++] = image;
    return image;
}

static void collect_images( struct document *doc, lxb_dom_node_t *node )
{
    for (node = lxb_dom_node_first_child( node ); node && doc->image_count < MAX_IMAGES; node = lxb_dom_node_next( node ))
    {
        lxb_dom_element_t *element;
        const char *src;
        size_t len;
        char *url;

        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        element = lxb_dom_interface_element( node );
        if (lxb_dom_element_tag_id( element ) == LXB_TAG_IMG && (src = attribute( element, "src", &len )) && len &&
            (url = document_resolve( doc, src, len )))
        {
            if (find_image( doc, url ) || !add_image( doc, url )) free( url );
        }
        collect_images( doc, node );
    }
}

/* the <img>s a page's scripts added since it loaded: each gets its place, failed until it
 * comes; their addresses come back for the view to fetch on a thread of its own */
unsigned int document_new_images( struct document *doc, char ***urls )
{
    unsigned int before = doc->image_count, count;

    *urls = NULL;
    if (!doc->html) return 0;
    collect_images( doc, lxb_dom_interface_node( doc->html ) );
    if (!(count = doc->image_count - before) || !(*urls = calloc( count, sizeof(char *) ))) return 0;
    for (unsigned int i = 0; i < count; i++) (*urls)[i] = _strdup( doc->images[before + i]->url );
    return count;
}

/* an image fetched and decoded on another thread: its picture goes to its place */
BOOL document_set_image( struct document *doc, const char *url, struct image *decoded )
{
    struct image *image = find_image( doc, url );

    if (!image || !image->failed) return FALSE;
    image->bitmap = decoded->bitmap;
    image->width = decoded->width;
    image->height = decoded->height;
    image->failed = decoded->failed;
    decoded->bitmap = NULL;
    return TRUE;
}

/* fetched and decoded, for a page that shows: the view does it on a thread of its own */
BOOL document_fetch_image( const char *url, const char *page, struct image *image )
{
    struct response response;
    BOOL ok = FALSE;

    memset( image, 0, sizeof(*image) );
    image->failed = TRUE;
    if (net_fetch( url, page, REQUEST_IMAGE, &response ) && response.status < 400 && response.body &&
        image_decode( image, response.body, response.length ))
    {
        image->failed = FALSE;
        ok = TRUE;
    }
    response_free( &response );
    return ok;
}

static DWORD WINAPI image_thread( void *arg )
{
    struct image_jobs *jobs = arg;
    LONG i;

    while ((i = InterlockedIncrement( &jobs->next ) - 1) < (LONG)jobs->count)
    {
        struct image_job *job = &jobs->jobs[i];

        net_fetch( job->image->url, jobs->doc->address, REQUEST_IMAGE, &job->response );
    }
    return 0;
}

static void load_images( struct document *doc )
{
    struct image_jobs jobs = { doc };
    HANDLE threads[IMAGE_THREADS];
    unsigned int thread_count = 0;

    collect_images( doc, lxb_dom_interface_node( doc->html ) );
    if (!doc->image_count || !(jobs.jobs = calloc( doc->image_count, sizeof(*jobs.jobs) ))) return;
    jobs.count = doc->image_count;
    for (unsigned int i = 0; i < jobs.count; i++) jobs.jobs[i].image = doc->images[i];

    for (unsigned int i = 0; i < min( IMAGE_THREADS, jobs.count ); i++)
        if ((threads[thread_count] = CreateThread( NULL, 0, image_thread, &jobs, 0, NULL ))) thread_count++;
    if (!thread_count) image_thread( &jobs );
    WaitForMultipleObjects( thread_count, threads, TRUE, INFINITE );
    for (unsigned int i = 0; i < thread_count; i++) CloseHandle( threads[i] );

    /* decoded here, on one thread: WIC lives in this thread's apartment */
    for (unsigned int i = 0; i < jobs.count; i++)
    {
        struct image_job *job = &jobs.jobs[i];

        if (job->response.blocked) doc->blocked++;
        else if (job->response.body && job->response.status < 400)
            job->image->failed = !image_decode( job->image, job->response.body, job->response.length );
        response_free( &job->response );
    }
    free( jobs.jobs );
}

/* --- the document --- */

static void find_base( struct document *doc, lxb_dom_node_t *node )
{
    for (node = lxb_dom_node_first_child( node ); node && !doc->base; node = lxb_dom_node_next( node ))
    {
        const char *href;
        size_t len;

        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (lxb_dom_element_tag_id( lxb_dom_interface_element( node ) ) == LXB_TAG_BASE &&
            (href = attribute( lxb_dom_interface_element( node ), "href", &len )) && len)
        {
            doc->base = url_resolve( doc->address, href, len );
            return;
        }
        find_base( doc, node );
    }
}

static struct document *load_once( const WCHAR *url, const struct post_data *post, int width, int height,
                                   struct document *parent );
static BOOL no_scripts(void);
/* the name of the frame being loaded on this thread: its document takes it before its scripts run */
static _Thread_local const char *tls_frame_name;

/* --- frames --- */

#define MAX_FRAME_DEPTH 3
#define MAX_FRAMES      16

struct frame *document_frame( struct document *doc, lxb_dom_element_t *element, BOOL create )
{
    struct frame *frame;

    for (unsigned int i = 0; i < doc->frame_count; i++) if (doc->frames[i]->element == element) return doc->frames[i];
    if (!create || doc->frame_count >= MAX_FRAMES || doc->depth >= MAX_FRAME_DEPTH) return NULL;
    if (doc->frame_count == doc->frame_capacity)
    {
        unsigned int capacity = doc->frame_capacity ? doc->frame_capacity * 2 : 4;
        struct frame **grown = realloc( doc->frames, capacity * sizeof(*grown) );

        if (!grown) return NULL;
        doc->frames = grown;
        doc->frame_capacity = capacity;
    }
    if (!(frame = calloc( 1, sizeof(*frame) ))) return NULL;
    frame->element = element;
    doc->frames[doc->frame_count++] = frame;
    return frame;
}

char *frame_url( struct document *doc, lxb_dom_element_t *element )
{
    size_t len;
    const char *src = (const char *)lxb_dom_element_get_attribute( element, (const lxb_char_t *)"src", 3, &len );

    if (!src || !len) return _strdup( "about:blank" );
    return document_resolve( doc, src, len );
}

static int frame_size( lxb_dom_element_t *element, const char *name, int fallback )
{
    size_t len;
    const char *value = (const char *)lxb_dom_element_get_attribute( element, (const lxb_char_t *)name, strlen( name ), &len );

    return value && atoi( value ) > 0 ? atoi( value ) : fallback;
}

/* the frames the page has as it loads, each loaded in turn */
static void load_frames( struct document *doc, lxb_dom_node_t *node )
{
    for (node = lxb_dom_node_first_child( node ); node; node = lxb_dom_node_next( node ))
    {
        lxb_dom_element_t *element;
        struct frame *frame;

        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        element = lxb_dom_interface_element( node );
        /* what is for browsers without scripts is not ours, when the page has scripts run */
        if (lxb_dom_element_tag_id( element ) == LXB_TAG_NOSCRIPT && !no_scripts()) continue;
        if (lxb_dom_element_tag_id( element ) == LXB_TAG_IFRAME)
        {
            if ((frame = document_frame( doc, element, TRUE )) && !frame->doc && !frame->loading)
            {
                size_t name_len;
                const char *name_attr = (const char *)lxb_dom_element_get_attribute( element, (const lxb_char_t *)"name", 4, &name_len );
                char *name = name_attr ? _strdup( name_attr ) : NULL;

                frame->url = frame_url( doc, element );
                frame->doc = document_load_frame( frame->url, doc, frame_size( element, "width", 300 ),
                                                  frame_size( element, "height", 150 ), name );
                free( name );
            }
            continue;
        }
        load_frames( doc, node );
    }
}

static struct document *frame_doc_named( struct document *doc, const char *name )
{
    if (doc && name && *name) doc->frame_name = _strdup( name );
    return doc;
}

struct document *document_load_frame( const char *url, struct document *parent, int width, int height,
                                      const char *name )
{
    WCHAR *wide = url ? utf8_to_wide( url, strlen( url ), NULL ) : NULL;
    struct document *doc;

    if (!wide) return NULL;
    TRACE( "frame %s in %s\n", url, parent->address );
    tls_frame_name = name;      /* its scripts see their name as they load */
    doc = load_once( wide, NULL, width, height, parent );
    free( wide );
    /* a frame that navigates itself while it loads goes there, once */
    if (doc && doc->redirect)
    {
        char *next = doc->redirect;

        doc->redirect = NULL;
        document_free( doc );
        wide = utf8_to_wide( next, strlen( next ), NULL );
        free( next );
        doc = wide ? load_once( wide, NULL, width, height, parent ) : NULL;
        free( wide );
        if (doc && doc->redirect) { free( doc->redirect ); doc->redirect = NULL; }
    }
    tls_frame_name = NULL;
    return frame_doc_named( doc, name );
}

struct document *document_owning( struct document *root, lxb_dom_element_t *element )
{
    if (!root || !element) return NULL;
    if (lxb_dom_interface_node( element )->owner_document == lxb_dom_interface_document( root->html )) return root;
    for (unsigned int i = 0; i < root->frame_count; i++)
    {
        struct document *found = document_owning( root->frames[i]->doc, element );

        if (found) return found;
    }
    return NULL;
}

/* ARCTIC_HTML_NOSCRIPT=1: pages as they were before the engine ran scripts */
static BOOL no_scripts(void)
{
    static int off = -1;

    if (off < 0) off = GetEnvironmentVariableA( "ARCTIC_HTML_NOSCRIPT", NULL, 0 ) > 0;
    return off;
}

static void load_frames( struct document *doc, lxb_dom_node_t *node );

static BOOL parse( struct document *doc, const char *html, size_t len, BOOL fetch, int width, int height )
{
    const lxb_char_t *title;
    size_t title_len;
    WCHAR *wide;
    ULONGLONG start = GetTickCount64(), parsed, sheets;

    if (!(doc->html = lxb_html_document_create())) return FALSE;
    if (lxb_style_init( doc->html ) != LXB_STATUS_OK ||
        lxb_html_document_parse( doc->html, (const lxb_char_t *)html, len ) != LXB_STATUS_OK)
    {
        ERR( "lexbor did not parse %s\n", doc->address );
        return FALSE;
    }
    parsed = GetTickCount64();
    if (fetch) find_base( doc, lxb_dom_interface_node( doc->html ) );
    /* the scripts change the tree before it is styled; one that navigates ends the page */
    if (fetch && !no_scripts())
    {
        script_run_document( doc, width, height );
        if (doc->redirect) return TRUE;
    }
    load_sheets( doc, width, height, fetch );
    sheets = GetTickCount64();
    if (fetch) load_images( doc );
    if (fetch) load_frames( doc, lxb_dom_interface_node( doc->html ) );
    TRACE( "%Iu bytes: parsed in %I64u ms, sheets in %I64u ms, %u images in %I64u ms, %u blocked\n", len,
           parsed - start, sheets - parsed, doc->image_count, GetTickCount64() - sheets, doc->blocked );

    if ((title = lxb_html_document_title( doc->html, &title_len )) && title_len &&
        (wide = utf8_to_wide( (const char *)title, title_len, NULL )))
    {
        lstrcpynW( doc->title, wide, ARRAY_SIZE(doc->title) );
        free( wide );
    }
    return TRUE;
}

/* what is not HTML, in HTML: text in <pre>, an image on its own */
static char *wrap( const struct response *response, const char *url, size_t *len )
{
    const char *type = response->content_type ? response->content_type : "";
    size_t size = response->length * 6 + strlen( url ) * 6 + 256, n = 0;
    char *html;

    if (!(html = malloc( size ))) return NULL;
    if (!_strnicmp( type, "image/", 6 ))
        n = snprintf( html, size, "<!DOCTYPE html><html><body style=\"margin:0;background:#0e0e0e\">"
                      "<img src=\"%s\"></body></html>", url );
    else
    {
        n = snprintf( html, size, "<!DOCTYPE html><html><body><pre>" );
        for (size_t i = 0; i < response->length; i++)
        {
            const char *escaped = response->body[i] == '<' ? "&lt;" : response->body[i] == '&' ? "&amp;" : NULL;

            if (escaped) { memcpy( html + n, escaped, strlen( escaped ) ); n += strlen( escaped ); }
            else html[n++] = response->body[i];
        }
        n += snprintf( html + n, size - n, "</pre></body></html>" );
    }
    *len = n;
    return html;
}

static char *error_page( const char *url, const char *reason, size_t *len )
{
    WCHAR *wide_url = utf8_to_wide( url, strlen( url ), NULL ), *wide_reason = utf8_to_wide( reason, strlen( reason ), NULL );
    char *html = page_error( wide_url ? wide_url : L"", wide_reason ? wide_reason : L"", len );

    free( wide_url );
    free( wide_reason );
    return html;
}


/* the page, and where its scripts send it - a few times at most, as a loop of them would go on */
struct document *document_load( const WCHAR *url, const struct post_data *post, int width, int height )
{
    struct document *doc = load_once( url, post, width, height, NULL );

    for (int hop = 0; doc && doc->redirect && hop < 5; hop++)
    {
        WCHAR *next = utf8_to_wide( doc->redirect, strlen( doc->redirect ), NULL );

        document_free( doc );
        if (!next) return NULL;
        doc = load_once( next, NULL, width, height, NULL );
        free( next );
    }
    if (doc && doc->redirect)
    {
        free( doc->redirect );
        doc->redirect = NULL;
    }
    return doc;
}

static struct document *load_once( const WCHAR *url, const struct post_data *post, int width, int height,
                                   struct document *parent )
{
    struct document *doc = calloc( 1, sizeof(*doc) );
    struct response response = { 0 };
    char *html = NULL;
    size_t len = 0;
    BOOL fetch = FALSE;

    if (!doc) return NULL;
    doc->parent = parent;
    doc->depth = parent ? parent->depth + 1 : 0;
    if (parent && tls_frame_name && *tls_frame_name) doc->frame_name = _strdup( tls_frame_name );
    lstrcpynW( doc->url, url, ARRAY_SIZE(doc->url) );

    if (!_wcsicmp( url, L"about:arctic" ) || !_wcsicmp( url, L"about:home" ))
        html = page_about( url, &len );
    else if (!_wcsicmp( url, L"about:blank" ))
    {
        html = _strdup( "<!DOCTYPE html><html><head><title></title></head><body></body></html>" );
        len = html ? strlen( html ) : 0;
    }
    else if (!(doc->address = url_from_typed( url )))
        html = page_error( url, L"Адреса не читається.", &len );
    else if (post ? !net_post( doc->address, post->referrer, post->body, post->length, &response )
                  : !net_fetch( doc->address, parent ? parent->address : NULL, parent ? REQUEST_FRAME : REQUEST_DOCUMENT,
                                &response ))
        html = error_page( doc->address, response.error, &len );
    else
    {
        const char *type = response.content_type ? response.content_type : "text/html";

        /* where the page is at the end: its addresses are relative to that */
        free( doc->address );
        doc->address = response.url;
        response.url = NULL;
        if (!MultiByteToWideChar( CP_UTF8, 0, doc->address, -1, doc->url, ARRAY_SIZE(doc->url) ))
            lstrcpynW( doc->url, url, ARRAY_SIZE(doc->url) );

        if (!_strnicmp( type, "text/html", 9 ) || !_strnicmp( type, "application/xhtml", 17 ) || !*type)
        {
            html = to_utf8( response.body, &response.length, content_type_code_page( type ) );
            len = response.length;
            response.body = NULL;
            fetch = TRUE;
        }
        else if (!_strnicmp( type, "text/", 5 ) || !_strnicmp( type, "image/", 6 ))
        {
            response.body = to_utf8( response.body, &response.length, content_type_code_page( type ) );
            html = wrap( &response, doc->address, &len );
            fetch = !_strnicmp( type, "image/", 6 );
        }
        else html = error_page( doc->address, "Рушій не показує файлів такого типу.", &len );
    }
    response_free( &response );
    if (!doc->address) doc->address = _strdup( "about:blank" );

    if (!html || !parse( doc, html, len, fetch, width, height ))
    {
        free( html );
        document_free( doc );
        return NULL;
    }
    free( html );
    if (!doc->title[0])
    {
        const WCHAR *name = wcsrchr( doc->url, '/' );

        lstrcpynW( doc->title, name && name[1] ? name + 1 : doc->url, ARRAY_SIZE(doc->title) );
    }
    return doc;
}

void document_free( struct document *doc )
{
    if (!doc) return;
    script_free( doc->script );     /* before the tree its objects stand for */
    doc->script = NULL;
    for (unsigned int i = 0; i < doc->frame_count; i++)
    {
        document_free( doc->frames[i]->doc );
        free( doc->frames[i]->url );
        free( doc->frames[i] );
    }
    free( doc->frames );
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
        free( doc->images[i]->url );
        free( doc->images[i] );
    }
    free( doc->images );
    free( doc->address );
    free( doc->base );
    free( doc->redirect );
    free( doc->frame_name );
    free( doc );
}

const struct image *document_image( struct document *doc, const char *src, size_t len )
{
    char *url = document_resolve( doc, src, len );
    const struct image *image = url ? find_image( doc, url ) : NULL;

    free( url );
    return image && !image->failed ? image : NULL;
}
