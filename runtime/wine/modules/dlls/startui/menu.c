/*
 * The Start menu of Arctic
 *
 * The two columns of Windows 7's Start menu in the dark acrylic of Windows
 * 10/11 (docs/start-menu.md), 460 x 530 at 100%, its corners rounded by 8:
 *
 * - the left column, 255 wide: the pinned programs, a separator and the most
 *   used ones, 38 a row with their 32 icons; under a separator All programs
 *   (32 high), which turns the column into the tree of the Programs folders
 *   (26 a row, 16 icons) and itself into Back; at the bottom the search box,
 *   240 x 26, white, rounded by 3. Typing anywhere searches: the column then
 *   shows the programs and the settings found, and Enter starts the first.
 * - the right column, 205 wide, 36 a row of 20 glyphs in groups: the user, Documents,
 *   Pictures, Music, Downloads; Recent documents, This PC; Control Panel,
 *   Settings, Update; Run. The user's picture stands over the top edge
 *   (avatar.c). At the bottom Shut down, its chevron a menu with Restart.
 *
 * Under the pointer #FFFFFF at 10%, pressed 20%; the menu rises from the
 * taskbar as dwm.exe slides the Start menu. It lives on the taskbar's thread
 * of explorer, which asks for it through ArcticStartMenuToggle and hears
 * that it closed through the message "ArcticStartMenuClosed".
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdlib.h>

#include "startui.h"
#include "shellapi.h"
#include "shlobj.h"
#include "knownfolders.h"
#include "dwmapi.h"
#include "winternl.h"
#include "powrprof.h"
#include "wine/arctic_dwm.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(startui);

#define MENU_WIDTH      px(460)   /* 420 on the picture; Ukrainian names need the room */
#define MENU_HEIGHT     px(530)
#define LEFT_WIDTH      px(255)
#define PAD             px(8)
#define ROW_LARGE       px(38)
#define ROW_SMALL       px(26)
#define ROW_ALL         px(32)
#define ROW_RIGHT       px(36)
#define GROUP_GAP       px(8)
#define RIGHT_TOP       px(48)
#define ICON_LARGE_SIZE      px(32)
#define ICON_SMALL_SIZE      px(16)
#define GLYPH           px(20)
#define CHEVRON         px(12)
#define SEARCH_WIDTH    px(240)
#define SEARCH_HEIGHT   px(26)
#define BOTTOM_MARGIN   px(12)
#define POWER_HEIGHT    px(30)
#define POWER_CHEVRON   px(28)
#define INDENT          px(16)
#define MAX_HOME        16
#define MAX_SEARCH      127

#define HOVER_ALPHA     0x1a
#define PRESSED_ALPHA   0x33
#define LINE_ALPHA      0x26

#define TIMER_CARET     1

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

#define IDHK_RUN        0x1f4   /* explorer's hot key for Win+R, which starts Run on a thread of its own */

BOOL WINAPI SetWindowCompositionAttribute( HWND hwnd, void *data );

enum mode { MODE_HOME, MODE_ALL, MODE_SEARCH };

enum target_kind { T_NONE, T_LEFT, T_ALL, T_SEARCH, T_RIGHT, T_POWER, T_POWER_MENU };

struct target
{
    enum target_kind kind;
    int index;
};

static HWND menu, tray;
static RECT button;
static UINT closed_message;
static DWORD hidden_at;
static enum mode mode;
static WCHAR search[MAX_SEARCH + 1];
static struct item *home[MAX_HOME];
static UINT home_count, pinned_count;
static struct item **list;       /* All programs or the search results */
static UINT list_count;
static int scroll;               /* rows of the list scrolled away */
static struct target hot, pressed, focus;
static BOOL focus_shown, caret_on, in_popup;
static HFONT font_item, font_bold, font_header;
static WCHAR user_name[128];
static struct start_settings settings;
static int right_visible[R_COUNT];   /* the places shown, in order */
static int right_count;

/**********************************************************************
 *          Layout
 */

static int home_row(void)
{
    return settings.large_icons ? ROW_LARGE : px( 28 );
}

static int left_row(void)
{
    return mode == MODE_HOME ? home_row() : ROW_SMALL;
}

static int left_count(void)
{
    return mode == MODE_HOME ? (int)home_count : (int)list_count;
}

static struct item *left_item( int index )
{
    if (index < 0 || index >= left_count()) return NULL;
    return mode == MODE_HOME ? home[index] : list[index];
}

static RECT search_rect(void)
{
    RECT rect;
    SetRect( &rect, PAD, MENU_HEIGHT - BOTTOM_MARGIN - SEARCH_HEIGHT, PAD + SEARCH_WIDTH, MENU_HEIGHT - BOTTOM_MARGIN );
    return rect;
}

static RECT all_rect(void)
{
    RECT rect, search_box = search_rect();
    SetRect( &rect, px( 4 ), search_box.top - BOTTOM_MARGIN - ROW_ALL, LEFT_WIDTH - px( 4 ), search_box.top - BOTTOM_MARGIN );
    return rect;
}

/* where the rows of the left column go */
static RECT list_area(void)
{
    RECT rect, all = all_rect();
    SetRect( &rect, 0, PAD, LEFT_WIDTH, all.top - px( 6 ) );
    return rect;
}

static int visible_rows(void)
{
    RECT area = list_area();
    return (area.bottom - area.top) / left_row();
}

/* the separator between the pinned and the most used takes a few pixels */
static int home_y( int index )
{
    int y = list_area().top + index * home_row();
    if (pinned_count && (UINT)index >= pinned_count) y += px( 9 );
    return y;
}

static BOOL left_item_rect( int index, RECT *rect )
{
    RECT area = list_area();
    int y;

    if (index < 0 || index >= left_count()) return FALSE;
    if (mode == MODE_HOME) y = home_y( index );
    else
    {
        if (index < scroll || index >= scroll + visible_rows()) return FALSE;
        y = area.top + (index - scroll) * ROW_SMALL;
    }
    if (y + left_row() > area.bottom) return FALSE;
    SetRect( rect, px( 4 ), y, LEFT_WIDTH - px( 4 ), y + left_row() );
    return TRUE;
}

static RECT right_rect( int index )
{
    RECT rect;
    int y = RIGHT_TOP;

    for (int i = 0; i < index; i++)
        y += ROW_RIGHT + (right_group( right_visible[i] ) != right_group( right_visible[i + 1] ) ? GROUP_GAP : 0);
    SetRect( &rect, LEFT_WIDTH + px( 4 ), y, MENU_WIDTH - px( 8 ), y + ROW_RIGHT );
    return rect;
}

static RECT power_rect( BOOL chevron )
{
    RECT rect, search_box = search_rect();
    int top = (search_box.top + search_box.bottom - POWER_HEIGHT) / 2;

    if (chevron) SetRect( &rect, MENU_WIDTH - px( 8 ) - POWER_CHEVRON, top, MENU_WIDTH - px( 8 ), top + POWER_HEIGHT );
    else SetRect( &rect, LEFT_WIDTH + px( 4 ), top, MENU_WIDTH - px( 8 ) - POWER_CHEVRON, top + POWER_HEIGHT );
    return rect;
}

static struct target hit_test( POINT pt )
{
    struct target t = { T_NONE, 0 };
    RECT rect;

    for (int i = 0; i < left_count(); i++)
    {
        if (left_item_rect( i, &rect ) && PtInRect( &rect, pt ))
        {
            if (left_item( i )->kind == ITEM_HEADER) return t;
            t.kind = T_LEFT;
            t.index = i;
            return t;
        }
    }
    if (rect = all_rect(), PtInRect( &rect, pt )) t.kind = T_ALL;
    else if (rect = search_rect(), PtInRect( &rect, pt )) t.kind = T_SEARCH;
    else if (rect = power_rect( FALSE ), PtInRect( &rect, pt )) t.kind = T_POWER;
    else if (rect = power_rect( TRUE ), PtInRect( &rect, pt )) t.kind = T_POWER_MENU;
    else
    {
        for (int i = 0; i < right_count; i++)
        {
            rect = right_rect( i );
            if (PtInRect( &rect, pt ))
            {
                t.kind = T_RIGHT;
                t.index = i;
                break;
            }
        }
    }
    return t;
}

static BOOL same( struct target a, struct target b )
{
    return a.kind == b.kind && ((a.kind != T_LEFT && a.kind != T_RIGHT) || a.index == b.index);
}

/**********************************************************************
 *          Content
 */

static void load_home(void)
{
    struct item **found;
    UINT count;

    count = items_pinned( &found );
    home_count = pinned_count = min( count, MAX_HOME );
    memcpy( home, found, home_count * sizeof(*home) );
    count = items_frequent( &found, min( settings.recent, MAX_HOME - home_count ) );
    for (UINT i = 0; i < count && home_count < MAX_HOME; i++)
    {
        RECT rect;

        home[home_count++] = found[i];
        /* as many as there is room for */
        if (!left_item_rect( home_count - 1, &rect ))
        {
            home_count--;
            break;
        }
    }
}

static void set_mode( enum mode new_mode )
{
    mode = new_mode;
    scroll = 0;
    hot.kind = pressed.kind = T_NONE;
    if (mode == MODE_HOME)
    {
        search[0] = 0;
        load_home();
    }
    else if (mode == MODE_ALL) list_count = items_all_programs( &list );
    else list_count = items_search( search, &list );
}

/* the first result that can be started, for Enter while searching */
static int first_result(void)
{
    for (int i = 0; i < (int)list_count; i++) if (list[i]->kind != ITEM_HEADER) return i;
    return -1;
}

static void update_search(void)
{
    if (!search[0])
    {
        set_mode( MODE_HOME );
        focus.kind = T_NONE;
    }
    else
    {
        set_mode( MODE_SEARCH );
        focus.kind = T_LEFT;
        focus.index = first_result();
        if (focus.index < 0) focus.kind = T_NONE;
    }
    InvalidateRect( menu, NULL, FALSE );
}

/**********************************************************************
 *          Painting
 */

static void draw_text( HDC hdc, const WCHAR *text, RECT *rect, HFONT font, COLORREF color, UINT format )
{
    HGDIOBJ old = SelectObject( hdc, font );

    SetTextColor( hdc, color );
    DrawTextW( hdc, text, -1, rect, format | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS );
    SelectObject( hdc, old );
}

static void highlight( HDC hdc, struct target t, const RECT *rect )
{
    BOOL is_hot = same( t, hot ), is_pressed = is_hot && same( t, pressed );

    /* the result Enter starts is lit, as in Windows 7 */
    if (!is_hot && mode == MODE_SEARCH && !focus_shown && same( t, focus )) is_hot = TRUE;

    if (is_hot || is_pressed) fill_round_rect( hdc, rect, (float)px( 4 ), RGB( 255, 255, 255 ),
                                               is_pressed ? PRESSED_ALPHA : HOVER_ALPHA );
    if (focus_shown && same( t, focus ))
    {
        HBRUSH brush = CreateSolidBrush( RGB( 255, 255, 255 ) );
        FrameRect( hdc, rect, brush );
        DeleteObject( brush );
    }
}

static void paint_left( HDC hdc )
{
    RECT rect, area = list_area();

    for (int i = 0; i < left_count(); i++)
    {
        struct item *item = left_item( i );
        struct target t = { T_LEFT, i };
        RECT text;
        int indent;

        if (!left_item_rect( i, &rect )) continue;
        if (item->kind == ITEM_HEADER)
        {
            text = rect;
            text.left += PAD;
            draw_text( hdc, item->name, &text, font_header, RGB( 0xa0, 0xa0, 0xa0 ), DT_LEFT );
            continue;
        }
        highlight( hdc, t, &rect );
        if (mode == MODE_HOME)
        {
            int size = settings.large_icons ? ICON_LARGE_SIZE : ICON_SMALL_SIZE;

            draw_icon_bitmap( hdc, items_icon( item, settings.large_icons ), rect.left + px( 4 ),
                              (rect.top + rect.bottom - size) / 2, size );
            SetRect( &text, rect.left + px( 4 ) + size + PAD, rect.top, rect.right - PAD, rect.bottom );
        }
        else
        {
            indent = rect.left + px( 4 ) + (mode == MODE_ALL ? item->depth * INDENT : 0);
            draw_icon_bitmap( hdc, items_icon( item, FALSE ), indent, (rect.top + rect.bottom - ICON_SMALL_SIZE) / 2, ICON_SMALL_SIZE );
            SetRect( &text, indent + ICON_SMALL_SIZE + PAD, rect.top, rect.right - PAD, rect.bottom );
        }
        draw_text( hdc, item->name, &text, font_item, RGB( 255, 255, 255 ), DT_LEFT );
    }

    if (mode == MODE_HOME && pinned_count && home_count > pinned_count)
    {
        int y = home_y( pinned_count ) - px( 5 );
        SetRect( &rect, PAD, y, LEFT_WIDTH - PAD, y + 1 );
        fill_alpha( hdc, &rect, RGB( 255, 255, 255 ), LINE_ALPHA );
    }
    if (mode == MODE_SEARCH && !left_count())
    {
        rect = area;
        rect.left += PAD;
        rect.bottom = rect.top + ROW_SMALL;
        draw_text( hdc, load_string( IDS_NO_RESULTS ), &rect, font_item, RGB( 0xa0, 0xa0, 0xa0 ), DT_LEFT );
    }
    /* a thin scroll bar when the list goes on */
    if (mode != MODE_HOME && left_count() > visible_rows())
    {
        int height = area.bottom - area.top, thumb = max( px( 20 ), height * visible_rows() / left_count() );
        int top = area.top + (height - thumb) * scroll / max( 1, left_count() - visible_rows() );

        SetRect( &rect, LEFT_WIDTH - px( 4 ), top, LEFT_WIDTH - px( 2 ), top + thumb );
        fill_alpha( hdc, &rect, RGB( 255, 255, 255 ), 0x66 );
    }
}

static void paint_all_row( HDC hdc )
{
    RECT rect = all_rect(), text, line;
    struct target t = { T_ALL, 0 };
    BOOL back = mode != MODE_HOME;

    SetRect( &line, PAD, rect.top - px( 6 ), LEFT_WIDTH - PAD, rect.top - px( 5 ) );
    fill_alpha( hdc, &line, RGB( 255, 255, 255 ), LINE_ALPHA );
    highlight( hdc, t, &rect );
    if (back)
    {
        draw_glyph( hdc, GLYPH_CHEVRON_LEFT, rect.left + PAD, (rect.top + rect.bottom - CHEVRON) / 2, CHEVRON,
                    RGB( 255, 255, 255 ), 255 );
        SetRect( &text, rect.left + PAD + CHEVRON + PAD, rect.top, rect.right - PAD, rect.bottom );
        draw_text( hdc, load_string( IDS_BACK ), &text, font_item, RGB( 255, 255, 255 ), DT_LEFT );
    }
    else
    {
        SetRect( &text, rect.left + PAD, rect.top, rect.right - PAD - CHEVRON - PAD, rect.bottom );
        draw_text( hdc, load_string( IDS_ALL_PROGRAMS ), &text, font_item, RGB( 255, 255, 255 ), DT_LEFT );
        draw_glyph( hdc, GLYPH_CHEVRON_RIGHT, rect.right - PAD - CHEVRON, (rect.top + rect.bottom - CHEVRON) / 2,
                    CHEVRON, RGB( 255, 255, 255 ), 255 );
    }
}

static void paint_search( HDC hdc )
{
    RECT box = search_rect(), text, inner;
    int glyph = px( 14 );

    fill_round_rect( hdc, &box, (float)px( 3 ), RGB( 255, 255, 255 ), 255 );
    draw_glyph( hdc, GLYPH_SEARCH, box.left + px( 7 ), (box.top + box.bottom - glyph) / 2, glyph,
                RGB( 0x60, 0x60, 0x60 ), 255 );
    SetRect( &text, box.left + px( 7 ) + glyph + px( 6 ), box.top, box.right - px( 6 ), box.bottom );
    if (search[0])
    {
        HGDIOBJ old = SelectObject( hdc, font_item );
        SIZE size;
        RECT caret;

        draw_text( hdc, search, &text, font_item, RGB( 0, 0, 0 ), DT_LEFT );
        GetTextExtentPoint32W( hdc, search, lstrlenW( search ), &size );
        SelectObject( hdc, old );
        if (caret_on && text.left + size.cx < text.right)
        {
            SetRect( &caret, text.left + size.cx + 1, box.top + px( 5 ), text.left + size.cx + 2, box.bottom - px( 5 ) );
            FillRect( hdc, &caret, GetStockObject( BLACK_BRUSH ) );
        }
    }
    else draw_text( hdc, load_string( IDS_SEARCH_HINT ), &text, font_item, RGB( 0x6d, 0x6d, 0x6d ), DT_LEFT );
    /* the text and the black caret are opaque on the white box, not see-through */
    SetRect( &inner, box.left + px( 3 ), box.top + 1, box.right - px( 3 ), box.bottom - 1 );
    make_opaque( hdc, &inner );
}

static void paint_right( HDC hdc )
{
    for (int i = 0; i < right_count; i++)
    {
        int action = right_visible[i];
        RECT rect = right_rect( i ), text;
        struct target t = { T_RIGHT, i };
        const WCHAR *name = action == R_USER ? user_name : right_name( action );

        highlight( hdc, t, &rect );
        draw_glyph( hdc, right_glyph( action ), rect.left + PAD, (rect.top + rect.bottom - GLYPH) / 2, GLYPH,
                    RGB( 255, 255, 255 ), 255 );
        SetRect( &text, rect.left + PAD + GLYPH + PAD, rect.top, rect.right - PAD, rect.bottom );
        /* a place shown as a menu has its chevron */
        if (settings.show[action] == SHOW_MENU)
        {
            text.right -= CHEVRON + px( 4 );
            draw_glyph( hdc, GLYPH_CHEVRON_RIGHT, rect.right - PAD - CHEVRON, (rect.top + rect.bottom - CHEVRON) / 2,
                        CHEVRON, RGB( 255, 255, 255 ), 255 );
        }
        draw_text( hdc, name, &text, action == R_USER ? font_bold : font_item, RGB( 255, 255, 255 ), DT_LEFT );
    }
}

static void paint_power( HDC hdc )
{
    RECT main = power_rect( FALSE ), chevron = power_rect( TRUE ), text, line, all;
    struct target t_main = { T_POWER, 0 }, t_chevron = { T_POWER_MENU, 0 };
    int glyph = px( 16 );

    UnionRect( &all, &main, &chevron );
    fill_round_rect( hdc, &all, (float)px( 4 ), RGB( 255, 255, 255 ), 0x14 );
    highlight( hdc, t_main, &main );
    highlight( hdc, t_chevron, &chevron );
    draw_glyph( hdc, GLYPH_POWER, main.left + PAD, (main.top + main.bottom - glyph) / 2, glyph, RGB( 255, 255, 255 ), 255 );
    SetRect( &text, main.left + PAD + glyph + PAD, main.top, main.right - px( 4 ), main.bottom );
    draw_text( hdc, load_string( IDS_SHUT_DOWN ), &text, font_item, RGB( 255, 255, 255 ), DT_LEFT );
    SetRect( &line, chevron.left, chevron.top + px( 6 ), chevron.left + 1, chevron.bottom - px( 6 ) );
    fill_alpha( hdc, &line, RGB( 255, 255, 255 ), LINE_ALPHA );
    draw_glyph( hdc, GLYPH_CHEVRON_RIGHT, (chevron.left + chevron.right - CHEVRON) / 2,
                (chevron.top + chevron.bottom - CHEVRON) / 2, CHEVRON, RGB( 255, 255, 255 ), 255 );
}

static void on_paint( HWND hwnd )
{
    PAINTSTRUCT ps;
    RECT client;
    HDC hdc = BeginPaint( hwnd, &ps ), mem;
    BITMAPINFO info = { { sizeof(info.bmiHeader), 0, 0, 1, 32, BI_RGB } };
    HBITMAP bitmap;
    void *bits;

    GetClientRect( hwnd, &client );
    info.bmiHeader.biWidth = client.right;
    info.bmiHeader.biHeight = -client.bottom;
    mem = CreateCompatibleDC( hdc );
    if ((bitmap = CreateDIBSection( hdc, &info, DIB_RGB_COLORS, &bits, NULL, 0 )))
    {
        HGDIOBJ old = SelectObject( mem, bitmap );

        /* black is the acrylic seen through */
        FillRect( mem, &client, GetStockObject( BLACK_BRUSH ) );
        SetBkMode( mem, TRANSPARENT );
        paint_left( mem );
        paint_all_row( mem );
        paint_search( mem );
        paint_right( mem );
        paint_power( mem );
        BitBlt( hdc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY );
        SelectObject( mem, old );
        DeleteObject( bitmap );
    }
    DeleteDC( mem );
    EndPaint( hwnd, &ps );
}

/**********************************************************************
 *          Acting
 */

static void open_folder_id( int csidl )
{
    SHELLEXECUTEINFOW info = { sizeof(info) };
    LPITEMIDLIST pidl;

    if (FAILED(SHGetSpecialFolderLocation( NULL, csidl, &pidl ))) return;
    info.fMask = SEE_MASK_IDLIST | SEE_MASK_FLAG_NO_UI;
    info.lpIDList = pidl;
    info.nShow = SW_SHOWNORMAL;
    ShellExecuteExW( &info );
    CoTaskMemFree( pidl );
}

static void open_path( const WCHAR *path )
{
    ShellExecuteW( NULL, L"open", path, NULL, NULL, SW_SHOWNORMAL );
}

static void open_downloads(void)
{
    WCHAR *path, fallback[MAX_PATH];

    if (SUCCEEDED(SHGetKnownFolderPath( &FOLDERID_Downloads, 0, NULL, &path )))
    {
        open_path( path );
        CoTaskMemFree( path );
        return;
    }
    if (SUCCEEDED(SHGetFolderPathW( NULL, CSIDL_PROFILE, NULL, SHGFP_TYPE_CURRENT, fallback )))
    {
        lstrcatW( fallback, L"\\Downloads" );
        open_path( fallback );
    }
}

static BOOL update_ready(void)
{
    WCHAR path[MAX_PATH];

    GetSystemDirectoryW( path, ARRAY_SIZE(path) );
    lstrcatW( path, L"\\Host\\Update\\ready" );
    return GetFileAttributesW( path ) != INVALID_FILE_ATTRIBUTES;
}

/* Power Options' "Show in Power menu" (FlyoutMenuSettings, as Windows keeps them) */
static BOOL flyout_shows( const WCHAR *name, DWORD fallback )
{
    DWORD value = fallback, size = sizeof(value);

    RegGetValueW( HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FlyoutMenuSettings", name,
                  RRF_RT_REG_DWORD, NULL, &value, &size );
    return value != 0;
}

/* "Режим глибокого сну", once msconfig turned hibernation on (HibernateEnabled,
 * as Windows keeps it; docs/hibernation.md) and Power Options show it */
static BOOL hibernate_enabled(void)
{
    DWORD value = 0, size = sizeof(value);

    return !RegGetValueW( HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Power", L"HibernateEnabled",
                          RRF_RT_REG_DWORD, NULL, &value, &size ) && value && flyout_shows( L"ShowHibernateOption", 1 );
}

/* "Сон" where the machine can sleep (docs/power.md) */
static BOOL sleep_enabled(void)
{
    return IsPwrSuspendAllowed() && flyout_shows( L"ShowSleepOption", 1 );
}

static DWORD WINAPI sleep_thread( void *arg )
{
    SetSuspendState( FALSE, FALSE, FALSE );
    return 0;
}

/* The machine sleeps inside SetSuspendState and the call returns when it
 * wakes: on a thread of its own, the menu keeps answering meanwhile */
static DWORD WINAPI hibernate_thread( void *arg )
{
    if (!SetSuspendState( TRUE, FALSE, FALSE ) && GetLastError() == ERROR_BUSY)
        MessageBoxW( NULL, load_string( IDS_HIBERNATE_REFUSED ), load_string( IDS_HIBERNATE ), MB_ICONWARNING );
    return 0;
}

static void place_menu( int index );

static void do_right( int index )
{
    if (index >= 0 && index < right_count && settings.show[right_visible[index]] == SHOW_MENU)
    {
        place_menu( index );
        return;
    }
    if (index >= 0 && index < right_count) index = right_visible[index];
    menu_hide();
    switch (index)
    {
    case R_USER:          open_folder_id( CSIDL_PROFILE ); break;
    case R_DOCUMENTS:     open_folder_id( CSIDL_PERSONAL ); break;
    case R_PICTURES:      open_folder_id( CSIDL_MYPICTURES ); break;
    case R_MUSIC:         open_folder_id( CSIDL_MYMUSIC ); break;
    case R_DOWNLOADS:     open_downloads(); break;
    case R_RECENT:        open_folder_id( CSIDL_RECENT ); break;
    case R_PC:            open_folder_id( CSIDL_DRIVES ); break;
    case R_CONTROL_PANEL:
    case R_SETTINGS:      open_folder_id( CSIDL_CONTROLS ); break;
    case R_UPDATE:        open_path( L"https://github.com/77450danik/project-arctic/releases" ); break;
    case R_RUN:           PostMessageW( tray, WM_HOTKEY, IDHK_RUN, 0 ); break;
    }
}

/* a place of the right column shown as a menu: what is in the folder (the
 * newest first in Recent documents), the drives of This PC, the settings of
 * the Control Panel; choosing opens it */
#define MAX_PLACE_ENTRIES 40

struct place_entry
{
    WCHAR path[MAX_PATH];
    WCHAR name[MAX_PATH];
    struct item *item;
    BOOL folder;
    FILETIME time;
};

static struct place_entry place_entries[MAX_PLACE_ENTRIES];
static UINT place_count;

static int __cdecl compare_entries( const void *a, const void *b )
{
    const struct place_entry *x = a, *y = b;

    if (x->folder != y->folder) return x->folder ? -1 : 1;
    return CompareStringW( LOCALE_USER_DEFAULT, NORM_IGNORECASE, x->name, -1, y->name, -1 ) - CSTR_EQUAL;
}

static int __cdecl compare_newest( const void *a, const void *b )
{
    const struct place_entry *x = a, *y = b;
    return -CompareFileTime( &x->time, &y->time );
}

static void read_place_folder( const WCHAR *dir, BOOL recent )
{
    WCHAR pattern[MAX_PATH];
    WIN32_FIND_DATAW data;
    HANDLE find;

    swprintf( pattern, ARRAY_SIZE(pattern), L"%s\\*", dir );
    if ((find = FindFirstFileW( pattern, &data )) == INVALID_HANDLE_VALUE) return;
    do
    {
        struct place_entry *entry;
        WCHAR *dot;

        if (!wcscmp( data.cFileName, L"." ) || !wcscmp( data.cFileName, L".." )) continue;
        if (data.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        if (!wcsicmp( data.cFileName, L"desktop.ini" )) continue;
        if (place_count == MAX_PLACE_ENTRIES && !recent) break;
        if (place_count == MAX_PLACE_ENTRIES)
        {
            /* the oldest makes room */
            qsort( place_entries, place_count, sizeof(*place_entries), compare_newest );
            if (CompareFileTime( &data.ftLastWriteTime, &place_entries[place_count - 1].time ) <= 0) continue;
            place_count--;
        }
        entry = &place_entries[place_count++];
        memset( entry, 0, sizeof(*entry) );
        swprintf( entry->path, MAX_PATH, L"%s\\%s", dir, data.cFileName );
        lstrcpynW( entry->name, data.cFileName, MAX_PATH );
        if (!wcsicmp( (dot = wcsrchr( entry->name, '.' )) ? dot : L"", L".lnk" )) *dot = 0;
        entry->folder = !!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        entry->time = data.ftLastWriteTime;
    } while (FindNextFileW( find, &data ));
    FindClose( find );
    if (recent) qsort( place_entries, place_count, sizeof(*place_entries), compare_newest );
    else qsort( place_entries, place_count, sizeof(*place_entries), compare_entries );
}

static void read_place( int action )
{
    static const int folders[] = { CSIDL_PROFILE, CSIDL_PERSONAL, CSIDL_MYPICTURES, CSIDL_MYMUSIC };
    WCHAR dir[MAX_PATH], *path;

    place_count = 0;
    switch (action)
    {
    case R_USER: case R_DOCUMENTS: case R_PICTURES: case R_MUSIC:
        if (SUCCEEDED(SHGetFolderPathW( NULL, folders[action], NULL, SHGFP_TYPE_CURRENT, dir )))
            read_place_folder( dir, FALSE );
        break;
    case R_DOWNLOADS:
        if (SUCCEEDED(SHGetKnownFolderPath( &FOLDERID_Downloads, 0, NULL, &path )))
        {
            read_place_folder( path, FALSE );
            CoTaskMemFree( path );
        }
        break;
    case R_RECENT:
        if (SUCCEEDED(SHGetFolderPathW( NULL, CSIDL_RECENT, NULL, SHGFP_TYPE_CURRENT, dir )))
            read_place_folder( dir, TRUE );
        if (place_count > 15) place_count = 15;
        break;
    case R_PC:
    {
        WCHAR drives[256];
        DWORD len = GetLogicalDriveStringsW( ARRAY_SIZE(drives), drives );

        for (WCHAR *d = drives; len && len < ARRAY_SIZE(drives) && *d && place_count < MAX_PLACE_ENTRIES;
             d += lstrlenW( d ) + 1)
        {
            struct place_entry *entry = &place_entries[place_count++];
            SHFILEINFOW info;

            memset( entry, 0, sizeof(*entry) );
            lstrcpynW( entry->path, d, MAX_PATH );
            if (SHGetFileInfoW( d, 0, &info, sizeof(info), SHGFI_DISPLAYNAME ))
                lstrcpynW( entry->name, info.szDisplayName, MAX_PATH );
            else lstrcpynW( entry->name, d, MAX_PATH );
            entry->folder = TRUE;
        }
        break;
    }
    case R_CONTROL_PANEL:
    {
        struct item **applets;
        UINT count = items_applets( &applets );

        for (UINT i = 0; i < count && place_count < MAX_PLACE_ENTRIES; i++)
        {
            struct place_entry *entry = &place_entries[place_count++];

            memset( entry, 0, sizeof(*entry) );
            lstrcpynW( entry->name, applets[i]->name, MAX_PATH );
            entry->item = applets[i];
        }
        break;
    }
    }
}

static void place_menu( int index )
{
    int action = right_visible[index];
    RECT rect = right_rect( index );
    POINT pt = { rect.right, rect.top };
    HBITMAP bitmaps[MAX_PLACE_ENTRIES] = { 0 };
    HMENU popup = CreatePopupMenu();
    UINT cmd;

    read_place( action );
    for (UINT i = 0; i < place_count; i++)
    {
        MENUITEMINFOW info = { sizeof(info) };
        SHFILEINFOW file;
        HICON icon = NULL;

        if (place_entries[i].item) bitmaps[i] = items_icon( place_entries[i].item, FALSE );
        else if (SHGetFileInfoW( place_entries[i].path, 0, &file, sizeof(file), SHGFI_ICON | SHGFI_SMALLICON ))
        {
            icon = file.hIcon;
            bitmaps[i] = icon_bitmap( icon, px( 16 ) );
            DestroyIcon( icon );
        }
        info.fMask = MIIM_ID | MIIM_STRING | MIIM_BITMAP;
        info.wID = 100 + i;
        info.dwTypeData = place_entries[i].name;
        info.hbmpItem = bitmaps[i];
        InsertMenuItemW( popup, i, TRUE, &info );
    }
    if (!place_count) AppendMenuW( popup, MF_STRING | MF_GRAYED, 0, load_string( IDS_EMPTY ) );

    ClientToScreen( menu, &pt );
    in_popup = TRUE;
    cmd = TrackPopupMenuEx( popup, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, menu, NULL );
    in_popup = FALSE;
    DestroyMenu( popup );
    for (UINT i = 0; i < place_count; i++)
        if (bitmaps[i] && !place_entries[i].item) DeleteObject( bitmaps[i] );
    if (cmd < 100 || cmd >= 100 + place_count) return;

    menu_hide();
    if (place_entries[cmd - 100].item) items_launch( place_entries[cmd - 100].item );
    else open_path( place_entries[cmd - 100].path );
}

static void power_menu(void)
{
    RECT rect = power_rect( TRUE );
    HMENU popup = CreatePopupMenu();
    POINT pt = { rect.right, rect.top };
    UINT cmd;

    /* Windows 10's order: Sleep, Hibernate, Shut down, Restart */
    if (sleep_enabled()) AppendMenuW( popup, MF_STRING, 4, load_string( IDS_SLEEP ) );
    if (hibernate_enabled()) AppendMenuW( popup, MF_STRING, 3, load_string( IDS_HIBERNATE ) );
    AppendMenuW( popup, MF_STRING, 2, load_string( IDS_SHUT_DOWN ) );
    AppendMenuW( popup, MF_STRING, 1, load_string( update_ready() ? IDS_UPDATE_AND_RESTART : IDS_RESTART ) );
    ClientToScreen( menu, &pt );
    in_popup = TRUE;
    cmd = TrackPopupMenuEx( popup, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_BOTTOMALIGN, pt.x, pt.y,
                            menu, NULL );
    in_popup = FALSE;
    DestroyMenu( popup );
    if (!cmd) return;
    menu_hide();
    /* winlogon ends the session, arctic-init restarts and the next start
     * installs a waiting update */
    if (cmd == 3 || cmd == 4)
    {
        HANDLE thread = CreateThread( NULL, 0, cmd == 3 ? hibernate_thread : sleep_thread, NULL, 0, NULL );
        if (thread) CloseHandle( thread );
    }
    else if (cmd == 1) ExitWindowsEx( EWX_REBOOT, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED );
    else ExitWindowsEx( EWX_SHUTDOWN | EWX_POWEROFF, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED );
}

static void activate( struct target t )
{
    struct item *item;

    switch (t.kind)
    {
    case T_LEFT:
        if (!(item = left_item( t.index ))) break;
        if (item->kind == ITEM_FOLDER)
        {
            item->expanded = !item->expanded;
            list_count = items_all_programs( &list );
            InvalidateRect( menu, NULL, FALSE );
        }
        else if (item->kind == ITEM_PROGRAM)
        {
            menu_hide();
            items_launch( item );
        }
        break;
    case T_ALL:
        set_mode( mode == MODE_HOME ? MODE_ALL : MODE_HOME );
        focus.kind = T_NONE;
        InvalidateRect( menu, NULL, FALSE );
        break;
    case T_RIGHT:
        do_right( t.index );
        break;
    case T_POWER:
        menu_hide();
        ExitWindowsEx( EWX_SHUTDOWN | EWX_POWEROFF, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED );
        break;
    case T_POWER_MENU:
        power_menu();
        break;
    default:
        break;
    }
}

static void context_menu( int index, POINT pt )
{
    struct item *item = left_item( index );
    BOOL pinned, frequent;
    HMENU popup;
    UINT cmd;

    if (!item || item->kind != ITEM_PROGRAM) return;
    pinned = items_is_pinned( item );
    frequent = mode == MODE_HOME && (UINT)index >= pinned_count;
    popup = CreatePopupMenu();
    AppendMenuW( popup, MF_STRING, 1, load_string( IDS_OPEN ) );
    SetMenuDefaultItem( popup, 1, FALSE );
    if (!item->args[0]) AppendMenuW( popup, MF_STRING, 4, load_string( IDS_OPEN_LOCATION ) );
    AppendMenuW( popup, MF_SEPARATOR, 0, NULL );
    AppendMenuW( popup, MF_STRING, 2, load_string( pinned ? IDS_UNPIN : IDS_PIN ) );
    if (frequent) AppendMenuW( popup, MF_STRING, 3, load_string( IDS_REMOVE_FROM_LIST ) );
    ClientToScreen( menu, &pt );
    in_popup = TRUE;
    cmd = TrackPopupMenuEx( popup, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, menu, NULL );
    in_popup = FALSE;
    DestroyMenu( popup );
    switch (cmd)
    {
    case 1:
        menu_hide();
        items_launch( item );
        return;
    case 2:
        items_pin( item, !pinned );
        break;
    case 3:
        items_forget( item );
        break;
    case 4:
        menu_hide();
        items_open_location( item );
        return;
    default:
        return;
    }
    if (mode == MODE_HOME) load_home();
    InvalidateRect( menu, NULL, FALSE );
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

static void scroll_by( int rows )
{
    int max_scroll = max( 0, left_count() - visible_rows() );
    int new_scroll = max( 0, min( scroll + rows, max_scroll ) );

    if (mode == MODE_HOME || new_scroll == scroll) return;
    scroll = new_scroll;
    InvalidateRect( menu, NULL, FALSE );
}

/* keep the focused row of a long list in view */
static void reveal( int index )
{
    if (mode == MODE_HOME) return;
    if (index < scroll) scroll = index;
    else if (index >= scroll + visible_rows()) scroll = index - visible_rows() + 1;
}

static void move_focus( int delta )
{
    focus_shown = TRUE;
    if (focus.kind == T_NONE)
    {
        focus.kind = left_count() ? T_LEFT : T_RIGHT;
        focus.index = 0;
        if (focus.kind == T_LEFT && left_item( 0 )->kind == ITEM_HEADER) focus.index = first_result();
        if (focus.kind == T_LEFT && focus.index < 0) focus.kind = T_ALL;
    }
    else if (focus.kind == T_LEFT || focus.kind == T_ALL)
    {
        int index = focus.kind == T_ALL ? left_count() : focus.index;

        do index += delta; while (index >= 0 && index < left_count() && left_item( index )->kind == ITEM_HEADER);
        if (index < 0) index = focus.kind == T_ALL ? left_count() - 1 : focus.index;
        if (index >= left_count())
        {
            focus.kind = T_ALL;
            focus.index = 0;
        }
        else
        {
            focus.kind = T_LEFT;
            focus.index = index;
            reveal( index );
        }
    }
    else
    {
        int index = focus.kind == T_RIGHT ? focus.index : right_count;

        index = max( 0, min( index + delta, right_count ) );
        if (index == right_count) focus.kind = T_POWER;
        else
        {
            focus.kind = T_RIGHT;
            focus.index = index;
        }
    }
    InvalidateRect( menu, NULL, FALSE );
}

static void on_key( WPARAM key )
{
    struct item *item;

    switch (key)
    {
    case VK_ESCAPE:
        if (mode == MODE_HOME) menu_hide();
        else
        {
            set_mode( MODE_HOME );
            focus.kind = T_NONE;
            InvalidateRect( menu, NULL, FALSE );
        }
        break;
    case VK_UP:
        move_focus( -1 );
        break;
    case VK_DOWN:
        move_focus( 1 );
        break;
    case VK_RIGHT:
    case VK_LEFT:
        if (mode == MODE_ALL && focus.kind == T_LEFT && (item = left_item( focus.index )) &&
            item->kind == ITEM_FOLDER && item->expanded != (key == VK_RIGHT))
        {
            activate( focus );
            break;
        }
        if (mode == MODE_SEARCH) break;   /* the caret's keys, as in the search box */
        focus_shown = TRUE;
        if (key == VK_RIGHT && (focus.kind == T_LEFT || focus.kind == T_ALL || focus.kind == T_NONE))
        {
            focus.kind = right_count ? T_RIGHT : T_POWER;
            focus.index = 0;
        }
        else if (key == VK_RIGHT && focus.kind == T_RIGHT && settings.show[right_visible[focus.index]] == SHOW_MENU)
        {
            place_menu( focus.index );
            break;
        }
        else if (key == VK_LEFT && (focus.kind == T_RIGHT || focus.kind == T_POWER || focus.kind == T_POWER_MENU))
        {
            focus.kind = left_count() ? T_LEFT : T_ALL;
            focus.index = 0;
        }
        else if (key == VK_RIGHT && focus.kind == T_POWER) focus.kind = T_POWER_MENU;
        InvalidateRect( menu, NULL, FALSE );
        break;
    case VK_RETURN:
        if (focus.kind != T_NONE) activate( focus );
        break;
    case VK_BACK:
        if (search[0])
        {
            search[lstrlenW( search ) - 1] = 0;
            update_search();
        }
        break;
    case VK_PRIOR:
        scroll_by( -visible_rows() );
        break;
    case VK_NEXT:
        scroll_by( visible_rows() );
        break;
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
        if (hot.kind == T_NONE)
        {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent( &tme );
        }
        set_hot( hit_test( pt ) );
        return 0;

    case WM_MOUSELEAVE:
        hot.kind = pressed.kind = T_NONE;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;

    case WM_LBUTTONDOWN:
        pressed = hit_test( pt );
        focus_shown = FALSE;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;

    case WM_LBUTTONUP:
        t = hit_test( pt );
        if (same( t, pressed ) && t.kind != T_NONE && t.kind != T_SEARCH)
        {
            pressed.kind = T_NONE;
            activate( t );
        }
        pressed.kind = T_NONE;
        if (menu_visible()) InvalidateRect( hwnd, NULL, FALSE );
        return 0;

    case WM_RBUTTONUP:
        t = hit_test( pt );
        if (t.kind == T_LEFT) context_menu( t.index, pt );
        return 0;

    case WM_MOUSEWHEEL:
        scroll_by( -(short)HIWORD( wparam ) / WHEEL_DELTA * 3 );
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
            RECT box = search_rect();
            caret_on = !caret_on;
            InvalidateRect( hwnd, &box, FALSE );
        }
        return 0;

    case WM_ACTIVATE:
        if (LOWORD( wparam ) == WA_INACTIVE && !in_popup && (HWND)lparam != avatar_window()) menu_hide();
        return 0;

    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        on_paint( hwnd );
        return 0;

    case WM_APP:   /* the avatar was clicked */
        menu_hide();
        open_folder_id( CSIDL_PROFILE );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

static BOOL create_menu(void)
{
    struct arctic_accent_policy policy = { ARCTIC_ACCENT_ENABLE_ACRYLICBLURBEHIND, 0, ACRYLIC_TINT, 0 };
    struct { DWORD attrib; void *data; SIZE_T size; } attr = { 19 /* WCA_ACCENT_POLICY */, &policy, sizeof(policy) };
    DWORD transition = ARCTIC_TRANSITION_SLIDE_UP, corner = 2 /* DWMWCP_ROUND */;
    DWORD size = ARRAY_SIZE(user_name);
    WNDCLASSW cls = { 0 };

    if (menu) return TRUE;
    cls.lpfnWndProc = menu_proc;
    cls.hInstance = startui_instance;
    cls.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_ARROW );
    cls.lpszClassName = L"ArcticStartMenu";
    RegisterClassW( &cls );
    menu = CreateWindowExW( WS_EX_TOOLWINDOW | WS_EX_TOPMOST, cls.lpszClassName, L"", WS_POPUP,
                            0, 0, MENU_WIDTH, MENU_HEIGHT, NULL, NULL, startui_instance, NULL );
    if (!menu) return FALSE;
    SetWindowCompositionAttribute( menu, &attr );
    DwmSetWindowAttribute( menu, DWMWA_ARCTIC_TRANSITION, &transition, sizeof(transition) );
    DwmSetWindowAttribute( menu, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner) );
    font_item = shell_font( px( 14 ), FW_NORMAL );
    font_bold = shell_font( px( 14 ), FW_SEMIBOLD );
    font_header = shell_font( px( 13 ), FW_SEMIBOLD );
    closed_message = RegisterWindowMessageW( L"ArcticStartMenuClosed" );
    if (!GetUserNameW( user_name, &size )) lstrcpyW( user_name, L"User" );
    return TRUE;
}

/**********************************************************************
 *          Showing it
 */

BOOL menu_visible(void)
{
    return menu && IsWindowVisible( menu );
}

void menu_hide(void)
{
    if (!menu_visible()) return;
    KillTimer( menu, TIMER_CARET );
    avatar_hide();
    ShowWindow( menu, SW_HIDE );
    hidden_at = GetTickCount();
    if (tray && closed_message) PostMessageW( tray, closed_message, 0, 0 );
}

/* over the Start button, on the taskbar's edge, inside the work area */
static void place(void)
{
    MONITORINFO info = { sizeof(info) };
    int x, y;

    GetMonitorInfoW( MonitorFromRect( &button, MONITOR_DEFAULTTONEAREST ), &info );
    x = max( info.rcWork.left, min( (int)button.left, (int)info.rcWork.right - MENU_WIDTH ) );
    if ((button.top + button.bottom) / 2 > (info.rcWork.top + info.rcWork.bottom) / 2)
        y = info.rcWork.bottom - MENU_HEIGHT;
    else
        y = info.rcWork.top;
    if (button.right <= info.rcWork.left || button.left >= info.rcWork.right)
    {
        /* a taskbar on the left or the right: the menu starts at the top of the button */
        y = max( info.rcWork.top, min( (int)button.top, (int)info.rcWork.bottom - MENU_HEIGHT ) );
    }
    SetWindowPos( menu, HWND_TOPMOST, x, y, MENU_WIDTH, MENU_HEIGHT, SWP_NOACTIVATE );
}

void menu_toggle( HWND tray_window, const RECT *button_rect, BOOL click )
{
    RECT rect;

    if (!create_menu()) return;
    if (menu_visible())
    {
        menu_hide();
        return;
    }
    /* the click on Start that took the focus away and closed it */
    if (click && GetTickCount() - hidden_at < 300) return;

    tray = tray_window;
    if (button_rect) button = *button_rect;
    else SetRect( &button, 0, GetSystemMetrics( SM_CYSCREEN ) - 1, 1, GetSystemMetrics( SM_CYSCREEN ) );
    items_reload();
    settings_load( &settings );
    right_count = 0;
    for (int i = 0; i < R_COUNT; i++) if (settings.show[i] != SHOW_HIDDEN) right_visible[right_count++] = i;
    search[0] = 0;
    focus.kind = hot.kind = pressed.kind = T_NONE;
    focus_shown = FALSE;
    set_mode( MODE_HOME );
    place();
    /* the shell's look, as chosen on the taskbar's page Оформлення */
    ArcticApplyShellLook( menu );
    InvalidateRect( menu, NULL, FALSE );
    ShowWindow( menu, SW_SHOW );
    SetForegroundWindow( menu );
    SetFocus( menu );
    SetTimer( menu, TIMER_CARET, 530, NULL );
    GetWindowRect( menu, &rect );
    avatar_show( menu, rect.left + LEFT_WIDTH + (MENU_WIDTH - LEFT_WIDTH) / 2, rect.top + px( 4 ) );
}
