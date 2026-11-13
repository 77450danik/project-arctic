/*
 * Arctic WordPad: the File page
 *
 * Word's "backstage": over the ribbon and the pages, a list of places on
 * the left (Home, New, Open, Info, Save, Save As, Print, Close) and the
 * chosen one on the right.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdio.h>
#include <shlwapi.h>
#include "wordpad.h"

enum page { P_HOME, P_NEW, P_OPEN, P_INFO, P_SAVE, P_SAVEAS, P_PRINT, P_CLOSE, P_ABOUT, P_COUNT };

static const struct { UINT label; const char *icon; } items[P_COUNT] =
{
    { IDS_BS_HOME, "home" }, { IDS_BS_NEW, "newdoc" }, { IDS_BS_OPEN, "folder" }, { IDS_BS_INFO, "info" },
    { IDS_BS_SAVE, "save" }, { IDS_BS_SAVEAS, "saveas" }, { IDS_BS_PRINT, "print" }, { IDS_BS_CLOSE, "close" },
    { IDS_BS_ABOUT, NULL },
};

static HWND backstage;
static struct surface surf;
static int current = P_HOME, hot = -1, scroll, pressed = -1;
static BOOL shown;

enum zone_kind { Z_NONE, Z_BACK, Z_ITEM, Z_BLANK, Z_RECENT, Z_BROWSE, Z_PRINT, Z_PAGESETUP };
struct zone { int kind, index; RECT rc; };
static struct zone zones[64];
static int zone_count;
static WCHAR recent[RECENT_MAX][MAX_PATH];
static int recent_count;

#define NAV_W   px(220)

static void add_zone( int kind, int index, const RECT *rc )
{
    if (zone_count < ARRAY_SIZE(zones))
    {
        zones[zone_count].kind = kind;
        zones[zone_count].index = index;
        zones[zone_count].rc = *rc;
        zone_count++;
    }
}

static BOOL is_hot( int kind, int index )
{
    return hot >= 0 && zones[hot].kind == kind && zones[hot].index == index;
}

static void heading( RECT *client, int *y, const WCHAR *text )
{
    RECT r = { NAV_W + px( 48 ), *y, client->right - px( 24 ), *y + px( 52 ) };

    draw_text( &surf, text, -1, &r, get_font( F_HEAD ), th.text, DT_LEFT );
    *y += px( 68 );
}

static void subheading( RECT *client, int *y, const WCHAR *text )
{
    RECT r = { NAV_W + px( 48 ), *y, client->right - px( 24 ), *y + px( 28 ) };

    draw_text( &surf, text, -1, &r, get_font( F_SUBHEAD ), th.text, DT_LEFT );
    *y += px( 36 );
}

static void blank_tile( int x, int y )
{
    RECT tile = { x, y, x + px( 150 ), y + px( 196 ) }, paper, label;

    add_zone( Z_BLANK, 0, &tile );
    if (is_hot( Z_BLANK, 0 )) fill_round( &surf, &tile, (float)px( 6 ), th.hover, 255 );
    SetRect( &paper, x + px( 12 ), y + px( 10 ), x + px( 138 ), y + px( 168 ) );
    fill( &surf, &paper, RGB( 255, 255, 255 ) );
    outline_round( &surf, &paper, 0, is_hot( Z_BLANK, 0 ) ? th.accent : th.page_line, 255 );
    SetRect( &label, x, paper.bottom + px( 4 ), tile.right, tile.bottom );
    draw_text( &surf, string( IDS_BLANK_DOCUMENT ), -1, &label, get_font( F_UI ), th.text, DT_CENTER );
}

static void recent_list( RECT *client, int *y )
{
    int x = NAV_W + px( 40 ), right = client->right - px( 32 );

    recent_count = recent_files( recent );
    if (!recent_count)
    {
        RECT r = { x + px( 8 ), *y, right, *y + px( 24 ) };
        draw_text( &surf, string( IDS_NO_RECENT ), -1, &r, get_font( F_UI ), th.dim, DT_LEFT );
        *y += px( 32 );
        return;
    }
    for (int i = 0; i < recent_count; i++)
    {
        RECT row = { x, *y, right, *y + px( 48 ) }, t;
        const WCHAR *base = wcsrchr( recent[i], '\\' );
        WCHAR folder[MAX_PATH];
        WIN32_FILE_ATTRIBUTE_DATA attr;

        add_zone( Z_RECENT, i, &row );
        if (is_hot( Z_RECENT, i )) fill_round( &surf, &row, (float)px( 4 ), th.hover, 255 );
        draw_icon( &surf, "document", row.left + px( 12 ), row.top + px( 12 ), px( 24 ), IS_NORMAL );
        SetRect( &t, row.left + px( 48 ), row.top + px( 6 ), row.right - px( 160 ), row.top + px( 26 ) );
        draw_text( &surf, base ? base + 1 : recent[i], -1, &t, get_font( F_UI ), th.text, DT_LEFT );
        lstrcpynW( folder, recent[i], MAX_PATH );
        if (base) folder[base - recent[i]] = 0;
        SetRect( &t, row.left + px( 48 ), row.top + px( 24 ), row.right - px( 160 ), row.top + px( 42 ) );
        draw_text( &surf, folder, -1, &t, get_font( F_SMALL ), th.dim, DT_LEFT | DT_PATH_ELLIPSIS );
        if (GetFileAttributesExW( recent[i], GetFileExInfoStandard, &attr ))
        {
            SYSTEMTIME st, local;
            WCHAR date[64];

            FileTimeToSystemTime( &attr.ftLastWriteTime, &st );
            SystemTimeToTzSpecificLocalTime( NULL, &st, &local );
            GetDateFormatEx( LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, NULL, date, ARRAY_SIZE(date), NULL );
            SetRect( &t, row.right - px( 150 ), row.top, row.right - px( 12 ), row.bottom );
            draw_text( &surf, date, -1, &t, get_font( F_SMALL ), th.dim, DT_RIGHT );
        }
        *y += px( 50 );
    }
}

static void big_button( int kind, const char *icon, UINT label, int x, int y )
{
    RECT r = { x, y, x + px( 120 ), y + px( 104 ) }, t;

    add_zone( kind, 0, &r );
    fill_round( &surf, &r, (float)px( 6 ), is_hot( kind, 0 ) ? th.hover : th.card, 255 );
    outline_round( &surf, &r, (float)px( 6 ), is_hot( kind, 0 ) ? th.checked_line : th.card_line, 255 );
    draw_icon( &surf, icon, x + px( 40 ), y + px( 18 ), px( 40 ), IS_NORMAL );
    SetRect( &t, x, y + px( 66 ), r.right, y + px( 92 ) );
    draw_text( &surf, string( label ), -1, &t, get_font( F_UI ), th.text, DT_CENTER );
}

static void info_line( RECT *client, int *y, UINT label, const WCHAR *value )
{
    RECT l = { NAV_W + px( 48 ), *y, NAV_W + px( 220 ), *y + px( 24 ) };
    RECT v = { l.right, *y, client->right - px( 32 ), *y + px( 24 ) };

    draw_text( &surf, string( label ), -1, &l, get_font( F_UI ), th.dim, DT_LEFT );
    draw_text( &surf, value, -1, &v, get_font( F_UI ), th.text, DT_LEFT | DT_PATH_ELLIPSIS );
    *y += px( 30 );
}

static void paint( HDC hdc )
{
    RECT client, r;
    int y;

    GetClientRect( backstage, &client );
    if (!surface_size( &surf, client.right, client.bottom )) return;
    zone_count = 0;
    fill( &surf, &client, th.card );

    /* the places */
    SetRect( &r, 0, 0, NAV_W, client.bottom );
    fill( &surf, &r, th.chrome );
    SetRect( &r, px( 8 ), px( 12 ), NAV_W - px( 8 ), px( 52 ) );
    add_zone( Z_BACK, 0, &r );
    if (is_hot( Z_BACK, 0 )) fill_round( &surf, &r, (float)px( 4 ), th.dark ? th.hover : RGB( 0xe3, 0xe6, 0xea ), 255 );
    draw_icon( &surf, "back", r.left + px( 10 ), r.top + px( 8 ), px( 24 ), IS_NORMAL );
    y = px( 64 );
    for (int i = 0; i < P_COUNT; i++)
    {
        if (i == P_ABOUT)
        {
            SetRect( &r, px( 8 ), client.bottom - px( 52 ), NAV_W - px( 8 ), client.bottom - px( 12 ) );
        }
        else
        {
            if (i == P_INFO)
            {
                RECT sep = { px( 20 ), y + px( 6 ), NAV_W - px( 20 ), y + px( 7 ) };
                fill( &surf, &sep, th.card_line );
                y += px( 14 );
            }
            SetRect( &r, px( 8 ), y, NAV_W - px( 8 ), y + px( 40 ) );
            y += px( 42 );
        }
        add_zone( Z_ITEM, i, &r );
        if (i == current) fill_round( &surf, &r, (float)px( 4 ), th.dark ? th.hover : RGB( 0xe1, 0xe5, 0xea ), 255 );
        else if (is_hot( Z_ITEM, i )) fill_round( &surf, &r, (float)px( 4 ), th.dark ? th.pressed : RGB( 0xe8, 0xeb, 0xef ), 255 );
        if (items[i].icon) draw_icon( &surf, items[i].icon, r.left + px( 12 ), r.top + px( 12 ), px( 16 ), IS_NORMAL );
        r.left += px( 40 );
        draw_text( &surf, string( items[i].label ), -1, &r, get_font( i == current ? F_UI_SEMI : F_UI ), th.text, DT_LEFT );
    }

    /* the page */
    y = px( 32 ) - scroll;
    switch (current)
    {
    case P_HOME:
    {
        SYSTEMTIME now;
        GetLocalTime( &now );
        heading( &client, &y, string( now.wHour < 12 && now.wHour >= 5 ? IDS_GOOD_MORNING : now.wHour < 18 && now.wHour >= 5 ? IDS_GOOD_DAY : IDS_GOOD_EVENING ) );
        blank_tile( NAV_W + px( 40 ), y );
        y += px( 220 );
        subheading( &client, &y, string( IDS_RECENT ) );
        recent_list( &client, &y );
        break;
    }
    case P_NEW:
        heading( &client, &y, string( IDS_BS_NEW ) );
        blank_tile( NAV_W + px( 40 ), y );
        break;
    case P_OPEN:
        heading( &client, &y, string( IDS_BS_OPEN ) );
        big_button( Z_BROWSE, "folder", IDS_BROWSE, NAV_W + px( 48 ), y );
        y += px( 128 );
        subheading( &client, &y, string( IDS_RECENT ) );
        recent_list( &client, &y );
        break;
    case P_INFO:
    {
        WCHAR value[MAX_PATH + 64];
        const WCHAR *path = current_path();
        WIN32_FILE_ATTRIBUTE_DATA attr;

        heading( &client, &y, string( IDS_BS_INFO ) );
        subheading( &client, &y, document_name() );
        info_line( &client, &y, IDS_INFO_LOCATION, path[0] ? path : string( IDS_NOT_SAVED ) );
        if (path[0] && GetFileAttributesExW( path, GetFileExInfoStandard, &attr ))
        {
            SYSTEMTIME st, local;
            WCHAR date[64], time[64];

            StrFormatByteSizeW( ((ULONGLONG)attr.nFileSizeHigh << 32) | attr.nFileSizeLow, value, ARRAY_SIZE(value) );
            info_line( &client, &y, IDS_INFO_SIZE, value );
            FileTimeToSystemTime( &attr.ftLastWriteTime, &st );
            SystemTimeToTzSpecificLocalTime( NULL, &st, &local );
            GetDateFormatEx( LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, &local, NULL, date, ARRAY_SIZE(date), NULL );
            GetTimeFormatEx( LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &local, NULL, time, ARRAY_SIZE(time) );
            swprintf( value, ARRAY_SIZE(value), L"%s, %s", date, time );
            info_line( &client, &y, IDS_INFO_MODIFIED, value );
        }
        swprintf( value, ARRAY_SIZE(value), string( IDS_INFO_STATS ), view_page_count(), word_count() );
        info_line( &client, &y, IDS_PAGES, value );
        break;
    }
    case P_PRINT:
    {
        WCHAR value[128];
        heading( &client, &y, string( IDS_BS_PRINT ) );
        big_button( Z_PRINT, "print", IDS_BS_PRINT, NAV_W + px( 48 ), y );
        big_button( Z_PAGESETUP, "margins", IDS_CMD( IDM_PAGESETUP ), NAV_W + px( 184 ), y );
        y += px( 128 );
        swprintf( value, ARRAY_SIZE(value), string( IDS_INFO_STATS ), view_page_count(), word_count() );
        info_line( &client, &y, IDS_PAGES, value );
        break;
    }
    }
    GdiFlush();
    BitBlt( hdc, 0, 0, client.right, client.bottom, surf.dc, 0, 0, SRCCOPY );
}

static int zone_at( POINT pt )
{
    for (int i = zone_count - 1; i >= 0; i--)
        if (PtInRect( &zones[i].rc, pt )) return i;
    return -1;
}

static void activate( int z )
{
    switch (zones[z].kind)
    {
    case Z_BACK:
        show_backstage( FALSE, 0 );
        break;
    case Z_ITEM:
        switch (zones[z].index)
        {
        case P_SAVE:
            if (save_document( FALSE )) show_backstage( FALSE, 0 );
            return;
        case P_SAVEAS:
            if (save_document( TRUE )) show_backstage( FALSE, 0 );
            return;
        case P_CLOSE:
            if (ask_save_changes()) new_document();
            return;
        case P_ABOUT:
            dialog_about( frame );
            return;
        }
        current = zones[z].index;
        scroll = 0;
        InvalidateRect( backstage, NULL, FALSE );
        break;
    case Z_BLANK:
        if (ask_save_changes()) new_document();
        break;
    case Z_RECENT:
        if (zones[z].index < recent_count && ask_save_changes()) open_file( recent[zones[z].index] );
        break;
    case Z_BROWSE:
        command( IDM_OPEN );
        break;
    case Z_PRINT:
        show_backstage( FALSE, 0 );
        command( IDM_PRINT );
        break;
    case Z_PAGESETUP:
        command( IDM_PAGESETUP );
        break;
    }
}

static LRESULT CALLBACK backstage_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    POINT pt = { GET_X( lparam ), GET_Y( lparam ) };

    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint( hwnd, &ps );
        paint( hdc );
        EndPaint( hwnd, &ps );
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE:
    {
        int z = zone_at( pt );
        TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, hwnd };

        if (z != hot)
        {
            hot = z;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        TrackMouseEvent( &track );
        return 0;
    }
    case WM_MOUSELEAVE:
        hot = -1;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    case WM_LBUTTONDOWN:
        pressed = zone_at( pt );
        return 0;
    case WM_LBUTTONUP:
    {
        /* the press that opened the page was on the File tab: its release
         * here is no click */
        int z = zone_at( pt );
        if (z >= 0 && z == pressed) activate( z );
        pressed = -1;
        return 0;
    }
    case WM_MOUSEWHEEL:
        scroll = max( 0, scroll - GET_WHEEL_DELTA_WPARAM( wparam ) * px( 48 ) / WHEEL_DELTA );
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) show_backstage( FALSE, 0 );
        return 0;
    case WM_SIZE:
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

void show_backstage( BOOL show, int page_index )
{
    if (!backstage && show)
    {
        WNDCLASSW cls = { 0, backstage_proc, 0, 0, instance, NULL, NULL, NULL, NULL, L"ArcticWordPadBackstage" };

        cls.hCursor = LoadCursorW( NULL, (LPCWSTR)IDC_ARROW );
        RegisterClassW( &cls );
        backstage = CreateWindowExW( 0, cls.lpszClassName, NULL, WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 10, 10, frame, NULL,
                                     instance, NULL );
    }
    if (!backstage) return;
    shown = show;
    if (show)
    {
        current = page_index;
        scroll = 0;
        hot = -1;
        layout_frame();
        SetWindowPos( backstage, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW );
        InvalidateRect( backstage, NULL, FALSE );
        SetFocus( backstage );
    }
    else
    {
        ShowWindow( backstage, SW_HIDE );
        view_focus();
    }
}

BOOL backstage_shown(void) { return shown; }

void backstage_place( int x, int y, int width, int height )
{
    if (backstage) SetWindowPos( backstage, HWND_TOP, x, y, width, height, SWP_NOACTIVATE | (shown ? 0 : SWP_NOZORDER) );
}

void backstage_theme_changed(void)
{
    if (backstage) InvalidateRect( backstage, NULL, FALSE );
}
