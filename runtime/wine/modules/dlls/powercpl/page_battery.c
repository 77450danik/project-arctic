/*
 * Power Options of the Control Panel: the batteries and graphics cards
 *
 * Arctic's page, in the Control Panel's look: the charge as a big battery
 * and its percentage, the state and the time left, what the machine draws
 * now; a graph of the last hour (the power policy samples the batteries
 * every ten seconds, docs/power.md); the batteries' health (design and full
 * capacity, wear, cycles); each graphics card with its role, whether it is
 * awake, what it draws, its temperature, load and memory, and the programs
 * on it; the processor's draw. All of it comes from powrprof.dll, whatever
 * the laptop has; what it does not say is not shown. Every two seconds again.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdlib.h>

#include "powercpl.h"
#include "winnls.h"
#include "commctrl.h"

#define VALUE_X        px(220)
#define ACCENT         RGB( 0x00, 0x78, 0xd7 )
#define GREEN          RGB( 0x10, 0x7c, 0x10 )
#define ORANGE         RGB( 0xf7, 0x63, 0x0c )
#define RED            RGB( 0xe8, 0x11, 0x23 )
#define GRID           RGB( 0xe6, 0xe6, 0xe6 )
#define AXIS_TEXT      RGB( 0x6d, 0x6d, 0x6d )

struct battery_data
{
    struct arctic_power_status status;
    struct arctic_gpu_list gpus;
    struct arctic_cpu_info cpu;
    struct arctic_power_history *history;
};

static struct battery_data *data_of( struct view *view )
{
    struct page_state *state = view_state( view );

    if (!state->data && (state->data = calloc( 1, sizeof(struct battery_data) )))
    {
        struct battery_data *data = state->data;
        HANDLE map = OpenFileMappingW( FILE_MAP_READ, FALSE, ARCTIC_POWER_HISTORY_NAME );

        if (map)
        {
            data->history = MapViewOfFile( map, FILE_MAP_READ, 0, 0, sizeof(*data->history) );
            CloseHandle( map );
        }
    }
    return state->data;
}

/* "12,4 Вт", "43,2 Вт·год": tenths with the decimal sign of the user */
static void format_tenths( UINT value_milli, UINT unit, WCHAR *text, size_t count )
{
    WCHAR sep[4] = L",";
    UINT tenths = (value_milli + 50) / 100;

    GetLocaleInfoW( LOCALE_USER_DEFAULT, LOCALE_SDECIMAL, sep, ARRAY_SIZE(sep) );
    swprintf( text, count, L"%u%s%u %s", tenths / 10, sep, tenths % 10, load_string( unit ) );
}

static const WCHAR *state_text( const struct arctic_power_status *s )
{
    switch (s->state)
    {
    case ARCTIC_BATTERY_CHARGING: return load_string( IDS_STATE_CHARGING );
    case ARCTIC_BATTERY_DISCHARGING: return load_string( IDS_STATE_DISCHARGING );
    case ARCTIC_BATTERY_FULL: return load_string( IDS_STATE_FULL );
    default: return load_string( s->ac_online ? IDS_STATE_NOT_CHARGING : IDS_STATE_DISCHARGING );
    }
}

static COLORREF charge_color( const struct arctic_power_status *s )
{
    if (s->ac_online) return ACCENT;
    if (s->percent <= 10) return RED;
    if (s->percent <= 20) return ORANGE;
    return GREEN;
}

/* the battery at the top: a body, its tip, the charge filling it */
static void paint_summary( struct view *view, HDC hdc, const RECT *rect )
{
    struct battery_data *data = data_of( view );
    const struct arctic_power_status *s = &data->status;
    RECT body = { rect->left, rect->top + px( 14 ), rect->left + px( 96 ), rect->top + px( 62 ) }, fill, tip, text;
    HBRUSH frame = CreateSolidBrush( RGB( 0x33, 0x33, 0x33 ) ), level;
    WCHAR buf[128];
    HGDIOBJ old;

    FrameRect( hdc, &body, frame );
    InflateRect( &body, -1, -1 );
    FrameRect( hdc, &body, frame );
    SetRect( &tip, body.right + 1, body.top + px( 14 ), body.right + px( 6 ), body.bottom - px( 14 ) );
    FillRect( hdc, &tip, frame );
    fill = body;
    InflateRect( &fill, -px( 4 ), -px( 4 ) );
    fill.right = fill.left + (fill.right - fill.left) * min( s->percent, 100 ) / 100;
    level = CreateSolidBrush( charge_color( s ) );
    if (s->percent != ARCTIC_UNKNOWN) FillRect( hdc, &fill, level );
    DeleteObject( level );
    DeleteObject( frame );

    /* the charge, big, and the state under it */
    old = SelectObject( hdc, view_font( STYLE_BIG ) );
    SetTextColor( hdc, RGB( 0, 0, 0 ) );
    SetRect( &text, rect->left + px( 120 ), rect->top, rect->left + px( 300 ), rect->top + px( 52 ) );
    if (s->percent != ARCTIC_UNKNOWN) swprintf( buf, ARRAY_SIZE(buf), L"%u %%", s->percent );
    else lstrcpyW( buf, L"—" );
    DrawTextW( hdc, buf, -1, &text, DT_SINGLELINE | DT_NOPREFIX | DT_BOTTOM );

    SelectObject( hdc, view_font( STYLE_BOLD ) );
    SetRect( &text, rect->left + px( 122 ), rect->top + px( 54 ), rect->right, rect->top + px( 72 ) );
    DrawTextW( hdc, state_text( s ), -1, &text, DT_SINGLELINE | DT_NOPREFIX );

    SelectObject( hdc, view_font( STYLE_BODY ) );
    SetTextColor( hdc, AXIS_TEXT );
    SetRect( &text, rect->left + px( 122 ), rect->top + px( 72 ), rect->right, rect->top + px( 90 ) );
    if (s->seconds_left != ARCTIC_UNKNOWN && s->seconds_left >= 60)
    {
        WCHAR time[64];
        format_timeout( s->seconds_left, time, ARRAY_SIZE(time) );
        lstrcpynW( buf, format_string( s->state == ARCTIC_BATTERY_CHARGING ? IDS_UNTIL_FULL : IDS_UNTIL_EMPTY, time ),
                   ARRAY_SIZE(buf) );
    }
    else lstrcpynW( buf, load_string( s->ac_online ? IDS_FROM_MAINS : IDS_FROM_BATTERY ), ARRAY_SIZE(buf) );
    DrawTextW( hdc, buf, -1, &text, DT_SINGLELINE | DT_NOPREFIX );

    /* what the machine draws, on the right */
    if (s->rate_mw != ARCTIC_UNKNOWN)
    {
        int right = rect->right;

        SelectObject( hdc, view_font( STYLE_BODY ) );
        SetRect( &text, right - px( 200 ), rect->top + px( 8 ), right, rect->top + px( 26 ) );
        DrawTextW( hdc, load_string( IDS_DRAW_NOW ), -1, &text, DT_SINGLELINE | DT_NOPREFIX | DT_RIGHT );
        SelectObject( hdc, view_font( STYLE_TITLE ) );
        SetTextColor( hdc, RGB( 0, 0, 0 ) );
        format_tenths( s->rate_mw, IDS_WATTS, buf, ARRAY_SIZE(buf) );
        SetRect( &text, right - px( 200 ), rect->top + px( 26 ), right, rect->top + px( 50 ) );
        DrawTextW( hdc, buf, -1, &text, DT_SINGLELINE | DT_NOPREFIX | DT_RIGHT );
    }
    SelectObject( hdc, old );
}

/* the last hour: what the batteries gave (or took), and the charge */
static void paint_graph( struct view *view, HDC hdc, const RECT *rect )
{
    struct battery_data *data = data_of( view );
    const struct arctic_power_history *h = data->history;
    RECT plot = { rect->left + px( 40 ), rect->top + px( 6 ), rect->right - px( 36 ), rect->bottom - px( 22 ) }, text;
    UINT max_mw = 5000, count = h ? min( h->count, ARCTIC_POWER_HISTORY_SIZE ) : 0;
    DWORD now = GetTickCount();
    HPEN grid = CreatePen( PS_SOLID, 1, GRID ), line = CreatePen( PS_SOLID, max( px( 2 ), 2 ), ACCENT );
    HPEN charge = CreatePen( PS_SOLID, 1, GREEN );
    HGDIOBJ old_pen, old_font;
    WCHAR buf[64];
    POINT *points;
    int n = 0;

    for (UINT i = 0; i < count; i++)
        if (h->samples[i].rate_mw != ARCTIC_UNKNOWN) max_mw = max( max_mw, h->samples[i].rate_mw );
    max_mw = (max_mw + 4999) / 5000 * 5000;

    old_pen = SelectObject( hdc, grid );
    old_font = SelectObject( hdc, view_font( STYLE_SMALL ) );
    SetTextColor( hdc, AXIS_TEXT );
    for (int i = 0; i <= 4; i++)
    {
        int y = plot.bottom - (plot.bottom - plot.top) * i / 4;
        MoveToEx( hdc, plot.left, y, NULL );
        LineTo( hdc, plot.right, y );
        SetRect( &text, rect->left, y - px( 8 ), plot.left - px( 4 ), y + px( 8 ) );
        swprintf( buf, ARRAY_SIZE(buf), L"%u", max_mw * i / 4 / 1000 );
        DrawTextW( hdc, buf, -1, &text, DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_NOPREFIX );
        SetRect( &text, plot.right + px( 4 ), y - px( 8 ), rect->right, y + px( 8 ) );
        swprintf( buf, ARRAY_SIZE(buf), L"%u%%", 25 * i );
        DrawTextW( hdc, buf, -1, &text, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_NOPREFIX );
    }
    SetRect( &text, plot.left, plot.bottom + px( 4 ), plot.right, rect->bottom );
    DrawTextW( hdc, load_string( IDS_LAST_HOUR ), -1, &text, DT_SINGLELINE | DT_LEFT | DT_NOPREFIX );
    SetRect( &text, plot.left, plot.top - px( 2 ), plot.left + px( 60 ), plot.top + px( 14 ) );
    DrawTextW( hdc, load_string( IDS_WATTS ), -1, &text, DT_SINGLELINE | DT_LEFT | DT_NOPREFIX );

    if (!count)
    {
        SetRect( &text, plot.left, plot.top, plot.right, plot.bottom );
        DrawTextW( hdc, load_string( IDS_GRAPH_EMPTY ), -1, &text, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX );
    }
    else if ((points = malloc( (count + 2) * sizeof(POINT) )))
    {
        UINT start = (h->next + ARCTIC_POWER_HISTORY_SIZE - count) % ARCTIC_POWER_HISTORY_SIZE;

        /* the draw, filled under its line */
        for (UINT k = 0; k < count; k++)
        {
            const typeof(h->samples[0]) *s = &h->samples[(start + k) % ARCTIC_POWER_HISTORY_SIZE];
            DWORD age = now - s->tick;
            if (s->rate_mw == ARCTIC_UNKNOWN || age > 3600000) continue;
            points[n].x = plot.right - (int)((plot.right - plot.left) * (LONGLONG)age / 3600000);
            points[n].y = plot.bottom - (int)((plot.bottom - plot.top) * (LONGLONG)min( s->rate_mw, max_mw ) / max_mw);
            n++;
        }
        if (n > 1)
        {
            HBRUSH fill = CreateSolidBrush( RGB( 0xcc, 0xe4, 0xf7 ) );
            HGDIOBJ old_brush = SelectObject( hdc, fill );
            HPEN none = GetStockObject( NULL_PEN );

            points[n].x = points[n - 1].x;
            points[n].y = plot.bottom;
            points[n + 1].x = points[0].x;
            points[n + 1].y = plot.bottom;
            SelectObject( hdc, none );
            Polygon( hdc, points, n + 2 );
            SelectObject( hdc, line );
            Polyline( hdc, points, n );
            SelectObject( hdc, old_brush );
            DeleteObject( fill );
        }
        /* the charge on the right axis */
        n = 0;
        for (UINT k = 0; k < count; k++)
        {
            const typeof(h->samples[0]) *s = &h->samples[(start + k) % ARCTIC_POWER_HISTORY_SIZE];
            DWORD age = now - s->tick;
            if (s->percent == ARCTIC_UNKNOWN || age > 3600000) continue;
            points[n].x = plot.right - (int)((plot.right - plot.left) * (LONGLONG)age / 3600000);
            points[n].y = plot.bottom - (plot.bottom - plot.top) * (int)min( s->percent, 100 ) / 100;
            n++;
        }
        if (n > 1)
        {
            SelectObject( hdc, charge );
            Polyline( hdc, points, n );
        }
        free( points );
    }
    SelectObject( hdc, old_pen );
    SelectObject( hdc, old_font );
    DeleteObject( grid );
    DeleteObject( line );
    DeleteObject( charge );
}

static void row( struct view *view, UINT label, const WCHAR *value )
{
    view_row_begin( view );
    view_text_at( view, STYLE_GRAY, load_string( label ), px( 4 ) );
    view_text_at( view, STYLE_BODY, value, VALUE_X );
    view_row_end( view );
    view_space( view, px( 4 ) );
}

static void go_graphics( struct view *view, UINT_PTR param )
{
    view_navigate( view, PAGE_GRAPHICS, NULL, FALSE );
}

/* the card's name as Windows shows it (powrprof: the adapter's DeviceDesc, else the PCI id database's) */
static void gpu_title( const struct arctic_gpu *gpu, WCHAR *name, DWORD count )
{
    lstrcpynW( name, gpu->name, count );
}

void battery_build( struct view *view )
{
    struct battery_data *data = data_of( view );
    const struct arctic_power_status *s;
    WCHAR buf[256];

    if (!data) return;
    ArcticPowerStatus( &data->status );
    ArcticGpuList( &data->gpus, TRUE );
    ArcticCpuInfo( &data->cpu );
    s = &data->status;

    nav_power_tasks( view );
    view_text( view, STYLE_TITLE, load_string( IDS_BATTERY_TITLE ) );
    view_text( view, STYLE_BODY, load_string( IDS_BATTERY_TEXT ) );
    view_space( view, px( 16 ) );

    if (s->battery_count)
    {
        view_paint_area( view, px( 96 ), paint_summary );
        view_space( view, px( 12 ) );
        view_group( view, load_string( IDS_LAST_HOUR ) );
        view_paint_area( view, px( 170 ), paint_graph );
        view_space( view, px( 16 ) );

        for (UINT i = 0; i < s->battery_count; i++)
        {
            const struct arctic_battery *b = &s->batteries[i];

            if (s->battery_count > 1) view_group( view, format_string( IDS_BATTERY_N, i + 1 ) );
            else view_group( view, load_string( IDS_HEALTH ) );
            view_space( view, px( 6 ) );
            if (b->manufacturer[0]) row( view, IDS_MANUFACTURER, b->manufacturer );
            if (b->model[0]) row( view, IDS_MODEL, b->model );
            if (b->chemistry[0]) row( view, IDS_CHEMISTRY, b->chemistry );
            if (b->design_mwh != ARCTIC_UNKNOWN)
            {
                format_tenths( b->design_mwh, IDS_MWH, buf, ARRAY_SIZE(buf) );
                row( view, IDS_DESIGN_CAPACITY, buf );
            }
            if (b->full_mwh != ARCTIC_UNKNOWN)
            {
                format_tenths( b->full_mwh, IDS_MWH, buf, ARRAY_SIZE(buf) );
                row( view, IDS_FULL_CAPACITY, buf );
            }
            if (b->full_mwh != ARCTIC_UNKNOWN && b->design_mwh != ARCTIC_UNKNOWN && b->design_mwh)
            {
                UINT wear = b->full_mwh >= b->design_mwh ? 0 : 100 - b->full_mwh * 100 / b->design_mwh;
                swprintf( buf, ARRAY_SIZE(buf), L"%u %%", wear );
                row( view, IDS_WEAR, buf );
            }
            if (b->cycles != ARCTIC_UNKNOWN)
            {
                if (b->cycles) swprintf( buf, ARRAY_SIZE(buf), L"%u", b->cycles );
                else lstrcpynW( buf, load_string( IDS_NOT_COUNTED ), ARRAY_SIZE(buf) );
                row( view, IDS_CYCLES, buf );
            }
            if (b->voltage_mv != ARCTIC_UNKNOWN)
            {
                WCHAR sep[4] = L",";
                GetLocaleInfoW( LOCALE_USER_DEFAULT, LOCALE_SDECIMAL, sep, ARRAY_SIZE(sep) );
                swprintf( buf, ARRAY_SIZE(buf), L"%u%s%02u В", b->voltage_mv / 1000, sep, b->voltage_mv % 1000 / 10 );
                row( view, IDS_VOLTAGE, buf );
            }
            view_space( view, px( 12 ) );
        }
    }
    else
    {
        view_text( view, STYLE_BODY, load_string( IDS_NO_BATTERY ) );
        view_space( view, px( 16 ) );
    }

    view_group( view, load_string( IDS_GPUS ) );
    view_space( view, px( 6 ) );
    for (UINT i = 0; i < data->gpus.count; i++)
    {
        const struct arctic_gpu *g = &data->gpus.gpus[i];
        WCHAR name[256];

        gpu_title( g, name, ARRAY_SIZE(name) );
        swprintf( buf, ARRAY_SIZE(buf), L"%s — %s", name,
                  load_string( data->gpus.count < 2 ? IDS_GPU_ONLY : g->integrated ? IDS_GPU_POWER_SAVING : IDS_GPU_HIGH_PERFORMANCE ) );
        view_text( view, STYLE_BOLD, buf );
        view_space( view, px( 4 ) );
        row( view, IDS_POWER_SOURCE, load_string( g->active ? IDS_GPU_ACTIVE : IDS_GPU_ASLEEP ) );
        if (g->power_mw != ARCTIC_UNKNOWN)
        {
            format_tenths( g->power_mw, IDS_WATTS, buf, ARRAY_SIZE(buf) );
            row( view, IDS_GPU_DRAW, buf );
        }
        if (g->temperature_mc != ARCTIC_UNKNOWN)
        {
            swprintf( buf, ARRAY_SIZE(buf), L"%u °C", (g->temperature_mc + 500) / 1000 );
            row( view, IDS_GPU_TEMPERATURE, buf );
        }
        if (g->busy_percent != ARCTIC_UNKNOWN)
        {
            swprintf( buf, ARRAY_SIZE(buf), L"%u %%", g->busy_percent );
            row( view, IDS_GPU_LOAD, buf );
        }
        if (g->memory_total_mb != ARCTIC_UNKNOWN && g->memory_used_mb != ARCTIC_UNKNOWN)
        {
            swprintf( buf, ARRAY_SIZE(buf), L"%u / %u МБ", g->memory_used_mb, g->memory_total_mb );
            row( view, IDS_GPU_MEMORY, buf );
        }
        view_text_at( view, STYLE_GRAY, load_string( IDS_GPU_PROGRAMS ), px( 4 ) );
        if (!g->user_count) view_text_at( view, STYLE_BODY, load_string( IDS_GPU_NO_PROGRAMS ), px( 20 ) );
        for (UINT k = 0; k < g->user_count; k++) view_text_at( view, STYLE_BODY, g->users[k].image, px( 20 ) );
        view_space( view, px( 14 ) );
    }
    view_link( view, load_string( IDS_GRAPHICS_TITLE ), go_graphics, 0, 0 );
    view_space( view, px( 16 ) );

    if (data->cpu.package_power_mw != ARCTIC_UNKNOWN)
    {
        view_group( view, load_string( IDS_PROCESSOR ) );
        view_space( view, px( 6 ) );
        format_tenths( data->cpu.package_power_mw, IDS_WATTS, buf, ARRAY_SIZE(buf) );
        row( view, IDS_GPU_DRAW, buf );
    }
    view_set_timer( view, 2000 );
}

void battery_timer( struct view *view )
{
    view_rebuild( view );
}

BOOL battery_command( struct view *view, UINT id, UINT code, HWND control )
{
    return FALSE;
}
