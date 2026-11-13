/*
 * Arctic WordPad: colours, fonts and drawing
 *
 * Everything the program draws of its own goes into a 32-bit DIB first: the
 * soft shapes are written into its pixels, the text by GDI, and the whole
 * is copied to the window at once.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <math.h>
#include "wordpad.h"

struct theme th;

#define RGBX(c) RGB( ((c) >> 16) & 0xff, ((c) >> 8) & 0xff, (c) & 0xff )

/* light or dark, as "Choose your default app mode" says. The pages stay
 * white: they are the paper the document will be printed on */
void theme_load(void)
{
    DWORD light = 1, size = sizeof(light);

    RegGetValueW( HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                  L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &light, &size );
    th.dark = !light;
    if (th.dark)
    {
        th.chrome = RGBX( 0x1f1f1f );
        th.card = RGBX( 0x2b2b2b );
        th.card_line = RGBX( 0x383838 );
        th.text = RGBX( 0xffffff );
        th.dim = RGBX( 0xc8c8c8 );
        th.faint = RGBX( 0x6a6a6a );
        th.hover = RGBX( 0x3b3b3b );
        th.pressed = RGBX( 0x333333 );
        th.checked = RGBX( 0x454545 );
        th.checked_line = RGBX( 0x5a5a5a );
        th.accent = RGBX( 0x4c9bff );
        th.on_accent = RGBX( 0x000000 );
        th.field = RGBX( 0x2f2f2f );
        th.field_line = RGBX( 0x474747 );
        th.canvas = RGBX( 0x1f1f1f );
        th.page_line = RGBX( 0x000000 );
        th.popup = RGBX( 0x2c2c2c );
        th.popup_line = RGBX( 0x454545 );
    }
    else
    {
        th.chrome = RGBX( 0xf1f3f6 );
        th.card = RGBX( 0xffffff );
        th.card_line = RGBX( 0xe1e4e8 );
        th.text = RGBX( 0x242424 );
        th.dim = RGBX( 0x616161 );
        th.faint = RGBX( 0xbdbdbd );
        th.hover = RGBX( 0xf0f0f0 );
        th.pressed = RGBX( 0xe0e0e0 );
        th.checked = RGBX( 0xe1e1e1 );
        th.checked_line = RGBX( 0xc8c8c8 );
        th.accent = RGBX( 0x185abd );
        th.on_accent = RGBX( 0xffffff );
        th.field = RGBX( 0xffffff );
        th.field_line = RGBX( 0xd1d1d1 );
        th.canvas = RGBX( 0xf1f3f6 );
        th.page_line = RGBX( 0xd6d9dd );
        th.popup = RGBX( 0xffffff );
        th.popup_line = RGBX( 0xd6d6d6 );
    }
    th.sel_back = RGBX( 0xc7d9f2 );
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
                x = rect->right - 2;   /* the inside: on to the right edge */
                continue;
            }
            blend_pixel( p + x, color, a * alpha / 255 );
        }
    }
}

/* the soft shadow round a page */
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

/**********************************************************************
 *          The pictures (tools/wordpad-icons.py)
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
        colors[0] = RGBX( 0xdedede ); colors[1] = RGBX( 0x4cc2ff ); colors[2] = CLR_NONE;
    }
    else
    {
        colors[0] = RGBX( 0x424242 ); colors[1] = RGBX( 0x0078d4 ); colors[2] = RGBX( 0xffffff );
    }
    colors[3] = RGBX( 0xffffff );
    colors[4] = RGBX( 0x2b2b2b );
    colors[5] = RGBX( 0x6e6e6e );
    if (state == IS_DISABLED)
    {
        colors[0] = colors[1] = colors[4] = colors[5] = th.faint;
    }
    else if (state == IS_WHITE)
    {
        colors[0] = colors[1] = colors[3] = colors[4] = colors[5] = RGBX( 0xffffff );
        colors[2] = CLR_NONE;
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

/**********************************************************************
 *          Glyphs: lines in a square of 16, drawn soft
 */

#define END  -1
#define ARCG -2   /* centre x, y, radius, from, to (degrees) */

static const float g_down[] = { 4, 6, 8, 10, 8, 10, 12, 6, END };
static const float g_up[] = { 4, 10, 8, 6, 8, 6, 12, 10, END };
static const float g_left[] = { 10, 4, 6, 8, 6, 8, 10, 12, END };
static const float g_right[] = { 6, 4, 10, 8, 10, 8, 6, 12, END };
static const float g_check[] = { 3, 8.5f, 6.5f, 12, 6.5f, 12, 13, 4.5f, END };
static const float g_launcher[] = { 3, 3, 3, 13, 3, 13, 13, 13, 13, 13, 13, 3, 7, 7, 12, 12, 12, 12, 12, 8.5f, 12, 12, 8.5f, 12, END };
static const float g_min[] = { 3, 8, 13, 8, END };
static const float g_max[] = { 3, 3, 13, 3, 13, 3, 13, 13, 13, 13, 3, 13, 3, 13, 3, 3, END };
static const float g_restore[] = { 3, 5, 11, 5, 11, 5, 11, 13, 11, 13, 3, 13, 3, 13, 3, 5, 5, 3, 13, 3, 13, 3, 13, 11, END };
static const float g_close[] = { 3, 3, 13, 13, 13, 3, 3, 13, END };
static const float g_plus[] = { 8, 2.5f, 8, 13.5f, 2.5f, 8, 13.5f, 8, END };
static const float g_minus[] = { 2.5f, 8, 13.5f, 8, END };
static const float g_search[] = { ARCG, 6.5f, 6.5f, 4.5f, 0, 360, 9.8f, 9.8f, 14, 14, END };
static const float g_more[] = { 3, 8, 3.2f, 8, 8, 8, 8.2f, 8, 13, 8, 13.2f, 8, END };
static const float g_down_small[] = { 5, 6.5f, 8, 9.5f, 8, 9.5f, 11, 6.5f, END };

static const float *const glyph_lines[] =
{
    g_down, g_up, g_left, g_right, g_check, g_launcher, g_min, g_max, g_restore, g_close, g_plus, g_minus,
    g_search, g_more, g_down_small,
};

struct seg { float x0, y0, x1, y1; };

static int glyph_segments( const float *p, struct seg *out, int max_out )
{
    int n = 0;

    while (*p != END && n < max_out)
    {
        if (*p == ARCG)
        {
            float cx = p[1], cy = p[2], r = p[3], a0 = p[4] * 3.14159265f / 180, a1 = p[5] * 3.14159265f / 180;
            int steps = 24;

            for (int i = 0; i < steps && n < max_out; i++, n++)
            {
                float a = a0 + (a1 - a0) * i / steps, b = a0 + (a1 - a0) * (i + 1) / steps;
                out[n].x0 = cx + r * cosf( a ); out[n].y0 = cy + r * sinf( a );
                out[n].x1 = cx + r * cosf( b ); out[n].y1 = cy + r * sinf( b );
            }
            p += 6;
        }
        else
        {
            out[n].x0 = p[0]; out[n].y0 = p[1]; out[n].x1 = p[2]; out[n].y1 = p[3];
            n++;
            p += 4;
        }
    }
    return n;
}

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
    struct seg segs[64];
    int n = glyph_segments( glyph_lines[glyph], segs, ARRAY_SIZE(segs) );
    float scale = 16.0f / size, half = max( 0.5f, 0.55f * scale * size / 16.0f );

    /* a line about a pixel wide at any size: half a unit at 16, less above */
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
    static const struct { int height, weight; BOOL italic, underline, strike; BOOL serif; } spec[F_COUNT] =
    {
        [F_UI] = { 12, FW_NORMAL },
        [F_UI_SEMI] = { 12, FW_SEMIBOLD },
        [F_SMALL] = { 11, FW_NORMAL },
        [F_TAB] = { 13, FW_NORMAL },
        [F_TITLE] = { 12, FW_NORMAL },
        [F_HEAD] = { 28, FW_NORMAL },
        [F_SUBHEAD] = { 15, FW_SEMIBOLD },
        [F_LETTER] = { 15, FW_NORMAL },
        [F_LETTER_BOLD] = { 15, FW_BOLD },
        [F_LETTER_ITALIC] = { 15, FW_NORMAL, TRUE },
        [F_LETTER_UNDER] = { 15, FW_NORMAL, FALSE, TRUE },
        [F_LETTER_STRIKE] = { 14, FW_NORMAL, FALSE, FALSE, TRUE },
        [F_LETTER_SMALL] = { 10, FW_NORMAL },
    };
    LOGFONTW lf = { 0 };

    if (fonts[kind]) return fonts[kind];
    lf.lfHeight = -px( spec[kind].height );
    lf.lfWeight = spec[kind].weight;
    lf.lfItalic = spec[kind].italic;
    lf.lfUnderline = spec[kind].underline;
    lf.lfStrikeOut = spec[kind].strike;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lstrcpynW( lf.lfFaceName, ui_face(), LF_FACESIZE );
    if (kind == F_HEAD) lf.lfWeight = FW_LIGHT;
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
