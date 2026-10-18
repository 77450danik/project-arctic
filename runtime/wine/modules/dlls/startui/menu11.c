/*
 * The Start menu of Windows 11
 *
 * As Windows 11 (22H2) lays it out, 642 wide at 100%, its corners rounded
 * by 8, 12 over the taskbar:
 *
 * - at the top the search box, 32 high, round at its ends; typing anywhere
 *   searches, and the results take the place of what is under it;
 * - "Закріплені" with "Усі програми >" on its right, and the pinned
 *   programs 6 to a row, a cell 96 x 84, their 32 icons over their names;
 *   three rows to a page, the wheel turns the pages, dots on the right
 *   show which;
 * - "Рекомендовані" with "Більше >": the programs added lately and the
 *   files opened lately, two to a row, 56 high, how long ago under each;
 * - at the bottom, on a darker band 64 high, the user on the left, the
 *   folders chosen and the power button on the right.
 *
 * All apps is one list by name under letters; a letter opens the grid of
 * letters to jump to. The layout (more pins, more recommendations), what
 * Recommended shows and the folders are set in the taskbar's properties
 * (settings.c), and the look as for the taskbar or its own (taskbar.c).
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

#define WIDTH           px(642)
#define MARGIN          px(12)     /* from the taskbar and the screen's edge */
#define SIDE            px(32)
#define SEARCH_TOP      px(32)
#define SEARCH_HEIGHT   px(32)
#define HEADER_HEIGHT   px(32)
#define CELL_WIDTH      px(96)
#define CELL_HEIGHT     px(84)
#define COLUMNS         6
#define REC_HEIGHT      px(56)
#define ROW_HEIGHT      px(40)
#define LETTER_HEIGHT   px(36)
#define FOOTER_HEIGHT   px(64)
#define ICON            px(32)
#define SMALL_ICON      px(24)
#define BUTTON_HEIGHT   px(24)
#define ROUND_BUTTON    px(40)
#define GLYPH           px(16)
#define CHEVRON         px(10)
#define INSET           px(56)     /* where the titles and the lists start */
#define MAX_PINS        64
#define MAX_RECOMMENDED 20
#define MAX_TARGETS     160
#define MAX_SEARCH      127
#define LETTER_CELL     px(64)

#define HOVER_ALPHA     0x14
#define PRESSED_ALPHA   0x0c

#define TIMER_CARET     1

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

enum mode { MODE_HOME, MODE_ALL, MODE_LETTERS, MODE_SEARCH, MODE_RECOMMENDED };

enum kind
{
    K_NONE, K_SEARCH, K_ALL_BUTTON, K_MORE_BUTTON, K_BACK_BUTTON, K_PIN, K_REC, K_ROW, K_LETTER, K_USER, K_FOLDER,
    K_POWER,
};

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
};

struct recommended
{
    struct item *item;           /* a program added lately, or */
    struct recent_file file;     /* a file opened lately */
    HBITMAP icon;                /* of the file */
    WCHAR detail[64];
};

static HWND menu, tray;
static RECT button;
static UINT closed_message;
static DWORD hidden_at;
static enum mode mode;
static struct palette pal;
static struct look look;      /* the menu's, which its context menus take */
static struct modern_settings settings;
static HFONT font_title, font_item, font_small, font_letter;
static WCHAR user_name[128], search[MAX_SEARCH + 1];
static int height;

static struct item *pins[MAX_PINS];
static int pin_count, page;
static struct recommended recs[MAX_RECOMMENDED];
static int rec_count;
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

static int pin_rows(void)
{
    return settings.layout == LAYOUT_MORE_PINS ? 4 : settings.layout == LAYOUT_MORE_RECOMMENDED ? 2 : 3;
}

static int rec_rows(void)
{
    return settings.layout == LAYOUT_MORE_PINS ? 1 : settings.layout == LAYOUT_MORE_RECOMMENDED ? 4 : 3;
}

static int pages(void)
{
    return max( 1, (pin_count + COLUMNS * pin_rows() - 1) / (COLUMNS * pin_rows()) );
}

static void free_recommended(void)
{
    for (int i = 0; i < rec_count; i++) if (recs[i].icon) DeleteObject( recs[i].icon );
    rec_count = 0;
}

static void load_recommended(void)
{
    struct recent_file files[MAX_RECOMMENDED];
    struct item **list;
    UINT count;

    free_recommended();
    if (settings.recently_added)
    {
        count = items_recently_added( &list, 3 );
        for (UINT i = 0; i < count && rec_count < MAX_RECOMMENDED; i++)
        {
            memset( &recs[rec_count], 0, sizeof(recs[rec_count]) );
            recs[rec_count].item = list[i];
            lstrcpynW( recs[rec_count].detail, load_string( IDS_RECENTLY_ADDED ), ARRAY_SIZE(recs[0].detail) );
            rec_count++;
        }
    }
    if (settings.recent_items)
    {
        count = items_recent_files( files, MAX_RECOMMENDED - rec_count );
        for (UINT i = 0; i < count; i++)
        {
            memset( &recs[rec_count], 0, sizeof(recs[rec_count]) );
            recs[rec_count].file = files[i];
            describe_age( &files[i].time, recs[rec_count].detail, ARRAY_SIZE(recs[0].detail) );
            rec_count++;
        }
    }
}

static BOOL add_row( enum row_kind kind, struct item *item, WCHAR letter )
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
    row_count++;
    return TRUE;
}

/* the letter a name goes under: its first, in capitals, or # */
static WCHAR name_letter( const WCHAR *name )
{
    WCHAR c = name[0];

    if (!IsCharAlphaW( c )) return '#';
    return (WCHAR)(ULONG_PTR)CharUpperW( (WCHAR *)(ULONG_PTR)c );
}

static void load_apps(void)
{
    struct item **list;
    UINT count = items_apps( &list );
    WCHAR last = 0;

    row_count = letter_count = 0;
    for (UINT i = 0; i < count; i++)
    {
        if (!list[i]->depth)
        {
            WCHAR letter = name_letter( list[i]->name );
            if (letter != last)
            {
                add_row( ROW_LETTER, NULL, letter );
                if (letter_count < ARRAY_SIZE(letters)) letters[letter_count++] = letter;
                last = letter;
            }
        }
        add_row( ROW_APP, list[i], 0 );
    }
}

static void load_search(void)
{
    struct item **list;
    UINT count = items_search( search, &list );

    row_count = 0;
    for (UINT i = 0; i < count; i++) add_row( list[i]->kind == ITEM_HEADER ? ROW_HEADER : ROW_APP, list[i], 0 );
}

static void load_pins(void)
{
    struct item **list;
    UINT count = items_pinned( &list );

    pin_count = min( count, MAX_PINS );
    memcpy( pins, list, pin_count * sizeof(*pins) );
    page = min( page, pages() - 1 );
}

/**********************************************************************
 *          Layout
 */

static int content_top(void)
{
    return SEARCH_TOP + SEARCH_HEIGHT + px( 24 );
}

static int footer_top(void)
{
    return height - FOOTER_HEIGHT;
}

static int pins_top(void)
{
    return content_top() + HEADER_HEIGHT + px( 8 );
}

static int rec_header_top(void)
{
    return pins_top() + pin_rows() * CELL_HEIGHT + px( 16 );
}

static int menu_height(void)
{
    return pins_top() + pin_rows() * CELL_HEIGHT + px( 16 ) + HEADER_HEIGHT + px( 8 ) + rec_rows() * REC_HEIGHT +
           px( 24 ) + FOOTER_HEIGHT;
}

/* the list of All apps, the results, More: from under its header to the footer */
static RECT list_area( BOOL with_header )
{
    RECT rect;
    SetRect( &rect, SIDE, content_top() + (with_header ? HEADER_HEIGHT + px( 8 ) : 0), WIDTH - SIDE, footer_top() - px( 8 ) );
    return rect;
}

static int row_height( const struct row *row )
{
    return row->kind == ROW_APP ? ROW_HEIGHT : LETTER_HEIGHT;
}

static int visible_rows( const RECT *area )
{
    int y = area->top, n = 0;

    for (int i = scroll; i < row_count && y + row_height( &rows[i] ) <= area->bottom; i++, n++)
        y += row_height( &rows[i] );
    return n;
}

static int text_width( HFONT font, const WCHAR *text )
{
    HDC hdc = GetDC( menu );
    HGDIOBJ old = SelectObject( hdc, font );
    SIZE size = { 0 };

    GetTextExtentPoint32W( hdc, text, lstrlenW( text ), &size );
    SelectObject( hdc, old );
    ReleaseDC( menu, hdc );
    return size.cx;
}

static void add_target( enum kind kind, int index, int left, int top, int right, int bottom )
{
    if (target_count == MAX_TARGETS) return;
    targets[target_count].kind = kind;
    targets[target_count].index = index;
    SetRect( &targets[target_count].rect, left, top, right, bottom );
    target_count++;
}

/* a button of a header, at its right: "Усі програми >", "Більше >", "< Назад" */
static void add_header_button( enum kind kind, int top )
{
    UINT id = kind == K_BACK_BUTTON ? IDS_BACK : kind == K_ALL_BUTTON ? IDS_ALL_PROGRAMS : IDS_MORE;
    int width = text_width( font_small, load_string( id ) ) + CHEVRON + px( 8 + 8 + 6 );
    int y = top + (HEADER_HEIGHT - BUTTON_HEIGHT) / 2;

    add_target( kind, 0, WIDTH - INSET - width + px( 8 ), y, WIDTH - INSET + px( 8 ), y + BUTTON_HEIGHT );
}

static void layout(void)
{
    int top = content_top();

    target_count = 0;
    add_target( K_SEARCH, 0, SIDE, SEARCH_TOP, WIDTH - SIDE, SEARCH_TOP + SEARCH_HEIGHT );

    switch (mode)
    {
    case MODE_HOME:
    {
        int first = page * COLUMNS * pin_rows(), y;

        add_header_button( K_ALL_BUTTON, top );
        for (int i = first; i < pin_count && i < first + COLUMNS * pin_rows(); i++)
        {
            int cell = i - first, x = SIDE + 1 + (cell % COLUMNS) * CELL_WIDTH;
            y = pins_top() + (cell / COLUMNS) * CELL_HEIGHT;
            add_target( K_PIN, i, x, y, x + CELL_WIDTH, y + CELL_HEIGHT );
        }
        y = rec_header_top();
        add_header_button( K_MORE_BUTTON, y );
        y += HEADER_HEIGHT + px( 8 );
        for (int i = 0; i < rec_count && i < 2 * rec_rows(); i++)
        {
            int x = SIDE + (i % 2) * (WIDTH - 2 * SIDE) / 2;
            int ry = y + (i / 2) * REC_HEIGHT;
            add_target( K_REC, i, x, ry, x + (WIDTH - 2 * SIDE) / 2, ry + REC_HEIGHT );
        }
        break;
    }
    case MODE_RECOMMENDED:
    {
        RECT area = list_area( TRUE );

        add_header_button( K_BACK_BUTTON, top );
        for (int i = 0; i < rec_count; i++)
        {
            int x = SIDE + (i % 2) * (WIDTH - 2 * SIDE) / 2, ry = area.top + (i / 2) * REC_HEIGHT;
            if (ry + REC_HEIGHT > area.bottom) break;
            add_target( K_REC, i, x, ry, x + (WIDTH - 2 * SIDE) / 2, ry + REC_HEIGHT );
        }
        break;
    }
    case MODE_ALL:
    case MODE_SEARCH:
    {
        RECT area = list_area( mode == MODE_ALL );
        int y = area.top;

        if (mode == MODE_ALL) add_header_button( K_BACK_BUTTON, top );
        for (int i = scroll; i < row_count; i++)
        {
            int h = row_height( &rows[i] );
            if (y + h > area.bottom) break;
            if (rows[i].kind != ROW_HEADER)
                add_target( rows[i].kind == ROW_LETTER ? K_LETTER : K_ROW, i, area.left + px( 8 ), y,
                            area.right - px( 8 ), y + h );
            y += h;
        }
        break;
    }
    case MODE_LETTERS:
    {
        RECT area = list_area( TRUE );
        int columns = (area.right - area.left) / LETTER_CELL;
        int x0 = area.left + ((area.right - area.left) - columns * LETTER_CELL) / 2;

        add_header_button( K_BACK_BUTTON, top );
        for (int i = 0; i < letter_count; i++)
        {
            int x = x0 + (i % columns) * LETTER_CELL, y = area.top + (i / columns) * LETTER_CELL;
            add_target( K_LETTER, i, x + px( 4 ), y + px( 4 ), x + LETTER_CELL - px( 4 ), y + LETTER_CELL - px( 4 ) );
        }
        break;
    }
    }

    /* the footer: the user, then from the right the power button and the folders */
    {
        int y = footer_top() + (FOOTER_HEIGHT - ROUND_BUTTON) / 2, x = WIDTH - INSET + px( 8 ) - ROUND_BUTTON;
        int name = min( text_width( font_small, user_name ), px( 220 ) );

        add_target( K_USER, 0, INSET - px( 12 ), y, INSET + ICON + px( 12 ) + name + px( 12 ), y + ROUND_BUTTON );
        add_target( K_POWER, 0, x, y, x + ROUND_BUTTON, y + ROUND_BUTTON );
        for (int i = FOLDER_COUNT - 1; i >= 0; i--)
        {
            if (!(settings.folders & (1 << i))) continue;
            x -= ROUND_BUTTON + px( 4 );
            add_target( K_FOLDER, i, x, y, x + ROUND_BUTTON, y + ROUND_BUTTON );
        }
    }
}

static struct target hit_test( POINT pt )
{
    struct target none = { K_NONE, 0 };

    for (int i = 0; i < target_count; i++) if (PtInRect( &targets[i].rect, pt )) return targets[i];
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
 *          Painting
 */

static void highlight( HDC hdc, struct target t, const RECT *rect, float radius )
{
    BOOL is_hot = same( t, hot ), is_pressed = is_hot && same( t, pressed );

    /* the result Enter starts is lit */
    if (!is_hot && mode == MODE_SEARCH && !focus_shown && same( t, focus )) is_hot = TRUE;
    if (is_hot || is_pressed) fill_round_rect( hdc, rect, radius, pal.ink, is_pressed ? PRESSED_ALPHA : HOVER_ALPHA );
    if (focus_shown && same( t, focus )) frame_rect( hdc, rect, px( 2 ), pal.ink );
}

static void paint_header( HDC hdc, int top, UINT title, enum kind button_kind )
{
    RECT text;
    const struct target *b;
    struct target t = { button_kind, 0 };

    SetRect( &text, INSET, top, WIDTH / 2, top + HEADER_HEIGHT );
    paint_text( hdc, load_string( title ), &text, font_title, pal.text, DT_LEFT );
    if (!(b = find_target( t ))) return;
    fill_round_rect( hdc, &b->rect, (float)px( 4 ), pal.ink, same( t, hot ) ? 0x1c : 0x0f );
    if (focus_shown && same( t, focus )) highlight( hdc, t, &b->rect, (float)px( 4 ) );
    if (button_kind == K_BACK_BUTTON)
    {
        draw_glyph( hdc, GLYPH_CHEVRON_LEFT, b->rect.left + px( 8 ), (b->rect.top + b->rect.bottom - CHEVRON) / 2,
                    CHEVRON, pal.ink, 255 );
        SetRect( &text, b->rect.left + px( 8 ) + CHEVRON + px( 6 ), b->rect.top, b->rect.right - px( 8 ), b->rect.bottom );
        paint_text( hdc, load_string( IDS_BACK ), &text, font_small, pal.text, DT_LEFT );
    }
    else
    {
        SetRect( &text, b->rect.left + px( 8 ), b->rect.top, b->rect.right - px( 8 ) - CHEVRON - px( 6 ), b->rect.bottom );
        paint_text( hdc, load_string( button_kind == K_ALL_BUTTON ? IDS_ALL_PROGRAMS : IDS_MORE ), &text, font_small,
                    pal.text, DT_LEFT );
        draw_glyph( hdc, GLYPH_CHEVRON_RIGHT, b->rect.right - px( 8 ) - CHEVRON,
                    (b->rect.top + b->rect.bottom - CHEVRON) / 2, CHEVRON, pal.ink, 255 );
    }
}

static void paint_search( HDC hdc )
{
    struct target t = { K_SEARCH, 0 };
    const struct target *box = find_target( t );
    RECT text, inner;
    float radius = SEARCH_HEIGHT / 2.0f;

    if (!box) return;
    inner = box->rect;
    if (pal.light)
    {
        fill_round_rect( hdc, &box->rect, radius, RGB( 0xd6, 0xd6, 0xd6 ), 255 );
        InflateRect( &inner, -1, -1 );
        fill_round_rect( hdc, &inner, radius - 1, RGB( 0xfb, 0xfb, 0xfb ), 255 );
    }
    else
    {
        fill_round_rect( hdc, &box->rect, radius, pal.ink, 0x1a );
        InflateRect( &inner, -1, -1 );
        fill_round_rect( hdc, &inner, radius - 1, RGB( 0, 0, 0 ), 0x60 );
    }
    draw_glyph( hdc, GLYPH_SEARCH, box->rect.left + px( 14 ), (box->rect.top + box->rect.bottom - GLYPH) / 2, GLYPH,
                pal.ink, 255 );
    SetRect( &text, box->rect.left + px( 14 ) + GLYPH + px( 10 ), box->rect.top, box->rect.right - px( 16 ),
             box->rect.bottom );
    if (search[0])
    {
        HGDIOBJ old = SelectObject( hdc, font_item );
        SIZE size;
        RECT caret;

        paint_text( hdc, search, &text, font_item, pal.text, DT_LEFT );
        GetTextExtentPoint32W( hdc, search, lstrlenW( search ), &size );
        SelectObject( hdc, old );
        if (caret_on && text.left + size.cx < text.right)
        {
            SetRect( &caret, text.left + size.cx + 1, box->rect.top + px( 8 ), text.left + size.cx + 2,
                     box->rect.bottom - px( 8 ) );
            fill_alpha( hdc, &caret, pal.ink, 255 );
        }
    }
    else paint_text( hdc, load_string( IDS_SEARCH_WIN11 ), &text, font_item, pal.dim, DT_LEFT );
    /* over a light look the box is opaque, and so is the text on it */
    if (pal.light)
    {
        InflateRect( &inner, -(int)radius / 2, -1 );
        make_opaque( hdc, &inner );
    }
}

static void paint_pins( HDC hdc )
{
    for (int i = 0; i < target_count; i++)
    {
        const struct target *t = &targets[i];
        struct item *item;
        RECT text;

        if (t->kind != K_PIN) continue;
        item = pins[t->index];
        highlight( hdc, *t, &t->rect, (float)px( 4 ) );
        draw_icon_bitmap( hdc, items_icon_sized( item, ICON ), (t->rect.left + t->rect.right - ICON) / 2,
                          t->rect.top + px( 14 ), ICON );
        SetRect( &text, t->rect.left + px( 4 ), t->rect.top + px( 14 ) + ICON + px( 6 ), t->rect.right - px( 4 ),
                 t->rect.top + px( 14 ) + ICON + px( 6 ) + px( 18 ) );
        paint_text( hdc, item->name, &text, font_small, pal.text, DT_CENTER );
    }
    /* the pages, as dots on the right */
    if (pages() > 1)
    {
        int x = WIDTH - SIDE / 2 - px( 2 ), y = pins_top() + (pin_rows() * CELL_HEIGHT - pages() * px( 12 )) / 2;

        for (int i = 0; i < pages(); i++)
        {
            RECT dot;
            SetRect( &dot, x - px( 2 ), y + i * px( 12 ), x + px( 2 ), y + i * px( 12 ) + px( 4 ) );
            if (i == page) InflateRect( &dot, px( 1 ), px( 1 ) );
            fill_round_rect( hdc, &dot, (dot.right - dot.left) / 2.0f, pal.ink, i == page ? 0xff : 0x80 );
        }
    }
}

static void paint_rec( HDC hdc, const struct target *t )
{
    struct recommended *rec = &recs[t->index];
    HBITMAP icon = rec->item ? items_icon_sized( rec->item, ICON ) : rec->icon;
    RECT text;
    int x = t->rect.left + px( 12 ), mid = (t->rect.top + t->rect.bottom) / 2;

    if (!rec->item && !rec->icon) icon = rec->icon = recent_file_icon( &rec->file, ICON );
    highlight( hdc, *t, &t->rect, (float)px( 4 ) );
    draw_icon_bitmap( hdc, icon, x, mid - ICON / 2, ICON );
    SetRect( &text, x + ICON + px( 12 ), mid - px( 18 ), t->rect.right - px( 8 ), mid );
    paint_text( hdc, rec->item ? rec->item->name : rec->file.name, &text, font_small, pal.text, DT_LEFT | DT_BOTTOM );
    SetRect( &text, x + ICON + px( 12 ), mid + px( 1 ), t->rect.right - px( 8 ), mid + px( 18 ) );
    paint_text( hdc, rec->detail, &text, font_small, pal.dim, DT_LEFT );
}

static void paint_recommended( HDC hdc, int top )
{
    BOOL any = FALSE;

    for (int i = 0; i < target_count; i++)
    {
        if (targets[i].kind != K_REC) continue;
        paint_rec( hdc, &targets[i] );
        any = TRUE;
    }
    if (!any)
    {
        RECT text;
        SetRect( &text, INSET, top, WIDTH - INSET, top + px( 60 ) );
        paint_text( hdc, load_string( IDS_NO_RECOMMENDED ), &text, font_small, pal.dim, DT_LEFT | DT_WORDBREAK );
    }
}

static void paint_rows( HDC hdc )
{
    RECT area = list_area( mode == MODE_ALL ), text;
    int y = area.top;

    for (int i = scroll; i < row_count; i++)
    {
        struct row *row = &rows[i];
        int h = row_height( row );
        struct target t = { row->kind == ROW_LETTER ? K_LETTER : K_ROW, i };
        const struct target *found = find_target( t );

        if (y + h > area.bottom) break;
        if (row->kind == ROW_HEADER)
        {
            SetRect( &text, area.left + px( 16 ), y, area.right, y + h );
            paint_text( hdc, row->item->name, &text, font_title, pal.text, DT_LEFT );
        }
        else if (row->kind == ROW_LETTER)
        {
            WCHAR letter[2] = { row->letter, 0 };
            if (found) highlight( hdc, t, &found->rect, (float)px( 4 ) );
            SetRect( &text, area.left + px( 24 ), y, area.right, y + h );
            paint_text( hdc, letter, &text, font_title, pal.text, DT_LEFT );
        }
        else
        {
            struct item *item = row->item;
            int x = area.left + px( 24 ) + (mode == MODE_ALL ? item->depth * px( 24 ) : 0);

            if (found) highlight( hdc, t, &found->rect, (float)px( 4 ) );
            if (item->kind == ITEM_FOLDER)
            {
                fill_round_rect( hdc, &(RECT){ x, y + (h - SMALL_ICON) / 2, x + SMALL_ICON, y + (h + SMALL_ICON) / 2 },
                                 (float)px( 3 ), pal.ink, 0x1a );
                draw_glyph( hdc, GLYPH_FOLDER, x + (SMALL_ICON - GLYPH) / 2, y + (h - GLYPH) / 2, GLYPH, pal.ink, 255 );
                draw_glyph( hdc, item->expanded ? GLYPH_CHEVRON_UP : GLYPH_CHEVRON_DOWN, area.right - px( 8 ) - px( 16 ) - CHEVRON,
                            y + (h - CHEVRON) / 2, CHEVRON, pal.ink, 255 );
            }
            else draw_icon_bitmap( hdc, items_icon_sized( item, SMALL_ICON ), x, y + (h - SMALL_ICON) / 2, SMALL_ICON );
            SetRect( &text, x + SMALL_ICON + px( 16 ), y, area.right - px( 48 ), y + h );
            paint_text( hdc, item->name, &text, font_item, pal.text, DT_LEFT );
        }
        y += h;
    }
    if (mode == MODE_SEARCH && !row_count)
    {
        SetRect( &text, area.left + px( 24 ), area.top, area.right, area.top + ROW_HEIGHT );
        paint_text( hdc, load_string( IDS_NO_RESULTS ), &text, font_item, pal.dim, DT_LEFT );
    }
    /* a thin bar when the list goes on */
    if (row_count > visible_rows( &area ) && visible_rows( &area ))
    {
        int span = area.bottom - area.top, shown = visible_rows( &area );
        int thumb = max( px( 24 ), span * shown / row_count ), max_scroll = max( 1, row_count - shown );
        int top = area.top + (span - thumb) * min( scroll, max_scroll ) / max_scroll;
        RECT bar;

        SetRect( &bar, area.right - px( 4 ), top, area.right - px( 2 ), top + thumb );
        fill_round_rect( hdc, &bar, (float)px( 1 ), pal.ink, 0x66 );
    }
}

static void paint_letters( HDC hdc )
{
    for (int i = 0; i < target_count; i++)
    {
        WCHAR letter[2];

        if (targets[i].kind != K_LETTER) continue;
        letter[0] = letters[targets[i].index];
        letter[1] = 0;
        highlight( hdc, targets[i], &targets[i].rect, (float)px( 4 ) );
        paint_text( hdc, letter, &targets[i].rect, font_letter, pal.text, DT_CENTER );
    }
}

static void paint_footer( HDC hdc )
{
    RECT band, line, text;

    SetRect( &band, 0, footer_top(), WIDTH, height );
    fill_alpha( hdc, &band, RGB( 0, 0, 0 ), pal.light ? 0x0a : 0x30 );
    SetRect( &line, 0, footer_top(), WIDTH, footer_top() + 1 );
    fill_alpha( hdc, &line, pal.ink, pal.light ? 0x14 : 0x18 );

    for (int i = 0; i < target_count; i++)
    {
        const struct target *t = &targets[i];

        switch (t->kind)
        {
        case K_USER:
            highlight( hdc, *t, &t->rect, (float)px( 4 ) );
            avatar_draw( hdc, INSET, (t->rect.top + t->rect.bottom - ICON) / 2, ICON, &pal );
            SetRect( &text, INSET + ICON + px( 12 ), t->rect.top, t->rect.right - px( 8 ), t->rect.bottom );
            paint_text( hdc, user_name, &text, font_small, pal.text, DT_LEFT );
            break;
        case K_POWER:
        case K_FOLDER:
            highlight( hdc, *t, &t->rect, (float)px( 4 ) );
            draw_glyph( hdc, t->kind == K_POWER ? GLYPH_POWER : folder_glyph( t->index ),
                        (t->rect.left + t->rect.right - GLYPH) / 2, (t->rect.top + t->rect.bottom - GLYPH) / 2, GLYPH,
                        pal.ink, 255 );
            break;
        default:
            break;
        }
    }
}

static void on_paint( HWND hwnd )
{
    PAINTSTRUCT ps;
    HBITMAP bitmap;
    HGDIOBJ old;
    HDC hdc = paint_begin( hwnd, &ps, &pal, &bitmap, &old );

    if (!hdc) return;
    paint_search( hdc );
    switch (mode)
    {
    case MODE_HOME:
        paint_header( hdc, content_top(), IDS_PINNED, K_ALL_BUTTON );
        paint_pins( hdc );
        paint_header( hdc, rec_header_top(), IDS_RECOMMENDED, K_MORE_BUTTON );
        paint_recommended( hdc, rec_header_top() + HEADER_HEIGHT + px( 8 ) );
        break;
    case MODE_RECOMMENDED:
        paint_header( hdc, content_top(), IDS_RECOMMENDED, K_BACK_BUTTON );
        paint_recommended( hdc, list_area( TRUE ).top );
        break;
    case MODE_ALL:
        paint_header( hdc, content_top(), IDS_ALL_PROGRAMS, K_BACK_BUTTON );
        paint_rows( hdc );
        break;
    case MODE_LETTERS:
        paint_header( hdc, content_top(), IDS_ALL_PROGRAMS, K_BACK_BUTTON );
        paint_letters( hdc );
        break;
    case MODE_SEARCH:
        paint_rows( hdc );
        break;
    }
    paint_footer( hdc );
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
    switch (mode)
    {
    case MODE_HOME:
        search[0] = 0;
        load_pins();
        break;
    case MODE_ALL:
        load_apps();
        break;
    case MODE_SEARCH:
        load_search();
        break;
    default:
        break;
    }
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

static void scroll_to_letter( WCHAR letter )
{
    set_mode( MODE_ALL );
    for (int i = 0; i < row_count; i++)
    {
        if (rows[i].kind != ROW_LETTER || rows[i].letter != letter) continue;
        scroll = i;
        break;
    }
    layout();
}

static void open_recommended( int index )
{
    struct recommended *rec = &recs[index];

    menu11_hide();
    if (rec->item) items_launch( rec->item );
    else open_path( rec->file.path );
}

static void activate( struct target t )
{
    POINT pt;
    const struct target *found = find_target( t );

    switch (t.kind)
    {
    case K_ALL_BUTTON:
        set_mode( MODE_ALL );
        break;
    case K_MORE_BUTTON:
        set_mode( MODE_RECOMMENDED );
        break;
    case K_BACK_BUTTON:
        set_mode( mode == MODE_LETTERS ? MODE_ALL : MODE_HOME );
        break;
    case K_PIN:
        menu11_hide();
        items_launch( pins[t.index] );
        return;
    case K_REC:
        open_recommended( t.index );
        return;
    case K_ROW:
    {
        struct item *item = rows[t.index].item;

        if (item->kind == ITEM_FOLDER)
        {
            int keep = scroll;
            item->expanded = !item->expanded;
            load_apps();
            scroll = min( keep, max( 0, row_count - 1 ) );
            layout();
            break;
        }
        menu11_hide();
        items_launch( item );
        return;
    }
    case K_LETTER:
        if (mode == MODE_LETTERS) scroll_to_letter( letters[t.index] );
        else
        {
            mode = MODE_LETTERS;
            hot.kind = pressed.kind = K_NONE;
            layout();
        }
        break;
    case K_FOLDER:
        menu11_hide();
        folder_open( t.index );
        return;
    case K_USER:
        if (!found) break;
        pt.x = found->rect.left;
        pt.y = found->rect.top;
        ClientToScreen( menu, &pt );
        user_popup( menu, pt, TPM_LEFTALIGN | TPM_BOTTOMALIGN, &in_popup, menu11_hide, POPUP_WIN11, &look );
        return;
    case K_POWER:
        if (!found) break;
        pt.x = found->rect.right;
        pt.y = found->rect.top;
        ClientToScreen( menu, &pt );
        power_popup( menu, pt, TPM_RIGHTALIGN | TPM_BOTTOMALIGN, &in_popup, menu11_hide, POPUP_WIN11, &look );
        return;
    default:
        return;
    }
    focus.kind = K_NONE;
    InvalidateRect( menu, NULL, FALSE );
}

static void context_menu( struct target t, POINT pt )
{
    struct item *item = NULL;

    if (t.kind == K_PIN) item = pins[t.index];
    else if (t.kind == K_ROW) item = rows[t.index].item;
    else if (t.kind == K_REC) item = recs[t.index].item;
    if (!item) return;
    ClientToScreen( menu, &pt );
    if (item_popup( menu, item, pt, FALSE, &in_popup, menu11_hide, POPUP_WIN11, &look ) != ACTION_CHANGED) return;
    load_pins();
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

static void scroll_by( int delta )
{
    if (mode == MODE_HOME)
    {
        int new_page = max( 0, min( page + (delta > 0 ? 1 : -1), pages() - 1 ) );
        if (new_page == page) return;
        page = new_page;
    }
    else if (mode == MODE_ALL || mode == MODE_SEARCH)
    {
        RECT area = list_area( mode == MODE_ALL );
        int max_scroll = 0, y = 0;

        /* the furthest it goes: the last rows fill the area */
        for (max_scroll = row_count; max_scroll > 0; max_scroll--)
        {
            y += row_height( &rows[max_scroll - 1] );
            if (y > area.bottom - area.top) break;
        }
        delta = max( 0, min( scroll + delta, max_scroll ) );
        if (delta == scroll) return;
        scroll = delta;
    }
    else return;
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
            if (targets[i].kind == K_SEARCH) continue;
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
    else if (dy && (mode == MODE_ALL || mode == MODE_SEARCH))
    {
        /* off the end of the list: it scrolls */
        scroll_by( dy );
        return;
    }
    InvalidateRect( menu, NULL, FALSE );
}

static void on_key( WPARAM key )
{
    switch (key)
    {
    case VK_ESCAPE:
        if (mode == MODE_HOME) menu11_hide();
        else
        {
            set_mode( mode == MODE_LETTERS ? MODE_ALL : MODE_HOME );
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
    case VK_PRIOR: scroll_by( -8 ); break;
    case VK_NEXT:  scroll_by( 8 ); break;
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
        if (menu11_visible()) InvalidateRect( hwnd, NULL, FALSE );
        return 0;

    case WM_RBUTTONUP:
        context_menu( hit_test( pt ), pt );
        return 0;

    case WM_MOUSEWHEEL:
        scroll_by( -(short)HIWORD( wparam ) / WHEEL_DELTA * (mode == MODE_HOME ? 1 : 3) );
        ScreenToClient( hwnd, &pt );
        set_hot( hit_test( pt ) );
        return 0;

    case WM_KEYDOWN:
        on_key( wparam );
        return 0;

    case WM_CHAR:
        on_char( (WCHAR)wparam );
        return 0;

    case WM_TIMER:
        if (wparam == TIMER_CARET && search[0])
        {
            caret_on = !caret_on;
            InvalidateRect( hwnd, &targets[0].rect, FALSE );
        }
        return 0;

    case WM_ACTIVATE:
        if (LOWORD( wparam ) == WA_INACTIVE && !in_popup) menu11_hide();
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
    /* Windows 11 rounds its Start menu by 8 */
    DWORD transition = ARCTIC_TRANSITION_SLIDE_UP, corner = ARCTIC_CORNER_RADIUS( 8 );
    DWORD size = ARRAY_SIZE(user_name);
    WNDCLASSW cls = { 0 };

    if (menu) return TRUE;
    cls.lpfnWndProc = menu_proc;
    cls.hInstance = startui_instance;
    cls.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_ARROW );
    cls.lpszClassName = L"ArcticStartMenu11";
    RegisterClassW( &cls );
    menu = CreateWindowExW( WS_EX_TOOLWINDOW | WS_EX_TOPMOST, cls.lpszClassName, L"", WS_POPUP,
                            0, 0, WIDTH, px( 700 ), NULL, NULL, startui_instance, NULL );
    if (!menu) return FALSE;
    DwmSetWindowAttribute( menu, DWMWA_ARCTIC_TRANSITION, &transition, sizeof(transition) );
    DwmSetWindowAttribute( menu, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner) );
    font_title = shell_font( px( 14 ), FW_SEMIBOLD );
    font_item = shell_font( px( 14 ), FW_NORMAL );
    font_small = shell_font( px( 12 ), FW_NORMAL );
    font_letter = shell_font( px( 20 ), FW_SEMIBOLD );
    closed_message = RegisterWindowMessageW( L"ArcticStartMenuClosed" );
    if (!GetUserNameW( user_name, &size )) lstrcpyW( user_name, L"User" );
    return TRUE;
}

/**********************************************************************
 *          Showing it
 */

BOOL menu11_visible(void)
{
    return menu && IsWindowVisible( menu );
}

void menu11_hide(void)
{
    if (!menu11_visible()) return;
    KillTimer( menu, TIMER_CARET );
    ShowWindow( menu, SW_HIDE );
    hidden_at = GetTickCount();
    free_recommended();
    if (tray && closed_message) PostMessageW( tray, closed_message, 0, 0 );
}

/* over the taskbar, 12 from it and from the screen's edge, by the Start button */
static void place(void)
{
    MONITORINFO info = { sizeof(info) };
    int x, y;

    GetMonitorInfoW( MonitorFromRect( &button, MONITOR_DEFAULTTONEAREST ), &info );
    height = min( menu_height(), (int)(info.rcWork.bottom - info.rcWork.top) - 2 * MARGIN );
    x = max( (int)info.rcWork.left + MARGIN, min( (int)button.left, (int)info.rcWork.right - WIDTH - MARGIN ) );
    if ((button.top + button.bottom) / 2 > (info.rcWork.top + info.rcWork.bottom) / 2)
        y = info.rcWork.bottom - height - MARGIN;
    else
        y = info.rcWork.top + MARGIN;
    if (button.right <= info.rcWork.left || button.left >= info.rcWork.right)
        y = max( (int)info.rcWork.top + MARGIN, min( (int)button.top, (int)info.rcWork.bottom - height - MARGIN ) );
    SetWindowPos( menu, HWND_TOPMOST, x, y, WIDTH, height, SWP_NOACTIVATE );
}

void menu11_toggle( HWND tray_window, const RECT *button_rect, BOOL click )
{
    if (!create_menu()) return;
    if (menu11_visible())
    {
        menu11_hide();
        return;
    }
    /* the click on Start that took the focus away and closed it */
    if (click && GetTickCount() - hidden_at < 300) return;

    tray = tray_window;
    if (button_rect) button = *button_rect;
    else SetRect( &button, 0, GetSystemMetrics( SM_CYSCREEN ) - 1, 1, GetSystemMetrics( SM_CYSCREEN ) );
    items_reload();
    modern_settings_load( STYLE_WIN11, &settings );
    search[0] = 0;
    page = 0;
    focus.kind = hot.kind = pressed.kind = K_NONE;
    focus_shown = FALSE;
    load_recommended();
    place();
    set_mode( MODE_HOME );
    /* the taskbar's look, or its own */
    start_look_load( STYLE_WIN11, &look );
    palette_init( &pal, look_apply( menu, &look ) );
    InvalidateRect( menu, NULL, FALSE );
    ShowWindow( menu, SW_SHOW );
    SetForegroundWindow( menu );
    SetFocus( menu );
    SetTimer( menu, TIMER_CARET, 530, NULL );
}
