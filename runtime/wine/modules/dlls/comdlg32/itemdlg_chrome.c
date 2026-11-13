/*
 * The parts of the file dialog around the browser, as Windows 10 and 11
 * have them: the flat buttons (back, forward, up; Organize, New folder; the
 * view), the address bar of folders and the search box.
 *
 * The address bar shows where the dialog is as a row of folders ("This PC
 * > Local Disk (C:) > Users"): a click on one goes there, a click beside them
 * gives the path to type. Its parent gets WM_COMMAND with CHROME_NAVIGATE
 * (the place in chrome_address_target) or CHROME_REFRESH. The buttons send
 * BN_CLICKED, or CHROME_DROPDOWN for their menus; the search box EN_CHANGE.
 *
 * The look follows the system: dark when the windows are, the rounded fields
 * of Windows 11 when the visual style has its size, everything at the DPI of
 * the window. Nothing here comes from Windows: the glyphs are lines, drawn
 * soft, from the lists below.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS

#include <stdarg.h>
#include <stdlib.h>
#include <math.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "winreg.h"
#include "commctrl.h"
#include "shlwapi.h"
#include "shlobj.h"
#include "commdlg.h"
#include "cdlg.h"
#include "itemdlg_chrome.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(commdlg);

#define BUTTONS_CLASS   L"ItemDlgButtons"
#define ADDRESS_CLASS   L"ItemDlgAddress"
#define SEARCH_CLASS    L"ItemDlgSearch"
#define ID_EDIT         1
#define MAX_BUTTONS     8
#define MAX_SEGMENTS    32
#define SUPERSAMPLE     4

#define THEME_KEY       L"Software\\Microsoft\\Windows\\CurrentVersion\\ThemeManager"

/**********************************************************************
 *          The look
 */

int chrome_scale( UINT dpi, int px )
{
    return MulDiv( px, dpi ? dpi : USER_DEFAULT_SCREEN_DPI, USER_DEFAULT_SCREEN_DPI );
}

/* Windows 11's look goes with the size of the visual style that has it,
 * which Personalization sets */
static BOOL style_is_11(void)
{
    WCHAR size[64] = L"";
    DWORD len = sizeof(size);

    if (RegGetValueW( HKEY_CURRENT_USER, THEME_KEY, L"SizeName", RRF_RT_REG_SZ, NULL, size, &len )) return FALSE;
    return !lstrcmpiW( size, L"Windows 11" );
}

void chrome_get_look( struct chrome_look *look, HWND hwnd )
{
    COLORREF window = GetSysColor( COLOR_WINDOW );

    look->dpi = hwnd ? GetDpiForWindow( hwnd ) : 0;
    if (!look->dpi) look->dpi = GetDpiForSystem();
    look->dark = GetRValue( window ) * 3 + GetGValue( window ) * 6 + GetBValue( window ) < 1280;
    look->win11 = style_is_11();
    look->band = GetSysColor( COLOR_3DFACE );
    look->text = GetSysColor( COLOR_WINDOWTEXT );

    if (look->win11 && look->dark)
    {
        look->top = RGB( 0x2c, 0x2c, 0x2c );
        look->field = RGB( 0x38, 0x38, 0x38 );
        look->border = RGB( 0x45, 0x45, 0x45 );
        look->focus = RGB( 0x60, 0xcd, 0xff );
        look->hover = RGB( 0x3a, 0x3a, 0x3a );
        look->down = RGB( 0x33, 0x33, 0x33 );
        look->hover_edge = look->hover;
        look->dim = RGB( 0xa8, 0xa8, 0xa8 );
        look->line = RGB( 0x40, 0x40, 0x40 );
    }
    else if (look->win11)
    {
        look->top = RGB( 0xf9, 0xf9, 0xf9 );
        look->field = RGB( 0xff, 0xff, 0xff );
        look->border = RGB( 0xe5, 0xe5, 0xe5 );
        look->focus = RGB( 0x00, 0x5f, 0xb8 );
        look->hover = RGB( 0xec, 0xec, 0xec );
        look->down = RGB( 0xe2, 0xe2, 0xe2 );
        look->hover_edge = look->hover;
        look->dim = RGB( 0x5d, 0x5d, 0x5d );
        look->line = RGB( 0xe5, 0xe5, 0xe5 );
    }
    else if (look->dark)
    {
        look->top = window;
        look->field = window;
        look->border = RGB( 0x53, 0x53, 0x53 );
        look->focus = RGB( 0x00, 0x78, 0xd7 );
        look->hover = RGB( 0x43, 0x43, 0x43 );
        look->down = RGB( 0x5a, 0x5a, 0x5a );
        look->hover_edge = look->hover;
        look->dim = RGB( 0xa0, 0xa0, 0xa0 );
        look->line = RGB( 0x40, 0x40, 0x40 );
    }
    else
    {
        look->top = window;
        look->field = window;
        look->border = RGB( 0xd9, 0xd9, 0xd9 );
        look->focus = RGB( 0x00, 0x78, 0xd7 );
        look->hover = RGB( 0xe5, 0xf3, 0xff );
        look->down = RGB( 0xcc, 0xe8, 0xff );
        look->hover_edge = RGB( 0xcc, 0xe8, 0xff );
        look->dim = RGB( 0x6d, 0x6d, 0x6d );
        look->line = RGB( 0xdf, 0xdf, 0xdf );
    }
}

/* the font of the messages, at the window's DPI */
HFONT chrome_font( UINT dpi )
{
    NONCLIENTMETRICSW metrics = { sizeof(metrics) };

    if (!dpi) dpi = GetDpiForSystem();
    if (!SystemParametersInfoForDpi( SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi ))
    {
        SystemParametersInfoW( SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0 );
        metrics.lfMessageFont.lfHeight = MulDiv( metrics.lfMessageFont.lfHeight, dpi, GetDpiForSystem() );
    }
    return CreateFontIndirectW( &metrics.lfMessageFont );
}

/**********************************************************************
 *          Drawing: soft glyphs, round fields
 */

/* premultiplied pixels over what is there */
static void blend_bits( HDC hdc, int x, int y, int width, int height, const UINT32 *pixels )
{
    BITMAPINFO info = { { sizeof(info.bmiHeader), width, -height, 1, 32, BI_RGB } };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    HDC mem = CreateCompatibleDC( hdc );
    HBITMAP bitmap;
    UINT32 *bits;

    if ((bitmap = CreateDIBSection( hdc, &info, DIB_RGB_COLORS, (void **)&bits, NULL, 0 )))
    {
        HGDIOBJ old = SelectObject( mem, bitmap );

        memcpy( bits, pixels, width * height * sizeof(*bits) );
        GdiAlphaBlend( hdc, x, y, width, height, mem, 0, 0, width, height, blend );
        SelectObject( mem, old );
        DeleteObject( bitmap );
    }
    DeleteDC( mem );
}

static UINT32 premultiply( COLORREF color, UINT32 a )
{
    return (a << 24) | ((GetRValue( color ) * a / 255) << 16) | ((GetGValue( color ) * a / 255) << 8) |
           (GetBValue( color ) * a / 255);
}

#define END     -1
#define ARC     -2      /* centre x, y, radius, from degrees, to degrees */
#define RBOX    -3      /* left, top, right, bottom, radius of the corners */
#define DOT     -4      /* centre x, y, radius */

/* lines in a square of 20 */
static const float g_back[] = { 17,10,3.5f,10, 3.5f,10,8.5f,5, 3.5f,10,8.5f,15, END };
static const float g_forward[] = { 3,10,16.5f,10, 16.5f,10,11.5f,5, 16.5f,10,11.5f,15, END };
static const float g_up[] = { 10,17,10,3.5f, 10,3.5f,5,8.5f, 10,3.5f,15,8.5f, END };
static const float g_refresh[] = { ARC,10,10.5f,6.5f,20,320, 15,6.3f,15,2.2f, 15,6.3f,10.9f,6.3f, END };
static const float g_search[] = { ARC,11.5f,8.5f,5,0,360, 7.9f,12.1f,3,17, END };
static const float g_right[] = { 8,5,13,10, 13,10,8,15, END };
static const float g_down[] = { 5,8,10,13, 10,13,15,8, END };
static const float g_more[] = { DOT,4,10,1.4f, DOT,10,10,1.4f, DOT,16,10,1.4f, END };
static const float g_details[] = { 3,5,17,5, 3,10,17,10, 3,15,17,15, END };
static const float g_list[] = { DOT,3.5f,5,1.2f, DOT,3.5f,10,1.2f, DOT,3.5f,15,1.2f, 7,5,17,5, 7,10,17,10, 7,15,17,15, END };
static const float g_icons[] = { RBOX,3,3,8.5f,8.5f,1.5f, RBOX,11.5f,3,17,8.5f,1.5f, RBOX,3,11.5f,8.5f,17,1.5f, RBOX,11.5f,11.5f,17,17,1.5f, END };
static const float g_tiles[] = { RBOX,3,3,17,8.5f,1.5f, RBOX,3,11.5f,17,17,1.5f, END };

static const float *const glyphs[GLYPH_COUNT] =
{
    g_back, g_forward, g_up, g_refresh, g_search, g_right, g_down, g_more, g_details, g_list, g_icons, g_tiles,
};

struct segment { float x0, y0, x1, y1; };

#define MAX_LINES 120

static void add_arc( struct segment *out, int *n, float cx, float cy, float r, float from_deg, float to_deg )
{
    float from = from_deg * 3.14159265f / 180, to = to_deg * 3.14159265f / 180;
    int steps = max( 3, (int)(fabsf( to_deg - from_deg ) / 15) ), i;

    for (i = 0; i < steps && *n < MAX_LINES; i++, (*n)++)
    {
        float a = from + (to - from) * i / steps, b = from + (to - from) * (i + 1) / steps;

        out[*n].x0 = cx + r * cosf( a );
        out[*n].y0 = cy + r * sinf( a );
        out[*n].x1 = cx + r * cosf( b );
        out[*n].y1 = cy + r * sinf( b );
    }
}

static int glyph_lines( const float *p, struct segment *out, float *dots )
{
    int n = 0, d = 0, i;

    while (*p != END)
    {
        if (*p == ARC)
        {
            add_arc( out, &n, p[1], p[2], p[3], p[4], p[5] );
            p += 6;
        }
        else if (*p == RBOX)
        {
            float l = p[1], t = p[2], r = p[3], b = p[4], q = p[5];
            const struct segment sides[4] = { { l + q, t, r - q, t }, { r, t + q, r, b - q }, { r - q, b, l + q, b },
                                              { l, b - q, l, t + q } };

            for (i = 0; i < 4 && n < MAX_LINES; i++) out[n++] = sides[i];
            add_arc( out, &n, r - q, t + q, q, 270, 360 );
            add_arc( out, &n, r - q, b - q, q, 0, 90 );
            add_arc( out, &n, l + q, b - q, q, 90, 180 );
            add_arc( out, &n, l + q, t + q, q, 180, 270 );
            p += 6;
        }
        else if (*p == DOT)
        {
            if (d < 10)
            {
                dots[d * 3] = p[1];
                dots[d * 3 + 1] = p[2];
                dots[d * 3 + 2] = p[3];
                d++;
            }
            p += 4;
        }
        else
        {
            if (n < MAX_LINES)
            {
                out[n].x0 = p[0];
                out[n].y0 = p[1];
                out[n].x1 = p[2];
                out[n].y1 = p[3];
                n++;
            }
            p += 4;
        }
    }
    return n | (d << 16);
}

static BOOL near_line( const struct segment *s, float x, float y, float width )
{
    float vx = s->x1 - s->x0, vy = s->y1 - s->y0, wx = x - s->x0, wy = y - s->y0, t, dx, dy;

    t = (wx * vx + wy * vy) / max( vx * vx + vy * vy, 1e-6f );
    t = max( 0.0f, min( 1.0f, t ) );
    dx = wx - t * vx;
    dy = wy - t * vy;
    return dx * dx + dy * dy <= width * width;
}

static UINT32 *make_glyph( int glyph, int size, COLORREF color )
{
    struct segment lines[MAX_LINES];
    float dots[30], scale = 20.0f / size;
    /* the stroke is a pixel wide whatever the size, a little more when large */
    float stroke = max( 0.5f * scale, 0.55f );
    int counts = glyph_lines( glyphs[glyph], lines, dots ), n = counts & 0xffff, d = counts >> 16;
    UINT32 *pixels = calloc( size * size, sizeof(*pixels) );
    int x, y, i, j, k;

    if (!pixels) return NULL;
    for (y = 0; y < size; y++)
    {
        for (x = 0; x < size; x++)
        {
            int hits = 0;

            for (j = 0; j < SUPERSAMPLE; j++)
            {
                for (i = 0; i < SUPERSAMPLE; i++)
                {
                    float u = (x + (i + 0.5f) / SUPERSAMPLE) * scale, v = (y + (j + 0.5f) / SUPERSAMPLE) * scale;
                    BOOL in = FALSE;

                    for (k = 0; k < n && !in; k++) in = near_line( &lines[k], u, v, stroke );
                    for (k = 0; k < d && !in; k++)
                    {
                        float dx = u - dots[k * 3], dy = v - dots[k * 3 + 1];
                        in = dx * dx + dy * dy <= dots[k * 3 + 2] * dots[k * 3 + 2];
                    }
                    if (in) hits++;
                }
            }
            if (hits) pixels[y * size + x] = premultiply( color, hits * 255 / (SUPERSAMPLE * SUPERSAMPLE) );
        }
    }
    return pixels;
}

struct cached_glyph
{
    int glyph, size;
    COLORREF color;
    UINT32 *pixels;
};

static struct cached_glyph glyph_cache[64];
static int glyph_cache_count;
static SRWLOCK glyph_lock = SRWLOCK_INIT;

void chrome_draw_glyph( HDC hdc, int glyph, int x, int y, int size, COLORREF color )
{
    UINT32 *pixels = NULL;
    BOOL cached = FALSE;
    int i;

    if (glyph < 0 || glyph >= GLYPH_COUNT || size <= 0) return;
    AcquireSRWLockExclusive( &glyph_lock );
    for (i = 0; i < glyph_cache_count && !pixels; i++)
        if (glyph_cache[i].glyph == glyph && glyph_cache[i].size == size && glyph_cache[i].color == color)
            pixels = glyph_cache[i].pixels;
    if (pixels) cached = TRUE;
    else if ((pixels = make_glyph( glyph, size, color )) && glyph_cache_count < ARRAY_SIZE(glyph_cache))
    {
        glyph_cache[glyph_cache_count].glyph = glyph;
        glyph_cache[glyph_cache_count].size = size;
        glyph_cache[glyph_cache_count].color = color;
        glyph_cache[glyph_cache_count++].pixels = pixels;
        cached = TRUE;
    }
    if (pixels) blend_bits( hdc, x, y, size, size, pixels );
    ReleaseSRWLockExclusive( &glyph_lock );
    if (!cached) free( pixels );
}

/* how much of the pixel at x, y a rectangle of width w, height h with
 * corners of radius r covers, in 0..255 */
static UINT32 round_cover( int x, int y, int w, int h, float r )
{
    int hits = 0, i, j;

    /* the inside and the edges away from the corners are whole */
    if ((x >= r && x < w - r) || (y >= r && y < h - r)) return 255;
    for (j = 0; j < SUPERSAMPLE; j++)
    {
        for (i = 0; i < SUPERSAMPLE; i++)
        {
            float u = x + (i + 0.5f) / SUPERSAMPLE, v = y + (j + 0.5f) / SUPERSAMPLE;
            float dx = max( max( r - u, u - (w - r) ), 0.f ), dy = max( max( r - v, v - (h - r) ), 0.f );
            if (dx * dx + dy * dy <= r * r) hits++;
        }
    }
    return hits * 255 / (SUPERSAMPLE * SUPERSAMPLE);
}

static void fill_round_alpha( HDC hdc, const RECT *rect, float radius, COLORREF color )
{
    int width = rect->right - rect->left, height = rect->bottom - rect->top, x, y;
    UINT32 *pixels;

    if (width <= 0 || height <= 0 || !(pixels = malloc( width * height * sizeof(*pixels) ))) return;
    for (y = 0; y < height; y++)
        for (x = 0; x < width; x++)
            pixels[y * width + x] = premultiply( color, round_cover( x, y, width, height, radius ) );
    blend_bits( hdc, rect->left, rect->top, width, height, pixels );
    free( pixels );
}

/* a field: square with an edge, or round as Windows 11 has them */
void chrome_fill_round( HDC hdc, const RECT *rect, int radius, COLORREF fill, COLORREF edge )
{
    RECT inner = *rect;

    if (radius <= 0)
    {
        HBRUSH brush = CreateSolidBrush( fill );

        FillRect( hdc, rect, brush );
        DeleteObject( brush );
        if (edge != CLR_NONE && edge != fill)
        {
            brush = CreateSolidBrush( edge );
            FrameRect( hdc, rect, brush );
            DeleteObject( brush );
        }
        return;
    }
    if (edge != CLR_NONE && edge != fill)
    {
        fill_round_alpha( hdc, rect, (float)radius, edge );
        InflateRect( &inner, -1, -1 );
        radius = max( radius - 1, 1 );
    }
    fill_round_alpha( hdc, &inner, (float)radius, fill );
}

static int text_width( HDC hdc, HFONT font, const WCHAR *text )
{
    HGDIOBJ old = SelectObject( hdc, font );
    SIZE size = { 0 };

    GetTextExtentPoint32W( hdc, text, lstrlenW( text ), &size );
    SelectObject( hdc, old );
    return size.cx;
}

static void draw_text( HDC hdc, const WCHAR *text, RECT *rect, HFONT font, COLORREF color, UINT format )
{
    HGDIOBJ old = SelectObject( hdc, font );

    SetBkMode( hdc, TRANSPARENT );
    SetTextColor( hdc, color );
    DrawTextW( hdc, text, -1, rect, format | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS );
    SelectObject( hdc, old );
}

/* paints through a bitmap: no flicker while the mouse moves */
struct buffer
{
    HDC hdc, target;
    HBITMAP bitmap;
    HGDIOBJ old;
    RECT rect;
};

static HDC buffer_begin( struct buffer *b, HDC target, const RECT *rect )
{
    b->target = target;
    b->rect = *rect;
    b->hdc = CreateCompatibleDC( target );
    b->bitmap = CreateCompatibleBitmap( target, max( 1, rect->right ), max( 1, rect->bottom ) );
    b->old = SelectObject( b->hdc, b->bitmap );
    return b->hdc;
}

static void buffer_end( struct buffer *b )
{
    BitBlt( b->target, 0, 0, b->rect.right, b->rect.bottom, b->hdc, 0, 0, SRCCOPY );
    SelectObject( b->hdc, b->old );
    DeleteObject( b->bitmap );
    DeleteDC( b->hdc );
}

static void fill( HDC hdc, const RECT *rect, COLORREF color )
{
    HBRUSH brush = CreateSolidBrush( color );

    FillRect( hdc, rect, brush );
    DeleteObject( brush );
}

static void notify_parent( HWND hwnd, UINT id, UINT code )
{
    SendMessageW( GetParent( hwnd ), WM_COMMAND, MAKEWPARAM( id, code ), (LPARAM)hwnd );
}

/**********************************************************************
 *          The buttons
 */

struct button
{
    UINT id, flags;
    int glyph;
    WCHAR text[64], tip[128];
    BOOL enabled, visible;
    RECT rect, arrow;   /* the whole button; the arrow of a split one */
};

struct buttons
{
    HWND hwnd, tooltip;
    HFONT font;
    struct button items[MAX_BUTTONS];
    int count;
    int hot, hot_arrow, down, down_arrow, focus;
    BOOL tracking, menu;
};

static void buttons_layout( struct buttons *b, int height )
{
    struct chrome_look look;
    HDC hdc = GetDC( b->hwnd );
    int x = 0, i;

    chrome_get_look( &look, b->hwnd );
    for (i = 0; i < b->count; i++)
    {
        struct button *item = &b->items[i];
        int w, pad = chrome_scale( look.dpi, look.win11 ? 12 : 9 );

        SetRectEmpty( &item->rect );
        SetRectEmpty( &item->arrow );
        if (!item->visible) continue;
        if (item->text[0])
        {
            w = 2 * pad + text_width( hdc, b->font, item->text );
            if (item->glyph != GLYPH_NONE) w += chrome_scale( look.dpi, 16 + 6 );
            if (item->flags & CHROME_BUTTON_MENU) w += chrome_scale( look.dpi, 14 );
        }
        else
            w = max( height, chrome_scale( look.dpi, look.win11 ? 36 : 28 ) );
        SetRect( &item->rect, x, 0, x + w, height );
        if (item->flags & CHROME_BUTTON_SPLIT)
        {
            SetRect( &item->arrow, x + w, 0, x + w + chrome_scale( look.dpi, 16 ), height );
            item->rect.right = item->arrow.right;
            w += item->arrow.right - item->arrow.left;
        }
        x += w + chrome_scale( look.dpi, 2 );
        if (b->tooltip && item->tip[0])
        {
            TTTOOLINFOW info = { sizeof(info), 0, b->hwnd, i };
            info.rect = item->rect;
            SendMessageW( b->tooltip, TTM_NEWTOOLRECTW, 0, (LPARAM)&info );
        }
    }
    ReleaseDC( b->hwnd, hdc );
}

static void buttons_paint( struct buttons *b, HDC target )
{
    struct chrome_look look;
    struct buffer buffer;
    RECT client;
    HDC hdc;
    int i;

    GetClientRect( b->hwnd, &client );
    chrome_get_look( &look, b->hwnd );
    hdc = buffer_begin( &buffer, target, &client );
    fill( hdc, &client, look.top );
    for (i = 0; i < b->count; i++)
    {
        struct button *item = &b->items[i];
        int glyph = chrome_scale( look.dpi, 16 ), radius = look.win11 ? chrome_scale( look.dpi, 4 ) : 0;
        COLORREF ink = item->enabled ? look.text : look.dim;
        BOOL hot = i == b->hot && item->enabled, down = i == b->down && item->enabled;
        RECT r = item->rect, body = item->rect;

        if (IsRectEmpty( &item->rect )) continue;
        if (!IsRectEmpty( &item->arrow )) body.right = item->arrow.left;
        if (look.win11) InflateRect( &r, 0, -chrome_scale( look.dpi, 2 ) );
        if (down || (hot && b->menu))
            chrome_fill_round( hdc, &r, radius, look.down, look.win11 ? CLR_NONE : look.hover_edge );
        else if (hot)
            chrome_fill_round( hdc, &r, radius, look.hover, look.win11 ? CLR_NONE : look.hover_edge );
        if (hot && !IsRectEmpty( &item->arrow ))
        {
            RECT line = { item->arrow.left, r.top + chrome_scale( look.dpi, 4 ), item->arrow.left + 1,
                          r.bottom - chrome_scale( look.dpi, 4 ) };
            fill( hdc, &line, look.dark ? look.line : look.hover_edge );
        }

        if (item->text[0])
        {
            RECT t = body;
            int pad = chrome_scale( look.dpi, look.win11 ? 12 : 9 );

            t.left += pad;
            if (item->glyph != GLYPH_NONE)
            {
                chrome_draw_glyph( hdc, item->glyph, t.left, (r.top + r.bottom - glyph) / 2, glyph, ink );
                t.left += glyph + chrome_scale( look.dpi, 6 );
            }
            t.right -= pad;
            if (item->flags & CHROME_BUTTON_MENU)
            {
                int arrow = chrome_scale( look.dpi, 10 );
                t.right -= chrome_scale( look.dpi, 14 );
                chrome_draw_glyph( hdc, GLYPH_DOWN, t.right + chrome_scale( look.dpi, 4 ),
                                   (r.top + r.bottom - arrow) / 2 + 1, arrow, ink );
            }
            draw_text( hdc, item->text, &t, b->font, ink, DT_LEFT );
        }
        else if (item->glyph != GLYPH_NONE)
            chrome_draw_glyph( hdc, item->glyph, (body.left + body.right - glyph) / 2, (r.top + r.bottom - glyph) / 2,
                               glyph, ink );
        if (!IsRectEmpty( &item->arrow ))
        {
            int arrow = chrome_scale( look.dpi, 10 );
            chrome_draw_glyph( hdc, GLYPH_DOWN, (item->arrow.left + item->arrow.right - arrow) / 2,
                               (r.top + r.bottom - arrow) / 2 + 1, arrow, ink );
        }
        if (i == b->focus && GetFocus() == b->hwnd)
        {
            RECT f = r;
            InflateRect( &f, -1, -1 );
            SetTextColor( hdc, look.text );
            SetBkColor( hdc, look.top );
            DrawFocusRect( hdc, &f );
        }
    }
    buffer_end( &buffer );
}

static int buttons_hit( struct buttons *b, POINT pt, BOOL *arrow )
{
    int i;

    for (i = 0; i < b->count; i++)
    {
        if (!PtInRect( &b->items[i].rect, pt )) continue;
        *arrow = PtInRect( &b->items[i].arrow, pt ) || (b->items[i].flags & CHROME_BUTTON_MENU);
        return i;
    }
    *arrow = FALSE;
    return -1;
}

static void buttons_menu( struct buttons *b, int index )
{
    b->menu = TRUE;
    b->hot = index;
    InvalidateRect( b->hwnd, NULL, FALSE );
    UpdateWindow( b->hwnd );
    notify_parent( b->hwnd, b->items[index].id, CHROME_DROPDOWN );
    b->menu = FALSE;
    b->hot = -1;
    InvalidateRect( b->hwnd, NULL, FALSE );
}

static void buttons_step_focus( struct buttons *b, int step )
{
    int i, next = b->focus;

    for (i = 0; i < b->count; i++)
    {
        next = (next + step + b->count) % b->count;
        if (b->items[next].visible && b->items[next].enabled) break;
    }
    b->focus = next;
    InvalidateRect( b->hwnd, NULL, FALSE );
}

static LRESULT CALLBACK buttons_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct buttons *b = (struct buttons *)GetWindowLongPtrW( hwnd, 0 );

    switch (msg)
    {
    case WM_NCCREATE:
        if (!(b = calloc( 1, sizeof(*b) ))) return FALSE;
        b->hwnd = hwnd;
        b->hot = b->down = -1;
        SetWindowLongPtrW( hwnd, 0, (LONG_PTR)b );
        break;
    case WM_CREATE:
        b->tooltip = CreateWindowExW( WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                      0, 0, 0, 0, hwnd, NULL, COMDLG32_hInstance, NULL );
        return 0;
    case WM_NCDESTROY:
        free( b );
        SetWindowLongPtrW( hwnd, 0, 0 );
        break;
    case WM_SETFONT:
        b->font = (HFONT)wparam;
        if (lparam) InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    case WM_GETFONT:
        return (LRESULT)b->font;
    case WM_SIZE:
        buttons_layout( b, HIWORD( lparam ) );
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint( hwnd, &ps );
        buttons_paint( b, hdc );
        EndPaint( hwnd, &ps );
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        POINT pt = { (short)LOWORD( lparam ), (short)HIWORD( lparam ) };
        BOOL arrow;
        int hot = buttons_hit( b, pt, &arrow );

        if (hot != b->hot || arrow != b->hot_arrow)
        {
            b->hot = hot;
            b->hot_arrow = arrow;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        if (!b->tracking)
        {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd };
            b->tracking = TrackMouseEvent( &tme );
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        b->tracking = FALSE;
        if (b->hot != -1 && b->down == -1)
        {
            b->hot = -1;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    {
        POINT pt = { (short)LOWORD( lparam ), (short)HIWORD( lparam ) };
        BOOL arrow;
        int hit = buttons_hit( b, pt, &arrow );

        if (hit < 0 || !b->items[hit].enabled) return 0;
        b->focus = hit;
        if (arrow)
        {
            buttons_menu( b, hit );
            return 0;
        }
        b->down = hit;
        SetCapture( hwnd );
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    }
    case WM_LBUTTONUP:
    {
        POINT pt = { (short)LOWORD( lparam ), (short)HIWORD( lparam ) };
        BOOL arrow;
        int hit = buttons_hit( b, pt, &arrow ), down = b->down;

        if (down == -1) return 0;
        b->down = -1;
        ReleaseCapture();
        InvalidateRect( hwnd, NULL, FALSE );
        if (hit == down && !arrow && b->items[hit].enabled) notify_parent( hwnd, b->items[hit].id, BN_CLICKED );
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (b->down != -1)
        {
            b->down = -1;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_SETFOCUS:
        if (b->focus < 0 || b->focus >= b->count || !b->items[b->focus].visible || !b->items[b->focus].enabled)
        {
            b->focus = -1;
            buttons_step_focus( b, 1 );
        }
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    case WM_KILLFOCUS:
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    case WM_KEYDOWN:
        if (b->focus < 0 || b->focus >= b->count) return 0;
        switch (wparam)
        {
        case VK_LEFT: buttons_step_focus( b, -1 ); break;
        case VK_RIGHT: buttons_step_focus( b, 1 ); break;
        case VK_DOWN:
            if (b->items[b->focus].flags & (CHROME_BUTTON_MENU | CHROME_BUTTON_SPLIT)) buttons_menu( b, b->focus );
            break;
        case VK_SPACE:
        case VK_RETURN:
            if (!b->items[b->focus].enabled) break;
            if (b->items[b->focus].flags & CHROME_BUTTON_MENU) buttons_menu( b, b->focus );
            else notify_parent( hwnd, b->items[b->focus].id, BN_CLICKED );
            break;
        }
        return 0;
    case WM_THEMECHANGED:
    case WM_SYSCOLORCHANGE:
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

static struct buttons *get_buttons( HWND hwnd )
{
    WCHAR cls[32];

    if (!hwnd || !GetClassNameW( hwnd, cls, ARRAY_SIZE(cls) ) || lstrcmpiW( cls, BUTTONS_CLASS )) return NULL;
    return (struct buttons *)GetWindowLongPtrW( hwnd, 0 );
}

static struct button *find_button( struct buttons *b, UINT id )
{
    int i;

    if (!b) return NULL;
    for (i = 0; i < b->count; i++) if (b->items[i].id == id) return &b->items[i];
    return NULL;
}

HWND chrome_create_buttons( HWND parent, UINT id )
{
    return CreateWindowExW( 0, BUTTONS_CLASS, NULL, WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS,
                            0, 0, 0, 0, parent, (HMENU)(UINT_PTR)id, COMDLG32_hInstance, NULL );
}

void chrome_buttons_add( HWND hwnd, UINT id, int glyph, const WCHAR *text, const WCHAR *tip, UINT flags )
{
    struct buttons *b = get_buttons( hwnd );
    struct button *item;

    if (!b || b->count >= MAX_BUTTONS) return;
    item = &b->items[b->count];
    memset( item, 0, sizeof(*item) );
    item->id = id;
    item->glyph = glyph;
    item->flags = flags;
    item->enabled = item->visible = TRUE;
    if (text) lstrcpynW( item->text, text, ARRAY_SIZE(item->text) );
    if (tip) lstrcpynW( item->tip, tip, ARRAY_SIZE(item->tip) );
    if (b->tooltip && item->tip[0])
    {
        TTTOOLINFOW info = { sizeof(info), TTF_SUBCLASS, hwnd, b->count };
        info.lpszText = item->tip;
        SendMessageW( b->tooltip, TTM_ADDTOOLW, 0, (LPARAM)&info );
    }
    b->count++;
}

void chrome_buttons_enable( HWND hwnd, UINT id, BOOL enable )
{
    struct button *item = find_button( get_buttons( hwnd ), id );

    if (!item || item->enabled == !!enable) return;
    item->enabled = !!enable;
    InvalidateRect( hwnd, NULL, FALSE );
}

void chrome_buttons_show( HWND hwnd, UINT id, BOOL show )
{
    struct buttons *b = get_buttons( hwnd );
    struct button *item = find_button( b, id );
    RECT client;

    if (!item || item->visible == !!show) return;
    item->visible = !!show;
    GetClientRect( hwnd, &client );
    buttons_layout( b, client.bottom );
    InvalidateRect( hwnd, NULL, FALSE );
}

void chrome_buttons_set_glyph( HWND hwnd, UINT id, int glyph )
{
    struct button *item = find_button( get_buttons( hwnd ), id );

    if (!item || item->glyph == glyph) return;
    item->glyph = glyph;
    InvalidateRect( hwnd, NULL, FALSE );
}

int chrome_buttons_width( HWND hwnd, int height )
{
    struct buttons *b = get_buttons( hwnd );
    int i, right = 0;

    if (!b) return 0;
    buttons_layout( b, height );
    for (i = 0; i < b->count; i++) right = max( right, (int)b->items[i].rect.right );
    return right;
}

BOOL chrome_buttons_rect( HWND hwnd, UINT id, RECT *rect )
{
    struct button *item = find_button( get_buttons( hwnd ), id );

    if (!item || IsRectEmpty( &item->rect )) return FALSE;
    *rect = item->rect;
    MapWindowPoints( hwnd, NULL, (POINT *)rect, 2 );
    return TRUE;
}

/**********************************************************************
 *          The address bar
 */

struct crumb
{
    LPITEMIDLIST pidl;
    WCHAR name[MAX_PATH];
    RECT rect;          /* where it is drawn, empty if it does not fit */
};

struct address
{
    HWND hwnd, edit;
    HFONT font;
    HICON icon;
    LPITEMIDLIST pidl, target;
    struct crumb crumbs[MAX_SEGMENTS];
    UINT count;
    int hot;            /* the crumb under the mouse, -1 none, -2 the refresh button */
    BOOL tracking, more, ending;
    RECT refresh, room;
    HBRUSH field_brush;
    COLORREF field_color;
};

static void address_free( struct address *a )
{
    UINT i;

    for (i = 0; i < a->count; i++) ILFree( a->crumbs[i].pidl );
    a->count = 0;
}

static void address_layout( struct address *a )
{
    struct chrome_look look;
    HDC hdc;
    RECT client;
    int chevron, pad, x, right, width = 0, first, i;

    chrome_get_look( &look, a->hwnd );
    chevron = chrome_scale( look.dpi, 16 );
    pad = chrome_scale( look.dpi, 5 );
    GetClientRect( a->hwnd, &client );
    a->refresh = client;
    a->refresh.left = client.right - chrome_scale( look.dpi, look.win11 ? 32 : 26 );
    a->room = client;
    a->room.left = chrome_scale( look.dpi, 8 + 16 + 4 );
    a->room.right = a->refresh.left - chrome_scale( look.dpi, 2 );

    for (i = 0; i < (int)a->count; i++) SetRectEmpty( &a->crumbs[i].rect );
    if (!a->count) return;
    hdc = GetDC( a->hwnd );
    /* from the last folder back, as many as fit; the first ones go */
    right = a->room.right;
    x = a->room.left;
    first = a->count;
    for (i = a->count - 1; i >= 0; i--)
    {
        int w = text_width( hdc, a->font, a->crumbs[i].name ) + 2 * pad + chevron;

        if (width + w + chevron > right - x && i < (int)a->count - 1) break;
        width += w;
        first = i;
    }
    a->more = first > 0;
    if (a->more) x += chevron;
    for (i = first; i < (int)a->count; i++)
    {
        int w = text_width( hdc, a->font, a->crumbs[i].name ) + 2 * pad;

        x += chevron;
        SetRect( &a->crumbs[i].rect, x, client.top + chrome_scale( look.dpi, look.win11 ? 3 : 2 ), min( right, x + w ),
                 client.bottom - chrome_scale( look.dpi, look.win11 ? 3 : 2 ) );
        x = a->crumbs[i].rect.right;
    }
    ReleaseDC( a->hwnd, hdc );
}

static void address_paint( struct address *a, HDC target )
{
    struct chrome_look look;
    struct buffer buffer;
    RECT client, field;
    HDC hdc;
    int radius, chevron, size, cy;
    BOOL focus = a->edit && IsWindowVisible( a->edit );
    UINT i;

    GetClientRect( a->hwnd, &client );
    chrome_get_look( &look, a->hwnd );
    radius = look.win11 ? chrome_scale( look.dpi, 4 ) : 0;
    chevron = chrome_scale( look.dpi, 16 );
    size = chrome_scale( look.dpi, 16 );
    cy = (client.top + client.bottom) / 2;
    hdc = buffer_begin( &buffer, target, &client );
    fill( hdc, &client, look.top );
    field = client;
    chrome_fill_round( hdc, &field, radius, look.field, focus ? look.focus : look.border );
    if (look.win11 && focus)
    {
        RECT line = { field.left + radius, field.bottom - chrome_scale( look.dpi, 2 ), field.right - radius, field.bottom };
        fill( hdc, &line, look.focus );
    }
    if (focus)
    {
        buffer_end( &buffer );
        return;
    }

    /* the folder's icon */
    if (a->icon)
        DrawIconEx( hdc, chrome_scale( look.dpi, 8 ), cy - size / 2, a->icon, size, size, 0, NULL, DI_NORMAL );

    if (a->more)
    {
        int more = chrome_scale( look.dpi, 14 );
        chrome_draw_glyph( hdc, GLYPH_MORE, a->room.left + (chevron - more) / 2, cy - more / 2, more, look.dim );
    }
    for (i = 0; i < a->count; i++)
    {
        struct crumb *c = &a->crumbs[i];
        RECT text = c->rect;
        int mark = chrome_scale( look.dpi, 12 );

        if (IsRectEmpty( &c->rect )) continue;
        if ((int)i == a->hot)
            chrome_fill_round( hdc, &c->rect, look.win11 ? chrome_scale( look.dpi, 3 ) : 0, look.hover,
                               look.win11 ? CLR_NONE : look.hover_edge );
        chrome_draw_glyph( hdc, GLYPH_RIGHT, c->rect.left - chevron + (chevron - mark) / 2, cy - mark / 2, mark,
                           look.dim );
        draw_text( hdc, c->name, &text, a->font, look.text, DT_CENTER );
    }

    if (a->hot == -2)
    {
        RECT hot = a->refresh;
        InflateRect( &hot, -chrome_scale( look.dpi, 2 ), -chrome_scale( look.dpi, 2 ) );
        chrome_fill_round( hdc, &hot, look.win11 ? chrome_scale( look.dpi, 3 ) : 0, look.hover,
                           look.win11 ? CLR_NONE : look.hover_edge );
    }
    if (!look.win11)
    {
        RECT line = { a->refresh.left, client.top + 1, a->refresh.left + 1, client.bottom - 1 };
        fill( hdc, &line, look.border );
    }
    chrome_draw_glyph( hdc, GLYPH_REFRESH, (a->refresh.left + a->refresh.right - size) / 2, cy - size / 2, size,
                       look.text );
    buffer_end( &buffer );
}

static int address_hit( struct address *a, POINT pt )
{
    UINT i;

    if (PtInRect( &a->refresh, pt )) return -2;
    for (i = 0; i < a->count; i++) if (PtInRect( &a->crumbs[i].rect, pt )) return i;
    return -1;
}

static void address_go( struct address *a, LPITEMIDLIST pidl )
{
    ILFree( a->target );
    a->target = pidl;
    notify_parent( a->hwnd, GetDlgCtrlID( a->hwnd ), CHROME_NAVIGATE );
}

static void address_end_edit( struct address *a, BOOL go )
{
    WCHAR text[MAX_PATH * 2], expanded[MAX_PATH * 2];
    LPITEMIDLIST pidl = NULL;
    BOOL had_focus;

    if (!a->edit || !IsWindowVisible( a->edit )) return;
    GetWindowTextW( a->edit, text, ARRAY_SIZE(text) );
    /* the focus goes on, not back to the bar, which would type again */
    had_focus = GetFocus() == a->edit;
    a->ending = TRUE;
    ShowWindow( a->edit, SW_HIDE );
    if (had_focus || GetFocus() == a->hwnd)
    {
        HWND next = GetNextDlgTabItem( GetParent( a->hwnd ), a->hwnd, FALSE );
        if (next && next != a->hwnd) SetFocus( next );
    }
    a->ending = FALSE;
    InvalidateRect( a->hwnd, NULL, FALSE );
    if (!go || !text[0]) return;
    ExpandEnvironmentStringsW( text, expanded, ARRAY_SIZE(expanded) );
    if (SUCCEEDED(SHParseDisplayName( expanded, NULL, &pidl, 0, NULL )) && pidl) address_go( a, pidl );
    else MessageBeep( MB_ICONWARNING );
}

static LRESULT CALLBACK address_edit_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR data )
{
    struct address *a = (struct address *)data;

    switch (msg)
    {
    case WM_GETDLGCODE:
        if (lparam && ((MSG *)lparam)->message == WM_KEYDOWN &&
            (((MSG *)lparam)->wParam == VK_RETURN || ((MSG *)lparam)->wParam == VK_ESCAPE))
            return DLGC_WANTALLKEYS | DefSubclassProc( hwnd, msg, wparam, lparam );
        break;
    case WM_KEYDOWN:
        if (wparam == VK_RETURN) { address_end_edit( a, TRUE ); return 0; }
        if (wparam == VK_ESCAPE) { address_end_edit( a, FALSE ); return 0; }
        break;
    case WM_CHAR:
        if (wparam == '\r' || wparam == 27) return 0;
        break;
    case WM_KILLFOCUS:
        address_end_edit( a, FALSE );
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass( hwnd, address_edit_proc, id );
        break;
    }
    return DefSubclassProc( hwnd, msg, wparam, lparam );
}

/* the path to type goes where the folders were */
static void address_place_edit( struct address *a )
{
    struct chrome_look look;
    TEXTMETRICW tm;
    RECT client, r;
    HGDIOBJ old;
    HDC hdc;
    int h;

    chrome_get_look( &look, a->hwnd );
    GetClientRect( a->hwnd, &client );
    hdc = GetDC( a->hwnd );
    old = SelectObject( hdc, a->font ? a->font : GetStockObject( DEFAULT_GUI_FONT ) );
    GetTextMetricsW( hdc, &tm );
    SelectObject( hdc, old );
    ReleaseDC( a->hwnd, hdc );
    h = tm.tmHeight + 2;
    r.left = chrome_scale( look.dpi, 8 );
    r.right = a->refresh.left - chrome_scale( look.dpi, 4 );
    r.top = (client.bottom - h) / 2;
    r.bottom = r.top + h;
    if (!a->edit)
    {
        a->edit = CreateWindowExW( 0, WC_EDITW, NULL, WS_CHILD | ES_AUTOHSCROLL, r.left, r.top, r.right - r.left,
                                   r.bottom - r.top, a->hwnd, (HMENU)ID_EDIT, COMDLG32_hInstance, NULL );
        SendMessageW( a->edit, WM_SETFONT, (WPARAM)a->font, FALSE );
        SetWindowSubclass( a->edit, address_edit_proc, 0, (DWORD_PTR)a );
        SHAutoComplete( a->edit, SHACF_FILESYS_DIRS );
    }
    else SetWindowPos( a->edit, NULL, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE );
}

static void address_begin_edit( struct address *a )
{
    WCHAR *path = NULL;

    address_place_edit( a );
    if (a->pidl && SUCCEEDED(SHGetNameFromIDList( a->pidl, SIGDN_DESKTOPABSOLUTEEDITING, &path )))
    {
        SetWindowTextW( a->edit, path );
        CoTaskMemFree( path );
    }
    ShowWindow( a->edit, SW_SHOW );
    SetFocus( a->edit );
    SendMessageW( a->edit, EM_SETSEL, 0, -1 );
    InvalidateRect( a->hwnd, NULL, FALSE );
}

static LRESULT CALLBACK address_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct address *a = (struct address *)GetWindowLongPtrW( hwnd, 0 );

    switch (msg)
    {
    case WM_NCCREATE:
        if (!(a = calloc( 1, sizeof(*a) ))) return FALSE;
        a->hwnd = hwnd;
        a->hot = -1;
        SetWindowLongPtrW( hwnd, 0, (LONG_PTR)a );
        break;
    case WM_NCDESTROY:
        if (a)
        {
            address_free( a );
            ILFree( a->pidl );
            ILFree( a->target );
            if (a->icon) DestroyIcon( a->icon );
            if (a->field_brush) DeleteObject( a->field_brush );
            free( a );
        }
        SetWindowLongPtrW( hwnd, 0, 0 );
        break;
    case WM_SETFONT:
        a->font = (HFONT)wparam;
        if (a->edit) SendMessageW( a->edit, WM_SETFONT, wparam, lparam );
        address_layout( a );
        if (lparam) InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    case WM_GETFONT:
        return (LRESULT)a->font;
    case WM_SIZE:
        address_layout( a );
        if (a->edit && IsWindowVisible( a->edit )) address_place_edit( a );
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint( hwnd, &ps );
        address_paint( a, hdc );
        EndPaint( hwnd, &ps );
        return 0;
    }
    case WM_CTLCOLOREDIT:
    {
        struct chrome_look look;

        chrome_get_look( &look, hwnd );
        if (!a->field_brush || a->field_color != look.field)
        {
            if (a->field_brush) DeleteObject( a->field_brush );
            a->field_brush = CreateSolidBrush( look.field );
            a->field_color = look.field;
        }
        SetBkColor( (HDC)wparam, look.field );
        SetTextColor( (HDC)wparam, look.text );
        return (LRESULT)a->field_brush;
    }
    case WM_MOUSEMOVE:
    {
        POINT pt = { (short)LOWORD( lparam ), (short)HIWORD( lparam ) };
        int hot = address_hit( a, pt );

        if (hot != a->hot)
        {
            a->hot = hot;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        if (!a->tracking)
        {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd };
            a->tracking = TrackMouseEvent( &tme );
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        a->tracking = FALSE;
        if (a->hot != -1)
        {
            a->hot = -1;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        return 0;
    case WM_LBUTTONUP:
    {
        POINT pt = { (short)LOWORD( lparam ), (short)HIWORD( lparam ) };
        int hit = address_hit( a, pt );

        if (hit == -2) notify_parent( hwnd, GetDlgCtrlID( hwnd ), CHROME_REFRESH );
        else if (hit >= 0 && hit < (int)a->count - 1) address_go( a, ILClone( a->crumbs[hit].pidl ) );
        else address_begin_edit( a );
        return 0;
    }
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_KEYDOWN:
        if (wparam == VK_F4 || wparam == VK_RETURN || wparam == VK_SPACE) address_begin_edit( a );
        return 0;
    case WM_SETFOCUS:
        /* tabbing in gives the path to type, as Windows does */
        if (!a->ending) address_begin_edit( a );
        return 0;
    case WM_THEMECHANGED:
    case WM_SYSCOLORCHANGE:
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

static struct address *get_address( HWND hwnd )
{
    WCHAR cls[32];

    if (!hwnd || !GetClassNameW( hwnd, cls, ARRAY_SIZE(cls) ) || lstrcmpiW( cls, ADDRESS_CLASS )) return NULL;
    return (struct address *)GetWindowLongPtrW( hwnd, 0 );
}

HWND chrome_create_address( HWND parent, UINT id )
{
    return CreateWindowExW( 0, ADDRESS_CLASS, NULL, WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                            0, 0, 0, 0, parent, (HMENU)(UINT_PTR)id, COMDLG32_hInstance, NULL );
}

/* the folders of a place, from the desktop on, the desktop left out */
void chrome_address_set( HWND hwnd, PCIDLIST_ABSOLUTE pidl )
{
    struct address *a = get_address( hwnd );
    LPCITEMIDLIST cur;
    SHFILEINFOW info;
    UINT n = 0, i, k;

    if (!a) return;
    address_free( a );
    ILFree( a->pidl );
    a->pidl = ILClone( pidl );
    for (cur = pidl; cur && cur->mkid.cb; cur = ILGetNext( cur )) n++;
    /* the desktop itself is its one folder */
    if (!n)
    {
        WCHAR *name = NULL;

        a->crumbs[0].pidl = ILClone( pidl );
        a->crumbs[0].name[0] = 0;
        if (SUCCEEDED(SHGetNameFromIDList( pidl, SIGDN_NORMALDISPLAY, &name )))
        {
            lstrcpynW( a->crumbs[0].name, name, MAX_PATH );
            CoTaskMemFree( name );
        }
        a->count = 1;
    }
    for (i = 1; i <= n && a->count < MAX_SEGMENTS; i++)
    {
        LPITEMIDLIST part = ILClone( pidl ), p = part;
        WCHAR *name = NULL;

        for (k = 0; k < i && p; k++) p = ILGetNext( p );
        if (p) p->mkid.cb = 0;
        a->crumbs[a->count].pidl = part;
        a->crumbs[a->count].name[0] = 0;
        if (SUCCEEDED(SHGetNameFromIDList( part, SIGDN_NORMALDISPLAY, &name )))
        {
            lstrcpynW( a->crumbs[a->count].name, name, MAX_PATH );
            CoTaskMemFree( name );
        }
        a->count++;
    }
    if (a->icon) DestroyIcon( a->icon );
    a->icon = NULL;
    if (SHGetFileInfoW( (const WCHAR *)pidl, 0, &info, sizeof(info), SHGFI_PIDL | SHGFI_ICON | SHGFI_SMALLICON ))
        a->icon = info.hIcon;
    if (a->edit) ShowWindow( a->edit, SW_HIDE );
    address_layout( a );
    InvalidateRect( hwnd, NULL, FALSE );
}

LPITEMIDLIST chrome_address_target( HWND hwnd )
{
    struct address *a = get_address( hwnd );
    return a && a->target ? ILClone( a->target ) : NULL;
}

void chrome_address_edit( HWND hwnd )
{
    struct address *a = get_address( hwnd );
    if (a) address_begin_edit( a );
}

/**********************************************************************
 *          The search box
 */

struct search
{
    HWND hwnd, edit;
    HFONT font;
    HBRUSH field_brush;
    COLORREF field_color;
};

static void search_place_edit( struct search *s )
{
    struct chrome_look look;
    TEXTMETRICW tm;
    RECT client;
    HDC hdc;
    HGDIOBJ old;
    int h;

    chrome_get_look( &look, s->hwnd );
    GetClientRect( s->hwnd, &client );
    hdc = GetDC( s->hwnd );
    old = SelectObject( hdc, s->font ? s->font : GetStockObject( DEFAULT_GUI_FONT ) );
    GetTextMetricsW( hdc, &tm );
    SelectObject( hdc, old );
    ReleaseDC( s->hwnd, hdc );
    h = tm.tmHeight + 2;
    SetWindowPos( s->edit, NULL, chrome_scale( look.dpi, 8 ), (client.bottom - h) / 2,
                  max( 0, client.right - chrome_scale( look.dpi, 8 + 30 ) ), h, SWP_NOZORDER | SWP_NOACTIVATE );
}

static LRESULT CALLBACK search_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct search *s = (struct search *)GetWindowLongPtrW( hwnd, 0 );

    switch (msg)
    {
    case WM_NCCREATE:
        if (!(s = calloc( 1, sizeof(*s) ))) return FALSE;
        s->hwnd = hwnd;
        SetWindowLongPtrW( hwnd, 0, (LONG_PTR)s );
        break;
    case WM_CREATE:
        s->edit = CreateWindowExW( 0, WC_EDITW, NULL, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                   0, 0, 0, 0, hwnd, (HMENU)ID_EDIT, COMDLG32_hInstance, NULL );
        return s->edit ? 0 : -1;
    case WM_NCDESTROY:
        if (s)
        {
            if (s->field_brush) DeleteObject( s->field_brush );
            free( s );
        }
        SetWindowLongPtrW( hwnd, 0, 0 );
        break;
    case WM_SETFONT:
        s->font = (HFONT)wparam;
        SendMessageW( s->edit, WM_SETFONT, wparam, lparam );
        search_place_edit( s );
        return 0;
    case WM_GETFONT:
        return (LRESULT)s->font;
    case WM_SIZE:
        search_place_edit( s );
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        struct chrome_look look;
        struct buffer buffer;
        PAINTSTRUCT ps;
        RECT client;
        HDC hdc = BeginPaint( hwnd, &ps ), mem;
        BOOL focus = GetFocus() == s->edit;
        int size, radius;

        chrome_get_look( &look, hwnd );
        size = chrome_scale( look.dpi, 16 );
        radius = look.win11 ? chrome_scale( look.dpi, 4 ) : 0;
        GetClientRect( hwnd, &client );
        mem = buffer_begin( &buffer, hdc, &client );
        fill( mem, &client, look.top );
        chrome_fill_round( mem, &client, radius, look.field, focus ? look.focus : look.border );
        if (look.win11 && focus)
        {
            RECT line = { client.left + radius, client.bottom - chrome_scale( look.dpi, 2 ), client.right - radius,
                          client.bottom };
            fill( mem, &line, look.focus );
        }
        chrome_draw_glyph( mem, GLYPH_SEARCH, client.right - size - chrome_scale( look.dpi, 8 ),
                           (client.bottom - size) / 2, size, look.dim );
        buffer_end( &buffer );
        EndPaint( hwnd, &ps );
        return 0;
    }
    case WM_CTLCOLOREDIT:
    {
        struct chrome_look look;

        chrome_get_look( &look, hwnd );
        if (!s->field_brush || s->field_color != look.field)
        {
            if (s->field_brush) DeleteObject( s->field_brush );
            s->field_brush = CreateSolidBrush( look.field );
            s->field_color = look.field;
        }
        SetBkColor( (HDC)wparam, look.field );
        SetTextColor( (HDC)wparam, look.text );
        return (LRESULT)s->field_brush;
    }
    case WM_COMMAND:
        if (LOWORD( wparam ) == ID_EDIT)
        {
            if (HIWORD( wparam ) == EN_SETFOCUS || HIWORD( wparam ) == EN_KILLFOCUS) InvalidateRect( hwnd, NULL, FALSE );
            notify_parent( hwnd, GetDlgCtrlID( hwnd ), HIWORD( wparam ) );
        }
        return 0;
    case WM_SETFOCUS:
        SetFocus( s->edit );
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus( s->edit );
        return 0;
    case WM_THEMECHANGED:
    case WM_SYSCOLORCHANGE:
        InvalidateRect( hwnd, NULL, TRUE );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

HWND chrome_create_search( HWND parent, UINT id )
{
    return CreateWindowExW( WS_EX_CONTROLPARENT, SEARCH_CLASS, NULL, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                            0, 0, 0, 0, parent, (HMENU)(UINT_PTR)id, COMDLG32_hInstance, NULL );
}

void chrome_search_set_cue( HWND hwnd, const WCHAR *folder )
{
    WCHAR text[MAX_PATH + 32], format[64];

    if (!LoadStringW( COMDLG32_hInstance, IDS_ITEMDLG_SEARCH_IN, format, ARRAY_SIZE(format) ))
        lstrcpyW( format, L"Search %s" );
    swprintf( text, ARRAY_SIZE(text), format, folder ? folder : L"" );
    SendDlgItemMessageW( hwnd, ID_EDIT, EM_SETCUEBANNER, FALSE, (LPARAM)text );
}

void chrome_search_get( HWND hwnd, WCHAR *text, int count )
{
    GetDlgItemTextW( hwnd, ID_EDIT, text, count );
}

void chrome_search_clear( HWND hwnd )
{
    SetDlgItemTextW( hwnd, ID_EDIT, L"" );
}

void chrome_search_focus( HWND hwnd )
{
    HWND edit = GetDlgItem( hwnd, ID_EDIT );

    if (!edit) return;
    SetFocus( edit );
    SendMessageW( edit, EM_SETSEL, 0, -1 );
}

/**********************************************************************
 *          The classes
 */

void chrome_register(void)
{
    WNDCLASSW wc;

    if (!GetClassInfoW( COMDLG32_hInstance, BUTTONS_CLASS, &wc ))
    {
        memset( &wc, 0, sizeof(wc) );
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = buttons_proc;
        wc.cbWndExtra = sizeof(LONG_PTR);
        wc.hInstance = COMDLG32_hInstance;
        wc.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_ARROW );
        wc.lpszClassName = BUTTONS_CLASS;
        RegisterClassW( &wc );
    }
    if (!GetClassInfoW( COMDLG32_hInstance, ADDRESS_CLASS, &wc ))
    {
        memset( &wc, 0, sizeof(wc) );
        wc.lpfnWndProc = address_proc;
        wc.cbWndExtra = sizeof(LONG_PTR);
        wc.hInstance = COMDLG32_hInstance;
        wc.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_ARROW );
        wc.lpszClassName = ADDRESS_CLASS;
        RegisterClassW( &wc );
    }
    if (!GetClassInfoW( COMDLG32_hInstance, SEARCH_CLASS, &wc ))
    {
        memset( &wc, 0, sizeof(wc) );
        wc.lpfnWndProc = search_proc;
        wc.cbWndExtra = sizeof(LONG_PTR);
        wc.hInstance = COMDLG32_hInstance;
        wc.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_IBEAM );
        wc.lpszClassName = SEARCH_CLASS;
        RegisterClassW( &wc );
    }
}
