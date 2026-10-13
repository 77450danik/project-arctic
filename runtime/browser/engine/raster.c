/*
 * Arctic web engine: the rasterizer and the surface it draws into
 *
 * The surface is a DIB section, so a filled rectangle is a write into the
 * same bytes that GDI draws a run of text into, and the whole page leaves
 * as one buffer. Stage 3 puts the glyphs into an atlas and the rectangles
 * into Vulkan; the display list it reads stays the same.
 *
 * The list is in the coordinates of the document; the rasterizer draws the
 * part of it the scroll position shows, and skips what is outside.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <string.h>

#include "arctichtml_private.h"

BOOL surface_create( struct surface *surface, int width, int height )
{
    BITMAPINFO info = { .bmiHeader = { .biSize = sizeof(BITMAPINFOHEADER), .biWidth = width,
                                       .biHeight = -height, /* top down, as the GPU wants it */
                                       .biPlanes = 1, .biBitCount = 32, .biCompression = BI_RGB } };
    HDC dc = GetDC( 0 );

    memset( surface, 0, sizeof(*surface) );
    if (width <= 0 || height <= 0) width = height = 1;

    surface->dc = CreateCompatibleDC( dc );
    ReleaseDC( 0, dc );
    if (!surface->dc) return FALSE;

    surface->bitmap = CreateDIBSection( surface->dc, &info, DIB_RGB_COLORS, (void **)&surface->bits, NULL, 0 );
    if (!surface->bitmap)
    {
        ERR( "no %dx%d surface\n", width, height );
        DeleteDC( surface->dc );
        surface->dc = 0;
        return FALSE;
    }
    surface->old_bitmap = SelectObject( surface->dc, surface->bitmap );
    surface->width = width;
    surface->height = height;
    SetBkMode( surface->dc, TRANSPARENT );
    SetTextAlign( surface->dc, TA_BASELINE | TA_LEFT | TA_NOUPDATECP );
    return TRUE;
}

void surface_destroy( struct surface *surface )
{
    if (!surface->dc) return;
    SelectObject( surface->dc, surface->old_bitmap );
    DeleteObject( surface->bitmap );
    DeleteDC( surface->dc );
    memset( surface, 0, sizeof(*surface) );
}

/* a rectangle of 0xAARRGGBB, blended over what is there when not opaque */
static void fill_rect( struct surface *surface, const RECT *rect, DWORD color )
{
    unsigned a = color >> 24, na = 255 - a;
    unsigned r = (color >> 16) & 0xff, g = (color >> 8) & 0xff, b = color & 0xff;
    int left = max( rect->left, 0 ), top = max( rect->top, 0 );
    int right = min( rect->right, surface->width ), bottom = min( rect->bottom, surface->height );

    if (!a) return;
    for (int y = top; y < bottom; y++)
    {
        DWORD *row = surface->bits + (size_t)y * surface->width;

        if (a == 255)
        {
            for (int x = left; x < right; x++) row[x] = 0xff000000 | (color & 0xffffff);
            continue;
        }
        for (int x = left; x < right; x++)
        {
            DWORD p = row[x];

            row[x] = 0xff000000 | (((r * a + ((p >> 16) & 0xff) * na) / 255) << 16) |
                     (((g * a + ((p >> 8) & 0xff) * na) / 255) << 8) | ((b * a + (p & 0xff) * na) / 255);
        }
    }
}

/* GDI leaves the alpha byte of a DIB section at whatever it was, and a
 * composited window with zeroes there is invisible: make the page opaque. */
static void make_opaque( struct surface *surface )
{
    size_t count = (size_t)surface->width * surface->height;

    for (size_t i = 0; i < count; i++) surface->bits[i] |= 0xff000000;
}

static void draw_image( struct surface *surface, HDC image_dc, const struct item *item, const RECT *where )
{
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    HBITMAP old = SelectObject( image_dc, item->image->bitmap );

    GdiAlphaBlend( surface->dc, where->left, where->top, where->right - where->left, where->bottom - where->top,
                   image_dc, 0, 0, item->image->width, item->image->height, blend );
    SelectObject( image_dc, old );
}

void raster_paint( struct surface *surface, const struct display_list *list, int scroll_y )
{
    RECT whole = { 0, 0, surface->width, surface->height };
    HDC image_dc = NULL;

    if (!surface->dc || !surface->bits) return;

    fill_rect( surface, &whole, list->background | 0xff000000 );

    /* the flow first, then what is positioned over it */
    for (unsigned int n = 0; n < list->count * 2; n++)
    {
        const struct item *item = &list->items[n % list->count];
        RECT where = item->rect;

        if (item->layer != (n >= list->count)) continue;

        OffsetRect( &where, 0, -scroll_y );
        if (where.bottom < 0 || where.top >= surface->height) continue;     /* not on the screen */
        if (item->clipped && item->type != ITEM_RUN)
        {
            RECT clip = item->clip;

            OffsetRect( &clip, 0, -scroll_y );
            if (item->type == ITEM_RECT && !IntersectRect( &where, &where, &clip )) continue;
            if (item->type == ITEM_IMAGE)
            {
                RECT visible;

                if (!IntersectRect( &visible, &where, &clip )) continue;
                GdiFlush();
                IntersectClipRect( surface->dc, clip.left, clip.top, clip.right, clip.bottom );
                if (!image_dc) image_dc = CreateCompatibleDC( surface->dc );
                if (image_dc && item->image && item->image->bitmap) draw_image( surface, image_dc, item, &where );
                SelectClipRgn( surface->dc, NULL );
                continue;
            }
        }

        switch (item->type)
        {
        case ITEM_RECT:
            GdiFlush();     /* text GDI may still hold back goes under, not over */
            fill_rect( surface, &where, item->color );
            break;

        case ITEM_RUN:
        {
            HFONT old = SelectObject( surface->dc, item->font->handle );

            SetTextColor( surface->dc, RGB( (item->color >> 16) & 0xff, (item->color >> 8) & 0xff,
                                            item->color & 0xff ) );
            if (item->clipped)
            {
                RECT clip = item->clip;

                OffsetRect( &clip, 0, -scroll_y );
                ExtTextOutW( surface->dc, item->x, item->baseline - scroll_y, ETO_CLIPPED, &clip, item->text,
                             item->length, NULL );
            }
            else ExtTextOutW( surface->dc, item->x, item->baseline - scroll_y, 0, NULL, item->text, item->length, NULL );
            if (log_tracing())
            {
                SIZE size;
                GetTextExtentPoint32W( surface->dc, item->text, item->length, &size );
                TRACE( "run %s: laid %ld drawn %ld, font %p %s %d\n", debugstr_w( item->text ),
                       item->rect.right - item->rect.left, size.cx, item->font->handle, debugstr_w(item->font->key.face),
                       item->font->key.px );
            }
            SelectObject( surface->dc, old );
            break;
        }
        case ITEM_IMAGE:
            if (!image_dc) image_dc = CreateCompatibleDC( surface->dc );
            if (image_dc && item->image && item->image->bitmap) draw_image( surface, image_dc, item, &where );
            break;
        }
    }
    if (image_dc) DeleteDC( image_dc );
    GdiFlush();
    make_opaque( surface );
}
