/*
 * The search of Windows 11
 *
 * What the search box of the taskbar (search.c) and Win+S open, over the
 * taskbar as the Start menu, 800 wide at 100%, its corners rounded by 8:
 *
 * - at the top the search box, which takes what is typed at once;
 * - under it the tabs: All, Apps, Documents, Settings;
 * - with nothing typed: the files opened lately on the left, the programs
 *   used most on the right;
 * - with something typed: the results on the left, the best match first and
 *   larger, and on the right what is chosen, its picture, its name and kind,
 *   and what can be done with it: open it, open its folder, pin it to Start
 *   or to the taskbar.
 *
 * Arrows choose, Enter opens, Tab goes through the tabs, Esc clears what was
 * typed and then closes it. It takes the look of the Start menu of Windows 11.
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

#define WIDTH           px(800)
#define MARGIN          px(12)     /* from the taskbar */
#define SIDE            px(24)
#define BOX_TOP         px(24)
#define BOX_HEIGHT      px(36)
#define TABS_TOP        (BOX_TOP + BOX_HEIGHT + px(12))
#define TABS_HEIGHT     px(32)
#define CONTENT_TOP     (TABS_TOP + TABS_HEIGHT + px(16))
#define HEADER_HEIGHT   px(32)
#define ROW_HEIGHT      px(40)
#define BEST_HEIGHT     px(64)
#define LEFT_WIDTH      px(372)
#define CELL_WIDTH      px(108)
#define CELL_HEIGHT     px(88)
#define ACTION_HEIGHT   px(40)
#define GLYPH           px(16)
#define MAX_RESULTS     48
#define MAX_RECENT      8
#define MAX_TOP         6
#define MAX_TARGETS     96
#define MAX_TEXT        127

#define TIMER_CARET     1

void WINAPI ArcticStartMenuHide(void);

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

enum tab { TAB_ALL, TAB_APPS, TAB_DOCUMENTS, TAB_SETTINGS, TAB_COUNT };
enum group { GROUP_APPS, GROUP_SETTINGS, GROUP_FILES };
enum kind { K_NONE, K_BOX, K_TAB, K_RESULT, K_ACTION, K_RECENT, K_TOP };
enum action { A_OPEN, A_LOCATION, A_PIN_START, A_PIN_TASKBAR, A_COUNT };

struct target
{
    enum kind kind;
    int index;
    RECT rect;
};

struct result
{
    struct item *item;
    enum group group;
    BOOL header;                 /* a group's title */
};

static HWND flyout;
static RECT button;
static DWORD hidden_at;
static struct palette pal;
static struct look look;
static HFONT font_box, font_item, font_small, font_title, font_name;
static WCHAR text[MAX_TEXT + 1];
static int height;
static enum tab tab;
static BOOL caret_on, in_popup;

static struct result results[MAX_RESULTS];
static int result_count, chosen = -1;
static struct recent_file recents[MAX_RECENT];
static HBITMAP recent_icons[MAX_RECENT];
static int recent_count;
static struct item *tops[MAX_TOP];
static int top_count;
static enum action actions[A_COUNT];
static int action_count;

static struct target targets[MAX_TARGETS];
static int target_count;
static struct target hot, pressed;

/**********************************************************************
 *          What it shows
 */

static enum group group_of_header( const struct item *header )
{
    if (!wcscmp( header->name, load_string( IDS_RESULTS_SETTINGS ) )) return GROUP_SETTINGS;
    if (!wcscmp( header->name, load_string( IDS_RESULTS_FILES ) )) return GROUP_FILES;
    return GROUP_APPS;
}

static BOOL tab_takes( enum group group )
{
    switch (tab)
    {
    case TAB_APPS:      return group == GROUP_APPS;
    case TAB_DOCUMENTS: return group == GROUP_FILES;
    case TAB_SETTINGS:  return group == GROUP_SETTINGS;
    default:            return TRUE;
    }
}

static BOOL is_result( int i )
{
    return i >= 0 && i < result_count && !results[i].header;
}

static int first_result(void)
{
    for (int i = 0; i < result_count; i++) if (is_result( i )) return i;
    return -1;
}

/* the results of what is typed, in the groups the tab takes: in All, the
 * best match first, then the groups with what is left, under their titles */
static void load_results(void)
{
    struct item **list;
    struct result found[MAX_RESULTS];
    int found_count = 0, best = -1;
    UINT count;
    enum group group = GROUP_APPS;

    result_count = 0;
    chosen = -1;
    if (!text[0]) return;
    count = items_search( text, &list );
    for (UINT i = 0; i < count && found_count < MAX_RESULTS; i++)
    {
        if (list[i]->kind == ITEM_HEADER) group = group_of_header( list[i] );
        else if (tab_takes( group )) found[found_count++] = (struct result){ list[i], group, FALSE };
    }
    if (tab != TAB_ALL)
    {
        memcpy( results, found, found_count * sizeof(*found) );
        result_count = found_count;
        chosen = first_result();
        return;
    }
    if (found_count)
    {
        best = 0;
        results[result_count++] = found[0];
    }
    for (int g = GROUP_APPS; g <= GROUP_FILES; g++)
    {
        static const UINT titles[] = { IDS_RESULTS_PROGRAMS, IDS_RESULTS_SETTINGS, IDS_RESULTS_FILES };
        static struct item headers[3];
        BOOL titled = FALSE;

        for (int i = 0; i < found_count && result_count < MAX_RESULTS; i++)
        {
            if (i == best || found[i].group != (enum group)g) continue;
            if (!titled && result_count < MAX_RESULTS - 1)
            {
                headers[g].kind = ITEM_HEADER;
                lstrcpynW( headers[g].name, load_string( titles[g] ), ARRAY_SIZE(headers[g].name) );
                results[result_count++] = (struct result){ &headers[g], (enum group)g, TRUE };
                titled = TRUE;
            }
            results[result_count++] = found[i];
        }
    }
    chosen = first_result();
}

static void free_recent(void)
{
    for (int i = 0; i < recent_count; i++) if (recent_icons[i]) DeleteObject( recent_icons[i] );
    memset( recent_icons, 0, sizeof(recent_icons) );
    recent_count = 0;
}

static void load_home(void)
{
    struct item **list;
    UINT count;

    free_recent();
    recent_count = items_recent_files( recents, MAX_RECENT );
    count = items_frequent( &list, MAX_TOP );
    top_count = min( count, MAX_TOP );
    memcpy( tops, list, top_count * sizeof(*tops) );
    /* nothing used yet: what is pinned */
    if (top_count < MAX_TOP)
    {
        count = items_pinned( &list );
        for (UINT i = 0; i < count && top_count < MAX_TOP; i++)
        {
            BOOL there = FALSE;
            for (int j = 0; j < top_count && !there; j++) there = tops[j] == list[i];
            if (!there) tops[top_count++] = list[i];
        }
    }
}

/* what can be done with what is chosen */
static void load_actions(void)
{
    struct item *item;

    action_count = 0;
    if (!is_result( chosen )) return;
    item = results[chosen].item;
    actions[action_count++] = A_OPEN;
    if (results[chosen].group != GROUP_SETTINGS) actions[action_count++] = A_LOCATION;
    if (results[chosen].group != GROUP_FILES) actions[action_count++] = A_PIN_START;
    if (results[chosen].group == GROUP_APPS && item->kind == ITEM_PROGRAM) actions[action_count++] = A_PIN_TASKBAR;
}

/**********************************************************************
 *          Layout
 */

static int text_width( HFONT font, const WCHAR *str )
{
    HDC hdc = GetDC( flyout );
    HGDIOBJ old = SelectObject( hdc, font );
    SIZE size = { 0 };

    GetTextExtentPoint32W( hdc, str, lstrlenW( str ), &size );
    SelectObject( hdc, old );
    ReleaseDC( flyout, hdc );
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

static int result_height( int i )
{
    if (results[i].header) return HEADER_HEIGHT;
    return i == 0 && tab == TAB_ALL ? BEST_HEIGHT : ROW_HEIGHT;
}

static RECT preview_rect(void)
{
    RECT r;
    SetRect( &r, SIDE + LEFT_WIDTH + px( 16 ), CONTENT_TOP, WIDTH - SIDE, height - SIDE );
    return r;
}

static void layout(void)
{
    int x = SIDE, y;

    target_count = 0;
    add_target( K_BOX, 0, SIDE, BOX_TOP, WIDTH - SIDE, BOX_TOP + BOX_HEIGHT );
    for (int i = 0; i < TAB_COUNT; i++)
    {
        int w = text_width( font_small, load_string( IDS_TAB_ALL + i ) ) + px( 24 );
        add_target( K_TAB, i, x, TABS_TOP, x + w, TABS_TOP + TABS_HEIGHT );
        x += w + px( 4 );
    }

    if (text[0])
    {
        RECT preview = preview_rect();

        y = CONTENT_TOP;
        for (int i = 0; i < result_count; i++)
        {
            int h = result_height( i );
            if (y + h > height - SIDE) break;
            if (!results[i].header) add_target( K_RESULT, i, SIDE, y, SIDE + LEFT_WIDTH, y + h );
            y += h;
        }
        /* the actions, under the chosen one's picture and name */
        y = preview.top + px( 32 + 64 + 16 ) + px( 28 + 22 ) + px( 24 );
        for (int i = 0; i < action_count; i++)
        {
            add_target( K_ACTION, i, preview.left + px( 12 ), y, preview.right - px( 12 ), y + ACTION_HEIGHT );
            y += ACTION_HEIGHT;
        }
    }
    else
    {
        y = CONTENT_TOP + HEADER_HEIGHT + px( 4 );
        for (int i = 0; i < recent_count; i++)
        {
            if (y + ROW_HEIGHT > height - SIDE) break;
            add_target( K_RECENT, i, SIDE, y, SIDE + LEFT_WIDTH, y + ROW_HEIGHT );
            y += ROW_HEIGHT;
        }
        x = SIDE + LEFT_WIDTH + px( 16 );
        y = CONTENT_TOP + HEADER_HEIGHT + px( 4 );
        for (int i = 0; i < top_count; i++)
        {
            int cx = x + (i % 3) * CELL_WIDTH, cy = y + (i / 3) * CELL_HEIGHT;
            add_target( K_TOP, i, cx, cy, cx + CELL_WIDTH, cy + CELL_HEIGHT );
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

/**********************************************************************
 *          Painting
 */

static void lit( HDC hdc, const struct target *t, BOOL selected )
{
    BOOL is_hot = same( *t, hot ), is_pressed = is_hot && same( *t, pressed );

    if (is_pressed) fill_round_rect( hdc, &t->rect, (float)px( 4 ), pal.ink, 0x0c );
    else if (is_hot || selected) fill_round_rect( hdc, &t->rect, (float)px( 4 ), pal.ink, selected && !is_hot ? 0x10 : 0x16 );
}

static void paint_box( HDC hdc )
{
    const RECT *box = &targets[0].rect;
    RECT inner = *box, r;
    float radius = BOX_HEIGHT / 2.0f;
    int x = box->left + px( 16 );

    if (pal.light)
    {
        fill_round_rect( hdc, box, radius, RGB( 0xd6, 0xd6, 0xd6 ), 255 );
        InflateRect( &inner, -1, -1 );
        fill_round_rect( hdc, &inner, radius - 1, RGB( 0xfb, 0xfb, 0xfb ), 255 );
    }
    else
    {
        fill_round_rect( hdc, box, radius, pal.ink, 0x1a );
        InflateRect( &inner, -1, -1 );
        fill_round_rect( hdc, &inner, radius - 1, RGB( 0, 0, 0 ), 0x60 );
    }
    draw_glyph( hdc, GLYPH_SEARCH, x, (box->top + box->bottom - GLYPH) / 2, GLYPH, pal.ink, 255 );
    x += GLYPH + px( 12 );
    SetRect( &r, x, box->top, box->right - px( 16 ), box->bottom );
    if (text[0]) paint_text( hdc, text, &r, font_box, pal.text, DT_LEFT );
    else paint_text( hdc, load_string( IDS_SEARCH_PLACEHOLDER ), &r, font_box, pal.dim, DT_LEFT );
    if (caret_on)
    {
        HGDIOBJ old = SelectObject( hdc, font_box );
        SIZE size = { 0 };
        RECT caret;

        if (text[0]) GetTextExtentPoint32W( hdc, text, lstrlenW( text ), &size );
        SelectObject( hdc, old );
        if (x + size.cx < r.right)
        {
            SetRect( &caret, x + size.cx + 1, box->top + px( 9 ), x + size.cx + 2, box->bottom - px( 9 ) );
            fill_alpha( hdc, &caret, pal.ink, 255 );
        }
    }
    if (pal.light)
    {
        InflateRect( &inner, -(int)radius / 2, -1 );
        make_opaque( hdc, &inner );
    }
}

static void paint_tabs( HDC hdc )
{
    for (int i = 0; i < target_count; i++)
    {
        const struct target *t = &targets[i];
        RECT r = t->rect;

        if (t->kind != K_TAB) continue;
        lit( hdc, t, FALSE );
        paint_text( hdc, load_string( IDS_TAB_ALL + t->index ), &r, t->index == (int)tab ? font_title : font_small,
                    t->index == (int)tab ? pal.text : pal.dim, DT_CENTER );
        if (t->index == (int)tab)
        {
            /* the pill of the chosen tab, in the accent */
            RECT pill = { (r.left + r.right) / 2 - px( 8 ), r.bottom - px( 3 ), (r.left + r.right) / 2 + px( 8 ), r.bottom };
            COLORREF accent = accent_color();

            if (!pal.light)
                accent = RGB( GetRValue( accent ) + (255 - GetRValue( accent )) * 45 / 100,
                              GetGValue( accent ) + (255 - GetGValue( accent )) * 45 / 100,
                              GetBValue( accent ) + (255 - GetBValue( accent )) * 45 / 100 );
            fill_round_rect( hdc, &pill, px( 3 ) / 2.0f, accent, 255 );
        }
    }
}

static void paint_header( HDC hdc, int left, int top, int right, const WCHAR *title )
{
    RECT r = { left + px( 12 ), top, right, top + HEADER_HEIGHT };
    paint_text( hdc, title, &r, font_title, pal.text, DT_LEFT );
}

static const WCHAR *kind_text( enum group group )
{
    return load_string( group == GROUP_SETTINGS ? IDS_KIND_SETTING : group == GROUP_FILES ? IDS_KIND_FILE : IDS_KIND_APP );
}

static void paint_results( HDC hdc )
{
    int y = CONTENT_TOP;

    for (int i = 0; i < result_count; i++)
    {
        int h = result_height( i );
        struct target t = { K_RESULT, i };
        const struct target *found = NULL;
        struct item *item = results[i].item;
        RECT r;

        if (y + h > height - SIDE) break;
        if (results[i].header)
        {
            paint_header( hdc, SIDE, y, SIDE + LEFT_WIDTH, item->name );
            y += h;
            continue;
        }
        for (int k = 0; k < target_count; k++) if (same( targets[k], t )) found = &targets[k];
        if (found) lit( hdc, found, i == chosen );
        if (h == BEST_HEIGHT)
        {
            int icon = px( 32 );

            draw_icon_bitmap( hdc, items_icon_sized( item, icon ), SIDE + px( 12 ), y + (h - icon) / 2, icon );
            SetRect( &r, SIDE + px( 12 ) + icon + px( 12 ), y + px( 12 ), SIDE + LEFT_WIDTH - px( 8 ), y + h / 2 );
            paint_text( hdc, item->name, &r, font_item, pal.text, DT_LEFT | DT_BOTTOM );
            SetRect( &r, SIDE + px( 12 ) + icon + px( 12 ), y + h / 2 + px( 2 ), SIDE + LEFT_WIDTH - px( 8 ), y + h - px( 10 ) );
            paint_text( hdc, kind_text( results[i].group ), &r, font_small, pal.dim, DT_LEFT );
        }
        else
        {
            int icon = px( 24 );

            draw_icon_bitmap( hdc, items_icon_sized( item, icon ), SIDE + px( 12 ), y + (h - icon) / 2, icon );
            SetRect( &r, SIDE + px( 12 ) + icon + px( 12 ), y, SIDE + LEFT_WIDTH - px( 8 ), y + h );
            paint_text( hdc, item->name, &r, font_item, pal.text, DT_LEFT );
        }
        y += h;
    }
    if (!result_count)
    {
        RECT r = { SIDE + px( 12 ), CONTENT_TOP, SIDE + LEFT_WIDTH, CONTENT_TOP + ROW_HEIGHT };
        paint_text( hdc, load_string( IDS_NO_RESULTS ), &r, font_item, pal.dim, DT_LEFT );
    }
}

static const WCHAR *action_text( enum action action, BOOL *pinned, enum glyph *glyph )
{
    struct item *item = results[chosen].item;
    WCHAR link[MAX_PATH];

    *pinned = FALSE;
    switch (action)
    {
    case A_OPEN:
        *glyph = GLYPH_NONE;
        return load_string( IDS_OPEN );
    case A_LOCATION:
        *glyph = GLYPH_FOLDER;
        return load_string( IDS_OPEN_LOCATION );
    case A_PIN_START:
        *pinned = items_is_pinned( item );
        *glyph = *pinned ? GLYPH_UNPIN : GLYPH_PIN;
        return load_string( *pinned ? IDS_UNPIN : IDS_PIN );
    case A_PIN_TASKBAR:
        *pinned = items_taskbar_pinned( item, link );
        *glyph = *pinned ? GLYPH_UNPIN : GLYPH_PIN;
        return load_string( *pinned ? IDS_UNPIN_TASKBAR : IDS_PIN_TASKBAR );
    default:
        *glyph = GLYPH_NONE;
        return L"";
    }
}

static void paint_preview( HDC hdc )
{
    RECT plate = preview_rect(), r;
    struct item *item;
    int icon = px( 64 ), mid = (plate.left + plate.right) / 2, y;

    if (!is_result( chosen )) return;
    item = results[chosen].item;
    fill_round_rect( hdc, &plate, (float)px( 8 ), pal.ink, pal.light ? 0x08 : 0x0c );
    y = plate.top + px( 32 );
    draw_icon_bitmap( hdc, items_icon_sized( item, icon ), mid - icon / 2, y, icon );
    y += icon + px( 16 );
    SetRect( &r, plate.left + px( 16 ), y, plate.right - px( 16 ), y + px( 28 ) );
    paint_text( hdc, item->name, &r, font_name, pal.text, DT_CENTER );
    y += px( 28 );
    SetRect( &r, plate.left + px( 16 ), y, plate.right - px( 16 ), y + px( 22 ) );
    paint_text( hdc, kind_text( results[chosen].group ), &r, font_small, pal.dim, DT_CENTER );
    y += px( 22 ) + px( 12 );
    SetRect( &r, plate.left + px( 16 ), y, plate.right - px( 16 ), y + 1 );
    fill_alpha( hdc, &r, pal.ink, pal.light ? 0x14 : 0x1a );

    for (int i = 0; i < target_count; i++)
    {
        const struct target *t = &targets[i];
        enum glyph glyph;
        BOOL pinned;
        const WCHAR *label;
        HBITMAP picture;
        int gx = t->rect.left + px( 12 ), gy = (t->rect.top + t->rect.bottom - GLYPH) / 2;

        if (t->kind != K_ACTION) continue;
        label = action_text( actions[t->index], &pinned, &glyph );
        lit( hdc, t, FALSE );
        /* the pictures of File Explorer's menus */
        picture = menu_picture( actions[t->index] == A_OPEN ? "open" : actions[t->index] == A_LOCATION ? "folder" :
                                pinned ? "unpin" : "pin", pal.light, GLYPH );
        if (picture) draw_icon_bitmap( hdc, picture, gx, gy, GLYPH );
        else if (glyph != GLYPH_NONE) draw_glyph( hdc, glyph, gx, gy, GLYPH, pal.ink, 255 );
        SetRect( &r, gx + GLYPH + px( 12 ), t->rect.top, t->rect.right - px( 8 ), t->rect.bottom );
        paint_text( hdc, label, &r, font_item, pal.text, DT_LEFT );
    }
}

static void paint_home( HDC hdc )
{
    int right = SIDE + LEFT_WIDTH + px( 16 );
    RECT r;
    WCHAR age[64];

    paint_header( hdc, SIDE, CONTENT_TOP, SIDE + LEFT_WIDTH, load_string( IDS_RECENT_SEARCH ) );
    paint_header( hdc, right, CONTENT_TOP, WIDTH - SIDE, load_string( IDS_TOP_APPS ) );
    if (!recent_count)
    {
        SetRect( &r, SIDE + px( 12 ), CONTENT_TOP + HEADER_HEIGHT, SIDE + LEFT_WIDTH, CONTENT_TOP + HEADER_HEIGHT + px( 48 ) );
        paint_text( hdc, load_string( IDS_NO_RECENT ), &r, font_small, pal.dim, DT_LEFT | DT_WORDBREAK );
    }
    for (int i = 0; i < target_count; i++)
    {
        const struct target *t = &targets[i];
        int icon;

        if (t->kind == K_RECENT)
        {
            int k = t->index;

            icon = px( 24 );
            lit( hdc, t, FALSE );
            if (!recent_icons[k]) recent_icons[k] = recent_file_icon( &recents[k], icon );
            draw_icon_bitmap( hdc, recent_icons[k], t->rect.left + px( 12 ), (t->rect.top + t->rect.bottom - icon) / 2, icon );
            describe_age( &recents[k].time, age, ARRAY_SIZE(age) );
            SetRect( &r, t->rect.right - px( 8 ) - text_width( font_small, age ), t->rect.top, t->rect.right - px( 12 ),
                     t->rect.bottom );
            paint_text( hdc, age, &r, font_small, pal.dim, DT_RIGHT );
            r.right = r.left - px( 8 );
            r.left = t->rect.left + px( 12 ) + icon + px( 12 );
            paint_text( hdc, recents[k].name, &r, font_item, pal.text, DT_LEFT );
        }
        else if (t->kind == K_TOP)
        {
            struct item *item = tops[t->index];

            icon = px( 32 );
            lit( hdc, t, FALSE );
            draw_icon_bitmap( hdc, items_icon_sized( item, icon ), (t->rect.left + t->rect.right - icon) / 2,
                              t->rect.top + px( 14 ), icon );
            SetRect( &r, t->rect.left + px( 4 ), t->rect.top + px( 14 ) + icon + px( 6 ), t->rect.right - px( 4 ),
                     t->rect.top + px( 14 ) + icon + px( 6 ) + px( 18 ) );
            paint_text( hdc, item->name, &r, font_small, pal.text, DT_CENTER );
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
    paint_box( hdc );
    paint_tabs( hdc );
    if (text[0])
    {
        paint_results( hdc );
        paint_preview( hdc );
    }
    else paint_home( hdc );
    paint_end( hwnd, &ps, hdc, bitmap, old );
}

/**********************************************************************
 *          Acting
 */

static void refresh(void)
{
    load_actions();
    layout();
    InvalidateRect( flyout, NULL, FALSE );
}

static void text_changed(void)
{
    hot.kind = pressed.kind = K_NONE;
    load_results();
    refresh();
}

static void set_tab( int new_tab )
{
    tab = (new_tab % TAB_COUNT + TAB_COUNT) % TAB_COUNT;
    text_changed();
}

/* the folder of what is chosen, it chosen there */
static void open_location( const struct result *result )
{
    WCHAR target[MAX_PATH], args[MAX_PATH + 16];

    if (result->group != GROUP_FILES)
    {
        items_open_location( result->item );
        return;
    }
    if (!link_target( result->item->path, target, ARRAY_SIZE(target) )) lstrcpynW( target, result->item->path, MAX_PATH );
    swprintf( args, ARRAY_SIZE(args), L"/select,\"%s\"", target );
    ShellExecuteW( NULL, NULL, L"explorer.exe", args, NULL, SW_SHOWNORMAL );
}

static void do_action( enum action action )
{
    struct result *result;
    WCHAR link[MAX_PATH];

    if (!is_result( chosen )) return;
    result = &results[chosen];
    switch (action)
    {
    case A_OPEN:
        search11_hide();
        items_launch( result->item );
        break;
    case A_LOCATION:
        search11_hide();
        open_location( result );
        break;
    case A_PIN_START:
        items_pin( result->item, !items_is_pinned( result->item ) );
        refresh();
        break;
    case A_PIN_TASKBAR:
        items_pin_taskbar( result->item, !items_taskbar_pinned( result->item, link ) );
        refresh();
        break;
    default:
        break;
    }
}

static void open_recent( int index )
{
    search11_hide();
    open_path( recents[index].path );
}

static void activate( struct target t )
{
    switch (t.kind)
    {
    case K_TAB:
        set_tab( t.index );
        break;
    case K_RESULT:
        if (t.index == chosen)
        {
            do_action( A_OPEN );
            return;
        }
        chosen = t.index;
        refresh();
        break;
    case K_ACTION:
        do_action( actions[t.index] );
        break;
    case K_RECENT:
        open_recent( t.index );
        break;
    case K_TOP:
        search11_hide();
        items_launch( tops[t.index] );
        break;
    default:
        break;
    }
}

static void move_choice( int delta )
{
    int i = chosen;

    if (!result_count) return;
    do
    {
        i += delta;
        if (i < 0 || i >= result_count) return;
    } while (!is_result( i ));
    chosen = i;
    refresh();
}

static void context_menu( struct target t, POINT pt )
{
    struct item *item = NULL;

    if (t.kind == K_RESULT && results[t.index].group != GROUP_FILES) item = results[t.index].item;
    else if (t.kind == K_TOP) item = tops[t.index];
    if (!item) return;
    ClientToScreen( flyout, &pt );
    if (item_popup( flyout, item, pt, FALSE, &in_popup, search11_hide, POPUP_WIN11, &look ) == ACTION_CHANGED)
    {
        if (!text[0]) load_home();
        refresh();
    }
}

static void on_key( WPARAM key )
{
    switch (key)
    {
    case VK_ESCAPE:
        if (text[0])
        {
            text[0] = 0;
            text_changed();
        }
        else search11_hide();
        break;
    case VK_UP:     move_choice( -1 ); break;
    case VK_DOWN:   move_choice( 1 ); break;
    case VK_TAB:    set_tab( tab + (GetKeyState( VK_SHIFT ) < 0 ? -1 : 1) ); break;
    case VK_RETURN: if (is_result( chosen )) do_action( A_OPEN ); break;
    case VK_BACK:
        if (text[0])
        {
            text[lstrlenW( text ) - 1] = 0;
            text_changed();
        }
        break;
    }
}

static void on_char( WCHAR ch )
{
    int len = lstrlenW( text );

    if (ch < 0x20 || ch == 0x7f || len >= MAX_TEXT) return;
    if (!len && ch == ' ') return;
    text[len] = ch;
    text[len + 1] = 0;
    caret_on = TRUE;
    text_changed();
}

static LRESULT WINAPI flyout_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
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
        t = hit_test( pt );
        if (!same( t, hot ))
        {
            hot = t;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        return 0;

    case WM_MOUSELEAVE:
        hot.kind = pressed.kind = K_NONE;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;

    case WM_LBUTTONDOWN:
        pressed = hit_test( pt );
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;

    case WM_LBUTTONUP:
        t = hit_test( pt );
        if (same( t, pressed ) && t.kind != K_NONE && t.kind != K_BOX)
        {
            pressed.kind = K_NONE;
            activate( t );
        }
        pressed.kind = K_NONE;
        if (search11_visible()) InvalidateRect( hwnd, NULL, FALSE );
        return 0;

    case WM_RBUTTONUP:
        context_menu( hit_test( pt ), pt );
        return 0;

    case WM_MOUSEWHEEL:
        move_choice( (short)HIWORD( wparam ) > 0 ? -1 : 1 );
        return 0;

    case WM_KEYDOWN:
        on_key( wparam );
        return 0;

    case WM_CHAR:
        on_char( (WCHAR)wparam );
        return 0;

    case WM_TIMER:
        if (wparam == TIMER_CARET)
        {
            caret_on = !caret_on;
            InvalidateRect( hwnd, &targets[0].rect, FALSE );
        }
        return 0;

    case WM_SETCURSOR:
        GetCursorPos( &pt );
        ScreenToClient( hwnd, &pt );
        if (hit_test( pt ).kind == K_BOX)
        {
            SetCursor( LoadCursorW( NULL, (const WCHAR *)IDC_IBEAM ) );
            return TRUE;
        }
        break;

    case WM_ACTIVATE:
        if (LOWORD( wparam ) == WA_INACTIVE && !in_popup) search11_hide();
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

static BOOL create_flyout(void)
{
    DWORD transition = ARCTIC_TRANSITION_START11, corner = ARCTIC_CORNER_RADIUS( 8 );
    WNDCLASSW cls = { 0 };

    if (flyout) return TRUE;
    cls.lpfnWndProc = flyout_proc;
    cls.hInstance = startui_instance;
    cls.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_ARROW );
    cls.lpszClassName = L"ArcticSearch11";
    RegisterClassW( &cls );
    flyout = CreateWindowExW( WS_EX_TOOLWINDOW | WS_EX_TOPMOST, cls.lpszClassName, L"", WS_POPUP,
                              0, 0, WIDTH, px( 680 ), NULL, NULL, startui_instance, NULL );
    if (!flyout) return FALSE;
    DwmSetWindowAttribute( flyout, DWMWA_ARCTIC_TRANSITION, &transition, sizeof(transition) );
    DwmSetWindowAttribute( flyout, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner) );
    font_box = shell_font( px( 14 ), FW_NORMAL );
    font_item = shell_font( px( 14 ), FW_NORMAL );
    font_small = shell_font( px( 13 ), FW_NORMAL );
    font_title = shell_font( px( 14 ), FW_SEMIBOLD );
    font_name = shell_font( px( 20 ), FW_SEMIBOLD );
    return TRUE;
}

/**********************************************************************
 *          Showing it
 */

BOOL search11_visible(void)
{
    return flyout && IsWindowVisible( flyout );
}

void search11_hide(void)
{
    if (!search11_visible()) return;
    KillTimer( flyout, TIMER_CARET );
    ShowWindow( flyout, SW_HIDE );
    hidden_at = GetTickCount();
    free_recent();
    search_button_refresh();
}

/* over the taskbar, 12 from it: in the middle of a centred taskbar, else from the box */
static void place(void)
{
    MONITORINFO info = { sizeof(info) };
    int x, y;

    GetMonitorInfoW( MonitorFromRect( &button, MONITOR_DEFAULTTONEAREST ), &info );
    height = min( px( 680 ), (int)(info.rcWork.bottom - info.rcWork.top) - 2 * MARGIN );
    x = max( (int)info.rcWork.left + MARGIN, min( (int)button.left, (int)info.rcWork.right - WIDTH - MARGIN ) );
    if (taskbar_centered()) x = (info.rcWork.left + info.rcWork.right - WIDTH) / 2;
    if ((button.top + button.bottom) / 2 > (info.rcWork.top + info.rcWork.bottom) / 2)
        y = info.rcWork.bottom - height - MARGIN;
    else
        y = info.rcWork.top + MARGIN;
    SetWindowPos( flyout, HWND_TOPMOST, x, y, WIDTH, height, SWP_NOACTIVATE );
}

void search11_toggle( HWND tray_window, const RECT *button_rect )
{
    if (!create_flyout()) return;
    if (search11_visible())
    {
        search11_hide();
        return;
    }
    /* the click on the box that took the focus away and closed it */
    if (GetTickCount() - hidden_at < 300) return;

    ArcticStartMenuHide();
    widgets_hide();
    if (button_rect) button = *button_rect;
    else SetRect( &button, 0, GetSystemMetrics( SM_CYSCREEN ) - 1, 1, GetSystemMetrics( SM_CYSCREEN ) );
    items_reload();
    text[0] = 0;
    tab = TAB_ALL;
    hot.kind = pressed.kind = K_NONE;
    result_count = 0;
    chosen = -1;
    load_home();
    place();
    start_look_load( STYLE_WIN11, &look );
    palette_init( &pal, look_apply( flyout, &look ) );
    refresh();
    caret_on = TRUE;
    ShowWindow( flyout, SW_SHOW );
    SetForegroundWindow( flyout );
    SetFocus( flyout );
    SetTimer( flyout, TIMER_CARET, 530, NULL );
    search_button_refresh();
}
