/*
 * Task Manager: the Performance tab
 *
 * On the left a tile for each resource with its small graph: the
 * processors, memory, every disk, the network and every graphics card, in
 * Windows 10's order. On the right the chosen one: its name and model, a
 * minute of its use on a grid in its own colour (the processors and the
 * graphics blue, memory purple, the disks green, the network brown, as
 * Windows 10), and what it is made of under it.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <math.h>
#include "taskmgr.h"

enum { RES_CPU, RES_MEMORY, RES_DISK, RES_NET, RES_GPU, RES_COUNT };

/* a tile: the kind of resource, and which disk slot or card */
struct item { int res; UINT index; };

struct palette { COLORREF line, fill, grid, border; };
static const struct palette palettes[RES_COUNT] =
{
    { RGB(17, 125, 187),  RGB(241, 246, 250), RGB(217, 234, 244), RGB(17, 125, 187) },
    { RGB(139, 18, 174),  RGB(244, 242, 244), RGB(236, 222, 240), RGB(139, 18, 174) },
    { RGB(77, 166, 12),   RGB(239, 247, 233), RGB(206, 230, 190), RGB(77, 166, 12) },
    { RGB(167, 79, 1),    RGB(252, 243, 235), RGB(242, 222, 200), RGB(167, 79, 1) },
    { RGB(17, 125, 187),  RGB(241, 246, 250), RGB(217, 234, 244), RGB(17, 125, 187) },
};

#define MAX_ITEMS (4 + MAX_DISKS + MAX_GPUS)

static HWND page;
static struct item current = { RES_CPU, 0 };
static BOOL logical_view;          /* the processors one graph each */
static int nav_width, nav_scroll;

/* ------------------------------------------------------------------ items */

/* the tiles as Windows 10 lists them: CPU, memory, the disks by number, the
 * network, the graphics cards */
static UINT list_items( struct item *items )
{
    UINT n = 0, slots[MAX_DISKS], disks = data_disk_order( slots );

    items[n].res = RES_CPU; items[n++].index = 0;
    items[n].res = RES_MEMORY; items[n++].index = 0;
    for (UINT i = 0; i < disks; i++) { items[n].res = RES_DISK; items[n++].index = slots[i]; }
    if (sysinfo.have_net) { items[n].res = RES_NET; items[n++].index = 0; }
    for (UINT i = 0; i < sysinfo.gpu_count; i++) { items[n].res = RES_GPU; items[n++].index = i; }
    return n;
}

static BOOL same_item( const struct item *a, const struct item *b )
{
    return a->res == b->res && a->index == b->index;
}

/* the chosen tile is still there, else the processors */
static void check_current(void)
{
    struct item items[MAX_ITEMS];
    UINT n = list_items( items );

    for (UINT i = 0; i < n; i++) if (same_item( &items[i], &current )) return;
    current.res = RES_CPU;
    current.index = 0;
}

/* ------------------------------------------------------------------ values */

static double disk_transfer_max( UINT slot )
{
    double m = 1024 * 1024;

    for (UINT i = 0; i < history_count; i++)
    {
        const struct sample *s = data_back( i );
        while (m < s->disks[slot].read + s->disks[slot].write) m *= 2;
    }
    return m;
}

/* a sample's value for the tile, 0..1 of the graph's height */
static double value_of( const struct item *it, const struct sample *s, double scale_max )
{
    switch (it->res)
    {
    case RES_CPU: return s->cpu / 100.0;
    case RES_MEMORY: return sysinfo.mem_total ? s->mem_used / sysinfo.mem_total : 0;
    case RES_DISK: return s->disks[it->index].active / 100.0;
    case RES_NET: return scale_max > 0 ? (s->net_send + s->net_recv) / scale_max : 0;
    case RES_GPU: return s->gpus[it->index].util / 100.0;
    }
    return 0;
}

static double value_send( const struct item *it, const struct sample *s, double m ) { return m > 0 ? s->net_send / m : 0; }

static double value_read_write( const struct item *it, const struct sample *s, double m )
{
    return m > 0 ? (s->disks[it->index].read + s->disks[it->index].write) / m : 0;
}

static double value_gpu_memory( const struct item *it, const struct sample *s, double m )
{
    double used = s->gpus[it->index].mem_used;
    return m > 0 && used > 0 ? used / m : 0;
}

/* the network's graph scales to what went through: 100 Kbit/s, 1 Mbit/s... */
static double net_scale(void)
{
    static const double steps[] = { 100e3, 500e3, 1e6, 5e6, 10e6, 50e6, 100e6, 500e6, 1e9, 5e9, 10e9 };
    double peak = 0;

    for (UINT i = 0; i < history_count; i++)
    {
        const struct sample *s = data_back( i );
        peak = max( peak, s->net_send + s->net_recv );
    }
    for (UINT i = 0; i < ARRAY_SIZE(steps); i++) if (peak <= steps[i]) return steps[i];
    return steps[ARRAY_SIZE(steps) - 1];
}

static double scale_of( const struct item *it )
{
    return it->res == RES_NET ? net_scale() : 1;
}

static void decimal_comma( WCHAR *buf )
{
    for (WCHAR *p = buf; *p; p++) if (*p == '.') *p = ',';
}

static void format_bits( WCHAR *buf, size_t len, double bps )
{
    if (bps >= 1e9) swprintf( buf, len, L"%.1f %s", bps / 1e9, load_string( IDS_UNIT_GBPS ) );
    else if (bps >= 1e6) swprintf( buf, len, L"%.1f %s", bps / 1e6, load_string( IDS_UNIT_MBPS ) );
    else swprintf( buf, len, L"%.0f %s", bps / 1e3, load_string( IDS_UNIT_KBPS ) );
    decimal_comma( buf );
}

static void format_ghz( WCHAR *buf, size_t len, DWORD mhz )
{
    swprintf( buf, len, L"%.2f %s", mhz / 1000.0, load_string( IDS_UNIT_GHZ ) );
    decimal_comma( buf );
}

/* "Диск 0 (C: D:)" */
static void disk_name( UINT slot, WCHAR *buf, size_t len )
{
    const struct disk_info *d = &sysinfo.disks[slot];

    if (d->letters[0]) swprintf( buf, len, L"%s %u (%s)", load_string( IDS_PERF_DISK ), (UINT)(d->number < 1000 ? d->number : slot), d->letters );
    else swprintf( buf, len, L"%s %u", load_string( IDS_PERF_DISK ), (UINT)d->number );
}

/* the tile's name, and over the page */
static void item_name( const struct item *it, WCHAR *buf, size_t len )
{
    switch (it->res)
    {
    case RES_CPU: lstrcpynW( buf, load_string( IDS_PERF_CPU ), len ); break;
    case RES_MEMORY: lstrcpynW( buf, load_string( IDS_PERF_MEMORY ), len ); break;
    case RES_DISK: disk_name( it->index, buf, len ); break;
    case RES_NET: lstrcpynW( buf, load_string( sysinfo.wifi ? IDS_PERF_WIFI : IDS_PERF_ETHERNET ), len ); break;
    case RES_GPU: swprintf( buf, len, L"%s %u", load_string( IDS_PERF_GPU ), it->index ); break;
    default: buf[0] = 0;
    }
}

static const WCHAR *disk_kind( const struct disk_info *d )
{
    return load_string( d->removable ? IDS_PERF_REMOVABLE : d->ssd ? IDS_PERF_SSD : IDS_PERF_HDD );
}

/* the tile's line between the name and the figures, if it has one: the
 * disk's type, the card's model, the network adapter */
static BOOL item_subtitle( const struct item *it, WCHAR *buf, size_t len )
{
    switch (it->res)
    {
    case RES_DISK: lstrcpynW( buf, disk_kind( &sysinfo.disks[it->index] ), len ); return TRUE;
    case RES_NET: lstrcpynW( buf, sysinfo.net_name, len ); return TRUE;
    case RES_GPU: lstrcpynW( buf, sysinfo.gpus[it->index].name, len ); return TRUE;
    }
    return FALSE;
}

/* the tile's figures: "8%  2,80 ГГц", "3,1/7,9 ГБ (39%)", "2% (45 °C)" */
static void item_summary( const struct item *it, WCHAR *buf, size_t len )
{
    const struct sample *s = data_latest();
    WCHAR a[64], b[64];

    switch (it->res)
    {
    case RES_CPU:
        format_ghz( a, ARRAY_SIZE(a), sysinfo.cpu_mhz_now );
        swprintf( buf, len, L"%.0f%%  %s", s->cpu, a );
        break;
    case RES_MEMORY:
        swprintf( a, ARRAY_SIZE(a), L"%.1f", s->mem_used / (1024.0 * 1024 * 1024) );
        swprintf( b, ARRAY_SIZE(b), L"%.1f", sysinfo.mem_total / (1024.0 * 1024 * 1024) );
        swprintf( buf, len, L"%s/%s %s (%.0f%%)", a, b, load_string( IDS_UNIT_GB ),
                  sysinfo.mem_total ? s->mem_used * 100.0 / sysinfo.mem_total : 0 );
        decimal_comma( buf );
        break;
    case RES_DISK:
        swprintf( buf, len, L"%.0f%%", s->disks[it->index].active );
        break;
    case RES_NET:
        format_bits( a, ARRAY_SIZE(a), s->net_send );
        format_bits( b, ARRAY_SIZE(b), s->net_recv );
        /* the tile is narrow: "Н: 0 О: 8 Кбіт/с", as Windows shortens it */
        swprintf( buf, len, L"%.1s: %s  %.1s: %s", load_string( IDS_PERF_SEND ), a, load_string( IDS_PERF_RECEIVE ), b );
        break;
    case RES_GPU:
        if (s->gpus[it->index].temperature != NO_VALUE)
            swprintf( buf, len, L"%.0f%% (%.0f °C)", s->gpus[it->index].util, s->gpus[it->index].temperature );
        else swprintf( buf, len, L"%.0f%%", s->gpus[it->index].util );
        break;
    }
}

/* ------------------------------------------------------------------ drawing */

static void fill( HDC hdc, const RECT *rc, COLORREF color )
{
    HBRUSH b = CreateSolidBrush( color );
    FillRect( hdc, rc, b );
    DeleteObject( b );
}

static void frame( HDC hdc, const RECT *rc, COLORREF color )
{
    HBRUSH b = CreateSolidBrush( color );
    FrameRect( hdc, rc, b );
    DeleteObject( b );
}

static void text_at( HDC hdc, HFONT font, COLORREF color, int x, int y, const WCHAR *text, UINT align )
{
    RECT rc = { x, y, x, y };
    SelectObject( hdc, font );
    SetTextColor( hdc, color );
    DrawTextW( hdc, text, -1, &rc, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX );
    if (align & DT_RIGHT) OffsetRect( &rc, x - rc.right, 0 );
    DrawTextW( hdc, text, -1, &rc, DT_SINGLELINE | DT_NOPREFIX );
}

/* a line cut with "..." where it does not fit */
static void text_in( HDC hdc, HFONT font, COLORREF color, int x, int y, int right, const WCHAR *text )
{
    RECT rc = { x, y, right, y + 100 };
    SelectObject( hdc, font );
    SetTextColor( hdc, color );
    DrawTextW( hdc, text, -1, &rc, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS );
}

static int text_height( HDC hdc, HFONT font )
{
    TEXTMETRICW tm;
    SelectObject( hdc, font );
    GetTextMetricsW( hdc, &tm );
    return tm.tmHeight;
}

/* a minute of values, newest on the right, on a grid that moves with time */
static void draw_graph( HDC hdc, const RECT *rc, const struct item *it,
                        double (*get)( const struct item *, const struct sample *, double ),
                        double scale_max, BOOL with_grid, int cpu )
{
    const struct palette *pal = &palettes[it->res];
    int w = rc->right - rc->left, h = rc->bottom - rc->top;
    POINT pts[HISTORY + 2];
    UINT n = 0;
    HPEN pen;
    HGDIOBJ old;
    HRGN clip;

    fill( hdc, rc, RGB(255, 255, 255) );
    if (with_grid)
    {
        HPEN gp = CreatePen( PS_SOLID, 1, pal->grid );
        int shift = (int)(history_count % 2);   /* the vertical lines step left as time goes */
        old = SelectObject( hdc, gp );
        for (int i = 1; i < 10; i++)
        {
            MoveToEx( hdc, rc->left, rc->top + h * i / 10, NULL );
            LineTo( hdc, rc->right, rc->top + h * i / 10 );
        }
        for (int i = 1; i < 20; i++)
        {
            int x = rc->left + (w * i / 20) - shift * w / 40;
            MoveToEx( hdc, x, rc->top, NULL );
            LineTo( hdc, x, rc->bottom );
        }
        SelectObject( hdc, old );
        DeleteObject( gp );
    }

    for (UINT i = 0; i < HISTORY; i++)
    {
        const struct sample *s = data_back( HISTORY - 1 - i );
        double v;
        if (!s) continue;
        v = cpu >= 0 ? s->cpu_each[cpu] / 100.0 : get( it, s, scale_max );
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        pts[n].x = rc->left + (int)((double)w * i / (HISTORY - 1));
        pts[n].y = rc->bottom - 1 - (int)(v * (h - 2));
        n++;
    }
    clip = CreateRectRgnIndirect( rc );
    SelectClipRgn( hdc, clip );
    if (n >= 2)
    {
        HBRUSH fb = CreateSolidBrush( pal->fill );
        HPEN np = CreatePen( PS_NULL, 0, 0 );
        pts[n].x = pts[n - 1].x; pts[n].y = rc->bottom;
        pts[n + 1].x = pts[0].x; pts[n + 1].y = rc->bottom;
        old = SelectObject( hdc, fb );
        SelectObject( hdc, np );
        Polygon( hdc, pts, n + 2 );
        SelectObject( hdc, old );
        DeleteObject( fb );
        DeleteObject( np );
        pen = CreatePen( PS_SOLID, max( 1, scale( 1 ) ), pal->line );
        old = SelectObject( hdc, pen );
        Polyline( hdc, pts, n );
        SelectObject( hdc, old );
        DeleteObject( pen );
    }
    SelectClipRgn( hdc, NULL );
    DeleteObject( clip );
    frame( hdc, rc, pal->border );
}

static int tile_height(void)
{
    return scale( 70 );
}

static RECT nav_item_rect( int index )
{
    RECT rc = { 0, scale( 8 ) + index * tile_height() - nav_scroll, nav_width,
                scale( 8 ) + (index + 1) * tile_height() - nav_scroll };
    return rc;
}

static void draw_nav( HDC hdc, const RECT *client )
{
    struct item items[MAX_ITEMS];
    UINT n = list_items( items );
    int lh = text_height( hdc, small_font );
    HRGN clip = CreateRectRgn( 0, 0, nav_width, client->bottom );

    SelectClipRgn( hdc, clip );
    for (UINT i = 0; i < n; i++)
    {
        RECT rc = nav_item_rect( i ), gr;
        WCHAR name[128], line[160];
        int tx, ty;
        BOOL sub;

        if (rc.bottom < 0 || rc.top > client->bottom) continue;
        if (same_item( &items[i], &current )) fill( hdc, &rc, RGB(204, 232, 255) );
        gr.left = rc.left + scale( 10 );
        gr.top = rc.top + scale( 10 );
        gr.right = gr.left + scale( 70 );
        gr.bottom = rc.bottom - scale( 10 );
        draw_graph( hdc, &gr, &items[i], value_of, scale_of( &items[i] ), FALSE, -1 );

        /* two lines, or three with the type or the model between */
        tx = gr.right + scale( 10 );
        item_name( &items[i], name, ARRAY_SIZE(name) );
        if (items[i].res == RES_GPU)
        {
            SIZE sz;

            /* "Графічний процесор 0" where it fits, "ГП 0" where it does not */
            SelectObject( hdc, title_font );
            GetTextExtentPoint32W( hdc, name, wcslen( name ), &sz );
            if (tx + sz.cx > rc.right - scale( 6 ))
                swprintf( name, ARRAY_SIZE(name), L"%s %u", load_string( IDS_PERF_GPU_SHORT ), items[i].index );
        }
        sub = item_subtitle( &items[i], line, ARRAY_SIZE(line) );
        ty = rc.top + (sub ? scale( 7 ) : scale( 12 ));
        text_in( hdc, title_font, RGB(0, 0, 0), tx, ty, rc.right - scale( 6 ), name );
        ty += text_height( hdc, title_font ) + (sub ? 0 : scale( 6 ));
        if (sub)
        {
            text_in( hdc, small_font, RGB(102, 102, 102), tx, ty, rc.right - scale( 6 ), line );
            ty += lh;
        }
        item_summary( &items[i], line, ARRAY_SIZE(line) );
        text_in( hdc, small_font, RGB(102, 102, 102), tx, ty, rc.right - scale( 6 ), line );
    }
    SelectClipRgn( hdc, NULL );
    DeleteObject( clip );
}

struct figure { UINT label; WCHAR value[64]; };

/* the figures under the graph: big values in a grid on the left, the
 * resource's facts in a table on the right */
static void draw_stats( HDC hdc, int x, int y, int right, const struct figure *big, UINT big_count, UINT per_row,
                        const struct figure *facts, UINT fact_count )
{
    int col_w = scale( 120 ), row_h = scale( 48 ), lh = text_height( hdc, small_font );
    int fx, fy = y, value_x = scale( 170 );
    UINT rows = (big_count + per_row - 1) / per_row;

    /* a column wide enough for its longest label */
    for (UINT i = 0; i < big_count; i++)
    {
        SIZE sz;
        const WCHAR *label = load_string( big[i].label );
        SelectObject( hdc, small_font );
        GetTextExtentPoint32W( hdc, label, wcslen( label ), &sz );
        col_w = max( col_w, sz.cx + scale( 14 ) );
    }

    if (right - x >= (int)per_row * col_w + scale( 330 )) fx = right - scale( 310 );
    else
    {
        fx = x;
        fy = y + rows * row_h + scale( 4 );
    }

    for (UINT i = 0; i < big_count; i++)
    {
        int cx = x + (i % per_row) * col_w, cy = y + (i / per_row) * row_h;
        text_at( hdc, small_font, RGB(102, 102, 102), cx, cy, load_string( big[i].label ), 0 );
        text_at( hdc, title_font, RGB(0, 0, 0), cx, cy + lh, big[i].value, 0 );
    }
    for (UINT i = 0; i < fact_count; i++)
    {
        WCHAR label[128];
        swprintf( label, ARRAY_SIZE(label), L"%s:", load_string( facts[i].label ) );
        text_at( hdc, small_font, RGB(102, 102, 102), fx, fy + i * (lh + scale( 4 )), label, 0 );
        text_at( hdc, small_font, RGB(0, 0, 0), fx + value_x, fy + i * (lh + scale( 4 )), facts[i].value, 0 );
    }
}

static void format_uptime( WCHAR *buf, size_t len )
{
    ULONGLONG s = sysinfo.uptime_ms / 1000;
    swprintf( buf, len, L"%u:%02u:%02u:%02u", (UINT)(s / 86400), (UINT)(s / 3600 % 24), (UINT)(s / 60 % 60), (UINT)(s % 60) );
}

/* a second, smaller graph under the first: a disk's transfers, a card's memory */
static int draw_second_graph( HDC hdc, int x, int y, int right, UINT label, double m, BOOL bytes_per_s,
                              double (*get)( const struct item *, const struct sample *, double ) )
{
    int lh = text_height( hdc, small_font );
    WCHAR buf[64];
    RECT g2;

    text_at( hdc, small_font, RGB(102, 102, 102), x, y, load_string( label ), 0 );
    format_bytes( buf, ARRAY_SIZE(buf), m, bytes_per_s );
    text_at( hdc, small_font, RGB(102, 102, 102), right, y, buf, DT_RIGHT );
    SetRect( &g2, x, y + lh + scale( 2 ), right, y + lh + scale( 2 ) + scale( 60 ) );
    draw_graph( hdc, &g2, &current, get, m, TRUE, -1 );
    return g2.bottom + scale( 14 );
}

static void draw_page( HDC hdc, const RECT *client )
{
    int x = nav_width + scale( 20 ), right = client->right - scale( 20 ), y = scale( 12 );
    int lh = text_height( hdc, small_font ), gh, below, title_right;
    const struct sample *s = data_latest();
    const struct palette *pal = &palettes[current.res];
    const struct disk_info *disk = current.res == RES_DISK ? &sysinfo.disks[current.index] : NULL;
    const struct gpu_info *gpu = current.res == RES_GPU ? &sysinfo.gpus[current.index] : NULL;
    BOOL gpu_memory = gpu && gpu->mem_total > 0;
    RECT gr;
    struct figure big[8], facts[8];
    UINT nb = 0, nf = 0;
    WCHAR buf[128], buf2[64];
    HPEN sep = CreatePen( PS_SOLID, 1, RGB(229, 229, 229) ), old;

    old = SelectObject( hdc, sep );
    MoveToEx( hdc, nav_width, 0, NULL );
    LineTo( hdc, nav_width, client->bottom );
    SelectObject( hdc, old );
    DeleteObject( sep );

    /* the title and, on the right, what it is */
    if (current.res == RES_GPU) lstrcpynW( buf, load_string( IDS_PERF_GPU ), ARRAY_SIZE(buf) );
    else item_name( &current, buf, ARRAY_SIZE(buf) );
    text_at( hdc, big_font, RGB(0, 0, 0), x, y, buf, 0 );
    {
        SIZE sz;
        SelectObject( hdc, big_font );
        GetTextExtentPoint32W( hdc, buf, wcslen( buf ), &sz );
        title_right = x + sz.cx + scale( 16 );
    }
    buf[0] = 0;
    switch (current.res)
    {
    case RES_CPU: lstrcpynW( buf, sysinfo.cpu_name, ARRAY_SIZE(buf) ); break;
    case RES_MEMORY: format_bytes( buf, ARRAY_SIZE(buf), (double)sysinfo.mem_total + sysinfo.mem_hw_reserved, FALSE ); break;
    case RES_DISK: lstrcpynW( buf, disk->model, ARRAY_SIZE(buf) ); break;
    case RES_NET: lstrcpynW( buf, sysinfo.net_name, ARRAY_SIZE(buf) ); break;
    case RES_GPU: lstrcpynW( buf, gpu->name, ARRAY_SIZE(buf) ); break;
    }
    if (buf[0] && right - title_right > scale( 60 ))
    {
        /* on the right of the title, cut where the two would meet */
        RECT rc = { title_right, y + scale( 12 ), right, y + scale( 12 ) + text_height( hdc, title_font ) };
        SelectObject( hdc, title_font );
        SetTextColor( hdc, RGB(0, 0, 0) );
        DrawTextW( hdc, buf, -1, &rc, DT_SINGLELINE | DT_NOPREFIX | DT_RIGHT | DT_END_ELLIPSIS );
    }
    y += text_height( hdc, big_font ) + scale( 10 );

    /* the graph's own labels */
    switch (current.res)
    {
    case RES_CPU:
    case RES_GPU:
        lstrcpynW( buf, load_string( IDS_PERF_UTIL ), ARRAY_SIZE(buf) ); wcscpy( buf2, L"100%" ); break;
    case RES_MEMORY:
        lstrcpynW( buf, load_string( IDS_PERF_MEM_USAGE ), ARRAY_SIZE(buf) );
        format_bytes( buf2, ARRAY_SIZE(buf2), sysinfo.mem_total, FALSE );
        break;
    case RES_DISK: lstrcpynW( buf, load_string( IDS_PERF_ACTIVE_TIME ), ARRAY_SIZE(buf) ); wcscpy( buf2, L"100%" ); break;
    case RES_NET:
        lstrcpynW( buf, load_string( IDS_PERF_THROUGHPUT ), ARRAY_SIZE(buf) );
        format_bits( buf2, ARRAY_SIZE(buf2), net_scale() );
        break;
    }
    text_at( hdc, small_font, RGB(102, 102, 102), x, y, buf, 0 );
    text_at( hdc, small_font, RGB(102, 102, 102), right, y, buf2, DT_RIGHT );
    y += lh + scale( 2 );

    below = current.res == RES_DISK || current.res == RES_MEMORY || gpu_memory ? 290 : 230;
    gh = max( scale( 100 ), client->bottom - y - scale( below ) );
    SetRect( &gr, x, y, right, y + gh );
    if (current.res == RES_CPU && logical_view && cpu_count > 1)
    {
        /* one graph per logical processor, in a grid as square as fits */
        UINT cols = (UINT)ceil( sqrt( (double)cpu_count * (gr.right - gr.left) / max( 1, gr.bottom - gr.top ) ) );
        UINT rows = (cpu_count + cols - 1) / cols;
        int cw = (gr.right - gr.left + scale( 4 )) / cols, ch = (gr.bottom - gr.top + scale( 4 )) / rows;
        for (UINT i = 0; i < cpu_count; i++)
        {
            RECT c = { gr.left + (i % cols) * cw, gr.top + (i / cols) * ch, gr.left + (i % cols + 1) * cw - scale( 4 ),
                       gr.top + (i / cols + 1) * ch - scale( 4 ) };
            draw_graph( hdc, &c, &current, value_of, 1, TRUE, i );
        }
    }
    else draw_graph( hdc, &gr, &current, value_of, scale_of( &current ), TRUE, -1 );
    if (current.res == RES_NET)
    {
        /* the sent part, dashed over the whole, as Windows draws it */
        HPEN dash = CreatePen( PS_DOT, 1, pal->line );
        double m = net_scale();
        POINT pts[HISTORY];
        UINT n = 0;
        for (UINT i = 0; i < HISTORY; i++)
        {
            const struct sample *ss = data_back( HISTORY - 1 - i );
            if (!ss) continue;
            pts[n].x = gr.left + (int)((double)(gr.right - gr.left) * i / (HISTORY - 1));
            pts[n].y = gr.bottom - 1 - (int)(min( 1.0, value_send( &current, ss, m ) ) * (gr.bottom - gr.top - 2));
            n++;
        }
        old = SelectObject( hdc, dash );
        if (n >= 2) Polyline( hdc, pts, n );
        SelectObject( hdc, old );
        DeleteObject( dash );
    }
    y = gr.bottom + scale( 2 );
    text_at( hdc, small_font, RGB(102, 102, 102), x, y, load_string( IDS_PERF_60S ), 0 );
    text_at( hdc, small_font, RGB(102, 102, 102), right, y, L"0", DT_RIGHT );
    y += lh + scale( 8 );

    if (current.res == RES_MEMORY)
    {
        /* the composition of memory: what is used, and what is left */
        RECT bar = { x, y + lh + scale( 2 ), right, y + lh + scale( 2 ) + scale( 36 ) }, used = bar;
        text_at( hdc, small_font, RGB(102, 102, 102), x, y, load_string( IDS_PERF_MEM_COMPOSITION ), 0 );
        fill( hdc, &bar, RGB(255, 255, 255) );
        used.right = bar.left + (LONG)((double)(bar.right - bar.left) * (sysinfo.mem_total ? s->mem_used / sysinfo.mem_total : 0));
        fill( hdc, &used, pal->fill );
        if (sysinfo.mem_cached)
        {
            RECT cached = used;
            cached.left = used.right;
            cached.right = min( bar.right, cached.left + (LONG)((double)(bar.right - bar.left) * sysinfo.mem_cached / sysinfo.mem_total) );
            fill( hdc, &cached, RGB(250, 248, 250) );
        }
        frame( hdc, &bar, pal->border );
        if (used.right > bar.left)
        {
            HPEN p = CreatePen( PS_SOLID, 1, pal->border );
            old = SelectObject( hdc, p );
            MoveToEx( hdc, used.right, bar.top, NULL );
            LineTo( hdc, used.right, bar.bottom );
            SelectObject( hdc, old );
            DeleteObject( p );
        }
        y = bar.bottom + scale( 14 );
    }
    if (disk) y = draw_second_graph( hdc, x, y, right, IDS_PERF_TRANSFER, disk_transfer_max( current.index ), TRUE, value_read_write );
    if (gpu_memory) y = draw_second_graph( hdc, x, y, right, IDS_PERF_GPU_MEM_USAGE, gpu->mem_total, FALSE, value_gpu_memory );

    switch (current.res)
    {
    case RES_CPU:
        big[nb].label = IDS_PERF_UTILIZATION; swprintf( big[nb++].value, 64, L"%.0f%%", s->cpu );
        big[nb].label = IDS_PERF_SPEED; format_ghz( big[nb++].value, 64, sysinfo.cpu_mhz_now );
        big[nb].label = IDS_PERF_PROCESSES; swprintf( big[nb++].value, 64, L"%u", sysinfo.processes );
        big[nb].label = IDS_PERF_THREADS; swprintf( big[nb++].value, 64, L"%u", sysinfo.threads );
        big[nb].label = IDS_PERF_HANDLES; swprintf( big[nb++].value, 64, L"%u", sysinfo.handles );
        big[nb].label = IDS_PERF_UPTIME; format_uptime( big[nb++].value, 64 );
        facts[nf].label = IDS_PERF_BASE_SPEED; format_ghz( facts[nf++].value, 64, sysinfo.cpu_mhz_base );
        facts[nf].label = IDS_PERF_SOCKETS; swprintf( facts[nf++].value, 64, L"%u", sysinfo.sockets );
        facts[nf].label = IDS_PERF_CORES; swprintf( facts[nf++].value, 64, L"%u", sysinfo.cores );
        facts[nf].label = IDS_PERF_LOGICAL; swprintf( facts[nf++].value, 64, L"%u", sysinfo.logical );
        facts[nf].label = IDS_PERF_VIRTUALIZATION;
        lstrcpynW( facts[nf++].value, load_string( sysinfo.virtualization ? IDS_PERF_ENABLED : IDS_PERF_NOT_SUPPORTED ), 64 );
        if (sysinfo.l1) { facts[nf].label = IDS_PERF_L1; format_bytes( facts[nf++].value, 64, sysinfo.l1, FALSE ); }
        if (sysinfo.l2) { facts[nf].label = IDS_PERF_L2; format_bytes( facts[nf++].value, 64, sysinfo.l2, FALSE ); }
        if (sysinfo.l3) { facts[nf].label = IDS_PERF_L3; format_bytes( facts[nf++].value, 64, sysinfo.l3, FALSE ); }
        draw_stats( hdc, x, y, right, big, nb, 3, facts, nf );
        break;
    case RES_MEMORY:
        big[nb].label = IDS_PERF_IN_USE; format_bytes( big[nb++].value, 64, s->mem_used, FALSE );
        big[nb].label = IDS_PERF_AVAILABLE; format_bytes( big[nb++].value, 64, sysinfo.mem_avail, FALSE );
        big[nb].label = IDS_PERF_COMMITTED;
        format_bytes( buf, ARRAY_SIZE(buf), sysinfo.mem_commit, FALSE );
        format_bytes( buf2, ARRAY_SIZE(buf2), sysinfo.mem_commit_limit, FALSE );
        swprintf( big[nb++].value, 64, L"%s/%s", buf, buf2 );
        big[nb].label = IDS_PERF_CACHED; format_bytes( big[nb++].value, 64, sysinfo.mem_cached, FALSE );
        big[nb].label = IDS_PERF_PAGED; format_bytes( big[nb++].value, 64, sysinfo.mem_paged, FALSE );
        big[nb].label = IDS_PERF_NONPAGED; format_bytes( big[nb++].value, 64, sysinfo.mem_nonpaged, FALSE );
        if (sysinfo.mem_speed) { facts[nf].label = IDS_PERF_MEM_SPEED; swprintf( facts[nf++].value, 64, L"%u %s", sysinfo.mem_speed, load_string( IDS_UNIT_MHZ ) ); }
        if (sysinfo.mem_slots) { facts[nf].label = IDS_PERF_SLOTS; swprintf( facts[nf++].value, 64, load_string( IDS_PERF_OF_SLOTS ), sysinfo.mem_slots_used, sysinfo.mem_slots ); }
        if (sysinfo.mem_form[0]) { facts[nf].label = IDS_PERF_FORM; lstrcpynW( facts[nf++].value, sysinfo.mem_form, 64 ); }
        if (sysinfo.mem_hw_reserved) { facts[nf].label = IDS_PERF_HW_RESERVED; format_bytes( facts[nf++].value, 64, sysinfo.mem_hw_reserved, FALSE ); }
        draw_stats( hdc, x, y, right, big, nb, 2, facts, nf );
        break;
    case RES_DISK:
    {
        double response = s->disks[current.index].response;

        big[nb].label = IDS_PERF_ACTIVE_TIME; swprintf( big[nb++].value, 64, L"%.0f%%", s->disks[current.index].active );
        big[nb].label = IDS_PERF_RESPONSE;
        swprintf( big[nb].value, 64, response >= 10 || response == 0 ? L"%.0f %s" : L"%.1f %s", response, load_string( IDS_UNIT_MS ) );
        decimal_comma( big[nb++].value );
        big[nb].label = IDS_PERF_READ_SPEED; format_bytes( big[nb++].value, 64, s->disks[current.index].read, TRUE );
        big[nb].label = IDS_PERF_WRITE_SPEED; format_bytes( big[nb++].value, 64, s->disks[current.index].write, TRUE );
        if (disk->capacity) { facts[nf].label = IDS_PERF_CAPACITY; format_bytes( facts[nf++].value, 64, disk->capacity, FALSE ); }
        facts[nf].label = IDS_PERF_FORMATTED; format_bytes( facts[nf++].value, 64, disk->formatted, FALSE );
        facts[nf].label = IDS_PERF_SYSTEM_DISK; lstrcpynW( facts[nf++].value, load_string( disk->system ? IDS_PERF_YES : IDS_PERF_NO ), 64 );
        facts[nf].label = IDS_PERF_PAGE_FILE; lstrcpynW( facts[nf++].value, load_string( IDS_PERF_NO ), 64 );
        facts[nf].label = IDS_PERF_TYPE; lstrcpynW( facts[nf++].value, disk_kind( disk ), 64 );
        draw_stats( hdc, x, y, right, big, nb, 2, facts, nf );
        break;
    }
    case RES_NET:
        big[nb].label = IDS_PERF_SEND; format_bits( big[nb++].value, 64, s->net_send );
        big[nb].label = IDS_PERF_RECEIVE; format_bits( big[nb++].value, 64, s->net_recv );
        facts[nf].label = IDS_PERF_ADAPTER; lstrcpynW( facts[nf++].value, sysinfo.net_name, 64 );
        facts[nf].label = IDS_PERF_CONN_TYPE; lstrcpynW( facts[nf++].value, sysinfo.wifi ? L"802.11" : L"Ethernet", 64 );
        if (sysinfo.net_ipv4[0]) { facts[nf].label = IDS_PERF_IPV4; lstrcpynW( facts[nf++].value, sysinfo.net_ipv4, 64 ); }
        if (sysinfo.net_ipv6[0]) { facts[nf].label = IDS_PERF_IPV6; lstrcpynW( facts[nf++].value, sysinfo.net_ipv6, 64 ); }
        draw_stats( hdc, x, y, right, big, nb, 2, facts, nf );
        break;
    case RES_GPU:
    {
        unsigned int domain, bus, dev, fn;
        const struct sample *g = s;

        big[nb].label = IDS_PERF_UTILIZATION; swprintf( big[nb++].value, 64, L"%.0f%%", g->gpus[current.index].util );
        if (gpu_memory)
        {
            big[nb].label = IDS_PERF_GPU_MEM;
            format_bytes( buf, ARRAY_SIZE(buf), max( 0.0, g->gpus[current.index].mem_used ), FALSE );
            format_bytes( buf2, ARRAY_SIZE(buf2), gpu->mem_total, FALSE );
            swprintf( big[nb++].value, 64, L"%s/%s", buf, buf2 );
        }
        if (g->gpus[current.index].temperature != NO_VALUE)
        {
            big[nb].label = IDS_PERF_GPU_TEMP;
            swprintf( big[nb++].value, 64, L"%.0f °C", g->gpus[current.index].temperature );
        }
        if (gpu->driver[0]) { facts[nf].label = IDS_PERF_DRIVER; lstrcpynW( facts[nf++].value, gpu->driver, 64 ); }
        if (swscanf( gpu->pci, L"%x:%x:%x.%x", &domain, &bus, &dev, &fn ) == 4)
        {
            facts[nf].label = IDS_PERF_LOCATION;
            swprintf( facts[nf++].value, 64, load_string( IDS_PERF_PCI ), bus, dev, fn );
        }
        draw_stats( hdc, x, y, right, big, nb, 2, facts, nf );
        break;
    }
    }
}

static void paint( HWND hwnd )
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint( hwnd, &ps ), mem;
    RECT rc;
    HBITMAP bmp, oldbmp;

    GetClientRect( hwnd, &rc );
    mem = CreateCompatibleDC( hdc );
    bmp = CreateCompatibleBitmap( hdc, max( 1, rc.right ), max( 1, rc.bottom ) );
    oldbmp = SelectObject( mem, bmp );
    FillRect( mem, &rc, GetStockObject( WHITE_BRUSH ) );
    SetBkMode( mem, TRANSPARENT );
    nav_width = scale( 240 );
    check_current();
    draw_nav( mem, &rc );
    draw_page( mem, &rc );
    BitBlt( hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY );
    SelectObject( mem, oldbmp );
    DeleteObject( bmp );
    DeleteDC( mem );
    EndPaint( hwnd, &ps );
}

/* the tiles scroll when there are more than fit */
static void scroll_nav( HWND hwnd, int delta )
{
    struct item items[MAX_ITEMS];
    RECT rc;
    int total = scale( 16 ) + (int)list_items( items ) * tile_height(), limit;

    GetClientRect( hwnd, &rc );
    limit = max( 0, total - rc.bottom );
    nav_scroll = min( limit, max( 0, nav_scroll + delta ) );
    InvalidateRect( hwnd, NULL, FALSE );
}

static void show_current( HWND hwnd )
{
    struct item items[MAX_ITEMS];
    UINT n = list_items( items );
    RECT rc, item;

    GetClientRect( hwnd, &rc );
    for (UINT i = 0; i < n; i++)
    {
        if (!same_item( &items[i], &current )) continue;
        item = nav_item_rect( i );
        if (item.top < 0) scroll_nav( hwnd, item.top );
        else if (item.bottom > rc.bottom) scroll_nav( hwnd, item.bottom - rc.bottom );
    }
    InvalidateRect( hwnd, NULL, FALSE );
}

static BOOL nav_hit( int x, int y, struct item *hit )
{
    struct item items[MAX_ITEMS];
    UINT n = list_items( items );
    POINT pt = { x, y };

    for (UINT i = 0; i < n; i++)
    {
        RECT rc = nav_item_rect( i );
        if (PtInRect( &rc, pt )) { *hit = items[i]; return TRUE; }
    }
    return FALSE;
}

static LRESULT CALLBACK perf_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    switch (msg)
    {
    case WM_PAINT:
        paint( hwnd );
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN:
    {
        struct item hit;
        SetFocus( hwnd );
        if (nav_hit( GET_X_LPARAM( lparam ), GET_Y_LPARAM( lparam ), &hit ))
        {
            current = hit;
            show_current( hwnd );
        }
        return 0;
    }
    case WM_GETDLGCODE:
        /* the arrows go through the tiles, not to the next control */
        return DLGC_WANTARROWS;
    case WM_MOUSEWHEEL:
        scroll_nav( hwnd, -GET_WHEEL_DELTA_WPARAM( wparam ) * tile_height() / WHEEL_DELTA );
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_DOWN || wparam == VK_UP)
        {
            struct item items[MAX_ITEMS];
            UINT n = list_items( items ), i;

            for (i = 0; i < n; i++) if (same_item( &items[i], &current )) break;
            if (i == n) i = 0;
            else i = (i + (wparam == VK_DOWN ? 1 : n - 1)) % n;
            current = items[i];
            show_current( hwnd );
        }
        return 0;
    case WM_CONTEXTMENU:
    {
        HMENU popup = LoadMenuW( instance, MAKEINTRESOURCEW(IDR_POPUP) ), sub = GetSubMenu( popup, 4 );
        POINT pt = { GET_X_LPARAM( lparam ), GET_Y_LPARAM( lparam ) };
        UINT cmd;
        CheckMenuRadioItem( sub, IDM_CPU_OVERALL, IDM_CPU_LOGICAL, logical_view ? IDM_CPU_LOGICAL : IDM_CPU_OVERALL, MF_BYCOMMAND );
        if (current.res != RES_CPU) EnableMenuItem( sub, 0, MF_BYPOSITION | MF_GRAYED );
        cmd = TrackPopupMenu( sub, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL );
        DestroyMenu( popup );
        if (cmd == IDM_CPU_OVERALL || cmd == IDM_CPU_LOGICAL)
        {
            logical_view = cmd == IDM_CPU_LOGICAL;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        else if (cmd == IDM_COPY)
        {
            WCHAR text[512], name[128], summary[160], model[128] = L"";
            HGLOBAL mem;

            item_name( &current, name, ARRAY_SIZE(name) );
            item_summary( &current, summary, ARRAY_SIZE(summary) );
            if (current.res == RES_CPU) lstrcpynW( model, sysinfo.cpu_name, ARRAY_SIZE(model) );
            else item_subtitle( &current, model, ARRAY_SIZE(model) );
            swprintf( text, ARRAY_SIZE(text), L"%s\r\n%s\r\n%s\r\n", name, model, summary );
            if (OpenClipboard( hwnd ))
            {
                EmptyClipboard();
                if ((mem = GlobalAlloc( GMEM_MOVEABLE, (wcslen( text ) + 1) * sizeof(WCHAR) )))
                {
                    wcscpy( GlobalLock( mem ), text );
                    GlobalUnlock( mem );
                    SetClipboardData( CF_UNICODETEXT, mem );
                }
                CloseClipboard();
            }
        }
        return 0;
    }
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

void perf_create( HWND parent )
{
    WNDCLASSW wc = { CS_HREDRAW | CS_VREDRAW, perf_proc, 0, 0, instance };

    wc.hCursor = LoadCursorW( NULL, (LPCWSTR)IDC_ARROW );
    wc.lpszClassName = L"ArcticTaskPerformance";
    RegisterClassW( &wc );
    page = CreateWindowExW( 0, wc.lpszClassName, NULL, WS_CHILD | WS_TABSTOP, 0, 0, 0, 0, parent, NULL, instance, NULL );
    set_page_hwnd( IDS_TAB_PERFORMANCE, page );
}

void perf_refresh(void)
{
    check_current();
    InvalidateRect( page, NULL, FALSE );
}

void perf_resize( int w, int h )
{
    scroll_nav( page, 0 );
    InvalidateRect( page, NULL, FALSE );
}
