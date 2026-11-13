/*
 * Paint: what draws into a picture
 *
 * The pencil, the eraser and the shapes are hard-edged, as Paint's always
 * were: a pixel is the colour or it is not. The brushes lay their marks
 * pixel by pixel, each its own way.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <math.h>
#include "paint.h"

#define PI 3.14159265358979

UINT32 to_pix( COLORREF c )
{
    return 0xff000000 | (GetRValue( c ) << 16) | (GetGValue( c ) << 8) | GetBValue( c );
}

COLORREF from_pix( UINT32 p )
{
    return RGB( (p >> 16) & 0xff, (p >> 8) & 0xff, p & 0xff );
}

static inline void put( struct image *im, int x, int y, UINT32 p )
{
    if (x >= 0 && y >= 0 && x < im->width && y < im->height) im->bits[(size_t)y * im->width + x] = p;
}

static inline void blend( struct image *im, int x, int y, COLORREF c, int a )
{
    UINT32 *p, d;
    int r, g, b;

    if (x < 0 || y < 0 || x >= im->width || y >= im->height || a <= 0) return;
    p = im->bits + (size_t)y * im->width + x;
    if (a >= 255)
    {
        *p = to_pix( c );
        return;
    }
    /* over what is there, which may be see-through itself */
    d = *p;
    {
        int da = d >> 24, rest = da * (255 - a) / 255, oa = a + rest;
        if (!oa) return;
        r = (GetRValue( c ) * a + ((d >> 16) & 0xff) * rest) / oa;
        g = (GetGValue( c ) * a + ((d >> 8) & 0xff) * rest) / oa;
        b = (GetBValue( c ) * a + (d & 0xff) * rest) / oa;
        *p = ((UINT32)oa << 24) | (r << 16) | (g << 8) | b;
    }
}

/* a square or a round dot of width pixels, its middle at x, y */
static void dot( struct image *im, int x, int y, UINT32 p, int width, BOOL square )
{
    int lo = -(width - 1) / 2, hi = width / 2;
    double r2 = width * width / 4.0;

    if (width <= 1)
    {
        put( im, x, y, p );
        return;
    }
    for (int j = lo; j <= hi; j++)
        for (int i = lo; i <= hi; i++)
        {
            double dx = i - (hi + lo) / 2.0, dy = j - (hi + lo) / 2.0;
            if (square || dx * dx + dy * dy <= r2 + 0.25) put( im, x + i, y + j, p );
        }
}

static void line_pix( struct image *im, int x0, int y0, int x1, int y1, UINT32 p, int width, BOOL square )
{
    int dx = abs( x1 - x0 ), dy = -abs( y1 - y0 ), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;

    for (;;)
    {
        int e2 = 2 * err;   /* both steps decided by the error before either */

        dot( im, x0, y0, p, width, square );
        if (x0 == x1 && y0 == y1) break;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void op_line( struct image *im, int x0, int y0, int x1, int y1, COLORREF color, int width, BOOL square )
{
    line_pix( im, x0, y0, x1, y1, to_pix( color ), width, square );
}

/* the eraser leaves the canvas see-through, as Windows 11's Paint does */
void op_erase( struct image *im, int x0, int y0, int x1, int y1, int size )
{
    line_pix( im, x0, y0, x1, y1, 0, size, TRUE );
}

static unsigned int next_rand( unsigned int *seed )
{
    *seed = *seed * 1103515245 + 12345;
    return (*seed >> 16) & 0x7fff;
}

/* one mark of a brush with its middle at x, y */
static void brush_mark( struct image *im, enum brush_kind kind, double x, double y, COLORREF color, int size,
                        unsigned int *seed )
{
    double r = max( size / 2.0, 0.5 );
    int x0 = (int)floor( x - r - 1 ), x1 = (int)ceil( x + r + 1 ), y0 = (int)floor( y - r - 1 ), y1 = (int)ceil( y + r + 1 );

    switch (kind)
    {
    case BR_BRUSH:
        for (int j = y0; j <= y1; j++)
            for (int i = x0; i <= x1; i++)
            {
                double d = hypot( i + 0.5 - x, j + 0.5 - y );
                if (d <= r - 0.5) blend( im, i, j, color, 255 );
                else if (d < r + 0.5) blend( im, i, j, color, (int)((r + 0.5 - d) * 255) );
            }
        break;
    case BR_CALLIGRAPHY:   /* a flat nib at 45 degrees, down to the right */
    case BR_PEN:           /* and the other way */
    {
        double sgn = kind == BR_CALLIGRAPHY ? 1 : -1;
        for (int k = (int)-r; k <= (int)r; k++)
        {
            int px_ = (int)floor( x + k * 0.7071 ), py_ = (int)floor( y + sgn * k * 0.7071 );
            blend( im, px_, py_, color, 255 );
            blend( im, px_ + 1, py_, color, 255 );
        }
        break;
    }
    case BR_AIRBRUSH:
    {
        int n = (int)(r * r * 0.35) + 2;
        for (int k = 0; k < n; k++)
        {
            double a = next_rand( seed ) / 32768.0 * 2 * PI, d = sqrt( next_rand( seed ) / 32768.0 ) * r * 1.6;
            blend( im, (int)floor( x + cos( a ) * d ), (int)floor( y + sin( a ) * d ), color, 255 );
        }
        break;
    }
    case BR_OIL:   /* bristles: a round mark of uneven strength */
        for (int j = y0; j <= y1; j++)
            for (int i = x0; i <= x1; i++)
            {
                double d = hypot( i + 0.5 - x, j + 0.5 - y );
                if (d > r + 0.5) continue;
                blend( im, i, j, color, 120 + (int)(((i * 7 + j * 13) & 15) * 8) );
            }
        break;
    case BR_CRAYON:   /* the paper's grain shows through */
        for (int j = y0; j <= y1; j++)
            for (int i = x0; i <= x1; i++)
            {
                double d = hypot( i + 0.5 - x, j + 0.5 - y );
                if (d > r) continue;
                if (next_rand( seed ) % 100 < 45) blend( im, i, j, color, 200 );
            }
        break;
    case BR_MARKER:   /* a square, half through: it darkens where it goes twice */
        for (int j = (int)floor( y - r ); j < (int)ceil( y + r ); j++)
            for (int i = (int)floor( x - r ); i < (int)ceil( x + r ); i++)
                blend( im, i, j, color, 18 );
        break;
    case BR_NATURAL:   /* a soft pencil: thin, grey at its edge */
        for (int j = y0; j <= y1; j++)
            for (int i = x0; i <= x1; i++)
            {
                double d = hypot( i + 0.5 - x, j + 0.5 - y ), rr = max( r * 0.6, 0.7 );
                if (d > rr + 0.5) continue;
                blend( im, i, j, color, (next_rand( seed ) % 100 < 70 ? 110 : 40) );
            }
        break;
    case BR_WATERCOLOR:   /* soft, lighter to its edge */
        for (int j = y0; j <= y1; j++)
            for (int i = x0; i <= x1; i++)
            {
                double d = hypot( i + 0.5 - x, j + 0.5 - y );
                if (d >= r) continue;
                blend( im, i, j, color, (int)(14 * (1 - d / r)) + 2 );
            }
        break;
    default:
        break;
    }
}

void op_brush_segment( struct image *im, enum brush_kind kind, double x0, double y0, double x1, double y1,
                       COLORREF color, int size, unsigned int *seed )
{
    double len = hypot( x1 - x0, y1 - y0 );
    /* marks that overlap a lot, except what builds up under itself */
    double step = kind == BR_MARKER || kind == BR_WATERCOLOR ? max( size / 4.0, 1.0 ) :
                  kind == BR_AIRBRUSH ? max( size / 2.0, 1.0 ) : 0.75;
    int n = (int)(len / step);

    if (kind == BR_AIRBRUSH && len < 0.5)
    {
        brush_mark( im, kind, x1, y1, color, size, seed );
        return;
    }
    for (int i = 1; i <= max( n, 1 ); i++)
    {
        double t = n ? (double)i / n : 1.0;
        brush_mark( im, kind, x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, color, size, seed );
    }
}

/* the area of the same colour as the pixel at x, y, joined by its sides */
void op_flood_fill( struct image *im, int x, int y, COLORREF color )
{
    UINT32 target, p = to_pix( color );
    int *stack, top = 0, cap;

    if (x < 0 || y < 0 || x >= im->width || y >= im->height) return;
    target = im->bits[(size_t)y * im->width + x];
    if (target == p) return;
    cap = 4096;
    if (!(stack = malloc( cap * 2 * sizeof(int) ))) return;
    stack[top++] = x;
    stack[top++] = y;
    while (top)
    {
        int sy = stack[--top], sx = stack[--top], lx = sx, rx = sx;
        UINT32 *row = im->bits + (size_t)sy * im->width;

        if (row[sx] != target) continue;
        while (lx > 0 && row[lx - 1] == target) lx--;
        while (rx < im->width - 1 && row[rx + 1] == target) rx++;
        for (int i = lx; i <= rx; i++) row[i] = p;
        for (int dy = -1; dy <= 1; dy += 2)
        {
            int ny = sy + dy;
            UINT32 *nrow;
            BOOL in = FALSE;

            if (ny < 0 || ny >= im->height) continue;
            nrow = im->bits + (size_t)ny * im->width;
            for (int i = lx; i <= rx; i++)
            {
                if (nrow[i] == target)
                {
                    if (!in)
                    {
                        if (top + 2 > cap * 2)
                        {
                            int *grown = realloc( stack, cap * 4 * sizeof(int) );
                            if (!grown) { free( stack ); return; }
                            stack = grown;
                            cap *= 2;
                        }
                        stack[top++] = i;
                        stack[top++] = ny;
                        in = TRUE;
                    }
                }
                else in = FALSE;
            }
        }
    }
    free( stack );
}

/**********************************************************************
 *          Shapes: outlines of points in a box, filled by scanlines
 */

#define MAXPTS 512

struct ptf { double x, y; };

static int ellipse_pts( struct ptf *out, double cx, double cy, double rx, double ry, double a0, double a1, int n )
{
    for (int i = 0; i <= n; i++)
    {
        double a = a0 + (a1 - a0) * i / n;
        out[i].x = cx + rx * cos( a );
        out[i].y = cy + ry * sin( a );
    }
    return n + 1;
}

static int regular( struct ptf *out, double cx, double cy, double rx, double ry, int sides, double start )
{
    for (int i = 0; i < sides; i++)
    {
        double a = start + 2 * PI * i / sides;
        out[i].x = cx + rx * cos( a );
        out[i].y = cy + ry * sin( a );
    }
    return sides;
}

static int star( struct ptf *out, double cx, double cy, double rx, double ry, int points, double inner )
{
    for (int i = 0; i < points * 2; i++)
    {
        double a = -PI / 2 + PI * i / points, f = (i & 1) ? inner : 1.0;
        out[i].x = cx + rx * f * cos( a );
        out[i].y = cy + ry * f * sin( a );
    }
    return points * 2;
}

/* the outline of a shape in its box, as one closed polygon (or open for the
 * line and the curve); returns how many points */
static int shape_outline( enum shape_kind kind, const POINT *pts, int count, const RECT *box, struct ptf *out,
                          BOOL *closed )
{
    double l = box->left, t = box->top, r = box->right - 1, b = box->bottom - 1;
    double w = r - l, h = b - t, cx = (l + r) / 2, cy = (t + b) / 2;
    int n = 0;

    *closed = TRUE;
    switch (kind)
    {
    case SH_LINE:
        out[0].x = pts[0].x; out[0].y = pts[0].y;
        out[1].x = pts[1].x; out[1].y = pts[1].y;
        *closed = FALSE;
        return 2;
    case SH_CURVE:
    {
        /* a cubic Bezier from the first point to the second, bent towards
         * the third and the fourth */
        struct ptf p0 = { pts[0].x, pts[0].y }, p3 = { pts[1].x, pts[1].y };
        struct ptf p1 = count > 2 ? (struct ptf){ pts[2].x, pts[2].y } : p0;
        struct ptf p2 = count > 3 ? (struct ptf){ pts[3].x, pts[3].y } : (count > 2 ? p1 : p3);
        int steps = 96;
        for (int i = 0; i <= steps; i++)
        {
            double s = (double)i / steps, u = 1 - s;
            out[i].x = u * u * u * p0.x + 3 * u * u * s * p1.x + 3 * u * s * s * p2.x + s * s * s * p3.x;
            out[i].y = u * u * u * p0.y + 3 * u * u * s * p1.y + 3 * u * s * s * p2.y + s * s * s * p3.y;
        }
        *closed = FALSE;
        return steps + 1;
    }
    case SH_OVAL:
        return ellipse_pts( out, cx, cy, w / 2, h / 2, 0, 2 * PI, 180 ) - 1;
    case SH_RECT:
        out[0] = (struct ptf){ l, t }; out[1] = (struct ptf){ r, t };
        out[2] = (struct ptf){ r, b }; out[3] = (struct ptf){ l, b };
        return 4;
    case SH_ROUNDRECT:
    {
        double rad = min( min( w, h ) / 2, max( min( w, h ) / 6, 2 ) );
        n += ellipse_pts( out + n, r - rad, t + rad, rad, rad, -PI / 2, 0, 16 );
        n += ellipse_pts( out + n, r - rad, b - rad, rad, rad, 0, PI / 2, 16 );
        n += ellipse_pts( out + n, l + rad, b - rad, rad, rad, PI / 2, PI, 16 );
        n += ellipse_pts( out + n, l + rad, t + rad, rad, rad, PI, 1.5 * PI, 16 );
        return n;
    }
    case SH_POLYGON:
        for (int i = 0; i < count && i < MAXPTS; i++) out[i] = (struct ptf){ pts[i].x, pts[i].y };
        return min( count, MAXPTS );
    case SH_TRIANGLE:
        out[0] = (struct ptf){ cx, t }; out[1] = (struct ptf){ r, b }; out[2] = (struct ptf){ l, b };
        return 3;
    case SH_RTRIANGLE:
        out[0] = (struct ptf){ l, t }; out[1] = (struct ptf){ r, b }; out[2] = (struct ptf){ l, b };
        return 3;
    case SH_DIAMOND:
        out[0] = (struct ptf){ cx, t }; out[1] = (struct ptf){ r, cy };
        out[2] = (struct ptf){ cx, b }; out[3] = (struct ptf){ l, cy };
        return 4;
    case SH_PENTAGON:
        return regular( out, cx, cy + h * 0.05, w / 2 / 0.951, h / 2 / 0.905, 5, -PI / 2 );
    case SH_HEXAGON:
        out[0] = (struct ptf){ l + w / 4, t }; out[1] = (struct ptf){ r - w / 4, t };
        out[2] = (struct ptf){ r, cy }; out[3] = (struct ptf){ r - w / 4, b };
        out[4] = (struct ptf){ l + w / 4, b }; out[5] = (struct ptf){ l, cy };
        return 6;
    case SH_ARROW_RIGHT:
    case SH_ARROW_LEFT:
    case SH_ARROW_UP:
    case SH_ARROW_DOWN:
    {
        /* drawn pointing right in a unit box, then turned */
        static const struct ptf unit[7] = { { 0, 0.25 }, { 0.55, 0.25 }, { 0.55, 0 }, { 1, 0.5 }, { 0.55, 1 }, { 0.55, 0.75 }, { 0, 0.75 } };
        for (int i = 0; i < 7; i++)
        {
            double u = unit[i].x, v = unit[i].y;
            switch (kind)
            {
            case SH_ARROW_LEFT: out[i] = (struct ptf){ r - u * w, t + v * h }; break;
            case SH_ARROW_UP: out[i] = (struct ptf){ l + v * w, b - u * h }; break;
            case SH_ARROW_DOWN: out[i] = (struct ptf){ l + v * w, t + u * h }; break;
            default: out[i] = (struct ptf){ l + u * w, t + v * h }; break;
            }
        }
        return 7;
    }
    case SH_STAR4: return star( out, cx, cy, w / 2, h / 2, 4, 0.38 );
    case SH_STAR5:
    {
        int k = star( out, cx, cy + h * 0.05, w / 2 / 0.951, h / 2 / 0.905, 5, 0.382 );
        return k;
    }
    case SH_STAR6: return star( out, cx, cy, w / 2, h / 2, 6, 0.55 );
    case SH_CALLOUT_RECT:
    {
        double bh = h * 0.75, rad = min( min( w, bh ) / 2, max( min( w, bh ) / 6, 2 ) ), bb = t + bh;
        n += ellipse_pts( out + n, r - rad, t + rad, rad, rad, -PI / 2, 0, 12 );
        n += ellipse_pts( out + n, r - rad, bb - rad, rad, rad, 0, PI / 2, 12 );
        out[n++] = (struct ptf){ l + w * 0.4, bb };
        out[n++] = (struct ptf){ l + w * 0.18, b };
        out[n++] = (struct ptf){ l + w * 0.22, bb };
        n += ellipse_pts( out + n, l + rad, bb - rad, rad, rad, PI / 2, PI, 12 );
        n += ellipse_pts( out + n, l + rad, t + rad, rad, rad, PI, 1.5 * PI, 12 );
        return n;
    }
    case SH_CALLOUT_OVAL:
    {
        double bh = h * 0.78, ecy = t + bh / 2;
        /* the oval from the tail's right side round to its left side */
        n += ellipse_pts( out + n, cx, ecy, w / 2, bh / 2, 0.62 * PI, 2.38 * PI, 120 );
        out[n++] = (struct ptf){ l + w * 0.14, b };
        return n;
    }
    case SH_CALLOUT_CLOUD:
    {
        /* bumps round an oval, a tail of two small rounds */
        double bh = h * 0.78, ecy = t + bh / 2;
        int bumps = 10;
        for (int i = 0; i < bumps; i++)
        {
            double a0 = 2 * PI * i / bumps, a1 = 2 * PI * (i + 1) / bumps, am = (a0 + a1) / 2;
            double bx = cx + w / 2 * 0.86 * cos( am ), by = ecy + bh / 2 * 0.86 * sin( am );
            double br = sin( PI / bumps ) * 1.05;
            n += ellipse_pts( out + n, bx, by, w / 2 * br, bh / 2 * br, am - PI / 2, am + PI / 2, 10 );
        }
        return n;
    }
    case SH_HEART:
    {
        for (int i = 0; i < 160; i++)
        {
            double s = 2 * PI * i / 160;
            double hx = 16 * pow( sin( s ), 3 );
            double hy = 13 * cos( s ) - 5 * cos( 2 * s ) - 2 * cos( 3 * s ) - cos( 4 * s );
            out[n++] = (struct ptf){ cx + hx / 16 * w / 2, t + (12 - hy) / 29 * h };
        }
        return n;
    }
    case SH_LIGHTNING:
    {
        static const struct ptf unit[11] = { { 0.38, 0 }, { 0.62, 0.30 }, { 0.53, 0.34 }, { 0.80, 0.60 },
                                             { 0.72, 0.64 }, { 1.0, 1.0 }, { 0.48, 0.70 }, { 0.58, 0.66 },
                                             { 0.22, 0.44 }, { 0.32, 0.40 }, { 0.0, 0.15 } };
        for (int i = 0; i < 11; i++) out[i] = (struct ptf){ l + unit[i].x * w, t + unit[i].y * h };
        return 11;
    }
    default:
        return 0;
    }
}

static int cmp_double( const void *a, const void *b )
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static void fill_polygon( struct image *im, const struct ptf *p, int n, COLORREF color )
{
    double ymin = 1e9, ymax = -1e9, xs[MAXPTS * 2];
    UINT32 pix = to_pix( color );

    for (int i = 0; i < n; i++)
    {
        ymin = min( ymin, p[i].y );
        ymax = max( ymax, p[i].y );
    }
    for (int y = max( 0, (int)floor( ymin ) ); y <= min( im->height - 1, (int)ceil( ymax ) ); y++)
    {
        double sy = y + 0.5;
        int k = 0;

        for (int i = 0; i < n && k < ARRAY_SIZE(xs); i++)
        {
            const struct ptf *a = &p[i], *b = &p[(i + 1) % n];
            if ((a->y <= sy && b->y > sy) || (b->y <= sy && a->y > sy))
                xs[k++] = a->x + (sy - a->y) * (b->x - a->x) / (b->y - a->y);
        }
        qsort( xs, k, sizeof(double), cmp_double );
        for (int i = 0; i + 1 < k; i += 2)
        {
            int x0 = max( 0, (int)ceil( xs[i] - 0.5 ) ), x1 = min( im->width - 1, (int)floor( xs[i + 1] - 0.5 ) );
            UINT32 *row = im->bits + (size_t)y * im->width;
            for (int x = x0; x <= x1; x++) row[x] = pix;
        }
    }
}

void op_shape( struct image *im, enum shape_kind kind, const POINT *pts, int count, const RECT *box,
               COLORREF outline_color, COLORREF fill_color, int width, BOOL outline, BOOL filled )
{
    static struct ptf p[MAXPTS * 2];
    BOOL closed;
    int n = shape_outline( kind, pts, count, box, p, &closed );

    if (n < 2) return;
    if (!closed) filled = FALSE, outline = TRUE;
    if (filled && n > 2) fill_polygon( im, p, n, fill_color );
    if (kind == SH_CALLOUT_CLOUD && filled)
    {
        /* the tail of the cloud: two small rounds down to the left */
        struct ptf q[64];
        double l = box->left, t = box->top, w = box->right - box->left, h = box->bottom - box->top;
        int k = ellipse_pts( q, l + w * 0.22, t + h * 0.86, w * 0.06, h * 0.05, 0, 2 * PI, 40 ) - 1;
        fill_polygon( im, q, k, fill_color );
        k = ellipse_pts( q, l + w * 0.12, t + h * 0.96, w * 0.035, h * 0.03, 0, 2 * PI, 40 ) - 1;
        fill_polygon( im, q, k, fill_color );
    }
    if (outline)
    {
        int segs = closed ? n : n - 1;
        for (int i = 0; i < segs; i++)
        {
            const struct ptf *a = &p[i], *b = &p[(i + 1) % n];
            op_line( im, (int)floor( a->x + 0.5 ), (int)floor( a->y + 0.5 ), (int)floor( b->x + 0.5 ),
                     (int)floor( b->y + 0.5 ), outline_color, width, FALSE );
        }
        if (kind == SH_CALLOUT_CLOUD)
        {
            struct ptf q[64];
            double l = box->left, t = box->top, w = box->right - box->left, h = box->bottom - box->top;
            int k = ellipse_pts( q, l + w * 0.22, t + h * 0.86, w * 0.06, h * 0.05, 0, 2 * PI, 40 );
            for (int i = 0; i + 1 < k; i++)
                op_line( im, (int)(q[i].x + 0.5), (int)(q[i].y + 0.5), (int)(q[i + 1].x + 0.5), (int)(q[i + 1].y + 0.5),
                         outline_color, width, FALSE );
            k = ellipse_pts( q, l + w * 0.12, t + h * 0.96, w * 0.035, h * 0.03, 0, 2 * PI, 40 );
            for (int i = 0; i + 1 < k; i++)
                op_line( im, (int)(q[i].x + 0.5), (int)(q[i].y + 0.5), (int)(q[i + 1].x + 0.5), (int)(q[i + 1].y + 0.5),
                         outline_color, width, FALSE );
        }
    }
}
