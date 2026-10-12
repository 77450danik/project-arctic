/*
 * Arctic web engine: the rasterizer and the surface it draws into
 *
 * The surface is a DIB section, so a filled rectangle is a write into the
 * same bytes that GDI draws a run of text into, and the whole page leaves
 * as one buffer. Stage 3 puts the glyphs into an atlas and the rectangles
 * into Vulkan; the display list it reads stays the same.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <string.h>

#include "arctichtml_private.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(arctichtml);

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
    SetGraphicsMode( surface->dc, GM_ADVANCED );
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

static void fill_rect( struct surface *surface, const RECT *rect, COLORREF color )
{
    DWORD pixel = 0xff000000 | (GetRValue(color) << 16) | (GetGValue(color) << 8) | GetBValue(color);
    int left = max( rect->left, 0 ), top = max( rect->top, 0 );
    int right = min( rect->right, surface->width ), bottom = min( rect->bottom, surface->height );

    for (int y = top; y < bottom; y++)
    {
        DWORD *row = surface->bits + (size_t)y * surface->width;

        for (int x = left; x < right; x++) row[x] = pixel;
    }
}

static const WCHAR *font_face( enum item_font font )
{
    switch (font)
    {
    case FONT_HEADING: return L"Arial";
    case FONT_MONO:    return L"Courier New";
    case FONT_UI:      return L"Source Sans Pro";
    case FONT_BODY:
    default:           return L"Times New Roman";
    }
}

static HFONT create_font( HDC dc, const struct item *item )
{
    LOGFONTW desc = { .lfWeight = item->bold || item->font == FONT_HEADING ? FW_BOLD : FW_NORMAL,
                      .lfCharSet = DEFAULT_CHARSET, .lfQuality = CLEARTYPE_QUALITY,
                      .lfPitchAndFamily = item->font == FONT_MONO ? FIXED_PITCH : VARIABLE_PITCH };

    desc.lfHeight = -MulDiv( item->size, GetDeviceCaps( dc, LOGPIXELSY ), 72 );
    lstrcpynW( desc.lfFaceName, font_face( item->font ), ARRAY_SIZE(desc.lfFaceName) );
    return CreateFontIndirectW( &desc );
}

/* GDI leaves the alpha byte of a DIB section at whatever it was, and a
 * composited window with zeroes there is invisible: make the page opaque. */
static void make_opaque( struct surface *surface )
{
    size_t count = (size_t)surface->width * surface->height;

    for (size_t i = 0; i < count; i++) surface->bits[i] |= 0xff000000;
}

void raster_paint( struct surface *surface, const struct display_list *list )
{
    RECT whole = { 0, 0, surface->width, surface->height };

    if (!surface->dc || !surface->bits) return;

    fill_rect( surface, &whole, list->background );

    for (unsigned int i = 0; i < list->count; i++)
    {
        const struct item *item = &list->items[i];

        switch (item->type)
        {
        case ITEM_RECT:
            fill_rect( surface, &item->rect, item->color );
            break;

        case ITEM_TEXT:
        {
            HFONT font = create_font( surface->dc, item );
            HFONT old = SelectObject( surface->dc, font );
            RECT rect = item->rect;

            SetTextColor( surface->dc, item->color );
            DrawTextW( surface->dc, item->text, -1, &rect, item->format );
            SelectObject( surface->dc, old );
            DeleteObject( font );
            break;
        }
        }
    }
    GdiFlush();
    make_opaque( surface );
}
