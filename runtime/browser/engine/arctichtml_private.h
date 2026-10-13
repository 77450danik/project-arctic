/*
 * Arctic web engine: what the parts of the engine tell each other
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __ARCTICHTML_PRIVATE_H
#define __ARCTICHTML_PRIVATE_H

#include <windows.h>

#include <lexbor/html/html.h>
#include <lexbor/css/css.h>
#include <lexbor/style/style.h>

#include "arctic_html.h"
#include "log.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

/* The pixels of a page. A DIB section, so GDI draws text into the same
 * bytes the GPU uploads: until the glyph atlas of stage 3 the rasterizer is
 * on the processor either way, and the way to the screen is the same. */
struct surface
{
    HDC      dc;
    HBITMAP  bitmap, old_bitmap;
    DWORD   *bits;          /* 0xAARRGGBB, top down, width * height */
    int      width, height;
};

BOOL surface_create( struct surface *surface, int width, int height );
void surface_destroy( struct surface *surface );

/* --- fonts: one cache for measuring and for drawing --- */

struct font_key
{
    WCHAR face[LF_FACESIZE];
    int   px;           /* the height of the em */
    int   weight;
    BOOL  italic, underline, strike;
};

struct font
{
    struct font_key key;
    HFONT           handle;
    int             ascent, descent;
    int             space;          /* the width of a space */
};

enum generic_family { GENERIC_SERIF, GENERIC_SANS, GENERIC_MONO, GENERIC_CURSIVE, GENERIC_FANTASY,
                      GENERIC_SYSTEM_UI };

const struct font *font_get( const struct font_key *key );
HDC font_dc(void);      /* a DC to measure with */
int font_measure( const struct font *font, const WCHAR *text, int len );
void font_cache_clear(void);
BOOL font_family_exists( const WCHAR *name );
/* the face for a family a page names, if this machine has it or its alias */
BOOL font_face_for( const WCHAR *name, WCHAR face[LF_FACESIZE] );
void font_generic_face( enum generic_family generic, WCHAR face[LF_FACESIZE] );

/* --- images: decoded once, by WIC, as premultiplied BGRA --- */

struct image
{
    WCHAR    path[MAX_PATH];
    HBITMAP  bitmap;        /* a DIB section, for AlphaBlend */
    int      width, height;
    BOOL     failed;
};

BOOL image_decode( struct image *image );

/* --- the display list --- */

/* What to draw, in the order to draw it, in the coordinates of the document,
 * with nothing of the document left in it: the layout fills it, the
 * rasterizer reads it. That split is what lets the rasterizer move to the
 * GPU without the layout knowing. */
enum item_type
{
    ITEM_RECT,      /* a filled rectangle, color with alpha */
    ITEM_RUN,       /* a run of text on one baseline */
    ITEM_IMAGE,     /* an image scaled into its rectangle */
};

struct item
{
    enum item_type       type;
    RECT                 rect;      /* the box; for a run, its line box */
    DWORD                color;     /* 0xAARRGGBB */
    /* ITEM_RUN */
    const WCHAR         *text;      /* owned by the list's strings */
    int                  length;
    const struct font   *font;
    int                  x, baseline;
    /* ITEM_IMAGE */
    const struct image  *image;
};

struct display_list
{
    struct item  *items;
    unsigned int  count, capacity;
    WCHAR       **strings;      /* copies the list owns */
    unsigned int  string_count, string_capacity;
    DWORD         background;   /* the canvas: 0xAARRGGBB */
    int           width, height;    /* the viewport it was built for */
    int           content_height;   /* how far the document goes down */
};

void display_list_reset( struct display_list *list );
void display_list_free( struct display_list *list );
struct item *display_list_add( struct display_list *list, enum item_type type );
const WCHAR *display_list_string( struct display_list *list, const WCHAR *text, int length );

void raster_paint( struct surface *surface, const struct display_list *list, int scroll_y );

/* --- the document --- */

struct document
{
    lxb_html_document_t *html;
    lxb_css_parser_t    *css_parser;
    lxb_css_stylesheet_t **sheets;  /* the ones <link> brought: we free them */
    unsigned int         sheet_count, sheet_capacity;
    WCHAR                url[2084];
    WCHAR                base_dir[MAX_PATH];    /* a folder for relative paths, file: only */
    WCHAR                title[256];
    struct image       **images;
    unsigned int         image_count, image_capacity;
};

struct document *document_load( const WCHAR *url );
void document_free( struct document *doc );
const struct image *document_image( struct document *doc, const char *src, size_t len );
/* the built-in pages: about:arctic, and the one that says why a page did not load */
char *page_about( const WCHAR *url, size_t *len );
char *page_error( const WCHAR *url, const WCHAR *reason, size_t *len );

/* the text of a sheet with the longhands of the shorthands lexbor drops;
 * for a style="", only those longhands */
char *css_rewrite( const char *css, size_t len, size_t *out_len );
char *css_expand_inline( const char *style, size_t len, size_t *out_len );

/* --- styles --- */

enum display_kind { DISPLAY_NONE, DISPLAY_INLINE, DISPLAY_BLOCK, DISPLAY_LIST_ITEM, DISPLAY_INLINE_BLOCK };
enum white_space_kind { WS_NORMAL, WS_NOWRAP, WS_PRE, WS_PRE_WRAP, WS_PRE_LINE };
enum text_align_kind { ALIGN_LEFT, ALIGN_RIGHT, ALIGN_CENTER, ALIGN_JUSTIFY };
enum length_unit { LEN_AUTO, LEN_PX, LEN_PERCENT };
enum list_kind { LIST_NONE, LIST_DISC, LIST_DECIMAL };

struct length
{
    enum length_unit unit;
    float            value;
};

struct style
{
    enum display_kind     display;
    enum white_space_kind white_space;
    enum text_align_kind  text_align;
    enum list_kind        list;         /* the marker a list-item inside shows */
    struct font_key       font;
    float                 font_size;    /* px, unrounded */
    float                 line_height;  /* px; 0 = normal */
    float                 line_factor;  /* a unitless line-height, inherited as such */
    DWORD                 color;
    DWORD                 background;
    struct length         width, height;
    struct length         margin[4];    /* top, right, bottom, left */
    float                 padding[4];
    float                 border[4];
    DWORD                 border_color[4];
};

/* the style of the root element, before any author style */
void style_initial( struct style *style );
void style_compute( const struct style *parent, lxb_dom_element_t *element, struct style *style );
/* a length in px against the containing width, auto counting as 0 */
float length_px( const struct length *length, float containing );

/* --- layout --- */

/* The whole document, laid out for a viewport width, into the list. */
void layout_document( struct document *doc, struct display_list *list, int width, int height );

/* --- presentation --- */

/* The way to the screen: Vulkan when the card and the driver allow it,
 * BitBlt when they do not or when ARCTIC_HTML_CPU is set. */
struct present;

struct present *present_create( HWND hwnd );
void present_destroy( struct present *present );
BOOL present_surface( struct present *present, const struct surface *surface );
BOOL present_on_gpu( const struct present *present );

/* --- the view --- */

/* The window of the engine inside the host's window, named
 * "Internet Explorer_Server" as in Windows. */
extern HINSTANCE arctichtml_instance;

BOOL view_register_class(void);
struct arctic_html_view *view_create( HWND parent, const RECT *rect );
void view_destroy( struct arctic_html_view *view );
void view_set_rect( struct arctic_html_view *view, const RECT *rect );
HWND view_window( struct arctic_html_view *view );
HRESULT view_navigate( struct arctic_html_view *view, const WCHAR *url );
HRESULT view_title( struct arctic_html_view *view, WCHAR *buffer, UINT count );
BOOL view_on_gpu( struct arctic_html_view *view );

/* UTF-8 of lexbor into a fresh UTF-16 string the caller frees */
WCHAR *utf8_to_wide( const char *text, size_t len, int *out_len );

#endif /* __ARCTICHTML_PRIVATE_H */
