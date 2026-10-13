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

/* The display list: what to draw, in the order to draw it, with nothing of
 * the document left in it. Stage 0 has the items a page of text needs; the
 * layout of stage 1 fills the same list. */
enum item_type
{
    ITEM_RECT,      /* a filled rectangle */
    ITEM_TEXT,      /* a run of text inside its rectangle */
};

enum item_font
{
    FONT_BODY,      /* Times New Roman, as a browser's default serif */
    FONT_HEADING,   /* Arial, bold */
    FONT_UI,        /* the font of the shell, for our own pages */
    FONT_MONO,      /* Courier New */
};

struct item
{
    enum item_type  type;
    RECT            rect;
    COLORREF        color;
    const WCHAR    *text;       /* owned by the list's strings */
    enum item_font  font;
    int             size;       /* in points */
    BOOL            bold;
    UINT            format;     /* DT_* for DrawTextW */
};

struct display_list
{
    struct item  *items;
    unsigned int  count, capacity;
    WCHAR       **strings;      /* copies the list owns */
    unsigned int  string_count, string_capacity;
    COLORREF      background;
    int           width, height;    /* the size the list was built for */
};

void display_list_reset( struct display_list *list );
void display_list_free( struct display_list *list );
struct item *display_list_add( struct display_list *list, enum item_type type );
const WCHAR *display_list_string( struct display_list *list, const WCHAR *text );

/* The page the engine shows while there is no document: stage 0 draws it
 * straight into the display list, with no parser and no layout. */
void build_start_page( struct display_list *list, int width, int height, const WCHAR *url );

void raster_paint( struct surface *surface, const struct display_list *list );

/* The way to the screen: Vulkan when the card and the driver allow it,
 * BitBlt when they do not or when ARCTIC_HTML_CPU is set. */
struct present;

struct present *present_create( HWND hwnd );
void present_destroy( struct present *present );
BOOL present_surface( struct present *present, const struct surface *surface );
BOOL present_on_gpu( const struct present *present );

/* The view: the window of the engine inside the host's window, named
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

#endif /* __ARCTICHTML_PRIVATE_H */
