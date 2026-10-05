/*
 * The Start menu: its pictures
 *
 * The right column of the menu has the line glyphs of Segoe Fluent Icons in
 * Windows 11 (Contact, Document, Photo, MusicNote, Download, Recent, This PC,
 * Equalizer, Settings, Sync, OpenInNewWindow, PowerButton, chevrons, Search,
 * Folder). The font is not ours to ship, so the same outlines are drawn
 * here: a 16-unit design of lines and arcs one unit thick, sampled 4 x 4 per
 * pixel, as the volume flyout draws its speaker.
 *
 * Program icons come from the shell in GDI's way, which knows no alpha over
 * a backdrop: each is drawn on black and on white, and the difference gives
 * its coverage, so dwm.exe lays it over the acrylic as it is.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <math.h>
#include <stdlib.h>

#include "startui.h"

#define SUPERSAMPLE  4
#define STROKE       1.0f
#define PI           3.14159265f
#define DEG          (PI / 180.0f)

enum shape { LINE, ARC, DISC, END };

struct stroke
{
    enum shape shape;
    float a, b, c, d, e;   /* LINE: x0 y0 x1 y1; ARC: cx cy r from to (degrees); DISC: cx cy r */
};

static const struct stroke person[] =
{
    { ARC, 8, 5, 3, -180, 180 },
    { ARC, 8, 15.5f, 6, -180, 0 },
    { END },
};

static const struct stroke document[] =
{
    { LINE, 3, 1, 10, 1 }, { LINE, 10, 1, 13, 4 }, { LINE, 13, 4, 13, 15 }, { LINE, 13, 15, 3, 15 },
    { LINE, 3, 15, 3, 1 }, { LINE, 10, 1, 10, 4 }, { LINE, 10, 4, 13, 4 },
    { LINE, 5.5f, 8, 10.5f, 8 }, { LINE, 5.5f, 11, 10.5f, 11 },
    { END },
};

static const struct stroke picture[] =
{
    { LINE, 1.5f, 2.5f, 14.5f, 2.5f }, { LINE, 14.5f, 2.5f, 14.5f, 13.5f }, { LINE, 14.5f, 13.5f, 1.5f, 13.5f },
    { LINE, 1.5f, 13.5f, 1.5f, 2.5f },
    { LINE, 1.5f, 12, 6, 7.5f }, { LINE, 6, 7.5f, 9.5f, 11 }, { LINE, 9.5f, 11, 11, 9.5f }, { LINE, 11, 9.5f, 14.5f, 13 },
    { ARC, 10.8f, 6, 1.4f, -180, 180 },
    { END },
};

static const struct stroke music[] =
{
    { LINE, 11, 2, 11, 11.5f }, { LINE, 11, 2, 14, 4 },
    { ARC, 8.6f, 12, 2.3f, -180, 180 },
    { END },
};

static const struct stroke download[] =
{
    { LINE, 8, 1.5f, 8, 10.5f }, { LINE, 4, 6.5f, 8, 10.5f }, { LINE, 8, 10.5f, 12, 6.5f },
    { LINE, 2, 14, 14, 14 },
    { END },
};

static const struct stroke recent[] =
{
    { ARC, 8, 8, 6.5f, -180, 180 },
    { LINE, 8, 4, 8, 8 }, { LINE, 8, 8, 11, 10 },
    { END },
};

static const struct stroke pc[] =
{
    { LINE, 1.5f, 2, 14.5f, 2 }, { LINE, 14.5f, 2, 14.5f, 11 }, { LINE, 14.5f, 11, 1.5f, 11 }, { LINE, 1.5f, 11, 1.5f, 2 },
    { LINE, 8, 11, 8, 14 }, { LINE, 5, 14, 11, 14 },
    { END },
};

static const struct stroke control_panel[] =
{
    { LINE, 1.5f, 4, 14.5f, 4 }, { LINE, 1.5f, 8, 14.5f, 8 }, { LINE, 1.5f, 12, 14.5f, 12 },
    { DISC, 5, 4, 1.8f }, { DISC, 10.5f, 8, 1.8f }, { DISC, 6.5f, 12, 1.8f },
    { END },
};

static const struct stroke settings[] =
{
    { ARC, 8, 8, 2.4f, -180, 180 },
    { ARC, 8, 8, 5, -180, 180 },
    { LINE, 8, 0.8f, 8, 3 }, { LINE, 8, 13, 8, 15.2f }, { LINE, 0.8f, 8, 3, 8 }, { LINE, 13, 8, 15.2f, 8 },
    { LINE, 2.9f, 2.9f, 4.5f, 4.5f }, { LINE, 11.5f, 11.5f, 13.1f, 13.1f },
    { LINE, 13.1f, 2.9f, 11.5f, 4.5f }, { LINE, 4.5f, 11.5f, 2.9f, 13.1f },
    { END },
};

static const struct stroke update[] =
{
    { ARC, 8, 8, 5.8f, -150, 150 },
    { LINE, 2.98f, 5.1f, 2.98f, 1.6f }, { LINE, 2.98f, 5.1f, 6.4f, 5.1f },
    { END },
};

static const struct stroke run[] =
{
    { LINE, 1.5f, 2.5f, 14.5f, 2.5f }, { LINE, 14.5f, 2.5f, 14.5f, 13.5f }, { LINE, 14.5f, 13.5f, 1.5f, 13.5f },
    { LINE, 1.5f, 13.5f, 1.5f, 2.5f }, { LINE, 1.5f, 5, 14.5f, 5 },
    { LINE, 5, 11.5f, 10, 6.8f }, { LINE, 6.6f, 6.8f, 10, 6.8f }, { LINE, 10, 6.8f, 10, 10.2f },
    { END },
};

static const struct stroke power[] =
{
    { ARC, 8, 8.8f, 5.6f, -55, 235 },
    { LINE, 8, 1.2f, 8, 8 },
    { END },
};

static const struct stroke chevron_right[] = { { LINE, 6, 3, 11, 8 }, { LINE, 11, 8, 6, 13 }, { END } };
static const struct stroke chevron_left[] = { { LINE, 10, 3, 5, 8 }, { LINE, 5, 8, 10, 13 }, { END } };
static const struct stroke chevron_down[] = { { LINE, 3, 6, 8, 11 }, { LINE, 8, 11, 13, 6 }, { END } };

static const struct stroke search[] =
{
    { ARC, 6.5f, 6.5f, 4.8f, -180, 180 },
    { LINE, 10, 10, 14.8f, 14.8f },
    { END },
};

static const struct stroke folder[] =
{
    { LINE, 1.5f, 3, 6, 3 }, { LINE, 6, 3, 7.5f, 4.8f }, { LINE, 7.5f, 4.8f, 14.5f, 4.8f },
    { LINE, 14.5f, 4.8f, 14.5f, 13.5f }, { LINE, 14.5f, 13.5f, 1.5f, 13.5f }, { LINE, 1.5f, 13.5f, 1.5f, 3 },
    { LINE, 1.5f, 7, 14.5f, 7 },
    { END },
};

static const struct stroke *glyphs[] =
{
    person, document, picture, music, download, recent, pc, control_panel, settings, update, run, power,
    chevron_right, chevron_left, chevron_down, search, folder,
};

static BOOL near_segment( float x, float y, float ax, float ay, float bx, float by )
{
    float dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
    float t = len2 ? ((x - ax) * dx + (y - ay) * dy) / len2 : 0, ex, ey;

    t = max( 0.f, min( 1.f, t ) );
    ex = ax + t * dx - x;
    ey = ay + t * dy - y;
    return ex * ex + ey * ey <= STROKE * STROKE / 4;
}

/* an arc from one angle to the other, clockwise on the screen, with round ends */
static BOOL near_arc( float x, float y, const struct stroke *s )
{
    float dx = x - s->a, dy = y - s->b, d = sqrtf( dx * dx + dy * dy );
    float from = s->d * DEG, to = s->e * DEG, t, ex, ey;

    if (to - from >= 2 * PI - 0.001f) return fabsf( d - s->c ) <= STROKE / 2;
    t = atan2f( dy, dx ) - from;
    while (t < 0) t += 2 * PI;
    while (t >= 2 * PI) t -= 2 * PI;
    if (fabsf( d - s->c ) <= STROKE / 2 && t <= to - from) return TRUE;
    ex = s->a + s->c * cosf( from ) - x;
    ey = s->b + s->c * sinf( from ) - y;
    if (ex * ex + ey * ey <= STROKE * STROKE / 4) return TRUE;
    ex = s->a + s->c * cosf( to ) - x;
    ey = s->b + s->c * sinf( to ) - y;
    return ex * ex + ey * ey <= STROKE * STROKE / 4;
}

static BOOL covered( const struct stroke *s, float x, float y )
{
    for (; s->shape != END; s++)
    {
        switch (s->shape)
        {
        case LINE:
            if (near_segment( x, y, s->a, s->b, s->c, s->d )) return TRUE;
            break;
        case ARC:
            if (near_arc( x, y, s )) return TRUE;
            break;
        case DISC:
            if ((x - s->a) * (x - s->a) + (y - s->b) * (y - s->b) <= s->c * s->c) return TRUE;
            break;
        default:
            break;
        }
    }
    return FALSE;
}

static HBITMAP create_dib( HDC hdc, int width, int height, UINT32 **bits )
{
    BITMAPINFO info = { { sizeof(info.bmiHeader), width, -height, 1, 32, BI_RGB } };
    return CreateDIBSection( hdc, &info, DIB_RGB_COLORS, (void **)bits, NULL, 0 );
}

/* premultiplied pixels over what is there */
static void blend_bits( HDC hdc, int x, int y, int width, int height, const UINT32 *pixels )
{
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    HDC mem = CreateCompatibleDC( hdc );
    HBITMAP bitmap;
    UINT32 *bits;

    if ((bitmap = create_dib( hdc, width, height, &bits )))
    {
        HGDIOBJ old = SelectObject( mem, bitmap );

        memcpy( bits, pixels, width * height * sizeof(*bits) );
        GdiAlphaBlend( hdc, x, y, width, height, mem, 0, 0, width, height, blend );
        SelectObject( mem, old );
        DeleteObject( bitmap );
    }
    DeleteDC( mem );
}

void draw_glyph( HDC hdc, enum glyph glyph, int x, int y, int size, COLORREF color, BYTE alpha )
{
    const struct stroke *s = glyphs[glyph];
    float scale = 16.0f / size;
    UINT32 *pixels;

    if (size <= 0 || !(pixels = malloc( size * size * sizeof(*pixels) ))) return;
    for (int py = 0; py < size; py++)
    {
        for (int px_ = 0; px_ < size; px_++)
        {
            int hits = 0;
            UINT32 a;

            for (int j = 0; j < SUPERSAMPLE; j++)
                for (int i = 0; i < SUPERSAMPLE; i++)
                    if (covered( s, (px_ + (i + 0.5f) / SUPERSAMPLE) * scale, (py + (j + 0.5f) / SUPERSAMPLE) * scale ))
                        hits++;
            a = hits * alpha / (SUPERSAMPLE * SUPERSAMPLE);
            pixels[py * size + px_] = (a << 24) | ((GetRValue( color ) * a / 255) << 16) |
                                      ((GetGValue( color ) * a / 255) << 8) | (GetBValue( color ) * a / 255);
        }
    }
    blend_bits( hdc, x, y, size, size, pixels );
    free( pixels );
}

/* every size is given at 96 DPI and drawn at the DPI of the system: the
 * shell's, which is the primary monitor's scale when the session starts */
int px( int n )
{
    return MulDiv( n, GetDpiForSystem(), 96 );
}

HFONT shell_font( int height, int weight )
{
    NONCLIENTMETRICSW metrics = { sizeof(metrics) };

    SystemParametersInfoW( SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0 );
    metrics.lfMessageFont.lfHeight = -height;
    metrics.lfMessageFont.lfWeight = weight;
    metrics.lfMessageFont.lfQuality = CLEARTYPE_QUALITY;
    return CreateFontIndirectW( &metrics.lfMessageFont );
}

void fill_alpha( HDC hdc, const RECT *rect, COLORREF color, BYTE alpha )
{
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    HDC mem = CreateCompatibleDC( hdc );
    HBITMAP bitmap;
    UINT32 *bits;

    if ((bitmap = create_dib( hdc, 1, 1, &bits )))
    {
        HGDIOBJ old = SelectObject( mem, bitmap );

        *bits = ((UINT32)alpha << 24) | ((GetRValue( color ) * alpha / 255) << 16) |
                ((GetGValue( color ) * alpha / 255) << 8) | (GetBValue( color ) * alpha / 255);
        GdiAlphaBlend( hdc, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, mem, 0, 0,
                       1, 1, blend );
        SelectObject( mem, old );
        DeleteObject( bitmap );
    }
    DeleteDC( mem );
}

void fill_round_rect( HDC hdc, const RECT *rect, float radius, COLORREF color, BYTE alpha )
{
    int width = rect->right - rect->left, height = rect->bottom - rect->top;
    UINT32 *pixels;

    if (width <= 0 || height <= 0 || !(pixels = malloc( width * height * sizeof(*pixels) ))) return;
    for (int py = 0; py < height; py++)
    {
        for (int px_ = 0; px_ < width; px_++)
        {
            int hits = 0;
            UINT32 a;

            for (int j = 0; j < SUPERSAMPLE; j++)
            {
                for (int i = 0; i < SUPERSAMPLE; i++)
                {
                    float x = px_ + (i + 0.5f) / SUPERSAMPLE, y = py + (j + 0.5f) / SUPERSAMPLE;
                    float dx = max( max( radius - x, x - (width - radius) ), 0.f );
                    float dy = max( max( radius - y, y - (height - radius) ), 0.f );
                    if (dx * dx + dy * dy <= radius * radius) hits++;
                }
            }
            a = hits * alpha / (SUPERSAMPLE * SUPERSAMPLE);
            pixels[py * width + px_] = (a << 24) | ((GetRValue( color ) * a / 255) << 16) |
                                       ((GetGValue( color ) * a / 255) << 8) | (GetBValue( color ) * a / 255);
        }
    }
    blend_bits( hdc, rect->left, rect->top, width, height, pixels );
    free( pixels );
}

/* GDI writes its text without alpha, which dwm.exe takes by brightness over
 * the acrylic: over something opaque (the search box) it has to stay opaque */
void make_opaque( HDC hdc, const RECT *rect )
{
    DIBSECTION dib;
    HGDIOBJ bitmap = GetCurrentObject( hdc, OBJ_BITMAP );
    int height, stride;
    BYTE *bits;

    GdiFlush();
    if (!bitmap || GetObjectW( bitmap, sizeof(dib), &dib ) != sizeof(dib) || !dib.dsBm.bmBits ||
        dib.dsBm.bmBitsPixel != 32)
        return;
    height = dib.dsBm.bmHeight;
    stride = dib.dsBm.bmWidthBytes;
    bits = dib.dsBm.bmBits;
    for (int y = max( rect->top, 0 ); y < min( rect->bottom, height ); y++)
    {
        /* the menu's own DIBs are all top-down */
        BYTE *row = bits + y * stride;
        for (int x = max( rect->left, 0 ); x < min( rect->right, dib.dsBm.bmWidth ); x++) row[x * 4 + 3] = 0xff;
    }
}

/* the icon in premultiplied ARGB, its alpha from drawing it on black and on white */
HBITMAP icon_bitmap( HICON icon, int size )
{
    HDC screen = GetDC( NULL ), mem = CreateCompatibleDC( screen );
    UINT32 *black_bits, *white_bits, *bits;
    HBITMAP black = create_dib( screen, size, size, &black_bits );
    HBITMAP white = create_dib( screen, size, size, &white_bits );
    HBITMAP result = create_dib( screen, size, size, &bits );
    HGDIOBJ old;

    ReleaseDC( NULL, screen );
    if (!black || !white || !result)
    {
        if (black) DeleteObject( black );
        if (white) DeleteObject( white );
        if (result) DeleteObject( result );
        DeleteDC( mem );
        return NULL;
    }
    memset( black_bits, 0, size * size * 4 );
    for (int i = 0; i < size * size; i++) white_bits[i] = 0x00ffffff;
    old = SelectObject( mem, black );
    DrawIconEx( mem, 0, 0, icon, size, size, 0, NULL, DI_NORMAL );
    SelectObject( mem, white );
    DrawIconEx( mem, 0, 0, icon, size, size, 0, NULL, DI_NORMAL );
    SelectObject( mem, old );
    GdiFlush();
    for (int i = 0; i < size * size; i++)
    {
        UINT32 b = black_bits[i], w = white_bits[i];
        int diff = (((w >> 16) & 0xff) - ((b >> 16) & 0xff) + ((w >> 8) & 0xff) - ((b >> 8) & 0xff) +
                    (w & 0xff) - (b & 0xff)) / 3;
        UINT32 a = 255 - max( 0, min( 255, diff ) );
        UINT32 r = min( (b >> 16) & 0xff, a ), g = min( (b >> 8) & 0xff, a ), bl = min( b & 0xff, a );

        bits[i] = (a << 24) | (r << 16) | (g << 8) | bl;
    }
    DeleteObject( black );
    DeleteObject( white );
    DeleteDC( mem );
    return result;
}

void draw_icon_bitmap( HDC hdc, HBITMAP bitmap, int x, int y, int size )
{
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    HDC mem;
    HGDIOBJ old;

    if (!bitmap) return;
    mem = CreateCompatibleDC( hdc );
    old = SelectObject( mem, bitmap );
    GdiAlphaBlend( hdc, x, y, size, size, mem, 0, 0, size, size, blend );
    SelectObject( mem, old );
    DeleteDC( mem );
}
