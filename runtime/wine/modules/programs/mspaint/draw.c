/*
 * Paint: colours, fonts and drawing
 *
 * What the program draws of its own goes into a 32-bit DIB first: the soft
 * shapes are written into its pixels, the text by GDI, and the whole is
 * copied to the window at once (as Arctic WordPad does).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <math.h>
#include "paint.h"

struct theme th;

#define RGBX(c) RGB( ((c) >> 16) & 0xff, ((c) >> 8) & 0xff, (c) & 0xff )

/* light or dark: the program's setting, or the app mode of the system */
void theme_load(void)
{
    DWORD light = 1, size = sizeof(light), mode = setting( L"Theme", 0 );

    RegGetValueW( HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                  L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &light, &size );
    th.dark = mode == 2 || (mode == 0 && !light);
    if (th.dark)
    {
        th.chrome = RGBX( 0x202020 );
        th.bar = RGBX( 0x2c2c2c );
        th.bar_line = RGBX( 0x1c1c1c );
        th.text = RGBX( 0xffffff );
        th.dim = RGBX( 0xc5c5c5 );
        th.faint = RGBX( 0x6a6a6a );
        th.hover = RGBX( 0x3a3a3a );
        th.pressed = RGBX( 0x333333 );
        th.checked = RGBX( 0x434a55 );
        th.checked_line = RGBX( 0x56606d );
        th.accent = RGBX( 0x4cc2ff );
        th.on_accent = RGBX( 0x000000 );
        th.field = RGBX( 0x2d2d2d );
        th.field_line = RGBX( 0x474747 );
        th.workspace = RGBX( 0x1b1b1b );
        th.popup = RGBX( 0x2c2c2c );
        th.popup_line = RGBX( 0x454545 );
    }
    else
    {
        th.chrome = RGBX( 0xf3f3f3 );
        th.bar = RGBX( 0xf9f9f9 );
        th.bar_line = RGBX( 0xe5e5e5 );
        th.text = RGBX( 0x1a1a1a );
        th.dim = RGBX( 0x5d5d5d );
        th.faint = RGBX( 0xbdbdbd );
        th.hover = RGBX( 0xeaeaea );
        th.pressed = RGBX( 0xe0e0e0 );
        th.checked = RGBX( 0xdde6f2 );
        th.checked_line = RGBX( 0xb7c9e0 );
        th.accent = RGBX( 0x005fb8 );
        th.on_accent = RGBX( 0xffffff );
        th.field = RGBX( 0xffffff );
        th.field_line = RGBX( 0xd1d1d1 );
        th.workspace = RGBX( 0xe6e9ee );
        th.popup = RGBX( 0xf9f9f9 );
        th.popup_line = RGBX( 0xd6d6d6 );
    }
}

COLORREF mix( COLORREF a, COLORREF b, int alpha )
{
    return RGB( (GetRValue( a ) * (255 - alpha) + GetRValue( b ) * alpha) / 255,
                (GetGValue( a ) * (255 - alpha) + GetGValue( b ) * alpha) / 255,
                (GetBValue( a ) * (255 - alpha) + GetBValue( b ) * alpha) / 255 );
}

/**********************************************************************
 *          The surface
 */

BOOL surface_size( struct surface *s, int width, int height )
{
    BITMAPINFO info = { { sizeof(info.bmiHeader) } };
    void *bits;
    HBITMAP bitmap;

    width = max( width, 1 );
    height = max( height, 1 );
    if (s->bitmap && s->width >= width && s->height >= height) return TRUE;
    width = max( width, s->width );
    height = max( height, s->height );
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    if (!(bitmap = CreateDIBSection( NULL, &info, DIB_RGB_COLORS, &bits, NULL, 0 ))) return FALSE;
    if (!s->dc) s->dc = CreateCompatibleDC( NULL );
    if (s->bitmap)
    {
        SelectObject( s->dc, s->old );
        DeleteObject( s->bitmap );
    }
    s->bitmap = bitmap;
    s->old = SelectObject( s->dc, bitmap );
    s->bits = bits;
    s->width = width;
    s->height = height;
    SetBkMode( s->dc, TRANSPARENT );
    return TRUE;
}

void surface_free( struct surface *s )
{
    if (s->bitmap)
    {
        SelectObject( s->dc, s->old );
        DeleteObject( s->bitmap );
    }
    if (s->dc) DeleteDC( s->dc );
    memset( s, 0, sizeof(*s) );
}

static inline UINT32 to_pixel( COLORREF c )
{
    return 0xff000000 | (GetRValue( c ) << 16) | (GetGValue( c ) << 8) | GetBValue( c );
}

static inline void blend_pixel( UINT32 *p, COLORREF c, int a )
{
    UINT32 d = *p;
    int r = (d >> 16) & 0xff, g = (d >> 8) & 0xff, b = d & 0xff;

    if (a <= 0) return;
    if (a >= 255)
    {
        *p = to_pixel( c );
        return;
    }
    r += (GetRValue( c ) - r) * a / 255;
    g += (GetGValue( c ) - g) * a / 255;
    b += (GetBValue( c ) - b) * a / 255;
    *p = 0xff000000 | (r << 16) | (g << 8) | b;
}

static BOOL clip_rect( struct surface *s, const RECT *rect, RECT *out )
{
    RECT all = { 0, 0, s->width, s->height };

    return IntersectRect( out, rect, &all );
}

void fill( struct surface *s, const RECT *rect, COLORREF color )
{
    RECT r;
    UINT32 pixel = to_pixel( color );

    GdiFlush();
    if (!clip_rect( s, rect, &r )) return;
    for (int y = r.top; y < r.bottom; y++)
    {
        UINT32 *p = s->bits + y * s->width;
        for (int x = r.left; x < r.right; x++) p[x] = pixel;
    }
}

/* how much of a pixel a rounded rectangle covers, 4 x 4 samples */
static int round_cover( float x, float y, float l, float t, float r, float b, float radius )
{
    int hits = 0;

    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++)
        {
            float sx = x + (i + 0.5f) / 4, sy = y + (j + 0.5f) / 4, dx, dy;

            if (sx < l || sx > r || sy < t || sy > b) continue;
            dx = max( max( l + radius - sx, sx - (r - radius) ), 0.0f );
            dy = max( max( t + radius - sy, sy - (b - radius) ), 0.0f );
            if (dx * dx + dy * dy <= radius * radius) hits++;
        }
    return hits * 255 / 16;
}

void fill_round( struct surface *s, const RECT *rect, float radius, COLORREF color, BYTE alpha )
{
    int corner = (int)ceilf( radius );
    RECT r;

    GdiFlush();
    if (!clip_rect( s, rect, &r )) return;
    for (int y = r.top; y < r.bottom; y++)
    {
        UINT32 *p = s->bits + y * s->width;
        BOOL middle = y >= rect->top + corner && y < rect->bottom - corner;

        for (int x = r.left; x < r.right; x++)
        {
            int a;

            if (middle || (x >= rect->left + corner && x < rect->right - corner)) a = 255;
            else a = round_cover( (float)x, (float)y, (float)rect->left, (float)rect->top, (float)rect->right,
                                  (float)rect->bottom, radius );
            blend_pixel( p + x, color, a * alpha / 255 );
        }
    }
}

/* a line one pixel wide along the inside of a rounded rectangle */
void outline_round( struct surface *s, const RECT *rect, float radius, COLORREF color, BYTE alpha )
{
    int corner = (int)ceilf( radius ) + 1;
    RECT r;

    GdiFlush();
    if (!clip_rect( s, rect, &r )) return;
    for (int y = r.top; y < r.bottom; y++)
    {
        UINT32 *p = s->bits + y * s->width;
        BOOL edge_row = y == rect->top || y == rect->bottom - 1;
        BOOL middle = y >= rect->top + corner && y < rect->bottom - corner;

        for (int x = r.left; x < r.right; x++)
        {
            int a;
            BOOL in_corner = !middle && (x < rect->left + corner || x >= rect->right - corner);

            if (in_corner)
            {
                int outer = round_cover( (float)x, (float)y, (float)rect->left, (float)rect->top,
                                         (float)rect->right, (float)rect->bottom, radius );
                int inner = round_cover( (float)x, (float)y, rect->left + 1.0f, rect->top + 1.0f,
                                         rect->right - 1.0f, rect->bottom - 1.0f, max( radius - 1, 0.0f ) );
                a = max( outer - inner, 0 );
            }
            else if (edge_row || x == rect->left || x == rect->right - 1) a = 255;
            else
            {
                x = max( x, rect->right - 2 );
                continue;
            }
            blend_pixel( p + x, color, a * alpha / 255 );
        }
    }
}

static int disc_cover( float x, float y, float cx, float cy, float r )
{
    int hits = 0;

    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++)
        {
            float dx = x + (i + 0.5f) / 4 - cx, dy = y + (j + 0.5f) / 4 - cy;
            if (dx * dx + dy * dy <= r * r) hits++;
        }
    return hits * 255 / 16;
}

void fill_circle( struct surface *s, float cx, float cy, float r, COLORREF color, BYTE alpha )
{
    GdiFlush();
    for (int y = max( 0, (int)(cy - r - 1) ); y < min( s->height, (int)(cy + r + 2) ); y++)
        for (int x = max( 0, (int)(cx - r - 1) ); x < min( s->width, (int)(cx + r + 2) ); x++)
        {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d = sqrtf( dx * dx + dy * dy );
            int a;

            if (d > r + 1) continue;
            a = d < r - 1 ? 255 : disc_cover( (float)x, (float)y, cx, cy, r );
            blend_pixel( s->bits + y * s->width + x, color, a * alpha / 255 );
        }
}

/* a circle's edge, width wide, inside r */
void ring( struct surface *s, float cx, float cy, float r, float width, COLORREF color, BYTE alpha )
{
    GdiFlush();
    for (int y = max( 0, (int)(cy - r - 1) ); y < min( s->height, (int)(cy + r + 2) ); y++)
        for (int x = max( 0, (int)(cx - r - 1) ); x < min( s->width, (int)(cx + r + 2) ); x++)
        {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d = sqrtf( dx * dx + dy * dy );
            int a;

            if (d > r + 1 || d < r - width - 1) continue;
            a = disc_cover( (float)x, (float)y, cx, cy, r ) - disc_cover( (float)x, (float)y, cx, cy, r - width );
            blend_pixel( s->bits + y * s->width + x, color, max( a, 0 ) * alpha / 255 );
        }
}

/* the soft shadow round a card */
void shadow_rect( struct surface *s, const RECT *rect, int spread, BYTE alpha )
{
    RECT r = *rect;

    InflateRect( &r, spread, spread );
    OffsetRect( &r, 0, spread / 3 );
    GdiFlush();
    for (int y = max( r.top, 0 ); y < min( r.bottom, s->height ); y++)
    {
        UINT32 *p = s->bits + y * s->width;
        for (int x = max( r.left, 0 ); x < min( r.right, s->width ); x++)
        {
            int dx = max( max( rect->left - x, x - (rect->right - 1) ), 0 );
            int dy = max( max( rect->top + spread / 3 - y, y - (rect->bottom - 1 + spread / 3) ), 0 );
            float d = sqrtf( (float)(dx * dx + dy * dy) ) / spread;
            int a;

            if (d >= 1.0f) continue;
            if (x >= rect->left && x < rect->right && y >= rect->top && y < rect->bottom) continue;
            a = (int)(alpha * (1 - d) * (1 - d));
            blend_pixel( p + x, RGB( 0, 0, 0 ), a );
        }
    }
}

/* the colours round a circle, white in the middle: "Edit colours" */
void color_wheel( struct surface *s, float cx, float cy, float r )
{
    GdiFlush();
    for (int y = max( 0, (int)(cy - r - 1) ); y < min( s->height, (int)(cy + r + 2) ); y++)
        for (int x = max( 0, (int)(cx - r - 1) ); x < min( s->width, (int)(cx + r + 2) ); x++)
        {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d = sqrtf( dx * dx + dy * dy );
            float h = atan2f( dy, dx ) / (2 * 3.14159265f) + 0.5f, sat = min( d / r, 1.0f ), f;
            int i, a = disc_cover( (float)x, (float)y, cx, cy, r );
            float rr, gg, bb, q, t;

            if (!a) continue;
            h *= 6;
            i = (int)h % 6;
            f = h - floorf( h );
            q = 1 - f;
            t = f;
            switch (i)
            {
            case 0: rr = 1; gg = t; bb = 0; break;
            case 1: rr = q; gg = 1; bb = 0; break;
            case 2: rr = 0; gg = 1; bb = t; break;
            case 3: rr = 0; gg = q; bb = 1; break;
            case 4: rr = t; gg = 0; bb = 1; break;
            default: rr = 1; gg = 0; bb = q; break;
            }
            rr = 1 - sat * (1 - rr);
            gg = 1 - sat * (1 - gg);
            bb = 1 - sat * (1 - bb);
            blend_pixel( s->bits + y * s->width + x, RGB( (int)(rr * 255), (int)(gg * 255), (int)(bb * 255) ), a );
        }
}

/**********************************************************************
 *          A picture zoomed into a surface
 */

/* the picture halved until it is no more than twice what is drawn: a
 * picture made smaller is the mean of the pixels it covers, near enough */
static struct mip
{
    const UINT32 *source;
    int width, height, level;
    UINT32 *bits;
    int w, h;
} mips[3];
static int mip_next;

static UINT32 *halve( const UINT32 *src, int sw, int sh, int *w, int *h )
{
    int nw = max( 1, sw / 2 ), nh = max( 1, sh / 2 );
    UINT32 *dst = malloc( (size_t)nw * nh * 4 );

    if (!dst) return NULL;
    for (int y = 0; y < nh; y++)
    {
        const UINT32 *r0 = src + (size_t)min( y * 2, sh - 1 ) * sw, *r1 = src + (size_t)min( y * 2 + 1, sh - 1 ) * sw;
        for (int x = 0; x < nw; x++)
        {
            int x0 = min( x * 2, sw - 1 ), x1 = min( x * 2 + 1, sw - 1 );
            UINT32 a = r0[x0], b = r0[x1], c = r1[x0], d = r1[x1], out = 0;
            for (int k = 0; k < 32; k += 8)
                out |= ((((a >> k) & 0xff) + ((b >> k) & 0xff) + ((c >> k) & 0xff) + ((d >> k) & 0xff) + 2) / 4) << k;
            dst[(size_t)y * nw + x] = out;
        }
    }
    *w = nw;
    *h = nh;
    return dst;
}

/* the level asked for; *owned when the caller frees it */
static const UINT32 *mip_level( const UINT32 *bits, int width, int height, int level, int *w, int *h,
                                BOOL cache, BOOL *owned )
{
    const UINT32 *src = bits;
    int sw = width, sh = height;

    *owned = FALSE;
    *w = width;
    *h = height;
    if (!level) return bits;
    if (cache)
    {
        for (int i = 0; i < ARRAY_SIZE(mips); i++)
            if (mips[i].bits && mips[i].source == bits && mips[i].width == width && mips[i].height == height &&
                mips[i].level == level)
            {
                *w = mips[i].w;
                *h = mips[i].h;
                return mips[i].bits;
            }
    }
    for (int l = 0; l < level; l++)
    {
        int nw, nh;
        UINT32 *dst = halve( src, sw, sh, &nw, &nh );

        if (!dst) break;
        if (src != bits) free( (void *)src );
        src = dst;
        sw = nw;
        sh = nh;
    }
    *w = sw;
    *h = sh;
    if (src == bits) return bits;
    if (cache)
    {
        struct mip *m = &mips[mip_next];
        mip_next = (mip_next + 1) % ARRAY_SIZE(mips);
        free( m->bits );
        m->source = bits;
        m->width = width;
        m->height = height;
        m->level = level;
        m->bits = (UINT32 *)src;
        m->w = sw;
        m->h = sh;
    }
    else *owned = TRUE;
    return src;
}

/* forget what was made of a picture whose pixels changed */
void blit_forget( const UINT32 *bits )
{
    for (int i = 0; i < ARRAY_SIZE(mips); i++)
        if (!bits || mips[i].source == bits)
        {
            free( mips[i].bits );
            memset( &mips[i], 0, sizeof(mips[i]) );
        }
}

static inline UINT32 over_back( UINT32 p, UINT32 back )
{
    unsigned int a = p >> 24, out = 0xff000000;

    if (a == 255) return p;
    if (!a) return back | 0xff000000;
    for (int k = 0; k < 24; k += 8)
        out |= ((((p >> k) & 0xff) * a + ((back >> k) & 0xff) * (255 - a)) / 255) << k;
    return out;
}

/* the picture with its top left corner at ox, oy of the surface, scale
 * screen pixels to one of its own. A picture is drawn over a chequer where
 * it is see-through, a layer over what the surface has */
void blit_image( struct surface *s, const RECT *clip, const UINT32 *bits, int width, int height,
                 double ox, double oy, double scale, UINT flags )
{
    RECT r, img = { (LONG)floor( ox ), (LONG)floor( oy ), (LONG)ceil( ox + width * scale ), (LONG)ceil( oy + height * scale ) };
    const UINT32 *src;
    int level = 0, sw, sh;
    double ls = scale;
    BOOL owned, layer = (flags & BLIT_LAYER) != 0, smooth_up = (flags & BLIT_SMOOTH) != 0;

    GdiFlush();
    if (!bits || !clip_rect( s, clip, &r ) || !IntersectRect( &r, &r, &img )) return;
    while (ls < 0.5 && (width >> (level + 1)) > 0 && (height >> (level + 1)) > 0)
    {
        level++;
        ls *= 2;
    }
    src = mip_level( bits, width, height, level, &sw, &sh, !layer, &owned );
    ls = scale * width / sw;   /* exact, after the rounding of halves */

    for (int y = r.top; y < r.bottom; y++)
    {
        UINT32 *out = s->bits + (size_t)y * s->width;
        double fy = (y + 0.5 - oy) / ls - 0.5;

        for (int x = r.left; x < r.right; x++)
        {
            double fx = (x + 0.5 - ox) / ls - 0.5;
            UINT32 p, back = layer ? out[x] : (((x - img.left) / 8 + (y - img.top) / 8) & 1) ? 0xffe8e8e8 : 0xffffffff;

            if (ls >= 1.0 && !smooth_up)
            {
                int ix = (int)floor( (x + 0.5 - ox) / ls ), iy = (int)floor( (y + 0.5 - oy) / ls );
                ix = max( 0, min( sw - 1, ix ) );
                iy = max( 0, min( sh - 1, iy ) );
                p = src[(size_t)iy * sw + ix];
            }
            else
            {
                int x0 = (int)floor( fx ), y0 = (int)floor( fy );
                int ax = (int)((fx - x0) * 256), ay = (int)((fy - y0) * 256);
                int x1 = min( sw - 1, max( 0, x0 + 1 ) ), y1 = min( sh - 1, max( 0, y0 + 1 ) );
                UINT32 a, b, c, d;

                x0 = min( sw - 1, max( 0, x0 ) );
                y0 = min( sh - 1, max( 0, y0 ) );
                a = src[(size_t)y0 * sw + x0];
                b = src[(size_t)y0 * sw + x1];
                c = src[(size_t)y1 * sw + x0];
                d = src[(size_t)y1 * sw + x1];
                p = 0;
                for (int k = 0; k < 32; k += 8)
                {
                    int top = ((a >> k) & 0xff) * (256 - ax) + ((b >> k) & 0xff) * ax;
                    int bot = ((c >> k) & 0xff) * (256 - ax) + ((d >> k) & 0xff) * ax;
                    p |= (UINT32)(((top * (256 - ay) + bot * ay) >> 16) & 0xff) << k;
                }
            }
            out[x] = over_back( p, back );
        }
    }
    if (owned) free( (void *)src );
}

/**********************************************************************
 *          The pictures (tools/paint-icons.py)
 */

#define ROLES 6
struct icon_entry
{
    char name[24];
    BYTE mask;
    const BYTE *data[16][ROLES];      /* per size and role: the run-length coverage */
};

static struct icon_entry *icon_table;
static int icon_count, size_count;
static BYTE sizes[16];

static const BYTE *skip_rle( const BYTE *p, const BYTE *end, int pixels )
{
    int done = 0;

    while (done < pixels && p + 2 <= end)
    {
        done += p[0] + p[1];
        p += 2 + p[1];
    }
    return p;
}

static void load_icons(void)
{
    HRSRC res = FindResourceW( instance, MAKEINTRESOURCEW( IDR_ICONS ), (LPCWSTR)RT_RCDATA );
    const BYTE *p, *end;
    DWORD len;

    if (icon_table || !res) return;
    if (!(p = LockResource( LoadResource( instance, res ) ))) return;
    len = SizeofResource( instance, res );
    end = p + len;
    if (len < 7 || memcmp( p, "AMI1", 4 )) return;
    icon_count = p[4] | (p[5] << 8);
    size_count = min( p[6], 16 );
    memcpy( sizes, p + 7, size_count );
    p += 7 + p[6];
    if (!(icon_table = calloc( icon_count, sizeof(*icon_table) ))) return;
    for (int i = 0; i < icon_count && p < end; i++)
    {
        int n = *p++;

        memcpy( icon_table[i].name, p, min( n, 23 ) );
        p += n;
        icon_table[i].mask = *p++;
        for (int s = 0; s < size_count; s++)
            for (int r = 0; r < ROLES; r++)
            {
                if (!(icon_table[i].mask & (1 << r))) continue;
                icon_table[i].data[s][r] = p;
                p = skip_rle( p, end, sizes[s] * sizes[s] );
            }
    }
}

static struct icon_entry *find_icon( const char *name )
{
    if (!name) return NULL;
    load_icons();
    for (int i = 0; i < icon_count; i++)
        if (!strcmp( icon_table[i].name, name )) return &icon_table[i];
    return NULL;
}

BOOL icon_exists( const char *name )
{
    return find_icon( name ) != NULL;
}

void draw_icon( struct surface *s, const char *name, int x, int y, int size, enum icon_state state )
{
    struct icon_entry *icon = find_icon( name );
    COLORREF colors[ROLES];
    BOOL used[ROLES];
    int index = -1;
    BYTE *cover;

    if (!icon) return;
    for (int i = 0; i < size_count; i++)
        if (sizes[i] == size) index = i;
    if (index < 0)   /* no such size: the nearest smaller one, in the middle */
    {
        for (int i = 0; i < size_count; i++)
            if (sizes[i] <= size && (index < 0 || sizes[i] > sizes[index])) index = i;
        if (index < 0) index = 0;
        x += (size - sizes[index]) / 2;
        y += (size - sizes[index]) / 2;
        size = sizes[index];
    }

    if (th.dark)
    {
        colors[0] = RGBX( 0xe6e6e6 ); colors[1] = RGBX( 0x4cc2ff ); colors[2] = CLR_NONE;
    }
    else
    {
        colors[0] = RGBX( 0x1f1f1f ); colors[1] = RGBX( 0x005fb8 ); colors[2] = RGBX( 0xffffff );
    }
    colors[3] = RGBX( 0xffffff );
    colors[4] = RGBX( 0x2b2b2b );
    colors[5] = RGBX( 0x8a8a8a );
    if (state == IS_DISABLED)
    {
        colors[0] = colors[1] = colors[4] = colors[5] = th.faint;
    }
    else if (state == IS_WHITE)
    {
        colors[0] = colors[1] = colors[3] = colors[4] = colors[5] = RGBX( 0xffffff );
        colors[2] = CLR_NONE;
    }
    else if (state == IS_ACCENT)
    {
        colors[0] = colors[1] = th.accent;
    }

    if (!(cover = malloc( size * size ))) return;
    GdiFlush();
    for (int r = 0; r < ROLES; r++)
    {
        const BYTE *p = icon->data[index][r];
        int done = 0;

        used[r] = (icon->mask & (1 << r)) && colors[r] != CLR_NONE;
        if (!(icon->mask & (1 << r)) || !p) continue;
        memset( cover, 0, size * size );
        while (done < size * size)
        {
            int zeros = p[0], count = p[1];

            done += zeros;
            if (count) memcpy( cover + done, p + 2, min( count, size * size - done ) );
            done += count;
            p += 2 + count;
        }
        if (!used[r]) continue;
        for (int j = 0; j < size; j++)
        {
            if (y + j < 0 || y + j >= s->height) continue;
            for (int i = 0; i < size; i++)
            {
                if (x + i < 0 || x + i >= s->width) continue;
                blend_pixel( s->bits + (y + j) * s->width + x + i, colors[r], cover[j * size + i] );
            }
        }
    }
    free( cover );
}

/* a picture as a bitmap with alpha, for the menus of the system */
HBITMAP icon_bitmap( const char *icon, int size )
{
    struct surface s = { 0 };
    BITMAPINFO info = { { sizeof(info.bmiHeader) } };
    HBITMAP bitmap;
    UINT32 *bits;

    if (!icon || !icon_exists( icon )) return NULL;
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    if (!(bitmap = CreateDIBSection( NULL, &info, DIB_RGB_COLORS, (void **)&bits, NULL, 0 ))) return NULL;
    /* drawn over black and over white: the difference is the alpha */
    if (surface_size( &s, size, size ))
    {
        RECT r = { 0, 0, size, size };
        UINT32 *over = malloc( size * size * sizeof(UINT32) );

        fill( &s, &r, RGB( 0, 0, 0 ) );
        draw_icon( &s, icon, 0, 0, size, IS_NORMAL );
        GdiFlush();
        for (int y = 0; y < size && over; y++) memcpy( over + y * size, s.bits + y * s.width, size * 4 );
        fill( &s, &r, RGB( 255, 255, 255 ) );
        draw_icon( &s, icon, 0, 0, size, IS_NORMAL );
        GdiFlush();
        for (int y = 0; y < size && over; y++)
            for (int x = 0; x < size; x++)
            {
                UINT32 b = over[y * size + x], w = s.bits[y * s.width + x];
                int a = 255 - (((w >> 8) & 0xff) - ((b >> 8) & 0xff));
                a = max( 0, min( 255, a ) );
                bits[y * size + x] = (a << 24) | (b & 0xffffff);
            }
        free( over );
    }
    surface_free( &s );
    return bitmap;
}

/**********************************************************************
 *          Glyphs: lines in a square of 16, drawn soft
 */

#define END  -1

static const float g_down[] = { 4, 6, 8, 10, 8, 10, 12, 6, END };
static const float g_up[] = { 4, 10, 8, 6, 8, 6, 12, 10, END };
static const float g_left[] = { 10, 4, 6, 8, 6, 8, 10, 12, END };
static const float g_right[] = { 6, 4, 10, 8, 10, 8, 6, 12, END };
static const float g_check[] = { 3, 8.5f, 6.5f, 12, 6.5f, 12, 13, 4.5f, END };
static const float g_min[] = { 3, 8, 13, 8, END };
static const float g_max[] = { 3, 3, 13, 3, 13, 3, 13, 13, 13, 13, 3, 13, 3, 13, 3, 3, END };
static const float g_restore[] = { 3, 5, 11, 5, 11, 5, 11, 13, 11, 13, 3, 13, 3, 13, 3, 5, 5, 3, 13, 3, 13, 3, 13, 11, END };
static const float g_close[] = { 3, 3, 13, 13, 13, 3, 3, 13, END };
static const float g_plus[] = { 8, 2.5f, 8, 13.5f, 2.5f, 8, 13.5f, 8, END };
static const float g_minus[] = { 2.5f, 8, 13.5f, 8, END };
static const float g_more[] = { 3, 8, 3.2f, 8, 8, 8, 8.2f, 8, 13, 8, 13.2f, 8, END };
static const float g_down_small[] = { 5, 6.5f, 8, 9.5f, 8, 9.5f, 11, 6.5f, END };

static const float *const glyph_lines[] =
{
    g_down, g_up, g_left, g_right, g_check, g_min, g_max, g_restore, g_close, g_plus, g_minus, g_more,
    g_down_small,
};

struct seg { float x0, y0, x1, y1; };

static float seg_dist2( const struct seg *s, float x, float y )
{
    float vx = s->x1 - s->x0, vy = s->y1 - s->y0, wx = x - s->x0, wy = y - s->y0, t, dx, dy;

    t = (wx * vx + wy * vy) / max( vx * vx + vy * vy, 1e-6f );
    t = max( 0.0f, min( 1.0f, t ) );
    dx = wx - t * vx;
    dy = wy - t * vy;
    return dx * dx + dy * dy;
}

void draw_glyph( struct surface *s, enum glyph glyph, int x, int y, int size, COLORREF color )
{
    struct seg segs[32];
    const float *p = glyph_lines[glyph];
    int n = 0;
    float scale = 16.0f / size, half;

    while (*p != END && n < ARRAY_SIZE(segs))
    {
        segs[n].x0 = p[0]; segs[n].y0 = p[1]; segs[n].x1 = p[2]; segs[n].y1 = p[3];
        n++;
        p += 4;
    }
    half = 0.5f * max( 1.0f, scale ) * (size >= 24 ? 1.15f : 1.0f);
    GdiFlush();
    for (int j = 0; j < size; j++)
    {
        if (y + j < 0 || y + j >= s->height) continue;
        for (int i = 0; i < size; i++)
        {
            int hits = 0;

            if (x + i < 0 || x + i >= s->width) continue;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++)
                {
                    float u = (i + (sx + 0.5f) / 4) * scale, v = (j + (sy + 0.5f) / 4) * scale;

                    for (int k = 0; k < n; k++)
                        if (seg_dist2( &segs[k], u, v ) <= half * half) { hits++; break; }
                }
            if (hits) blend_pixel( s->bits + (y + j) * s->width + x + i, color, hits * 255 / 16 );
        }
    }
}

/**********************************************************************
 *          Fonts and text
 */

static HFONT fonts[F_COUNT];
static WCHAR face[LF_FACESIZE];

const WCHAR *ui_face(void)
{
    if (!face[0])
    {
        NONCLIENTMETRICSW metrics = { .cbSize = sizeof(metrics) };

        SystemParametersInfoW( SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0 );
        lstrcpynW( face, metrics.lfMessageFont.lfFaceName, ARRAY_SIZE(face) );
        if (!face[0]) lstrcpyW( face, L"Segoe UI" );
    }
    return face;
}

void fonts_reset(void)
{
    for (int i = 0; i < F_COUNT; i++)
    {
        if (fonts[i]) DeleteObject( fonts[i] );
        fonts[i] = NULL;
    }
}

HFONT get_font( enum font_kind kind )
{
    static const struct { int height, weight; } spec[F_COUNT] =
    {
        [F_UI] = { 12, FW_NORMAL },
        [F_UI_SEMI] = { 12, FW_SEMIBOLD },
        [F_SMALL] = { 11, FW_NORMAL },
        [F_TITLE] = { 12, FW_NORMAL },
    };
    LOGFONTW lf = { 0 };

    if (fonts[kind]) return fonts[kind];
    lf.lfHeight = -px( spec[kind].height );
    lf.lfWeight = spec[kind].weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lstrcpynW( lf.lfFaceName, ui_face(), LF_FACESIZE );
    fonts[kind] = CreateFontIndirectW( &lf );
    return fonts[kind];
}

void draw_text( struct surface *s, const WCHAR *text, int len, const RECT *rect, HFONT font, COLORREF color, UINT format )
{
    HGDIOBJ old = SelectObject( s->dc, font );
    RECT r = *rect;

    SetTextColor( s->dc, color );
    SetBkMode( s->dc, TRANSPARENT );
    if (!(format & DT_WORDBREAK)) format |= DT_SINGLELINE | DT_VCENTER;
    DrawTextW( s->dc, text, len, &r, format | DT_NOPREFIX | DT_END_ELLIPSIS );
    SelectObject( s->dc, old );
}

int text_width( HFONT font, const WCHAR *text, int len )
{
    HDC hdc = GetDC( 0 );
    HGDIOBJ old = SelectObject( hdc, font );
    SIZE size = { 0 };

    if (len < 0) len = lstrlenW( text );
    GetTextExtentPoint32W( hdc, text, len, &size );
    SelectObject( hdc, old );
    ReleaseDC( 0, hdc );
    return size.cx;
}

/**********************************************************************
 *          Popups of our own and menus of the system
 */

static LRESULT CALLBACK popup_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct popup *p = (struct popup *)GetWindowLongPtrW( hwnd, GWLP_USERDATA );
    POINT pt = { GET_X( lparam ), GET_Y( lparam ) };

    switch (msg)
    {
    case WM_NCCREATE:
        SetWindowLongPtrW( hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lparam)->lpCreateParams );
        break;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint( hwnd, &ps );
        RECT r = { 0, 0, p->width, p->height };

        if (surface_size( &p->surf, p->width, p->height ))
        {
            fill( &p->surf, &r, th.popup );
            p->paint( p );
            GdiFlush();
            BitBlt( hdc, 0, 0, p->width, p->height, p->surf.dc, 0, 0, SRCCOPY );
        }
        EndPaint( hwnd, &ps );
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE:
    {
        int hot = p->hit( p, pt );
        if (hot != p->hot)
        {
            p->hot = hot;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        return 0;
    }
    case WM_LBUTTONUP:
    {
        RECT r;
        GetClientRect( hwnd, &r );
        if (!PtInRect( &r, pt )) return 0;
        if ((p->result = p->hit( p, pt )) >= 0) p->done = TRUE;
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    {
        RECT r;
        GetClientRect( hwnd, &r );
        if (!PtInRect( &r, pt ))
        {
            p->result = -1;
            p->done = TRUE;
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE)
        {
            p->result = -1;
            p->done = TRUE;
        }
        return 0;
    case WM_CAPTURECHANGED:
        if ((HWND)lparam != hwnd) p->done = TRUE;
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

int run_popup( HWND owner, const RECT *anchor, struct popup *p )
{
    static BOOL registered;
    MONITORINFO mon = { sizeof(mon) };
    int x = anchor->left, y = anchor->bottom + px( 2 );
    MSG msg;

    if (!registered)
    {
        WNDCLASSW cls = { CS_DROPSHADOW, popup_proc, 0, 0, instance, NULL, NULL, NULL, NULL, L"ArcticPaintPopup" };
        cls.hCursor = LoadCursorW( NULL, (LPCWSTR)IDC_ARROW );
        RegisterClassW( &cls );
        registered = TRUE;
    }
    GetMonitorInfoW( MonitorFromRect( anchor, MONITOR_DEFAULTTONEAREST ), &mon );
    if (x + p->width > mon.rcWork.right) x = mon.rcWork.right - p->width;
    if (y + p->height > mon.rcWork.bottom) y = anchor->top - p->height - px( 2 );
    p->hot = p->result = -1;
    p->done = FALSE;
    p->hwnd = CreateWindowExW( WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"ArcticPaintPopup", NULL, WS_POPUP | WS_BORDER,
                               x, y, p->width + 2, p->height + 2, owner, NULL, instance, p );
    if (!p->hwnd) return -1;
    ShowWindow( p->hwnd, SW_SHOWNOACTIVATE );
    SetCapture( p->hwnd );
    while (!p->done && GetMessageW( &msg, 0, 0, 0 ) > 0)
    {
        if (msg.message == WM_KEYDOWN || msg.message == WM_CHAR) msg.hwnd = p->hwnd;
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
        if (GetCapture() != p->hwnd && !p->done) p->done = TRUE;
    }
    if (GetCapture() == p->hwnd) ReleaseCapture();
    DestroyWindow( p->hwnd );
    surface_free( &p->surf );
    return p->result;
}

static HMENU build_menu( const struct menu_item *items, int count, HBITMAP *bitmaps, int *used )
{
    HMENU menu = CreatePopupMenu();

    for (int i = 0; i < count; i++)
    {
        MENUITEMINFOW item = { sizeof(item) };
        WCHAR text[160];

        if (!items[i].id && !items[i].sub)
        {
            AppendMenuW( menu, MF_SEPARATOR, 0, NULL );
            continue;
        }
        lstrcpynW( text, items[i].text, 120 );
        if (items[i].keys)
        {
            lstrcatW( text, L"\t" );
            lstrcatW( text, items[i].keys );
        }
        item.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE | MIIM_FTYPE;
        item.wID = items[i].id;
        item.dwTypeData = text;
        item.fType = items[i].flags & MFT_RADIOCHECK;
        item.fState = items[i].flags & (MFS_CHECKED | MFS_DISABLED);
        if (items[i].sub)
        {
            item.fMask |= MIIM_SUBMENU;
            item.hSubMenu = build_menu( items[i].sub, items[i].sub_count, bitmaps, used );
        }
        if (items[i].icon && *used < 128 && (bitmaps[*used] = icon_bitmap( items[i].icon, px( 16 ) )))
        {
            item.fMask |= MIIM_BITMAP;
            item.hbmpItem = bitmaps[(*used)++];
        }
        InsertMenuItemW( menu, i, TRUE, &item );
    }
    return menu;
}

int menu_run( HWND owner, const RECT *anchor, const struct menu_item *items, int count )
{
    HBITMAP bitmaps[128] = { 0 };
    TPMPARAMS params = { sizeof(params) };
    int used = 0, id;
    HMENU menu = build_menu( items, count, bitmaps, &used );

    params.rcExclude = *anchor;
    id = TrackPopupMenuEx( menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_VERTICAL, anchor->left,
                           anchor->bottom, owner, &params );
    DestroyMenu( menu );
    for (int i = 0; i < used; i++) DeleteObject( bitmaps[i] );
    return id;
}
