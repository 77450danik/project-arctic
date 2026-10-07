/*
 * Battery icon of the notification area: the glyphs
 *
 * Windows 10 draws its battery with glyphs of Segoe MDL2 Assets, in the
 * notification area and at 64 in the flyout: Battery0-10 (E850-E859, E83F)
 * by the charge in tenths, BatteryCharging0-10 (E85A-E862, E83E, EA93) on
 * the mains, BatterySaver0-10 (E863-E86B, EA94, EA95) with battery saver on,
 * BatteryUnknown (E996). Their outlines are in glyphs.h
 * (tools/power/mdl2-glyphs.py); they are filled here, nonzero, sampled 5 x 5
 * a pixel.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdlib.h>

#include "batmeter.h"
#include "glyphs.h"

#define SUPERSAMPLE 5
#define EM          2048

static const WCHAR battery_codes[3][11] =
{
    { 0xe850, 0xe851, 0xe852, 0xe853, 0xe854, 0xe855, 0xe856, 0xe857, 0xe858, 0xe859, 0xe83f },
    { 0xe85a, 0xe85b, 0xe85c, 0xe85d, 0xe85e, 0xe85f, 0xe860, 0xe861, 0xe862, 0xe83e, 0xea93 },
    { 0xe863, 0xe864, 0xe865, 0xe866, 0xe867, 0xe868, 0xe869, 0xe86a, 0xe86b, 0xea94, 0xea95 },
};

/* the glyph of a charge: tenths, rounded, as the flyout's converter picks them */
WCHAR battery_glyph( enum battery_glyph kind, UINT percent )
{
    UINT tenth = min( (percent + 5) / 10, 10 );

    if (percent == ARCTIC_UNKNOWN) return 0xe996;
    return battery_codes[kind][tenth];
}

static const short *find_outline( WCHAR code )
{
    for (UINT i = 0; i < ARRAY_SIZE(glyphs); i++) if (glyphs[i].code == code) return glyphs[i].outline;
    return NULL;
}

/* nonzero winding of the point (font units) around every contour */
static BOOL inside( const short *outline, float x, float y )
{
    int winding = 0;

    for (const short *c = outline; *c; c += 1 + 2 * c[0])
    {
        int n = c[0];
        const short *p = c + 1;

        for (int i = 0; i < n; i++)
        {
            float x0 = p[2 * i], y0 = p[2 * i + 1];
            float x1 = p[2 * ((i + 1) % n)], y1 = p[2 * ((i + 1) % n) + 1];

            if (y0 <= y)
            {
                if (y1 > y && (x1 - x0) * (y - y0) - (x - x0) * (y1 - y0) > 0) winding++;
            }
            else if (y1 <= y && (x1 - x0) * (y - y0) - (x - x0) * (y1 - y0) < 0) winding--;
        }
    }
    return winding != 0;
}

/* how much of each pixel the glyph covers, size x size, top row first */
float *glyph_coverage( WCHAR code, int size )
{
    const short *outline = find_outline( code );
    float *cov = calloc( size * size, sizeof(float) ), scale = (float)EM / size;

    if (!cov || !outline) return cov;
    for (int py = 0; py < size; py++)
    {
        for (int px_ = 0; px_ < size; px_++)
        {
            int hits = 0;

            for (int j = 0; j < SUPERSAMPLE; j++)
                for (int i = 0; i < SUPERSAMPLE; i++)
                {
                    float x = (px_ + (i + 0.5f) / SUPERSAMPLE) * scale;
                    float y = EM - (py + (j + 0.5f) / SUPERSAMPLE) * scale;
                    if (inside( outline, x, y )) hits++;
                }
            cov[py * size + px_] = (float)hits / (SUPERSAMPLE * SUPERSAMPLE);
        }
    }
    return cov;
}

static HBITMAP create_dib( HDC hdc, int width, int height, UINT32 **bits )
{
    BITMAPINFO info = { { sizeof(info.bmiHeader), width, -height, 1, 32, BI_RGB } };
    return CreateDIBSection( hdc, &info, DIB_RGB_COLORS, (void **)bits, NULL, 0 );
}

/* the glyph in a colour at an opacity, over what is there */
void draw_glyph( HDC hdc, WCHAR code, int x, int y, int size, COLORREF color, BYTE alpha )
{
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    float *cov = glyph_coverage( code, size );
    HDC mem = CreateCompatibleDC( hdc );
    HBITMAP bitmap;
    UINT32 *bits;

    if (cov && (bitmap = create_dib( hdc, size, size, &bits )))
    {
        HGDIOBJ old = SelectObject( mem, bitmap );

        /* premultiplied */
        for (int i = 0; i < size * size; i++)
        {
            UINT32 a = (UINT32)(min( cov[i], 1.f ) * alpha + 0.5f);
            bits[i] = (a << 24) | ((GetRValue( color ) * a / 255) << 16) | ((GetGValue( color ) * a / 255) << 8) |
                      (GetBValue( color ) * a / 255);
        }
        GdiAlphaBlend( hdc, x, y, size, size, mem, 0, 0, size, size, blend );
        SelectObject( mem, old );
        DeleteObject( bitmap );
    }
    DeleteDC( mem );
    free( cov );
}

/* the notification area's icon: white, as on the dark taskbar */
HICON make_glyph_icon( WCHAR code, int size )
{
    ICONINFO info = { .fIcon = TRUE };
    float *cov = glyph_coverage( code, size );
    UINT32 *bits;
    HICON icon = NULL;

    if (!cov) return NULL;
    if ((info.hbmColor = create_dib( NULL, size, size, &bits )))
    {
        /* icons carry straight alpha */
        for (int i = 0; i < size * size; i++)
            bits[i] = ((UINT32)(min( cov[i], 1.f ) * 255 + 0.5f) << 24) | 0xffffff;
        info.hbmMask = CreateBitmap( size, size, 1, 1, NULL );
        icon = CreateIconIndirect( &info );
        DeleteObject( info.hbmColor );
        DeleteObject( info.hbmMask );
    }
    free( cov );
    return icon;
}

/* a colour with its own alpha, which dwm keeps over the acrylic */
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

/* a rounded rectangle of a colour with its own alpha: the slider's thumb */
void fill_round_rect( HDC hdc, const RECT *rect, float radius, COLORREF color, BYTE alpha )
{
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    int width = rect->right - rect->left, height = rect->bottom - rect->top;
    HDC mem = CreateCompatibleDC( hdc );
    HBITMAP bitmap;
    UINT32 *bits;

    if (width <= 0 || height <= 0 || !(bitmap = create_dib( hdc, width, height, &bits )))
    {
        DeleteDC( mem );
        return;
    }
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
            bits[py * width + px_] = (a << 24) | ((GetRValue( color ) * a / 255) << 16) |
                                     ((GetGValue( color ) * a / 255) << 8) | (GetBValue( color ) * a / 255);
        }
    }
    {
        HGDIOBJ old = SelectObject( mem, bitmap );
        GdiAlphaBlend( hdc, rect->left, rect->top, width, height, mem, 0, 0, width, height, blend );
        SelectObject( mem, old );
    }
    DeleteObject( bitmap );
    DeleteDC( mem );
}
