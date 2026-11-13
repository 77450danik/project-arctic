/*
 * Display of the Control Panel: the page Desktop
 *
 * What desk.cpl kept for the desktop, on a page of its own: the system's
 * icons on it (This PC, the Recycle Bin, the user's files, the Control
 * Panel, the Network: HideDesktopIcons, both of its menus' keys, as desk.cpl
 * wrote them), their size (shell32's 32, 48 or 64 pixels), the pictures they
 * have (Explorer\CLSID\{...}\DefaultIcon of the user, as its Change Icon
 * wrote it, and Restore Default), and the widgets of the desktop
 * (startui.dll's dialog).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdio.h>

#include "displaycpl.h"
#include "shlobj.h"
#include "shlwapi.h"
#include "shellapi.h"

#define HIDE_KEY        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\HideDesktopIcons\\"
#define USER_CLASS_KEY  L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\CLSID\\"
#define FCIDM_DESKBROWSER_REFRESH 0xa220

#define ID_SHOW_FIRST   100     /* + the icon */
#define ID_ICON_SIZE    110
#define ID_CHANGE_FIRST 120     /* + the picture */
#define ID_RESTORE      130
#define ID_WIDGETS      131

#define ID_DO_SHOW      200     /* + the icon */
#define ID_DO_ICON_SIZE 210
#define ID_DO_CHANGE    220     /* + the picture */
#define ID_DO_RESTORE   230

#define CLSID_COMPUTER  L"{20D04FE0-3AEA-1069-A2D8-08002B30309D}"
#define CLSID_RECYCLE   L"{645FF040-5081-101B-9F08-00AA002F954E}"
#define CLSID_DOCUMENTS L"{450D8FBA-AD25-11D0-98A8-0800361B1103}"
#define CLSID_CONTROL   L"{21EC2020-3AEA-1069-A2DD-08002B30309D}"
#define CLSID_NETWORK   L"{208D2C60-3AEA-1069-A2D7-08002B30309D}"

/* the system's icons the desktop may show, and what it shows when nothing
 * was chosen (shell32's SHELL32_DesktopIconHidden: This PC, the Recycle Bin) */
static const struct
{
    const WCHAR *clsid;
    BOOL hidden;
} shown_icons[] =
{
    { CLSID_COMPUTER, FALSE },
    { CLSID_RECYCLE, FALSE },
    { CLSID_DOCUMENTS, TRUE },
    { CLSID_CONTROL, TRUE },
    { CLSID_NETWORK, TRUE },
};

/* the pictures one may change: the Recycle Bin has two */
static const struct
{
    const WCHAR *clsid;
    const WCHAR *value;        /* in DefaultIcon */
    UINT name;                 /* 0: the item's own */
} pictures[] =
{
    { CLSID_COMPUTER, NULL, 0 },
    { CLSID_DOCUMENTS, NULL, 0 },
    { CLSID_NETWORK, NULL, 0 },
    { CLSID_RECYCLE, L"Full", IDS_RECYCLE_FULL },
    { CLSID_RECYCLE, L"Empty", IDS_RECYCLE_EMPTY },
};

/* the menu of two columns or the classic one: each keeps which icons it hides */
static BOOL start_panel_on(void)
{
    /* SHGetSetSettings, exported by ordinal only */
    void (WINAPI *get_settings)( SHELLSTATE *, DWORD, BOOL ) =
        (void *)GetProcAddress( GetModuleHandleW( L"shell32.dll" ), (const char *)68 );
    SHELLSTATE ss;

    if (!get_settings) return TRUE;
    memset( &ss, 0, sizeof(ss) );
    get_settings( &ss, 0x00400000 /* SSF_STARTPANELON */, FALSE );
    return ss.fStartPanelOn != 0;
}

static BOOL icon_hidden( UINT i )
{
    WCHAR path[MAX_PATH];

    swprintf( path, ARRAY_SIZE(path), L"%s%s", HIDE_KEY, start_panel_on() ? L"NewStartPanel" : L"ClassicStartMenu" );
    return SHRegGetBoolUSValueW( path, shown_icons[i].clsid, FALSE, shown_icons[i].hidden );
}

static void refresh_desktop(void)
{
    PostMessageW( GetShellWindow(), WM_COMMAND, FCIDM_DESKBROWSER_REFRESH, 0 );
}

static void set_hidden( UINT i, BOOL hidden )
{
    static const WCHAR * const menus[] = { L"NewStartPanel", L"ClassicStartMenu" };
    DWORD value = hidden != 0;
    WCHAR path[MAX_PATH];

    for (UINT m = 0; m < ARRAY_SIZE(menus); m++)
    {
        swprintf( path, ARRAY_SIZE(path), L"%s%s", HIDE_KEY, menus[m] );
        SHSetValueW( HKEY_CURRENT_USER, path, shown_icons[i].clsid, REG_DWORD, &value, sizeof(value) );
    }
    refresh_desktop();
}

/* the item's name, as the namespace has it */
static void item_name( const WCHAR *clsid, WCHAR *name, UINT count )
{
    WCHAR parse[64];
    SHFILEINFOW info;
    ITEMIDLIST *pidl;

    name[0] = 0;
    swprintf( parse, ARRAY_SIZE(parse), L"::%s", clsid );
    if (FAILED(SHParseDisplayName( parse, NULL, &pidl, 0, NULL ))) return;
    if (SHGetFileInfoW( (const WCHAR *)pidl, 0, &info, sizeof(info), SHGFI_PIDL | SHGFI_DISPLAYNAME ))
        lstrcpynW( name, info.szDisplayName, count );
    ILFree( pidl );
}

/* where a picture comes from: the user's own, else the system's */
static void picture_location( UINT i, WCHAR *location, DWORD count )
{
    WCHAR path[MAX_PATH];
    DWORD size = count * sizeof(WCHAR);

    swprintf( path, ARRAY_SIZE(path), L"%s%s\\DefaultIcon", USER_CLASS_KEY, pictures[i].clsid );
    if (!RegGetValueW( HKEY_CURRENT_USER, path, pictures[i].value, RRF_RT_REG_SZ, NULL,
                       location, &size ) && location[0])
        return;
    size = count * sizeof(WCHAR);
    swprintf( path, ARRAY_SIZE(path), L"CLSID\\%s\\DefaultIcon", pictures[i].clsid );
    if (!RegGetValueW( HKEY_CLASSES_ROOT, path, pictures[i].value, RRF_RT_REG_SZ, NULL,
                       location, &size ) && location[0])
        return;
    lstrcpynW( location, L"%SystemRoot%\\system32\\shell32.dll,0", count );
}

/* an icon of a location ("file,index"), kept while the icon theme is the same */
static HICON location_icon( const WCHAR *location, int size )
{
    static struct { WCHAR location[MAX_PATH + 16], theme[64]; int size; HICON icon; } cache[16];
    static UINT next;
    WCHAR file[MAX_PATH], theme[64] = L"We10X";
    DWORD theme_size = sizeof(theme);
    HICON icon = NULL;
    int index;

    RegGetValueW( HKEY_CURRENT_USER, L"Software\\Arctic\\Icons", L"Theme", RRF_RT_REG_SZ, NULL, theme, &theme_size );
    for (UINT i = 0; i < ARRAY_SIZE(cache); i++)
        if (cache[i].icon && cache[i].size == size && !wcsicmp( cache[i].location, location ) &&
            !wcsicmp( cache[i].theme, theme ))
            return cache[i].icon;
    ExpandEnvironmentStringsW( location, file, ARRAY_SIZE(file) );
    index = PathParseIconLocationW( file );
    if (PrivateExtractIconsW( file, index, size, size, &icon, NULL, 1, 0 ) != 1 || !icon) return NULL;
    next %= ARRAY_SIZE(cache);
    /* the oldest goes: a page shows far fewer */
    if (cache[next].icon) DestroyIcon( cache[next].icon );
    lstrcpynW( cache[next].location, location, ARRAY_SIZE(cache[next].location) );
    lstrcpynW( cache[next].theme, theme, ARRAY_SIZE(cache[next].theme) );
    cache[next].size = size;
    cache[next].icon = icon;
    next++;
    return icon;
}

void desktop_build( struct view *view )
{
    WCHAR name[256], location[MAX_PATH + 16];
    DWORD size = desktop_icon_size();
    HWND control;

    nav_display( view, PAGE_DESKTOP );
    view_heading( view, load_string( IDS_DESKTOP ) );

    view_section( view, load_string( IDS_DESKTOP_ICONS ) );
    for (UINT i = 0; i < ARRAY_SIZE(shown_icons); i++)
    {
        UINT k;

        for (k = 0; k < ARRAY_SIZE(pictures) && wcscmp( pictures[k].clsid, shown_icons[i].clsid ); k++) ;
        if (k < ARRAY_SIZE(pictures)) picture_location( k, location, ARRAY_SIZE(location) );
        else
        {
            DWORD bytes = sizeof(location);
            WCHAR path[MAX_PATH];

            swprintf( path, ARRAY_SIZE(path), L"CLSID\\%s\\DefaultIcon", shown_icons[i].clsid );
            if (RegGetValueW( HKEY_CLASSES_ROOT, path, NULL, RRF_RT_REG_SZ, NULL, location, &bytes ))
                lstrcpyW( location, L"%SystemRoot%\\system32\\shell32.dll,0" );
        }
        item_name( shown_icons[i].clsid, name, ARRAY_SIZE(name) );
        view_card_toggle( view, location_icon( location, px( 32 ) ), name, NULL, !icon_hidden( i ), ID_SHOW_FIRST + i );
    }
    control = card_combo( view, ICON_ICON_SIZE, IDS_ICON_SIZE, 0, ID_ICON_SIZE );
    combo_add( control, load_string( IDS_ICONS_SMALL ), 32, size == 32 );
    combo_add( control, load_string( IDS_ICONS_MEDIUM ), 48, size == 48 );
    combo_add( control, load_string( IDS_ICONS_LARGE ), 64, size == 64 );

    view_section( view, load_string( IDS_ICON_LOOK ) );
    view_card_flow( view, px( 8 ) );
    for (UINT i = 0; i < ARRAY_SIZE(pictures); i++)
    {
        HWND tile;

        if (pictures[i].name) lstrcpynW( name, load_string( pictures[i].name ), ARRAY_SIZE(name) );
        else item_name( pictures[i].clsid, name, ARRAY_SIZE(name) );
        picture_location( i, location, ARRAY_SIZE(location) );
        tile = view_tile( view, px( 112 ), px( 48 + 26 + 22 ), name, ID_CHANGE_FIRST + i );
        tile_set_icon( tile, location_icon( location, px( 48 ) ), px( 48 ) );
    }
    lstrcpynW( name, load_string( IDS_DEFAULT_ICONS ), ARRAY_SIZE(name) );
    view_card( view, view_icon( ICON_DESKTOP_ICONS ), name, load_string( IDS_ICON_LOOK_HINT ), L"BUTTON",
               load_string( IDS_RESTORE ), BS_PUSHBUTTON | WS_TABSTOP, px( 130 ), px( 28 ), ID_RESTORE );

    view_section( view, load_string( IDS_WIDGETS_GROUP ) );
    lstrcpynW( name, load_string( IDS_WIDGETS ), ARRAY_SIZE(name) );
    control = view_card( view, view_icon( ICON_WIDGETS ), name, load_string( IDS_WIDGETS_HINT ), L"BUTTON",
                         load_string( IDS_CUSTOMIZE ), BS_PUSHBUTTON | WS_TABSTOP, px( 130 ), px( 28 ), ID_WIDGETS );
}

/**********************************************************************
 *          What the page changes
 */

static void change_picture( struct view *view, UINT i )
{
    WCHAR location[MAX_PATH + 16], file[MAX_PATH], unexpanded[MAX_PATH], key[MAX_PATH];
    int index;

    picture_location( i, location, ARRAY_SIZE(location) );
    ExpandEnvironmentStringsW( location, file, ARRAY_SIZE(file) );
    index = PathParseIconLocationW( file );
    if (!PickIconDlg( GetAncestor( view_window( view ), GA_ROOT ), file, ARRAY_SIZE(file), &index )) return;
    if (!PathUnExpandEnvStringsW( file, unexpanded, ARRAY_SIZE(unexpanded) )) lstrcpynW( unexpanded, file, ARRAY_SIZE(unexpanded) );
    swprintf( location, ARRAY_SIZE(location), L"%s,%d", unexpanded, index );
    swprintf( key, ARRAY_SIZE(key), L"%s%s\\DefaultIcon", USER_CLASS_KEY, pictures[i].clsid );
    SHSetValueW( HKEY_CURRENT_USER, key, pictures[i].value, location[0] == '%' ? REG_EXPAND_SZ : REG_SZ, location,
                 (lstrlenW( location ) + 1) * sizeof(WCHAR) );
    /* the empty Recycle Bin is its own icon too */
    if (pictures[i].value && !wcscmp( pictures[i].value, L"Empty" ))
        SHSetValueW( HKEY_CURRENT_USER, key, NULL, location[0] == '%' ? REG_EXPAND_SZ : REG_SZ, location,
                     (lstrlenW( location ) + 1) * sizeof(WCHAR) );
    refresh_desktop();
    SHChangeNotify( SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL );
}

static void restore_pictures(void)
{
    WCHAR key[MAX_PATH];

    for (UINT i = 0; i < ARRAY_SIZE(pictures); i++)
    {
        swprintf( key, ARRAY_SIZE(key), L"%s%s\\DefaultIcon", USER_CLASS_KEY, pictures[i].clsid );
        SHDeleteValueW( HKEY_CURRENT_USER, key, pictures[i].value );
        if (pictures[i].value) SHDeleteValueW( HKEY_CURRENT_USER, key, NULL );
    }
    refresh_desktop();
    SHChangeNotify( SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL );
}

BOOL desktop_command( struct view *view, UINT id, UINT code, HWND control )
{
    HWND content = view_window( view );

    if (id >= ID_SHOW_FIRST && id < ID_SHOW_FIRST + ARRAY_SIZE(shown_icons))
    {
        if (code == BN_CLICKED) set_hidden( id - ID_SHOW_FIRST, SendMessageW( control, BM_GETCHECK, 0, 0 ) != BST_CHECKED );
        return TRUE;
    }
    if (id >= ID_CHANGE_FIRST && id < ID_CHANGE_FIRST + ARRAY_SIZE(pictures))
    {
        if (code == BN_CLICKED) defer_command( view, ID_DO_CHANGE + id - ID_CHANGE_FIRST );
        return TRUE;
    }
    if (id >= ID_DO_CHANGE && id < ID_DO_CHANGE + ARRAY_SIZE(pictures))
    {
        change_picture( view, id - ID_DO_CHANGE );
        view_rebuild( view );
        return TRUE;
    }
    switch (id)
    {
    case ID_ICON_SIZE:
        if (code == CBN_SELCHANGE) defer_command( view, ID_DO_ICON_SIZE );
        return TRUE;
    case ID_RESTORE:
        if (code == BN_CLICKED) defer_command( view, ID_DO_RESTORE );
        return TRUE;
    case ID_WIDGETS:
        if (code == BN_CLICKED)
        {
            HMODULE startui = LoadLibraryW( L"startui.dll" );
            void (WINAPI *dialog)( HWND ) = startui ? (void *)GetProcAddress( startui, "ArcticDesktopWidgetsDialog" ) : NULL;

            if (dialog) dialog( GetAncestor( content, GA_ROOT ) );
        }
        return TRUE;
    case ID_DO_ICON_SIZE:
    {
        LPARAM size = combo_data( GetDlgItem( content, ID_ICON_SIZE ) );

        if (size != -1) desktop_icon_size_set( (DWORD)size );
        return TRUE;
    }
    case ID_DO_RESTORE:
        restore_pictures();
        view_rebuild( view );
        return TRUE;
    }
    return FALSE;
}
