/*
 * The Start menu of Windows 10
 *
 * As Windows 10 (21H2) lays it out at 100%, against the taskbar, square:
 *
 * - on the left a rail 48 wide: the menu button at the top, which widens
 *   the rail to 256 with the names of its buttons; at the bottom the user,
 *   the folders chosen (Documents, Pictures, Settings by default) and the
 *   power button, a 16 glyph in each 48 square;
 * - the list of programs, 272 wide: "Нещодавно додані", "Часто
 *   використовувані", then every program and folder by name under its
 *   letter, 36 a row with its 24 icon; a letter opens the grid of letters
 *   to jump to; typing searches, the results in its place;
 * - the tiles: the pinned programs in the accent colour, with the icon and
 *   the name, small (48), medium (100), wide (204 x 100) or large (204),
 *   4 apart on a grid of 52, six of it across, as Windows 10 packs them;
 *   their context menu (popup.c) resizes them.
 *
 * It rises far from the taskbar and settles slowly, its list and its tiles
 * a little after it, row by row (ARCTIC_TRANSITION_START10 and the intro
 * here); the rail opens and closes over 200 ms.
 *
 * What it shows is set in the taskbar's properties (settings.c), the look
 * as for the taskbar or its own (taskbar.c).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdlib.h>
#include <stdio.h>

#include "startui.h"
#include "shellapi.h"
#include "shlobj.h"
#include "dwmapi.h"
#include "wine/arctic_dwm.h"

#define RAIL            px(48)
#define RAIL_OPEN       px(256)
#define LIST_WIDTH      px(272)
#define TILE_UNIT       px(52)     /* a small tile and the gap after it */
#define TILE_GAP        px(4)
#define GROUP_UNITS     6          /* across a group: three medium tiles */
#define MAX_UNIT_ROWS   256
#define TILES_LEFT_PAD  px(12)
#define TILES_RIGHT_PAD px(28)
#define GROUP_HEADER    px(40)
#define MENU_HEIGHT     px(640)
#define ROW             px(36)
#define ICON_BOX        px(32)
#define SMALL_ICON      px(24)
#define TILE_ICON       px(32)
#define GLYPH           px(16)
#define CHEVRON         px(10)
#define SEARCH_HEIGHT   px(32)
#define LETTER_CELL     px(56)
#define MAX_TARGETS     160
#define MAX_PINS        64
#define MAX_SEARCH      127

#define HOVER_ALPHA     0x1a
#define PRESSED_ALPHA   0x33

#define TIMER_CARET     1
#define TIMER_ANIM      2

#define RAIL_MS         200
#define INTRO_MS        420

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

enum mode { MODE_HOME, MODE_LETTERS, MODE_SEARCH };

enum kind { K_NONE, K_MENU, K_USER, K_FOLDER, K_POWER, K_ROW, K_LETTER, K_TILE, K_SEARCH };

struct target
{
    enum kind kind;
    int index;
    RECT rect;
};

enum row_kind { ROW_APP, ROW_LETTER, ROW_HEADER };

struct row
{
    enum row_kind kind;
    struct item *item;
    WCHAR letter;
    UINT title;          /* of a header */
};

/* the rail's folders top to bottom, as Windows 10 stacks them */
static const int folder_order[] =
{
    FOLDER_DOCUMENTS, FOLDER_DOWNLOADS, FOLDER_MUSIC, FOLDER_PICTURES, FOLDER_VIDEOS, FOLDER_NETWORK, FOLDER_USER,
    FOLDER_EXPLORER, FOLDER_SETTINGS,
};

static HWND menu, tray;
static RECT button;
static UINT closed_message;
static DWORD hidden_at;
static enum mode mode;
static struct palette pal;
static struct modern_settings settings;
static HFONT font_item, font_title, font_small, font_letter, font_caption;
static WCHAR user_name[128], search[MAX_SEARCH + 1];
static int width, height;
static BOOL rail_open;                /* where the rail is going */
static int rail_width, rail_from;     /* now, and when it started to move */
static DWORD rail_start, intro_start;
static struct look look;              /* the menu's, which its context menus take */

enum tile_size { TILE_SMALL, TILE_MEDIUM, TILE_WIDE, TILE_LARGE, TILE_SIZES };

static const struct { int across, down; } tile_units[TILE_SIZES] = { { 1, 1 }, { 2, 2 }, { 4, 2 }, { 4, 4 } };

static struct item *pins[MAX_PINS];
static BYTE pin_size[MAX_PINS];
static int pin_col[MAX_PINS], pin_row[MAX_PINS];   /* on the grid of units */
static int pin_count, tile_scroll, unit_rows;
static struct row *rows;
static int row_count, row_capacity, scroll;
static WCHAR letters[64];
static int letter_count;

static struct target targets[MAX_TARGETS];
static int target_count;
static struct target hot, pressed, focus;
static BOOL focus_shown, caret_on, in_popup;

/**********************************************************************
 *          What it shows
 */

static BOOL add_row( enum row_kind kind, struct item *item, WCHAR letter, UINT title )
{
    if (row_count == row_capacity)
    {
        struct row *grown = realloc( rows, (row_capacity = max( 64, row_capacity * 2 )) * sizeof(*rows) );
        if (!grown) return FALSE;
        rows = grown;
    }
    rows[row_count].kind = kind;
    rows[row_count].item = item;
    rows[row_count].letter = letter;
    rows[row_count].title = title;
    row_count++;
    return TRUE;
}

static WCHAR name_letter( const WCHAR *name )
{
    WCHAR c = name[0];

    if (!IsCharAlphaW( c )) return '#';
    return (WCHAR)(ULONG_PTR)CharUpperW( (WCHAR *)(ULONG_PTR)c );
}

static void load_rows(void)
{
    struct item **list, *copy[16];
    UINT count;
    WCHAR last = 0;

    row_count = letter_count = 0;
    if (settings.recently_added && (count = items_recently_added( &list, 3 )))
    {
        add_row( ROW_HEADER, NULL, 0, IDS_RECENTLY_ADDED );
        memcpy( copy, list, count * sizeof(*copy) );
        for (UINT i = 0; i < count; i++) add_row( ROW_APP, copy[i], 0, 0 );
    }
    if (settings.most_used && (count = items_frequent( &list, 6 )))
    {
        add_row( ROW_HEADER, NULL, 0, IDS_MOST_USED );
        count = min( count, ARRAY_SIZE(copy) );
        memcpy( copy, list, count * sizeof(*copy) );
        for (UINT i = 0; i < count; i++) add_row( ROW_APP, copy[i], 0, 0 );
    }
    count = items_apps( &list );
    for (UINT i = 0; i < count; i++)
    {
        if (!list[i]->depth)
        {
            WCHAR letter = name_letter( list[i]->name );
            if (letter != last)
            {
                add_row( ROW_LETTER, NULL, letter, 0 );
                if (letter_count < ARRAY_SIZE(letters)) letters[letter_count++] = letter;
                last = letter;
            }
        }
        add_row( ROW_APP, list[i], 0, 0 );
    }
}

static void load_search(void)
{
    struct item **list;
    UINT count = items_search( search, &list );

    row_count = 0;
    for (UINT i = 0; i < count; i++)
        add_row( list[i]->kind == ITEM_HEADER ? ROW_HEADER : ROW_APP, list[i], 0, 0 );
}

#define TILES_KEY L"Software\\Arctic\\StartMenu\\Windows10\\Tiles"

/* each tile's size, by the key of its program */
static BYTE tile_size_of( const struct item *item )
{
    DWORD value = TILE_MEDIUM, size = sizeof(value);

    if (RegGetValueW( HKEY_CURRENT_USER, TILES_KEY, item->key, RRF_RT_REG_DWORD, NULL, &value, &size ) ||
        value >= TILE_SIZES)
        return TILE_MEDIUM;
    return value;
}

static void set_tile_size( const struct item *item, BYTE size )
{
    DWORD value = size;
    RegSetKeyValueW( HKEY_CURRENT_USER, TILES_KEY, item->key, REG_DWORD, &value, sizeof(value) );
}

/* Each tile takes the first place it fits, row by row, on the grid of
 * small tiles; the bigger ones only where an even column and row start */
static void pack_tiles(void)
{
    static BYTE used[MAX_UNIT_ROWS][GROUP_UNITS];

    memset( used, 0, sizeof(used) );
    unit_rows = 0;
    for (int i = 0; i < pin_count; i++)
    {
        int across = tile_units[pin_size[i]].across, down = tile_units[pin_size[i]].down, step = across > 1 ? 2 : 1;
        BOOL placed = FALSE;

        for (int r = 0; r + down <= MAX_UNIT_ROWS && !placed; r += step)
        {
            for (int c = 0; c + across <= GROUP_UNITS && !placed; c += step)
            {
                BOOL free_here = TRUE;

                for (int y = r; y < r + down && free_here; y++)
                    for (int x = c; x < c + across; x++) if (used[y][x]) free_here = FALSE;
                if (!free_here) continue;
                for (int y = r; y < r + down; y++)
                    for (int x = c; x < c + across; x++) used[y][x] = 1;
                pin_col[i] = c;
                pin_row[i] = r;
                unit_rows = max( unit_rows, r + down );
                placed = TRUE;
            }
        }
        if (!placed) pin_col[i] = pin_row[i] = -1;
    }
}

static void load_pins(void)
{
    struct item **list;
    UINT count = items_pinned( &list );

    pin_count = min( count, MAX_PINS );
    memcpy( pins, list, pin_count * sizeof(*pins) );
    for (int i = 0; i < pin_count; i++) pin_size[i] = tile_size_of( pins[i] );
    pack_tiles();
}

/**********************************************************************
 *          Layout
 */

static int list_left(void)
{
    return RAIL;
}

static int tiles_left(void)
{
    return RAIL + (settings.app_list ? LIST_WIDTH : 0) + TILES_LEFT_PAD;
}

static int menu_width(void)
{
    return tiles_left() + GROUP_UNITS * TILE_UNIT - TILE_GAP + TILES_RIGHT_PAD;
}

/* where the rows go: the list's column, or over the tiles while searching without it */
static RECT list_area(void)
{
    RECT rect;

    if (settings.app_list) SetRect( &rect, list_left(), px( 4 ), list_left() + LIST_WIDTH, height - px( 4 ) );
    else SetRect( &rect, RAIL, px( 4 ), width, height - px( 4 ) );
    if (mode == MODE_SEARCH) rect.top += SEARCH_HEIGHT + px( 16 );
    return rect;
}

static RECT search_rect(void)
{
    RECT area = list_area(), rect;
    SetRect( &rect, area.left + px( 12 ), px( 12 ), area.right - px( 12 ), px( 12 ) + SEARCH_HEIGHT );
    return rect;
}

static BOOL rows_shown(void)
{
    return settings.app_list || mode == MODE_SEARCH;
}

static int visible_rows( const RECT *area )
{
    return max( 0, (area->bottom - area->top) / ROW );
}

static int tiles_top(void)
{
    return px( 8 ) + GROUP_HEADER;
}

static int max_tile_scroll(void)
{
    int need = unit_rows * TILE_UNIT, room = height - tiles_top() - px( 8 );
    return max( 0, need - room );
}

static void add_target( enum kind kind, int index, int left, int top, int right, int bottom )
{
    if (target_count == MAX_TARGETS) return;
    targets[target_count].kind = kind;
    targets[target_count].index = index;
    SetRect( &targets[target_count].rect, left, top, right, bottom );
    target_count++;
}

static void layout(void)
{
    int rail = rail_width, y;

    target_count = 0;

    /* the rail first: when open it lies over the list */
    add_target( K_MENU, 0, 0, 0, rail, RAIL );
    y = height - RAIL;
    add_target( K_POWER, 0, 0, y, rail, y + RAIL );
    for (int i = ARRAY_SIZE(folder_order) - 1; i >= 0; i--)
    {
        if (!(settings.folders & (1 << folder_order[i]))) continue;
        y -= RAIL;
        add_target( K_FOLDER, folder_order[i], 0, y, rail, y + RAIL );
    }
    y -= RAIL;
    add_target( K_USER, 0, 0, y, rail, y + RAIL );

    if (mode == MODE_SEARCH) add_target( K_SEARCH, 0, search_rect().left, search_rect().top, search_rect().right,
                                         search_rect().bottom );
    if (mode == MODE_LETTERS)
    {
        RECT area = list_area();
        int columns = max( 1, (area.right - area.left - px( 16 )) / LETTER_CELL );

        for (int i = 0; i < letter_count; i++)
        {
            int x = area.left + px( 8 ) + (i % columns) * LETTER_CELL, ly = area.top + px( 8 ) + (i / columns) * LETTER_CELL;
            add_target( K_LETTER, i, x + px( 2 ), ly + px( 2 ), x + LETTER_CELL - px( 2 ), ly + LETTER_CELL - px( 2 ) );
        }
    }
    else if (rows_shown())
    {
        RECT area = list_area();
        int ry = area.top;

        for (int i = scroll; i < row_count && ry + ROW <= area.bottom; i++, ry += ROW)
        {
            if (rows[i].kind == ROW_HEADER) continue;
            add_target( rows[i].kind == ROW_LETTER ? K_LETTER : K_ROW, i, area.left, ry, area.right - px( 6 ), ry + ROW );
        }
    }
    if (mode != MODE_SEARCH || settings.app_list)
    {
        for (int i = 0; i < pin_count; i++)
        {
            int x = tiles_left() + pin_col[i] * TILE_UNIT, ty = tiles_top() + pin_row[i] * TILE_UNIT - tile_scroll;
            int w = tile_units[pin_size[i]].across * TILE_UNIT - TILE_GAP, h = tile_units[pin_size[i]].down * TILE_UNIT - TILE_GAP;

            if (pin_col[i] < 0 || ty + h <= tiles_top() - px( 8 ) || ty >= height) continue;
            add_target( K_TILE, i, x, ty, x + w, ty + h );
        }
    }
}

static BOOL in_rail( POINT pt )
{
    return pt.x < rail_width;
}

static struct target hit_test( POINT pt )
{
    struct target none = { K_NONE, 0 };

    for (int i = 0; i < target_count; i++)
    {
        /* the open rail hides what is under it */
        if (rail_width > RAIL && targets[i].kind > K_POWER && in_rail( pt )) break;
        if (PtInRect( &targets[i].rect, pt )) return targets[i];
    }
    return none;
}

static BOOL same( struct target a, struct target b )
{
    return a.kind == b.kind && a.index == b.index;
}

static const struct target *find_target( struct target t )
{
    for (int i = 0; i < target_count; i++) if (same( targets[i], t )) return &targets[i];
    return NULL;
}

/**********************************************************************
 *          Animations
 */

/* Windows 10's motion: most of the way at once, then a slow settle */
static float ease_out( float t )
{
    t = 1 - max( 0.f, min( t, 1.f ) );
    return 1 - t * t * t * t;
}

/* how far below its place something of the intro still is, delayed by ms */
static int intro_offset( int delay, int distance )
{
    DWORD elapsed = GetTickCount() - intro_start;

    if (!intro_start || elapsed >= INTRO_MS + (DWORD)delay) return 0;
    if (elapsed < (DWORD)delay) return distance;
    return (int)((1 - ease_out( (float)(elapsed - delay) / (INTRO_MS - 120) )) * distance);
}

static BOOL animating(void)
{
    return (intro_start && GetTickCount() - intro_start < INTRO_MS + 400) ||
           rail_width != (rail_open ? RAIL_OPEN : RAIL);
}

static void open_rail( BOOL open )
{
    rail_open = open;
    rail_from = rail_width;
    rail_start = GetTickCount();
    SetTimer( menu, TIMER_ANIM, 15, NULL );
}

static void on_anim_timer(void)
{
    int target = rail_open ? RAIL_OPEN : RAIL;

    if (rail_width != target)
    {
        float t = (float)(GetTickCount() - rail_start) / RAIL_MS;
        rail_width = t >= 1 ? target : rail_from + (int)((target - rail_from) * ease_out( t ));
        layout();
    }
    if (!animating())
    {
        KillTimer( menu, TIMER_ANIM );
        intro_start = 0;
    }
    InvalidateRect( menu, NULL, FALSE );
}

/**********************************************************************
 *          Painting
 */

static void highlight( HDC hdc, struct target t, const RECT *rect )
{
    BOOL is_hot = same( t, hot ), is_pressed = is_hot && same( t, pressed );

    if (!is_hot && mode == MODE_SEARCH && !focus_shown && same( t, focus )) is_hot = TRUE;
    if (is_hot || is_pressed) fill_alpha( hdc, rect, pal.ink, is_pressed ? PRESSED_ALPHA : HOVER_ALPHA );
    if (focus_shown && same( t, focus )) frame_rect( hdc, rect, px( 2 ), pal.ink );
}

static void paint_rows( HDC hdc )
{
    RECT area = list_area(), text;
    int y = area.top + intro_offset( 40, px( 48 ) );

    SaveDC( hdc );
    IntersectClipRect( hdc, area.left, area.top, area.right, area.bottom );
    for (int i = scroll; i < row_count && y + ROW <= area.bottom + px( 48 ); i++, y += ROW)
    {
        struct row *row = &rows[i];
        struct target t = { row->kind == ROW_LETTER ? K_LETTER : K_ROW, i };
        const struct target *found = find_target( t );
        int x = area.left + px( 16 );

        if (found)
        {
            RECT lit = found->rect;
            OffsetRect( &lit, 0, y - lit.top );
            highlight( hdc, t, &lit );
        }
        SetRect( &text, x, y, area.right - px( 12 ), y + ROW );
        switch (row->kind)
        {
        case ROW_HEADER:
            paint_text( hdc, row->item ? row->item->name : load_string( row->title ), &text, font_title, pal.text,
                        DT_LEFT );
            break;
        case ROW_LETTER:
        {
            WCHAR letter[2] = { row->letter, 0 };
            paint_text( hdc, letter, &text, font_title, pal.text, DT_LEFT );
            break;
        }
        case ROW_APP:
            if (row->item->kind == ITEM_FOLDER)
            {
                RECT plate;
                SetRect( &plate, x, y + (ROW - ICON_BOX) / 2, x + ICON_BOX, y + (ROW + ICON_BOX) / 2 );
                fill_alpha( hdc, &plate, accent_color(), 255 );
                draw_glyph( hdc, GLYPH_FOLDER, x + (ICON_BOX - GLYPH) / 2, y + (ROW - GLYPH) / 2, GLYPH,
                            RGB( 255, 255, 255 ), 255 );
                make_opaque( hdc, &plate );
                draw_glyph( hdc, row->item->expanded ? GLYPH_CHEVRON_UP : GLYPH_CHEVRON_DOWN,
                            area.right - px( 20 ) - CHEVRON, y + (ROW - CHEVRON) / 2, CHEVRON, pal.ink, 255 );
                text.right -= px( 24 );
            }
            else draw_icon_bitmap( hdc, items_icon_sized( row->item, SMALL_ICON ), x + (ICON_BOX - SMALL_ICON) / 2,
                                   y + (ROW - SMALL_ICON) / 2, SMALL_ICON );
            text.left = x + ICON_BOX + px( 12 );
            paint_text( hdc, row->item->name, &text, font_item, pal.text, DT_LEFT );
            break;
        }
    }
    RestoreDC( hdc, -1 );
    if (mode == MODE_SEARCH && !row_count)
    {
        SetRect( &text, area.left + px( 16 ), area.top, area.right, area.top + ROW );
        paint_text( hdc, load_string( IDS_NO_RESULTS ), &text, font_item, pal.dim, DT_LEFT );
    }
    /* the thin bar, while the pointer is over the list */
    if (row_count > visible_rows( &area ) && visible_rows( &area ) && (hot.kind == K_ROW || hot.kind == K_LETTER))
    {
        int span = area.bottom - area.top, shown = visible_rows( &area );
        int thumb = max( px( 24 ), span * shown / row_count ), max_scroll = max( 1, row_count - shown );
        int top = area.top + (span - thumb) * min( scroll, max_scroll ) / max_scroll;
        RECT bar;

        SetRect( &bar, area.right - px( 4 ), top, area.right - px( 2 ), top + thumb );
        fill_alpha( hdc, &bar, pal.ink, 0x80 );
    }
}

static void paint_search( HDC hdc )
{
    RECT box = search_rect(), text, inner;

    fill_alpha( hdc, &box, RGB( 0x80, 0x80, 0x80 ), 255 );
    inner = box;
    InflateRect( &inner, -px( 2 ), -px( 2 ) );
    fill_alpha( hdc, &inner, RGB( 255, 255, 255 ), 255 );
    draw_glyph( hdc, GLYPH_SEARCH, inner.left + px( 8 ), (inner.top + inner.bottom - GLYPH) / 2, GLYPH,
                RGB( 0x40, 0x40, 0x40 ), 255 );
    SetRect( &text, inner.left + px( 8 ) + GLYPH + px( 8 ), inner.top, inner.right - px( 8 ), inner.bottom );
    paint_text( hdc, search, &text, font_item, RGB( 0, 0, 0 ), DT_LEFT );
    if (caret_on)
    {
        HGDIOBJ old = SelectObject( hdc, font_item );
        SIZE size;
        RECT caret;

        GetTextExtentPoint32W( hdc, search, lstrlenW( search ), &size );
        SelectObject( hdc, old );
        if (text.left + size.cx < text.right)
        {
            SetRect( &caret, text.left + size.cx + 1, inner.top + px( 6 ), text.left + size.cx + 2, inner.bottom - px( 6 ) );
            fill_alpha( hdc, &caret, RGB( 0, 0, 0 ), 255 );
        }
    }
    /* the box is opaque, and so is the text on it */
    make_opaque( hdc, &box );
}

static void paint_letters( HDC hdc )
{
    for (int i = 0; i < target_count; i++)
    {
        WCHAR letter[2];

        if (targets[i].kind != K_LETTER) continue;
        letter[0] = letters[targets[i].index];
        letter[1] = 0;
        highlight( hdc, targets[i], &targets[i].rect );
        paint_text( hdc, letter, &targets[i].rect, font_letter, pal.text, DT_CENTER );
    }
}

/* a tile in the accent colour, lighter under the pointer, its name always white */
static void paint_tiles( HDC hdc )
{
    COLORREF accent = accent_color();
    RECT header, clip;

    SetRect( &header, tiles_left() + px( 4 ), px( 8 ), width - TILES_RIGHT_PAD, px( 8 ) + GROUP_HEADER );
    paint_text( hdc, load_string( IDS_PINNED ), &header, font_title, pal.text, DT_LEFT );
    SetRect( &clip, tiles_left(), tiles_top() - px( 4 ), width, height );
    SaveDC( hdc );
    IntersectClipRect( hdc, clip.left, clip.top, clip.right, clip.bottom );
    for (int i = 0; i < target_count; i++)
    {
        const struct target *t = &targets[i];
        struct item *item;
        RECT rect = t->rect, text;
        BOOL is_hot = same( *t, hot );
        int size, icon;

        if (t->kind != K_TILE) continue;
        item = pins[t->index];
        size = pin_size[t->index];
        icon = size == TILE_SMALL ? px( 24 ) : size == TILE_LARGE ? px( 48 ) : TILE_ICON;
        /* the intro: each row of tiles a little after the one above */
        OffsetRect( &rect, 0, intro_offset( 80 + pin_row[t->index] * 18, px( 64 ) ) );
        if (same( *t, pressed ) && is_hot) InflateRect( &rect, -px( 2 ), -px( 2 ) );
        fill_alpha( hdc, &rect, accent, 255 );
        if (is_hot) fill_alpha( hdc, &rect, RGB( 255, 255, 255 ), 0x26 );
        draw_icon_bitmap( hdc, items_icon_sized( item, icon ), (rect.left + rect.right - icon) / 2,
                          rect.top + (rect.bottom - rect.top - icon) / 2 - (size == TILE_SMALL ? 0 : px( 8 )), icon );
        /* a small tile has no name */
        SetRect( &text, rect.left + px( 8 ), rect.bottom - px( 8 ) - px( 16 ), rect.right - px( 6 ), rect.bottom - px( 8 ) );
        if (size != TILE_SMALL) paint_text( hdc, item->name, &text, font_small, RGB( 255, 255, 255 ), DT_LEFT | DT_BOTTOM );
        if (is_hot) frame_rect( hdc, &rect, 1, RGB( 255, 255, 255 ) );
        if (focus_shown && same( *t, focus )) frame_rect( hdc, &rect, px( 2 ), RGB( 255, 255, 255 ) );
        /* the white name on the accent, not keyed into the backdrop */
        IntersectRect( &rect, &rect, &clip );
        make_opaque( hdc, &rect );
    }
    RestoreDC( hdc, -1 );
}

static void paint_rail( HDC hdc )
{
    int rail = rail_width;

    if (rail > RAIL)
    {
        RECT back;
        int opened = (rail - RAIL) * 255 / (RAIL_OPEN - RAIL);

        SetRect( &back, 0, 0, rail, height );
        /* the open rail is nearly opaque over the list, more so as it opens */
        fill_alpha( hdc, &back, pal.light ? RGB( 0xe4, 0xe4, 0xe4 ) : RGB( 0x2b, 0x2b, 0x2b ), 0xa0 + opened * 0x50 / 255 );
    }
    SaveDC( hdc );
    IntersectClipRect( hdc, 0, 0, rail, height );
    for (int i = 0; i < target_count; i++)
    {
        const struct target *t = &targets[i];
        RECT text;
        const WCHAR *name = NULL;
        int gx = (RAIL - GLYPH) / 2, gy = (t->rect.top + t->rect.bottom - GLYPH) / 2;

        if (t->kind > K_POWER) continue;
        highlight( hdc, *t, &t->rect );
        switch (t->kind)
        {
        case K_MENU:
            draw_glyph( hdc, GLYPH_HAMBURGER, gx, gy, GLYPH, pal.ink, 255 );
            break;
        case K_USER:
            avatar_draw( hdc, (RAIL - px( 24 )) / 2, (t->rect.top + t->rect.bottom - px( 24 )) / 2, px( 24 ), &pal );
            name = user_name;
            break;
        case K_FOLDER:
            draw_glyph( hdc, folder_glyph( t->index ), gx, gy, GLYPH, pal.ink, 255 );
            name = folder_name( t->index );
            break;
        case K_POWER:
            draw_glyph( hdc, GLYPH_POWER, gx, gy, GLYPH, pal.ink, 255 );
            name = load_string( IDS_POWER );
            break;
        default:
            break;
        }
        if (rail <= RAIL) continue;
        SetRect( &text, RAIL, t->rect.top, RAIL_OPEN - px( 12 ), t->rect.bottom );
        if (t->kind == K_MENU)
        {
            WCHAR caption[64];
            lstrcpynW( caption, load_string( IDS_START_CAPTION ), ARRAY_SIZE(caption) );
            CharUpperW( caption );
            paint_text( hdc, caption, &text, font_caption, pal.text, DT_LEFT );
        }
        else if (name) paint_text( hdc, name, &text, font_item, pal.text, DT_LEFT );
    }
    RestoreDC( hdc, -1 );
}

static void on_paint( HWND hwnd )
{
    PAINTSTRUCT ps;
    HBITMAP bitmap;
    HGDIOBJ old;
    HDC hdc = paint_begin( hwnd, &ps, &pal, &bitmap, &old );

    if (!hdc) return;
    if (mode == MODE_LETTERS) paint_letters( hdc );
    else if (rows_shown()) paint_rows( hdc );
    if (mode == MODE_SEARCH) paint_search( hdc );
    if (mode != MODE_SEARCH || settings.app_list) paint_tiles( hdc );
    paint_rail( hdc );
    paint_end( hwnd, &ps, hdc, bitmap, old );
}

/**********************************************************************
 *          Acting
 */

static void refresh(void)
{
    layout();
    InvalidateRect( menu, NULL, FALSE );
}

static void set_mode( enum mode new_mode )
{
    mode = new_mode;
    scroll = 0;
    hot.kind = pressed.kind = K_NONE;
    if (mode == MODE_HOME)
    {
        search[0] = 0;
        load_rows();
    }
    else if (mode == MODE_SEARCH) load_search();
    layout();
}

static void update_search(void)
{
    focus.kind = K_NONE;
    if (!search[0]) set_mode( MODE_HOME );
    else
    {
        set_mode( MODE_SEARCH );
        for (int i = 0; i < target_count; i++)
        {
            if (targets[i].kind != K_ROW) continue;
            focus = targets[i];
            break;
        }
    }
    InvalidateRect( menu, NULL, FALSE );
}

static void activate( struct target t )
{
    const struct target *found = find_target( t );
    POINT pt;

    if (t.kind != K_MENU && rail_open) open_rail( FALSE );
    switch (t.kind)
    {
    case K_MENU:
        open_rail( !rail_open );
        break;
    case K_USER:
        if (!found) break;
        pt.x = found->rect.left;
        pt.y = found->rect.top;
        ClientToScreen( menu, &pt );
        user_popup( menu, pt, TPM_LEFTALIGN | TPM_BOTTOMALIGN, &in_popup, menu10_hide, POPUP_WIN10, &look );
        return;
    case K_FOLDER:
        menu10_hide();
        folder_open( t.index );
        return;
    case K_POWER:
        if (!found) break;
        pt.x = found->rect.left;
        pt.y = found->rect.top;
        ClientToScreen( menu, &pt );
        power_popup( menu, pt, TPM_LEFTALIGN | TPM_BOTTOMALIGN, &in_popup, menu10_hide, POPUP_WIN10, &look );
        return;
    case K_ROW:
    {
        struct item *item = rows[t.index].item;

        if (item->kind == ITEM_FOLDER)
        {
            int keep = scroll;
            item->expanded = !item->expanded;
            load_rows();
            scroll = min( keep, max( 0, row_count - 1 ) );
            break;
        }
        menu10_hide();
        items_launch( item );
        return;
    }
    case K_LETTER:
        if (mode == MODE_LETTERS)
        {
            WCHAR letter = letters[t.index];
            set_mode( MODE_HOME );
            for (int i = 0; i < row_count; i++)
            {
                if (rows[i].kind != ROW_LETTER || rows[i].letter != letter) continue;
                scroll = i;
                break;
            }
        }
        else
        {
            mode = MODE_LETTERS;
            hot.kind = pressed.kind = K_NONE;
        }
        break;
    case K_TILE:
        menu10_hide();
        items_launch( pins[t.index] );
        return;
    default:
        return;
    }
    focus.kind = K_NONE;
    refresh();
}

/* Windows 10's context menu of a tile or a program: unpin or pin, Resize
 * with its four sizes, More with Open file location */
#define CMD_PIN       1
#define CMD_LOCATION  2
#define CMD_SIZE      10   /* + enum tile_size */

static void context_menu( struct target t, POINT pt )
{
    static const UINT size_names[TILE_SIZES] = { IDS_TILE_SMALL, IDS_TILE_MEDIUM, IDS_TILE_WIDE, IDS_TILE_LARGE };
    struct popup_item sizes[TILE_SIZES], more[1], items[3];
    struct item *item = NULL;
    BOOL tile = t.kind == K_TILE, pinned;
    UINT count = 0, cmd;

    if (tile) item = pins[t.index];
    else if (t.kind == K_ROW) item = rows[t.index].item;
    if (!item || item->kind != ITEM_PROGRAM) return;
    pinned = items_is_pinned( item );

    memset( items, 0, sizeof(items) );
    memset( sizes, 0, sizeof(sizes) );
    for (int i = 0; i < TILE_SIZES; i++)
    {
        sizes[i].id = CMD_SIZE + i;
        sizes[i].text = load_string( size_names[i] );
        sizes[i].glyph = GLYPH_NONE;
        sizes[i].checked = tile && pin_size[t.index] == i;
    }
    more[0] = (struct popup_item){ CMD_LOCATION, load_string( IDS_OPEN_LOCATION ), GLYPH_FOLDER };
    more[0].disabled = item->args[0] != 0;
    items[count++] = (struct popup_item){ CMD_PIN, load_string( pinned ? IDS_UNPIN_START10 : IDS_PIN_START10 ),
                                          pinned ? GLYPH_UNPIN : GLYPH_PIN };
    if (tile) items[count++] = (struct popup_item){ 0, load_string( IDS_RESIZE ), GLYPH_RESIZE, FALSE, FALSE, sizes, TILE_SIZES };
    items[count++] = (struct popup_item){ 0, load_string( IDS_MORE_ACTIONS ), GLYPH_MORE, FALSE, FALSE, more, 1 };

    ClientToScreen( menu, &pt );
    in_popup = TRUE;
    cmd = popup_track( menu, pt, TPM_LEFTALIGN | TPM_TOPALIGN, items, count, POPUP_WIN10, &look );
    in_popup = FALSE;
    switch (cmd)
    {
    case CMD_PIN:
        items_pin( item, !pinned );
        break;
    case CMD_LOCATION:
        menu10_hide();
        items_open_location( item );
        return;
    case CMD_SIZE + TILE_SMALL:
    case CMD_SIZE + TILE_MEDIUM:
    case CMD_SIZE + TILE_WIDE:
    case CMD_SIZE + TILE_LARGE:
        set_tile_size( item, cmd - CMD_SIZE );
        break;
    default:
        /* a click elsewhere took the foreground: the menu goes too */
        if (GetForegroundWindow() != menu) menu10_hide();
        return;
    }
    load_pins();
    if (mode == MODE_HOME) load_rows();
    refresh();
}

/**********************************************************************
 *          Input
 */

static void set_hot( struct target t )
{
    if (same( t, hot )) return;
    hot = t;
    InvalidateRect( menu, NULL, FALSE );
}

static void scroll_list( int rows_by )
{
    RECT area = list_area();
    int max_scroll = max( 0, row_count - visible_rows( &area ) );
    int new_scroll = max( 0, min( scroll + rows_by, max_scroll ) );

    if (new_scroll == scroll) return;
    scroll = new_scroll;
    refresh();
}

static void scroll_tiles( int by )
{
    int new_scroll = max( 0, min( tile_scroll + by, max_tile_scroll() ) );

    if (new_scroll == tile_scroll) return;
    tile_scroll = new_scroll;
    refresh();
}

/* the target nearest in a direction, for the arrows */
static void move_focus( int dx, int dy )
{
    const struct target *from = find_target( focus );
    int best = -1, best_score = 0;
    POINT c;

    focus_shown = TRUE;
    if (!from)
    {
        for (int i = 0; i < target_count; i++)
        {
            if (targets[i].kind != K_ROW && targets[i].kind != K_TILE) continue;
            focus = targets[i];
            break;
        }
        InvalidateRect( menu, NULL, FALSE );
        return;
    }
    c.x = (from->rect.left + from->rect.right) / 2;
    c.y = (from->rect.top + from->rect.bottom) / 2;
    for (int i = 0; i < target_count; i++)
    {
        const struct target *t = &targets[i];
        int x = (t->rect.left + t->rect.right) / 2 - c.x, y = (t->rect.top + t->rect.bottom) / 2 - c.y, along, across;

        if (t == from || t->kind == K_SEARCH) continue;
        along = dx ? x * dx : y * dy;
        across = abs( dx ? y : x );
        if (along <= 0) continue;
        if (best < 0 || along + 2 * across < best_score)
        {
            best = i;
            best_score = along + 2 * across;
        }
    }
    if (best >= 0) focus = targets[best];
    else if (dy && from->kind == K_ROW)
    {
        scroll_list( dy );
        return;
    }
    InvalidateRect( menu, NULL, FALSE );
}

static void on_key( WPARAM key )
{
    switch (key)
    {
    case VK_ESCAPE:
        if (rail_open) open_rail( FALSE );
        else if (mode == MODE_HOME) menu10_hide();
        else
        {
            set_mode( MODE_HOME );
            focus.kind = K_NONE;
            InvalidateRect( menu, NULL, FALSE );
        }
        break;
    case VK_UP:    move_focus( 0, -1 ); break;
    case VK_DOWN:  move_focus( 0, 1 ); break;
    case VK_LEFT:  if (mode != MODE_SEARCH) move_focus( -1, 0 ); break;
    case VK_RIGHT: if (mode != MODE_SEARCH) move_focus( 1, 0 ); break;
    case VK_RETURN:
        if (focus.kind != K_NONE && find_target( focus )) activate( focus );
        break;
    case VK_BACK:
        if (search[0])
        {
            search[lstrlenW( search ) - 1] = 0;
            update_search();
        }
        break;
    case VK_PRIOR: scroll_list( -8 ); break;
    case VK_NEXT:  scroll_list( 8 ); break;
    }
}

static void on_char( WCHAR ch )
{
    int len = lstrlenW( search );

    if (ch < 0x20 || ch == 0x7f || len >= MAX_SEARCH) return;
    if (!len && ch == ' ') return;
    search[len] = ch;
    search[len + 1] = 0;
    caret_on = TRUE;
    if (rail_open) open_rail( FALSE );
    update_search();
}

static LRESULT WINAPI menu_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    POINT pt = { (short)LOWORD( lparam ), (short)HIWORD( lparam ) };
    struct target t;

    switch (msg)
    {
    case WM_MOUSEMOVE:
        if (hot.kind == K_NONE)
        {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent( &tme );
        }
        set_hot( hit_test( pt ) );
        return 0;

    case WM_MOUSELEAVE:
        hot.kind = pressed.kind = K_NONE;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;

    case WM_LBUTTONDOWN:
        pressed = hit_test( pt );
        focus_shown = FALSE;
        if (rail_open && !in_rail( pt ))
        {
            /* a click beside the open rail only closes it */
            open_rail( FALSE );
            pressed.kind = K_NONE;
            return 0;
        }
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;

    case WM_LBUTTONUP:
        t = hit_test( pt );
        if (same( t, pressed ) && t.kind != K_NONE && t.kind != K_SEARCH)
        {
            pressed.kind = K_NONE;
            activate( t );
        }
        pressed.kind = K_NONE;
        if (menu10_visible()) InvalidateRect( hwnd, NULL, FALSE );
        return 0;

    case WM_RBUTTONUP:
        context_menu( hit_test( pt ), pt );
        return 0;

    case WM_MOUSEWHEEL:
    {
        int notches = -(short)HIWORD( wparam ) / WHEEL_DELTA;
        POINT client = pt;

        ScreenToClient( hwnd, &client );
        if (client.x >= tiles_left() - TILES_LEFT_PAD && (mode != MODE_SEARCH || settings.app_list))
            scroll_tiles( notches * TILE_UNIT );
        else scroll_list( notches * 3 );
        set_hot( hit_test( client ) );
        return 0;
    }

    case WM_KEYDOWN:
        on_key( wparam );
        return 0;

    case WM_CHAR:
        on_char( (WCHAR)wparam );
        return 0;

    case WM_TIMER:
        if (wparam == TIMER_ANIM)
        {
            on_anim_timer();
            return 0;
        }
        if (wparam == TIMER_CARET && search[0])
        {
            RECT box = search_rect();
            caret_on = !caret_on;
            InvalidateRect( hwnd, &box, FALSE );
        }
        return 0;

    case WM_ACTIVATE:
        if (LOWORD( wparam ) == WA_INACTIVE && !in_popup) menu10_hide();
        return 0;

    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        on_paint( hwnd );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

static BOOL create_menu(void)
{
    DWORD transition = ARCTIC_TRANSITION_START10, corner = 1 /* DWMWCP_DONOTROUND */;
    DWORD size = ARRAY_SIZE(user_name);
    WNDCLASSW cls = { 0 };

    if (menu) return TRUE;
    cls.lpfnWndProc = menu_proc;
    cls.hInstance = startui_instance;
    cls.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_ARROW );
    cls.lpszClassName = L"ArcticStartMenu10";
    RegisterClassW( &cls );
    menu = CreateWindowExW( WS_EX_TOOLWINDOW | WS_EX_TOPMOST, cls.lpszClassName, L"", WS_POPUP,
                            0, 0, px( 668 ), MENU_HEIGHT, NULL, NULL, startui_instance, NULL );
    if (!menu) return FALSE;
    DwmSetWindowAttribute( menu, DWMWA_ARCTIC_TRANSITION, &transition, sizeof(transition) );
    DwmSetWindowAttribute( menu, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner) );
    font_item = shell_font( px( 14 ), FW_NORMAL );
    font_title = shell_font( px( 14 ), FW_SEMIBOLD );
    font_small = shell_font( px( 12 ), FW_NORMAL );
    font_letter = shell_font( px( 22 ), FW_NORMAL );
    font_caption = shell_font( px( 14 ), FW_BOLD );
    closed_message = RegisterWindowMessageW( L"ArcticStartMenuClosed" );
    if (!GetUserNameW( user_name, &size )) lstrcpyW( user_name, L"User" );
    return TRUE;
}

/**********************************************************************
 *          Showing it
 */

BOOL menu10_visible(void)
{
    return menu && IsWindowVisible( menu );
}

void menu10_hide(void)
{
    if (!menu10_visible()) return;
    KillTimer( menu, TIMER_CARET );
    KillTimer( menu, TIMER_ANIM );
    intro_start = 0;
    ShowWindow( menu, SW_HIDE );
    hidden_at = GetTickCount();
    if (tray && closed_message) PostMessageW( tray, closed_message, 0, 0 );
}

/* against the taskbar, over the Start button, inside the work area */
static void place(void)
{
    MONITORINFO info = { sizeof(info) };
    int x, y;

    GetMonitorInfoW( MonitorFromRect( &button, MONITOR_DEFAULTTONEAREST ), &info );
    width = menu_width();
    height = min( MENU_HEIGHT, (int)(info.rcWork.bottom - info.rcWork.top) );
    x = max( (int)info.rcWork.left, min( (int)button.left, (int)info.rcWork.right - width ) );
    if ((button.top + button.bottom) / 2 > (info.rcWork.top + info.rcWork.bottom) / 2) y = info.rcWork.bottom - height;
    else y = info.rcWork.top;
    if (button.right <= info.rcWork.left || button.left >= info.rcWork.right)
        y = max( (int)info.rcWork.top, min( (int)button.top, (int)info.rcWork.bottom - height ) );
    SetWindowPos( menu, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE );
}

void menu10_toggle( HWND tray_window, const RECT *button_rect, BOOL click )
{
    if (!create_menu()) return;
    if (menu10_visible())
    {
        menu10_hide();
        return;
    }
    if (click && GetTickCount() - hidden_at < 300) return;

    tray = tray_window;
    if (button_rect) button = *button_rect;
    else SetRect( &button, 0, GetSystemMetrics( SM_CYSCREEN ) - 1, 1, GetSystemMetrics( SM_CYSCREEN ) );
    items_reload();
    modern_settings_load( STYLE_WIN10, &settings );
    search[0] = 0;
    rail_open = FALSE;
    rail_width = RAIL;
    tile_scroll = 0;
    focus.kind = hot.kind = pressed.kind = K_NONE;
    focus_shown = FALSE;
    load_pins();
    place();
    set_mode( MODE_HOME );
    start_look_load( STYLE_WIN10, &look );
    palette_init( &pal, look_apply( menu, &look ) );
    InvalidateRect( menu, NULL, FALSE );
    ShowWindow( menu, SW_SHOW );
    SetForegroundWindow( menu );
    SetFocus( menu );
    SetTimer( menu, TIMER_CARET, 530, NULL );
    /* the list and the tiles rise after the menu */
    intro_start = GetTickCount();
    SetTimer( menu, TIMER_ANIM, 15, NULL );
}
