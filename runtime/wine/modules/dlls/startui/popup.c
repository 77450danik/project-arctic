/*
 * The Start menus: their context menus
 *
 * As the Start menus of Windows 10 and 11 draw theirs, not as USER's menus:
 * over the menu's own backdrop (its look, a little more opaque), 32 a row
 * with a 16 glyph at 12 and the text at 44, a chevron for a submenu, a
 * check for the chosen one. Windows 10's are square, the row under the
 * pointer lit edge to edge; Windows 11's are rounded by 8 with 4 around
 * the rows, each rounded by 4 when lit. A submenu opens beside its row when
 * the pointer rests on it or the row is clicked; dwm.exe fades and slides
 * each in as it fades popups in. The call returns what was chosen, or 0.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdlib.h>

#include "startui.h"
#include "dwmapi.h"
#include "wine/arctic_dwm.h"

#define ROW            px(32)
#define SEPARATOR      px(9)
#define PADDING        (style == POPUP_WIN11 ? px(4) : px(6))
#define MIN_WIDTH      px(200)
#define GLYPH_LEFT     px(12)
#define TEXT_LEFT      px(44)
#define GLYPH_SIZE     px(16)
#define CHEVRON_SIZE   px(10)
#define MAX_LEVELS     3

#define TIMER_SUBMENU  1
#define SUBMENU_DELAY  300

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

struct level
{
    HWND hwnd;
    const struct popup_item *items;
    UINT count;
    int hot;
    int parent_row;              /* of the level before, which opened it */
    int width, height;
};

static struct level levels[MAX_LEVELS];
static int level_count;
static int style;
static struct look popup_look;
static struct palette pal;
static HFONT font;
static UINT result;
static BOOL done, keyboard;
static int pending_level = -1, pending_row = -1;   /* the submenu the timer opens */

static int row_height( const struct popup_item *item )
{
    return item->id || item->sub ? ROW : SEPARATOR;
}

static int row_top( const struct level *level, int row )
{
    int y = PADDING;
    for (int i = 0; i < row; i++) y += row_height( &level->items[i] );
    return y;
}

static RECT row_rect( const struct level *level, int row )
{
    RECT rect;
    int inset = style == POPUP_WIN11 ? px( 4 ) : 0;

    SetRect( &rect, inset, row_top( level, row ), level->width - inset,
             row_top( level, row ) + row_height( &level->items[row] ) );
    return rect;
}

static BOOL selectable( const struct popup_item *item )
{
    return (item->id || item->sub) && !item->disabled;
}

static int level_of( HWND hwnd )
{
    for (int i = 0; i < level_count; i++) if (levels[i].hwnd == hwnd) return i;
    return -1;
}

static int row_at( const struct level *level, POINT pt )
{
    for (UINT i = 0; i < level->count; i++)
    {
        RECT rect = row_rect( level, i );
        if (PtInRect( &rect, pt )) return i;
    }
    return -1;
}

static void paint( HWND hwnd )
{
    struct level *level = &levels[level_of( hwnd )];
    PAINTSTRUCT ps;
    HBITMAP bitmap;
    HGDIOBJ old;
    HDC hdc = paint_begin( hwnd, &ps, &pal, &bitmap, &old );

    if (!hdc) return;
    for (UINT i = 0; i < level->count; i++)
    {
        const struct popup_item *item = &level->items[i];
        RECT rect = row_rect( level, i ), text;
        COLORREF color = item->disabled ? pal.dim : pal.text;
        int mid = (rect.top + rect.bottom) / 2;

        if (!item->id && !item->sub)
        {
            RECT line;
            SetRect( &line, px( 4 ), mid, level->width - px( 4 ), mid + 1 );
            fill_alpha( hdc, &line, pal.ink, 0x26 );
            continue;
        }
        if ((int)i == level->hot && !item->disabled)
        {
            if (style == POPUP_WIN11) fill_round_rect( hdc, &rect, (float)px( 4 ), pal.ink, 0x14 );
            else fill_alpha( hdc, &rect, pal.ink, 0x1a );
        }
        if (item->checked)
            draw_glyph( hdc, GLYPH_CHECK, GLYPH_LEFT, mid - GLYPH_SIZE / 2, GLYPH_SIZE, color, 255 );
        else if (item->glyph != GLYPH_NONE)
            draw_glyph( hdc, item->glyph, GLYPH_LEFT, mid - GLYPH_SIZE / 2, GLYPH_SIZE, color, 255 );
        SetRect( &text, TEXT_LEFT, rect.top, rect.right - px( 12 ) - (item->sub ? CHEVRON_SIZE + px( 8 ) : 0),
                 rect.bottom );
        paint_text( hdc, item->text, &text, font, color, DT_LEFT );
        if (item->sub)
            draw_glyph( hdc, GLYPH_CHEVRON_RIGHT, rect.right - px( 12 ) - CHEVRON_SIZE, mid - CHEVRON_SIZE / 2,
                        CHEVRON_SIZE, color, 255 );
    }
    paint_end( hwnd, &ps, hdc, bitmap, old );
}

static void close_after( int index )
{
    while (level_count > index + 1)
    {
        level_count--;
        DestroyWindow( levels[level_count].hwnd );
        levels[level_count].hwnd = NULL;
    }
}

static BOOL open_level( HWND owner, POINT pt, UINT align, const struct popup_item *items, UINT count, int parent_row );

/* the submenu of a row, beside it */
static void open_submenu( int index, int row )
{
    struct level *level = &levels[index];
    RECT rect = row_rect( level, row );
    POINT pt = { level->width - px( 4 ), rect.top - PADDING };

    close_after( index );
    if (!level->items[row].sub || level->items[row].disabled) return;
    ClientToScreen( level->hwnd, &pt );
    open_level( level->hwnd, pt, TPM_LEFTALIGN | TPM_TOPALIGN, level->items[row].sub, level->items[row].sub_count, row );
    if (keyboard && level_count > index + 1)
    {
        struct level *sub = &levels[index + 1];
        for (UINT i = 0; i < sub->count; i++) if (selectable( &sub->items[i] )) { sub->hot = i; break; }
        InvalidateRect( sub->hwnd, NULL, FALSE );
    }
}

static void choose( int index, int row )
{
    const struct popup_item *item = &levels[index].items[row];

    if (!selectable( item )) return;
    if (item->sub)
    {
        if (level_count <= index + 1 || levels[index + 1].parent_row != row) open_submenu( index, row );
        return;
    }
    result = item->id;
    done = TRUE;
}

static void set_hot( int index, int row )
{
    struct level *level = &levels[index];

    if (level->hot == row) return;
    level->hot = row;
    InvalidateRect( level->hwnd, NULL, FALSE );
    /* resting on a row opens its submenu, or closes the one of another row */
    KillTimer( level->hwnd, TIMER_SUBMENU );
    pending_level = index;
    pending_row = row;
    if (row >= 0 && (level->items[row].sub || level_count > index + 1))
        SetTimer( level->hwnd, TIMER_SUBMENU, SUBMENU_DELAY, NULL );
}

static void move_hot( int index, int delta )
{
    struct level *level = &levels[index];
    int row = level->hot;

    for (UINT n = 0; n < level->count; n++)
    {
        row = (row + delta + (int)level->count) % (int)level->count;
        if (row < 0) row = 0;
        if (selectable( &level->items[row] )) break;
    }
    level->hot = row;
    InvalidateRect( level->hwnd, NULL, FALSE );
}

static LRESULT WINAPI popup_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    POINT pt = { (short)LOWORD( lparam ), (short)HIWORD( lparam ) };
    int index = level_of( hwnd ), row;

    if (index < 0) return DefWindowProcW( hwnd, msg, wparam, lparam );
    switch (msg)
    {
    case WM_MOUSEMOVE:
    {
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent( &tme );
        keyboard = FALSE;
        row = row_at( &levels[index], pt );
        set_hot( index, row >= 0 && selectable( &levels[index].items[row] ) ? row : -1 );
        return 0;
    }
    case WM_MOUSELEAVE:
        /* the row with its submenu open stays lit */
        if (level_count <= index + 1) set_hot( index, -1 );
        return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        if ((row = row_at( &levels[index], pt )) >= 0) choose( index, row );
        return 0;
    case WM_TIMER:
        if (wparam == TIMER_SUBMENU)
        {
            KillTimer( hwnd, TIMER_SUBMENU );
            if (pending_level == index && pending_row >= 0 && levels[index].items[pending_row].sub)
            {
                if (level_count <= index + 1 || levels[index + 1].parent_row != pending_row)
                    open_submenu( index, pending_row );
            }
            else if (pending_level == index) close_after( index );
        }
        return 0;
    case WM_KEYDOWN:
    {
        /* the keys go to the deepest level */
        int deepest = level_count - 1;
        struct level *level = &levels[deepest];

        keyboard = TRUE;
        switch (wparam)
        {
        case VK_UP: move_hot( deepest, -1 ); break;
        case VK_DOWN: move_hot( deepest, 1 ); break;
        case VK_RIGHT:
            if (level->hot >= 0 && level->items[level->hot].sub) open_submenu( deepest, level->hot );
            /* a submenu the pointer opened: the keys go into it */
            else if (level->hot < 0 && deepest) move_hot( deepest, 1 );
            break;
        case VK_LEFT:
        case VK_ESCAPE:
            if (deepest) close_after( deepest - 1 );
            else done = TRUE;
            break;
        case VK_RETURN:
        case VK_SPACE:
            if (level->hot >= 0) choose( deepest, level->hot );
            break;
        }
        return 0;
    }
    case WM_ACTIVATE:
        /* a click elsewhere: the menu is gone, as Windows' */
        if (LOWORD( wparam ) == WA_INACTIVE && level_of( (HWND)lparam ) < 0) done = TRUE;
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint( hwnd );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

static BOOL open_level( HWND owner, POINT pt, UINT align, const struct popup_item *items, UINT count, int parent_row )
{
    struct level *level = &levels[level_count];
    DWORD transition = ARCTIC_TRANSITION_DEFAULT;
    DWORD corner = style == POPUP_WIN11 ? ARCTIC_CORNER_RADIUS( 8 ) : 1 /* DWMWCP_DONOTROUND */;
    MONITORINFO info = { sizeof(info) };
    HDC hdc;
    HGDIOBJ old;
    int x, y;

    if (level_count == MAX_LEVELS) return FALSE;
    memset( level, 0, sizeof(*level) );
    level->items = items;
    level->count = count;
    level->hot = -1;
    level->parent_row = parent_row;

    /* as wide as its longest text, as high as its rows */
    hdc = GetDC( NULL );
    old = SelectObject( hdc, font );
    level->width = MIN_WIDTH;
    for (UINT i = 0; i < count; i++)
    {
        SIZE size = { 0 };
        if (!items[i].text) continue;
        GetTextExtentPoint32W( hdc, items[i].text, lstrlenW( items[i].text ), &size );
        level->width = max( level->width, TEXT_LEFT + size.cx + px( 24 ) + (items[i].sub ? CHEVRON_SIZE + px( 8 ) : 0) );
    }
    SelectObject( hdc, old );
    ReleaseDC( NULL, hdc );
    level->height = row_top( level, count ) + PADDING;

    /* where it was asked for, inside the monitor; a submenu that has no room on the right goes left */
    GetMonitorInfoW( MonitorFromPoint( pt, MONITOR_DEFAULTTONEAREST ), &info );
    x = (align & TPM_RIGHTALIGN) ? pt.x - level->width : (align & TPM_CENTERALIGN) ? pt.x - level->width / 2 : pt.x;
    y = (align & TPM_BOTTOMALIGN) ? pt.y - level->height : pt.y;
    if (parent_row >= 0 && x + level->width > info.rcWork.right)
    {
        RECT parent;
        GetWindowRect( owner, &parent );
        x = parent.left - level->width + px( 4 );
    }
    x = max( (int)info.rcWork.left, min( x, (int)info.rcWork.right - level->width ) );
    y = max( (int)info.rcWork.top, min( y, (int)info.rcWork.bottom - level->height ) );

    level->hwnd = CreateWindowExW( WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"ArcticStartPopup", L"", WS_POPUP,
                                   x, y, level->width, level->height, owner, NULL, startui_instance, NULL );
    if (!level->hwnd) return FALSE;
    level_count++;
    DwmSetWindowAttribute( level->hwnd, DWMWA_ARCTIC_TRANSITION, &transition, sizeof(transition) );
    DwmSetWindowAttribute( level->hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner) );
    look_apply( level->hwnd, &popup_look );
    ShowWindow( level->hwnd, SW_SHOWNOACTIVATE );
    return TRUE;
}

/* The items with their texts copied: those of load_string are reused by
 * whatever loads a string while the menu is up (the Start menu painting) */
#define MAX_COPIES 48

static struct popup_item copies[MAX_COPIES];
static WCHAR texts[MAX_COPIES][128];
static UINT copied;

static const struct popup_item *copy_items( const struct popup_item *items, UINT count )
{
    struct popup_item *copy;

    if (copied + count > MAX_COPIES) return NULL;
    copy = &copies[copied];
    copied += count;
    for (UINT i = 0; i < count; i++)
    {
        copy[i] = items[i];
        if (items[i].text)
        {
            lstrcpynW( texts[copy - copies + i], items[i].text, ARRAY_SIZE(texts[0]) );
            copy[i].text = texts[copy - copies + i];
        }
        if (items[i].sub && !(copy[i].sub = copy_items( items[i].sub, items[i].sub_count ))) copy[i].sub_count = 0;
    }
    return copy;
}

/* the menu at a point, until something is chosen (its id) or it is left (0);
 * POPUP_SYSTEM makes it a menu of USER's */
UINT popup_track( HWND owner, POINT pt, UINT align, const struct popup_item *items, UINT count, int popup_style,
                  const struct look *look )
{
    static BOOL registered;
    HWND first;
    MSG msg;

    if (popup_style == POPUP_SYSTEM)
    {
        HMENU menu = CreatePopupMenu();
        UINT cmd;

        for (UINT i = 0; i < count; i++)
        {
            if (!items[i].id && !items[i].sub) AppendMenuW( menu, MF_SEPARATOR, 0, NULL );
            else if (items[i].sub)
            {
                HMENU sub = CreatePopupMenu();
                for (UINT j = 0; j < items[i].sub_count; j++)
                    AppendMenuW( sub, MF_STRING | (items[i].sub[j].checked ? MF_CHECKED : 0) |
                                 (items[i].sub[j].disabled ? MF_GRAYED : 0), items[i].sub[j].id, items[i].sub[j].text );
                AppendMenuW( menu, MF_POPUP, (UINT_PTR)sub, items[i].text );
            }
            else AppendMenuW( menu, MF_STRING | (items[i].checked ? MF_CHECKED : 0) | (items[i].disabled ? MF_GRAYED : 0),
                              items[i].id, items[i].text );
        }
        cmd = TrackPopupMenuEx( menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | align, pt.x, pt.y, owner, NULL );
        DestroyMenu( menu );
        return cmd;
    }

    if (!registered)
    {
        WNDCLASSW cls = { 0 };
        cls.lpfnWndProc = popup_proc;
        cls.hInstance = startui_instance;
        cls.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_ARROW );
        cls.lpszClassName = L"ArcticStartPopup";
        RegisterClassW( &cls );
        registered = TRUE;
    }
    style = popup_style;
    popup_look = *look;
    /* over what is under it, a little more opaque than the menu */
    if (popup_look.effect != 1 /* ARCTIC_ACCENT_ENABLE_GRADIENT */) popup_look.opacity = max( popup_look.opacity, 90 );
    palette_init( &pal, look_is_light( &popup_look ) );
    if (!font) font = shell_font( px( 14 ), FW_NORMAL );
    result = 0;
    done = keyboard = FALSE;
    level_count = 0;
    copied = 0;
    if (!(items = copy_items( items, count ))) return 0;
    if (!open_level( owner, pt, align, items, count, -1 )) return 0;
    first = levels[0].hwnd;
    SetForegroundWindow( first );
    SetFocus( first );

    while (!done && GetMessageW( &msg, NULL, 0, 0 ))
    {
        /* the keys of the menus go to the popup */
        if (msg.message == WM_KEYDOWN && level_of( msg.hwnd ) < 0) msg.hwnd = first;
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }
    close_after( -1 );
    if (IsWindow( owner )) SetForegroundWindow( owner );
    return result;
}
