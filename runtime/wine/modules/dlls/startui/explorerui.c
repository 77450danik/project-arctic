/*
 * The window of File Explorer as Windows 10 and 11 have it
 *
 * browseui's CShellBrowser keeps doing the work: the folder view, the tree,
 * navigation. In the styles of Windows 10 and 11 its old toolbars stay hidden
 * and this draws what stands around the view. Folder Options chooses
 * (HKCU\Software\Arctic\Explorer, Style: 0 classic, 1 Windows 10, 2 Windows 11).
 *
 * Windows 11: tabs in the title bar, the row of the address, the command bar,
 * a status bar with the two views. Each tab remembers its folder and its own
 * way back and forward; the window shows one folder at a time.
 *
 * Windows 10: the ribbon, with its Home, Share and View tabs, and the address.
 *
 * Nothing here comes from Windows: the glyphs are drawn from the lists below.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "startui.h"
#include "winreg.h"
#include "commctrl.h"
#include "shlobj.h"
#include "shellapi.h"
#include "dwmapi.h"
#include "wine/arctic_dwm.h"

#define WM_ARCTIC_BROWSE    (WM_APP + 0x51)     /* wparam: not a new step of the history; lparam: an ITEMIDLIST */
#define WM_ARCTIC_SEARCH    (WM_APP + 0x52)     /* lparam: the text; nothing: back where the search began */
#define WM_ARCTIC_NAVPANE   (WM_APP + 0x53)     /* wparam: the navigation pane shown */
#define WM_ARCTIC_RELAYOUT  (WM_APP + 0x54)     /* the rows and the side are laid out again */

#define EXPLORER_WIN10      1
#define EXPLORER_WIN11      2

#define MAX_TABS            24
#define MAX_HISTORY         32
#define MAX_SEGMENTS        16
#define MAX_ZONES           96

#define TIMER_POLL          1
#define TIMER_SEARCH        2
#define SIDE_ROW            px(44)
#define ID_ADDRESS          100
#define ID_SEARCH           101

/* browseui's commands and shell32's */
#define IDM_FILE_CLOSE           0xA021
#define IDM_GOTO_UPONELEVEL      0xA022
#define IDM_TOOLS_FOLDEROPTIONS  0xA123
#define IDM_EXPLORERBAR_SEARCH   0xA231
#define CMD_DELETE               0x7011
#define CMD_RENAME               0x7012
#define CMD_PROPERTIES           0x7013
#define CMD_CUT                  0x7018
#define CMD_COPY                 0x7019
#define CMD_PASTE                0x701A
#define CMD_PASTE_LINK           0x701C
#define CMD_COPY_TO              0x701E
#define CMD_MOVE_TO              0x701F
#define CMD_SELECT_ALL           0x7021
#define CMD_INVERT               0x7022
#define CMD_LARGE                0x7029
#define CMD_SMALL                0x702A
#define CMD_LIST                 0x702B
#define CMD_DETAILS              0x702C
#define CMD_TILES                0x702E
#define CMD_NEW_FOLDER           0x7053
#define CMD_REFRESH              0x7103
#define CMD_SORT_FIRST           0x7FF6     /* + the column: name, size, type, date */

/* what is not a command of the view */
#define CMD_COPY_PATH            1
#define CMD_NEW_WINDOW           2
#define CMD_SELECT_NONE          3
#define CMD_SORT_MENU            4
#define CMD_NAV_PANE             5
#define CMD_ADD_DEVICE           6     /* Devices and Printers: the wizard */
#define CMD_ADD_PRINTER          7

enum { NEED_NONE, NEED_SELECTION, NEED_CLIPBOARD };

enum icon
{
    I_BACK, I_FORWARD, I_UP, I_REFRESH, I_PLUS, I_CUT, I_COPY, I_PASTE, I_RENAME, I_SHARE, I_DELETE, I_SORT, I_VIEW,
    I_MORE, I_DOWN, I_RIGHT, I_CLOSE, I_MIN, I_MAX, I_RESTORE, I_SEARCH, I_DETAILS, I_TILES, I_NEW_FOLDER, I_PROPS,
    I_SELECT, I_MOVE, I_LIST, I_LARGE, I_OPTIONS, I_NAVPANE, I_NONE,
};

enum kind
{
    Z_NONE, Z_TAB, Z_TAB_CLOSE, Z_NEW_TAB, Z_MIN, Z_MAX, Z_CLOSE, Z_BACK, Z_FORWARD, Z_UP, Z_REFRESH, Z_ADDRESS, Z_SEGMENT,
    Z_SEARCH, Z_COMMAND, Z_NEW_MENU, Z_SORT_MENU, Z_VIEW_MENU, Z_MORE_MENU, Z_RIBBON_TAB, Z_FILE_MENU,
};

struct tab
{
    LPITEMIDLIST pidl;
    WCHAR title[128];
    HBITMAP icon;
    LPITEMIDLIST back[MAX_HISTORY], forward[MAX_HISTORY];
    int back_count, forward_count;
};

struct zone
{
    int kind, index;
    RECT rect;
    BOOL enabled;
};

struct chrome
{
    HWND browser, top, bottom, edit;
    int style;
    struct tab tabs[MAX_TABS];
    int tab_count, active;
    BOOL from_history, tree_done, can_paste, backdrop;
    HWND search;                 /* the search box, while it is typed in */
    WCHAR query[MAX_PATH];       /* what the results in the window are of */
    int selected, ribbon_tab;
    struct zone zones[MAX_ZONES], low[6];
    int zone_count, low_count;
    int hot_kind, hot_index, down_kind, down_index;
    LPITEMIDLIST segment[MAX_SEGMENTS];
    WCHAR segment_name[MAX_SEGMENTS][64];
    int segment_count;
    WCHAR status[160];
    HHOOK hook;
    int low_hot;
    int caption_top;             /* the client area's top below the window's, for dwm */
    /* in the Control Panel its items stand where the tree was */
    HWND side, split;            /* the list, and the edge that drags its width */
    BOOL in_controls, side_shown;
    struct side_item *side_items;
    int side_count, side_width, side_hot, side_selected, side_scroll;
    int split_grab;              /* where in the edge it was taken, while dragged */
};

struct side_item
{
    LPITEMIDLIST pidl;
    WCHAR name[128];
    HBITMAP icon;
    BOOL folder;                 /* opens in the window; the others start */
};

struct colors
{
    BOOL dark;
    COLORREF strip, bar, field, text, dim, line, hover, down, accent;
};

static void get_colors( struct colors *c )
{
    COLORREF window = GetSysColor( COLOR_WINDOW );

    c->dark = GetRValue( window ) * 3 + GetGValue( window ) * 6 + GetBValue( window ) < 1280;
    if (c->dark)
    {
        c->strip = RGB( 0x20, 0x20, 0x20 );
        c->bar = RGB( 0x2c, 0x2c, 0x2c );
        c->field = RGB( 0x38, 0x38, 0x38 );
        c->text = RGB( 0xff, 0xff, 0xff );
        c->dim = RGB( 0xa8, 0xa8, 0xa8 );
        c->line = RGB( 0x40, 0x40, 0x40 );
        c->hover = RGB( 0x3a, 0x3a, 0x3a );
        c->down = RGB( 0x33, 0x33, 0x33 );
        c->accent = RGB( 0x60, 0xcd, 0xff );
    }
    else
    {
        c->strip = RGB( 0xee, 0xf2, 0xf7 );
        c->bar = RGB( 0xf9, 0xf9, 0xf9 );
        c->field = RGB( 0xff, 0xff, 0xff );
        c->text = RGB( 0x1b, 0x1b, 0x1b );
        c->dim = RGB( 0x5d, 0x5d, 0x5d );
        c->line = RGB( 0xe0, 0xe0, 0xe0 );
        c->hover = RGB( 0xec, 0xec, 0xec );
        c->down = RGB( 0xe2, 0xe2, 0xe2 );
        c->accent = RGB( 0x00, 0x5f, 0xb8 );
    }
}

#define TAB_H       px(40)
#define NAV_H       px(48)
#define CMD_H       px(48)
#define STATUS_H    px(26)
#define RIBTAB_H    px(24)
#define RIBBON_H    px(94)
#define ADDR_H      px(40)

/**********************************************************************
 *          The glyphs: lines in a square of 20, drawn soft
 */

#define END     -1
#define ARC     -2      /* centre x, y, radius, from degrees, to degrees */
#define BOX     -3      /* left, top, right, bottom */
#define DOT     -4      /* centre x, y, radius */
#define RBOX    -5      /* left, top, right, bottom, radius of the corners */

static const float g_back[] = { 17,10,3.5f,10, 3.5f,10,8.5f,5, 3.5f,10,8.5f,15, END };
static const float g_forward[] = { 3,10,16.5f,10, 16.5f,10,11.5f,5, 16.5f,10,11.5f,15, END };
static const float g_up[] = { 10,17,10,3.5f, 10,3.5f,5,8.5f, 10,3.5f,15,8.5f, END };
/* a ring turning clockwise, open at the upper right; its head a corner pointing on */
static const float g_refresh[] = { ARC,10,10,6.5f,-10,310, 16,5.3f,16,2, 16,5.3f,12.7f,5.3f, END };
static const float g_plus[] = { 10,3.5f,10,16.5f, 3.5f,10,16.5f,10, END };
static const float g_cut[] = { ARC,5.6f,15,2.6f,0,360, ARC,14.4f,15,2.6f,0,360, 7,12.8f,14.5f,3, 13,12.8f,5.5f,3, END };
static const float g_copy[] = { RBOX,7,3,17,14,2, 3.5f,7,3.5f,15, ARC,6,15,2.5f,180,90, 6,17.5f,13,17.5f, END };
static const float g_paste[] = { RBOX,4.5f,4.5f,15.5f,18,2.2f, RBOX,7.5f,2.5f,12.5f,6.5f,1.2f, END };
static const float g_rename[] = { 11,3,11,17, 9,3,13,3, 9,17,13,17, 8.5f,6.5f,5,6.5f, ARC,5,8.5f,2,270,180, 3,8.5f,3,11.5f, ARC,5,11.5f,2,180,90, 5,13.5f,8.5f,13.5f, 13.5f,6.5f,15,6.5f, ARC,15,8.5f,2,270,360, 17,8.5f,17,11.5f, ARC,15,11.5f,2,0,90, 15,13.5f,13.5f,13.5f, END };
static const float g_share[] = { 16.5f,5.5f,12.5f,2, 16.5f,5.5f,12.5f,9, 16.5f,5.5f,11,5.5f, ARC,11,10.5f,5,270,180, 6,10.5f,6,12, 3.5f,8,3.5f,15, ARC,6,15,2.5f,180,90, 6,17.5f,14,17.5f, ARC,14,15,2.5f,90,0, 16.5f,15,16.5f,12.5f, END };
static const float g_delete[] = { 3.5f,5.5f,16.5f,5.5f, ARC,10,5.5f,2.5f,180,360, 5,5.5f,5.8f,15.5f, ARC,7.8f,15.5f,2,180,90, 7.8f,17.5f,12.2f,17.5f, ARC,12.2f,15.5f,2,90,0, 14.2f,15.5f,15,5.5f, 8.5f,8.5f,8.5f,14.5f, 11.5f,8.5f,11.5f,14.5f, END };
static const float g_sort[] = { 6,3,6,17, 6,17,3,14, 6,17,9,14, 14,17,14,3, 14,3,11,6, 14,3,17,6, END };
static const float g_view[] = { RBOX,3,3,8.5f,8.5f,1.5f, RBOX,11.5f,3,17,8.5f,1.5f, RBOX,3,11.5f,8.5f,17,1.5f, RBOX,11.5f,11.5f,17,17,1.5f, END };
static const float g_more[] = { DOT,4,10,1.4f, DOT,10,10,1.4f, DOT,16,10,1.4f, END };
static const float g_down[] = { 5,8,10,13, 10,13,15,8, END };
static const float g_right[] = { 8,5,13,10, 13,10,8,15, END };
static const float g_close[] = { 4,4,16,16, 16,4,4,16, END };
static const float g_min[] = { 4,10,16,10, END };
static const float g_max[] = { RBOX,4,4,16,16,1.5f, END };
static const float g_restore[] = { RBOX,4,6.5f,13.5f,16,1.5f, 6.5f,4,14.5f,4, ARC,14.5f,5.5f,1.5f,270,360, 16,5.5f,16,13.5f, END };
static const float g_search[] = { ARC,8.5f,8.5f,5,0,360, 12.2f,12.2f,17,17, END };
static const float g_details[] = { 3,5,17,5, 3,10,17,10, 3,15,17,15, END };
static const float g_tiles[] = { RBOX,3,3,17,8.5f,1.5f, RBOX,3,11.5f,17,17,1.5f, END };
static const float g_new_folder[] = { RBOX,2.5f,6,17.5f,16.5f,2, 3.5f,6,4.5f,4, 4.5f,4,8,4, 8,4,9.5f,6, 10,9,10,14, 7.5f,11.5f,12.5f,11.5f, END };
static const float g_props[] = { RBOX,4,3,16,17,2, 7,7,13,7, 7,10,13,10, 7,13,11,13, END };
static const float g_select[] = { RBOX,3,3,17,17,2.5f, 6.5f,10,9,13, 9,13,14,7, END };
static const float g_move[] = { RBOX,2.5f,6,17.5f,16.5f,2, 3.5f,6,4.5f,4, 4.5f,4,8,4, 8,4,9.5f,6, 7,11.5f,13,11.5f, 13,11.5f,10.5f,9, 13,11.5f,10.5f,14, END };
static const float g_list[] = { DOT,3.5f,5,1.2f, DOT,3.5f,10,1.2f, DOT,3.5f,15,1.2f, 7,5,17,5, 7,10,17,10, 7,15,17,15, END };
static const float g_large[] = { RBOX,3,3,17,14,2, 7,17,13,17, END };
static const float g_options[] = { ARC,10,10,3,0,360, ARC,10,10,7,0,360, END };
static const float g_navpane[] = { RBOX,3,4,17,16,2, 8,4,8,16, END };

static const float *const glyphs[] =
{
    g_back, g_forward, g_up, g_refresh, g_plus, g_cut, g_copy, g_paste, g_rename, g_share, g_delete, g_sort, g_view,
    g_more, g_down, g_right, g_close, g_min, g_max, g_restore, g_search, g_details, g_tiles, g_new_folder, g_props,
    g_select, g_move, g_list, g_large, g_options, g_navpane,
};

struct segment { float x0, y0, x1, y1; };

#define MAX_LINES 220

static void add_arc( struct segment *out, int *n, float cx, float cy, float r, float from_deg, float to_deg )
{
    float from = from_deg * 3.14159265f / 180, to = to_deg * 3.14159265f / 180;
    int steps = max( 3, (int)(fabsf( to_deg - from_deg ) / 15) );

    for (int i = 0; i < steps && *n < MAX_LINES; i++, (*n)++)
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
    int n = 0, d = 0;

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
            const struct segment sides[4] = { { l + q, t, r - q, t }, { r, t + q, r, b - q }, { r - q, b, l + q, b }, { l, b - q, l, t + q } };

            for (int i = 0; i < 4 && n < MAX_LINES; i++) out[n++] = sides[i];
            add_arc( out, &n, r - q, t + q, q, 270, 360 );
            add_arc( out, &n, r - q, b - q, q, 0, 90 );
            add_arc( out, &n, l + q, b - q, q, 90, 180 );
            add_arc( out, &n, l + q, t + q, q, 180, 270 );
            p += 6;
        }
        else if (*p == BOX)
        {
            float l = p[1], t = p[2], r = p[3], b = p[4];
            const struct segment sides[4] = { { l, t, r, t }, { r, t, r, b }, { r, b, l, b }, { l, b, l, t } };

            for (int i = 0; i < 4 && n < MAX_LINES; i++) out[n++] = sides[i];
            p += 5;
        }
        else if (*p == DOT)
        {
            dots[d * 3] = p[1];
            dots[d * 3 + 1] = p[2];
            dots[d * 3 + 2] = p[3];
            d++;
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

struct cached_icon
{
    int icon, size;
    COLORREF color;
    UINT32 *pixels;
};

static struct cached_icon icon_cache[160];
static int icon_cache_count;
static SRWLOCK icon_lock = SRWLOCK_INIT;

static UINT32 *make_icon( int icon, int size, COLORREF color )
{
    struct segment lines[MAX_LINES];
    float dots[30], scale = 20.0f / size;
    float stroke = 0.5f;
    int counts = glyph_lines( glyphs[icon], lines, dots ), n = counts & 0xffff, d = counts >> 16;
    UINT32 *pixels = calloc( size * size, sizeof(*pixels) );

    if (!pixels) return NULL;
    for (int y = 0; y < size; y++)
    {
        for (int x = 0; x < size; x++)
        {
            int hits = 0;

            for (int j = 0; j < 4; j++)
            {
                for (int i = 0; i < 4; i++)
                {
                    float u = (x + (i + 0.5f) / 4) * scale, v = (y + (j + 0.5f) / 4) * scale;
                    BOOL in = FALSE;

                    for (int k = 0; k < n && !in; k++) in = near_line( &lines[k], u, v, stroke );
                    for (int k = 0; k < d && !in; k++)
                    {
                        float dx = u - dots[k * 3], dy = v - dots[k * 3 + 1];

                        in = dx * dx + dy * dy <= dots[k * 3 + 2] * dots[k * 3 + 2];
                    }
                    if (in) hits++;
                }
            }
            if (hits)
            {
                UINT32 a = hits * 255 / 16;

                pixels[y * size + x] = (a << 24) | ((GetRValue( color ) * a / 255) << 16) | ((GetGValue( color ) * a / 255) << 8) |
                                       (GetBValue( color ) * a / 255);
            }
        }
    }
    return pixels;
}

static void draw_icon( HDC hdc, int icon, int x, int y, int size, COLORREF color )
{
    UINT32 *pixels = NULL;
    BOOL kept = TRUE;

    AcquireSRWLockExclusive( &icon_lock );
    for (int i = 0; i < icon_cache_count && !pixels; i++)
        if (icon_cache[i].icon == icon && icon_cache[i].size == size && icon_cache[i].color == color) pixels = icon_cache[i].pixels;
    if (!pixels && (pixels = make_icon( icon, size, color )))
    {
        if ((kept = icon_cache_count < ARRAY_SIZE(icon_cache)))
        {
            icon_cache[icon_cache_count].icon = icon;
            icon_cache[icon_cache_count].size = size;
            icon_cache[icon_cache_count].color = color;
            icon_cache[icon_cache_count++].pixels = pixels;
        }
    }
    ReleaseSRWLockExclusive( &icon_lock );
    if (pixels) blend_bits( hdc, x, y, size, size, pixels );
    if (!kept) free( pixels );   /* the cache is full: drawn once */
}

/**********************************************************************
 *          Tabs and the places
 */

static void tab_describe( struct tab *t, LPCITEMIDLIST pidl )
{
    SHFILEINFOW info;

    if (t->icon) DeleteObject( t->icon );
    t->icon = NULL;
    t->title[0] = 0;
    if (SHGetFileInfoW( (const WCHAR *)pidl, 0, &info, sizeof(info), SHGFI_PIDL | SHGFI_DISPLAYNAME | SHGFI_ICON | SHGFI_SMALLICON ))
    {
        lstrcpynW( t->title, info.szDisplayName, ARRAY_SIZE(t->title) );
        if (info.hIcon)
        {
            t->icon = icon_bitmap( info.hIcon, px( 16 ) );
            DestroyIcon( info.hIcon );
        }
        return;
    }
    /* an item without an icon of its own, a page of a Control Panel item:
     * its name, and the icon of what it is in */
    if (SHGetFileInfoW( (const WCHAR *)pidl, 0, &info, sizeof(info), SHGFI_PIDL | SHGFI_DISPLAYNAME ))
        lstrcpynW( t->title, info.szDisplayName, ARRAY_SIZE(t->title) );
    {
        ITEMIDLIST *parent = ILClone( pidl );

        if (parent && ILRemoveLastID( parent ) &&
            SHGetFileInfoW( (const WCHAR *)parent, 0, &info, sizeof(info), SHGFI_PIDL | SHGFI_ICON | SHGFI_SMALLICON ) &&
            info.hIcon)
        {
            t->icon = icon_bitmap( info.hIcon, px( 16 ) );
            DestroyIcon( info.hIcon );
        }
        ILFree( parent );
    }
}

static void tab_clear( struct tab *t )
{
    ILFree( t->pidl );
    for (int i = 0; i < t->back_count; i++) ILFree( t->back[i] );
    for (int i = 0; i < t->forward_count; i++) ILFree( t->forward[i] );
    if (t->icon) DeleteObject( t->icon );
    memset( t, 0, sizeof(*t) );
}

static void push( LPITEMIDLIST *list, int *count, LPITEMIDLIST pidl )
{
    if (*count == MAX_HISTORY)
    {
        ILFree( list[0] );
        memmove( list, list + 1, (MAX_HISTORY - 1) * sizeof(*list) );
        (*count)--;
    }
    list[(*count)++] = pidl;
}

static void browse( struct chrome *c, LPCITEMIDLIST pidl, BOOL from_history )
{
    LPITEMIDLIST copy = ILClone( pidl );

    if (!copy) return;
    c->from_history = from_history;
    if (!PostMessageW( c->browser, WM_ARCTIC_BROWSE, from_history, (LPARAM)copy )) ILFree( copy );
}

static LPITEMIDLIST this_pc(void)
{
    LPITEMIDLIST pidl = NULL;

    SHGetSpecialFolderLocation( NULL, CSIDL_DRIVES, &pidl );
    return pidl;
}

static void redraw( struct chrome *c )
{
    InvalidateRect( c->top, NULL, FALSE );
    if (c->bottom) InvalidateRect( c->bottom, NULL, FALSE );
}

static void new_tab( struct chrome *c, LPCITEMIDLIST pidl )
{
    struct tab *t;
    LPITEMIDLIST start = NULL;

    if (c->tab_count >= MAX_TABS) return;
    if (!pidl) pidl = start = this_pc();
    if (!pidl) return;
    t = &c->tabs[c->tab_count];
    memset( t, 0, sizeof(*t) );
    t->pidl = ILClone( pidl );
    tab_describe( t, pidl );
    c->active = c->tab_count++;
    browse( c, pidl, TRUE );
    ILFree( start );
    redraw( c );
}

static void switch_tab( struct chrome *c, int index )
{
    if (index < 0 || index >= c->tab_count || index == c->active) return;
    c->active = index;
    browse( c, c->tabs[index].pidl, TRUE );
    redraw( c );
}

static void close_tab( struct chrome *c, int index )
{
    if (index < 0 || index >= c->tab_count) return;
    if (c->tab_count == 1)
    {
        PostMessageW( c->browser, WM_CLOSE, 0, 0 );
        return;
    }
    tab_clear( &c->tabs[index] );
    memmove( &c->tabs[index], &c->tabs[index + 1], (c->tab_count - index - 1) * sizeof(c->tabs[0]) );
    c->tab_count--;
    if (index < c->active) c->active--;
    else if (index == c->active)
    {
        c->active = min( index, c->tab_count - 1 );
        browse( c, c->tabs[c->active].pidl, TRUE );
    }
    redraw( c );
}

static void go_back( struct chrome *c )
{
    struct tab *t = &c->tabs[c->active];
    LPITEMIDLIST target;

    if (!t->back_count) return;
    target = t->back[--t->back_count];
    push( t->forward, &t->forward_count, ILClone( t->pidl ) );
    browse( c, target, TRUE );
    ILFree( target );
}

static void go_forward( struct chrome *c )
{
    struct tab *t = &c->tabs[c->active];
    LPITEMIDLIST target;

    if (!t->forward_count) return;
    target = t->forward[--t->forward_count];
    push( t->back, &t->back_count, ILClone( t->pidl ) );
    browse( c, target, TRUE );
    ILFree( target );
}

static void build_segments( struct chrome *c, LPCITEMIDLIST pidl )
{
    LPITEMIDLIST cur = ILClone( pidl ), reversed[MAX_SEGMENTS];
    int n = 0;

    for (int i = 0; i < c->segment_count; i++) ILFree( c->segment[i] );
    c->segment_count = 0;
    while (cur && cur->mkid.cb && n < MAX_SEGMENTS)
    {
        reversed[n++] = ILClone( cur );
        ILRemoveLastID( cur );
    }
    ILFree( cur );
    for (int i = 0; i < n; i++)
    {
        SHFILEINFOW info;

        c->segment[i] = reversed[n - 1 - i];
        c->segment_name[i][0] = 0;
        if (SHGetFileInfoW( (const WCHAR *)c->segment[i], 0, &info, sizeof(info), SHGFI_PIDL | SHGFI_DISPLAYNAME ))
            lstrcpynW( c->segment_name[i], info.szDisplayName, ARRAY_SIZE(c->segment_name[i]) );
    }
    c->segment_count = n;
}

/**********************************************************************
 *          Commands
 */

static void command( struct chrome *c, UINT id )
{
    PostMessageW( c->browser, WM_COMMAND, MAKEWPARAM( id, 0 ), 0 );
}

static HWND find_child( HWND parent, const WCHAR *cls )
{
    HWND child, found;
    WCHAR name[64];

    for (child = GetWindow( parent, GW_CHILD ); child; child = GetWindow( child, GW_HWNDNEXT ))
    {
        if (GetClassNameW( child, name, ARRAY_SIZE(name) ) && !lstrcmpiW( name, cls )) return child;
        if ((found = find_child( child, cls ))) return found;
    }
    return NULL;
}

static void copy_path( struct chrome *c )
{
    WCHAR path[MAX_PATH] = L"";
    HGLOBAL mem;
    SIZE_T size;

    if (!SHGetPathFromIDListW( c->tabs[c->active].pidl, path ) || !OpenClipboard( c->top )) return;
    size = (lstrlenW( path ) + 1) * sizeof(WCHAR);
    EmptyClipboard();
    if ((mem = GlobalAlloc( GMEM_MOVEABLE, size )))
    {
        memcpy( GlobalLock( mem ), path, size );
        GlobalUnlock( mem );
        SetClipboardData( CF_UNICODETEXT, mem );
    }
    CloseClipboard();
}

/* View, Show, Navigation pane: Windows' own setting, for the tree and for
 * the Control Panel's list in its place */
static BOOL nav_pane_wanted(void)
{
    DWORD value = 1, size = sizeof(value);

    RegGetValueW( HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"NavPaneVisible",
                  RRF_RT_REG_DWORD, NULL, &value, &size );
    return value != 0;
}

static void run_command( struct chrome *c, UINT id )
{
    HWND list;

    switch (id)
    {
    case CMD_NAV_PANE:
        PostMessageW( c->browser, WM_ARCTIC_NAVPANE, !nav_pane_wanted(), 0 );
        break;
    case CMD_COPY_PATH:
        copy_path( c );
        break;
    case CMD_NEW_WINDOW:
        ShellExecuteW( NULL, L"open", L"explorer.exe", NULL, NULL, SW_SHOWNORMAL );
        break;
    case CMD_SELECT_NONE:
        if ((list = find_child( c->browser, L"SysListView32" )))
        {
            LVITEMW item = { 0 };

            item.stateMask = LVIS_SELECTED;
            SendMessageW( list, LVM_SETITEMSTATE, -1, (LPARAM)&item );
        }
        break;
    default:
        command( c, id );
        break;
    }
}

static void menu_look( struct look *look, const struct colors *col )
{
    memset( look, 0, sizeof(*look) );
    /* a plain blur, as Windows 11's menus: no grain, a good deal shows through */
    look->effect = ARCTIC_ACCENT_ENABLE_BLURBEHIND;
    look->color = col->dark ? RGB( 0x12, 0x12, 0x12 ) : RGB( 0xf2, 0xf2, 0xf2 );
    look->opacity = 72;
    look->blur = 85;
    look->text = TEXT_AUTO;
}

static UINT show_menu( struct chrome *c, const RECT *anchor, const struct popup_item *items, UINT count )
{
    POINT pt = { anchor->left, anchor->bottom };
    struct colors col;
    struct look look;

    get_colors( &col );
    menu_look( &look, &col );
    ClientToScreen( c->top, &pt );
    return popup_track( c->browser, pt, TPM_LEFTALIGN | TPM_TOPALIGN, items, count,
                        c->style == EXPLORER_WIN11 ? POPUP_WIN11 : POPUP_WIN10, &look );
}

/* Devices and Printers (devicescpl.dll) is open: "Створити" adds a device there, as Windows 11's Settings */
static BOOL in_devices( struct chrome *c )
{
    static struct chrome *checked_chrome;
    static LPITEMIDLIST checked;
    static BOOL result;
    WCHAR *name;

    if (!c->tab_count || !c->tabs[c->active].pidl) return FALSE;
    if (c == checked_chrome && c->tabs[c->active].pidl == checked) return result;
    checked_chrome = c;
    checked = c->tabs[c->active].pidl;
    result = FALSE;
    if (SUCCEEDED(SHGetNameFromIDList( checked, SIGDN_DESKTOPABSOLUTEPARSING, &name )))
    {
        result = !!wcsstr( _wcsupr( name ), L"{A8A91A66-3A7D-4424-8D24-04E180695C7A}" );
        CoTaskMemFree( name );
    }
    return result;
}

static void add_device( struct chrome *c, int kind )
{
    HMODULE module = LoadLibraryW( L"devicescpl.dll" );
    void (WINAPI *wizard)( HWND, int ) = module ? (void *)GetProcAddress( module, "ArcticAddDevice" ) : NULL;

    if (wizard) wizard( c->browser, kind );
}

static void new_menu( struct chrome *c, const RECT *anchor )
{
    const struct popup_item items[] =
    {
        { CMD_NEW_FOLDER, load_string( IDS_EX_FOLDER ), GLYPH_FOLDER },
    };
    const struct popup_item devices[] =
    {
        { CMD_ADD_DEVICE, load_string( IDS_EX_BLUETOOTH_DEVICE ), GLYPH_NONE },
        { CMD_ADD_PRINTER, load_string( IDS_EX_PRINTER ), GLYPH_NONE },
    };
    UINT id;

    if (in_devices( c )) id = show_menu( c, anchor, devices, ARRAY_SIZE(devices) );
    else id = show_menu( c, anchor, items, ARRAY_SIZE(items) );
    if (id == CMD_ADD_DEVICE) add_device( c, 0 );
    else if (id == CMD_ADD_PRINTER) add_device( c, 1 );
    else if (id) run_command( c, id );
}

static void sort_menu( struct chrome *c, const RECT *anchor )
{
    const struct popup_item items[] =
    {
        { CMD_SORT_FIRST + 0, load_string( IDS_EX_SORT_NAME ), GLYPH_NONE },
        { CMD_SORT_FIRST + 3, load_string( IDS_EX_SORT_DATE ), GLYPH_NONE },
        { CMD_SORT_FIRST + 2, load_string( IDS_EX_SORT_TYPE ), GLYPH_NONE },
        { CMD_SORT_FIRST + 1, load_string( IDS_EX_SORT_SIZE ), GLYPH_NONE },
    };
    UINT id = show_menu( c, anchor, items, ARRAY_SIZE(items) );

    if (id) run_command( c, id );
}

static void view_menu( struct chrome *c, const RECT *anchor )
{
    const struct popup_item show[] =
    {
        { CMD_NAV_PANE, load_string( IDS_EX_NAV_PANE ), GLYPH_NONE, nav_pane_wanted() },
    };
    const struct popup_item items[] =
    {
        { CMD_LARGE, load_string( IDS_EX_LARGE ), GLYPH_NONE },
        { CMD_SMALL, load_string( IDS_EX_SMALL ), GLYPH_NONE },
        { CMD_LIST, load_string( IDS_EX_LIST ), GLYPH_NONE },
        { CMD_DETAILS, load_string( IDS_EX_DETAILS ), GLYPH_NONE },
        { CMD_TILES, load_string( IDS_EX_TILES ), GLYPH_NONE },
        { 0 },
        { 0, load_string( IDS_EX_SHOW ), GLYPH_NONE, FALSE, FALSE, show, ARRAY_SIZE(show) },
    };
    UINT id = show_menu( c, anchor, items, ARRAY_SIZE(items) );

    if (id) run_command( c, id );
}

static void more_menu( struct chrome *c, const RECT *anchor )
{
    const struct popup_item items[] =
    {
        { CMD_SELECT_ALL, load_string( IDS_EX_SELECT_ALL ), GLYPH_NONE },
        { CMD_SELECT_NONE, load_string( IDS_EX_SELECT_NONE ), GLYPH_NONE },
        { CMD_INVERT, load_string( IDS_EX_INVERT ), GLYPH_NONE },
        { 0 },
        { CMD_PROPERTIES, load_string( IDS_EX_PROPERTIES ), GLYPH_NONE, FALSE, c->selected == 0 },
        { IDM_TOOLS_FOLDEROPTIONS, load_string( IDS_EX_OPTIONS ), GLYPH_NONE },
    };
    UINT id = show_menu( c, anchor, items, ARRAY_SIZE(items) );

    if (id) run_command( c, id );
}

static void file_menu( struct chrome *c, const RECT *anchor )
{
    const struct popup_item items[] =
    {
        { CMD_NEW_WINDOW, load_string( IDS_EX_NEW_WINDOW ), GLYPH_NONE },
        { IDM_TOOLS_FOLDEROPTIONS, load_string( IDS_EX_OPTIONS ), GLYPH_NONE },
        { 0 },
        { IDM_FILE_CLOSE, load_string( IDS_CLOSE_WINDOW ), GLYPH_NONE },
    };
    UINT id = show_menu( c, anchor, items, ARRAY_SIZE(items) );

    if (id) run_command( c, id );
}

/**********************************************************************
 *          The address, when it is typed
 */

static void edit_finish( struct chrome *c, BOOL go )
{
    WCHAR text[MAX_PATH * 2];
    HWND edit = c->edit;
    LPITEMIDLIST pidl = NULL;

    if (!edit) return;
    c->edit = NULL;
    GetWindowTextW( edit, text, ARRAY_SIZE(text) );
    DestroyWindow( edit );
    if (go && text[0])
    {
        if (SUCCEEDED(SHParseDisplayName( text, NULL, &pidl, 0, NULL )))
        {
            browse( c, pidl, FALSE );
            ILFree( pidl );
        }
        else ShellExecuteW( NULL, L"open", text, NULL, NULL, SW_SHOWNORMAL );
    }
    redraw( c );
}

static LRESULT CALLBACK edit_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR data )
{
    struct chrome *c = (struct chrome *)data;

    switch (msg)
    {
    case WM_KEYDOWN:
        if (wparam == VK_RETURN) { edit_finish( c, TRUE ); return 0; }
        if (wparam == VK_ESCAPE) { edit_finish( c, FALSE ); return 0; }
        break;
    case WM_CHAR:
        if (wparam == VK_RETURN || wparam == VK_ESCAPE) return 0;
        break;
    case WM_KILLFOCUS:
        if (c->edit == hwnd) edit_finish( c, FALSE );
        return 0;
    case WM_NCDESTROY:
        RemoveWindowSubclass( hwnd, edit_proc, id );
        break;
    }
    return DefSubclassProc( hwnd, msg, wparam, lparam );
}

static void edit_start( struct chrome *c, const RECT *field )
{
    WCHAR path[MAX_PATH] = L"";
    struct tab *t = &c->tabs[c->active];
    HFONT font = shell_font( px( 12 ), FW_NORMAL );

    if (c->edit) return;
    if (!SHGetPathFromIDListW( t->pidl, path )) lstrcpynW( path, t->title, ARRAY_SIZE(path) );
    c->edit = CreateWindowExW( 0, L"EDIT", path, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, field->left + px( 8 ), field->top + px( 8 ),
                               field->right - field->left - px( 16 ), field->bottom - field->top - px( 14 ), c->top,
                               (HMENU)ID_ADDRESS, startui_instance, NULL );
    if (!c->edit) return;
    SendMessageW( c->edit, WM_SETFONT, (WPARAM)font, TRUE );
    SetWindowSubclass( c->edit, edit_proc, 1, (DWORD_PTR)c );
    SendMessageW( c->edit, EM_SETSEL, 0, -1 );
    SetFocus( c->edit );
    redraw( c );
}

/**********************************************************************
 *          The search: its results show in the window (browseui's find
 *          folder), a moment after the typing stops
 */

static void search_run( struct chrome *c )
{
    KillTimer( c->top, TIMER_SEARCH );
    if (c->search) GetWindowTextW( c->search, c->query, ARRAY_SIZE(c->query) );
    SendMessageW( c->browser, WM_ARCTIC_SEARCH, 0, (LPARAM)c->query );
}

static void search_finish( struct chrome *c )
{
    HWND edit = c->search;

    if (!edit) return;
    c->search = NULL;
    GetWindowTextW( edit, c->query, ARRAY_SIZE(c->query) );
    DestroyWindow( edit );
    redraw( c );
}

static LRESULT CALLBACK search_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR data )
{
    struct chrome *c = (struct chrome *)data;

    switch (msg)
    {
    case WM_KEYDOWN:
        if (wparam == VK_RETURN) { search_run( c ); return 0; }
        if (wparam == VK_ESCAPE)
        {
            SetWindowTextW( hwnd, L"" );
            search_run( c );
            SetFocus( c->browser );
            return 0;
        }
        break;
    case WM_CHAR:
        if (wparam == VK_RETURN || wparam == VK_ESCAPE) return 0;
        break;
    case WM_KILLFOCUS:
        if (c->search == hwnd) search_finish( c );
        return 0;
    case WM_NCDESTROY:
        RemoveWindowSubclass( hwnd, search_proc, id );
        break;
    }
    return DefSubclassProc( hwnd, msg, wparam, lparam );
}

static void search_start( struct chrome *c, const RECT *field )
{
    HFONT font = shell_font( px( 12 ), FW_NORMAL );

    if (c->search) return;
    c->search = CreateWindowExW( 0, L"EDIT", c->query, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, field->left + px( 10 ),
                                 field->top + px( 8 ), field->right - field->left - px( 40 ), field->bottom - field->top - px( 14 ),
                                 c->top, (HMENU)ID_SEARCH, startui_instance, NULL );
    if (!c->search) return;
    SendMessageW( c->search, WM_SETFONT, (WPARAM)font, TRUE );
    SetWindowSubclass( c->search, search_proc, 1, (DWORD_PTR)c );
    SendMessageW( c->search, EM_SETSEL, 0, -1 );
    SetFocus( c->search );
}

/* the results of a search: browseui's find folder */
static BOOL is_find_folder( LPCITEMIDLIST pidl )
{
    static LPITEMIDLIST find;

    if (!find && FAILED(SHParseDisplayName( L"shell:::{E17D4FC0-5564-11D1-83F2-00A0C90DC849}", NULL, &find, 0, NULL )))
        return FALSE;
    return ILIsEqual( pidl, find );
}

/**********************************************************************
 *          Painting
 */

static void zone_add( struct chrome *c, int kind, int index, const RECT *r, BOOL enabled )
{
    if (c->zone_count >= MAX_ZONES) return;
    c->zones[c->zone_count].kind = kind;
    c->zones[c->zone_count].index = index;
    c->zones[c->zone_count].rect = *r;
    c->zones[c->zone_count].enabled = enabled;
    c->zone_count++;
}

static BOOL is_hot( const struct chrome *c, int kind, int index )
{
    return c->hot_kind == kind && c->hot_index == index;
}

static void fill( HDC hdc, const RECT *r, COLORREF color )
{
    fill_alpha( hdc, r, color, 255 );
}

/* a button: its state under it, the glyph and maybe a label after it */
static void paint_button( struct chrome *c, HDC hdc, const struct colors *col, RECT r, int kind, int index, int icon,
                          COLORREF ink, const WCHAR *label, HFONT font, BOOL enabled, BOOL chevron )
{
    int size = px( 20 ), x;

    zone_add( c, kind, index, &r, enabled );
    if (enabled && is_hot( c, kind, index ))
        fill_round_rect( hdc, &r, (float)px( 4 ), c->down_kind == kind && c->down_index == index ? col->down : col->hover, 255 );
    if (!enabled) ink = col->dark ? RGB( 0x70, 0x70, 0x70 ) : RGB( 0xb0, 0xb0, 0xb0 );
    if (!label)
    {
        draw_icon( hdc, icon, (r.left + r.right - size) / 2, (r.top + r.bottom - size) / 2, size, ink );
        return;
    }
    x = r.left + px( 8 );
    if (icon != I_NONE)
    {
        draw_icon( hdc, icon, x, (r.top + r.bottom - size) / 2, size, ink );
        x += size + px( 6 );
    }
    {
        RECT t = r;

        t.left = x;
        t.right = r.right - (chevron ? px( 20 ) : px( 6 ));
        paint_text( hdc, label, &t, font, enabled ? col->text : ink, DT_LEFT );
    }
    if (chevron) draw_icon( hdc, I_DOWN, r.right - px( 18 ), (r.top + r.bottom - px( 12 )) / 2, px( 12 ), enabled ? col->dim : ink );
}

static int text_px( HDC hdc, HFONT font, const WCHAR *text )
{
    HGDIOBJ old = SelectObject( hdc, font );
    SIZE size = { 0 };

    GetTextExtentPoint32W( hdc, text, lstrlenW( text ), &size );
    SelectObject( hdc, old );
    return size.cx;
}

/* the tabs in the title bar, Windows 11's */
static void paint_tabs( struct chrome *c, HDC hdc, const struct colors *col, const RECT *client, HFONT font )
{
    int buttons = 3 * px( 46 ), add = px( 36 ), left = px( 8 ), avail = client->right - buttons - left - add - px( 40 );
    int width = max( px( 110 ), min( px( 240 ), avail / max( c->tab_count, 1 ) ) ), top = px( 6 );
    BOOL zoomed = IsZoomed( c->browser );
    RECT r;

    for (int i = 0; i < c->tab_count; i++)
    {
        struct tab *t = &c->tabs[i];
        BOOL active = i == c->active;
        RECT close, text;

        SetRect( &r, left + i * width, top, left + (i + 1) * width, TAB_H );
        zone_add( c, Z_TAB, i, &r, TRUE );
        if (active)
        {
            RECT shape = r;

            shape.bottom += px( 10 );
            SaveDC( hdc );
            IntersectClipRect( hdc, r.left, r.top, r.right, r.bottom );
            fill_round_rect( hdc, &shape, (float)px( 8 ), col->bar, 255 );
            RestoreDC( hdc, -1 );
        }
        else if (is_hot( c, Z_TAB, i ) || is_hot( c, Z_TAB_CLOSE, i ))
        {
            RECT shape = r;

            InflateRect( &shape, -px( 2 ), 0 );
            shape.top += px( 1 );
            shape.bottom -= px( 4 );
            fill_round_rect( hdc, &shape, (float)px( 6 ), col->hover, 255 );
        }
        else if (i != c->active - 1 && i < c->tab_count - 1)
        {
            RECT sep = { r.right - 1, r.top + px( 8 ), r.right, r.bottom - px( 10 ) };

            fill( hdc, &sep, col->line );
        }
        if (t->icon) draw_icon_bitmap( hdc, t->icon, r.left + px( 12 ), (r.top + r.bottom - px( 16 )) / 2 - (active ? 0 : px( 1 )), px( 16 ) );
        SetRect( &text, r.left + px( 36 ), r.top, r.right - px( 32 ), r.bottom - (active ? 0 : px( 2 )) );
        paint_text( hdc, t->title, &text, font, active ? col->text : col->dim, DT_LEFT );

        SetRect( &close, r.right - px( 30 ), (r.top + r.bottom - px( 24 )) / 2 - px( 1 ), r.right - px( 6 ), 0 );
        close.bottom = close.top + px( 24 );
        if (active || is_hot( c, Z_TAB, i ) || is_hot( c, Z_TAB_CLOSE, i ))
        {
            zone_add( c, Z_TAB_CLOSE, i, &close, TRUE );
            if (is_hot( c, Z_TAB_CLOSE, i )) fill_round_rect( hdc, &close, (float)px( 4 ), col->down, 255 );
            draw_icon( hdc, I_CLOSE, close.left + px( 5 ), close.top + px( 5 ), px( 14 ), col->dim );
        }
    }
    SetRect( &r, left + c->tab_count * width + px( 4 ), top + px( 2 ), left + c->tab_count * width + px( 4 ) + px( 30 ), top + px( 30 ) );
    zone_add( c, Z_NEW_TAB, 0, &r, c->tab_count < MAX_TABS );
    if (is_hot( c, Z_NEW_TAB, 0 )) fill_round_rect( hdc, &r, (float)px( 4 ), col->hover, 255 );
    draw_icon( hdc, I_PLUS, r.left + px( 7 ), r.top + px( 6 ), px( 16 ), col->text );

    /* the window's buttons */
    for (int i = 0; i < 3; i++)
    {
        int kinds[3] = { Z_MIN, Z_MAX, Z_CLOSE }, icons[3] = { I_MIN, zoomed ? I_RESTORE : I_MAX, I_CLOSE };
        int size = px( 16 );
        BOOL hot = is_hot( c, kinds[i], 0 );

        SetRect( &r, client->right - buttons + i * px( 46 ), 0, client->right - buttons + (i + 1) * px( 46 ), px( 32 ) );
        zone_add( c, kinds[i], 0, &r, TRUE );
        if (hot) fill_round_rect( hdc, &r, 0, i == 2 ? RGB( 0xc4, 0x2b, 0x1c ) : col->hover, 255 );   /* opaque over the backdrop */
        draw_icon( hdc, icons[i], (r.left + r.right - size) / 2, (r.top + r.bottom - size) / 2, size,
                   hot && i == 2 ? RGB( 255, 255, 255 ) : col->text );
    }
}

/* the address: its places, or the typed text; at its end, as in Windows, the
 * button that refreshes the folder, and goes where the text says while it
 * is typed */
static void paint_address( struct chrome *c, HDC hdc, const struct colors *col, const RECT *field, HFONT font )
{
    int radius = c->style == EXPLORER_WIN11 ? px( 4 ) : 0, x = field->left + px( 8 ), first = 0, total = 0, room;
    int size = px( 16 );
    RECT text = *field, button = *field;

    fill_round_rect( hdc, field, (float)radius, col->field, 255 );
    text.right -= px( 34 );
    button.left = text.right;
    InflateRect( &button, 0, -px( 2 ) );
    button.right -= px( 2 );
    zone_add( c, Z_ADDRESS, 0, &text, TRUE );
    zone_add( c, Z_REFRESH, 0, &button, TRUE );
    if (is_hot( c, Z_REFRESH, 0 ))
        fill_round_rect( hdc, &button, (float)px( 3 ), c->down_kind == Z_REFRESH ? col->down : col->hover, 255 );
    draw_icon( hdc, c->edit ? I_FORWARD : I_REFRESH, (button.left + button.right - size) / 2,
               (button.top + button.bottom - size) / 2, size, c->edit ? col->text : col->dim );
    if (c->edit) return;
    room = text.right - text.left - px( 24 );
    for (int i = c->segment_count - 1; i >= 0; i--)
    {
        total += text_px( hdc, font, c->segment_name[i] ) + px( 24 );
        if (total > room) { first = i + 1; break; }
    }
    if (first) { draw_icon( hdc, I_MORE, x, (field->top + field->bottom - px( 16 )) / 2, px( 16 ), col->dim ); x += px( 20 ); }
    for (int i = first; i < c->segment_count; i++)
    {
        int w = text_px( hdc, font, c->segment_name[i] ) + px( 8 );
        RECT r = { x, field->top + px( 4 ), x + w, field->bottom - px( 4 ) }, t = r;

        zone_add( c, Z_SEGMENT, i, &r, TRUE );
        if (is_hot( c, Z_SEGMENT, i )) fill_round_rect( hdc, &r, (float)px( 3 ), col->hover, 255 );
        t.left += px( 4 );
        paint_text( hdc, c->segment_name[i], &t, font, col->text, DT_LEFT );
        x += w;
        if (i < c->segment_count - 1)
        {
            draw_icon( hdc, I_RIGHT, x + px( 1 ), (field->top + field->bottom - px( 12 )) / 2, px( 12 ), col->dim );
            x += px( 14 );
        }
    }
}

static void paint_search( struct chrome *c, HDC hdc, const struct colors *col, const RECT *field, HFONT font )
{
    int radius = c->style == EXPLORER_WIN11 ? px( 4 ) : 0, size = px( 16 );
    WCHAR text[128];
    RECT t = *field;

    fill_round_rect( hdc, field, (float)radius, is_hot( c, Z_SEARCH, 0 ) ? col->hover : col->field, 255 );
    zone_add( c, Z_SEARCH, 0, field, TRUE );
    swprintf( text, ARRAY_SIZE(text), load_string( IDS_EX_SEARCH_IN ), c->tab_count ? c->tabs[c->active].title : L"" );
    t.left += px( 10 );
    t.right -= px( 30 );
    if (c->query[0]) paint_text( hdc, c->query, &t, font, col->text, DT_LEFT );
    else if (!c->search) paint_text( hdc, text, &t, font, col->dim, DT_LEFT );
    draw_icon( hdc, I_SEARCH, field->right - size - px( 8 ), (field->top + field->bottom - size) / 2, size, col->dim );
}

/* back, forward, up, the address and the search */
static void paint_nav( struct chrome *c, HDC hdc, const struct colors *col, const RECT *row, HFONT font )
{
    struct tab *t = c->tab_count ? &c->tabs[c->active] : NULL;
    int side = px( 36 ), high = px( 32 ), y = (row->top + row->bottom - high) / 2, x = px( 8 ), search = px( 240 );
    BOOL has_parent = c->segment_count > 0;
    RECT r, field;

    SetRect( &r, x, y, x + side, y + high );
    paint_button( c, hdc, col, r, Z_BACK, 0, I_BACK, col->text, NULL, NULL, t && t->back_count, FALSE );
    OffsetRect( &r, side + px( 2 ), 0 );
    paint_button( c, hdc, col, r, Z_FORWARD, 0, I_FORWARD, col->text, NULL, NULL, t && t->forward_count, FALSE );
    OffsetRect( &r, side + px( 2 ), 0 );
    paint_button( c, hdc, col, r, Z_UP, 0, I_UP, col->text, NULL, NULL, has_parent, FALSE );

    SetRect( &field, r.right + px( 6 ), y, row->right - search - px( 16 ), y + high );
    paint_address( c, hdc, col, &field, font );
    SetRect( &field, row->right - search - px( 8 ), y, row->right - px( 8 ), y + high );
    paint_search( c, hdc, col, &field, font );
}

/* New, cut, copy, paste, rename, share, delete, sort, view, more */
static void paint_commands( struct chrome *c, HDC hdc, const struct colors *col, const RECT *row, HFONT font )
{
    int high = px( 36 ), y = (row->top + row->bottom - high) / 2, x = px( 8 ), side = px( 40 );
    BOOL any = c->selected > 0;
    RECT r;
    static const struct { int icon; UINT cmd; int need; } tools[] =
    {
        { I_CUT, CMD_CUT, NEED_SELECTION }, { I_COPY, CMD_COPY, NEED_SELECTION }, { I_PASTE, CMD_PASTE, NEED_CLIPBOARD },
        { I_RENAME, CMD_RENAME, NEED_SELECTION }, { I_SHARE, CMD_PROPERTIES, NEED_SELECTION }, { I_DELETE, CMD_DELETE, NEED_SELECTION },
    };

    {
        /* in Devices and Printers: "Додати пристрій" */
        const WCHAR *label = load_string( in_devices( c ) ? IDS_EX_ADD_DEVICE : IDS_EX_NEW );

        SetRect( &r, x, y, x + text_px( hdc, font, label ) + px( 60 ), y + high );
        paint_button( c, hdc, col, r, Z_NEW_MENU, 0, I_PLUS, col->accent, label, font, TRUE, TRUE );
    }
    x = r.right + px( 6 );
    {
        RECT sep = { x, y + px( 8 ), x + 1, y + high - px( 8 ) };

        fill( hdc, &sep, col->line );
    }
    x += px( 7 );
    for (int i = 0; i < ARRAY_SIZE(tools); i++, x += side + px( 2 ))
    {
        BOOL ok = tools[i].need == NEED_NONE || (tools[i].need == NEED_SELECTION && any) || (tools[i].need == NEED_CLIPBOARD && c->can_paste);

        SetRect( &r, x, y, x + side, y + high );
        paint_button( c, hdc, col, r, Z_COMMAND, tools[i].cmd, tools[i].icon, col->accent, NULL, NULL, ok, FALSE );
    }
    x += px( 5 );
    {
        RECT sep = { x, y + px( 8 ), x + 1, y + high - px( 8 ) };

        fill( hdc, &sep, col->line );
    }
    x += px( 7 );
    SetRect( &r, x, y, x + text_px( hdc, font, load_string( IDS_EX_SORT ) ) + px( 60 ), y + high );
    paint_button( c, hdc, col, r, Z_SORT_MENU, 0, I_SORT, col->text, load_string( IDS_EX_SORT ), font, TRUE, TRUE );
    x = r.right + px( 2 );
    SetRect( &r, x, y, x + text_px( hdc, font, load_string( IDS_EX_VIEW ) ) + px( 60 ), y + high );
    paint_button( c, hdc, col, r, Z_VIEW_MENU, 0, I_VIEW, col->text, load_string( IDS_EX_VIEW ), font, TRUE, TRUE );
    x = r.right + px( 2 );
    SetRect( &r, x, y, x + side, y + high );
    paint_button( c, hdc, col, r, Z_MORE_MENU, 0, I_MORE, col->text, NULL, NULL, TRUE, FALSE );
}

/* The ribbon of Windows 10 */
struct ribbon_button { UINT cmd; UINT label; int icon; int need; };
struct ribbon_group { UINT title; const struct ribbon_button *big; int big_count; const struct ribbon_button *small_list; int small_count; };

static const struct ribbon_button home_clip_big[] = { { CMD_COPY, IDS_EX_COPY, I_COPY, NEED_SELECTION }, { CMD_PASTE, IDS_EX_PASTE, I_PASTE, NEED_CLIPBOARD } };
static const struct ribbon_button home_clip_small[] = { { CMD_CUT, IDS_EX_CUT, I_CUT, NEED_SELECTION }, { CMD_COPY_PATH, IDS_EX_COPY_PATH, I_COPY, NEED_NONE }, { CMD_PASTE_LINK, IDS_EX_PASTE_LINK, I_PASTE, NEED_CLIPBOARD } };
static const struct ribbon_button home_org_big[] = { { CMD_MOVE_TO, IDS_EX_MOVE_TO, I_MOVE, NEED_SELECTION }, { CMD_COPY_TO, IDS_EX_COPY_TO, I_COPY, NEED_SELECTION }, { CMD_DELETE, IDS_EX_DELETE, I_DELETE, NEED_SELECTION }, { CMD_RENAME, IDS_EX_RENAME, I_RENAME, NEED_SELECTION } };
static const struct ribbon_button home_new_big[] = { { CMD_NEW_FOLDER, IDS_EX_NEW_FOLDER, I_NEW_FOLDER, NEED_NONE } };
static const struct ribbon_button home_open_big[] = { { CMD_PROPERTIES, IDS_EX_PROPERTIES, I_PROPS, NEED_SELECTION } };
static const struct ribbon_button home_sel_small[] = { { CMD_SELECT_ALL, IDS_EX_SELECT_ALL, I_SELECT, NEED_NONE }, { CMD_SELECT_NONE, IDS_EX_SELECT_NONE, I_SELECT, NEED_NONE }, { CMD_INVERT, IDS_EX_INVERT, I_SELECT, NEED_NONE } };

static const struct ribbon_group home_groups[] =
{
    { IDS_EX_CLIPBOARD, home_clip_big, ARRAY_SIZE(home_clip_big), home_clip_small, ARRAY_SIZE(home_clip_small) },
    { IDS_EX_ORGANIZE, home_org_big, ARRAY_SIZE(home_org_big), NULL, 0 },
    { IDS_EX_NEW, home_new_big, ARRAY_SIZE(home_new_big), NULL, 0 },
    { IDS_EX_OPEN, home_open_big, ARRAY_SIZE(home_open_big), NULL, 0 },
    { IDS_EX_SELECT, NULL, 0, home_sel_small, ARRAY_SIZE(home_sel_small) },
};

static const struct ribbon_button share_big[] = { { CMD_PROPERTIES, IDS_EX_PROPERTIES, I_SHARE, NEED_SELECTION }, { CMD_COPY_PATH, IDS_EX_COPY_PATH, I_COPY, NEED_NONE } };
static const struct ribbon_group share_groups[] = { { IDS_EX_SHARE_WITH, share_big, ARRAY_SIZE(share_big), NULL, 0 } };

static const struct ribbon_button view_panes_big[] = { { CMD_NAV_PANE, IDS_EX_NAV_PANE, I_NAVPANE, NEED_NONE } };
static const struct ribbon_button view_layout_big[] = { { CMD_LARGE, IDS_EX_LARGE, I_LARGE, NEED_NONE }, { CMD_DETAILS, IDS_EX_DETAILS, I_DETAILS, NEED_NONE }, { CMD_TILES, IDS_EX_TILES, I_TILES, NEED_NONE } };
static const struct ribbon_button view_layout_small[] = { { CMD_SMALL, IDS_EX_SMALL, I_VIEW, NEED_NONE }, { CMD_LIST, IDS_EX_LIST, I_LIST, NEED_NONE } };
static const struct ribbon_button view_sort_big[] = { { CMD_SORT_MENU, IDS_EX_SORT, I_SORT, NEED_NONE } };
static const struct ribbon_button view_opt_big[] = { { IDM_TOOLS_FOLDEROPTIONS, IDS_EX_OPTIONS, I_OPTIONS, NEED_NONE } };
static const struct ribbon_group view_groups[] =
{
    { IDS_EX_PANES, view_panes_big, ARRAY_SIZE(view_panes_big), NULL, 0 },
    { IDS_EX_LAYOUT, view_layout_big, ARRAY_SIZE(view_layout_big), view_layout_small, ARRAY_SIZE(view_layout_small) },
    { IDS_EX_CURRENT_VIEW, view_sort_big, ARRAY_SIZE(view_sort_big), NULL, 0 },
    { IDS_EX_OPTIONS, view_opt_big, ARRAY_SIZE(view_opt_big), NULL, 0 },
};

/* the coloured pictures of the classic toolbar, 36 pixels each */
#define STRIP_CELL 36

static UINT32 *strip_pixels;
static int strip_count;

static BOOL load_strip(void)
{
    static LONG tried;
    DIBSECTION dib;
    HMODULE module;
    HBITMAP bitmap;

    if (strip_pixels) return TRUE;
    if (InterlockedExchange( &tried, 1 )) return FALSE;
    if (!(module = GetModuleHandleW( L"browseui.dll" ))) return FALSE;
    if (!(bitmap = LoadImageW( module, MAKEINTRESOURCEW( 220 ), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION ))) return FALSE;
    if (GetObjectW( bitmap, sizeof(dib), &dib ) == sizeof(dib) && dib.dsBm.bmBitsPixel == 32 && dib.dsBm.bmBits)
    {
        int width = dib.dsBm.bmWidth, height = abs( dib.dsBm.bmHeight );
        UINT32 *pixels = malloc( width * height * sizeof(*pixels) );

        if (pixels)
        {
            /* top-down, and premultiplied for AlphaBlend */
            for (int y = 0; y < height; y++)
            {
                const UINT32 *src = (const UINT32 *)((const BYTE *)dib.dsBm.bmBits + (height - 1 - y) * dib.dsBm.bmWidthBytes);

                for (int x = 0; x < width; x++)
                {
                    UINT32 p = src[x], a = p >> 24;

                    pixels[y * width + x] = (a << 24) | ((((p >> 16) & 0xff) * a / 255) << 16) | ((((p >> 8) & 0xff) * a / 255) << 8) |
                                            ((p & 0xff) * a / 255);
                }
            }
            strip_count = width / STRIP_CELL;
            strip_pixels = pixels;
        }
    }
    DeleteObject( bitmap );
    return strip_pixels != NULL;
}

static int strip_index( UINT cmd )
{
    switch (cmd)
    {
    case CMD_CUT: return 5;
    case CMD_COPY: case CMD_COPY_PATH: return 6;
    case CMD_PASTE: case CMD_PASTE_LINK: return 7;
    case CMD_DELETE: return 10;
    case CMD_PROPERTIES: return 15;
    case CMD_LARGE: return 20;
    case CMD_LIST: case CMD_SMALL: return 21;
    case CMD_TILES: return 22;
    case CMD_DETAILS: return 23;
    case CMD_NEW_FOLDER: return 31;
    case CMD_RENAME: return 39;
    case CMD_MOVE_TO: return 44;
    case CMD_COPY_TO: return 45;
    }
    return -1;
}

static void draw_ribbon_icon( HDC hdc, const struct ribbon_button *b, int x, int y, int size, COLORREF ink, BOOL enabled )
{
    int index = strip_index( b->cmd );

    if (index >= 0 && load_strip() && index < strip_count)
    {
        BITMAPINFO info = { { sizeof(info.bmiHeader), STRIP_CELL, -STRIP_CELL, 1, 32, BI_RGB } };
        BLENDFUNCTION blend = { AC_SRC_OVER, 0, enabled ? 255 : 90, AC_SRC_ALPHA };
        HDC mem = CreateCompatibleDC( hdc );
        UINT32 *bits;
        HBITMAP bitmap = CreateDIBSection( hdc, &info, DIB_RGB_COLORS, (void **)&bits, NULL, 0 );

        if (bitmap)
        {
            HGDIOBJ old = SelectObject( mem, bitmap );

            for (int row = 0; row < STRIP_CELL; row++)
                memcpy( bits + row * STRIP_CELL, strip_pixels + row * strip_count * STRIP_CELL + index * STRIP_CELL, STRIP_CELL * sizeof(*bits) );
            GdiAlphaBlend( hdc, x, y, size, size, mem, 0, 0, STRIP_CELL, STRIP_CELL, blend );
            SelectObject( mem, old );
            DeleteObject( bitmap );
        }
        DeleteDC( mem );
        return;
    }
    draw_icon( hdc, b->icon, x, y, size, ink );
}

/* a button of the ribbon breaks its label between words, never inside one */
static int longest_word( HDC hdc, HFONT font, const WCHAR *text )
{
    WCHAR word[64];
    int longest = 0, n = 0;

    for (;; text++)
    {
        if (*text && *text != ' ' && n < ARRAY_SIZE(word) - 1)
        {
            word[n++] = *text;
            continue;
        }
        word[n] = 0;
        longest = max( longest, text_px( hdc, font, word ) );
        n = 0;
        if (!*text) return longest;
    }
}

static BOOL allowed( const struct chrome *c, int need )
{
    return need == NEED_NONE || (need == NEED_SELECTION && c->selected > 0) || (need == NEED_CLIPBOARD && c->can_paste);
}

static void paint_ribbon( struct chrome *c, HDC hdc, const struct colors *col, const RECT *body, HFONT font, HFONT font_small )
{
    const struct ribbon_group *groups = c->ribbon_tab == 2 ? view_groups : c->ribbon_tab == 1 ? share_groups : home_groups;
    int count = c->ribbon_tab == 2 ? ARRAY_SIZE(view_groups) : c->ribbon_tab == 1 ? ARRAY_SIZE(share_groups) : ARRAY_SIZE(home_groups);
    int x = body->left + px( 4 ), top = body->top + px( 3 ), big_w = px( 62 ), big_h = px( 68 ), row_h = px( 22 ), icon = px( 26 );

    for (int g = 0; g < count; g++)
    {
        const struct ribbon_group *group = &groups[g];
        int start = x, small_w = 0;
        RECT r, t;

        for (int i = 0; i < group->big_count; i++)
        {
            const struct ribbon_button *b = &group->big[i];
            BOOL ok = allowed( c, b->need );
            COLORREF ink = ok ? col->accent : (col->dark ? RGB( 0x70, 0x70, 0x70 ) : RGB( 0xb0, 0xb0, 0xb0 ));
            int wide = max( big_w, longest_word( hdc, font_small, load_string( b->label ) ) + px( 12 ) );

            SetRect( &r, x, top, x + wide - px( 2 ), top + big_h );
            zone_add( c, Z_COMMAND, b->cmd, &r, ok );
            if (ok && is_hot( c, Z_COMMAND, b->cmd )) fill( hdc, &r, col->hover );
            draw_ribbon_icon( hdc, b, (r.left + r.right - icon) / 2, top + px( 6 ), icon, ink, ok );
            SetRect( &t, r.left + px( 1 ), top + px( 34 ), r.right - px( 1 ), r.bottom );
            paint_text( hdc, load_string( b->label ), &t, font_small, ok ? col->text : ink, DT_CENTER | DT_WORDBREAK );
            x += wide;
        }
        for (int i = 0; i < group->small_count; i++)
        {
            int w = text_px( hdc, font_small, load_string( group->small_list[i].label ) ) + px( 34 );

            small_w = max( small_w, w );
        }
        for (int i = 0; i < group->small_count; i++)
        {
            const struct ribbon_button *b = &group->small_list[i];
            BOOL ok = allowed( c, b->need );
            COLORREF ink = ok ? col->accent : (col->dark ? RGB( 0x70, 0x70, 0x70 ) : RGB( 0xb0, 0xb0, 0xb0 ));

            SetRect( &r, x, top + i * row_h, x + small_w, top + (i + 1) * row_h );
            zone_add( c, Z_COMMAND, b->cmd, &r, ok );
            if (ok && is_hot( c, Z_COMMAND, b->cmd )) fill( hdc, &r, col->hover );
            draw_ribbon_icon( hdc, b, r.left + px( 4 ), r.top + px( 3 ), px( 16 ), ink, ok );
            SetRect( &t, r.left + px( 26 ), r.top, r.right, r.bottom );
            paint_text( hdc, load_string( b->label ), &t, font_small, ok ? col->text : ink, DT_LEFT );
        }
        x += small_w ? small_w + px( 4 ) : 0;
        x = max( x, start + px( 62 ) );
        SetRect( &t, start, body->bottom - px( 20 ), x, body->bottom - px( 3 ) );
        paint_text( hdc, load_string( group->title ), &t, font_small, col->dim, DT_CENTER );
        SetRect( &r, x + px( 2 ), top + px( 2 ), x + px( 3 ), body->bottom - px( 5 ) );
        fill( hdc, &r, col->line );
        x += px( 7 );
    }
    (void)font;
}

static void paint_top( struct chrome *c )
{
    struct colors col;
    struct palette pal;
    PAINTSTRUCT ps;
    HBITMAP bitmap;
    HGDIOBJ old;
    RECT client, row;
    HFONT font, font_small;
    HDC mem;

    get_colors( &col );
    palette_init( &pal, !col.dark );
    if (!(mem = paint_begin( c->top, &ps, &pal, &bitmap, &old ))) return;
    GetClientRect( c->top, &client );
    font = shell_font( px( 13 ), FW_NORMAL );
    font_small = shell_font( px( 12 ), FW_NORMAL );
    c->zone_count = 0;
    if (c->style != EXPLORER_WIN11) fill( mem, &client, col.strip );
    else
    {
        row = client;
        row.top = TAB_H;
        fill( mem, &row, col.bar );
    }

    if (c->style == EXPLORER_WIN11)
    {
        SetRect( &row, 0, 0, client.right, TAB_H );
        if (!c->backdrop) fill( mem, &row, col.strip );
        paint_tabs( c, mem, &col, &client, font );
        SetRect( &row, 0, TAB_H, client.right, TAB_H + NAV_H );
        paint_nav( c, mem, &col, &row, font );
        SetRect( &row, 0, TAB_H + NAV_H, client.right, TAB_H + NAV_H + CMD_H );
        paint_commands( c, mem, &col, &row, font );
        SetRect( &row, 0, client.bottom - 1, client.right, client.bottom );
        fill( mem, &row, col.line );
    }
    else
    {
        static const UINT names[] = { IDS_EX_HOME, IDS_EX_SHARE, IDS_EX_VIEW_TAB };
        int x = 0, w = px( 54 );

        SetRect( &row, 0, 0, w, RIBTAB_H );
        zone_add( c, Z_FILE_MENU, 0, &row, TRUE );
        fill( mem, &row, is_hot( c, Z_FILE_MENU, 0 ) ? RGB( 0x29, 0x8b, 0xe0 ) : RGB( 0x1a, 0x75, 0xc9 ) );
        paint_text( mem, load_string( IDS_EX_FILE ), &row, font_small, RGB( 255, 255, 255 ), DT_CENTER );
        x = w;
        for (int i = 0; i < ARRAY_SIZE(names); i++)
        {
            int wide = text_px( mem, font_small, load_string( names[i] ) ) + px( 24 );

            SetRect( &row, x, 0, x + wide, RIBTAB_H + 1 );
            zone_add( c, Z_RIBBON_TAB, i, &row, TRUE );
            if (i == c->ribbon_tab) fill( mem, &row, col.bar );
            else if (is_hot( c, Z_RIBBON_TAB, i )) fill( mem, &row, col.hover );
            paint_text( mem, load_string( names[i] ), &row, font_small, col.text, DT_CENTER );
            x += wide;
        }
        SetRect( &row, 0, RIBTAB_H, client.right, RIBTAB_H + RIBBON_H );
        fill( mem, &row, col.bar );
        paint_ribbon( c, mem, &col, &row, font, font_small );
        SetRect( &row, 0, RIBTAB_H + RIBBON_H - 1, client.right, RIBTAB_H + RIBBON_H );
        fill( mem, &row, col.line );
        SetRect( &row, 0, RIBTAB_H + RIBBON_H, client.right, client.bottom );
        fill( mem, &row, col.dark ? RGB( 0x20, 0x20, 0x20 ) : RGB( 0xff, 0xff, 0xff ) );
        paint_nav( c, mem, &col, &row, font );
    }
    row = client;
    if (c->backdrop) row.top = TAB_H;
    make_opaque( mem, &row );
    DeleteObject( font );
    DeleteObject( font_small );
    paint_end( c->top, &ps, mem, bitmap, old );
}

static void paint_bottom( struct chrome *c )
{
    struct colors col;
    struct palette pal;
    PAINTSTRUCT ps;
    HBITMAP bitmap;
    HGDIOBJ old;
    RECT client, r;
    HFONT font;
    HDC mem;
    static const struct { int icon; UINT cmd; } views[] = { { I_DETAILS, CMD_DETAILS }, { I_TILES, CMD_TILES } };

    get_colors( &col );
    palette_init( &pal, !col.dark );
    if (!(mem = paint_begin( c->bottom, &ps, &pal, &bitmap, &old ))) return;
    GetClientRect( c->bottom, &client );
    font = shell_font( px( 12 ), FW_NORMAL );
    fill( mem, &client, col.bar );
    SetRect( &r, px( 12 ), 0, client.right - px( 80 ), client.bottom );
    paint_text( mem, c->status, &r, font, col.text, DT_LEFT );
    c->low_count = 0;
    for (int i = 0; i < ARRAY_SIZE(views); i++)
    {
        SetRect( &r, client.right - px( 58 ) + i * px( 26 ), px( 2 ), client.right - px( 34 ) + i * px( 26 ), client.bottom - px( 2 ) );
        c->low[i].kind = Z_COMMAND;
        c->low[i].index = views[i].cmd;
        c->low[i].rect = r;
        c->low[i].enabled = TRUE;
        c->low_count++;
        if (c->low_hot == i) fill_round_rect( mem, &r, (float)px( 3 ), col.hover, 255 );
        draw_icon( mem, views[i].icon, (r.left + r.right - px( 16 )) / 2, (r.top + r.bottom - px( 16 )) / 2, px( 16 ), col.text );
    }
    make_opaque( mem, &client );
    DeleteObject( font );
    paint_end( c->bottom, &ps, mem, bitmap, old );
}

/**********************************************************************
 *          The Control Panel's items where the tree was, as Windows 11's
 *          side shows its settings: they stay there on every page of it
 */

static LPCITEMIDLIST controls_pidl(void)
{
    static LPITEMIDLIST controls;

    if (!controls) SHGetSpecialFolderLocation( NULL, CSIDL_CONTROLS, &controls );
    return controls;
}

static BOOL is_controls( LPCITEMIDLIST pidl )
{
    LPCITEMIDLIST controls = controls_pidl();

    return controls && pidl && (ILIsEqual( controls, pidl ) || ILIsParent( controls, pidl, FALSE ));
}

static int __cdecl compare_side( const void *a, const void *b )
{
    const struct side_item *x = a, *y = b;

    return CompareStringW( LOCALE_USER_DEFAULT, NORM_IGNORECASE, x->name, -1, y->name, -1 ) - CSTR_EQUAL;
}

static void side_add( struct chrome *c, LPITEMIDLIST pidl, int *capacity )
{
    struct side_item *item;
    SHFILEINFOW info;

    if (!pidl) return;
    memset( &info, 0, sizeof(info) );
    info.dwAttributes = SFGAO_FOLDER;
    if (!SHGetFileInfoW( (const WCHAR *)pidl, 0, &info, sizeof(info),
                         SHGFI_PIDL | SHGFI_DISPLAYNAME | SHGFI_ICON | SHGFI_LARGEICON | SHGFI_ATTRIBUTES | SHGFI_ATTR_SPECIFIED ) ||
        !info.szDisplayName[0])
    {
        if (info.hIcon) DestroyIcon( info.hIcon );
        ILFree( pidl );
        return;
    }
    if (c->side_count == *capacity)
    {
        struct side_item *grown = realloc( c->side_items, (*capacity = max( 32, *capacity * 2 )) * sizeof(*grown) );

        if (!grown)
        {
            if (info.hIcon) DestroyIcon( info.hIcon );
            ILFree( pidl );
            return;
        }
        c->side_items = grown;
    }
    item = &c->side_items[c->side_count++];
    item->pidl = pidl;
    lstrcpynW( item->name, info.szDisplayName, ARRAY_SIZE(item->name) );
    item->folder = (info.dwAttributes & SFGAO_FOLDER) != 0;
    item->icon = NULL;
    if (info.hIcon)
    {
        item->icon = icon_bitmap( info.hIcon, px( 32 ) );
        DestroyIcon( info.hIcon );
    }
}

/* the Control Panel itself first, then its items by name */
static void side_load( struct chrome *c )
{
    LPCITEMIDLIST controls = controls_pidl();
    IShellFolder *desktop, *folder;
    LPITEMIDLIST child;
    IEnumIDList *list;
    int capacity = 0;

    if (c->side_items || !controls) return;
    side_add( c, ILClone( controls ), &capacity );
    if (FAILED(SHGetDesktopFolder( &desktop ))) return;
    if (SUCCEEDED(IShellFolder_BindToObject( desktop, controls, NULL, &IID_IShellFolder, (void **)&folder )))
    {
        if (IShellFolder_EnumObjects( folder, c->browser, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &list ) == S_OK)
        {
            while (IEnumIDList_Next( list, 1, &child, NULL ) == S_OK)
            {
                side_add( c, ILCombine( controls, child ), &capacity );
                ILFree( child );
            }
            IEnumIDList_Release( list );
        }
        IShellFolder_Release( folder );
    }
    IShellFolder_Release( desktop );
    if (c->side_count > 1) qsort( c->side_items + 1, c->side_count - 1, sizeof(*c->side_items), compare_side );
}

static void side_free( struct chrome *c )
{
    for (int i = 0; i < c->side_count; i++)
    {
        ILFree( c->side_items[i].pidl );
        if (c->side_items[i].icon) DeleteObject( c->side_items[i].icon );
    }
    free( c->side_items );
    c->side_items = NULL;
    c->side_count = 0;
}

/* the item the window is in, or one of whose pages */
static void side_select( struct chrome *c, LPCITEMIDLIST pidl )
{
    c->side_selected = -1;
    for (int i = 1; i < c->side_count && c->side_selected < 0; i++)
        if (ILIsEqual( c->side_items[i].pidl, pidl ) || ILIsParent( c->side_items[i].pidl, pidl, FALSE ))
            c->side_selected = i;
    if (c->side_selected < 0 && c->side_count) c->side_selected = 0;
}

/* The list's width, dragged by its edge, is kept for every window
 * (HKCU\Software\Arctic\Explorer, ControlPanelPaneWidth, at 96 DPI) */
#define SPLIT_W px( 5 )

static int side_width_saved(void)
{
    DWORD value = 0, size = sizeof(value);

    if (RegGetValueW( HKEY_CURRENT_USER, L"Software\\Arctic\\Explorer", L"ControlPanelPaneWidth", RRF_RT_REG_DWORD,
                      NULL, &value, &size ) || value < 120 || value > 2000)
        value = 240;
    return px( value );
}

static void side_width_save( int width )
{
    DWORD value = MulDiv( width, 96, GetDpiForSystem() );

    RegSetKeyValueW( HKEY_CURRENT_USER, L"Software\\Arctic\\Explorer", L"ControlPanelPaneWidth", REG_DWORD,
                     &value, sizeof(value) );
}

/* not narrower than its icons, and the folder keeps room of its own */
static int side_width_fit( int width, int window_width )
{
    return max( px( 140 ), min( width, max( px( 140 ), window_width - px( 320 ) ) ) );
}

static int side_content( struct chrome *c )
{
    return px( 8 ) + c->side_count * SIDE_ROW;
}

static void side_scroll_to( struct chrome *c, int pos )
{
    RECT client;
    SCROLLINFO info = { sizeof(info), SIF_ALL };

    GetClientRect( c->side, &client );
    c->side_scroll = max( 0, min( pos, side_content( c ) - client.bottom ) );
    info.nMin = 0;
    info.nMax = side_content( c ) - 1;
    info.nPage = client.bottom;
    info.nPos = c->side_scroll;
    SetScrollInfo( c->side, SB_VERT, &info, TRUE );
    InvalidateRect( c->side, NULL, FALSE );
}

static int side_at( struct chrome *c, int y )
{
    int index = (y + c->side_scroll - px( 4 )) / SIDE_ROW;

    return y + c->side_scroll >= px( 4 ) && index < c->side_count ? index : -1;
}

static void side_open( struct chrome *c, int index )
{
    struct side_item *item;
    SHELLEXECUTEINFOW info = { sizeof(info) };

    if (index < 0 || index >= c->side_count) return;
    item = &c->side_items[index];
    if (!index || item->folder)
    {
        browse( c, item->pidl, FALSE );
        return;
    }
    /* an applet: its window, as from the Control Panel's view */
    info.fMask = SEE_MASK_INVOKEIDLIST | SEE_MASK_FLAG_NO_UI;
    info.lpIDList = item->pidl;
    info.hwnd = c->browser;
    info.nShow = SW_SHOWNORMAL;
    ShellExecuteExW( &info );
}

static void paint_side( struct chrome *c )
{
    struct colors col;
    struct palette pal;
    PAINTSTRUCT ps;
    HBITMAP bitmap;
    HGDIOBJ old;
    RECT client;
    HFONT font;
    HDC mem;

    get_colors( &col );
    palette_init( &pal, !col.dark );
    if (!(mem = paint_begin( c->side, &ps, &pal, &bitmap, &old ))) return;
    GetClientRect( c->side, &client );
    fill( mem, &client, GetSysColor( COLOR_WINDOW ) );
    font = shell_font( px( 13 ), FW_NORMAL );
    for (int i = 0; i < c->side_count; i++)
    {
        struct side_item *item = &c->side_items[i];
        int y = px( 4 ) + i * SIDE_ROW - c->side_scroll;
        RECT row = { px( 4 ), y + px( 2 ), client.right - px( 4 ), y + SIDE_ROW - px( 2 ) }, text;

        if (row.bottom < 0 || row.top > client.bottom) continue;
        if (i == c->side_selected)
            fill_round_rect( mem, &row, (float)px( 4 ), col.dark ? RGB( 0x2d, 0x2d, 0x2d ) : RGB( 0xe6, 0xe6, 0xe6 ), 255 );
        else if (i == c->side_hot)
            fill_round_rect( mem, &row, (float)px( 4 ), col.dark ? RGB( 0x26, 0x26, 0x26 ) : RGB( 0xf0, 0xf0, 0xf0 ), 255 );
        if (item->icon) draw_icon_bitmap( mem, item->icon, row.left + px( 8 ), (row.top + row.bottom - px( 32 )) / 2, px( 32 ) );
        SetRect( &text, row.left + px( 50 ), row.top, row.right - px( 6 ), row.bottom );
        paint_text( mem, item->name, &text, font, col.text, DT_LEFT );
    }
    make_opaque( mem, &client );
    DeleteObject( font );
    paint_end( c->side, &ps, mem, bitmap, old );
}

static LRESULT WINAPI side_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct chrome *c = (struct chrome *)GetWindowLongPtrW( hwnd, GWLP_USERDATA );

    if (!c) return DefWindowProcW( hwnd, msg, wparam, lparam );
    switch (msg)
    {
    case WM_MOUSEMOVE:
    {
        int hot = side_at( c, (short)HIWORD( lparam ) );

        if (hot != c->side_hot)
        {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };

            TrackMouseEvent( &tme );
            c->side_hot = hot;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        c->side_hot = -1;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    case WM_LBUTTONUP:
        side_open( c, side_at( c, (short)HIWORD( lparam ) ) );
        return 0;
    case WM_MOUSEWHEEL:
        side_scroll_to( c, c->side_scroll - GET_WHEEL_DELTA_WPARAM( wparam ) * SIDE_ROW * 3 / WHEEL_DELTA );
        return 0;
    case WM_VSCROLL:
    {
        SCROLLINFO info = { sizeof(info), SIF_ALL };
        RECT client;

        GetScrollInfo( hwnd, SB_VERT, &info );
        GetClientRect( hwnd, &client );
        switch (LOWORD( wparam ))
        {
        case SB_LINEUP: side_scroll_to( c, c->side_scroll - SIDE_ROW ); break;
        case SB_LINEDOWN: side_scroll_to( c, c->side_scroll + SIDE_ROW ); break;
        case SB_PAGEUP: side_scroll_to( c, c->side_scroll - client.bottom ); break;
        case SB_PAGEDOWN: side_scroll_to( c, c->side_scroll + client.bottom ); break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: side_scroll_to( c, info.nTrackPos ); break;
        case SB_TOP: side_scroll_to( c, 0 ); break;
        case SB_BOTTOM: side_scroll_to( c, side_content( c ) ); break;
        }
        return 0;
    }
    case WM_SIZE:
        side_scroll_to( c, c->side_scroll );
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint_side( c );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

/* the edge between the list and the folder: dragged, the list's width */
static LRESULT WINAPI split_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct chrome *c = (struct chrome *)GetWindowLongPtrW( hwnd, GWLP_USERDATA );

    if (!c) return DefWindowProcW( hwnd, msg, wparam, lparam );
    switch (msg)
    {
    case WM_LBUTTONDOWN:
        c->split_grab = (short)LOWORD( lparam );
        SetCapture( hwnd );
        return 0;
    case WM_MOUSEMOVE:
        if (GetCapture() == hwnd)
        {
            POINT pt = { (short)LOWORD( lparam ), 0 };
            RECT client;
            int width;

            MapWindowPoints( hwnd, c->browser, &pt, 1 );
            GetClientRect( c->browser, &client );
            width = side_width_fit( pt.x - c->split_grab, client.right );
            if (width != c->side_width)
            {
                c->side_width = width;
                SendMessageW( c->browser, WM_ARCTIC_RELAYOUT, 0, 0 );
                UpdateWindow( c->browser );
            }
        }
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == hwnd) ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        side_width_save( c->side_width );
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint( hwnd, &ps );
        HBRUSH brush = CreateSolidBrush( GetSysColor( COLOR_WINDOW ) );

        FillRect( hdc, &ps.rcPaint, brush );
        DeleteObject( brush );
        EndPaint( hwnd, &ps );
        return 0;
    }
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

/* where the window went: the list comes or goes with the Control Panel */
static void side_navigated( struct chrome *c, LPCITEMIDLIST pidl )
{
    BOOL in = is_controls( pidl ) && !is_find_folder( pidl );

    if (in && !c->side_width) c->side_width = side_width_saved();
    c->in_controls = in;
    if (in)
    {
        side_load( c );
        side_select( c, pidl );
        if (c->side) InvalidateRect( c->side, NULL, FALSE );
    }
    /* browseui lays the view out anew, and asks ArcticExplorerLayout */
    if ((in && nav_pane_wanted()) != c->side_shown) SendMessageW( c->browser, WM_ARCTIC_RELAYOUT, 0, 0 );
}

/**********************************************************************
 *          The windows
 */

static const struct zone *zone_at( const struct chrome *c, POINT pt )
{
    /* the last one laid out is on top: a tab's × over its tab */
    for (int i = c->zone_count - 1; i >= 0; i--)
        if (PtInRect( &c->zones[i].rect, pt )) return &c->zones[i];
    return NULL;
}

static void activate_zone( struct chrome *c, const struct zone *z )
{
    RECT r = z->rect;
    struct tab *t = &c->tabs[c->active];
    int index = z->index;

    switch (z->kind)
    {
    case Z_TAB: switch_tab( c, index ); break;
    case Z_TAB_CLOSE: close_tab( c, index ); break;
    case Z_NEW_TAB: new_tab( c, NULL ); break;
    case Z_MIN: PostMessageW( c->browser, WM_SYSCOMMAND, SC_MINIMIZE, 0 ); break;
    case Z_MAX: PostMessageW( c->browser, WM_SYSCOMMAND, IsZoomed( c->browser ) ? SC_RESTORE : SC_MAXIMIZE, 0 ); break;
    case Z_CLOSE: PostMessageW( c->browser, WM_CLOSE, 0, 0 ); break;
    case Z_BACK: go_back( c ); break;
    case Z_FORWARD: go_forward( c ); break;
    case Z_UP: if (c->segment_count > 1) browse( c, c->segment[c->segment_count - 2], FALSE ); else command( c, IDM_GOTO_UPONELEVEL ); break;
    case Z_REFRESH: if (c->edit) edit_finish( c, TRUE ); else command( c, CMD_REFRESH ); break;
    case Z_SEGMENT:
        if (index < c->segment_count - 1) browse( c, c->segment[index], FALSE );
        else
            for (int i = 0; i < c->zone_count; i++)
                if (c->zones[i].kind == Z_ADDRESS) { r = c->zones[i].rect; edit_start( c, &r ); break; }
        break;
    case Z_ADDRESS: edit_start( c, &r ); break;
    case Z_SEARCH: search_start( c, &r ); break;
    case Z_COMMAND: if (index == CMD_SORT_MENU) sort_menu( c, &r ); else run_command( c, index ); break;
    case Z_NEW_MENU: new_menu( c, &r ); break;
    case Z_SORT_MENU: sort_menu( c, &r ); break;
    case Z_VIEW_MENU: view_menu( c, &r ); break;
    case Z_MORE_MENU: more_menu( c, &r ); break;
    case Z_RIBBON_TAB: c->ribbon_tab = index; redraw( c ); break;
    case Z_FILE_MENU: file_menu( c, &r ); break;
    }
    (void)t;
}

/* what is selected and what can be pasted, as the buttons show it */
static void poll_state( struct chrome *c )
{
    HWND list = find_child( c->browser, L"SysListView32" ), tree, bar;
    int selected = list ? (int)SendMessageW( list, LVM_GETSELECTEDCOUNT, 0, 0 ) : 0;
    BOOL paste = IsClipboardFormatAvailable( CF_HDROP ) || IsClipboardFormatAvailable( RegisterClipboardFormatW( L"Shell IDList Array" ) );

    /* what the view says of itself goes to the old status bar, hidden */
    if (c->bottom && (bar = find_child( c->browser, L"msctls_statusbar32" )))
    {
        WCHAR text[ARRAY_SIZE(c->status)] = L"";

        if (LOWORD( SendMessageW( bar, SB_GETTEXTLENGTHW, 0, 0 ) ) < ARRAY_SIZE(text)) SendMessageW( bar, SB_GETTEXTW, 0, (LPARAM)text );
        if (lstrcmpW( text, c->status ))
        {
            lstrcpyW( c->status, text );
            InvalidateRect( c->bottom, NULL, FALSE );
        }
    }
    if (selected != c->selected || paste != c->can_paste)
    {
        c->selected = selected;
        c->can_paste = paste;
        InvalidateRect( c->top, NULL, FALSE );
    }
    if (!c->tree_done && (tree = find_child( c->browser, L"SysTreeView32" )))
    {
        DWORD style = GetWindowLongW( tree, GWL_STYLE );

        SetWindowLongW( tree, GWL_STYLE, (style & ~TVS_HASLINES) | TVS_FULLROWSELECT | TVS_TRACKSELECT );
        SendMessageW( tree, TVM_SETITEMHEIGHT, px( 30 ), 0 );
        SendMessageW( tree, TVM_SETINDENT, px( 16 ), 0 );
        InvalidateRect( tree, NULL, TRUE );
        c->tree_done = TRUE;
    }
}

static LRESULT WINAPI top_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct chrome *c = (struct chrome *)GetWindowLongPtrW( hwnd, GWLP_USERDATA );
    POINT pt = { (short)LOWORD( lparam ), (short)HIWORD( lparam ) };
    const struct zone *z;

    if (!c) return DefWindowProcW( hwnd, msg, wparam, lparam );
    switch (msg)
    {
    case WM_NCHITTEST:
        ScreenToClient( hwnd, &pt );
        if (c->style == EXPLORER_WIN11 && pt.y < TAB_H && (!(z = zone_at( c, pt )) || pt.y < px( 4 ))) return HTTRANSPARENT;
        return HTCLIENT;

    case WM_MOUSEMOVE:
        z = zone_at( c, pt );
        {
            int kind = z && z->enabled ? z->kind : Z_NONE, index = z && z->enabled ? z->index : -1;

            if (kind != c->hot_kind || index != c->hot_index)
            {
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };

                TrackMouseEvent( &tme );
                c->hot_kind = kind;
                c->hot_index = index;
                InvalidateRect( hwnd, NULL, FALSE );
            }
        }
        return 0;

    case WM_MOUSELEAVE:
        c->hot_kind = c->down_kind = Z_NONE;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;

    case WM_LBUTTONDOWN:
        z = zone_at( c, pt );
        c->down_kind = z && z->enabled ? z->kind : Z_NONE;
        c->down_index = z ? z->index : -1;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;

    case WM_LBUTTONUP:
        z = zone_at( c, pt );
        if (z && z->enabled && z->kind == c->down_kind && z->index == c->down_index)
        {
            struct zone copy = *z;

            c->down_kind = Z_NONE;
            activate_zone( c, &copy );
        }
        c->down_kind = Z_NONE;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;

    case WM_MBUTTONUP:
        if ((z = zone_at( c, pt )) && z->kind == Z_TAB) close_tab( c, z->index );
        return 0;

    case WM_CTLCOLOREDIT:
    {
        static HBRUSH brush_dark, brush_light;
        struct colors col;

        get_colors( &col );
        if (!brush_dark) brush_dark = CreateSolidBrush( RGB( 0x38, 0x38, 0x38 ) );
        if (!brush_light) brush_light = CreateSolidBrush( RGB( 0xff, 0xff, 0xff ) );
        SetTextColor( (HDC)wparam, col.text );
        SetBkColor( (HDC)wparam, col.field );
        return (LRESULT)(col.dark ? brush_dark : brush_light);
    }

    case WM_COMMAND:
        if (LOWORD( wparam ) == ID_SEARCH && HIWORD( wparam ) == EN_CHANGE && c->search)
            SetTimer( hwnd, TIMER_SEARCH, 500, NULL );
        break;

    case WM_TIMER:
        if (wparam == TIMER_POLL) poll_state( c );
        if (wparam == TIMER_SEARCH) search_run( c );
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        paint_top( c );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

static LRESULT WINAPI bottom_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct chrome *c = (struct chrome *)GetWindowLongPtrW( hwnd, GWLP_USERDATA );
    POINT pt = { (short)LOWORD( lparam ), (short)HIWORD( lparam ) };
    int hit = -1;

    if (!c) return DefWindowProcW( hwnd, msg, wparam, lparam );
    for (int i = 0; i < c->low_count; i++) if (PtInRect( &c->low[i].rect, pt )) hit = i;
    switch (msg)
    {
    case WM_MOUSEMOVE:
        if (hit != c->low_hot)
        {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };

            TrackMouseEvent( &tme );
            c->low_hot = hit;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        return 0;
    case WM_MOUSELEAVE:
        c->low_hot = -1;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    case WM_LBUTTONUP:
        if (hit >= 0) run_command( c, c->low[hit].index );
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint_bottom( c );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

/* the row of the tabs over what the title bars have behind them: the same
 * settings as uxtheme's (desk.cpl, HKCU\\Software\\Arctic\\Caption) */
static DWORD caption_value( const WCHAR *name, DWORD def, BOOL *found )
{
    DWORD value = def, size = sizeof(value);
    BOOL ok = !RegGetValueW( HKEY_CURRENT_USER, L"Software\\Arctic\\Caption", name, RRF_RT_REG_DWORD, NULL, &value, &size );

    if (found) *found = ok;
    return ok ? value : def;
}

/* how far below the window's top its client area begins */
static int client_top( struct chrome *c )
{
    POINT origin = { 0, 0 };
    RECT window;

    if (!GetWindowRect( c->browser, &window ) || !ClientToScreen( c->browser, &origin )) return 0;
    return max( 0, origin.y - window.top );
}

static void set_caption_backdrop( struct chrome *c )
{
    struct arctic_caption cap = { 0 };
    struct colors col;
    DWORD effect = caption_value( L"Effect", ARCTIC_ACCENT_ENABLE_ACRYLICBLURBEHIND, NULL ), alpha;
    BOOL has_color;
    COLORREF color;

    get_colors( &col );
    color = caption_value( L"Color", 0, &has_color ) & 0xffffff;
    if (!has_color) color = col.dark ? RGB( 0x20, 0x20, 0x20 ) : RGB( 0xf3, 0xf3, 0xf3 );
    alpha = effect == ARCTIC_ACCENT_ENABLE_GRADIENT ? 255 : min( caption_value( L"Opacity", 75, NULL ), 100 ) * 255 / 100;
    c->backdrop = effect != 0;
    c->caption_top = client_top( c );
    if (c->backdrop)
    {
        /* dwm counts the rows from the window's top: a maximized window's
         * frame reaches past the screen, above the tabs */
        cap.height = TAB_H + c->caption_top;
        cap.accent = effect;
        cap.color = (alpha << 24) | (GetBValue( color ) << 16) | (GetGValue( color ) << 8) | GetRValue( color );
        cap.blur = max( 1, min( caption_value( L"Blur", 60, NULL ), 100 ) );
        cap.light = !col.dark;
    }
    DwmSetWindowAttribute( c->browser, DWMWA_ARCTIC_CAPTION, &cap, sizeof(cap) );
}

/* the thin frame around the client area, in the colour of the rows */
static void paint_frame( struct chrome *c )
{
    struct colors col;
    RECT window, client;
    POINT origin = { 0, 0 };
    HDC hdc = GetWindowDC( c->browser );
    HBRUSH brush;

    if (!hdc) return;
    get_colors( &col );
    GetWindowRect( c->browser, &window );
    GetClientRect( c->browser, &client );
    ClientToScreen( c->browser, &origin );
    OffsetRect( &client, origin.x - window.left, origin.y - window.top );
    OffsetRect( &window, -window.left, -window.top );
    ExcludeClipRect( hdc, client.left, client.top, client.right, client.bottom );
    brush = CreateSolidBrush( col.bar );
    FillRect( hdc, &window, brush );
    DeleteObject( brush );
    {
        /* beside the tabs the strip's colour, beside the view the view's: no line shows */
        RECT top = { c->top ? 0 : 0, 0, 0, 0 }, side;
        HBRUSH strip = CreateSolidBrush( !c->backdrop ? col.strip : col.dark ? RGB( 0, 0, 0 ) : RGB( 255, 255, 255 ) );
        HBRUSH view = CreateSolidBrush( GetSysColor( COLOR_WINDOW ) );
        int rows = 0, status = 0;

        if (GetClientRect( c->top, &top )) rows = top.bottom;
        if (c->bottom && GetClientRect( c->bottom, &top )) status = top.bottom;
        SetRect( &side, window.left, 0, client.left, client.top + TAB_H );
        FillRect( hdc, &side, strip );
        SetRect( &side, client.right, 0, window.right, client.top + TAB_H );
        FillRect( hdc, &side, strip );
        SetRect( &side, window.left, client.top + rows, client.left, client.bottom - status );
        FillRect( hdc, &side, view );
        SetRect( &side, client.right, client.top + rows, window.right, client.bottom - status );
        FillRect( hdc, &side, view );
        DeleteObject( strip );
        DeleteObject( view );
    }
    ReleaseDC( c->browser, hdc );
}

/* Windows 11's title bar is the row of the tabs: the window has no caption of
 * its own to draw, only its buttons and the place to drag it by */
static LRESULT CALLBACK browser_subclass( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR data )
{
    struct chrome *c = (struct chrome *)data;

    switch (msg)
    {
    case WM_NCCALCSIZE:
        if (wparam && c->style == EXPLORER_WIN11)
        {
            NCCALCSIZE_PARAMS *params = (NCCALCSIZE_PARAMS *)lparam;
            int top = params->rgrc[0].top;
            LRESULT result = DefSubclassProc( hwnd, msg, wparam, lparam );

            params->rgrc[0].top = top + (IsZoomed( hwnd ) ? GetSystemMetrics( SM_CYSIZEFRAME ) + GetSystemMetrics( SM_CXPADDEDBORDER ) : 0);
            return result;
        }
        break;

    case WM_GETMINMAXINFO:
        /* Without a caption of its own a window is maximized over the whole
         * monitor, as a popup is: the taskbar went under it. Windows 11's
         * File Explorer stops at the work area, its frame off the edges as
         * a captioned window's. */
        if (c->style == EXPLORER_WIN11)
        {
            MINMAXINFO *info = (MINMAXINFO *)lparam;
            MONITORINFO monitor = { sizeof(monitor) };
            LRESULT result = DefSubclassProc( hwnd, msg, wparam, lparam );
            int bx = max( 0, -info->ptMaxPosition.x ), by = max( 0, -info->ptMaxPosition.y );

            if (GetMonitorInfoW( MonitorFromWindow( hwnd, MONITOR_DEFAULTTONEAREST ), &monitor ))
            {
                info->ptMaxPosition.x = monitor.rcWork.left - monitor.rcMonitor.left - bx;
                info->ptMaxPosition.y = monitor.rcWork.top - monitor.rcMonitor.top - by;
                info->ptMaxSize.x = monitor.rcWork.right - monitor.rcWork.left + 2 * bx;
                info->ptMaxSize.y = monitor.rcWork.bottom - monitor.rcWork.top + 2 * by;
            }
            return result;
        }
        break;

    case WM_NCPAINT:
        if (c->style == EXPLORER_WIN11)
        {
            paint_frame( c );
            return 0;
        }
        break;

    case WM_NCACTIVATE:
        /* -1: nothing of the frame is drawn for this */
        if (c->style == EXPLORER_WIN11)
        {
            LRESULT result = DefSubclassProc( hwnd, msg, wparam, (LPARAM)-1 );

            set_caption_backdrop( c );
            paint_frame( c );
            InvalidateRect( c->top, NULL, FALSE );
            return result;
        }
        break;

    case WM_SETTEXT:
    case WM_SETICON:
        /* a new title or icon draws the caption over the tabs: they are drawn again */
        if (c->style == EXPLORER_WIN11)
        {
            LRESULT result = DefSubclassProc( hwnd, msg, wparam, lparam );

            paint_frame( c );
            RedrawWindow( c->top, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW );
            return result;
        }
        break;

    case WM_NCHITTEST:
        if (c->style == EXPLORER_WIN11)
        {
            LRESULT hit = DefSubclassProc( hwnd, msg, wparam, lparam );
            POINT pt = { (short)LOWORD( lparam ), (short)HIWORD( lparam ) };
            RECT rect;

            if (hit != HTCLIENT) return hit;
            GetClientRect( hwnd, &rect );
            ScreenToClient( hwnd, &pt );
            if (!IsZoomed( hwnd ) && pt.y < px( 4 ))
            {
                if (pt.x < px( 8 )) return HTTOPLEFT;
                if (pt.x >= rect.right - px( 8 )) return HTTOPRIGHT;
                return HTTOP;
            }
            if (pt.y < TAB_H && !zone_at( c, pt )) return HTCAPTION;
            return hit;
        }
        break;

    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        if (c->style == EXPLORER_WIN11) set_caption_backdrop( c );
        InvalidateRect( c->top, NULL, FALSE );
        break;

    case WM_SIZE:
        /* maximized or back: the title bar's rows begin elsewhere */
        if (c->style == EXPLORER_WIN11 && client_top( c ) != c->caption_top) set_caption_backdrop( c );
        InvalidateRect( c->top, NULL, FALSE );
        break;

    case WM_ACTIVATE:
        InvalidateRect( c->top, NULL, FALSE );
        break;

    case WM_NCDESTROY:
        KillTimer( c->top, TIMER_POLL );
        if (c->hook) UnhookWindowsHookEx( c->hook );
        RemoveWindowSubclass( hwnd, browser_subclass, id );
        RemovePropW( hwnd, L"ArcticExplorerChrome" );
        RemovePropW( hwnd, L"ArcticExplorerSide" );
        side_free( c );
        for (int i = 0; i < c->tab_count; i++) tab_clear( &c->tabs[i] );
        for (int i = 0; i < c->segment_count; i++) ILFree( c->segment[i] );
        free( c );
        break;
    }
    return DefSubclassProc( hwnd, msg, wparam, lparam );
}

/* the keys of the tabs and of the address, wherever the focus is in the window */
static LRESULT CALLBACK key_hook( int code, WPARAM wparam, LPARAM lparam )
{
    MSG *m = (MSG *)lparam;

    /* the wheel turns what is under the pointer, as in Windows 10: the
     * Control Panel's list, though the focus is in the folder's view */
    if (code == HC_ACTION && wparam == PM_REMOVE && m->message == WM_MOUSEWHEEL)
    {
        POINT pt = { (short)LOWORD( m->lParam ), (short)HIWORD( m->lParam ) };
        HWND under = WindowFromPoint( pt ), root = under ? GetAncestor( under, GA_ROOT ) : NULL;
        struct chrome *c = root ? (struct chrome *)GetPropW( root, L"ArcticExplorerChrome" ) : NULL;

        if (c && c->side && under == c->side && m->hwnd != c->side)
        {
            SendMessageW( c->side, WM_MOUSEWHEEL, m->wParam, m->lParam );
            m->message = WM_NULL;
        }
        return CallNextHookEx( NULL, code, wparam, lparam );
    }
    if (code == HC_ACTION && wparam == PM_REMOVE && m->message == WM_KEYDOWN)
    {
        HWND root = GetAncestor( m->hwnd, GA_ROOT );
        struct chrome *c = root ? (struct chrome *)GetPropW( root, L"ArcticExplorerChrome" ) : NULL;
        BOOL ctrl = GetKeyState( VK_CONTROL ) < 0, shift = GetKeyState( VK_SHIFT ) < 0, handled = TRUE;

        if (!c) return CallNextHookEx( NULL, code, wparam, lparam );
        if (ctrl && m->wParam == 'L')
        {
            for (int i = 0; i < c->zone_count; i++)
                if (c->zones[i].kind == Z_ADDRESS) { RECT r = c->zones[i].rect; edit_start( c, &r ); break; }
        }
        else if (c->style != EXPLORER_WIN11) handled = FALSE;
        else if (ctrl && m->wParam == 'T') new_tab( c, c->tabs[c->active].pidl );
        else if (ctrl && m->wParam == 'W') close_tab( c, c->active );
        else if (ctrl && m->wParam == VK_TAB && c->tab_count > 1)
            switch_tab( c, (c->active + (shift ? c->tab_count - 1 : 1)) % c->tab_count );
        else handled = FALSE;
        if (handled) m->message = WM_NULL;
    }
    return CallNextHookEx( NULL, code, wparam, lparam );
}

/**********************************************************************
 *          What browseui calls
 */

HWND WINAPI ArcticExplorerAttach( HWND browser, int style )
{
    static BOOL registered;
    struct chrome *c;

    if (!registered)
    {
        WNDCLASSW cls = { 0 };

        cls.hInstance = startui_instance;
        cls.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_ARROW );
        cls.lpfnWndProc = top_proc;
        cls.lpszClassName = L"ArcticExplorerTop";
        RegisterClassW( &cls );
        cls.lpfnWndProc = bottom_proc;
        cls.lpszClassName = L"ArcticExplorerBottom";
        RegisterClassW( &cls );
        cls.lpfnWndProc = side_proc;
        cls.lpszClassName = L"ArcticExplorerSide";
        RegisterClassW( &cls );
        cls.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_SIZEWE );
        cls.lpfnWndProc = split_proc;
        cls.lpszClassName = L"ArcticExplorerSplit";
        RegisterClassW( &cls );
        registered = TRUE;
    }
    if (!(c = calloc( 1, sizeof(*c) ))) return NULL;
    c->browser = browser;
    c->style = style;
    c->low_hot = -1;
    c->side_hot = c->side_selected = -1;
    c->top = CreateWindowExW( 0, L"ArcticExplorerTop", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, 0, 0, browser, NULL,
                              startui_instance, NULL );
    if (style == EXPLORER_WIN11)
        c->bottom = CreateWindowExW( 0, L"ArcticExplorerBottom", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 0, 0, browser, NULL,
                                     startui_instance, NULL );
    if (!c->top)
    {
        free( c );
        return NULL;
    }
    SetWindowLongPtrW( c->top, GWLP_USERDATA, (LONG_PTR)c );
    if (c->bottom) SetWindowLongPtrW( c->bottom, GWLP_USERDATA, (LONG_PTR)c );
    SetPropW( browser, L"ArcticExplorerChrome", c );
    SetWindowSubclass( browser, browser_subclass, 1, (DWORD_PTR)c );
    SetTimer( c->top, TIMER_POLL, 300, NULL );
    c->hook = SetWindowsHookExW( WH_GETMESSAGE, key_hook, NULL, GetCurrentThreadId() );
    if (style == EXPLORER_WIN11)
    {
        /* no caption at all: what uxtheme would draw of it showed around the tabs */
        SetWindowLongW( browser, GWL_STYLE, GetWindowLongW( browser, GWL_STYLE ) & ~WS_DLGFRAME );
        set_caption_backdrop( c );
        SetWindowPos( browser, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE );
    }
    return c->top;
}

/* the client area is this large: where the rows go, and what they take of
 * it; in the Control Panel its list at the left, where the tree was */
void WINAPI ArcticExplorerLayout( HWND top, int width, int height, int *cy_top, int *cy_bottom, int *cx_left )
{
    struct chrome *c = (struct chrome *)GetWindowLongPtrW( top, GWLP_USERDATA );
    BOOL shown;
    int high;

    *cy_top = *cy_bottom = *cx_left = 0;
    if (!c) return;
    high = c->style == EXPLORER_WIN11 ? TAB_H + NAV_H + CMD_H + 1 : RIBTAB_H + RIBBON_H + ADDR_H;
    /* over the side panel, whose own margin at the top goes under it */
    SetWindowPos( c->top, HWND_TOP, 0, 0, width, high, SWP_NOACTIVATE );
    *cy_top = high;
    if (c->bottom)
    {
        SetWindowPos( c->bottom, NULL, 0, height - STATUS_H, width, STATUS_H, SWP_NOZORDER | SWP_NOACTIVATE );
        *cy_bottom = STATUS_H;
    }

    shown = c->in_controls && nav_pane_wanted() && c->side_count;
    if (shown && !c->side)
    {
        c->side = CreateWindowExW( 0, L"ArcticExplorerSide", L"", WS_CHILD | WS_CLIPSIBLINGS | WS_VSCROLL, 0, 0, 0, 0,
                                   c->browser, NULL, startui_instance, NULL );
        c->split = CreateWindowExW( 0, L"ArcticExplorerSplit", L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0,
                                    c->browser, NULL, startui_instance, NULL );
        if (c->side) SetWindowLongPtrW( c->side, GWLP_USERDATA, (LONG_PTR)c );
        if (c->split) SetWindowLongPtrW( c->split, GWLP_USERDATA, (LONG_PTR)c );
        if (!c->side) shown = FALSE;
    }
    if (shown)
    {
        int cx = side_width_fit( c->side_width ? c->side_width : side_width_saved(), width ), cy = max( 0, height - high - *cy_bottom );

        SetWindowPos( c->side, HWND_TOP, 0, high, cx, cy, SWP_SHOWWINDOW | SWP_NOACTIVATE );
        *cx_left = cx;
        if (c->split)
        {
            SetWindowPos( c->split, HWND_TOP, cx, high, SPLIT_W, cy, SWP_SHOWWINDOW | SWP_NOACTIVATE );
            *cx_left = cx + SPLIT_W;
        }
    }
    else
    {
        if (c->side) ShowWindow( c->side, SW_HIDE );
        if (c->split) ShowWindow( c->split, SW_HIDE );
    }
    if (shown != c->side_shown)
    {
        /* the Control Panel's pages put their task links on the right then */
        c->side_shown = shown;
        if (shown) SetPropW( c->browser, L"ArcticExplorerSide", (HANDLE)1 );
        else RemovePropW( c->browser, L"ArcticExplorerSide" );
    }
}

/* the window went to a folder: the tab's history, its name and the address */
void WINAPI ArcticExplorerNavigated( HWND top, LPCITEMIDLIST pidl )
{
    struct chrome *c = (struct chrome *)GetWindowLongPtrW( top, GWLP_USERDATA );
    struct tab *t;

    if (!c || !pidl) return;
    if (!c->tab_count)
    {
        c->tab_count = 1;
        c->active = 0;
    }
    t = &c->tabs[c->active];
    if (!c->from_history && t->pidl && !ILIsEqual( t->pidl, pidl ))
    {
        push( t->back, &t->back_count, t->pidl );
        t->pidl = NULL;
        while (t->forward_count) ILFree( t->forward[--t->forward_count] );
    }
    c->from_history = FALSE;
    /* away from the results, the search box is empty again */
    if (!c->search && !is_find_folder( pidl )) c->query[0] = 0;
    ILFree( t->pidl );
    t->pidl = ILClone( pidl );
    tab_describe( t, pidl );
    build_segments( c, pidl );
    side_navigated( c, pidl );
    redraw( c );
}
