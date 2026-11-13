/*
 * The flyouts of the notification area in the style of Windows 11
 *
 * When the taskbar has Windows 11's buttons (HKCU\Software\Arctic\Taskbar
 * ButtonStyle 1, the default), the flyouts of the volume, the battery and
 * the network draw their controls as Windows 11's Quick Settings do (the
 * WinUI 2 templates): a slider with a 4 high rounded track and a round thumb
 * whose accent dot grows under the pointer, rounded buttons and toggles,
 * the dark theme's accent #60CDFF (the light one's #005FB8). They float 12
 * from the taskbar and the screen's edge.
 *
 * Everything is drawn with alpha over the shell's backdrop (black, or white
 * over a light look, is the backdrop seen through: wine/arctic_shell.h):
 * shapes get their coverage from a distance field, one sample a pixel.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_ARCTIC_FLUENT_H
#define __WINE_ARCTIC_FLUENT_H

#include <math.h>
#include <stdlib.h>

/* the taskbar's buttons are Windows 11's: until the user says otherwise */
static inline BOOL arctic_style11(void)
{
    DWORD value = 1, size = sizeof(value);

    RegGetValueW( HKEY_CURRENT_USER, L"Software\\Arctic\\Taskbar", L"ButtonStyle", RRF_RT_REG_DWORD, NULL, &value,
                  &size );
    return value == 1;
}

/* Windows 11's colours (the dark theme's, or the light one's) */
#define FLUENT_ACCENT( light )      ((light) ? RGB( 0x00, 0x5f, 0xb8 ) : RGB( 0x60, 0xcd, 0xff ))  /* AccentFillColorDefault */
#define FLUENT_ACCENT_HOT( light )  ((light) ? RGB( 0x19, 0x6e, 0xbf ) : RGB( 0x5a, 0xbc, 0xe8 ))  /* ...Secondary */
#define FLUENT_ON_ACCENT( light )   ((light) ? RGB( 0xff, 0xff, 0xff ) : RGB( 0x00, 0x00, 0x00 ))  /* TextOnAccentFillColorPrimary */
#define FLUENT_THUMB( light )       ((light) ? RGB( 0xff, 0xff, 0xff ) : RGB( 0x45, 0x45, 0x45 ))  /* ControlSolidFillColorDefault */
#define FLUENT_MARGIN               12      /* from the taskbar and the screen's edge */
#define FLUENT_RADIUS               8       /* OverlayCornerRadius */

/* how far (x, y) is outside a rounded rectangle; negative inside */
static inline float fluent_box_distance( float x, float y, float left, float top, float right, float bottom, float radius )
{
    float cx = (left + right) / 2, cy = (top + bottom) / 2;
    float qx = fabsf( x - cx ) - ((right - left) / 2 - radius), qy = fabsf( y - cy ) - ((bottom - top) / 2 - radius);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0, in = qx > qy ? qx : qy;

    return sqrtf( ox * ox + oy * oy ) + (in < 0 ? in : 0) - radius;
}

static inline float fluent_coverage( float distance )
{
    float c = 0.5f - distance;
    return c < 0 ? 0 : c > 1 ? 1 : c;
}

/* a coverage map (width x height, top row first) in a colour at an opacity, over what is there */
static inline void fluent_blend( HDC hdc, int x, int y, int width, int height, const float *coverage, COLORREF color,
                                 BYTE alpha )
{
    BITMAPINFO info = { { sizeof(info.bmiHeader), width, -height, 1, 32, BI_RGB } };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    HDC mem;
    HBITMAP bitmap;
    UINT32 *bits;

    if (width <= 0 || height <= 0) return;
    if (!(bitmap = CreateDIBSection( hdc, &info, DIB_RGB_COLORS, (void **)&bits, NULL, 0 ))) return;
    for (int i = 0; i < width * height; i++)
    {
        UINT32 a = (UINT32)(coverage[i] * alpha + 0.5f);
        bits[i] = (a << 24) | ((GetRValue( color ) * a / 255) << 16) | ((GetGValue( color ) * a / 255) << 8) |
                  (GetBValue( color ) * a / 255);
    }
    mem = CreateCompatibleDC( hdc );
    SelectObject( mem, bitmap );
    GdiAlphaBlend( hdc, x, y, width, height, mem, 0, 0, width, height, blend );
    DeleteDC( mem );
    DeleteObject( bitmap );
}

/* a rounded rectangle, filled, or only its edge (stroke > 0), at fractional coordinates */
static inline void fluent_box( HDC hdc, float left, float top, float right, float bottom, float radius, float stroke,
                               COLORREF color, BYTE alpha )
{
    int x0 = (int)floorf( left ), y0 = (int)floorf( top ), x1 = (int)ceilf( right ), y1 = (int)ceilf( bottom );
    int width = x1 - x0, height = y1 - y0;
    float *cov;

    if (width <= 0 || height <= 0 || !(cov = malloc( width * height * sizeof(*cov) ))) return;
    for (int j = 0; j < height; j++)
    {
        for (int i = 0; i < width; i++)
        {
            float d = fluent_box_distance( x0 + i + 0.5f, y0 + j + 0.5f, left, top, right, bottom, radius );
            if (stroke > 0) d = fabsf( d + stroke / 2 ) - stroke / 2;
            cov[j * width + i] = fluent_coverage( d );
        }
    }
    fluent_blend( hdc, x0, y0, width, height, cov, color, alpha );
    free( cov );
}

static inline void fluent_rect( HDC hdc, const RECT *rect, float radius, COLORREF color, BYTE alpha )
{
    fluent_box( hdc, rect->left, rect->top, rect->right, rect->bottom, radius, 0, color, alpha );
}

static inline void fluent_circle( HDC hdc, float cx, float cy, float radius, COLORREF color, BYTE alpha )
{
    fluent_box( hdc, cx - radius, cy - radius, cx + radius, cy + radius, radius, 0, color, alpha );
}

/* a ring of a level, as a battery's: the track all round, the level's arc over it from the top,
 * clockwise, with round ends; fraction 0..1 */
static inline void fluent_ring( HDC hdc, float cx, float cy, float radius, float stroke, float fraction,
                                COLORREF track, BYTE track_alpha, COLORREF arc, BYTE arc_alpha )
{
    const float pi2 = 6.2831853f;
    int x0 = (int)floorf( cx - radius - stroke ), y0 = (int)floorf( cy - radius - stroke );
    int size = (int)ceilf( 2 * (radius + stroke) ) + 2;
    float end = (fraction < 0 ? 0 : fraction > 1 ? 1 : fraction) * pi2;
    float sx = cx, sy = cy - radius, ex = cx + radius * sinf( end ), ey = cy - radius * cosf( end );
    float *cov_track, *cov_arc;

    if (!(cov_track = malloc( 2 * size * size * sizeof(float) ))) return;
    cov_arc = cov_track + size * size;
    for (int j = 0; j < size; j++)
    {
        for (int i = 0; i < size; i++)
        {
            float px = x0 + i + 0.5f, py = y0 + j + 0.5f, dx = px - cx, dy = py - cy;
            float ring = fabsf( sqrtf( dx * dx + dy * dy ) - radius ) - stroke / 2, a = atan2f( dx, -dy ), d;

            if (a < 0) a += pi2;
            if (end <= 0) d = 1e9f;
            else if (a <= end) d = ring;
            else d = min( hypotf( px - sx, py - sy ), hypotf( px - ex, py - ey ) ) - stroke / 2;
            cov_arc[j * size + i] = fluent_coverage( d );
            cov_track[j * size + i] = fluent_coverage( ring ) * (1 - cov_arc[j * size + i]);
        }
    }
    fluent_blend( hdc, x0, y0, size, size, cov_track, track, track_alpha );
    if (end > 0) fluent_blend( hdc, x0, y0, size, size, cov_arc, arc, arc_alpha );
    free( cov_track );
}

/* a round-ended line from (x0, y0) to (x1, y1) */
static inline void fluent_line( HDC hdc, float x0, float y0, float x1, float y1, float width, COLORREF color, BYTE alpha )
{
    int left = (int)floorf( min( x0, x1 ) - width ), top = (int)floorf( min( y0, y1 ) - width );
    int w = (int)ceilf( max( x0, x1 ) + width ) - left, h = (int)ceilf( max( y0, y1 ) + width ) - top;
    float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy, *cov;

    if (w <= 0 || h <= 0 || !(cov = malloc( w * h * sizeof(*cov) ))) return;
    for (int j = 0; j < h; j++)
    {
        for (int i = 0; i < w; i++)
        {
            float px = left + i + 0.5f, py = top + j + 0.5f;
            float t = len2 ? ((px - x0) * dx + (py - y0) * dy) / len2 : 0, ex, ey;

            t = t < 0 ? 0 : t > 1 ? 1 : t;
            ex = x0 + t * dx - px;
            ey = y0 + t * dy - py;
            cov[j * w + i] = fluent_coverage( sqrtf( ex * ex + ey * ey ) - width / 2 );
        }
    }
    fluent_blend( hdc, left, top, w, h, cov, color, alpha );
    free( cov );
}

/* ChevronRight / ChevronLeft / ChevronDown / ChevronUp at 12, centred on (cx, cy) */
enum fluent_chevron { FLUENT_RIGHT, FLUENT_LEFT, FLUENT_DOWN, FLUENT_UP };

static inline void fluent_chevron( HDC hdc, int cx, int cy, int size, enum fluent_chevron kind, COLORREF color,
                                   BYTE alpha )
{
    float a = size * 0.18f, b = size * 0.36f, w = max( size / 12.f, 1.f );

    switch (kind)
    {
    case FLUENT_RIGHT:
        fluent_line( hdc, cx - a, cy - b, cx + a, cy, w, color, alpha );
        fluent_line( hdc, cx + a, cy, cx - a, cy + b, w, color, alpha );
        break;
    case FLUENT_LEFT:
        fluent_line( hdc, cx + a, cy - b, cx - a, cy, w, color, alpha );
        fluent_line( hdc, cx - a, cy, cx + a, cy + b, w, color, alpha );
        break;
    case FLUENT_DOWN:
        fluent_line( hdc, cx - b, cy - a, cx, cy + a, w, color, alpha );
        fluent_line( hdc, cx, cy + a, cx + b, cy - a, w, color, alpha );
        break;
    case FLUENT_UP:
        fluent_line( hdc, cx - b, cy + a, cx, cy - a, w, color, alpha );
        fluent_line( hdc, cx, cy - a, cx + b, cy + a, w, color, alpha );
        break;
    }
}

/* SliderStyle of WinUI 2: the track 4 high, rounded, the part up to the
 * thumb in the accent; the thumb a disc of 20 (ControlSolidFillColorDefault,
 * a faint edge) with the accent's dot in it, 12 across, 14 under the
 * pointer, 10 pressed. value 0..1; unit is fluent_unit(). */
enum fluent_state { FLUENT_REST, FLUENT_HOT, FLUENT_PRESSED, FLUENT_DISABLED };

/* a pixel at 96 DPI, at the DPI of the system (the shell's) */
static inline float fluent_unit(void)
{
    return GetDpiForSystem() / 96.f;
}

static inline int fluent_slider_x( int left, int right, float unit, float value )
{
    int inset = (int)(10 * unit + 0.5f);
    return left + inset + (int)((right - left - 2 * inset) * value + 0.5f);
}

static inline float fluent_slider_value( int left, int right, float unit, int x )
{
    int inset = (int)(10 * unit + 0.5f);
    float v = (float)(x - left - inset) / max( right - left - 2 * inset, 1 );
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

static inline void fluent_slider( HDC hdc, int left, int right, int cy, float unit, float value,
                                  enum fluent_state state, BOOL light, BYTE opacity )
{
    int inset = (int)(10 * unit + 0.5f), x = fluent_slider_x( left, right, unit, value );
    float r = 10.f * unit, dot = (state == FLUENT_HOT ? 7.f : state == FLUENT_PRESSED ? 5.f : 6.f) * unit;
    COLORREF accent = state == FLUENT_DISABLED ? (light ? RGB( 0xa0, 0xa0, 0xa0 ) : RGB( 0x78, 0x78, 0x78 ))
                                               : FLUENT_ACCENT( light );
    COLORREF track = light ? RGB( 0x00, 0x00, 0x00 ) : RGB( 0xff, 0xff, 0xff );

    /* ControlStrongFillColorDefault: 45% black, 54% white */
    fluent_box( hdc, left + inset, cy - 2.f * unit, right - inset, cy + 2.f * unit, 2.f * unit, 0, track,
                (BYTE)((light ? 0x72 : 0x8b) * opacity / 255) );
    if (state != FLUENT_DISABLED)
        fluent_box( hdc, left + inset, cy - 2.f * unit, x, cy + 2.f * unit, 2.f * unit, 0, accent, opacity );
    /* the disc, its edge (ControlStrokeColorDefault), the dot */
    fluent_circle( hdc, x, cy, r, FLUENT_THUMB( light ), opacity );
    fluent_box( hdc, x - r, cy - r, x + r, cy + r, r, 1.f, light ? RGB( 0, 0, 0 ) : RGB( 0xff, 0xff, 0xff ),
                (BYTE)((light ? 0x0f : 0x17) * opacity / 255) );
    fluent_circle( hdc, x, cy, dot, accent, opacity );
}

/* a subtle button's fill: transparent at rest, SubtleFillColorSecondary
 * under the pointer, ...Tertiary pressed */
static inline BYTE fluent_subtle_alpha( enum fluent_state state, BOOL light )
{
    if (state == FLUENT_HOT) return light ? 0x09 : 0x0f;
    if (state == FLUENT_PRESSED) return light ? 0x06 : 0x0a;
    return 0;
}

/* GDI writes no alpha: where something opaque was drawn, the pixels keep it */
static inline void fluent_make_opaque( UINT32 *bits, int width, int height, const RECT *rect )
{
    for (int y = max( (int)rect->top, 0 ); y < min( (int)rect->bottom, height ); y++)
        for (int x = max( (int)rect->left, 0 ); x < min( (int)rect->right, width ); x++)
            bits[y * width + x] |= 0xff000000;
}

/* A quick action of Windows 11: a rounded button 48 high, the accent when
 * on (ControlFillColorDefault with an edge when off), its glyph in the middle;
 * its name under it. The caller draws the glyph in the colour it gets. */
static inline COLORREF fluent_toggle( HDC hdc, const RECT *rect, BOOL on, enum fluent_state state, BOOL light,
                                      UINT32 *bits, int width, int height )
{
    float radius = 4.f * (rect->bottom - rect->top) / 48;

    if (on && state != FLUENT_DISABLED)
    {
        fluent_rect( hdc, rect, radius, state == FLUENT_HOT ? FLUENT_ACCENT_HOT( light ) : FLUENT_ACCENT( light ),
                     0xff );
        /* the rounded corners keep their partial alpha */
        if (bits)
        {
            RECT inner = *rect;
            InflateRect( &inner, -(int)ceilf( radius ), 0 );
            fluent_make_opaque( bits, width, height, &inner );
        }
        return FLUENT_ON_ACCENT( light );
    }
    /* ControlFillColorDefault 6% / 70% white; ...Secondary under the pointer */
    if (light) fluent_rect( hdc, rect, radius, RGB( 0xff, 0xff, 0xff ), state == FLUENT_HOT ? 0x80 : 0xb3 );
    else fluent_rect( hdc, rect, radius, RGB( 0xff, 0xff, 0xff ), state == FLUENT_HOT ? 0x15 : state == FLUENT_PRESSED ? 0x08 : 0x0f );
    fluent_box( hdc, rect->left, rect->top, rect->right, rect->bottom, radius, 1.f,
                light ? RGB( 0, 0, 0 ) : RGB( 0xff, 0xff, 0xff ), light ? 0x0f : 0x12 );
    return state == FLUENT_DISABLED ? (light ? RGB( 0xa0, 0xa0, 0xa0 ) : RGB( 0x78, 0x78, 0x78 )) : ARCTIC_INK( light );
}

/* where a flyout of Windows 11 stands: 12 from the taskbar and the screen's
 * edge, at the right end of the taskbar */
static inline void fluent_anchor( const RECT *icon, int width, int *x, int *y, BOOL *above )
{
    MONITORINFO info = { sizeof(info) };
    int margin = (int)(FLUENT_MARGIN * fluent_unit() + 0.5f);

    GetMonitorInfoW( MonitorFromRect( icon, MONITOR_DEFAULTTONEAREST ), &info );
    *above = (icon->top + icon->bottom) / 2 > (info.rcWork.top + info.rcWork.bottom) / 2;
    *x = info.rcWork.right - width - margin;
    if (info.rcWork.left > info.rcMonitor.left) *x = info.rcWork.left + margin;   /* the taskbar on the left */
    *y = *above ? info.rcWork.bottom - margin : info.rcWork.top + margin;
}

#endif  /* __WINE_ARCTIC_FLUENT_H */
