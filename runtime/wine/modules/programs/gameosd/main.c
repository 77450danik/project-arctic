/*
 * The Games overlay (gameosd.exe, docs/games.md)
 *
 * What RivaTuner's on-screen display shows, over a game: its frames a
 * second and frame time (dwm counts them while game mode is on), the
 * processor's and the graphics card's load, clocks, temperatures, power and
 * memory (powrprof's ArcticSensors), and the time. The Games panel chooses
 * what, where and how big, and the keys: one shows or hides it, one opens
 * the panel.
 *
 * A layered window that clicks go through, topmost, never active: dwm
 * composes the game under it (a click-through overlay keeps the game its
 * content, and game mode on). It reads anything only while it shows; with
 * the overlay off it only waits for its keys, and NVIDIA's library is let go
 * whenever it hides (an open one keeps the card awake).
 *
 * Started at logon (HKLM\...\Run) and by the panel.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "winnls.h"
#include "commctrl.h"
#include "wine/arctic_power.h"
#include "wine/arctic_games.h"

#define TIMER_WATCH    1       /* is a game there: once a second */
#define TIMER_UPDATE   2       /* the readings, twice a second, while shown */
#define TIMER_FRAMES   3       /* the game's frames, ten times a second, while shown over one */
#define HOTKEY_OVERLAY 1
#define HOTKEY_PANEL   2

#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT   0x4000  /* held down, the key does not repeat its WM_HOTKEY */
#endif

#define BACKGROUND     0x101010
#define BACKGROUND_A   0xb4
#define COLOR_LABEL    RGB( 0x9a, 0xa4, 0xb0 )
#define COLOR_VALUE    RGB( 0xff, 0xff, 0xff )
#define COLOR_UNIT     RGB( 0x9a, 0xa4, 0xb0 )
#define COLOR_HOT      RGB( 0xff, 0x8a, 0x3d )   /* 85 °C and over */
#define COLOR_GRAPH    RGB( 0x6e, 0xd0, 0x8c )
#define COLOR_GRID     RGB( 0x50, 0x56, 0x5e )

/* dwm counts a game's frames only while this is there (docs/games.md) */
#define OVERLAY_FILE   L"\\\\?\\unix\\run\\arctic\\power\\overlay"
#define STATS_FILE     L"\\\\?\\unix\\run\\arctic\\power\\game-stats"
#define HISTORY        512     /* frame times kept for the graph */

static HWND window;
static struct arctic_games settings;
static struct arctic_sensors sensors;
static BOOL have_sensors, shown, game;
static int forced;            /* 1 shown, -1 hidden by its key; 0 as the settings say */
static WCHAR decimal[4] = L",";
static UINT32 history[HISTORY];  /* µs between frames, a ring */
static UINT history_next, history_count, stats_seq;

static int scaled( int n )
{
    return MulDiv( n, GetDpiForSystem() * max( settings.overlay_scale, 50 ), 96 * 100 );
}

static BOOL game_running(void)
{
    return GetFileAttributesW( ARCTIC_GAME_FILE ) != INVALID_FILE_ATTRIBUTES;
}

/**********************************************************************
 *          What it shows
 */

struct cell
{
    WCHAR value[32];
    const WCHAR *unit;
    const WCHAR *widest;      /* the cell keeps the width of this: values do not shift */
    COLORREF color;
};

struct row
{
    const WCHAR *label;
    struct cell cells[6];
    int count;
};

static struct cell *add_cell( struct row *row, const WCHAR *widest, const WCHAR *unit )
{
    struct cell *cell = &row->cells[row->count++];

    cell->value[0] = 0;
    cell->unit = unit;
    cell->widest = widest;
    cell->color = COLOR_VALUE;
    return cell;
}

static void number( struct cell *cell, UINT value )
{
    if (value == ARCTIC_UNKNOWN) lstrcpyW( cell->value, L"—" );
    else swprintf( cell->value, ARRAY_SIZE(cell->value), L"%u", value );
}

static void tenths( struct cell *cell, UINT value_tenths )
{
    if (value_tenths == ARCTIC_UNKNOWN) lstrcpyW( cell->value, L"—" );
    else swprintf( cell->value, ARRAY_SIZE(cell->value), L"%u%s%u", value_tenths / 10, decimal, value_tenths % 10 );
}

static void temperature( struct cell *cell, UINT mc )
{
    number( cell, mc == ARCTIC_UNKNOWN ? mc : (mc + 500) / 1000 );
    if (mc != ARCTIC_UNKNOWN && mc >= 85000) cell->color = COLOR_HOT;
}

static void memory_cell( struct cell *cell, UINT used_mb, UINT total_mb )
{
    if (used_mb == ARCTIC_UNKNOWN || total_mb == ARCTIC_UNKNOWN) lstrcpyW( cell->value, L"—" );
    else swprintf( cell->value, ARRAY_SIZE(cell->value), L"%u%s%u/%u%s%u", used_mb * 10 / 1024 / 10, decimal,
                   used_mb * 10 / 1024 % 10, total_mb * 10 / 1024 / 10, decimal, total_mb * 10 / 1024 % 10 );
}

static int build_rows( struct row *rows )
{
    const struct arctic_sensors *s = &sensors;
    int items = settings.overlay_items, count = 0;
    struct row *row;

    if (items & (OSD_FPS | OSD_FRAME_TIME))
    {
        row = &rows[count++];
        row->label = L"FPS";
        row->count = 0;
        if (items & OSD_FPS)
        {
            struct cell *cell = add_cell( row, L"000", L"" );
            number( cell, s->fps_tenths == ARCTIC_UNKNOWN ? ARCTIC_UNKNOWN : (s->fps_tenths + 5) / 10 );
        }
        if (items & OSD_FRAME_TIME)
        {
            struct cell *cell = add_cell( row, L"00,0", L"мс" );
            tenths( cell, s->frame_us == ARCTIC_UNKNOWN ? ARCTIC_UNKNOWN : (s->frame_us + 50) / 100 );
            cell = add_cell( row, L"000,0", L"мс макс" );
            tenths( cell, s->frame_worst_us == ARCTIC_UNKNOWN ? ARCTIC_UNKNOWN : (s->frame_worst_us + 50) / 100 );
        }
    }
    if (items & (OSD_CPU_LOAD | OSD_CPU_MHZ | OSD_CPU_TEMP | OSD_CPU_POWER | OSD_CPU_VOLTAGE))
    {
        row = &rows[count++];
        row->label = L"ЦП";
        row->count = 0;
        if (items & OSD_CPU_LOAD) number( add_cell( row, L"100", L"%" ), s->cpu_load_percent );
        if (items & OSD_CPU_MHZ) number( add_cell( row, L"0000", L"МГц" ), s->cpu_mhz );
        if (items & OSD_CPU_TEMP) temperature( add_cell( row, L"100", L"°C" ), s->cpu_temperature_mc );
        if (items & OSD_CPU_POWER)
            number( add_cell( row, L"000", L"Вт" ), s->cpu_power_mw == ARCTIC_UNKNOWN ? ARCTIC_UNKNOWN : (s->cpu_power_mw + 500) / 1000 );
        if (items & OSD_CPU_VOLTAGE)
        {
            struct cell *cell = add_cell( row, L"0,000", L"В" );

            if (s->cpu_voltage_mv == ARCTIC_UNKNOWN) lstrcpyW( cell->value, L"—" );
            else swprintf( cell->value, ARRAY_SIZE(cell->value), L"%u%s%03u", s->cpu_voltage_mv / 1000, decimal,
                           s->cpu_voltage_mv % 1000 );
        }
    }
    if (items & (OSD_GPU_LOAD | OSD_GPU_MHZ | OSD_GPU_TEMP | OSD_GPU_MEMORY | OSD_GPU_POWER))
    {
        row = &rows[count++];
        row->label = L"ГП";
        row->count = 0;
        if (items & OSD_GPU_LOAD) number( add_cell( row, L"100", L"%" ), s->gpu_load_percent );
        if (items & OSD_GPU_MHZ) number( add_cell( row, L"0000", L"МГц" ), s->gpu_mhz );
        if (items & OSD_GPU_TEMP) temperature( add_cell( row, L"100", L"°C" ), s->gpu_temperature_mc );
        if (items & OSD_GPU_MEMORY) memory_cell( add_cell( row, L"00,0/00,0", L"ГБ" ), s->gpu_memory_used_mb, s->gpu_memory_total_mb );
        if (items & OSD_GPU_POWER)
            number( add_cell( row, L"000", L"Вт" ), s->gpu_power_mw == ARCTIC_UNKNOWN ? ARCTIC_UNKNOWN : (s->gpu_power_mw + 500) / 1000 );
    }
    if (items & OSD_RAM)
    {
        row = &rows[count++];
        row->label = L"ОЗП";
        row->count = 0;
        memory_cell( add_cell( row, L"000,0/000,0", L"ГБ" ), s->ram_used_mb, s->ram_total_mb );
    }

    /* what this machine does not tell, or no game now, is left out */
    {
        int kept = 0;

        for (int r = 0; r < count; r++)
        {
            int cells = 0;

            for (int c = 0; c < rows[r].count; c++)
                if (wcscmp( rows[r].cells[c].value, L"—" )) rows[r].cells[cells++] = rows[r].cells[c];
            rows[r].count = cells;
            if (cells) rows[kept++] = rows[r];
        }
        count = kept;
    }
    return count;
}

/**********************************************************************
 *          Drawing: GDI's antialiased text, given an alpha of its own
 */

static HFONT font, unit_font;
static int font_scale;

static void make_fonts(void)
{
    NONCLIENTMETRICSW metrics = { sizeof(metrics) };
    LOGFONTW lf;

    if (font && font_scale == settings.overlay_scale) return;
    if (font) DeleteObject( font );
    if (unit_font) DeleteObject( unit_font );
    SystemParametersInfoW( SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0 );
    lf = metrics.lfMessageFont;
    lf.lfHeight = -scaled( 15 );
    lf.lfWeight = FW_SEMIBOLD;
    lf.lfQuality = ANTIALIASED_QUALITY;   /* grey levels: the coverage the alpha is made of */
    font = CreateFontIndirectW( &lf );
    lf.lfHeight = -scaled( 11 );
    lf.lfWeight = FW_NORMAL;
    unit_font = CreateFontIndirectW( &lf );
    font_scale = settings.overlay_scale;
}

static int text_width( HDC hdc, HFONT f, const WCHAR *text )
{
    SIZE size = { 0 };

    SelectObject( hdc, f );
    GetTextExtentPoint32W( hdc, text, lstrlenW( text ), &size );
    return size.cx;
}

/* each text twice: in its colour into one bitmap, in white into another,
 * whose grey is how much of the pixel the text covers */
static void draw_text( HDC color_dc, HDC mask_dc, HFONT f, const WCHAR *text, COLORREF color, RECT *rect, UINT flags )
{
    SelectObject( color_dc, f );
    SetTextColor( color_dc, color );
    DrawTextW( color_dc, text, -1, rect, flags | DT_SINGLELINE | DT_NOPREFIX );
    SelectObject( mask_dc, f );
    SetTextColor( mask_dc, RGB( 255, 255, 255 ) );
    DrawTextW( mask_dc, text, -1, rect, flags | DT_SINGLELINE | DT_NOPREFIX );
}

static HBITMAP make_dib( HDC hdc, int width, int height, UINT32 **bits )
{
    BITMAPINFO info = { { sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB } };

    return CreateDIBSection( hdc, &info, DIB_RGB_COLORS, (void **)bits, NULL, 0 );
}

/* RivaTuner's graph: each frame's time, newest on the right, two pixels a
 * frame; the scale grows with the longest of them, 20 ms at least, and
 * says what its top is */
static void draw_graph( HDC color_dc, HDC mask_dc, int x, int y, int width, int height )
{
    UINT step = max( 1, scaled( 2 ) ), count = min( history_count, (UINT)width / step + 1 ), top_us = 20000;
    POINT points[HISTORY];
    HPEN pen, grid, old_color, old_mask;
    WCHAR text[16];
    RECT rect;

    for (UINT i = 0; i < count; i++) top_us = max( top_us, history[(history_next + HISTORY - 1 - i) % HISTORY] );
    top_us = (top_us + 9999) / 10000 * 10000;   /* whole tens of milliseconds */
    for (UINT i = 0; i < count; i++)
    {
        UINT us = history[(history_next + HISTORY - 1 - i) % HISTORY];

        points[count - 1 - i].x = x + width - 1 - i * step;
        points[count - 1 - i].y = y + height - 1 - MulDiv( min( us, top_us ), height - 1, top_us );
    }

    /* the floor and the middle of the scale, faint */
    grid = CreatePen( PS_SOLID, 1, COLOR_GRID );
    old_color = SelectObject( color_dc, grid );
    old_mask = SelectObject( mask_dc, GetStockObject( WHITE_PEN ) );
    MoveToEx( color_dc, x, y + height - 1, NULL );
    LineTo( color_dc, x + width, y + height - 1 );
    MoveToEx( color_dc, x, y + height / 2, NULL );
    LineTo( color_dc, x + width, y + height / 2 );
    /* the mask makes them half there */
    {
        HPEN half = CreatePen( PS_SOLID, 1, RGB( 0x60, 0x60, 0x60 ) );
        SelectObject( mask_dc, half );
        MoveToEx( mask_dc, x, y + height - 1, NULL );
        LineTo( mask_dc, x + width, y + height - 1 );
        MoveToEx( mask_dc, x, y + height / 2, NULL );
        LineTo( mask_dc, x + width, y + height / 2 );
        SelectObject( mask_dc, GetStockObject( WHITE_PEN ) );
        DeleteObject( half );
    }

    pen = CreatePen( PS_SOLID, max( 1, scaled( 3 ) / 2 ), COLOR_GRAPH );
    SelectObject( color_dc, pen );
    {
        HPEN white = CreatePen( PS_SOLID, max( 1, scaled( 3 ) / 2 ), RGB( 255, 255, 255 ) );
        SelectObject( mask_dc, white );
        Polyline( color_dc, points, count );
        Polyline( mask_dc, points, count );
        SelectObject( mask_dc, GetStockObject( WHITE_PEN ) );
        DeleteObject( white );
    }
    SelectObject( color_dc, old_color );
    SelectObject( mask_dc, old_mask );
    DeleteObject( pen );
    DeleteObject( grid );

    swprintf( text, ARRAY_SIZE(text), L"%u мс", top_us / 1000 );
    SetRect( &rect, x, y, x + width, y + scaled( 14 ) );
    draw_text( color_dc, mask_dc, unit_font, text, COLOR_UNIT, &rect, DT_LEFT | DT_TOP );
}

static void paint(void)
{
    struct row rows[8];
    int count = build_rows( rows ), pad = scaled( 8 ), gap = scaled( 10 ), line = scaled( 21 ), label_width = 0;
    int widths[8][6], width = 0, height, radius = scaled( 6 ), graph;
    HDC screen = GetDC( NULL ), color_dc = CreateCompatibleDC( screen ), mask_dc = CreateCompatibleDC( screen );
    HBITMAP color_bitmap, mask_bitmap;
    UINT32 *color_bits, *mask_bits;
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    MONITORINFO monitor = { sizeof(monitor) };
    POINT pos, origin = { 0, 0 };
    SIZE size;

    make_fonts();
    for (int r = 0; r < count; r++)
    {
        int w = 0;

        label_width = max( label_width, text_width( color_dc, font, rows[r].label ) );
        for (int c = 0; c < rows[r].count; c++)
        {
            const struct cell *cell = &rows[r].cells[c];

            widths[r][c] = max( text_width( color_dc, font, cell->widest ), text_width( color_dc, font, cell->value ) );
            w += widths[r][c] + (cell->unit[0] ? scaled( 3 ) + text_width( color_dc, unit_font, cell->unit ) : 0) + gap;
        }
        width = max( width, w );
    }
    graph = (settings.overlay_items & OSD_FRAME_GRAPH) && game && history_count > 1 ? scaled( 46 ) : 0;
    if (!count && !graph)
    {
        ShowWindow( window, SW_HIDE );   /* nothing to show yet */
        goto done;
    }
    /* each cell counted a gap after it: the one after the label instead */
    width += pad * 2 + label_width;
    if (graph) width = max( width, scaled( 240 ) );
    height = pad * 2 + count * line + (graph ? graph + (count ? scaled( 6 ) : 0) : 0);

    color_bitmap = make_dib( screen, width, height, &color_bits );
    mask_bitmap = make_dib( screen, width, height, &mask_bits );
    if (!color_bitmap || !mask_bitmap)
    {
        if (color_bitmap) DeleteObject( color_bitmap );
        if (mask_bitmap) DeleteObject( mask_bitmap );
        goto done;
    }
    SelectObject( color_dc, color_bitmap );
    SelectObject( mask_dc, mask_bitmap );
    memset( color_bits, 0, width * height * 4 );
    memset( mask_bits, 0, width * height * 4 );
    SetBkMode( color_dc, TRANSPARENT );
    SetBkMode( mask_dc, TRANSPARENT );

    for (int r = 0; r < count; r++)
    {
        int x = pad + label_width + gap, top = pad + r * line;
        RECT rect = { pad, top, pad + label_width, top + line };

        draw_text( color_dc, mask_dc, font, rows[r].label, COLOR_LABEL, &rect, DT_LEFT | DT_VCENTER );
        for (int c = 0; c < rows[r].count; c++)
        {
            const struct cell *cell = &rows[r].cells[c];

            SetRect( &rect, x, top, x + widths[r][c], top + line );
            draw_text( color_dc, mask_dc, font, cell->value, cell->color, &rect, DT_RIGHT | DT_VCENTER );
            x += widths[r][c];
            if (cell->unit[0])
            {
                int w = text_width( color_dc, unit_font, cell->unit );

                /* the unit on the number's baseline, near enough */
                SetRect( &rect, x + scaled( 3 ), top + scaled( 2 ), x + scaled( 3 ) + w, top + line );
                draw_text( color_dc, mask_dc, unit_font, cell->unit, COLOR_UNIT, &rect, DT_LEFT | DT_VCENTER );
                x += scaled( 3 ) + w;
            }
            x += gap;
        }
    }
    if (graph) draw_graph( color_dc, mask_dc, pad, height - pad - graph, width - 2 * pad, graph );
    GdiFlush();

    /* out = background under what the text leaves, plus the text: premultiplied */
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++)
        {
            UINT32 *out = &color_bits[y * width + x];
            UINT cover = mask_bits[y * width + x] & 0xff, bg_a = BACKGROUND_A, a, rgb[3];
            int dx = x < radius ? radius - x : x >= width - radius ? x - (width - radius - 1) : 0;
            int dy = y < radius ? radius - y : y >= height - radius ? y - (height - radius - 1) : 0;

            if (dx && dy && dx * dx + dy * dy > radius * radius) bg_a = 0;   /* the rounded corners */
            a = bg_a + (255 - bg_a) * cover / 255;
            for (int k = 0; k < 3; k++)
            {
                UINT bg = ((BACKGROUND >> (8 * k)) & 0xff) * bg_a / 255;
                rgb[k] = min( 255, bg * (255 - cover) / 255 + ((*out >> (8 * k)) & 0xff) );
            }
            *out = (a << 24) | (rgb[2] << 16) | (rgb[1] << 8) | rgb[0];
        }

    /* the corner of the monitor the game is on */
    GetMonitorInfoW( MonitorFromWindow( GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY ), &monitor );
    {
        int margin = scaled( 10 );
        RECT m = monitor.rcMonitor;

        pos.x = settings.overlay_corner == OSD_TOP_RIGHT || settings.overlay_corner == OSD_BOTTOM_RIGHT
                ? m.right - margin - width : m.left + margin;
        pos.y = settings.overlay_corner == OSD_BOTTOM_LEFT || settings.overlay_corner == OSD_BOTTOM_RIGHT
                ? m.bottom - margin - height : m.top + margin;
    }
    size.cx = width;
    size.cy = height;
    UpdateLayeredWindow( window, screen, &pos, &size, color_dc, &origin, 0, &blend, ULW_ALPHA );
    /* over a game that made itself topmost too */
    SetWindowPos( window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE );
    if (!IsWindowVisible( window )) ShowWindow( window, SW_SHOWNOACTIVATE );

    DeleteDC( mask_dc );
    mask_dc = NULL;
    DeleteObject( mask_bitmap );
    DeleteDC( color_dc );
    color_dc = NULL;
    DeleteObject( color_bitmap );
done:
    if (mask_dc) DeleteDC( mask_dc );
    if (color_dc) DeleteDC( color_dc );
    ReleaseDC( NULL, screen );
}

/**********************************************************************
 *          When it shows
 */

/* dwm's file: the half second's numbers, then the frames since its last write */
static void read_frames(void)
{
    char text[4096], *p, *end;
    unsigned int fps, avg, worst, seq, n;
    DWORD done = 0;
    HANDLE file;

    file = CreateFileW( STATS_FILE, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL );
    if (file == INVALID_HANDLE_VALUE) return;
    ReadFile( file, text, sizeof(text) - 1, &done, NULL );
    CloseHandle( file );
    text[done] = 0;
    if (sscanf( text, "%u %u %u %u %u", &fps, &avg, &worst, &seq, &n ) != 5) return;
    sensors.fps_tenths = fps;
    sensors.frame_us = avg;
    sensors.frame_worst_us = worst;
    if (seq == stats_seq) return;
    stats_seq = seq;
    if (!(p = strchr( text, '\n' ))) return;
    strtoul( p + 1, &p, 10 );   /* the count of writes */
    strtoul( p, &p, 10 );       /* ... and of the frames that follow */
    for (unsigned int i = 0; i < n; i++)
    {
        unsigned long us = strtoul( p, &end, 10 );

        if (end == p) break;
        p = end;
        history[history_next] = us;
        history_next = (history_next + 1) % HISTORY;
        if (history_count < HISTORY) history_count++;
    }
}

static void update(void)
{
    have_sensors = ArcticSensors( &sensors );
    if (!have_sensors) memset( &sensors, 0xff, sizeof(sensors) );
    if (game) read_frames();
    paint();
}

/* dwm counts the game's frames while the overlay shows */
static void want_frames( BOOL want )
{
    HANDLE file;

    if (!want)
    {
        DeleteFileW( OVERLAY_FILE );
        return;
    }
    file = CreateFileW( OVERLAY_FILE, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL );
    if (file != INVALID_HANDLE_VALUE) CloseHandle( file );
}

static void refresh(void)
{
    BOOL now_game = game_running(), want;

    if (now_game != game)
    {
        forced = 0;   /* a game came or went: as the settings say again */
        history_count = history_next = 0;
    }
    game = now_game;
    want = forced > 0 || (!forced && settings.overlay && (settings.overlay_always || game));

    if (want && !shown)
    {
        shown = TRUE;
        want_frames( TRUE );
        update();
        SetTimer( window, TIMER_UPDATE, 500, NULL );
    }
    else if (!want && shown)
    {
        shown = FALSE;
        want_frames( FALSE );
        KillTimer( window, TIMER_UPDATE );
        KillTimer( window, TIMER_FRAMES );
        ShowWindow( window, SW_HIDE );
        ArcticSensors( NULL );
    }
    /* the graph moves with the frames, over a game */
    if (shown && game && (settings.overlay_items & (OSD_FRAME_GRAPH | OSD_FPS | OSD_FRAME_TIME)))
        SetTimer( window, TIMER_FRAMES, 100, NULL );
    else KillTimer( window, TIMER_FRAMES );
    /* nothing to watch for while the overlay is off */
    if (settings.overlay || forced) SetTimer( window, TIMER_WATCH, 1000, NULL );
    else KillTimer( window, TIMER_WATCH );
}

static UINT hotkey_modifiers( int hotkey )
{
    UINT mods = MOD_NOREPEAT;

    if (HIBYTE( hotkey ) & HOTKEYF_SHIFT) mods |= MOD_SHIFT;
    if (HIBYTE( hotkey ) & HOTKEYF_CONTROL) mods |= MOD_CONTROL;
    if (HIBYTE( hotkey ) & HOTKEYF_ALT) mods |= MOD_ALT;
    return mods;
}

static void load_settings(void)
{
    arctic_games_read( &settings );
    UnregisterHotKey( window, HOTKEY_OVERLAY );
    UnregisterHotKey( window, HOTKEY_PANEL );
    if (LOBYTE( settings.hotkey_overlay ))
        RegisterHotKey( window, HOTKEY_OVERLAY, hotkey_modifiers( settings.hotkey_overlay ), LOBYTE( settings.hotkey_overlay ) );
    if (LOBYTE( settings.hotkey_panel ))
        RegisterHotKey( window, HOTKEY_PANEL, hotkey_modifiers( settings.hotkey_panel ), LOBYTE( settings.hotkey_panel ) );
    forced = 0;
}

static void open_panel(void)
{
    HMODULE module = LoadLibraryW( L"powercpl.dll" );
    BOOL (WINAPI *open)( const WCHAR * ) = module ? (void *)GetProcAddress( module, "ArcticOpenPowerOptions" ) : NULL;

    /* the key was pressed for it: the window comes to the front over the game */
    AllowSetForegroundWindow( ASFW_ANY );
    if (open) open( L"games" );
}

static LRESULT WINAPI overlay_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    switch (msg)
    {
    case WM_TIMER:
        if (wp == TIMER_UPDATE && shown) update();
        else if (wp == TIMER_FRAMES && shown)
        {
            read_frames();
            paint();
        }
        else if (wp == TIMER_WATCH) refresh();
        return 0;
    case WM_HOTKEY:
    {
        /* a key held down repeats, MOD_NOREPEAT or not */
        static DWORD last;
        DWORD now = GetTickCount();

        if (now - last < 400)
        {
            last = now;
            return 0;
        }
        last = now;
        if (wp == HOTKEY_OVERLAY)
        {
            forced = shown ? -1 : 1;
            refresh();
        }
        else if (wp == HOTKEY_PANEL) open_panel();
        return 0;
    }
    case WM_ARCTIC_GAMES_RELOAD:
        load_settings();
        if (shown) update();
        refresh();
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
        if (shown) update();
        return 0;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_ENDSESSION:
        if (wp) want_frames( FALSE );
        return 0;
    case WM_DESTROY:
        want_frames( FALSE );
        PostQuitMessage( 0 );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wp, lp );
}

int WINAPI wWinMain( HINSTANCE instance, HINSTANCE prev, WCHAR *cmdline, int show )
{
    WNDCLASSW cls = { 0 };
    HWND running;
    MSG msg;

    /* one overlay: a second start tells the first to read its settings */
    if ((running = FindWindowW( ARCTIC_OVERLAY_CLASS, NULL )))
    {
        PostMessageW( running, WM_ARCTIC_GAMES_RELOAD, 0, 0 );
        return 0;
    }
    SetProcessDPIAware();
    GetLocaleInfoW( LOCALE_USER_DEFAULT, LOCALE_SDECIMAL, decimal, ARRAY_SIZE(decimal) );

    cls.lpfnWndProc = overlay_proc;
    cls.hInstance = instance;
    cls.lpszClassName = ARCTIC_OVERLAY_CLASS;
    RegisterClassW( &cls );
    window = CreateWindowExW( WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                              ARCTIC_OVERLAY_CLASS, L"", WS_POPUP, 0, 0, 1, 1, NULL, NULL, instance, NULL );
    if (!window) return 1;

    load_settings();
    refresh();
    while (GetMessageW( &msg, NULL, 0, 0 ))
    {
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }
    return 0;
}
