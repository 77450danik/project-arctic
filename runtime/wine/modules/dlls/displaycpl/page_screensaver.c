/*
 * Display of the Control Panel: the page Screen saver
 *
 * desk.cpl's Screen Saver page as a page: the screen saver at work in small
 * (its /p in a window of the page), the ones in System32 and Windows (their
 * string 1 is their name), its own settings (/c) and a look at it (/s), the
 * minutes before it comes, the sign-in screen after it, and Power Options.
 * Each change is at once: HKCU\Control Panel\Desktop's SCRNSAVE.EXE and
 * ScreenSaverIsSecure, SPI_SETSCREENSAVEACTIVE and SPI_SETSCREENSAVETIMEOUT,
 * as desk.cpl wrote them.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdio.h>

#include "displaycpl.h"

#define DESKTOP_KEY     L"Control Panel\\Desktop"
#define PREVIEW_CLASS   L"ArcticSaverPreview"

#define ID_PREVIEW_BOX  100
#define ID_SAVER        101
#define ID_SETTINGS     102
#define ID_SHOW         103
#define ID_WAIT         104
#define ID_SECURE       105

#define ID_DO_SAVER     200
#define ID_DO_WAIT      201

#define MAX_SAVERS      64

static const UINT waits[] = { 1, 2, 3, 5, 10, 15, 20, 25, 30, 45, 60, 120, 180, 240, 300 };

static BOOL desktop_string( const WCHAR *name, WCHAR *value, DWORD count )
{
    DWORD size = count * sizeof(WCHAR);

    value[0] = 0;
    return !RegGetValueW( HKEY_CURRENT_USER, DESKTOP_KEY, name, RRF_RT_REG_SZ, NULL, value,
                          &size ) && value[0];
}

static void add_saver( struct display_state *s, const WCHAR *path, const WCHAR *file )
{
    struct saver *saver;
    HMODULE module;

    if (s->saver_count >= MAX_SAVERS) return;
    for (UINT i = 0; i < s->saver_count; i++) if (!wcsicmp( s->savers[i].path, path )) return;
    saver = &s->savers[s->saver_count];
    lstrcpynW( saver->path, path, ARRAY_SIZE(saver->path) );
    saver->name[0] = 0;
    if ((module = LoadLibraryExW( path, NULL, LOAD_LIBRARY_AS_DATAFILE | DONT_RESOLVE_DLL_REFERENCES )))
    {
        LoadStringW( module, 1, saver->name, ARRAY_SIZE(saver->name) );
        FreeLibrary( module );
    }
    /* without a name: the file's, without .scr */
    if (!saver->name[0])
    {
        WCHAR *dot;

        lstrcpynW( saver->name, file, ARRAY_SIZE(saver->name) );
        if ((dot = wcsrchr( saver->name, '.' ))) *dot = 0;
    }
    s->saver_count++;
}

static void search( struct display_state *s, const WCHAR *dir )
{
    WCHAR mask[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW data;
    HANDLE find;

    swprintf( mask, ARRAY_SIZE(mask), L"%s\\*.scr", dir );
    if ((find = FindFirstFileW( mask, &data )) == INVALID_HANDLE_VALUE) return;
    do
    {
        /* desk.cpl leaves out the hidden ones */
        if (data.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_DIRECTORY)) continue;
        swprintf( path, ARRAY_SIZE(path), L"%s\\%s", dir, data.cFileName );
        add_saver( s, path, data.cFileName );
    } while (FindNextFileW( find, &data ));
    FindClose( find );
}

static void list_savers( struct display_state *s )
{
    WCHAR dir[MAX_PATH], current[MAX_PATH];

    if (!s->savers && !(s->savers = calloc( MAX_SAVERS, sizeof(*s->savers) ))) return;
    s->saver_count = 0;
    GetSystemDirectoryW( dir, ARRAY_SIZE(dir) );
    search( s, dir );
    GetWindowsDirectoryW( dir, ARRAY_SIZE(dir) );
    search( s, dir );
    /* the one set, wherever it is */
    if (desktop_string( L"SCRNSAVE.EXE", current, ARRAY_SIZE(current) ) &&
        GetFileAttributesW( current ) != INVALID_FILE_ATTRIBUTES)
        add_saver( s, current, wcsrchr( current, '\\' ) ? wcsrchr( current, '\\' ) + 1 : current );
}

/* the one set, -1 for none */
static int current_saver( struct display_state *s )
{
    WCHAR current[MAX_PATH];
    BOOL active = FALSE;

    SystemParametersInfoW( SPI_GETSCREENSAVEACTIVE, 0, &active, 0 );
    if (!desktop_string( L"SCRNSAVE.EXE", current, ARRAY_SIZE(current) ) || !active) return -1;
    for (UINT i = 0; i < s->saver_count; i++) if (!wcsicmp( s->savers[i].path, current )) return i;
    return -1;
}

/* a screen saver run with its arguments, its window under ours */
static HANDLE run_saver( const WCHAR *path, const WCHAR *args )
{
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    WCHAR cmdline[MAX_PATH + 64];

    swprintf( cmdline, ARRAY_SIZE(cmdline), L"\"%s\" %s", path, args );
    if (!CreateProcessW( NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi )) return NULL;
    CloseHandle( pi.hThread );
    return pi.hProcess;
}

static void stop_preview( struct display_state *s )
{
    if (!s->saver_preview) return;
    /* desk.cpl ends it so: a screen saver may not notice its window going */
    TerminateProcess( s->saver_preview, 0 );
    CloseHandle( s->saver_preview );
    s->saver_preview = NULL;
}

static LRESULT WINAPI preview_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    if (msg == WM_ERASEBKGND)
    {
        RECT client;

        GetClientRect( hwnd, &client );
        FillRect( (HDC)wp, &client, GetStockObject( BLACK_BRUSH ) );
        return 1;
    }
    return DefWindowProcW( hwnd, msg, wp, lp );
}

static void format_wait( UINT minutes, WCHAR *text, UINT count )
{
    if (minutes >= 60 && !(minutes % 60)) lstrcpynW( text, format_string( IDS_HOURS, minutes / 60 ), count );
    else lstrcpynW( text, format_string( IDS_MINUTES, minutes ), count );
}

void screensaver_build( struct view *view )
{
    struct display_state *s = display_state( view );
    int pre_h = px( 150 ), pre_w = preview_width( pre_h ), frame = px( 10 ), chosen;
    WCHAR name[256], text[64], secure[8];
    UINT timeout = 0, minutes;
    HWND control;

    if (!s) return;
    stop_preview( s );
    list_savers( s );
    chosen = current_saver( s );
    nav_display( view, PAGE_SCREENSAVER );
    view_heading( view, load_string( IDS_SCREENSAVER ) );

    /* the screen saver at work, or the desktop without one */
    view_card_area( view, pre_h + frame, NULL, 0 );
    if (chosen >= 0)
    {
        static BOOL registered;
        HWND box;

        if (!registered)
        {
            WNDCLASSW cls = { 0 };

            cls.lpfnWndProc = preview_proc;
            cls.hInstance = cp_instance;
            cls.lpszClassName = PREVIEW_CLASS;
            registered = RegisterClassW( &cls ) != 0;
        }
        box = view_card_control( view, PREVIEW_CLASS, L"", WS_CLIPCHILDREN, px( 5 ), px( 5 ), pre_w, pre_h, ID_PREVIEW_BOX );
        swprintf( text, ARRAY_SIZE(text), L"/p %Iu", (ULONG_PTR)box );
        if (box) s->saver_preview = run_saver( s->savers[chosen].path, text );
    }
    else
    {
        HWND tile = view_tile( view, pre_w + frame, pre_h + frame, NULL, ID_PREVIEW_BOX );
        int tw, th;

        preview_init( &s->preview, pre_w, pre_h );
        tile_set_paint( tile, preview_paint, (UINT_PTR)&s->preview );
        EnableWindow( tile, FALSE );
        preview_thumb_size( &s->preview, &tw, &th );
        if (s->preview.picture && tw) picture_to_tile( tile, s->preview.path, tw, th, FALSE );
    }

    control = card_combo( view, ICON_SCREENSAVER, IDS_SAVER, 0, ID_SAVER );
    combo_add( control, load_string( IDS_SAVER_NONE ), -1, chosen < 0 );
    for (UINT i = 0; i < s->saver_count; i++) combo_add( control, s->savers[i].name, i, (int)i == chosen );

    lstrcpynW( name, load_string( IDS_SAVER_SETTINGS ), ARRAY_SIZE(name) );
    view_card( view, view_icon( ICON_SETTINGS ), name, NULL, NULL, NULL, 0, 0, 0, 0 );
    control = view_card_extra( view, L"BUTTON", load_string( IDS_SAVER_PREVIEW ), BS_PUSHBUTTON | WS_TABSTOP, px( 130 ),
                               px( 28 ), ID_SHOW );
    EnableWindow( control, chosen >= 0 );
    control = view_card_extra( view, L"BUTTON", load_string( IDS_SAVER_OPTIONS ), BS_PUSHBUTTON | WS_TABSTOP, px( 130 ),
                               px( 28 ), ID_SETTINGS );
    EnableWindow( control, chosen >= 0 );

    SystemParametersInfoW( SPI_GETSCREENSAVETIMEOUT, 0, &timeout, 0 );
    minutes = max( 1, timeout / 60 );
    control = card_combo( view, ICON_WAIT, IDS_WAIT, IDS_WAIT_HINT, ID_WAIT );
    {
        BOOL listed = FALSE;

        for (UINT i = 0; i < ARRAY_SIZE(waits); i++)
        {
            /* a time set elsewhere, in its place among them */
            if (!listed && minutes < waits[i])
            {
                format_wait( minutes, text, ARRAY_SIZE(text) );
                combo_add( control, text, minutes, TRUE );
                listed = TRUE;
            }
            format_wait( waits[i], text, ARRAY_SIZE(text) );
            combo_add( control, text, waits[i], waits[i] == minutes );
            if (waits[i] == minutes) listed = TRUE;
        }
        if (!listed)
        {
            format_wait( minutes, text, ARRAY_SIZE(text) );
            combo_add( control, text, minutes, TRUE );
        }
    }
    EnableWindow( control, chosen >= 0 );

    desktop_string( L"ScreenSaverIsSecure", secure, ARRAY_SIZE(secure) );
    lstrcpynW( name, load_string( IDS_SECURE ), ARRAY_SIZE(name) );
    control = view_card_toggle( view, view_icon( ICON_SECURE ), name, load_string( IDS_SECURE_HINT ), secure[0] == '1',
                                ID_SECURE );
    EnableWindow( control, chosen >= 0 );

    view_section( view, load_string( IDS_RELATED ) );
    lstrcpynW( name, load_string( IDS_POWER ), ARRAY_SIZE(name) );
    view_card_link( view, view_icon( ICON_POWER ), name, load_string( IDS_POWER_HINT ), go_item, (UINT_PTR)ITEM_POWER );
}

static void set_saver( struct display_state *s, int index )
{
    HKEY key;

    if (RegCreateKeyExW( HKEY_CURRENT_USER, DESKTOP_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL )) return;
    if (index >= 0 && (UINT)index < s->saver_count)
    {
        const WCHAR *path = s->savers[index].path;

        RegSetValueExW( key, L"SCRNSAVE.EXE", 0, REG_SZ, (const BYTE *)path, (lstrlenW( path ) + 1) * sizeof(WCHAR) );
        RegCloseKey( key );
        SystemParametersInfoW( SPI_SETSCREENSAVEACTIVE, TRUE, NULL, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE );
        return;
    }
    /* Windows deletes it when there is none */
    RegDeleteValueW( key, L"SCRNSAVE.EXE" );
    RegCloseKey( key );
    SystemParametersInfoW( SPI_SETSCREENSAVEACTIVE, FALSE, NULL, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE );
}

BOOL screensaver_command( struct view *view, UINT id, UINT code, HWND control )
{
    struct display_state *s = display_state( view );
    HWND content = view_window( view );
    int chosen;

    if (!s) return FALSE;
    switch (id)
    {
    case ID_SAVER:
        if (code == CBN_SELCHANGE) defer_command( view, ID_DO_SAVER );
        return TRUE;
    case ID_WAIT:
        if (code == CBN_SELCHANGE) defer_command( view, ID_DO_WAIT );
        return TRUE;
    case ID_SETTINGS:
        if (code == BN_CLICKED && (chosen = current_saver( s )) >= 0)
        {
            WCHAR args[32];
            HANDLE process;

            swprintf( args, ARRAY_SIZE(args), L"/c:%Iu", (ULONG_PTR)GetAncestor( content, GA_ROOT ) );
            if ((process = run_saver( s->savers[chosen].path, args ))) CloseHandle( process );
        }
        return TRUE;
    case ID_SHOW:
        if (code == BN_CLICKED && (chosen = current_saver( s )) >= 0)
        {
            HANDLE process;

            /* the small one rests while the large one shows */
            stop_preview( s );
            if ((process = run_saver( s->savers[chosen].path, L"/s" )))
            {
                WaitForInputIdle( process, 2000 );
                CloseHandle( process );
            }
        }
        return TRUE;
    case ID_SECURE:
        if (code == BN_CLICKED)
        {
            const WCHAR *value = SendMessageW( control, BM_GETCHECK, 0, 0 ) == BST_CHECKED ? L"1" : L"0";

            RegSetKeyValueW( HKEY_CURRENT_USER, DESKTOP_KEY, L"ScreenSaverIsSecure", REG_SZ, value, 2 * sizeof(WCHAR) );
        }
        return TRUE;

    case ID_DO_SAVER:
    {
        LPARAM index = combo_data( GetDlgItem( content, ID_SAVER ) );

        set_saver( s, (int)index );
        view_rebuild( view );
        return TRUE;
    }
    case ID_DO_WAIT:
    {
        LPARAM minutes = combo_data( GetDlgItem( content, ID_WAIT ) );

        if (minutes > 0)
            SystemParametersInfoW( SPI_SETSCREENSAVETIMEOUT, (UINT)minutes * 60, NULL, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE );
        return TRUE;
    }
    }
    return FALSE;
}
