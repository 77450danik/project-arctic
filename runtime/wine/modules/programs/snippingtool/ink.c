/*
 * Snipping Tool: images, ink and the glyphs of the buttons
 *
 * Everything is drawn here in software, smoothed: a line's coverage of a
 * pixel is how far its centre lies inside the line's edge, so the ends and
 * joins come out round. A stroke is laid on as one coverage, so a marker
 * that crosses itself is not darker there.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <math.h>

#include "snippingtool.h"

struct image *image_create( int width, int height )
{
    struct image *image;

    if (width <= 0 || height <= 0) return NULL;
    if (!(image = calloc( 1, sizeof(*image) ))) return NULL;
    if (!(image->bits = calloc( (SIZE_T)width * height, sizeof(UINT32) )))
    {
        free( image );
        return NULL;
    }
    image->ref = 1;
    image->width = width;
    image->height = height;
    image->owned = TRUE;
    return image;
}

struct image *image_addref( struct image *image )
{
    if (image) InterlockedIncrement( &image->ref );
    return image;
}

void image_release( struct image *image )
{
    if (!image || InterlockedDecrement( &image->ref )) return;
    if (image->owned) free( image->bits );
    free( image );
}

void image_rect( const struct image *image, RECT *rect )
{
    SetRect( rect, 0, 0, image->width, image->height );
}

/* rect within src, clipped to it */
struct image *image_crop( const struct image *src, const RECT *rect )
{
    struct image *image;
    RECT bounds, r;

    image_rect( src, &bounds );
    if (!IntersectRect( &r, rect, &bounds )) return NULL;
    if (!(image = image_create( r.right - r.left, r.bottom - r.top ))) return NULL;
    for (int y = 0; y < image->height; y++)
        memcpy( image->bits + (SIZE_T)y * image->width, src->bits + (SIZE_T)(r.top + y) * src->width + r.left,
                image->width * sizeof(UINT32) );
    return image;
}

/* the same rect of two images of one size */
void image_copy_rect( struct image *dst, const struct image *src, const RECT *rect )
{
    RECT bounds, r;

    image_rect( dst, &bounds );
    if (!IntersectRect( &r, rect, &bounds )) return;
    for (int y = r.top; y < r.bottom; y++)
        memcpy( dst->bits + (SIZE_T)y * dst->width + r.left, src->bits + (SIZE_T)y * src->width + r.left,
                (r.right - r.left) * sizeof(UINT32) );
}

void image_fill( struct image *dst, const RECT *rect, UINT32 color )
{
    RECT bounds, r;

    image_rect( dst, &bounds );
    if (!IntersectRect( &r, rect, &bounds )) return;
    for (int y = r.top; y < r.bottom; y++)
    {
        UINT32 *row = dst->bits + (SIZE_T)y * dst->width;
        for (int x = r.left; x < r.right; x++) row[x] = color;
    }
}

static inline UINT32 blend( UINT32 dst, UINT32 color, unsigned int a )
{
    unsigned int na = 255 - a, da = dst >> 24;
    unsigned int b = ((dst & 0xff) * na + (color & 0xff) * a + 127) / 255;
    unsigned int g = (((dst >> 8) & 0xff) * na + ((color >> 8) & 0xff) * a + 127) / 255;
    unsigned int r = (((dst >> 16) & 0xff) * na + ((color >> 16) & 0xff) * a + 127) / 255;

    da += (255 - da) * a / 255;
    return (da << 24) | (r << 16) | (g << 8) | b;
}

/* a marker: what is under it shows through, darkened by its colour */
static inline UINT32 multiply( UINT32 dst, UINT32 color, unsigned int a )
{
    unsigned int fb = 255 - a + a * (color & 0xff) / 255;
    unsigned int fg = 255 - a + a * ((color >> 8) & 0xff) / 255;
    unsigned int fr = 255 - a + a * ((color >> 16) & 0xff) / 255;
    unsigned int da = dst >> 24;

    da += (255 - da) * a / 255;
    return (da << 24) | (((dst >> 16) & 0xff) * fr / 255) << 16 | (((dst >> 8) & 0xff) * fg / 255) << 8 |
           ((dst & 0xff) * fb / 255);
}

/* color's alpha over rect */
void image_blend_rect( struct image *dst, const RECT *rect, UINT32 color )
{
    unsigned int a = color >> 24;
    RECT bounds, r;

    image_rect( dst, &bounds );
    if (!IntersectRect( &r, rect, &bounds )) return;
    for (int y = r.top; y < r.bottom; y++)
    {
        UINT32 *row = dst->bits + (SIZE_T)y * dst->width;
        for (int x = r.left; x < r.right; x++) row[x] = blend( row[x], color, a );
    }
}

/* strokes */

struct stroke *stroke_create( UINT32 color, float radius, BOOL highlighter )
{
    struct stroke *stroke = calloc( 1, sizeof(*stroke) );

    if (!stroke) return NULL;
    stroke->ref = 1;
    stroke->color = color;
    stroke->radius = radius;
    stroke->highlighter = highlighter;
    return stroke;
}

struct stroke *stroke_addref( struct stroke *stroke )
{
    InterlockedIncrement( &stroke->ref );
    return stroke;
}

void stroke_release( struct stroke *stroke )
{
    if (!stroke || InterlockedDecrement( &stroke->ref )) return;
    free( stroke->points );
    free( stroke );
}

BOOL stroke_add( struct stroke *stroke, float x, float y )
{
    if (stroke->count == stroke->capacity)
    {
        int capacity = max( 64, stroke->capacity * 2 );
        struct pointf *points = realloc( stroke->points, capacity * sizeof(*points) );
        if (!points) return FALSE;
        stroke->points = points;
        stroke->capacity = capacity;
    }
    stroke->points[stroke->count].x = x;
    stroke->points[stroke->count].y = y;
    stroke->count++;
    return TRUE;
}

/* a copy, dx, dy further: what a crop makes of it */
struct stroke *stroke_moved( const struct stroke *stroke, float dx, float dy )
{
    struct stroke *copy = stroke_create( stroke->color, stroke->radius, stroke->highlighter );

    if (!copy) return NULL;
    for (int i = 0; i < stroke->count; i++)
        stroke_add( copy, stroke->points[i].x + dx, stroke->points[i].y + dy );
    return copy;
}

static void points_bounds( const struct pointf *points, int count, float radius, RECT *rect )
{
    float left = points[0].x, top = points[0].y, right = left, bottom = top;

    for (int i = 1; i < count; i++)
    {
        left = min( left, points[i].x );
        right = max( right, points[i].x );
        top = min( top, points[i].y );
        bottom = max( bottom, points[i].y );
    }
    SetRect( rect, (int)floorf( left - radius - 1 ), (int)floorf( top - radius - 1 ),
             (int)ceilf( right + radius + 1 ), (int)ceilf( bottom + radius + 1 ) );
}

/* what the points from "from" on (and the one before) cover */
void stroke_bounds( const struct stroke *stroke, int from, RECT *rect )
{
    if (from > 0) from--;
    if (from >= stroke->count) SetRectEmpty( rect );
    else points_bounds( stroke->points + from, stroke->count - from, stroke->radius, rect );
}

static float segment_distance2( float px, float py, const struct pointf *a, const struct pointf *b )
{
    float dx = b->x - a->x, dy = b->y - a->y, len2 = dx * dx + dy * dy, t = 0;

    if (len2 > 0)
    {
        t = ((px - a->x) * dx + (py - a->y) * dy) / len2;
        t = t < 0 ? 0 : t > 1 ? 1 : t;
    }
    px -= a->x + t * dx;
    py -= a->y + t * dy;
    return px * px + py * py;
}

BOOL stroke_hit( const struct stroke *stroke, float x, float y, float radius )
{
    float reach = (stroke->radius + radius) * (stroke->radius + radius);

    for (int i = 0; i < stroke->count; i++)
    {
        const struct pointf *b = &stroke->points[i + 1 < stroke->count ? i + 1 : i];
        if (segment_distance2( x, y, &stroke->points[i], b ) <= reach) return TRUE;
    }
    return FALSE;
}

/* the coverage of a polyline in box, the highest of its segments' */
static void polyline_coverage( BYTE *coverage, const RECT *box, const struct pointf *points, int count,
                               BOOL closed, float radius )
{
    int width = box->right - box->left, segments = count == 1 ? 1 : closed ? count : count - 1;
    float outer = (radius + 0.5f) * (radius + 0.5f), inner = radius > 0.5f ? (radius - 0.5f) * (radius - 0.5f) : 0;

    for (int i = 0; i < segments; i++)
    {
        const struct pointf *a = &points[i], *b = &points[count == 1 ? 0 : (i + 1) % count];
        int left = max( box->left, (int)floorf( min( a->x, b->x ) - radius - 1 ) );
        int right = min( box->right, (int)ceilf( max( a->x, b->x ) + radius + 1 ) );
        int top = max( box->top, (int)floorf( min( a->y, b->y ) - radius - 1 ) );
        int bottom = min( box->bottom, (int)ceilf( max( a->y, b->y ) + radius + 1 ) );

        for (int y = top; y < bottom; y++)
        {
            BYTE *row = coverage + (SIZE_T)(y - box->top) * width - box->left;
            for (int x = left; x < right; x++)
            {
                float d2 = segment_distance2( x + 0.5f, y + 0.5f, a, b );
                BYTE c;

                if (d2 >= outer) continue;
                c = d2 <= inner ? 255 : (BYTE)min( 255.0f, (radius + 0.5f - sqrtf( d2 )) * 255.0f );
                if (c > row[x]) row[x] = c;
            }
        }
    }
}

static void paint_polyline( struct image *dst, const struct pointf *points, int count, BOOL closed, float radius,
                            UINT32 color, BOOL highlighter, const RECT *clip )
{
    unsigned int alpha = color >> 24;
    RECT bounds, box;
    BYTE *coverage;
    int width;

    if (!count) return;
    points_bounds( points, count, radius, &box );
    image_rect( dst, &bounds );
    if (!IntersectRect( &box, &box, &bounds )) return;
    if (clip && !IntersectRect( &box, &box, clip )) return;
    width = box.right - box.left;
    if (!(coverage = calloc( (SIZE_T)width * (box.bottom - box.top), 1 ))) return;
    polyline_coverage( coverage, &box, points, count, closed, radius );

    for (int y = box.top; y < box.bottom; y++)
    {
        const BYTE *c = coverage + (SIZE_T)(y - box.top) * width;
        UINT32 *row = dst->bits + (SIZE_T)y * dst->width + box.left;
        for (int x = 0; x < width; x++)
        {
            unsigned int a = c[x] * alpha / 255;
            if (!a) continue;
            row[x] = highlighter ? multiply( row[x], color, a ) : blend( row[x], color, a );
        }
    }
    free( coverage );
}

void stroke_render( struct image *dst, const struct stroke *stroke, const RECT *clip )
{
    paint_polyline( dst, stroke->points, stroke->count, FALSE, stroke->radius, stroke->color,
                    stroke->highlighter, clip );
}

void ink_polyline( struct image *dst, const struct pointf *points, int count, BOOL closed, float radius,
                   UINT32 color, const RECT *clip )
{
    paint_polyline( dst, points, count, closed, radius, color, FALSE, clip );
}

/* filled, its corners round and smoothed */
void ink_round_rect( struct image *dst, const RECT *rect, float radius, UINT32 color, const RECT *clip )
{
    unsigned int alpha = color >> 24;
    float left = rect->left + radius, right = rect->right - radius;
    float top = rect->top + radius, bottom = rect->bottom - radius;
    RECT bounds, box;

    image_rect( dst, &bounds );
    if (!IntersectRect( &box, rect, &bounds )) return;
    if (clip && !IntersectRect( &box, &box, clip )) return;
    for (int y = box.top; y < box.bottom; y++)
    {
        UINT32 *row = dst->bits + (SIZE_T)y * dst->width;
        float py = y + 0.5f, qy = py < top ? top - py : py > bottom ? py - bottom : 0;
        for (int x = box.left; x < box.right; x++)
        {
            float px = x + 0.5f, qx = px < left ? left - px : px > right ? px - right : 0;
            float d = qx || qy ? sqrtf( qx * qx + qy * qy ) - radius : -1;
            unsigned int a = d <= -0.5f ? 255 : d >= 0.5f ? 0 : (unsigned int)((0.5f - d) * 255);

            if ((a = a * alpha / 255)) row[x] = blend( row[x], color, a );
        }
    }
}

/* dst_rect of dst from src shown at scale (at most 1): each pixel the
 * average of a grid of samples of the area it stands for */
void ink_scale( const struct image *src, struct image *dst, float scale, const RECT *dst_rect )
{
    int n = scale >= 1 ? 1 : min( 4, (int)ceilf( 1 / scale ) ), samples = n * n;
    RECT bounds, r;

    image_rect( dst, &bounds );
    if (!IntersectRect( &r, dst_rect, &bounds )) return;
    for (int y = r.top; y < r.bottom; y++)
    {
        UINT32 *row = dst->bits + (SIZE_T)y * dst->width;
        int sy[4];

        for (int j = 0; j < n; j++) sy[j] = min( src->height - 1, (int)((y + (j + 0.5f) / n) / scale) );
        for (int x = r.left; x < r.right; x++)
        {
            unsigned int b = 0, g = 0, rr = 0, a = 0;
            for (int i = 0; i < n; i++)
            {
                int sx = min( src->width - 1, (int)((x + (i + 0.5f) / n) / scale) );
                for (int j = 0; j < n; j++)
                {
                    UINT32 p = src->bits[(SIZE_T)sy[j] * src->width + sx];
                    b += p & 0xff;
                    g += (p >> 8) & 0xff;
                    rr += (p >> 16) & 0xff;
                    a += p >> 24;
                }
            }
            row[x] = ((a + samples / 2) / samples) << 24 | ((rr + samples / 2) / samples) << 16 |
                     ((g + samples / 2) / samples) << 8 | (b + samples / 2) / samples;
        }
    }
}

/* The glyphs, on a 20x20 grid with lines of 1.5, as Segoe Fluent Icons'
 * outlines are drawn: each a list of polylines, a count of points first,
 * negative when the line closes, 0 to end. */
static const float glyph_rectangle[] = { -4, 3,4, 17,4, 17,16, 3,16, 2, 10,7, 10,13, 2, 7,10, 13,10, 0 };
static const float glyph_window[] = { -4, 2.5f,3.5f, 17.5f,3.5f, 17.5f,16.5f, 2.5f,16.5f, 2, 2.5f,7, 17.5f,7, 0 };
static const float glyph_full_screen[] = { -4, 2,3, 18,3, 18,13.5f, 2,13.5f, 2, 10,13.5f, 10,17, 2, 6,17, 14,17, 0 };
static const float glyph_close[] = { 2, 5,5, 15,15, 2, 15,5, 5,15, 0 };
static const float glyph_new[] = { 2, 10,4, 10,16, 2, 4,10, 16,10, 0 };
static const float glyph_pen[] = { -5, 3.5f,16.5f, 4.5f,12.5f, 13,4, 16,7, 7.5f,15.5f, 2, 11,6, 14,9, 0 };
static const float glyph_highlighter[] = { -5, 5,14, 5,11, 12,4, 15,7, 8,14, 2, 5,14, 8,14, 2, 4,15, 3.5f,17, 0 };
static const float glyph_eraser[] = { -4, 3,12, 10,5, 16,11, 9,18, 2, 6.5f,8.5f, 12.5f,14.5f, 2, 9,18, 17,18, 0 };
static const float glyph_crop[] = { 3, 6,2, 6,14, 18,14, 3, 2,6, 14,6, 14,18, 0 };
static const float glyph_copy[] = { -4, 7,3, 17,3, 17,13, 7,13, 3, 13,13, 13,17, 3,17, 2, 3,17, 3,7, 2, 3,7, 7,7, 0 };
static const float glyph_save[] = { -5, 3,3, 14,3, 17,6, 17,17, 3,17, 3, 6.5f,3, 6.5f,7, 13,7, 3, 6,17, 6,11.5f, 14,11.5f,
                                    2, 14,11.5f, 14,17, 0 };
static const float glyph_folder[] = { -6, 2,4.5f, 8,4.5f, 10,6.5f, 18,6.5f, 18,16, 2,16, 2, 2,8.5f, 18,8.5f, 0 };

static void draw_lines( struct image *dst, const float *data, float x, float y, float k, float radius, UINT32 color,
                        const RECT *clip )
{
    struct pointf points[16];

    while (*data)
    {
        int count = (int)*data++;
        BOOL closed = count < 0;

        if (closed) count = -count;
        for (int i = 0; i < count; i++)
        {
            points[i].x = x + data[2 * i] * k;
            points[i].y = y + data[2 * i + 1] * k;
        }
        data += 2 * count;
        ink_polyline( dst, points, count, closed, radius, color, clip );
    }
}

/* an arrow on a half circle, the arrow at the end of the arc; mirrored, redo */
static void draw_undo( struct image *dst, BOOL redo, float x, float y, float k, float radius, UINT32 color,
                       const RECT *clip )
{
    struct pointf arc[24], head[3];
    int count = 0;

    for (int i = 0; i <= 18; i++)
    {
        float angle = 3.14159265f * i / 18;  /* from the right end over the top to the left */
        float px = 11 + 5.5f * cosf( angle ), py = 12.5f - 5.5f * sinf( angle );
        arc[count].x = x + (redo ? 20 - px : px) * k;
        arc[count++].y = y + py * k;
    }
    ink_polyline( dst, arc, count, FALSE, radius, color, clip );
    head[0].x = x + (redo ? 20 - 2.5f : 2.5f) * k;  head[0].y = y + 9 * k;
    head[1].x = arc[count - 1].x;                    head[1].y = arc[count - 1].y + 0.5f * k;
    head[2].x = x + (redo ? 20 - 9 : 9) * k;         head[2].y = y + 9.5f * k;
    ink_polyline( dst, head, 3, FALSE, radius, color, clip );
}

/* the freeform glyph: a loop, wider at one side */
static void draw_freeform( struct image *dst, float x, float y, float k, float radius, UINT32 color, const RECT *clip )
{
    struct pointf loop[40];

    for (int i = 0; i < 40; i++)
    {
        float angle = 2 * 3.14159265f * i / 40, r = 6.5f + 1.3f * sinf( 3 * angle + 0.6f );
        loop[i].x = x + (10 + r * cosf( angle ) * 1.1f) * k;
        loop[i].y = y + (10 + r * sinf( angle ) * 0.95f) * k;
    }
    ink_polyline( dst, loop, 40, TRUE, radius, color, clip );
}

/* glyph, size pixels high, centred in rect */
void ink_glyph( struct image *dst, enum glyph glyph, const RECT *rect, int size, UINT32 color, const RECT *clip )
{
    static const float *const lines[] =
    {
        glyph_rectangle, glyph_window, glyph_full_screen, NULL, glyph_close, glyph_new, glyph_pen,
        glyph_highlighter, glyph_eraser, glyph_crop, NULL, NULL, glyph_copy, glyph_save, glyph_folder,
    };
    float k = size / 20.0f, radius = max( 0.55f, 0.75f * k );
    float x = (rect->left + rect->right - size) / 2.0f, y = (rect->top + rect->bottom - size) / 2.0f;

    switch (glyph)
    {
    case GLYPH_FREEFORM: draw_freeform( dst, x, y, k, radius, color, clip ); break;
    case GLYPH_UNDO:     draw_undo( dst, FALSE, x, y, k, radius, color, clip ); break;
    case GLYPH_REDO:     draw_undo( dst, TRUE, x, y, k, radius, color, clip ); break;
    default:             draw_lines( dst, lines[glyph], x, y, k, radius, color, clip ); break;
    }
}
