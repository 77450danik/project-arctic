/*
 * Volume icon of the notification area: its pictures
 *
 * Windows 10 draws the speaker with glyphs of Segoe MDL2 Assets: Volume0-3
 * (E992-E995, none to three waves by the level), Mute (E74F, a cross for the
 * waves), and VolumeBars (EBC5, all three waves, faint behind the lit ones in
 * the flyout); the list of outputs opens with ChevronUp/ChevronDown
 * (E70E/E70D). The font is not ours to ship, so the same outlines are drawn
 * here: a 16-unit design, one unit thick, sampled 4 x 4 per pixel. The level
 * picks the waves as the flyout's UnmutedGlyphConverter does.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <math.h>
#include <stdlib.h>

#include "sndvolsso.h"

#define SUPERSAMPLE  4
#define STROKE       1.0f
#define WAVE_ANGLE   0.84f        /* radians either side of the axis */

static const float speaker[][2] =  /* the box and the cone, around */
{
    { 1.0f, 5.5f }, { 4.2f, 5.5f }, { 7.8f, 2.4f }, { 7.8f, 13.6f }, { 4.2f, 10.5f }, { 1.0f, 10.5f },
};
static const float wave_radius[3] = { 2.7f, 5.0f, 7.3f };
static const float wave_center[2] = { 7.8f, 8.0f };

static BOOL near_segment( float x, float y, float ax, float ay, float bx, float by )
{
    float dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
    float t = len2 ? ((x - ax) * dx + (y - ay) * dy) / len2 : 0, ex, ey;

    t = max( 0.f, min( 1.f, t ) );
    ex = ax + t * dx - x;
    ey = ay + t * dy - y;
    return ex * ex + ey * ey <= STROKE * STROKE / 4;
}

/* an arc right of the cone, with round ends */
static BOOL near_wave( float x, float y, float r )
{
    float dx = x - wave_center[0], dy = y - wave_center[1], d = sqrtf( dx * dx + dy * dy );
    float ex = wave_center[0] + r * cosf( WAVE_ANGLE ), ey = r * sinf( WAVE_ANGLE );

    if (fabsf( d - r ) <= STROKE / 2 && fabsf( atan2f( dy, dx ) ) <= WAVE_ANGLE) return TRUE;
    /* the ends */
    dx = x - ex;
    return (dx * dx + (y - wave_center[1] - ey) * (y - wave_center[1] - ey) <= STROKE * STROKE / 4) ||
           (dx * dx + (y - wave_center[1] + ey) * (y - wave_center[1] + ey) <= STROKE * STROKE / 4);
}

static BOOL in_speaker( float x, float y )
{
    for (UINT i = 0; i < ARRAY_SIZE(speaker); i++)
    {
        UINT j = (i + 1) % ARRAY_SIZE(speaker);
        if (near_segment( x, y, speaker[i][0], speaker[i][1], speaker[j][0], speaker[j][1] )) return TRUE;
    }
    return FALSE;
}

static BOOL in_cross( float x, float y )
{
    return near_segment( x, y, 10.4f, 5.8f, 14.8f, 10.2f ) || near_segment( x, y, 14.8f, 5.8f, 10.4f, 10.2f );
}

/* the waves the level lights: none at 0, one up to a third, two up to two thirds */
static int lit_waves( float level )
{
    int percent = (int)(level * 100 + 0.5f);
    return !percent ? 0 : percent < 34 ? 1 : percent < 67 ? 2 : 3;
}

/* coverage of the lit glyph and of the faint waves behind it, per pixel */
static void render( int size, float level, BOOL mute, float *lit, float *faint )
{
    int waves = mute ? 0 : lit_waves( level );
    float scale = 16.0f / size;

    for (int py = 0; py < size; py++)
    {
        for (int px = 0; px < size; px++)
        {
            int hits = 0, faint_hits = 0;

            for (int j = 0; j < SUPERSAMPLE; j++)
            {
                for (int i = 0; i < SUPERSAMPLE; i++)
                {
                    float x = (px + (i + 0.5f) / SUPERSAMPLE) * scale, y = (py + (j + 0.5f) / SUPERSAMPLE) * scale;
                    BOOL on = in_speaker( x, y ) || (mute && in_cross( x, y ));

                    for (int w = 0; !on && w < waves; w++) on = near_wave( x, y, wave_radius[w] );
                    if (on) hits++;
                    else if (!mute && faint)
                    {
                        for (int w = waves; w < 3; w++)
                            if (near_wave( x, y, wave_radius[w] )) { faint_hits++; break; }
                    }
                }
            }
            lit[py * size + px] = (float)hits / (SUPERSAMPLE * SUPERSAMPLE);
            if (faint) faint[py * size + px] = (float)faint_hits / (SUPERSAMPLE * SUPERSAMPLE);
        }
    }
}

static HBITMAP create_dib( HDC hdc, int width, int height, UINT32 **bits )
{
    BITMAPINFO info = { { sizeof(info.bmiHeader), width, -height, 1, 32, BI_RGB } };
    return CreateDIBSection( hdc, &info, DIB_RGB_COLORS, (void **)bits, NULL, 0 );
}

static void blend_white( HDC hdc, int x, int y, int width, int height, const float *alpha )
{
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    HDC mem = CreateCompatibleDC( hdc );
    UINT32 *bits;
    HBITMAP bitmap;

    if ((bitmap = create_dib( hdc, width, height, &bits )))
    {
        HGDIOBJ old = SelectObject( mem, bitmap );

        /* premultiplied white */
        for (int i = 0; i < width * height; i++)
        {
            UINT32 a = (UINT32)(min( alpha[i], 1.f ) * 255 + 0.5f);
            bits[i] = (a << 24) | (a << 16) | (a << 8) | a;
        }
        GdiAlphaBlend( hdc, x, y, width, height, mem, 0, 0, width, height, blend );
        SelectObject( mem, old );
        DeleteObject( bitmap );
    }
    DeleteDC( mem );
}

/* the speaker in white at alpha, the unlit waves at bars_alpha (0: none) */
void draw_speaker( HDC hdc, int x, int y, int size, float level, BOOL mute, BYTE alpha, BYTE bars_alpha )
{
    float *lit = malloc( size * size * sizeof(float) ), *faint = malloc( size * size * sizeof(float) );

    if (lit && faint)
    {
        render( size, level, mute, lit, bars_alpha ? faint : NULL );
        for (int i = 0; i < size * size; i++)
        {
            float a = lit[i] * alpha / 255.f, b = bars_alpha ? faint[i] * bars_alpha / 255.f : 0;
            lit[i] = a + b * (1 - a);
        }
        blend_white( hdc, x, y, size, size, lit );
    }
    free( lit );
    free( faint );
}

/* ChevronUp or ChevronDown at 12 pixels: 10 across, 5 high */
void draw_chevron( HDC hdc, int cx, int cy, BOOL up, BYTE alpha )
{
    const int size = 12;
    float cov[12 * 12];

    for (int py = 0; py < size; py++)
    {
        for (int px = 0; px < size; px++)
        {
            int hits = 0;

            for (int j = 0; j < SUPERSAMPLE; j++)
            {
                for (int i = 0; i < SUPERSAMPLE; i++)
                {
                    float x = px + (i + 0.5f) / SUPERSAMPLE - 6, y = py + (j + 0.5f) / SUPERSAMPLE - 6;
                    float tip = up ? -2.5f : 2.5f;

                    if (near_segment( x, y, -5, -tip, 0, tip ) || near_segment( x, y, 0, tip, 5, -tip )) hits++;
                }
            }
            cov[py * size + px] = hits * (alpha / 255.f) / (SUPERSAMPLE * SUPERSAMPLE);
        }
    }
    blend_white( hdc, cx - 6, cy - 6, size, size, cov );
}

/* the notification area's icon: white, as on the dark taskbar */
HICON make_speaker_icon( int size, float level, BOOL mute )
{
    ICONINFO info = { .fIcon = TRUE };
    float *lit = malloc( size * size * sizeof(float) );
    UINT32 *bits;
    HICON icon = NULL;

    if (!lit) return NULL;
    render( size, level, mute, lit, NULL );
    if ((info.hbmColor = create_dib( NULL, size, size, &bits )))
    {
        /* icons carry straight alpha */
        for (int i = 0; i < size * size; i++)
            bits[i] = ((UINT32)(min( lit[i], 1.f ) * 255 + 0.5f) << 24) | 0xffffff;
        info.hbmMask = CreateBitmap( size, size, 1, 1, NULL );
        icon = CreateIconIndirect( &info );
        DeleteObject( info.hbmColor );
        DeleteObject( info.hbmMask );
    }
    free( lit );
    return icon;
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
        for (int px = 0; px < width; px++)
        {
            int hits = 0;
            UINT32 a;

            for (int j = 0; j < SUPERSAMPLE; j++)
            {
                for (int i = 0; i < SUPERSAMPLE; i++)
                {
                    float x = px + (i + 0.5f) / SUPERSAMPLE, y = py + (j + 0.5f) / SUPERSAMPLE;
                    float dx = max( max( radius - x, x - (width - radius) ), 0.f );
                    float dy = max( max( radius - y, y - (height - radius) ), 0.f );
                    if (dx * dx + dy * dy <= radius * radius) hits++;
                }
            }
            a = hits * alpha / (SUPERSAMPLE * SUPERSAMPLE);
            bits[py * width + px] = (a << 24) | ((GetRValue( color ) * a / 255) << 16) |
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
