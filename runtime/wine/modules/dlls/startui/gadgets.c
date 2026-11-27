/*
 * Windows Widgets: the lists of widgets, the widgets that need no network
 * and the desktop that shows them
 *
 * One set of widgets stands on the widgets board (widgets.c) and on the
 * desktop alike, each widget in one of its sizes there: the weather, the
 * news and Copilot (drawn by widgets.c); the day, the date and the time in
 * large thin letters, the processor, the graphics card, the memory and the
 * disk C: as the Games panel shows them, and the battery (drawn here).
 *
 * On the desktop they stay just above it, show a frame while the pointer is
 * over them or a button is down on them, and are dragged anywhere by it; a
 * click that does not move one is a click on what is under the pointer.
 * A thread of their own keeps the live ones (the time, the sensors, the
 * battery) for the desktop and the board.
 *
 * HKCU\Software\Arctic\Widgets: Board and Desktop (REG_MULTI_SZ, a widget a
 * line, "Name,size" and on the desktop ",x,y" once it was put somewhere),
 * ClockEnglish, MonitorItems. HKCU\Software\Arctic\DesktopWidgets, what came
 * before, gives the desktop's list until there is one.
 *
 * Copyright 2026 Arctic contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "startui.h"
#include "winreg.h"
#include "dwmapi.h"
#include "shlobj.h"
#include "shellapi.h"
#include "winioctl.h"
#include "bthsdpdef.h"
#include "bluetoothapis.h"
#include "bthdef.h"
#include "bthledef.h"
#include "bthioctl.h"
#include "wine/winebth.h"
#include "wine/arctic_dwm.h"
#include "wine/arctic_power.h"

#define WIDGETS_KEY      L"Software\\Arctic\\Widgets"
#define OLD_KEY          L"Software\\Arctic\\DesktopWidgets"
#define GADGETS_CHANGED  L"ArcticDesktopWidgets"   /* WM_SETTINGCHANGE's lparam, from before */
#define RELOAD_MESSAGE   L"ArcticWidgetsChanged"   /* posted to the manager: the lists changed */
#define MANAGER_CLASS    L"ArcticDesktopWidgets"

#define WM_NET_DATA      (WM_APP + 2)               /* the weather or the news came */

/* the monitoring's rows, MonitorItems */
enum { ITEM_CPU = 1, ITEM_GPU = 2, ITEM_RAM = 4, ITEM_DISK = 8, ITEM_ALL = 15 };

#define TIMER_TICK  1
#define TIMER_ICONS 2
#define TIMER_CARET 3

static DWORD monitor_items = ITEM_ALL, clock_english = 1;
static struct arctic_sensors sensors;
static BOOL have_sensors;
static SRWLOCK sensors_lock = SRWLOCK_INIT;
static SYSTEMTIME shown_time;
static int ticks;
static HWND manager;

/**********************************************************************
 *          The lists
 */

static const WCHAR *const kind_keys[W_KINDS] = { L"Weather", L"News", L"Copilot", L"Clock", L"Monitor", L"Battery" };

/* a size in columns and rows of the board; none: the widget has not that size */
static const BYTE cells[W_KINDS][W_SIZES][2] =
{
    { { 1, 1 }, { 1, 2 }, { 2, 2 } },   /* the weather */
    { { 1, 1 }, { 1, 2 }, { 2, 2 } },   /* the news */
    { { 1, 1 }, { 2, 1 }, { 0, 0 } },   /* Copilot */
    { { 1, 1 }, { 2, 1 }, { 2, 2 } },   /* the clock */
    { { 1, 1 }, { 1, 2 }, { 2, 2 } },   /* the monitoring */
    { { 1, 1 }, { 1, 2 }, { 0, 0 } },   /* the battery */
};

#define COLUMN  px( 360 )
#define ROW     px( 160 )
#define GAP     px( 16 )

int widget_gap(void)
{
    return GAP;
}

BOOL widget_allows( enum widget_kind kind, enum widget_size size )
{
    return kind < W_KINDS && size < W_SIZES && cells[kind][size][0];
}

void widget_cells( enum widget_kind kind, enum widget_size size, int *columns, int *rows )
{
    if (kind >= W_KINDS) kind = W_WEATHER;
    if (!widget_allows( kind, size )) size = W_SMALL;
    *columns = cells[kind][size][0];
    *rows = cells[kind][size][1];
}

void widget_extent( enum widget_kind kind, enum widget_size size, int *width, int *height )
{
    int columns, rows;

    widget_cells( kind, size, &columns, &rows );
    *width = columns * COLUMN + (columns - 1) * GAP;
    *height = rows * ROW + (rows - 1) * GAP;
}

const WCHAR *widget_name( enum widget_kind kind )
{
    static const UINT ids[W_KINDS] = { IDS_WW_WEATHER, IDS_WW_NEWS, IDS_WW_COPILOT, IDS_DW_CLOCK, IDS_DW_MONITOR, IDS_DW_BATTERY };

    return load_string( ids[kind < W_KINDS ? kind : 0] );
}

/* the size a widget comes in, put somewhere */
static enum widget_size default_size( enum widget_kind kind, enum widget_host host )
{
    if (kind == W_NEWS && host == HOST_BOARD) return W_LARGE;
    if (kind == W_COPILOT || kind == W_BATTERY) return W_SMALL;
    return W_MEDIUM;
}

static BOOL find_dword( const WCHAR *key, const WCHAR *name, DWORD *value )
{
    DWORD size = sizeof(*value);

    return !RegGetValueW( HKEY_CURRENT_USER, key, name, RRF_RT_REG_DWORD, NULL, value, &size );
}

/* a setting of the widgets, or what the desktop's widgets had before */
static DWORD get_setting( const WCHAR *name, DWORD def )
{
    DWORD value;

    if (find_dword( WIDGETS_KEY, name, &value ) || find_dword( OLD_KEY, name, &value )) return value;
    return def;
}

static void set_setting( const WCHAR *name, DWORD value )
{
    HKEY key;

    if (RegCreateKeyExW( HKEY_CURRENT_USER, WIDGETS_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL )) return;
    RegSetValueExW( key, name, 0, REG_DWORD, (const BYTE *)&value, sizeof(value) );
    RegCloseKey( key );
}

static int find_kind( const struct widget *list, int count, enum widget_kind kind )
{
    for (int i = 0; i < count; i++) if (list[i].kind == kind) return i;
    return -1;
}

static void add_widget( struct widget *list, int *count, enum widget_kind kind, enum widget_size size )
{
    if (*count >= W_KINDS || find_kind( list, *count, kind ) >= 0) return;
    memset( &list[*count], 0, sizeof(list[*count]) );
    list[*count].kind = kind;
    list[*count].size = widget_allows( kind, size ) ? size : W_SMALL;
    (*count)++;
}

/* "Weather,1" or "Clock,1,960,48" */
static int parse_list( const WCHAR *multi, struct widget *list )
{
    int count = 0;

    for (const WCHAR *p = multi; *p && count < W_KINDS; p += lstrlenW( p ) + 1)
    {
        const WCHAR *comma = wcschr( p, ',' );
        int n = comma ? comma - p : lstrlenW( p ), kind = W_KINDS;
        struct widget w = { 0 };
        WCHAR name[32], *end, *end2;

        if (n <= 0 || n >= ARRAY_SIZE(name)) continue;
        memcpy( name, p, n * sizeof(WCHAR) );
        name[n] = 0;
        for (int k = 0; k < W_KINDS; k++) if (!lstrcmpiW( name, kind_keys[k] )) kind = k;
        if (kind == W_KINDS || find_kind( list, count, kind ) >= 0) continue;
        w.kind = kind;
        w.size = default_size( kind, HOST_DESKTOP );
        if (comma)
        {
            long size = wcstol( comma + 1, &end, 10 );

            if (end != comma + 1 && size >= 0 && size < W_SIZES) w.size = size;
            if (*end == ',')
            {
                long x = wcstol( end + 1, &end2, 10 );

                if (end2 != end + 1 && *end2 == ',')
                {
                    long y = wcstol( end2 + 1, &end, 10 );

                    if (end != end2 + 1)
                    {
                        w.placed = TRUE;
                        w.x = x;
                        w.y = y;
                    }
                }
            }
        }
        if (!widget_allows( w.kind, w.size )) w.size = W_SMALL;
        list[count++] = w;
    }
    return count;
}

/* the board as it comes: the weather and, beside it, Copilot over the
 * monitoring; the news under them across the width, three stories */
static int board_defaults( struct widget *list )
{
    int count = 0;

    add_widget( list, &count, W_WEATHER, W_MEDIUM );
    add_widget( list, &count, W_COPILOT, W_SMALL );
    add_widget( list, &count, W_MONITOR, W_SMALL );
    add_widget( list, &count, W_NEWS, W_LARGE );
    return count;
}

/* the desktop: what its widgets were before they were these, else the clock */
static int desktop_defaults( struct widget *list )
{
    static const enum widget_kind old_kinds[] = { W_CLOCK, W_MONITOR, W_BATTERY };
    static const DWORD old_default[] = { 1, 0, 0 };
    int count = 0;

    for (int i = 0; i < ARRAY_SIZE(old_kinds); i++)
    {
        DWORD on = old_default[i], x, y;
        WCHAR name[32];

        find_dword( OLD_KEY, kind_keys[old_kinds[i]], &on );
        if (!on) continue;
        add_widget( list, &count, old_kinds[i], W_MEDIUM );
        swprintf( name, ARRAY_SIZE(name), L"%sX", kind_keys[old_kinds[i]] );
        if (!find_dword( OLD_KEY, name, &x )) continue;
        swprintf( name, ARRAY_SIZE(name), L"%sY", kind_keys[old_kinds[i]] );
        if (!find_dword( OLD_KEY, name, &y )) continue;
        list[count - 1].placed = TRUE;
        list[count - 1].x = (int)x;
        list[count - 1].y = (int)y;
    }
    return count;
}

int widgets_load( enum widget_host host, struct widget *list )
{
    DWORD size = 2048 * sizeof(WCHAR);
    WCHAR *multi = calloc( 2048 + 2, sizeof(WCHAR) );
    int count;

    if (multi && !RegGetValueW( HKEY_CURRENT_USER, WIDGETS_KEY, host == HOST_BOARD ? L"Board" : L"Desktop",
                                RRF_RT_REG_MULTI_SZ, NULL, multi, &size ))
        count = parse_list( multi, list );
    else
        count = host == HOST_BOARD ? board_defaults( list ) : desktop_defaults( list );
    free( multi );
    return count;
}

void widgets_save( enum widget_host host, const struct widget *list, int count )
{
    WCHAR multi[W_KINDS * 48 + 2];
    int used = 0;
    HKEY key;

    for (int i = 0; i < count; i++)
    {
        if (host == HOST_DESKTOP && list[i].placed)
            used += swprintf( multi + used, 48, L"%s,%d,%d,%d", kind_keys[list[i].kind], list[i].size, list[i].x, list[i].y );
        else
            used += swprintf( multi + used, 48, L"%s,%d", kind_keys[list[i].kind], list[i].size );
        multi[used++] = 0;
    }
    multi[used++] = 0;
    if (!count) multi[used++] = 0;
    if (RegCreateKeyExW( HKEY_CURRENT_USER, WIDGETS_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL )) return;
    RegSetValueExW( key, host == HOST_BOARD ? L"Board" : L"Desktop", 0, REG_MULTI_SZ, (const BYTE *)multi,
                    used * sizeof(WCHAR) );
    RegCloseKey( key );
}

/* the lists changed: the desktop and the board take them again, from this
 * process or another (the Display panel's dialog) */
void widgets_notify(void)
{
    HWND window = FindWindowW( MANAGER_CLASS, NULL );

    if (window) PostMessageW( window, RegisterWindowMessageW( RELOAD_MESSAGE ), 0, 0 );
}

/* the weather or the news came: the desktop draws them again */
void widgets_desktop_data(void)
{
    if (manager) PostMessageW( manager, WM_NET_DATA, 0, 0 );
}

/**********************************************************************
 *          What a widget is made of, on both
 */

void widget_add_target( struct widget_paint *p, enum widget_hit hit, int index, const RECT *rect )
{
    if (!p->targets || p->target_count >= WIDGET_TARGETS) return;
    p->targets[p->target_count].hit = hit;
    p->targets[p->target_count].index = index;
    p->targets[p->target_count].rect = *rect;
    p->target_count++;
}

/* the last that has the point: what is inside something comes after it */
const struct widget_target *widget_hit_test( const struct widget_target *targets, int count, POINT pt )
{
    const struct widget_target *found = NULL;

    for (int i = 0; i < count; i++) if (PtInRect( &targets[i].rect, pt )) found = &targets[i];
    return found;
}

/* "...", at the top right of a widget, for its sizes and the rest */
void widget_more_rect( const RECT *card, RECT *rect )
{
    int size = px( 28 ), margin = px( 6 );

    SetRect( rect, card->right - margin - size, card->top + margin, card->right - margin, card->top + margin + size );
}

void widget_more_button( HDC hdc, const struct palette *pal, const RECT *card, BOOL style11, BOOL hot, BOOL opaque_under )
{
    int glyph = px( 16 );
    RECT r;

    widget_more_rect( card, &r );
    if (opaque_under) fill_round_rect( hdc, &r, (float)(style11 ? px( 4 ) : 0), RGB( 0, 0, 0 ), hot ? 0x70 : 0x40 );
    else if (hot) fill_round_rect( hdc, &r, (float)(style11 ? px( 4 ) : 0), pal->ink, 0x14 );
    draw_glyph( hdc, GLYPH_MORE, (r.left + r.right - glyph) / 2, (r.top + r.bottom - glyph) / 2, glyph,
                opaque_under ? RGB( 255, 255, 255 ) : pal->ink, 255 );
}

/* a menu's text of its own: load_string's buffers go round, and the board
 * may paint while the menu is open */
static const WCHAR *keep_text( WCHAR *store, const WCHAR *text )
{
    lstrcpynW( store, text, 64 );
    return store;
}

static void popup_look( struct look *look, int *style )
{
    look_load_taskbar( look );
    *style = (taskbar_style11() ? POPUP_WIN11 : POPUP_WIN10) | POPUP_TASKBAR;
}

/* a widget's menu: its sizes, the other place it can also stand, what it
 * has of its own, and taking it away. TRUE when something was chosen. */
BOOL widget_menu( HWND owner, POINT pt, enum widget_host host, enum widget_kind kind )
{
    enum { ID_SIZE = 1, ID_OTHER = 10, ID_REMOVE, ID_LOCATION, ID_REFRESH, ID_SETTINGS };
    static const UINT size_names[W_SIZES] = { IDS_TILE_SMALL, IDS_TILE_MEDIUM, IDS_TILE_LARGE };
    enum widget_host other_host = host == HOST_BOARD ? HOST_DESKTOP : HOST_BOARD;
    struct widget list[W_KINDS], other[W_KINDS];
    int count = widgets_load( host, list ), other_count = widgets_load( other_host, other ), style;
    int at = find_kind( list, count, kind ), other_at = find_kind( other, other_count, kind );
    struct popup_item items[W_SIZES + 8];
    WCHAR texts[W_SIZES + 8][64];
    struct look look;
    UINT n = 0, t = 0, cmd;

    if (at < 0) return FALSE;
    memset( items, 0, sizeof(items) );
    for (int s = 0; s < W_SIZES; s++)
        if (widget_allows( kind, s ))
            items[n++] = (struct popup_item){ ID_SIZE + s, keep_text( texts[t++], load_string( size_names[s] ) ), GLYPH_NONE, list[at].size == s };
    items[n++] = (struct popup_item){ 0 };
    items[n++] = (struct popup_item){ ID_OTHER, keep_text( texts[t++], load_string( host == HOST_BOARD ? IDS_WW_ON_DESKTOP : IDS_WW_ON_BOARD ) ),
                                      GLYPH_NONE, other_at >= 0 };
    if (kind == W_WEATHER) items[n++] = (struct popup_item){ ID_LOCATION, keep_text( texts[t++], load_string( IDS_WIDGET_LOCATION ) ), GLYPH_NONE };
    if (kind == W_WEATHER || kind == W_NEWS)
        items[n++] = (struct popup_item){ ID_REFRESH, keep_text( texts[t++], load_string( IDS_WIDGET_REFRESH ) ), GLYPH_NONE };
    if (kind == W_CLOCK || kind == W_MONITOR)
        items[n++] = (struct popup_item){ ID_SETTINGS, keep_text( texts[t++], load_string( IDS_DW_SETTINGS ) ), GLYPH_NONE };
    items[n++] = (struct popup_item){ 0 };
    items[n++] = (struct popup_item){ ID_REMOVE, keep_text( texts[t++], load_string( IDS_DW_REMOVE ) ), GLYPH_UNPIN };
    popup_look( &look, &style );
    cmd = popup_track( owner, pt, TPM_LEFTALIGN | TPM_TOPALIGN, items, n, style, &look );
    switch (cmd)
    {
    case 0:
        return FALSE;
    case ID_OTHER:
        if (other_at >= 0)
        {
            memmove( &other[other_at], &other[other_at + 1], (other_count - other_at - 1) * sizeof(*other) );
            other_count--;
        }
        else add_widget( other, &other_count, kind, list[at].size );   /* the same size there */
        widgets_save( other_host, other, other_count );
        break;
    case ID_REMOVE:
        memmove( &list[at], &list[at + 1], (count - at - 1) * sizeof(*list) );
        widgets_save( host, list, count - 1 );
        break;
    case ID_LOCATION:
        widgets_change_location( owner );
        return TRUE;
    case ID_REFRESH:
        widgets_refresh();
        return TRUE;
    case ID_SETTINGS:
        ArcticDesktopWidgetsDialog( NULL );
        return TRUE;
    default:
        if (cmd >= ID_SIZE && cmd < ID_SIZE + W_SIZES)
        {
            list[at].size = cmd - ID_SIZE;
            widgets_save( host, list, count );
        }
        break;
    }
    widgets_notify();
    return TRUE;
}

/* "Add widgets": what is not there yet */
BOOL widget_add_menu( HWND owner, POINT pt, enum widget_host host )
{
    struct widget list[W_KINDS];
    struct popup_item items[W_KINDS];
    WCHAR texts[W_KINDS][64];
    int count = widgets_load( host, list ), style;
    struct look look;
    UINT n = 0, t = 0, cmd;

    memset( items, 0, sizeof(items) );
    for (int k = 0; k < W_KINDS; k++)
        if (find_kind( list, count, k ) < 0) items[n++] = (struct popup_item){ 1 + k, keep_text( texts[t++], widget_name( k ) ), GLYPH_NONE };
    if (!n) return FALSE;
    popup_look( &look, &style );
    if (!(cmd = popup_track( owner, pt, TPM_LEFTALIGN | TPM_TOPALIGN, items, n, style, &look ))) return FALSE;
    add_widget( list, &count, cmd - 1, default_size( cmd - 1, host ) );
    widgets_save( host, list, count );
    widgets_notify();
    return TRUE;
}

/**********************************************************************
 *          The letters of the day: thin, wide, round, own drawings
 *
 * In units of the capital's height, y down; 1 x0 y0 x1 y1 is a line,
 * 2 cx cy r from to an arc (degrees, 0 to the right, 90 down), 0 the end.
 */

struct letter { WCHAR ch; float width; const float *ops; };

#define LN 1.f
#define AR 2.f

static const float l_a[] = { LN,0,1,.45f,0, LN,.45f,0,.9f,1, 0 };
static const float l_be[] = { LN,.9f,0,0,0, LN,0,0,0,1, LN,0,.45f,.625f,.45f, AR,.625f,.725f,.275f,-90,90, LN,.625f,1,0,1, 0 };
static const float l_b[] = { LN,0,0,0,1, LN,0,0,.625f,0, AR,.625f,.225f,.225f,-90,90, LN,0,.45f,.625f,.45f,
                             AR,.625f,.725f,.275f,-90,90, LN,.625f,1,0,1, 0 };
static const float l_ge[] = { LN,0,0,0,1, LN,0,0,.9f,0, 0 };
static const float l_de[] = { LN,.25f,0,.8f,0, LN,.8f,0,.8f,1, LN,.25f,0,.1f,1, LN,-.05f,1,.95f,1, LN,-.05f,1,-.05f,1.15f,
                              LN,.95f,1,.95f,1.15f, 0 };
static const float l_d[] = { LN,0,0,0,1, LN,0,0,.4f,0, AR,.4f,.5f,.5f,-90,90, LN,.4f,1,0,1, 0 };
static const float l_e[] = { LN,.9f,0,0,0, LN,0,0,0,1, LN,0,.5f,.7f,.5f, LN,0,1,.9f,1, 0 };
static const float l_f[] = { LN,.9f,0,0,0, LN,0,0,0,1, LN,0,.5f,.7f,.5f, 0 };
static const float l_h[] = { LN,0,0,0,1, LN,.9f,0,.9f,1, LN,0,.5f,.9f,.5f, 0 };
static const float l_i[] = { LN,0,0,0,1, 0 };
static const float l_y_uk[] = { LN,0,0,0,1, LN,.9f,0,.9f,1, LN,.9f,0,0,1, 0 };   /* И */
static const float l_k[] = { LN,0,0,0,1, LN,0,.55f,.85f,0, LN,.3f,.38f,.9f,1, 0 };
static const float l_el[] = { LN,.3f,0,.9f,0, LN,.9f,0,.9f,1, LN,.3f,0,0,1, 0 };
static const float l_m[] = { LN,0,1,0,0, LN,0,0,.45f,.55f, LN,.45f,.55f,.9f,0, LN,.9f,0,.9f,1, 0 };
static const float l_n[] = { LN,0,1,0,0, LN,0,0,.9f,1, LN,.9f,1,.9f,0, 0 };
static const float l_o[] = { AR,.5f,.5f,.5f,0,360, 0 };
static const float l_pe[] = { LN,0,1,0,0, LN,0,0,.9f,0, LN,.9f,0,.9f,1, 0 };
static const float l_p[] = { LN,0,1,0,0, LN,0,0,.65f,0, AR,.65f,.25f,.25f,-90,90, LN,.65f,.5f,0,.5f, 0 };
static const float l_r[] = { LN,0,1,0,0, LN,0,0,.65f,0, AR,.65f,.25f,.25f,-90,90, LN,.65f,.5f,0,.5f, LN,.5f,.5f,.9f,1, 0 };
static const float l_c[] = { AR,.5f,.5f,.5f,40,320, 0 };
static const float l_s[] = { LN,.9f,0,.25f,0, AR,.25f,.25f,.25f,270,90, LN,.25f,.5f,.65f,.5f, AR,.65f,.75f,.25f,-90,90,
                             LN,.65f,1,0,1, 0 };
static const float l_t[] = { LN,0,0,.9f,0, LN,.45f,0,.45f,1, 0 };
static const float l_u_uk[] = { LN,0,0,.45f,.55f, LN,.9f,0,.25f,1, 0 };   /* У */
static const float l_u[] = { LN,0,0,0,.55f, AR,.45f,.55f,.45f,180,0, LN,.9f,.55f,.9f,0, 0 };
static const float l_w[] = { LN,0,0,.275f,1, LN,.275f,1,.55f,.35f, LN,.55f,.35f,.825f,1, LN,.825f,1,1.1f,0, 0 };
static const float l_y[] = { LN,0,0,.45f,.5f, LN,.9f,0,.45f,.5f, LN,.45f,.5f,.45f,1, 0 };
static const float l_tse[] = { LN,0,0,0,1, LN,.8f,0,.8f,1, LN,0,1,.95f,1, LN,.95f,1,.95f,1.15f, 0 };
static const float l_che[] = { LN,0,0,0,.5f, LN,0,.5f,.9f,.5f, LN,.9f,0,.9f,1, 0 };
static const float l_ya[] = { LN,.9f,0,.9f,1, LN,.9f,0,.25f,0, AR,.25f,.25f,.25f,270,90, LN,.25f,.5f,.9f,.5f, LN,.4f,.5f,0,1, 0 };
static const float l_soft[] = { LN,0,0,0,1, LN,0,.45f,.625f,.45f, AR,.625f,.725f,.275f,-90,90, LN,.625f,1,0,1, 0 };
static const float l_apos[] = { LN,0,0,0,.28f, 0 };

static const struct letter letters[] =
{
    { 'A', .9f, l_a }, { 'D', .9f, l_d }, { 'E', .9f, l_e }, { 'F', .9f, l_f }, { 'H', .9f, l_h }, { 'I', 0, l_i },
    { 'M', .9f, l_m }, { 'N', .9f, l_n }, { 'O', 1, l_o }, { 'R', .9f, l_r }, { 'S', .9f, l_s }, { 'T', .9f, l_t },
    { 'U', .9f, l_u }, { 'W', 1.1f, l_w }, { 'Y', .9f, l_y }, { 'B', .9f, l_b }, { 'C', 1, l_c }, { 'K', .9f, l_k },
    { 'P', .9f, l_p }, { '\'', 0, l_apos }, { 0x2019, 0, l_apos },
    { 0x0410, .9f, l_a }, { 0x0411, .9f, l_be }, { 0x0412, .9f, l_b }, { 0x0413, .9f, l_ge }, { 0x0414, .9f, l_de },
    { 0x0415, .9f, l_e }, { 0x0406, 0, l_i }, { 0x0418, .9f, l_y_uk }, { 0x041a, .9f, l_k }, { 0x041b, .9f, l_el },
    { 0x041c, .9f, l_m }, { 0x041d, .9f, l_h }, { 0x041e, 1, l_o }, { 0x041f, .9f, l_pe }, { 0x0420, .9f, l_p },
    { 0x0421, 1, l_c }, { 0x0422, .9f, l_t }, { 0x0423, .9f, l_u_uk }, { 0x0426, .95f, l_tse }, { 0x0427, .9f, l_che },
    { 0x042f, .9f, l_ya }, { 0x042c, .9f, l_soft },
};

static const struct letter *find_letter( WCHAR ch )
{
    for (int i = 0; i < ARRAY_SIZE(letters); i++) if (letters[i].ch == ch) return &letters[i];
    return NULL;
}

static BOOL letters_cover( const WCHAR *text )
{
    for (; *text; text++) if (*text != ' ' && !find_letter( *text )) return FALSE;
    return TRUE;
}

/* the width of a word, the letters apart by gap (in heights) */
static float word_width( const WCHAR *text, float gap )
{
    float width = 0;
    int count = 0;

    for (; *text; text++)
    {
        const struct letter *l = find_letter( *text );

        width += l ? l->width : .5f;
        count++;
    }
    return count ? width + gap * (count - 1) : 0;
}

struct mask { int width, height; BYTE *bits; };

/* a stroke of round ends into the mask, each pixel by 4x4 samples */
static void mask_segment( struct mask *m, float x0, float y0, float x1, float y1, float r )
{
    int left = max( 0, (int)floorf( min( x0, x1 ) - r - 1 ) ), right = min( m->width, (int)ceilf( max( x0, x1 ) + r + 1 ) );
    int top = max( 0, (int)floorf( min( y0, y1 ) - r - 1 ) ), bottom = min( m->height, (int)ceilf( max( y0, y1 ) + r + 1 ) );
    float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;

    for (int y = top; y < bottom; y++)
    {
        for (int x = left; x < right; x++)
        {
            int hits = 0;
            BYTE *p = &m->bits[y * m->width + x];

            for (int j = 0; j < 4; j++)
            {
                for (int i = 0; i < 4; i++)
                {
                    float px_ = x + (i + .5f) / 4 - x0, py_ = y + (j + .5f) / 4 - y0;
                    float t = len2 > 0 ? (px_ * dx + py_ * dy) / len2 : 0, ex, ey;

                    t = t < 0 ? 0 : t > 1 ? 1 : t;
                    ex = px_ - t * dx;
                    ey = py_ - t * dy;
                    if (ex * ex + ey * ey <= r * r) hits++;
                }
            }
            if (hits * 255 / 16 > *p) *p = hits * 255 / 16;
        }
    }
}

static void mask_word( struct mask *m, const WCHAR *text, float x, float y, float height, float gap, float stroke )
{
    for (; *text; text++)
    {
        const struct letter *l = find_letter( *text );
        const float *p;

        if (!l) { x += (.5f + gap) * height; continue; }
        for (p = l->ops; *p; )
        {
            if (*p == LN)
            {
                mask_segment( m, x + p[1] * height, y + p[2] * height, x + p[3] * height, y + p[4] * height, stroke / 2 );
                p += 5;
            }
            else
            {
                float cx = x + p[1] * height, cy = y + p[2] * height, r = p[3] * height;
                float from = p[4] * 3.14159265f / 180, to = p[5] * 3.14159265f / 180;
                int steps = max( 6, (int)(fabsf( p[5] - p[4] ) / 5) );

                for (int i = 0; i < steps; i++)
                {
                    float a = from + (to - from) * i / steps, b = from + (to - from) * (i + 1) / steps;

                    mask_segment( m, cx + r * cosf( a ), cy + r * sinf( a ), cx + r * cosf( b ), cy + r * sinf( b ), stroke / 2 );
                }
                p += 6;
            }
        }
        x += (l->width + gap) * height;
    }
}

/* the mask in white, premultiplied, over what is drawn */
static void blend_mask( HDC hdc, const struct mask *m, int x, int y, COLORREF color )
{
    UINT32 *pixels = malloc( m->width * m->height * sizeof(*pixels) );

    if (!pixels) return;
    for (int i = 0; i < m->width * m->height; i++)
    {
        UINT32 a = m->bits[i];

        pixels[i] = (a << 24) | ((GetRValue( color ) * a / 255) << 16) | ((GetGValue( color ) * a / 255) << 8) |
                    (GetBValue( color ) * a / 255);
    }
    blend_bits( hdc, x, y, m->width, m->height, pixels );
    free( pixels );
}

/**********************************************************************
 *          Drawing
 */

/* see-through pixels of the key (alpha 0): a veil and a frame lighter than
 * what is behind in the dark, darker in the light, the frame of the pointer */
static void paint_frame( HDC hdc, HBITMAP bitmap, const RECT *rect, float radius, int veil, int line, BOOL light )
{
    int width = rect->right - rect->left, height = rect->bottom - rect->top;
    DIBSECTION dib;
    BYTE *bits;

    GdiFlush();
    if (GetObjectW( bitmap, sizeof(dib), &dib ) != sizeof(dib) || !(bits = dib.dsBm.bmBits)) return;
    for (int y = max( rect->top, 0 ); y < min( rect->bottom, abs( dib.dsBm.bmHeight ) ); y++)
    {
        BYTE *row = bits + y * dib.dsBm.bmWidthBytes;

        for (int x = max( rect->left, 0 ); x < min( rect->right, dib.dsBm.bmWidth ); x++)
        {
            int inside = 0, edge = 0, value;

            for (int j = 0; j < 3; j++)
            {
                for (int i = 0; i < 3; i++)
                {
                    float sx = x - rect->left + (i + .5f) / 3, sy = y - rect->top + (j + .5f) / 3;
                    float dx = max( max( radius - sx, sx - (width - radius) ), 0.f );
                    float dy = max( max( radius - sy, sy - (height - radius) ), 0.f );
                    float d = sqrtf( dx * dx + dy * dy );

                    if (d > radius) continue;
                    inside++;
                    /* the frame: within a pixel of the edge */
                    if (min( min( sx, width - sx ), min( sy, height - sy ) ) < 1.2f || (dx > 0 && dy > 0 && d > radius - 1.2f))
                        edge++;
                }
            }
            if (!inside) continue;
            value = (veil * (inside - edge) + line * edge) / 9;
            if (row[x * 4 + 3]) continue;   /* drawn opaque already */
            for (int c = 0; c < 3; c++)
            {
                if (light) row[x * 4 + c] = min( row[x * 4 + c], 255 - value );
                else row[x * 4 + c] = max( row[x * 4 + c], value );
            }
        }
    }
}

/* a ring of how much, from the top round to the right, its ends round */
static void paint_ring( HDC hdc, int cx, int cy, int radius, int thickness, float fraction, COLORREF track, COLORREF fill )
{
    int size = radius * 2 + 2, left = cx - radius - 1, top = cy - radius - 1;
    float mid = radius - thickness / 2.f, half = thickness / 2.f, sweep = max( 0.f, min( fraction, 1.f ) ) * 2 * 3.14159265f;
    float end_x = cosf( sweep - 3.14159265f / 2 ) * mid, end_y = sinf( sweep - 3.14159265f / 2 ) * mid;
    UINT32 *pixels = malloc( size * size * sizeof(*pixels) );

    if (!pixels) return;
    for (int y = 0; y < size; y++)
    {
        for (int x = 0; x < size; x++)
        {
            int on = 0, lit = 0;

            for (int j = 0; j < 4; j++)
            {
                for (int i = 0; i < 4; i++)
                {
                    float sx = left + x + (i + .5f) / 4 - cx, sy = top + y + (j + .5f) / 4 - cy;
                    float d = sqrtf( sx * sx + sy * sy ), angle = atan2f( sx, -sy );

                    if (angle < 0) angle += 2 * 3.14159265f;
                    if (fabsf( d - mid ) <= half)
                    {
                        on++;
                        if (angle <= sweep) lit++;
                    }
                    else if (sweep > 0 && ((sx * sx + (sy + mid) * (sy + mid) <= half * half) ||
                             ((sx - end_x) * (sx - end_x) + (sy - end_y) * (sy - end_y) <= half * half)))
                        on++, lit++;
                }
            }
            {
                UINT32 a = on * 255 / 16, la = lit * 255 / 16, ta = a - min( a, la );
                UINT32 r = (GetRValue( fill ) * la + GetRValue( track ) * ta) / 255;
                UINT32 g = (GetGValue( fill ) * la + GetGValue( track ) * ta) / 255;
                UINT32 b = (GetBValue( fill ) * la + GetBValue( track ) * ta) / 255;

                pixels[y * size + x] = (a << 24) | (r << 16) | (g << 8) | b;
            }
        }
    }
    blend_bits( hdc, left, top, size, size, pixels );
    free( pixels );
}

/* a number with the decimal point of the user's language */
static void format_decimal( WCHAR *out, int count, double value )
{
    WCHAR point[4] = L".";

    swprintf( out, count, L"%.1f", value );
    if (GetLocaleInfoEx( LOCALE_NAME_USER_DEFAULT, LOCALE_SDECIMAL, point, ARRAY_SIZE(point) ) && point[0] != '.')
        for (WCHAR *p = out; *p; p++) if (*p == '.') *p = point[0];
}

static void format_gb( WCHAR *out, int count, double gb )
{
    WCHAR number[32];

    format_decimal( number, ARRAY_SIZE(number), gb );
    swprintf( out, count, load_string( IDS_DW_GB ), number );
}

/**********************************************************************
 *          The clock: the day, the date, the time
 */

/* Rainmeter's Mond, which this clock is: the day in Anurati at 40 points,
 * 10 pixels before and after each letter; the date and the time in
 * Quicksand Bold at 14 points; each line 5 pixels under the one above, all
 * white, in capitals, on one middle. Anurati is free for personal use only,
 * so it is not shipped: installed (for the user or in Windows' Fonts), the
 * day is in it, else in the letters drawn above. All of it at a scale: 1 is
 * the middle size on the desktop. */
#define DAY_POINTS   40
#define LINE_POINTS  14
#define DAY_GAP      .8f          /* the drawn letters apart, in heights */

static const float desktop_scale[W_SIZES] = { .7f, 1.f, 1.4f };

static BOOL have_anurati, have_quicksand, fonts_checked;

static int scaled( int value, float scale )
{
    return (int)(value * scale + .5f);
}

static int CALLBACK font_found( const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM found )
{
    *(BOOL *)found = TRUE;
    return 0;
}

static BOOL font_exists( const WCHAR *face )
{
    LOGFONTW lf = { 0 };
    BOOL found = FALSE;
    HDC hdc = GetDC( NULL );

    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW( lf.lfFaceName, face, ARRAY_SIZE(lf.lfFaceName) );
    EnumFontFamiliesExW( hdc, &lf, font_found, (LPARAM)&found, 0 );
    ReleaseDC( NULL, hdc );
    return found;
}

/* a font installed for the user only, or put in Windows' Fonts since this
 * process started: loaded for it */
static BOOL font_load( const WCHAR *face, const WCHAR *file )
{
    WCHAR path[MAX_PATH];
    UINT len;

    if (font_exists( face )) return TRUE;
    if ((len = GetEnvironmentVariableW( L"LOCALAPPDATA", path, MAX_PATH )) && len < MAX_PATH - 64)
    {
        swprintf( path + len, MAX_PATH - len, L"\\Microsoft\\Windows\\Fonts\\%s", file );
        AddFontResourceExW( path, FR_PRIVATE, NULL );
    }
    if ((len = GetWindowsDirectoryW( path, MAX_PATH )) && len < MAX_PATH - 64)
    {
        swprintf( path + len, MAX_PATH - len, L"\\Fonts\\%s", file );
        AddFontResourceExW( path, FR_PRIVATE, NULL );
    }
    return font_exists( face );
}

static void clock_fonts(void)
{
    if (fonts_checked) return;
    have_anurati = font_load( L"Anurati", L"Anurati.otf" );
    have_quicksand = font_load( L"Quicksand", L"Quicksand-Bold.otf" );
    fonts_checked = TRUE;
}

static HFONT point_font( const WCHAR *face, float points, int weight )
{
    LOGFONTW lf = { 0 };

    lf.lfHeight = -max( 1, (int)(points * px( 96 ) / 72.f + .5f) );
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = ANTIALIASED_QUALITY;
    lstrcpynW( lf.lfFaceName, face, ARRAY_SIZE(lf.lfFaceName) );
    return CreateFontIndirectW( &lf );
}

static HFONT line_font( float scale )
{
    return point_font( have_quicksand ? L"Quicksand" : L"Inter", LINE_POINTS * scale, FW_BOLD );
}

static const WCHAR *clock_locale(void)
{
    if (clock_english) return L"en-US";
    switch (PRIMARYLANGID( GetUserDefaultUILanguage() ))
    {
    case LANG_UKRAINIAN: return L"uk-UA";
    case LANG_RUSSIAN: return L"ru-RU";
    }
    return L"en-US";
}

static void day_name( const SYSTEMTIME *st, WCHAR *out, int count )
{
    const WCHAR *locale = clock_locale();

    if (!GetDateFormatEx( locale, 0, st, L"dddd", out, count, NULL )) out[0] = 0;
    CharUpperW( out );
    if (!letters_cover( out ))
    {
        GetDateFormatEx( L"en-US", 0, st, L"dddd", out, count, NULL );
        CharUpperW( out );
    }
}

/* Anurati has the Latin capitals only */
static BOOL day_in_anurati( const WCHAR *name )
{
    if (!have_anurati) return FALSE;
    for (; *name; name++) if (*name != ' ' && (*name < 'A' || *name > 'Z')) return FALSE;
    return TRUE;
}

static void date_line( const SYSTEMTIME *st, WCHAR *out, int count )
{
    WCHAR date[96];

    if (clock_english || !wcscmp( clock_locale(), L"en-US" ))
    {
        /* Mond's "%d  %B,  %Y", and the point of the picture it is known by */
        if (!GetDateFormatEx( L"en-US", 0, st, L"dd  MMMM,  yyyy", date, ARRAY_SIZE(date), NULL )) date[0] = 0;
    }
    else if (!GetDateFormatEx( clock_locale(), 0, st, L"d MMMM, yyyy", date, ARRAY_SIZE(date), NULL )) date[0] = 0;
    CharUpperW( date );
    swprintf( out, count, L"%s.", date );
}

static void time_line( const SYSTEMTIME *st, WCHAR *out, int count )
{
    WCHAR time[64];

    if (clock_english || !wcscmp( clock_locale(), L"en-US" ))
    {
        if (!GetTimeFormatEx( L"en-US", 0, st, L"hh:mm tt", time, ARRAY_SIZE(time) )) time[0] = 0;
    }
    else if (!GetTimeFormatEx( clock_locale(), TIME_NOSECONDS, st, NULL, time, ARRAY_SIZE(time) )) time[0] = 0;
    CharUpperW( time );
    swprintf( out, count, L"- %s -", time );
}

/* the day's line: its width and height, its letters drawn in the middle of
 * left and width when hdc is given */
static void day_line( HDC hdc, const WCHAR *name, int left, int width, int y, float scale, COLORREF color, SIZE *size )
{
    int spacing = scaled( px( 10 ), scale );

    if (day_in_anurati( name ))
    {
        HFONT font = point_font( L"Anurati", DAY_POINTS * scale, FW_NORMAL );
        HDC screen = hdc ? NULL : GetDC( NULL ), dc = hdc ? hdc : screen;
        HGDIOBJ old = SelectObject( dc, font );
        TEXTMETRICW tm;

        SetTextCharacterExtra( dc, spacing * 2 );
        GetTextExtentPoint32W( dc, name, lstrlenW( name ), size );
        GetTextMetricsW( dc, &tm );
        size->cy = tm.tmHeight + tm.tmExternalLeading;
        if (hdc)
        {
            SetTextColor( hdc, color );
            SetBkMode( hdc, TRANSPARENT );
            TextOutW( hdc, left + (width - size->cx) / 2 + spacing, y, name, lstrlenW( name ) );
        }
        SetTextCharacterExtra( dc, 0 );
        SelectObject( dc, old );
        DeleteObject( font );
        if (screen) ReleaseDC( NULL, screen );
        return;
    }
    {
        int height = max( 8, scaled( px( 40 ), scale ) ), stroke = max( 2, height * 7 / 100 );
        struct mask m;

        m.height = height * 115 / 100 + stroke + 2;
        m.width = (int)(word_width( name, DAY_GAP ) * height) + stroke + 2;
        size->cx = m.width;
        size->cy = m.height;
        if (!hdc || !(m.bits = calloc( m.width, m.height ))) return;
        mask_word( &m, name, stroke / 2.f + 1, stroke / 2.f + 1, (float)height, DAY_GAP, (float)stroke );
        blend_mask( hdc, &m, left + (width - m.width) / 2, y, color );
        free( m.bits );
    }
}

/* a line of the date or the time: its height drawn, or its width */
static int text_line( HDC hdc, const WCHAR *text, int left, int width, int y, float scale, COLORREF color )
{
    HFONT font = line_font( scale );
    HDC screen = hdc ? NULL : GetDC( NULL ), dc = hdc ? hdc : screen;
    HGDIOBJ old = SelectObject( dc, font );
    TEXTMETRICW tm;
    SIZE size;

    GetTextMetricsW( dc, &tm );
    GetTextExtentPoint32W( dc, text, lstrlenW( text ), &size );
    if (hdc)
    {
        SetTextColor( hdc, color );
        SetBkMode( hdc, TRANSPARENT );
        TextOutW( hdc, left + (width - size.cx) / 2, y, text, lstrlenW( text ) );
    }
    SelectObject( dc, old );
    DeleteObject( font );
    if (screen) ReleaseDC( NULL, screen );
    return hdc ? tm.tmHeight + tm.tmExternalLeading : size.cx;
}

/* wide enough for the longest day of the language */
static void clock_size( float scale, int *width, int *height )
{
    SYSTEMTIME st, day;
    FILETIME ft;
    ULARGE_INTEGER t;
    WCHAR name[64], line[96];
    int widest = 0, day_high = 0, line_high, pad = scaled( px( 16 ), scale ), gap = scaled( px( 5 ), scale );
    TEXTMETRICW tm;
    HDC screen;
    HFONT font;
    HGDIOBJ old;
    SIZE size;

    clock_fonts();
    GetLocalTime( &st );
    SystemTimeToFileTime( &st, &ft );
    for (int i = 0; i < 7; i++)
    {
        FILETIME next;

        t.LowPart = ft.dwLowDateTime;
        t.HighPart = ft.dwHighDateTime;
        t.QuadPart += (ULONGLONG)i * 24 * 3600 * 10000000;
        next.dwLowDateTime = t.LowPart;
        next.dwHighDateTime = t.HighPart;
        FileTimeToSystemTime( &next, &day );
        day_name( &day, name, ARRAY_SIZE(name) );
        day_line( NULL, name, 0, 0, 0, scale, 0, &size );
        widest = max( widest, size.cx );
        day_high = max( day_high, size.cy );
    }
    date_line( &st, line, ARRAY_SIZE(line) );
    widest = max( widest, text_line( NULL, line, 0, 0, 0, scale, 0 ) + scaled( px( 80 ), scale ) );

    screen = GetDC( NULL );
    font = line_font( scale );
    old = SelectObject( screen, font );
    GetTextMetricsW( screen, &tm );
    line_high = tm.tmHeight + tm.tmExternalLeading;
    SelectObject( screen, old );
    DeleteObject( font );
    ReleaseDC( NULL, screen );

    *width = widest + pad * 2;
    *height = pad + day_high + gap + line_high + gap + line_high + pad;
}

/* in the middle of r */
static void paint_clock( HDC hdc, const RECT *r, float scale, COLORREF color )
{
    WCHAR name[64], line[96];
    int width, height, y, gap = scaled( px( 5 ), scale );
    SIZE size;

    clock_size( scale, &width, &height );
    y = r->top + (r->bottom - r->top - height) / 2 + scaled( px( 16 ), scale );
    GetLocalTime( &shown_time );
    day_name( &shown_time, name, ARRAY_SIZE(name) );
    day_line( hdc, name, r->left, r->right - r->left, y, scale, color, &size );
    y += size.cy + gap;
    date_line( &shown_time, line, ARRAY_SIZE(line) );
    y += text_line( hdc, line, r->left, r->right - r->left, y, scale, color ) + gap;
    time_line( &shown_time, line, ARRAY_SIZE(line) );
    text_line( hdc, line, r->left, r->right - r->left, y, scale, color );
}

/* on the desktop the size says the scale; on the board the clock fills its card */
static float clock_scale( const struct widget_paint *p )
{
    int width, height;
    float fit;

    if (p->desktop) return desktop_scale[p->size];
    clock_size( 1.f, &width, &height );
    fit = min( (float)(p->rect.right - p->rect.left) / width, (float)(p->rect.bottom - p->rect.top) / height );
    return max( .3f, min( fit, desktop_scale[p->size] + .2f ) );
}

/**********************************************************************
 *          The monitoring and the battery: rings
 */

#define CARD_PAD   px( 14 )

struct row
{
    float fraction;
    WCHAR center[16];
    WCHAR title[64];
    WCHAR detail[96];
    COLORREF color;
};

static int monitor_rows( struct row *rows, int max_rows )
{
    struct arctic_sensors s;
    BOOL have;
    int count = 0;
    WCHAR a[32], b[32];

    AcquireSRWLockShared( &sensors_lock );
    s = sensors;
    have = have_sensors;
    ReleaseSRWLockShared( &sensors_lock );

    if ((monitor_items & ITEM_CPU) && count < max_rows)
    {
        struct row *r = &rows[count++];
        UINT32 load = have ? s.cpu_load_percent : ARCTIC_UNKNOWN;

        memset( r, 0, sizeof(*r) );
        r->fraction = load != ARCTIC_UNKNOWN ? load / 100.f : 0;
        if (load != ARCTIC_UNKNOWN) swprintf( r->center, ARRAY_SIZE(r->center), L"%u%%", load );
        lstrcpynW( r->title, load_string( IDS_DW_CPU ), ARRAY_SIZE(r->title) );
        if (have && s.cpu_temperature_mc != ARCTIC_UNKNOWN)
            swprintf( r->detail, ARRAY_SIZE(r->detail), L"%u \x00b0" L"C", (s.cpu_temperature_mc + 500) / 1000 );
        if (have && s.cpu_mhz != ARCTIC_UNKNOWN && s.cpu_mhz)
        {
            format_decimal( a, ARRAY_SIZE(a), s.cpu_mhz / 1000.0 );
            swprintf( b, ARRAY_SIZE(b), load_string( IDS_DW_GHZ ), a );
            if (r->detail[0]) wcscat( r->detail, L"  \x00b7  " );
            wcscat( r->detail, b );
        }
    }
    if ((monitor_items & ITEM_GPU) && count < max_rows)
    {
        struct row *r = &rows[count++];
        UINT32 load = have ? s.gpu_load_percent : ARCTIC_UNKNOWN;

        memset( r, 0, sizeof(*r) );
        r->fraction = load != ARCTIC_UNKNOWN ? load / 100.f : 0;
        if (load != ARCTIC_UNKNOWN) swprintf( r->center, ARRAY_SIZE(r->center), L"%u%%", load );
        lstrcpynW( r->title, have && s.gpu_name[0] ? s.gpu_name : load_string( IDS_DW_GPU ), ARRAY_SIZE(r->title) );
        if (have && s.gpu_temperature_mc != ARCTIC_UNKNOWN)
            swprintf( r->detail, ARRAY_SIZE(r->detail), L"%u \x00b0" L"C", (s.gpu_temperature_mc + 500) / 1000 );
        if (have && s.gpu_mhz != ARCTIC_UNKNOWN && s.gpu_mhz)
        {
            swprintf( b, ARRAY_SIZE(b), load_string( IDS_DW_MHZ ), s.gpu_mhz );
            if (r->detail[0]) wcscat( r->detail, L"  \x00b7  " );
            wcscat( r->detail, b );
        }
    }
    if ((monitor_items & ITEM_RAM) && count < max_rows)
    {
        struct row *r = &rows[count++];
        MEMORYSTATUSEX mem = { sizeof(mem) };

        memset( r, 0, sizeof(*r) );
        lstrcpynW( r->title, load_string( IDS_DW_RAM ), ARRAY_SIZE(r->title) );
        if (GlobalMemoryStatusEx( &mem ) && mem.ullTotalPhys)
        {
            double total = mem.ullTotalPhys / 1073741824.0, used = (mem.ullTotalPhys - mem.ullAvailPhys) / 1073741824.0;

            r->fraction = (float)(used / total);
            swprintf( r->center, ARRAY_SIZE(r->center), L"%u%%", (UINT)(r->fraction * 100 + .5f) );
            format_decimal( a, ARRAY_SIZE(a), used );
            format_gb( b, ARRAY_SIZE(b), total );
            swprintf( r->detail, ARRAY_SIZE(r->detail), L"%s / %s", a, b );
        }
    }
    if ((monitor_items & ITEM_DISK) && count < max_rows)
    {
        struct row *r = &rows[count++];
        ULARGE_INTEGER avail, total, free_bytes;

        memset( r, 0, sizeof(*r) );
        lstrcpynW( r->title, load_string( IDS_DW_DISK ), ARRAY_SIZE(r->title) );
        if (GetDiskFreeSpaceExW( L"C:\\", &avail, &total, &free_bytes ) && total.QuadPart)
        {
            r->fraction = (float)(1.0 - (double)free_bytes.QuadPart / total.QuadPart);
            swprintf( r->center, ARRAY_SIZE(r->center), L"%u%%", (UINT)(r->fraction * 100 + .5f) );
            format_gb( a, ARRAY_SIZE(a), free_bytes.QuadPart / 1073741824.0 );
            format_gb( b, ARRAY_SIZE(b), total.QuadPart / 1073741824.0 );
            swprintf( r->detail, ARRAY_SIZE(r->detail), load_string( IDS_DW_FREE ), a, b );
        }
    }
    return count;
}

static void battery_row( struct row *row )
{
    SYSTEM_POWER_STATUS status;

    memset( row, 0, sizeof(*row) );
    lstrcpynW( row->title, load_string( IDS_DW_BATTERY ), ARRAY_SIZE(row->title) );
    if (!GetSystemPowerStatus( &status ) || (status.BatteryFlag & 128) || status.BatteryLifePercent > 100)
    {
        lstrcpynW( row->detail, load_string( IDS_DW_NO_BATTERY ), ARRAY_SIZE(row->detail) );
        return;
    }
    row->fraction = status.BatteryLifePercent / 100.f;
    swprintf( row->center, ARRAY_SIZE(row->center), L"%u%%", status.BatteryLifePercent );
    if (status.BatteryFlag & 8)
    {
        lstrcpynW( row->detail, load_string( IDS_DW_CHARGING ), ARRAY_SIZE(row->detail) );
        row->color = RGB( 0x6c, 0xcb, 0x5f );
    }
    else if (status.ACLineStatus == 1)
        lstrcpynW( row->detail, load_string( IDS_DW_PLUGGED ), ARRAY_SIZE(row->detail) );
    else if (status.BatteryLifeTime != (DWORD)-1)
        swprintf( row->detail, ARRAY_SIZE(row->detail), load_string( IDS_DW_REMAINING ),
                  status.BatteryLifeTime / 3600, status.BatteryLifeTime / 60 % 60 );
    else
        lstrcpynW( row->detail, load_string( IDS_DW_ON_BATTERY ), ARRAY_SIZE(row->detail) );
    if (status.ACLineStatus != 1 && status.BatteryLifePercent <= 20) row->color = RGB( 0xf7, 0x63, 0x0c );
}

/* the batteries of the Bluetooth devices connected: headphones, earbuds and
 * their case, a mouse; winebth.sys's, as the network flyout lists them */
#define MAX_BT 4

struct bt_battery
{
    WCHAR name[64];
    BYTE level, left, right, case_level, charging;   /* charging: a bit each, in that order */
};

static struct bt_battery bt_batteries[MAX_BT];
static int bt_count;

static void read_bluetooth(void)
{
    static HMODULE module;
    static HBLUETOOTH_RADIO_FIND (WINAPI *find_radio)( const BLUETOOTH_FIND_RADIO_PARAMS *, HANDLE * );
    static BOOL (WINAPI *radio_close)( HBLUETOOTH_RADIO_FIND );
    static HBLUETOOTH_DEVICE_FIND (WINAPI *find_first)( const BLUETOOTH_DEVICE_SEARCH_PARAMS *, BLUETOOTH_DEVICE_INFO * );
    static BOOL (WINAPI *find_next)( HBLUETOOTH_DEVICE_FIND, BLUETOOTH_DEVICE_INFO * );
    static BOOL (WINAPI *find_close)( HBLUETOOTH_DEVICE_FIND );
    BLUETOOTH_FIND_RADIO_PARAMS radio_params = { sizeof(radio_params) };
    BLUETOOTH_DEVICE_SEARCH_PARAMS search = { sizeof(search), TRUE, TRUE, FALSE, TRUE, FALSE, 0 };
    BLUETOOTH_DEVICE_INFO info = { sizeof(info) };
    struct winebth_radio_state state = { 0 };
    struct bt_battery found[MAX_BT];
    HBLUETOOTH_RADIO_FIND radio_find;
    HBLUETOOTH_DEVICE_FIND device_find;
    HANDLE radio = NULL;
    DWORD bytes;
    int count = 0;

    /* loaded when needed: a machine without Bluetooth never pays for it */
    if (!module && (module = LoadLibraryW( L"bluetoothapis.dll" )))
    {
        find_radio = (void *)GetProcAddress( module, "BluetoothFindFirstRadio" );
        radio_close = (void *)GetProcAddress( module, "BluetoothFindRadioClose" );
        find_first = (void *)GetProcAddress( module, "BluetoothFindFirstDevice" );
        find_next = (void *)GetProcAddress( module, "BluetoothFindNextDevice" );
        find_close = (void *)GetProcAddress( module, "BluetoothFindDeviceClose" );
    }
    if (find_radio && radio_close && find_first && find_next && find_close &&
        (radio_find = find_radio( &radio_params, &radio )))
    {
        radio_close( radio_find );
        search.hRadio = radio;
        DeviceIoControl( radio, IOCTL_WINEBTH_RADIO_GET_STATE, NULL, 0, &state, sizeof(state), &bytes, NULL );
        if ((device_find = find_first( &search, &info )))
        {
            do
            {
                union { BTH_ADDR address; struct winebth_device_battery battery; } io;
                BOOL audio = state.audio_device && state.audio_device == info.Address.ullLong;

                if (!info.fAuthenticated || !(info.fConnected || audio)) continue;
                memset( &io, 0, sizeof(io) );
                io.address = info.Address.ullLong;
                if (!DeviceIoControl( radio, IOCTL_WINEBTH_RADIO_GET_BATTERY, &io, sizeof(io), &io, sizeof(io), &bytes, NULL ) ||
                    bytes < sizeof(io.battery))
                    continue;
                if (io.battery.level == WINEBTH_BATTERY_UNKNOWN && io.battery.left == WINEBTH_BATTERY_UNKNOWN &&
                    io.battery.right == WINEBTH_BATTERY_UNKNOWN)
                    continue;
                lstrcpynW( found[count].name, info.szName, ARRAY_SIZE(found[count].name) );
                found[count].level = io.battery.level;
                found[count].left = io.battery.left;
                found[count].right = io.battery.right;
                found[count].case_level = io.battery.case_level;
                found[count].charging = io.battery.charging;
                count++;
            } while (count < MAX_BT && (info.dwSize = sizeof(info), find_next( device_find, &info )));
            find_close( device_find );
        }
        CloseHandle( radio );
    }
    AcquireSRWLockExclusive( &sensors_lock );
    memcpy( bt_batteries, found, count * sizeof(*found) );
    bt_count = count;
    ReleaseSRWLockExclusive( &sensors_lock );
}

static void add_part( WCHAR *detail, int size, UINT id, BYTE value )
{
    WCHAR part[48];

    if (value == WINEBTH_BATTERY_UNKNOWN) return;
    swprintf( part, ARRAY_SIZE(part), load_string( id ), value );
    if (detail[0]) wcsncat( detail, L"  \x00b7  ", size - lstrlenW( detail ) - 1 );
    wcsncat( detail, part, size - lstrlenW( detail ) - 1 );
}

/* a device: its own level in the ring, or the earbud with less, and each
 * of the earbuds and their case beside it */
static void bt_row( struct row *row, const struct bt_battery *b )
{
    BYTE ring = b->level;

    memset( row, 0, sizeof(*row) );
    lstrcpynW( row->title, b->name, ARRAY_SIZE(row->title) );
    if (ring == WINEBTH_BATTERY_UNKNOWN)
    {
        if (b->left != WINEBTH_BATTERY_UNKNOWN && b->right != WINEBTH_BATTERY_UNKNOWN) ring = min( b->left, b->right );
        else ring = b->left != WINEBTH_BATTERY_UNKNOWN ? b->left : b->right;
    }
    row->fraction = ring / 100.f;
    swprintf( row->center, ARRAY_SIZE(row->center), L"%u%%", ring );
    add_part( row->detail, ARRAY_SIZE(row->detail), IDS_WW_LEFT, b->left );
    add_part( row->detail, ARRAY_SIZE(row->detail), IDS_WW_RIGHT, b->right );
    add_part( row->detail, ARRAY_SIZE(row->detail), IDS_WW_CASE, b->case_level );
    if (!row->detail[0] && (b->charging & 1)) lstrcpynW( row->detail, load_string( IDS_DW_CHARGING ), ARRAY_SIZE(row->detail) );
    if (b->charging) row->color = RGB( 0x6c, 0xcb, 0x5f );
    else if (ring <= 20) row->color = RGB( 0xf7, 0x63, 0x0c );
}

/* this machine's battery, if it has one, and the devices' */
static int battery_rows( struct row *rows, int max_rows )
{
    struct bt_battery bt[MAX_BT];
    SYSTEM_POWER_STATUS status;
    BOOL own = GetSystemPowerStatus( &status ) && !(status.BatteryFlag & 128) && status.BatteryLifePercent <= 100;
    int count = 0, devices;

    AcquireSRWLockShared( &sensors_lock );
    devices = bt_count;
    memcpy( bt, bt_batteries, devices * sizeof(*bt) );
    ReleaseSRWLockShared( &sensors_lock );
    if (own || !devices) battery_row( &rows[count++] );
    for (int i = 0; i < devices && count < max_rows; i++) bt_row( &rows[count++], &bt[i] );
    return count;
}

static COLORREF ring_track( const struct palette *pal )
{
    return pal->light ? RGB( 0xd8, 0xd8, 0xd8 ) : RGB( 0x44, 0x44, 0x44 );
}

/* rows of a ring and two lines beside it, in so many columns */
static void paint_rows( HDC hdc, const struct palette *pal, const RECT *area, const struct row *rows, int count, int columns,
                        int ring, BOOL big )
{
    HFONT title = shell_font( px( big ? 16 : 14 ), FW_SEMIBOLD ), detail_font = shell_font( px( big ? 13 : 12 ), FW_NORMAL );
    HFONT ring_font = shell_font( px( big ? 16 : ring >= px( 24 ) ? 12 : 11 ), FW_SEMIBOLD );
    COLORREF accent = accent_color();
    int lines = (max( count, 1 ) + columns - 1) / columns;
    int cell_w = (area->right - area->left) / columns, cell_h = (area->bottom - area->top) / lines;
    RECT r;

    for (int i = 0; i < count; i++)
    {
        int left = area->left + (i % columns) * cell_w, top = area->top + (i / columns) * cell_h;
        int cx = left + ring + 1, cy = top + cell_h / 2, x = cx + ring + px( big ? 16 : 14 );
        int right = left + cell_w - px( 4 ) - (i / columns == 0 && i % columns == columns - 1 ? px( 26 ) : 0);

        paint_ring( hdc, cx, cy, ring, max( px( 4 ), ring / 6 ), rows[i].fraction, ring_track( pal ),
                    rows[i].color ? rows[i].color : accent );
        SetRect( &r, cx - ring, cy - ring, cx + ring, cy + ring );
        paint_text( hdc, rows[i].center, &r, ring_font, pal->text, DT_CENTER );
        SetRect( &r, x, cy - px( big ? 22 : 19 ), right, cy );
        paint_text( hdc, rows[i].title, &r, title, pal->text, DT_LEFT | DT_END_ELLIPSIS );
        SetRect( &r, x, cy, right, cy + px( big ? 20 : 18 ) );
        paint_text( hdc, rows[i].detail, &r, detail_font, pal->dim, DT_LEFT | DT_END_ELLIPSIS );
    }
    DeleteObject( title );
    DeleteObject( detail_font );
    DeleteObject( ring_font );
}

/* the battery, tall: one large ring, what it says under it */
static void paint_battery_big( HDC hdc, const struct palette *pal, const RECT *area, const struct row *row )
{
    HFONT number = shell_font( px( 30 ), FW_SEMIBOLD ), title = shell_font( px( 16 ), FW_SEMIBOLD );
    HFONT detail = shell_font( px( 13 ), FW_NORMAL );
    int ring = px( 64 ), total = 2 * ring + px( 14 + 22 + 4 + 20 );
    int cx = (area->left + area->right) / 2, top = area->top + (area->bottom - area->top - total) / 2, cy = top + ring;
    RECT r;

    paint_ring( hdc, cx, cy, ring, px( 8 ), row->fraction, ring_track( pal ), row->color ? row->color : accent_color() );
    SetRect( &r, cx - ring, cy - ring, cx + ring, cy + ring );
    paint_text( hdc, row->center, &r, number, pal->text, DT_CENTER );
    SetRect( &r, area->left, cy + ring + px( 14 ), area->right, cy + ring + px( 36 ) );
    paint_text( hdc, row->title, &r, title, pal->text, DT_CENTER );
    SetRect( &r, area->left, cy + ring + px( 40 ), area->right, cy + ring + px( 60 ) );
    paint_text( hdc, row->detail, &r, detail, pal->dim, DT_CENTER | DT_END_ELLIPSIS );
    DeleteObject( number );
    DeleteObject( title );
    DeleteObject( detail );
}

/* the clock, the monitoring or the battery, into p->rect */
void widget_paint_local( struct widget_paint *p )
{
    struct row rows[4];
    RECT area = p->rect;
    int count;

    if (!p->desktop)
        fill_round_rect( p->hdc, &p->rect, (float)(p->style11 ? px( 8 ) : 0), p->pal->ink, p->pal->light ? 0x0a : 0x10 );
    InflateRect( &area, -CARD_PAD, -CARD_PAD );
    switch (p->kind)
    {
    case W_CLOCK:
        paint_clock( p->hdc, &p->rect, clock_scale( p ), p->desktop ? RGB( 255, 255, 255 ) : p->pal->text );
        break;
    case W_MONITOR:
        widget_add_target( p, HIT_CARD, 0, &p->rect );
        if (p->size == W_LARGE)
        {
            InflateRect( &area, -px( 8 ), -px( 8 ) );
            count = monitor_rows( rows, 4 );
            paint_rows( p->hdc, p->pal, &area, rows, count, 2, px( 40 ), TRUE );
        }
        else if (p->size == W_MEDIUM)
        {
            count = monitor_rows( rows, 4 );
            paint_rows( p->hdc, p->pal, &area, rows, count, 1, px( 24 ), FALSE );
        }
        else
        {
            count = monitor_rows( rows, 2 );
            paint_rows( p->hdc, p->pal, &area, rows, count, 1, px( 21 ), FALSE );
        }
        break;
    case W_BATTERY:
        widget_add_target( p, HIT_CARD, 0, &p->rect );
        count = battery_rows( rows, p->size == W_MEDIUM ? 4 : 2 );
        if (p->size == W_MEDIUM && count == 1) paint_battery_big( p->hdc, p->pal, &area, rows );
        else paint_rows( p->hdc, p->pal, &area, rows, count, 1, p->size == W_MEDIUM ? px( 24 ) : count > 1 ? px( 21 ) : px( 28 ),
                         FALSE );
        break;
    default:
        break;
    }
}

/**********************************************************************
 *          The desktop's windows: one a widget
 */

struct gadget
{
    HWND hwnd;
    struct widget w;
    BOOL hot, pressed, light, more_hot;
    int width, height;
    struct widget_target targets[WIDGET_TARGETS], hot_target;
    int target_count;
    struct widget_ask ask;
};

static struct gadget gadgets[W_KINDS];

static enum widget_kind gadget_of( HWND hwnd )
{
    for (int i = 0; i < W_KINDS; i++) if (gadgets[i].hwnd == hwnd) return i;
    return W_KINDS;
}

static BOOL is_local( enum widget_kind kind )
{
    return kind == W_CLOCK || kind == W_MONITOR || kind == W_BATTERY;
}

/* what lies under "...": a picture or the weather's colours */
static BOOL opaque_top( enum widget_kind kind, BOOL style11 )
{
    return kind == W_NEWS || (kind == W_WEATHER && style11);
}

/* its look: when it comes, and when the mode changes */
static void gadget_look( enum widget_kind kind )
{
    struct look look = { 0 };
    DWORD corner;

    if (kind == W_CLOCK)
    {
        /* nothing behind the letters: white on what the desktop shows */
        look.effect = ARCTIC_ACCENT_ENABLE_TRANSPARENTGRADIENT;
        look.color = RGB( 0, 0, 0 );
        look.opacity = 0;
        look.blur = 1;
        look.text = TEXT_WHITE;
        corner = 1;   /* DWMWCP_DONOTROUND: no shadow either */
    }
    else
    {
        BOOL mode_light = system_mode_light();

        look.effect = ARCTIC_ACCENT_ENABLE_BLURBEHIND;
        look.color = mode_light ? RGB( 0xf3, 0xf3, 0xf3 ) : RGB( 0x1c, 0x1c, 0x1c );
        look.opacity = 62;
        look.blur = 70;
        look.text = mode_light ? TEXT_BLACK : TEXT_WHITE;
        corner = taskbar_style11() ? ARCTIC_FLYOUT_CORNER : 1;
    }
    DwmSetWindowAttribute( gadgets[kind].hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner) );
    gadgets[kind].light = look_apply( gadgets[kind].hwnd, &look );
}

static void gadget_paint( HWND hwnd )
{
    enum widget_kind kind = gadget_of( hwnd );
    struct widget_paint p = { 0 };
    struct gadget *g;
    struct palette pal;
    PAINTSTRUCT ps;
    HBITMAP bitmap;
    HGDIOBJ old;
    RECT r;
    HDC mem;

    if (kind == W_KINDS) return;
    g = &gadgets[kind];
    palette_init( &pal, g->light );
    if (!(mem = paint_begin( hwnd, &ps, &pal, &bitmap, &old ))) return;
    SetRect( &r, 0, 0, g->width, g->height );
    if (g->hot || g->pressed)
    {
        if (kind == W_CLOCK) paint_frame( mem, bitmap, &r, (float)px( 8 ), g->pressed ? 0x30 : 0x20, g->pressed ? 0xa0 : 0x78, FALSE );
        else paint_frame( mem, bitmap, &r, (float)px( 8 ), g->pressed ? 0x18 : 0x0c, g->pressed ? 0x90 : 0x68, pal.light );
    }
    p.hdc = mem;
    p.pal = &pal;
    p.rect = r;
    p.kind = kind;
    p.size = g->w.size;
    p.style11 = taskbar_style11();
    p.desktop = TRUE;
    p.targets = g->targets;
    p.ask = &g->ask;
    if (is_local( kind )) widget_paint_local( &p );
    else widget_paint_net( &p );
    g->target_count = p.target_count;
    if (g->hot && !g->more_hot && (g->hot_target.hit == HIT_STORY || (g->hot_target.hit == HIT_CARD && !is_local( kind ))))
    {
        BOOL white = kind == W_WEATHER && p.style11;

        widget_tint( mem, &g->hot_target.rect, 0, white ? RGB( 255, 255, 255 ) : pal.ink, white ? 0x14 : 0x0c );
    }
    if (kind == W_COPILOT) widget_paint_ask( mem, &pal, &g->ask, g->hot_target.hit == HIT_SEND, p.style11, 0 );
    if (g->hot) widget_more_button( mem, &pal, &r, p.style11, g->more_hot, opaque_top( kind, p.style11 ) );
    paint_end( hwnd, &ps, mem, bitmap, old );
}

static void gadget_size( struct gadget *g )
{
    if (g->w.kind == W_CLOCK) clock_size( desktop_scale[g->w.size], &g->width, &g->height );
    else widget_extent( g->w.kind, g->w.size, &g->width, &g->height );
}

/* where it goes when it was never put anywhere: the clock above the middle,
 * the rest down the top right corner, one under the other */
static void default_position( enum widget_kind kind, int width, int height, int *x, int *y )
{
    MONITORINFO info = { sizeof(info) };
    POINT origin = { 0, 0 };

    GetMonitorInfoW( MonitorFromPoint( origin, MONITOR_DEFAULTTOPRIMARY ), &info );
    if (kind == W_CLOCK)
    {
        *x = (info.rcWork.left + info.rcWork.right - width) / 2;
        *y = info.rcWork.top + px( 48 );
        return;
    }
    *x = info.rcWork.right - width - px( 24 );
    *y = info.rcWork.top + px( 24 );
    for (int pass = 0; pass < W_KINDS; pass++)
    {
        RECT me, other, overlap;
        BOOL moved = FALSE;

        SetRect( &me, *x, *y, *x + width, *y + height );
        for (int k = 0; k < W_KINDS && !moved; k++)
        {
            if (k == kind || !gadgets[k].hwnd || !gadgets[k].w.placed || !GetWindowRect( gadgets[k].hwnd, &other )) continue;
            if (IntersectRect( &overlap, &me, &other ))
            {
                *y = other.bottom + px( 12 );
                moved = TRUE;
            }
        }
        if (!moved) break;
    }
}

/* where it stands now, in the desktop's list */
static void remember_position( enum widget_kind kind )
{
    struct gadget *g = &gadgets[kind];
    struct widget list[W_KINDS];
    int count, at;
    RECT r;

    if (!GetWindowRect( g->hwnd, &r )) return;
    g->w.placed = TRUE;
    g->w.x = kind == W_CLOCK ? (r.left + r.right) / 2 : r.left;   /* the clock keeps its middle: the days are not as long */
    g->w.y = r.top;
    count = widgets_load( HOST_DESKTOP, list );
    if ((at = find_kind( list, count, kind )) < 0) return;
    list[at].placed = TRUE;
    list[at].x = g->w.x;
    list[at].y = g->w.y;
    widgets_save( HOST_DESKTOP, list, count );
}

static void gadget_place( enum widget_kind kind )
{
    struct gadget *g = &gadgets[kind];
    int x, y;
    RECT virt, r;

    gadget_size( g );
    if (g->w.placed)
    {
        x = g->w.x - (kind == W_CLOCK ? g->width / 2 : 0);
        y = g->w.y;
    }
    else default_position( kind, g->width, g->height, &x, &y );

    /* not lost off the screens */
    SetRect( &virt, GetSystemMetrics( SM_XVIRTUALSCREEN ), GetSystemMetrics( SM_YVIRTUALSCREEN ),
             GetSystemMetrics( SM_XVIRTUALSCREEN ) + GetSystemMetrics( SM_CXVIRTUALSCREEN ),
             GetSystemMetrics( SM_YVIRTUALSCREEN ) + GetSystemMetrics( SM_CYVIRTUALSCREEN ) );
    SetRect( &r, x, y, x + g->width, y + g->height );
    if (r.right < virt.left + px( 40 ) || r.left > virt.right - px( 40 ) || r.top < virt.top || r.top > virt.bottom - px( 40 ))
        default_position( kind, g->width, g->height, &x, &y );
    else
    {
        /* a larger size where a smaller one stood: all of it on its screen */
        MONITORINFO info = { sizeof(info) };

        GetMonitorInfoW( MonitorFromRect( &r, MONITOR_DEFAULTTONEAREST ), &info );
        x = max( (int)info.rcWork.left, min( x, (int)info.rcWork.right - g->width ) );
        y = max( (int)info.rcWork.top, min( y, (int)info.rcWork.bottom - g->height ) );
    }
    SetWindowPos( g->hwnd, HWND_BOTTOM, x, y, g->width, g->height, SWP_NOACTIVATE );
    if (!g->w.placed) remember_position( kind );
}

/* Copilot's field takes the keyboard: the window may be activated meanwhile */
static void gadget_typing( struct gadget *g, BOOL on )
{
    LONG ex = GetWindowLongW( g->hwnd, GWL_EXSTYLE );

    if (g->ask.active == on) return;
    g->ask.active = g->ask.caret = on;
    if (on)
    {
        SetWindowLongW( g->hwnd, GWL_EXSTYLE, ex & ~WS_EX_NOACTIVATE );
        SetForegroundWindow( g->hwnd );
        SetFocus( g->hwnd );
        SetTimer( g->hwnd, TIMER_CARET, 530, NULL );
    }
    else
    {
        SetWindowLongW( g->hwnd, GWL_EXSTYLE, ex | WS_EX_NOACTIVATE );
        KillTimer( g->hwnd, TIMER_CARET );
    }
    InvalidateRect( g->hwnd, NULL, FALSE );
}

static void gadget_menu( enum widget_kind kind, POINT pt )
{
    gadgets[kind].hot = gadgets[kind].more_hot = FALSE;
    InvalidateRect( gadgets[kind].hwnd, NULL, FALSE );
    widget_menu( gadgets[kind].hwnd, pt, HOST_DESKTOP, kind );
}

static void update_hot( struct gadget *g, POINT pt )
{
    const struct widget_target *t = widget_hit_test( g->targets, g->target_count, pt );
    struct widget_target none = { HIT_NONE };
    BOOL more;
    RECT r, card;

    SetRect( &card, 0, 0, g->width, g->height );
    widget_more_rect( &card, &r );
    more = PtInRect( &r, pt );
    if (!t) t = &none;
    if (more == g->more_hot && t->hit == g->hot_target.hit && t->index == g->hot_target.index) return;
    g->more_hot = more;
    g->hot_target = *t;
    InvalidateRect( g->hwnd, NULL, FALSE );
}

static LRESULT CALLBACK gadget_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    enum widget_kind kind = gadget_of( hwnd );
    struct gadget *g = kind < W_KINDS ? &gadgets[kind] : NULL;
    POINT pt = { (short)LOWORD( lparam ), (short)HIWORD( lparam ) };

    switch (msg)
    {
    case WM_MOUSEACTIVATE:
    {
        POINT cursor;
        const struct widget_target *t;

        if (!g) return MA_NOACTIVATE;
        GetCursorPos( &cursor );
        ScreenToClient( hwnd, &cursor );
        t = widget_hit_test( g->targets, g->target_count, cursor );
        return t && t->hit == HIT_ASK ? MA_ACTIVATE : MA_NOACTIVATE;
    }

    case WM_WINDOWPOSCHANGING:
    {
        /* above the desktop, under every program: win32u keeps a popup over its owner */
        WINDOWPOS *wp = (WINDOWPOS *)lparam;

        if (!(wp->flags & SWP_NOZORDER)) wp->hwndInsertAfter = HWND_BOTTOM;
        break;
    }

    case WM_SETCURSOR:
    {
        POINT cursor;
        const struct widget_target *t;

        if (!g) break;
        GetCursorPos( &cursor );
        ScreenToClient( hwnd, &cursor );
        t = widget_hit_test( g->targets, g->target_count, cursor );
        if (t && t->hit == HIT_ASK)
        {
            SetCursor( LoadCursorW( NULL, (const WCHAR *)IDC_IBEAM ) );
            return TRUE;
        }
        break;
    }

    case WM_MOUSEMOVE:
        if (!g) return 0;
        if (!g->hot)
        {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };

            g->hot = TRUE;
            TrackMouseEvent( &tme );
            InvalidateRect( hwnd, NULL, FALSE );
        }
        update_hot( g, pt );
        return 0;

    case WM_MOUSELEAVE:
        if (g && g->hot && !g->pressed)
        {
            g->hot = g->more_hot = FALSE;
            g->hot_target.hit = HIT_NONE;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        return 0;

    case WM_LBUTTONDOWN:
    {
        const struct widget_target *t;
        RECT before, after, card, more;

        if (!g) break;
        SetRect( &card, 0, 0, g->width, g->height );
        widget_more_rect( &card, &more );
        if (PtInRect( &more, pt ))
        {
            POINT at = { more.left, more.bottom };

            ClientToScreen( hwnd, &at );
            gadget_menu( kind, at );
            return 0;
        }
        t = widget_hit_test( g->targets, g->target_count, pt );
        if (t && t->hit == HIT_ASK)
        {
            gadget_typing( g, TRUE );
            return 0;
        }
        g->pressed = TRUE;
        RedrawWindow( hwnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW );
        GetWindowRect( hwnd, &before );
        /* dragged by itself: the system moves it, from where the button went down */
        SendMessageW( hwnd, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, GetMessagePos() );
        g->pressed = FALSE;
        GetWindowRect( hwnd, &after );
        {
            POINT cursor;

            GetCursorPos( &cursor );
            g->hot = PtInRect( &after, cursor );
        }
        if (EqualRect( &before, &after ))
        {
            /* it did not move: a click on what is there */
            if ((t = widget_hit_test( g->targets, g->target_count, pt )))
            {
                struct widget_target copy = *t;

                widget_activate( kind, &copy, &g->ask );
            }
        }
        else remember_position( kind );
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    }

    case WM_RBUTTONUP:
        if (!g) break;
        ClientToScreen( hwnd, &pt );
        gadget_menu( kind, pt );
        return 0;

    case WM_KEYDOWN:
    case WM_CHAR:
        if (!g || !g->ask.active) break;
        switch (widget_ask_input( &g->ask, msg, wparam, hwnd ))
        {
        case ASK_SEND:
        {
            struct widget_target send = { HIT_SEND };

            widget_activate( W_COPILOT, &send, &g->ask );
            gadget_typing( g, FALSE );
            break;
        }
        case ASK_LEAVE:
            gadget_typing( g, FALSE );
            break;
        case ASK_CHANGED:
            g->ask.caret = TRUE;
            SetTimer( hwnd, TIMER_CARET, 530, NULL );
            InvalidateRect( hwnd, NULL, FALSE );
            break;
        default:
            break;
        }
        return 0;

    case WM_ACTIVATE:
        if (g && LOWORD( wparam ) == WA_INACTIVE) gadget_typing( g, FALSE );
        return 0;

    case WM_TIMER:
        if (g && wparam == TIMER_CARET)
        {
            g->ask.caret = !g->ask.caret;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        gadget_paint( hwnd );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

static void gadget_create( enum widget_kind kind )
{
    static const WCHAR class_name[] = L"ArcticDesktopWidget";
    static BOOL registered;
    HWND owner = GetShellWindow();
    DWORD transition = ARCTIC_TRANSITION_FADE;

    if (!registered)
    {
        WNDCLASSW cls = { 0 };

        cls.lpfnWndProc = gadget_proc;
        cls.hInstance = startui_instance;
        cls.hCursor = LoadCursorW( NULL, (LPCWSTR)IDC_ARROW );
        cls.lpszClassName = class_name;
        registered = RegisterClassW( &cls ) != 0;
    }
    if (!owner) owner = FindWindowW( L"Progman", NULL );
    gadgets[kind].hwnd = CreateWindowExW( WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, class_name, L"", WS_POPUP, 0, 0, 1, 1, owner,
                                          NULL, startui_instance, NULL );
    if (!gadgets[kind].hwnd) return;
    DwmSetWindowAttribute( gadgets[kind].hwnd, DWMWA_ARCTIC_TRANSITION, &transition, sizeof(transition) );
    gadget_look( kind );
    gadget_place( kind );
    ShowWindow( gadgets[kind].hwnd, SW_SHOWNOACTIVATE );
}

/**********************************************************************
 *          The thread of the widgets
 */

static void read_sensors(void)
{
    struct arctic_sensors s;
    BOOL have = ArcticSensors( &s );

    ArcticSensors( NULL );   /* NVIDIA's library goes: an open one keeps the card awake */
    AcquireSRWLockExclusive( &sensors_lock );
    sensors = s;
    have_sensors = have;
    ReleaseSRWLockExclusive( &sensors_lock );
}

static void gadgets_reload(void)
{
    struct widget list[W_KINDS];
    int count = widgets_load( HOST_DESKTOP, list );

    monitor_items = get_setting( L"MonitorItems", ITEM_ALL ) & ITEM_ALL;
    if (!monitor_items) monitor_items = ITEM_ALL;
    clock_english = get_setting( L"ClockEnglish", 1 );
    fonts_checked = FALSE;
    for (int k = 0; k < W_KINDS; k++)
    {
        struct gadget *g = &gadgets[k];
        int at = find_kind( list, count, k );

        if (at < 0)
        {
            if (g->hwnd) DestroyWindow( g->hwnd );
            memset( g, 0, sizeof(*g) );
            continue;
        }
        if (k == W_MONITOR && !g->hwnd) read_sensors();
        if (k == W_BATTERY && !g->hwnd) read_bluetooth();
        g->w = list[at];
        if (!g->hwnd) gadget_create( k );
        else
        {
            gadget_place( k );
            InvalidateRect( g->hwnd, NULL, FALSE );
        }
    }
}

static BOOL CALLBACK repaint_window( HWND hwnd, LPARAM pid )
{
    DWORD owner;

    GetWindowThreadProcessId( hwnd, &owner );
    if (owner == (DWORD)pid) RedrawWindow( hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN );
    return TRUE;
}

/* another icon theme: shell32 notices it at its next look-up (half a second
 * after the last at most) and makes the icons it keeps again; this process's
 * windows (the desktop, the taskbar, File Explorer) paint them */
static void icons_changed(void)
{
    SHFILEINFOW info;

    SHGetFileInfoW( L"C:\\", 0, &info, sizeof(info), SHGFI_SYSICONINDEX | SHGFI_SMALLICON );
    SHChangeNotify( SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL );
    EnumWindows( repaint_window, GetCurrentProcessId() );
}

static void invalidate_gadget( enum widget_kind kind )
{
    if (gadgets[kind].hwnd) InvalidateRect( gadgets[kind].hwnd, NULL, FALSE );
}

static LRESULT CALLBACK manager_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    static UINT reload_message;

    if (!reload_message) reload_message = RegisterWindowMessageW( RELOAD_MESSAGE );
    if (msg == reload_message)
    {
        gadgets_reload();
        widgets_board_reload();
        return 0;
    }
    switch (msg)
    {
    case WM_SETTINGCHANGE:
        if (lparam && !lstrcmpiW( (const WCHAR *)lparam, GADGETS_CHANGED ))
        {
            gadgets_reload();
            widgets_board_reload();
        }
        else if (lparam && !lstrcmpiW( (const WCHAR *)lparam, ICONS_CHANGED )) SetTimer( hwnd, TIMER_ICONS, 700, NULL );
        else for (int i = 0; i < W_KINDS; i++)
        {
            /* the mode, the accent: the cards follow */
            if (!gadgets[i].hwnd) continue;
            gadget_look( i );
            InvalidateRect( gadgets[i].hwnd, NULL, FALSE );
        }
        return 0;

    case WM_NET_DATA:
        invalidate_gadget( W_WEATHER );
        invalidate_gadget( W_NEWS );
        return 0;

    case WM_DISPLAYCHANGE:
        for (int i = 0; i < W_KINDS; i++) if (gadgets[i].hwnd) gadget_place( i );
        return 0;

    case WM_POWERBROADCAST:
        invalidate_gadget( W_BATTERY );
        if (widgets_board_has( W_BATTERY )) widgets_board_tick( FALSE );
        return TRUE;

    case WM_TIMER:
    {
        SYSTEMTIME now;
        BOOL board = FALSE, minute;

        if (wparam == TIMER_ICONS)
        {
            KillTimer( hwnd, TIMER_ICONS );
            icons_changed();
            return 0;
        }
        ticks++;
        GetLocalTime( &now );
        minute = now.wMinute != shown_time.wMinute || now.wDay != shown_time.wDay;
        if (minute)
        {
            invalidate_gadget( W_CLOCK );
            if (widgets_board_has( W_CLOCK )) board = TRUE;
            shown_time = now;
        }
        if (!(ticks % 2) && (gadgets[W_MONITOR].hwnd || widgets_board_has( W_MONITOR )))
        {
            read_sensors();
            invalidate_gadget( W_MONITOR );
            if (widgets_board_has( W_MONITOR )) board = TRUE;
        }
        {
            static BOOL board_battery;
            BOOL on_board = widgets_board_has( W_BATTERY );

            /* the devices' batteries: every ten seconds while shown, and at
             * once when the board opens with them */
            if ((on_board && !board_battery) || (!(ticks % 10) && (on_board || gadgets[W_BATTERY].hwnd)))
            {
                read_bluetooth();
                invalidate_gadget( W_BATTERY );
                if (on_board) board = TRUE;
            }
            board_battery = on_board;
        }
        if (board) widgets_board_tick( minute );
        return 0;
    }
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

static DWORD WINAPI gadgets_thread( void *arg )
{
    WNDCLASSW cls = { 0 };
    MSG msg;

    /* the desktop's window first: the widgets stay over it */
    for (int i = 0; i < 100 && !GetShellWindow() && !FindWindowW( L"Progman", NULL ); i++) Sleep( 100 );
    cls.lpfnWndProc = manager_proc;
    cls.hInstance = startui_instance;
    cls.lpszClassName = MANAGER_CLASS;
    RegisterClassW( &cls );
    /* a hidden window of its own: a broadcast reaches it */
    manager = CreateWindowExW( WS_EX_TOOLWINDOW, cls.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL, startui_instance, NULL );
    if (!manager) return 0;
    gadgets_reload();
    SetTimer( manager, TIMER_TICK, 1000, NULL );
    while (GetMessageW( &msg, NULL, 0, 0 ))
    {
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }
    return 0;
}

/* explorer's taskbar came: the widgets of the desktop start, once */
void gadgets_start(void)
{
    static LONG started;
    HANDLE thread;

    if (InterlockedExchange( &started, 1 )) return;
    if ((thread = CreateThread( NULL, 0, gadgets_thread, NULL, 0, NULL ))) CloseHandle( thread );
}

/**********************************************************************
 *          The settings: Display's "Widgets..." and a widget's menu
 */

enum
{
    IDC_DW_LANGUAGE = 100, IDC_DW_CPU, IDC_DW_GPU, IDC_DW_RAM, IDC_DW_DISK, IDC_DW_RESET,
    IDC_DW_KIND = 200, IDC_DW_SIZE = 300,
};

struct dialog
{
    HWND hwnd;
    BOOL done, ok, reset;
};

static BOOL kind_checked( HWND hwnd, enum widget_kind kind )
{
    return IsDlgButtonChecked( hwnd, IDC_DW_KIND + kind ) == BST_CHECKED;
}

static void dialog_enable( HWND hwnd )
{
    for (int k = 0; k < W_KINDS; k++) EnableWindow( GetDlgItem( hwnd, IDC_DW_SIZE + k ), kind_checked( hwnd, k ) );
    EnableWindow( GetDlgItem( hwnd, IDC_DW_LANGUAGE ), kind_checked( hwnd, W_CLOCK ) );
    for (int id = IDC_DW_CPU; id <= IDC_DW_DISK; id++) EnableWindow( GetDlgItem( hwnd, id ), kind_checked( hwnd, W_MONITOR ) );
}

static LRESULT CALLBACK dialog_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct dialog *d = (struct dialog *)GetWindowLongPtrW( hwnd, GWLP_USERDATA );

    switch (msg)
    {
    case WM_CREATE:
        SetWindowLongPtrW( hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lparam)->lpCreateParams );
        return 0;

    case WM_COMMAND:
        switch (LOWORD( wparam ))
        {
        case IDOK: d->ok = TRUE; d->done = TRUE; return 0;
        case IDCANCEL: d->done = TRUE; return 0;
        case IDC_DW_RESET: d->reset = TRUE; EnableWindow( (HWND)lparam, FALSE ); return 0;
        }
        if (LOWORD( wparam ) >= IDC_DW_KIND && LOWORD( wparam ) < IDC_DW_KIND + W_KINDS)
        {
            dialog_enable( hwnd );
            return 0;
        }
        break;

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetBkColor( (HDC)wparam, GetSysColor( COLOR_BTNFACE ) );
        SetTextColor( (HDC)wparam, GetSysColor( COLOR_BTNTEXT ) );
        return (LRESULT)GetSysColorBrush( COLOR_BTNFACE );

    case WM_CLOSE:
        if (d) d->done = TRUE;
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

static HWND dialog_control( HWND parent, const WCHAR *cls, const WCHAR *text, DWORD style, int id, int x, int y, int w, int h,
                            HFONT font )
{
    HWND control = CreateWindowExW( 0, cls, text, WS_CHILD | WS_VISIBLE | style, px( x ), px( y ), px( w ), px( h ), parent,
                                    (HMENU)(INT_PTR)id, startui_instance, NULL );

    SendMessageW( control, WM_SETFONT, (WPARAM)font, FALSE );
    return control;
}

static enum widget_size chosen_size( HWND hwnd, enum widget_kind kind )
{
    HWND combo = GetDlgItem( hwnd, IDC_DW_SIZE + kind );
    LRESULT sel = SendMessageW( combo, CB_GETCURSEL, 0, 0 );

    return sel == CB_ERR ? W_MEDIUM : (enum widget_size)SendMessageW( combo, CB_GETITEMDATA, sel, 0 );
}

/* which widgets stand on the desktop and how large */
void WINAPI ArcticDesktopWidgetsDialog( HWND parent )
{
    static const WCHAR class_name[] = L"ArcticDesktopWidgetsDialog";
    static const UINT size_names[W_SIZES] = { IDS_TILE_SMALL, IDS_TILE_MEDIUM, IDS_TILE_LARGE };
    static const struct { int id; DWORD bit; UINT name; } items[] =
    {
        { IDC_DW_CPU, ITEM_CPU, IDS_DW_CPU }, { IDC_DW_GPU, ITEM_GPU, IDS_DW_GPU },
        { IDC_DW_RAM, ITEM_RAM, IDS_DW_RAM }, { IDC_DW_DISK, ITEM_DISK, IDS_DW_DISK },
    };
    NONCLIENTMETRICSW metrics = { sizeof(metrics) };
    struct widget list[W_KINDS], out[W_KINDS];
    int count = widgets_load( HOST_DESKTOP, list ), n = 0, y = 44;
    struct dialog d = { 0 };
    WNDCLASSW cls = { 0 };
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, items_on = get_setting( L"MonitorItems", ITEM_ALL );
    RECT r, owner_rect;
    HWND combo = NULL, focus;
    HFONT font;
    MSG msg;

    /* a page of a property sheet: the sheet waits */
    if (parent) parent = GetAncestor( parent, GA_ROOT );
    cls.lpfnWndProc = dialog_proc;
    cls.hInstance = startui_instance;
    cls.hCursor = LoadCursorW( NULL, (LPCWSTR)IDC_ARROW );
    cls.hbrBackground = GetSysColorBrush( COLOR_BTNFACE );
    cls.lpszClassName = class_name;
    RegisterClassW( &cls );
    SystemParametersInfoW( SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0 );
    font = CreateFontIndirectW( &metrics.lfMessageFont );

    d.hwnd = CreateWindowExW( WS_EX_DLGMODALFRAME, class_name, load_string( IDS_WW_TITLE ), style, 0, 0, 100, 100, parent, NULL,
                              startui_instance, &d );
    if (!d.hwnd)
    {
        DeleteObject( font );
        return;
    }

    dialog_control( d.hwnd, L"STATIC", load_string( IDS_WW_DESKTOP ), 0, -1, 16, 14, 300, 20, font );
    for (int k = 0; k < W_KINDS; k++)
    {
        int at = find_kind( list, count, k );
        enum widget_size size = at >= 0 ? list[at].size : default_size( k, HOST_DESKTOP );
        HWND sizes;

        dialog_control( d.hwnd, L"BUTTON", widget_name( k ), BS_AUTOCHECKBOX | WS_TABSTOP, IDC_DW_KIND + k, 16, y, 200, 24, font );
        CheckDlgButton( d.hwnd, IDC_DW_KIND + k, at >= 0 ? BST_CHECKED : BST_UNCHECKED );
        sizes = dialog_control( d.hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, IDC_DW_SIZE + k,
                                226, y - 1, 118, 200, font );
        for (int s = 0; s < W_SIZES; s++)
        {
            LRESULT index;

            if (!widget_allows( k, s )) continue;
            index = SendMessageW( sizes, CB_ADDSTRING, 0, (LPARAM)load_string( size_names[s] ) );
            SendMessageW( sizes, CB_SETITEMDATA, index, s );
            if (s == size) SendMessageW( sizes, CB_SETCURSEL, index, 0 );
        }
        if (k == W_CLOCK)
        {
            combo = dialog_control( d.hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, IDC_DW_LANGUAGE,
                                    352, y - 1, 152, 200, font );
            SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)load_string( IDS_DW_LANG_SYSTEM ) );
            SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)L"English" );
            SendMessageW( combo, CB_SETCURSEL, get_setting( L"ClockEnglish", 1 ) ? 1 : 0, 0 );
        }
        y += 30;
        if (k == W_MONITOR)
        {
            for (int i = 0; i < ARRAY_SIZE(items); i++)
            {
                HWND check = dialog_control( d.hwnd, L"BUTTON", load_string( items[i].name ), BS_AUTOCHECKBOX | WS_TABSTOP,
                                             items[i].id, 40 + (i % 2) * 180, y + (i / 2) * 28, 170, 24, font );

                SendMessageW( check, BM_SETCHECK, (items_on & items[i].bit) ? BST_CHECKED : BST_UNCHECKED, 0 );
            }
            y += 60;
        }
    }
    y += 14;
    dialog_control( d.hwnd, L"BUTTON", load_string( IDS_DW_RESET ), BS_PUSHBUTTON | WS_TABSTOP, IDC_DW_RESET, 16, y, 180, 26, font );
    dialog_control( d.hwnd, L"BUTTON", load_string( IDS_DW_OK ), BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK, 316, y, 90, 26, font );
    dialog_control( d.hwnd, L"BUTTON", load_string( IDS_DW_CANCEL ), BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL, 414, y, 90, 26, font );
    dialog_enable( d.hwnd );

    SetRect( &r, 0, 0, px( 520 ), px( y + 26 + 16 ) );
    AdjustWindowRectEx( &r, style, FALSE, WS_EX_DLGMODALFRAME );
    if (!parent || !GetWindowRect( parent, &owner_rect ))
        SetRect( &owner_rect, 0, 0, GetSystemMetrics( SM_CXSCREEN ), GetSystemMetrics( SM_CYSCREEN ) );
    SetWindowPos( d.hwnd, NULL, (owner_rect.left + owner_rect.right - (r.right - r.left)) / 2,
                  (owner_rect.top + owner_rect.bottom - (r.bottom - r.top)) / 2, r.right - r.left, r.bottom - r.top,
                  SWP_NOZORDER | SWP_NOACTIVATE );

    focus = GetFocus();
    if (parent) EnableWindow( parent, FALSE );
    ShowWindow( d.hwnd, SW_SHOW );
    SetFocus( GetDlgItem( d.hwnd, IDC_DW_KIND ) );
    while (!d.done && GetMessageW( &msg, NULL, 0, 0 ))
    {
        if (IsDialogMessageW( d.hwnd, &msg )) continue;
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }

    if (d.ok)
    {
        DWORD bits = 0;

        /* those that stay keep their order and their places */
        for (int i = 0; i < count; i++)
        {
            if (!kind_checked( d.hwnd, list[i].kind )) continue;
            out[n] = list[i];
            out[n].size = chosen_size( d.hwnd, list[i].kind );
            if (d.reset) out[n].placed = FALSE;
            n++;
        }
        for (int k = 0; k < W_KINDS; k++)
            if (kind_checked( d.hwnd, k ) && find_kind( out, n, k ) < 0) add_widget( out, &n, k, chosen_size( d.hwnd, k ) );
        widgets_save( HOST_DESKTOP, out, n );
        for (int i = 0; i < ARRAY_SIZE(items); i++)
            if (IsDlgButtonChecked( d.hwnd, items[i].id ) == BST_CHECKED) bits |= items[i].bit;
        set_setting( L"MonitorItems", bits ? bits : ITEM_ALL );
        set_setting( L"ClockEnglish", SendMessageW( combo, CB_GETCURSEL, 0, 0 ) == 1 );
        widgets_notify();
    }
    if (parent)
    {
        EnableWindow( parent, TRUE );
        SetActiveWindow( parent );
    }
    DestroyWindow( d.hwnd );
    DeleteObject( font );
    if (focus && IsWindow( focus )) SetFocus( focus );
}
