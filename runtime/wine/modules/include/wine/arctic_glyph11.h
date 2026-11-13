/*
 * Windows 11's pictures of the notification area: the speaker and the battery
 *
 * Windows 11 draws them with glyphs of Segoe Fluent Icons (Volume0-3, Mute,
 * Battery0-10, BatteryCharging0-10, BatterySaver0-10): outlines a little
 * heavier than Windows 10's, rounded, the battery lying down with a rounded
 * end and its charge a rounded bar inside it, a lightning bolt over it on the
 * mains and a leaf with battery saver, each cut out of what is under it. The
 * font is not ours to ship: the same shapes are drawn here on a 16 unit grid
 * with the stroke of the Wi-Fi beside them, sampled 4 x 4 a pixel.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_ARCTIC_GLYPH11_H
#define __WINE_ARCTIC_GLYPH11_H

#include <math.h>
#include <stdlib.h>

#define G11_SAMPLES 4
#define G11_STROKE  1.3f

static inline float g11_segment( float x, float y, float ax, float ay, float bx, float by )
{
    float dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
    float t = len2 ? ((x - ax) * dx + (y - ay) * dy) / len2 : 0, ex, ey;

    t = t < 0 ? 0 : t > 1 ? 1 : t;
    ex = ax + t * dx - x;
    ey = ay + t * dy - y;
    return sqrtf( ex * ex + ey * ey );
}

/* signed distance to a closed polygon: negative inside */
static inline float g11_polygon( float x, float y, const float (*p)[2], int n )
{
    float d = 1e9f;
    BOOL in = FALSE;

    for (int i = 0, j = n - 1; i < n; j = i++)
    {
        float s = g11_segment( x, y, p[j][0], p[j][1], p[i][0], p[i][1] );
        if (s < d) d = s;
        if ((p[i][1] > y) != (p[j][1] > y) &&
            x < (p[j][0] - p[i][0]) * (y - p[i][1]) / (p[j][1] - p[i][1]) + p[i][0])
            in = !in;
    }
    return in ? -d : d;
}

static inline float g11_box( float x, float y, float left, float top, float right, float bottom, float radius )
{
    float cx = (left + right) / 2, cy = (top + bottom) / 2;
    float qx = fabsf( x - cx ) - ((right - left) / 2 - radius), qy = fabsf( y - cy ) - ((bottom - top) / 2 - radius);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0, in = qx > qy ? qx : qy;

    return sqrtf( ox * ox + oy * oy ) + (in < 0 ? in : 0) - radius;
}

/* an arc right of (cx, cy), as far as the angle either side, with round ends */
static inline float g11_wave( float x, float y, float cx, float cy, float radius, float angle )
{
    float dx = x - cx, dy = y - cy;
    float ex = cx + radius * cosf( angle ), ey = radius * sinf( angle );

    if (fabsf( atan2f( dy, dx ) ) <= angle) return fabsf( sqrtf( dx * dx + dy * dy ) - radius );
    return min( sqrtf( (x - ex) * (x - ex) + (y - cy - ey) * (y - cy - ey) ),
                sqrtf( (x - ex) * (x - ex) + (y - cy + ey) * (y - cy + ey) ) );
}

/* the speaker: lit with no wave at 0, one up to a third, two up to two thirds, three */
static inline BOOL g11_speaker_sample( float x, float y, int waves, BOOL mute )
{
    static const float body[6][2] =
    {
        { 1.3f, 5.8f }, { 4.4f, 5.8f }, { 7.7f, 2.7f }, { 7.7f, 13.3f }, { 4.4f, 10.2f }, { 1.3f, 10.2f },
    };
    static const float radii[3] = { 2.9f, 5.2f, 7.5f };
    float d = 1e9f;

    for (int i = 0, j = 5; i < 6; j = i++)
        d = min( d, g11_segment( x, y, body[j][0], body[j][1], body[i][0], body[i][1] ) );
    if (d <= G11_STROKE / 2) return TRUE;
    if (mute)
        return g11_segment( x, y, 10.6f, 6.0f, 14.6f, 10.0f ) <= G11_STROKE / 2 ||
               g11_segment( x, y, 14.6f, 6.0f, 10.6f, 10.0f ) <= G11_STROKE / 2;
    for (int i = 0; i < waves; i++)
        if (g11_wave( x, y, 7.7f, 8.0f, radii[i], 0.82f ) <= G11_STROKE / 2) return TRUE;
    return FALSE;
}

static inline int g11_waves( float level, BOOL mute )
{
    int percent = (int)(level * 100 + 0.5f);

    if (mute) return 0;
    return !percent ? 0 : percent < 34 ? 1 : percent < 67 ? 2 : 3;
}

/* what is over the battery */
enum g11_badge { G11_NONE, G11_BOLT, G11_LEAF };

static inline float g11_badge_distance( float x, float y, enum g11_badge badge )
{
    static const float bolt[6][2] =
    {
        { 9.0f, 2.0f }, { 4.9f, 8.9f }, { 7.5f, 8.9f }, { 6.4f, 14.0f }, { 10.7f, 7.0f }, { 8.0f, 7.0f },
    };

    if (badge == G11_BOLT) return g11_polygon( x, y, bolt, 6 );
    if (badge == G11_LEAF)
    {
        /* a lens across the lower right corner: two circles of 5.25 either side of its axis */
        float ax = 12.6f - 2.65f, ay = 12.0f - 2.65f, bx = 12.6f + 2.65f, by = 12.0f + 2.65f;
        float da = sqrtf( (x - bx) * (x - bx) + (y - by) * (y - by) ) - 5.25f;
        float db = sqrtf( (x - ax) * (x - ax) + (y - ay) * (y - ay) ) - 5.25f;
        float leaf = max( da, db ), vein = g11_segment( x, y, 10.6f, 14.0f, 13.6f, 11.0f ) - 0.35f;

        return max( leaf, -vein );
    }
    return 1e9f;
}

/* the battery: level 0..1 (below 0: unknown, nothing in it) */
static inline BOOL g11_battery_sample( float x, float y, float level, enum g11_badge badge )
{
    const float left = 1.0f, top = 4.2f, right = 13.6f, bottom = 11.8f, gap = 0.9f;
    float d, body;

    if (badge != G11_NONE)
    {
        d = g11_badge_distance( x, y, badge );
        if (d <= 0) return TRUE;
        if (d <= gap) return FALSE;
    }
    body = g11_box( x, y, left, top, right, bottom, 2.2f );
    if (fabsf( body + G11_STROKE / 2 ) <= G11_STROKE / 2) return TRUE;
    /* the end */
    if (g11_box( x, y, 14.2f, 6.3f, 15.6f, 9.7f, 0.7f ) <= 0) return TRUE;
    if (level > 0)
    {
        float inner_left = left + G11_STROKE + gap, inner_right = right - G11_STROKE - gap;
        float fill = inner_left + max( (inner_right - inner_left) * min( level, 1.f ), 1.2f );

        if (g11_box( x, y, inner_left, top + G11_STROKE + gap, fill, bottom - G11_STROKE - gap, 0.9f ) <= 0)
            return TRUE;
    }
    return FALSE;
}

/* coverage, size x size, top row first; kind 0 the speaker, 1 the battery */
static inline float *g11_render( int size, int kind, float level, int waves_or_badge, BOOL mute )
{
    float *cov = malloc( size * size * sizeof(float) ), unit = 16.0f / size;

    if (!cov) return NULL;
    for (int py = 0; py < size; py++)
    {
        for (int px_ = 0; px_ < size; px_++)
        {
            int hits = 0;

            for (int j = 0; j < G11_SAMPLES; j++)
            {
                for (int i = 0; i < G11_SAMPLES; i++)
                {
                    float x = (px_ + (i + 0.5f) / G11_SAMPLES) * unit, y = (py + (j + 0.5f) / G11_SAMPLES) * unit;
                    if (kind ? g11_battery_sample( x, y, level, waves_or_badge )
                             : g11_speaker_sample( x, y, waves_or_badge, mute ))
                        hits++;
                }
            }
            cov[py * size + px_] = (float)hits / (G11_SAMPLES * G11_SAMPLES);
        }
    }
    return cov;
}

/* an icon in a colour with the coverage as its (straight) alpha */
static inline HICON g11_icon( const float *cov, int size, COLORREF color )
{
    BITMAPINFO info = { { sizeof(info.bmiHeader), size, -size, 1, 32, BI_RGB } };
    ICONINFO icon = { .fIcon = TRUE };
    UINT32 rgb = (GetRValue( color ) << 16) | (GetGValue( color ) << 8) | GetBValue( color ), *bits;
    HICON ret = NULL;

    if (!cov) return NULL;
    if (!(icon.hbmColor = CreateDIBSection( NULL, &info, DIB_RGB_COLORS, (void **)&bits, NULL, 0 ))) return NULL;
    for (int i = 0; i < size * size; i++)
        bits[i] = ((UINT32)(min( cov[i], 1.f ) * 255 + 0.5f) << 24) | rgb;
    icon.hbmMask = CreateBitmap( size, size, 1, 1, NULL );
    ret = CreateIconIndirect( &icon );
    DeleteObject( icon.hbmColor );
    DeleteObject( icon.hbmMask );
    return ret;
}

/* drawn into a DC, premultiplied, over what is there */
static inline void g11_draw( HDC hdc, const float *cov, int x, int y, int size, COLORREF color, BYTE alpha )
{
    BITMAPINFO info = { { sizeof(info.bmiHeader), size, -size, 1, 32, BI_RGB } };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    HBITMAP bitmap;
    UINT32 *bits;
    HDC mem;

    if (!cov || !(bitmap = CreateDIBSection( hdc, &info, DIB_RGB_COLORS, (void **)&bits, NULL, 0 ))) return;
    for (int i = 0; i < size * size; i++)
    {
        UINT32 a = (UINT32)(min( cov[i], 1.f ) * alpha + 0.5f);
        bits[i] = (a << 24) | ((GetRValue( color ) * a / 255) << 16) | ((GetGValue( color ) * a / 255) << 8) |
                  (GetBValue( color ) * a / 255);
    }
    mem = CreateCompatibleDC( hdc );
    SelectObject( mem, bitmap );
    GdiAlphaBlend( hdc, x, y, size, size, mem, 0, 0, size, size, blend );
    DeleteDC( mem );
    DeleteObject( bitmap );
}

#endif  /* __WINE_ARCTIC_GLYPH11_H */
