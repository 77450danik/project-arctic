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

#include <winsock2.h>   /* before windows.h, or winsock.h comes in its place */
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
    char    *url;           /* absolute, UTF-8 */
    HBITMAP  bitmap;        /* a DIB section, for AlphaBlend */
    int      width, height;
    BOOL     failed;
};

BOOL image_decode( struct image *image, const void *data, size_t len );

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
    /* the item shows only inside this: the box of a form control, or of overflow: hidden */
    BOOL                 clipped;
    RECT                 clip;
    int                  layer;     /* 0 the flow, 1 positioned boxes: drawn over the flow */
    int                  absolute;  /* how many absolute boxes it is inside: they do not size their parents */
    lxb_dom_element_t   *owner;     /* the element it was drawn for: what a click on it hits */
};

/* where a link is on the page: what a click on it opens */
struct link_area
{
    RECT               rect;
    lxb_dom_element_t *element;     /* the <a href> */
};

/* a form control on the page: what a click on it does, where its text is */
enum control_kind
{
    CONTROL_TEXT,       /* input of text, search, email, password...; textarea */
    CONTROL_CHECKBOX,
    CONTROL_RADIO,
    CONTROL_SUBMIT,     /* input type=submit or image, button that submits */
    CONTROL_BUTTON,     /* input type=button or reset, button type=button: nothing without scripts */
    CONTROL_SELECT,
};

struct control_area
{
    RECT               rect;        /* the border box */
    lxb_dom_element_t *element;
    enum control_kind  kind;
    int                text_x;      /* where the value's first character stands, scrolled */
    const struct font *font;
};

/* where a block with an id begins: what #fragment of an address scrolls to */
struct anchor
{
    int                y;
    lxb_dom_element_t *element;
};

struct display_list
{
    struct item  *items;
    unsigned int  count, capacity;
    struct link_area *links;
    unsigned int  link_count, link_capacity;
    struct anchor *anchors;
    unsigned int  anchor_count, anchor_capacity;
    struct control_area *controls;
    unsigned int  control_count, control_capacity;
    WCHAR       **strings;      /* copies the list owns */
    unsigned int  string_count, string_capacity;
    DWORD         background;   /* the canvas: 0xAARRGGBB */
    int           width, height;    /* the viewport it was built for */
    int           content_height;   /* how far the document goes down */
    int           layer;            /* what display_list_add gives a new item */
    int           absolute;         /* and how deep in absolute boxes it is */
    lxb_dom_element_t *owner;       /* and for which element */
};

void display_list_reset( struct display_list *list );
void display_list_free( struct display_list *list );
struct item *display_list_add( struct display_list *list, enum item_type type );
const WCHAR *display_list_string( struct display_list *list, const WCHAR *text, int length );
void display_list_link( struct display_list *list, const RECT *rect, lxb_dom_element_t *element );
void display_list_anchor( struct display_list *list, int y, lxb_dom_element_t *element );
/* the element drawn topmost at a point of the document, or NULL */
lxb_dom_element_t *display_list_hit( const struct display_list *list, int x, int y );
struct control_area *display_list_control( struct display_list *list );
/* what an inline-block laid out at the origin left in the list, moved to its place on the line */
struct list_mark
{
    unsigned int items, links, anchors, controls;
    unsigned int deferred;      /* the layout's positioned boxes waiting for their containing block */
};
void display_list_mark( const struct display_list *list, struct list_mark *mark );
void display_list_shift( struct display_list *list, const struct list_mark *from, const struct list_mark *to,
                         int dx, int dy );

void raster_paint( struct surface *surface, const struct display_list *list, int scroll_y );

/* --- the document --- */

struct document
{
    lxb_html_document_t *html;
    lxb_css_parser_t    *css_parser;
    lxb_css_stylesheet_t **sheets;  /* the ones we parsed: we free them */
    unsigned int         sheet_count, sheet_capacity;
    char                *address;   /* UTF-8, where the page came from at the end, after redirects */
    char                *base;      /* what its addresses are relative to: <base href> or the address */
    WCHAR                url[2084]; /* the address, for the address bar */
    WCHAR                title[256];
    struct image       **images;
    unsigned int         image_count, image_capacity;
    unsigned int         blocked;   /* requests the filter took */
    char                *redirect;  /* where its scripts sent the page, UTF-8, or NULL */
    struct script_env   *script;    /* its scripts, alive while it shows */
    struct document     *parent;    /* of a frame's document: the page the frame is in */
    char                *frame_name;    /* the name="" of its <iframe>: its window.name */
    int                  depth;     /* how deep in frames */
    struct frame       **frames;    /* its <iframe>s and their documents */
    unsigned int         frame_count, frame_capacity;
    lxb_dom_element_t   *focus;     /* the form control that takes the keys, or NULL */
    int                  caret;     /* in its value, in UTF-16 units */
};

/* an <iframe> of a document, and the document in it once it came */
struct frame
{
    lxb_dom_element_t *element;
    struct document   *doc;
    BOOL               loading;
    char              *url;     /* what it was loaded from */
};

/* what a form sends with POST */
struct post_data
{
    char   *body;
    size_t  length;
    char   *referrer;   /* the address of the form's page */
};

/* the document at an address: fetched (POSTed to, with post), parsed, with
 * its sheets and images, its media queries evaluated for a viewport of this size */
struct document *document_load( const WCHAR *url, const struct post_data *post, int width, int height );

/* --- scripts (script/js.c) --- */

/* runs the page's scripts, its load events and its timers, as it loads */
void script_run_document( struct document *doc, int width, int height );
/* the page shows: its timers go to this window from now on, in real time */
void script_attach( struct document *doc, HWND window );
void script_on_timer( struct document *doc );
/* the user's event on an element: mousedown, mouseup, click, input, change, submit...;
 * TRUE when the page cancelled what the browser does for it */
BOOL script_event( struct document *doc, lxb_dom_element_t *target, const char *type, int x, int y );
/* how many times scripts changed the tree: the page is laid out again when this moves */
unsigned int script_mutations( struct document *doc );
void script_free( struct script_env *env );
/* the timer a page's scripts set in the view, to tell whose a WM_TIMER is */
UINT_PTR script_timer_id( struct document *doc );

/* --- forms (form.c) --- */

enum control_kind control_kind_of( lxb_dom_element_t *element );
BOOL control_is_password( lxb_dom_element_t *element );
/* the value of a control as the user sees it; the caller frees */
WCHAR *control_value( lxb_dom_element_t *element );
void control_set_value( lxb_dom_element_t *element, const WCHAR *value );
lxb_dom_element_t *control_form( lxb_dom_element_t *element );
/* the <option> a <select> shows */
lxb_dom_element_t *select_chosen( lxb_dom_element_t *select );
/* the text of an element, its spaces collapsed; the caller frees */
WCHAR *element_text( lxb_dom_element_t *element );
/* the first submit button of a form, the one Enter presses */
lxb_dom_element_t *form_default_button( lxb_dom_element_t *form );
/* where submitting the form goes: a GET address with the query, or the
 * address and the body of a POST; FALSE when there is nowhere */
BOOL form_submission( struct document *doc, lxb_dom_element_t *form, lxb_dom_element_t *submitter,
                      char **url, struct post_data *post );
void document_free( struct document *doc );
/* the document of a frame, its scripts run, its own frames loaded */
struct document *document_load_frame( const char *url, struct document *parent, int width, int height,
                                     const char *name );
/* the frame of an <iframe>, made if there is none yet */
struct frame *document_frame( struct document *doc, lxb_dom_element_t *element, BOOL create );
/* where a frame's document goes: its src, about:blank without one */
char *frame_url( struct document *doc, lxb_dom_element_t *element );
/* which document of a page and its frames an element is in */
struct document *document_owning( struct document *root, lxb_dom_element_t *element );
/* the decoded image of an <img src>, if it came */
const struct image *document_image( struct document *doc, const char *src, size_t len );
/* an address the page writes, made absolute against its base; the caller frees */
char *document_resolve( const struct document *doc, const char *ref, size_t len );
UINT charset_code_page( const char *name, size_t len );
/* the built-in pages: about:arctic, and the one that says why a page did not load */
char *page_about( const WCHAR *url, size_t *len );
char *page_error( const WCHAR *url, const WCHAR *reason, size_t *len );

/* what CSS needs before lexbor sees it (css.c): @media evaluated for the
 * viewport, var() replaced, the shorthands lexbor drops expanded */
struct css_context
{
    int             width, height;  /* the viewport the page opens in */
    struct css_var *vars;           /* the document's custom properties */
    unsigned int    var_count, var_capacity;
    int             heads[4096];    /* a hash of their names: the first of each chain, -1 for none */
    struct name_set *names;         /* the classes and ids the document has: rules for others go */
};

void css_context_init( struct css_context *ctx, int width, int height );
void css_context_free( struct css_context *ctx );
/* the first pass over every sheet: the custom properties of the document */
void css_collect( struct css_context *ctx, const char *css, size_t len );
char *css_rewrite( const struct css_context *ctx, const char *css, size_t len, size_t *out_len );
BOOL css_media_matches( const struct css_context *ctx, const char *media, size_t len );
/* a class or an id the document uses: a rule that asks for none of them cannot match */
void css_context_add_name( struct css_context *ctx, const char *name, size_t len );
/* for a style="", only what lexbor did not have */
char *css_expand_inline( const struct css_context *ctx, const char *style, size_t len, size_t *out_len );

/* --- styles --- */

enum display_kind { DISPLAY_NONE, DISPLAY_INLINE, DISPLAY_BLOCK, DISPLAY_LIST_ITEM, DISPLAY_INLINE_BLOCK };
enum white_space_kind { WS_NORMAL, WS_NOWRAP, WS_PRE, WS_PRE_WRAP, WS_PRE_LINE };
enum text_align_kind { ALIGN_LEFT, ALIGN_RIGHT, ALIGN_CENTER, ALIGN_JUSTIFY };
enum length_unit { LEN_AUTO, LEN_PX, LEN_PERCENT };
enum list_kind { LIST_NONE, LIST_DISC, LIST_DECIMAL };
enum valign_kind { VALIGN_BASELINE, VALIGN_MIDDLE, VALIGN_TOP, VALIGN_BOTTOM };
enum position_kind { POSITION_STATIC, POSITION_RELATIVE, POSITION_ABSOLUTE, POSITION_FIXED };
enum flex_direction { FLEX_ROW, FLEX_ROW_REVERSE, FLEX_COLUMN, FLEX_COLUMN_REVERSE };
enum justify_kind { JUSTIFY_START, JUSTIFY_END, JUSTIFY_CENTER, JUSTIFY_BETWEEN, JUSTIFY_AROUND, JUSTIFY_EVENLY };
enum flex_align { FALIGN_AUTO, FALIGN_STRETCH, FALIGN_START, FALIGN_END, FALIGN_CENTER, FALIGN_BASELINE };

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
    struct length         min_width, max_width;     /* auto: none */
    BOOL                  border_box;   /* box-sizing: width and height count the padding and the border */
    struct length         margin[4];    /* top, right, bottom, left */
    float                 padding[4];
    float                 border[4];
    DWORD                 border_color[4];
    BOOL                  invisible;    /* visibility: hidden - inherited, a child may show itself */
    BOOL                  transparent;  /* opacity: 0 here or above - no child shows */
    BOOL                  out_of_flow;  /* position: absolute or fixed */
    enum position_kind    position;     /* sticky is relative here */
    BOOL                  clips;        /* overflow other than visible: what goes out of it is cut */
    struct length         inset[4];     /* top, right, bottom, left; auto when not set */
    enum valign_kind      valign;       /* of an inline-block or an image on its line */
    /* flexbox: the container's */
    BOOL                  flex;         /* display: flex or inline-flex */
    enum flex_direction   flex_direction;
    BOOL                  flex_wrap;
    enum justify_kind     justify;
    enum flex_align       align_items;
    float                 gap_row, gap_column;
    /* and the item's */
    enum flex_align       align_self;
    float                 grow, shrink;
    struct length         basis;        /* auto: the width */
    int                   order;
};

/* the style of the root element, before any author style */
void style_initial( struct style *style );
void style_compute( const struct style *parent, lxb_dom_element_t *element, struct style *style );
/* a length in px against the containing width, auto counting as 0 */
float length_px( const struct length *length, float containing );

/* --- layout --- */

/* the document of a frame, laid out in the list at x, y, as big as the frame's box */
void layout_frame_document( struct display_list *list, struct document *doc, float x, float y, float width,
                            float height );

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
