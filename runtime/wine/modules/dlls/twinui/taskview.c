/*
 * Task View, as in Windows 11
 *
 * The Task View button on the taskbar and Win+Tab open it over the monitor
 * the pointer is on, all of it but the taskbar: the shell's acrylic, and on
 * it every window of that monitor as a live picture (DWM thumbnails), in
 * rows, each under its icon and title. The pictures fly from where the
 * windows are into their places as the backdrop fades in, and back when it
 * closes; the one clicked lands where its window is and comes to the front.
 * A picture under the pointer is lit, its X closes the window (so do the
 * middle button and Delete), and the others move up into the gap. Arrows,
 * Tab, Enter and Esc work as in Windows.
 *
 * It runs in the thread of the task switcher (switcher.c).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <limits.h>
#include <math.h>
#include <stdlib.h>

#include "private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(twinui);

#define MAX_CARDS       64
#define WM_TV_TOGGLE    (WM_APP + 1)
#define WM_TV_HIDE      (WM_APP + 2)
#define TIMER_FRAME     1
#define TIMER_WATCH     2

/* the layout of Windows 11, at 96 dpi */
#define MARGIN_SIDE     px(48)
#define MARGIN_TOP      px(56)
#define MARGIN_BOTTOM   px(56)
#define GAP             px(24)
#define PAD             px(8)       /* the plate around a card */
#define TITLE_H         px(32)
#define ICON            px(16)
#define CLOSE           px(24)
#define RADIUS          px(8)

/* how long things take, in ms */
#define OPEN_MS         320
#define CLOSE_MS        260
#define HOT_MS          130
#define LEAVE_MS        170
#define MOVE_MS         260

enum phase { HIDDEN, OPENING, SHOWN, CLOSING };

struct tween
{
    RECT from, to;
    float op_from, op_to;
    double start, length;
};

struct card
{
    HWND hwnd;
    WCHAR title[256];
    HICON icon, big_icon;
    RECT real;                  /* the window, client coordinates (its normal place when minimized) */
    BOOL minimized;
    RECT plate, title_rect, slot, close;
    HTHUMBNAIL thumbnail;
    struct tween move;
    RECT shown;                 /* where the picture is now */
    BYTE shown_op;
    float hot, hot_from, hot_to;
    double hot_start;
    BOOL leaving;               /* its window is being closed */
    double leave_start;
    HBITMAP label;              /* the icon and the title, premultiplied */
    int label_w;
};

static HWND view, previous, target;
static struct card cards[MAX_CARDS];
static int count, hot = -1, focus;
static BOOL hot_close, focus_shown;
static enum phase phase;
static double phase_start;
static double left_at;          /* when another window took the foreground from the view */
static float label_alpha;       /* the titles fade in after the pictures */
static RECT origin;             /* the work area the view covers */
static HFONT font;
static HDC back_dc;             /* what is painted, kept between paints */
static HBITMAP back_bitmap;
static DWORD *back_bits;
static int back_w, back_h;

static double now_ms(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER counter;

    if (!freq.QuadPart) QueryPerformanceFrequency( &freq );
    QueryPerformanceCounter( &counter );
    return counter.QuadPart * 1000.0 / freq.QuadPart;
}

/* The moment the frame being prepared will show: the next vertical blank,
 * as dwm.exe tells it (DwmGetCompositionTimingInfo). Positions are worked
 * out for it, not for when the timer fired: each frame on the screen is
 * where the curve says it is then, and the motion is even. */
static double frame_ms(void)
{
    DWM_TIMING_INFO info = { sizeof(info) };
    LARGE_INTEGER counter;
    static LARGE_INTEGER freq;
    LONGLONG next;

    if (!freq.QuadPart) QueryPerformanceFrequency( &freq );
    if (FAILED(DwmGetCompositionTimingInfo( NULL, &info )) || info.qpcRefreshPeriod <= 0) return now_ms();
    QueryPerformanceCounter( &counter );
    next = info.qpcVBlank + info.qpcRefreshPeriod;
    if (next <= counter.QuadPart)
        next += ((counter.QuadPart - next) / info.qpcRefreshPeriod + 1) * info.qpcRefreshPeriod;
    return next * 1000.0 / freq.QuadPart;
}

static float clamp01( float t )
{
    return t < 0 ? 0 : t > 1 ? 1 : t;
}

/* quick to start, slow into place: Windows 11's "decelerate" */
static float ease_out( float t )
{
    t = 1 - clamp01( t );
    return 1 - t * t * t * t;
}

/* slow to start, quick at the end, for what goes away */
static float ease_in_out( float t )
{
    t = clamp01( t );
    return t < 0.5f ? 4 * t * t * t : 1 - powf( -2 * t + 2, 3 ) / 2;
}

static LONG lerp( LONG a, LONG b, float t )
{
    return a + (LONG)lroundf( (b - a) * t );
}

static RECT lerp_rect( const RECT *a, const RECT *b, float t )
{
    RECT r = { lerp( a->left, b->left, t ), lerp( a->top, b->top, t ), lerp( a->right, b->right, t ),
               lerp( a->bottom, b->bottom, t ) };
    return r;
}

/* a rectangle made smaller about its middle */
static RECT scaled_rect( const RECT *r, float scale )
{
    int w = r->right - r->left, h = r->bottom - r->top;
    int nw = (int)(w * scale), nh = (int)(h * scale);
    RECT s = { r->left + (w - nw) / 2, r->top + (h - nh) / 2, 0, 0 };

    s.right = s.left + nw;
    s.bottom = s.top + nh;
    return s;
}

static void tween_start( struct card *card, const RECT *to, float op_to, double length, BOOL ease_in )
{
    card->move.from = card->shown;
    card->move.op_from = card->shown_op;
    card->move.to = *to;
    card->move.op_to = op_to;
    card->move.start = now_ms();
    card->move.length = ease_in ? -length : length;   /* the sign says which easing */
}

/**********************************************************************
 *          Painting helpers: premultiplied pixels, which dwm keeps over the backdrop
 */

static DWORD premultiply( COLORREF color, BYTE alpha )
{
    return ((DWORD)alpha << 24) | ((GetRValue( color ) * alpha / 255) << 16) |
           ((GetGValue( color ) * alpha / 255) << 8) | (GetBValue( color ) * alpha / 255);
}

static DWORD over( DWORD src, DWORD dst )
{
    DWORD a = src >> 24, inv = 255 - a;

    if (!a) return dst;
    if (a == 255) return src;
    return ((a + ((dst >> 24) * inv) / 255) << 24) |
           ((((src >> 16) & 0xff) + (((dst >> 16) & 0xff) * inv) / 255) << 16) |
           ((((src >> 8) & 0xff) + (((dst >> 8) & 0xff) * inv) / 255) << 8) |
           ((src & 0xff) + ((dst & 0xff) * inv) / 255);
}

/* how much of a pixel a rounded rectangle covers, its corners smoothed by their distance */
static float round_cover( const RECT *rect, float radius, float fx, float fy )
{
    float dx = 0, dy = 0, d;

    if (fx <= rect->left || fx >= rect->right || fy <= rect->top || fy >= rect->bottom)
    {
        /* the half pixel outside the edge */
        if (fx < rect->left - 0.5f || fx > rect->right + 0.5f || fy < rect->top - 0.5f || fy > rect->bottom + 0.5f)
            return 0;
    }
    if (fx < rect->left + radius) dx = rect->left + radius - fx;
    else if (fx > rect->right - radius) dx = fx - (rect->right - radius);
    if (fy < rect->top + radius) dy = rect->top + radius - fy;
    else if (fy > rect->bottom - radius) dy = fy - (rect->bottom - radius);
    d = (dx || dy) ? sqrtf( dx * dx + dy * dy ) : 0;
    return clamp01( radius - d + 0.5f );
}

/* a rounded rectangle into the back buffer, or only its edge two pixels wide */
static void fill_round( const RECT *rect, const RECT *clip, float radius, COLORREF color, BYTE alpha, BOOL ring )
{
    RECT r, inner = *rect;
    int w = px( 2 );

    if (!alpha || !IntersectRect( &r, rect, clip )) return;
    InflateRect( &inner, -w, -w );
    r.left = max( r.left, 0 );
    r.top = max( r.top, 0 );
    r.right = min( r.right, back_w );
    r.bottom = min( r.bottom, back_h );
    for (int y = r.top; y < r.bottom; y++)
    {
        DWORD *row = back_bits + (size_t)y * back_w;

        for (int x = r.left; x < r.right; x++)
        {
            float fx = x + 0.5f, fy = y + 0.5f, cover = round_cover( rect, radius, fx, fy );

            if (ring && cover > 0) cover = clamp01( cover - round_cover( &inner, max( radius - w, 0.f ), fx, fy ) );
            if (cover <= 0) continue;
            row[x] = over( premultiply( color, (BYTE)(alpha * cover) ), row[x] );
        }
    }
}

/* the backdrop: black lets the acrylic through (white over a light look) */
static void fill_back( const RECT *rect )
{
    RECT r;
    DWORD back = shell_light ? 0x00ffffff : 0;

    if (!IntersectRect( &r, rect, &(RECT){ 0, 0, back_w, back_h } )) return;
    for (int y = r.top; y < r.bottom; y++)
    {
        DWORD *row = back_bits + (size_t)y * back_w;
        for (int x = r.left; x < r.right; x++) row[x] = back;
    }
}

/* a bitmap drawn on black and on white: the difference is its coverage */
static void draw_icon_alpha( DWORD *bits, int stride, int x0, int y0, HICON icon, int size )
{
    BITMAPINFO info = { { sizeof(info.bmiHeader), size, -size, 1, 32, BI_RGB } };
    DWORD *black, *white;
    HBITMAP b1, b2;
    HDC dc = CreateCompatibleDC( NULL );
    HGDIOBJ old;

    b1 = CreateDIBSection( dc, &info, DIB_RGB_COLORS, (void **)&black, NULL, 0 );
    b2 = CreateDIBSection( dc, &info, DIB_RGB_COLORS, (void **)&white, NULL, 0 );
    if (b1 && b2)
    {
        old = SelectObject( dc, b1 );
        memset( black, 0, size * size * 4 );
        DrawIconEx( dc, 0, 0, icon, size, size, 0, NULL, DI_NORMAL );
        SelectObject( dc, b2 );
        memset( white, 0xff, size * size * 4 );
        DrawIconEx( dc, 0, 0, icon, size, size, 0, NULL, DI_NORMAL );
        GdiFlush();
        SelectObject( dc, old );
        for (int y = 0; y < size; y++)
        {
            for (int x = 0; x < size; x++)
            {
                DWORD b = black[y * size + x], w = white[y * size + x];
                int a = 255 - (int)(((w >> 8) & 0xff) - ((b >> 8) & 0xff));

                a = max( 0, min( 255, a ) );
                bits[(y0 + y) * stride + x0 + x] = ((DWORD)a << 24) | (b & 0xffffff);
            }
        }
    }
    if (b1) DeleteObject( b1 );
    if (b2) DeleteObject( b2 );
    DeleteDC( dc );
}

/* the icon and the title of a card, ready to be blended at any opacity */
static void make_label( struct card *card )
{
    int width = card->title_rect.right - card->title_rect.left - CLOSE - px( 4 ), height = TITLE_H;
    BITMAPINFO info = { { sizeof(info.bmiHeader), 0, 0, 1, 32, BI_RGB } };
    DWORD *bits;
    HDC dc;
    HGDIOBJ old_bitmap, old_font;
    RECT text;
    COLORREF ink = INK;

    if (card->label) DeleteObject( card->label );
    card->label = NULL;
    if (width <= ICON) return;
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    dc = CreateCompatibleDC( NULL );
    if (!(card->label = CreateDIBSection( dc, &info, DIB_RGB_COLORS, (void **)&bits, NULL, 0 )))
    {
        DeleteDC( dc );
        return;
    }
    card->label_w = width;
    memset( bits, 0, width * height * 4 );
    old_bitmap = SelectObject( dc, card->label );
    old_font = SelectObject( dc, font );
    SetBkMode( dc, TRANSPARENT );
    SetTextColor( dc, RGB( 255, 255, 255 ) );
    SetRect( &text, ICON + px( 10 ), 0, width, height );
    DrawTextW( dc, card->title, -1, &text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX );
    GdiFlush();
    /* white on black: its brightness is how much of the ink covers it */
    for (int i = 0; i < width * height; i++)
    {
        DWORD p = bits[i];
        BYTE a = (BYTE)max( max( (p >> 16) & 0xff, (p >> 8) & 0xff ), p & 0xff );

        bits[i] = premultiply( ink, a );
    }
    SelectObject( dc, old_font );
    SelectObject( dc, old_bitmap );
    DeleteDC( dc );
    draw_icon_alpha( bits, width, 0, (height - ICON) / 2, card->icon, ICON );
}

/* src premultiplied, at an opacity of its own */
static void blend_bitmap( HBITMAP bitmap, int width, int height, int x0, int y0, const RECT *clip, BYTE alpha )
{
    DIBSECTION dib;
    const DWORD *src;

    if (!bitmap || !alpha || GetObjectW( bitmap, sizeof(dib), &dib ) != sizeof(dib)) return;
    src = dib.dsBm.bmBits;
    for (int y = max( y0, (int)clip->top ); y < min( y0 + height, (int)clip->bottom ); y++)
    {
        DWORD *row = back_bits + (size_t)y * back_w;

        if (y < 0 || y >= back_h) continue;
        for (int x = max( x0, (int)clip->left ); x < min( x0 + width, (int)clip->right ); x++)
        {
            DWORD p = src[(y - y0) * width + (x - x0)];

            if (x < 0 || x >= back_w || !(p >> 24)) continue;
            if (alpha != 255)
                p = ((((p >> 24) * alpha / 255)) << 24) | (((((p >> 16) & 0xff) * alpha / 255)) << 16) |
                    (((((p >> 8) & 0xff) * alpha / 255)) << 8) | ((p & 0xff) * alpha / 255);
            row[x] = over( p, row[x] );
        }
    }
}

static void draw_close( const RECT *rect, BOOL lit )
{
    if (lit) fill_round( rect, rect, (float)px( 4 ), RGB( 0xc4, 0x2b, 0x1c ), 255, FALSE );
    else fill_round( rect, rect, (float)px( 4 ), INK, 0x18, FALSE );
}

/**********************************************************************
 *          Painting
 */

static void ensure_back( int width, int height )
{
    BITMAPINFO info = { { sizeof(info.bmiHeader), width, -height, 1, 32, BI_RGB } };

    if (back_bitmap && back_w == width && back_h == height) return;
    if (back_bitmap) DeleteObject( back_bitmap );
    if (!back_dc) back_dc = CreateCompatibleDC( NULL );
    back_bitmap = CreateDIBSection( back_dc, &info, DIB_RGB_COLORS, (void **)&back_bits, NULL, 0 );
    back_w = back_bitmap ? width : 0;
    back_h = back_bitmap ? height : 0;
    if (back_bitmap) SelectObject( back_dc, back_bitmap );
}

static void paint_card( struct card *card, int index, const RECT *clip )
{
    BYTE fade = (BYTE)(label_alpha * 255);
    BOOL leaving = card->leaving;
    float leave = leaving ? 1 - clamp01( (float)((now_ms() - card->leave_start) / LEAVE_MS) ) : 1;
    BOOL with_close = (index == hot || (focus_shown && index == focus)) && !leaving;

    fade = (BYTE)(fade * leave);
    if (card->hot > 0)
        fill_round( &card->plate, clip, (float)RADIUS, INK, (BYTE)(card->hot * (shell_light ? 0x10 : 0x1c) * leave), FALSE );
    if (focus_shown && index == focus && !leaving)
        fill_round( &card->plate, clip, (float)RADIUS, INK, (BYTE)(0xe0 * label_alpha), TRUE );

    blend_bitmap( card->label, card->label_w, TITLE_H, card->title_rect.left, card->title_rect.top, clip, fade );

    /* under the picture, for a window dwm has none of (minimized before it was seen) */
    if (card->minimized && card->big_icon && phase == SHOWN && !leaving)
    {
        int size = px( 48 );
        DWORD *bits = back_bits;
        int x = (card->slot.left + card->slot.right - size) / 2, y = (card->slot.top + card->slot.bottom - size) / 2;

        if (x >= clip->left && y >= clip->top && x + size <= clip->right && y + size <= clip->bottom &&
            x >= 0 && y >= 0 && x + size <= back_w && y + size <= back_h)
        {
            DWORD *tmp = malloc( size * size * 4 );

            if (tmp)
            {
                draw_icon_alpha( tmp, size, 0, 0, card->big_icon, size );
                for (int j = 0; j < size; j++)
                    for (int i = 0; i < size; i++)
                        bits[(size_t)(y + j) * back_w + x + i] = over( tmp[j * size + i], bits[(size_t)(y + j) * back_w + x + i] );
                free( tmp );
            }
        }
    }

    /* the X itself goes on with GDI, after the pixels */
    if (with_close && fade) draw_close( &card->close, index == hot && hot_close );
}

static void on_paint(void)
{
    PAINTSTRUCT ps;
    RECT client, clip;
    HDC hdc = BeginPaint( view, &ps );

    GetClientRect( view, &client );
    ensure_back( client.right, client.bottom );
    if (!back_bitmap)
    {
        EndPaint( view, &ps );
        return;
    }
    clip = ps.rcPaint;
    GdiFlush();
    fill_back( &clip );
    for (int i = 0; i < count; i++)
    {
        RECT r;

        if (IntersectRect( &r, &cards[i].plate, &clip )) paint_card( &cards[i], i, &clip );
    }
    /* the X of the card that shows one: GDI, white on the red */
    for (int i = 0; i < count; i++)
    {
        RECT r;
        BOOL with_close = (i == hot || (focus_shown && i == focus)) && !cards[i].leaving;

        if (!with_close || label_alpha < 0.5f || !IntersectRect( &r, &cards[i].close, &clip )) continue;
        draw_glyph_close( back_dc, &cards[i].close, i == hot && hot_close ? RGB( 255, 255, 255 ) : INK );
    }
    GdiFlush();
    BitBlt( hdc, clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top, back_dc, clip.left, clip.top,
            SRCCOPY );
    EndPaint( view, &ps );
}

/**********************************************************************
 *          Windows and where they go
 */

static BOOL CALLBACK add_window( HWND hwnd, LPARAM lparam )
{
    HMONITOR monitor = (HMONITOR)lparam;
    WINDOWPLACEMENT placement = { sizeof(placement) };
    struct card *card;
    RECT rect;

    if (count == MAX_CARDS) return FALSE;
    if (hwnd == view || !is_task_window( hwnd )) return TRUE;
    if (MonitorFromWindow( hwnd, MONITOR_DEFAULTTONEAREST ) != monitor) return TRUE;
    card = &cards[count++];
    memset( card, 0, sizeof(*card) );
    card->hwnd = hwnd;
    InternalGetWindowText( hwnd, card->title, ARRAY_SIZE(card->title) );
    card->icon = window_icon( hwnd, FALSE );
    card->big_icon = window_icon( hwnd, TRUE );
    card->minimized = IsIconic( hwnd );
    if (card->minimized && GetWindowPlacement( hwnd, &placement ))
    {
        /* workspace coordinates, from the work area's corner */
        MONITORINFO info = { sizeof(info) };

        GetMonitorInfoW( monitor, &info );
        rect = placement.rcNormalPosition;
        OffsetRect( &rect, info.rcWork.left, info.rcWork.top );
    }
    else GetWindowRect( hwnd, &rect );
    OffsetRect( &rect, -origin.left, -origin.top );
    card->real = rect;
    if (card->real.right <= card->real.left) card->real.right = card->real.left + 1;
    if (card->real.bottom <= card->real.top) card->real.bottom = card->real.top + 1;
    return TRUE;
}

static int width_of( const struct card *card )
{
    return card->real.right - card->real.left;
}

static int height_of( const struct card *card )
{
    return card->real.bottom - card->real.top;
}

/* As Windows 11: rows of pictures the same height, each as wide as its
 * window's shape, the rows in the middle; as many rows as make them largest */
static void layout(void)
{
    int area_w = (origin.right - origin.left) - 2 * MARGIN_SIDE;
    int area_h = (origin.bottom - origin.top) - MARGIN_TOP - MARGIN_BOTTOM;
    int max_thumb = min( px( 400 ), area_h * 6 / 10 );
    int best_rows = 1, best_h = 0, live = 0, idx[MAX_CARDS];
    double best_score = -1;

    for (int i = 0; i < count; i++) if (!cards[i].leaving) idx[live++] = i;
    if (!live) return;

    for (int rows = 1; rows <= live; rows++)
    {
        int cols = (live + rows - 1) / rows;
        int cell_w = (area_w - (cols - 1) * GAP) / cols - 2 * PAD;
        int thumb_h = (area_h - (rows - 1) * GAP) / rows - TITLE_H - 2 * PAD;
        double score = 0;
        int widest_row = 0;

        if (cell_w < px( 60 ) || thumb_h < px( 40 )) continue;
        thumb_h = min( thumb_h, max_thumb );
        /* the rows as they would be, narrowed if they do not fit */
        for (int r = 0; r < rows; r++)
        {
            int row_w = 0;
            for (int k = r * cols; k < min( live, (r + 1) * cols ); k++)
            {
                const struct card *c = &cards[idx[k]];
                int w = min( thumb_h * width_of( c ) / height_of( c ), cell_w );
                int h = w * height_of( c ) / width_of( c );

                row_w += w + 2 * PAD + (k > r * cols ? GAP : 0);
                score += (double)w * h;
            }
            widest_row = max( widest_row, row_w );
        }
        if (widest_row > area_w) score *= (double)area_w / widest_row;
        if (score > best_score)
        {
            best_score = score;
            best_rows = rows;
            best_h = thumb_h;
        }
    }
    if (!best_h) best_h = px( 60 );

    {
        int rows = best_rows, cols = (live + rows - 1) / rows;
        int cell_w = max( px( 40 ), (area_w - (cols - 1) * GAP) / cols - 2 * PAD );
        int total_h = rows * (best_h + TITLE_H + 2 * PAD) + (rows - 1) * GAP;
        int y = MARGIN_TOP + max( 0, (area_h - total_h) / 2 );

        for (int r = 0; r < rows; r++)
        {
            int first = r * cols, last = min( live, (r + 1) * cols ), row_w = 0, x;

            for (int k = first; k < last; k++)
            {
                const struct card *c = &cards[idx[k]];
                row_w += min( best_h * width_of( c ) / height_of( c ), cell_w ) + 2 * PAD + (k > first ? GAP : 0);
            }
            x = MARGIN_SIDE + (area_w - row_w) / 2;
            for (int k = first; k < last; k++)
            {
                struct card *c = &cards[idx[k]];
                int w = max( px( 40 ), min( best_h * width_of( c ) / height_of( c ), cell_w ) );
                int h = min( best_h, max( 1, w * height_of( c ) / width_of( c ) ) );
                int plate_w = w + 2 * PAD;

                /* a narrow picture keeps room for its title */
                plate_w = max( plate_w, px( 120 ) );
                SetRect( &c->plate, x, y, x + plate_w, y + 2 * PAD + TITLE_H + best_h );
                SetRect( &c->title_rect, x + PAD, y + PAD, x + plate_w - PAD, y + PAD + TITLE_H );
                SetRect( &c->slot, x + (plate_w - w) / 2, y + PAD + TITLE_H + (best_h - h) / 2, 0, 0 );
                c->slot.right = c->slot.left + w;
                c->slot.bottom = c->slot.top + h;
                SetRect( &c->close, c->title_rect.right - CLOSE, c->title_rect.top + (TITLE_H - CLOSE) / 2,
                         c->title_rect.right, c->title_rect.top + (TITLE_H + CLOSE) / 2 );
                x += plate_w + GAP;
            }
            y += best_h + TITLE_H + 2 * PAD + GAP;
        }
    }
}

static void update_thumbnail( struct card *card, BOOL force )
{
    DWM_THUMBNAIL_PROPERTIES props = { DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY |
                                       DWM_TNP_SOURCECLIENTAREAONLY };
    double t, now = frame_ms();
    float e;
    RECT rect;
    BYTE op;

    if (!card->thumbnail) return;
    t = card->move.length ? (now - card->move.start) / fabs( card->move.length ) : 1;
    e = card->move.length < 0 ? ease_in_out( (float)t ) : ease_out( (float)t );
    rect = lerp_rect( &card->move.from, &card->move.to, e );
    op = (BYTE)lroundf( card->move.op_from + (card->move.op_to - card->move.op_from) * e );
    if (card->leaving)
    {
        float l = clamp01( (float)((now - card->leave_start) / LEAVE_MS) );

        rect = scaled_rect( &card->slot, 1 - 0.12f * ease_out( l ) );
        op = (BYTE)(255 * (1 - l));
    }
    if (!force && EqualRect( &rect, &card->shown ) && op == card->shown_op) return;
    card->shown = rect;
    card->shown_op = op;
    props.rcDestination = rect;
    props.fVisible = op > 0 && rect.right > rect.left && rect.bottom > rect.top;
    props.opacity = op;
    props.fSourceClientAreaOnly = FALSE;
    DwmUpdateThumbnailProperties( card->thumbnail, &props );
}

static BOOL tween_running( const struct card *card, double now )
{
    return card->move.length && now - card->move.start < fabs( card->move.length );
}

static void notify_button( BOOL visible )
{
    HWND tray = FindWindowW( L"Shell_TrayWnd", NULL ), button;

    if (!tray || !(button = FindWindowExW( tray, NULL, L"ArcticTaskViewButton", NULL ))) return;
    PostMessageW( button, RegisterWindowMessageW( L"ArcticTaskViewChanged" ), visible, 0 );
}

static void free_cards(void)
{
    for (int i = 0; i < count; i++)
    {
        if (cards[i].thumbnail) DwmUnregisterThumbnail( cards[i].thumbnail );
        if (cards[i].label) DeleteObject( cards[i].label );
    }
    count = 0;
}

static void set_backdrop_fade( BYTE fade )
{
    DWORD value = fade;

    DwmSetWindowAttribute( view, DWMWA_ARCTIC_BACKDROP_FADE, &value, sizeof(value) );
}

static void hide_view( DWORD transition )
{
    KillTimer( view, TIMER_FRAME );
    KillTimer( view, TIMER_WATCH );
    set_transition( view, transition );
    ShowWindow( view, SW_HIDE );
    set_backdrop_fade( 0 );
    free_cards();
    hot = -1;
    phase = HIDDEN;
    notify_button( FALSE );
}

/* something else took the screen: the view fades away from over it */
static void hide_now(void)
{
    hide_view( ARCTIC_TRANSITION_FADE );
}

/* back to the windows: each picture flies to its window, the one chosen comes to the front */
static void begin_close( HWND chosen )
{
    if (phase == HIDDEN || phase == CLOSING) return;
    phase = CLOSING;
    phase_start = now_ms();
    target = chosen;
    hot = -1;
    for (int i = 0; i < count; i++)
    {
        struct card *card = &cards[i];

        if (card->leaving) continue;
        if (card->minimized && card->hwnd != chosen)
        {
            RECT to = scaled_rect( &card->slot, 0.9f );
            tween_start( card, &to, 0, CLOSE_MS, TRUE );
        }
        else tween_start( card, &card->real, 255, CLOSE_MS, TRUE );
    }
    /* The chosen picture flies over the others: its thumbnail is made anew,
     * and dwm draws the thumbnail made last on top. Under the window that
     * was in front, it flew in second and the switch came only at the end,
     * at once (2026-11-18). */
    for (int i = 0; chosen && i < count; i++)
    {
        HTHUMBNAIL thumbnail;

        if (cards[i].hwnd != chosen || !cards[i].thumbnail) continue;
        if (SUCCEEDED(DwmRegisterThumbnail( view, chosen, &thumbnail )))
        {
            DwmUnregisterThumbnail( cards[i].thumbnail );
            cards[i].thumbnail = thumbnail;
            update_thumbnail( &cards[i], TRUE );
        }
    }
    if (chosen && IsWindow( chosen )) SwitchToThisWindow( chosen, TRUE );
    InvalidateRect( view, NULL, FALSE );
    SetTimer( view, TIMER_FRAME, 8, NULL );
}

static void relayout_animated(void)
{
    layout();
    for (int i = 0; i < count; i++)
    {
        if (cards[i].leaving) continue;
        make_label( &cards[i] );
        tween_start( &cards[i], &cards[i].slot, 255, MOVE_MS, FALSE );
    }
    InvalidateRect( view, NULL, FALSE );
    SetTimer( view, TIMER_FRAME, 8, NULL );
}

static void close_card( int index )
{
    struct card *card;

    if (index < 0 || index >= count || cards[index].leaving || phase == CLOSING) return;
    card = &cards[index];
    PostMessageW( card->hwnd, WM_SYSCOMMAND, SC_CLOSE, 0 );
    card->leaving = TRUE;
    card->leave_start = now_ms();
    if (hot == index) hot = -1;
    relayout_animated();
}

/* a window that went by itself goes from the view too */
static void watch_windows(void)
{
    BOOL changed = FALSE;

    for (int i = 0; i < count; i++)
    {
        if (cards[i].leaving || (IsWindow( cards[i].hwnd ) && IsWindowVisible( cards[i].hwnd ))) continue;
        cards[i].leaving = TRUE;
        cards[i].leave_start = now_ms();
        changed = TRUE;
    }
    if (changed) relayout_animated();
}

/* the cards whose closing is over are taken out */
static void drop_left( double now )
{
    for (int i = 0; i < count; )
    {
        struct card *card = &cards[i];

        if (!card->leaving || now - card->leave_start < LEAVE_MS)
        {
            i++;
            continue;
        }
        if (card->thumbnail) DwmUnregisterThumbnail( card->thumbnail );
        if (card->label) DeleteObject( card->label );
        memmove( card, card + 1, (count - i - 1) * sizeof(*card) );
        count--;
        if (hot == i) hot = -1;
        else if (hot > i) hot--;
        if (focus >= count) focus = max( 0, count - 1 );
        else if (focus > i) focus--;
        InvalidateRect( view, NULL, FALSE );
    }
}

static void frame(void)
{
    double now = frame_ms();
    BOOL busy = FALSE;
    float old_alpha = label_alpha;

    if (phase == OPENING)
    {
        float t = (float)((now - phase_start) / OPEN_MS);

        /* the titles come once the pictures are nearly there */
        label_alpha = clamp01( (t - 0.55f) / 0.45f );
        if (t >= 1) phase = SHOWN;
    }
    else if (phase == CLOSING)
    {
        float t = (float)((now - phase_start) / CLOSE_MS);

        label_alpha = 1 - clamp01( t / 0.35f );
        /* the backdrop fades as the pictures fly, gone when they land: hidden
         * only then, at once, the screen does not blink (2026-11-18) */
        set_backdrop_fade( (BYTE)(255 * ease_in_out( clamp01( t ) )) );
        if (t >= 1)
        {
            for (int i = 0; i < count; i++) update_thumbnail( &cards[i], FALSE );
            if (!target && IsWindow( previous )) SetForegroundWindow( previous );
            hide_view( ARCTIC_TRANSITION_NONE );
            return;
        }
    }
    else label_alpha = 1;

    for (int i = 0; i < count; i++)
    {
        struct card *card = &cards[i];

        update_thumbnail( card, FALSE );
        if (tween_running( card, now ) || card->leaving) busy = TRUE;
        if (card->hot != card->hot_to)
        {
            float t = clamp01( (float)((now - card->hot_start) / HOT_MS) );

            card->hot = card->hot_from + (card->hot_to - card->hot_from) * ease_out( t );
            if (t >= 1) card->hot = card->hot_to;
            InvalidateRect( view, &card->plate, FALSE );
            busy = TRUE;
        }
        else if (card->leaving) InvalidateRect( view, &card->plate, FALSE );
    }
    if (label_alpha != old_alpha)
    {
        for (int i = 0; i < count; i++) InvalidateRect( view, &cards[i].plate, FALSE );
        busy = TRUE;
    }
    drop_left( now );
    if (phase == OPENING || phase == CLOSING) busy = TRUE;
    if (!busy) KillTimer( view, TIMER_FRAME );
}

static void kick(void)
{
    SetTimer( view, TIMER_FRAME, 8, NULL );
}

static void set_hot( int index, BOOL on_close )
{
    if (index == hot && on_close == hot_close) return;
    if (hot >= 0 && hot < count)
    {
        cards[hot].hot_from = cards[hot].hot;
        cards[hot].hot_to = 0;
        cards[hot].hot_start = now_ms();
        InvalidateRect( view, &cards[hot].plate, FALSE );
    }
    if (index >= 0 && index != hot)
    {
        cards[index].hot_from = cards[index].hot;
        cards[index].hot_to = 1;
        cards[index].hot_start = now_ms();
    }
    if (index >= 0) InvalidateRect( view, &cards[index].plate, FALSE );
    hot = index;
    hot_close = on_close;
    kick();
}

static void open_view(void)
{
    MONITORINFO info = { sizeof(info) };
    HMONITOR monitor;
    POINT pt;
    DWORD corner = ARCTIC_CORNER_RADIUS( 0 );

    if (phase != HIDDEN) return;
    previous = GetForegroundWindow();
    target = NULL;
    GetCursorPos( &pt );
    monitor = MonitorFromPoint( pt, MONITOR_DEFAULTTOPRIMARY );
    GetMonitorInfoW( monitor, &info );
    origin = info.rcWork;
    count = 0;
    hot = -1;
    focus = 0;
    focus_shown = FALSE;
    EnumWindows( add_window, (LPARAM)monitor );
    /* the window that was active, first */
    for (int i = 1; i < count; i++)
    {
        if (cards[i].hwnd != previous) continue;
        focus = i;
        break;
    }
    layout();

    set_acrylic( view );     /* the look may have changed since */
    if (!font) font = shell_font( px( 12 ), FW_NORMAL );
    DwmSetWindowAttribute( view, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner) );
    set_transition( view, ARCTIC_TRANSITION_FADE );
    SetWindowPos( view, HWND_TOPMOST, origin.left, origin.top, origin.right - origin.left,
                  origin.bottom - origin.top, SWP_NOACTIVATE );

    /* the windows lower down first, so that the top one is drawn over them */
    for (int i = count - 1; i >= 0; i--)
    {
        struct card *card = &cards[i];

        make_label( card );
        if (FAILED(DwmRegisterThumbnail( view, card->hwnd, &card->thumbnail ))) card->thumbnail = NULL;
        card->shown = card->minimized ? scaled_rect( &card->slot, 0.88f ) : card->real;
        card->shown_op = card->minimized ? 0 : 255;
        tween_start( card, &card->slot, 255, OPEN_MS, FALSE );
        update_thumbnail( card, TRUE );
    }
    phase = OPENING;
    phase_start = now_ms();
    label_alpha = 0;
    ShowWindow( view, SW_SHOW );
    SetForegroundWindow( view );
    SetFocus( view );
    SetTimer( view, TIMER_FRAME, 8, NULL );
    SetTimer( view, TIMER_WATCH, 250, NULL );
    notify_button( TRUE );
}

static int hit_test( POINT pt, BOOL *on_close )
{
    *on_close = FALSE;
    for (int i = 0; i < count; i++)
    {
        if (cards[i].leaving || !PtInRect( &cards[i].plate, pt )) continue;
        *on_close = PtInRect( &cards[i].close, pt );
        return i;
    }
    return -1;
}

/* the nearest card in a direction */
static void move_focus( int dx, int dy )
{
    const RECT *from;
    int best = -1, best_score = INT_MAX;
    POINT c;

    if (!count) return;
    if (!focus_shown)
    {
        focus_shown = TRUE;
        InvalidateRect( view, NULL, FALSE );
        return;
    }
    from = &cards[focus].plate;
    c.x = (from->left + from->right) / 2;
    c.y = (from->top + from->bottom) / 2;
    for (int i = 0; i < count; i++)
    {
        const RECT *r = &cards[i].plate;
        int x = (r->left + r->right) / 2 - c.x, y = (r->top + r->bottom) / 2 - c.y, along, across;

        if (i == focus || cards[i].leaving) continue;
        along = dx ? x * dx : y * dy;
        across = abs( dx ? y : x );
        if (along <= 0) continue;
        if (along + 3 * across < best_score)
        {
            best_score = along + 3 * across;
            best = i;
        }
    }
    if (best < 0) return;
    InvalidateRect( view, &cards[focus].plate, FALSE );
    focus = best;
    InvalidateRect( view, &cards[focus].plate, FALSE );
}

static void cycle_focus( int delta )
{
    if (!count) return;
    if (focus_shown) focus = ((focus + delta) % count + count) % count;
    focus_shown = TRUE;
    InvalidateRect( view, NULL, FALSE );
}

static LRESULT CALLBACK view_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    POINT pt = { (short)LOWORD( lparam ), (short)HIWORD( lparam ) };
    BOOL on_close;
    int i;

    switch (msg)
    {
    case WM_TV_TOGGLE:
        /* the click on the button took the foreground first, and the view
         * went with it: that click closed it, as in Windows, and does not
         * open it again */
        if (phase == HIDDEN && now_ms() - left_at < 500) return 0;
        if (phase == HIDDEN) open_view();
        else begin_close( NULL );
        return 0;

    case WM_TV_HIDE:
        if (phase != HIDDEN) begin_close( NULL );
        return 0;

    case WM_TIMER:
        if (wparam == TIMER_FRAME) frame();
        else if (wparam == TIMER_WATCH && phase == SHOWN) watch_windows();
        return 0;

    case WM_MOUSEMOVE:
    {
        TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, hwnd };

        if (phase == CLOSING) return 0;
        i = hit_test( pt, &on_close );
        set_hot( i, on_close );
        TrackMouseEvent( &track );
        return 0;
    }

    case WM_MOUSELEAVE:
        set_hot( -1, FALSE );
        return 0;

    case WM_LBUTTONUP:
        if (phase == CLOSING || phase == HIDDEN) return 0;
        i = hit_test( pt, &on_close );
        if (i < 0) begin_close( NULL );
        else if (on_close) close_card( i );
        else begin_close( cards[i].hwnd );
        return 0;

    case WM_MBUTTONUP:
        if ((i = hit_test( pt, &on_close )) >= 0) close_card( i );
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (phase == CLOSING || phase == HIDDEN) return 0;
        switch (wparam)
        {
        case VK_ESCAPE: begin_close( NULL ); break;
        case VK_LEFT:   move_focus( -1, 0 ); break;
        case VK_RIGHT:  move_focus( 1, 0 ); break;
        case VK_UP:     move_focus( 0, -1 ); break;
        case VK_DOWN:   move_focus( 0, 1 ); break;
        case VK_TAB:    cycle_focus( GetKeyState( VK_SHIFT ) < 0 ? -1 : 1 ); break;
        case VK_RETURN:
        case VK_SPACE:
            if (count && focus < count) begin_close( cards[focus].hwnd );
            else begin_close( NULL );
            break;
        case VK_DELETE: if (focus_shown) close_card( focus ); break;
        }
        kick();
        return 0;

    case WM_SYSCOMMAND:
        if ((wparam & 0xfff0) == SC_KEYMENU) return 0;
        break;

    case WM_ACTIVATE:
        /* something else took the foreground: the view goes at once */
        if (LOWORD( wparam ) == WA_INACTIVE && (phase == OPENING || phase == SHOWN))
        {
            HWND other = (HWND)lparam;

            if (other && GetWindowThreadProcessId( other, NULL ) == GetCurrentThreadId()) return 0;
            left_at = now_ms();
            /* the taskbar (its Task View button): back to the windows as by Esc */
            if (other)
            {
                WCHAR cls[32];

                GetClassNameW( GetAncestor( other, GA_ROOT ), cls, ARRAY_SIZE(cls) );
                if (!wcscmp( cls, L"Shell_TrayWnd" ))
                {
                    begin_close( NULL );
                    return 0;
                }
            }
            hide_now();
        }
        return 0;

    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        on_paint();
        return 0;

    case WM_SETTINGCHANGE:
    case WM_DISPLAYCHANGE:
        if (phase != HIDDEN) hide_now();
        break;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

/* in the switcher's thread */
void taskview_init(void)
{
    WNDCLASSW cls = { 0 };

    cls.lpfnWndProc = view_proc;
    cls.hInstance = twinui_instance;
    cls.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_ARROW );
    cls.lpszClassName = L"ArcticTaskView";
    RegisterClassW( &cls );
    view = CreateWindowExW( WS_EX_TOOLWINDOW | WS_EX_TOPMOST, cls.lpszClassName, L"", WS_POPUP,
                            0, 0, 1, 1, NULL, NULL, twinui_instance, NULL );
    if (!view) ERR( "no Task View: %lu\n", GetLastError() );
}

BOOL taskview_toggle(void)
{
    if (!switcher_start() || !view) return FALSE;
    return PostMessageW( view, WM_TV_TOGGLE, 0, 0 );
}

BOOL taskview_visible(void)
{
    return phase != HIDDEN && phase != CLOSING;
}
