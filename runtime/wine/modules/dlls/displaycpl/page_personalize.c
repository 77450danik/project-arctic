/*
 * Display of the Control Panel: the page Personalization
 *
 * Windows 11's Personalization and its Colors: the mode (Light, Dark, or
 * Custom with one for Windows and one for programs), each a desktop in small
 * with the wallpaper of this one; the look of the controls (Windows 10 or
 * 11), the title bars, the icon theme; and the pages of the rest (the
 * background, the desktop, the screen saver, the taskbar, Start). What they
 * change, startui.dll changes, as its page of desk.cpl's dialog did.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdio.h>

#include "displaycpl.h"

#define PERSONALIZE_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"
#define ICONS_KEY       L"Software\\Arctic\\Icons"

enum { MODE_LIGHT, MODE_DARK, MODE_CUSTOM, MODE_COUNT };

#define ID_MODE_FIRST   100     /* + the mode */
#define ID_SYSTEM       110
#define ID_APPS         111
#define ID_STYLE        112
#define ID_CAPTIONS     113
#define ID_ICONS        114

#define ID_DO_MODE      200     /* + the mode */
#define ID_DO_SYSTEM    210
#define ID_DO_APPS      211
#define ID_DO_STYLE     212
#define ID_DO_ICONS     213

static DWORD reg_dword( const WCHAR *path, const WCHAR *name, DWORD def )
{
    DWORD value, size = sizeof(value);

    if (RegGetValueW( HKEY_CURRENT_USER, path, name, RRF_RT_REG_DWORD, NULL, &value, &size )) return def;
    return value;
}

static void icon_theme( WCHAR *name, DWORD count )
{
    DWORD size = count * sizeof(WCHAR);

    lstrcpynW( name, L"We10X", count );
    RegGetValueW( HKEY_CURRENT_USER, ICONS_KEY, L"Theme", RRF_RT_REG_SZ, NULL, name, &size );
}

DWORD modes_get(void)
{
    WCHAR theme[64];
    DWORD ret = 0;

    if (reg_dword( PERSONALIZE_KEY, L"SystemUsesLightTheme", 1 )) ret |= OVERLAY_SYSTEM_LIGHT;
    if (reg_dword( PERSONALIZE_KEY, L"AppsUseLightTheme", 1 )) ret |= OVERLAY_APPS_LIGHT;
    if (reg_dword( L"Software\\Arctic\\Personalize", L"Style", 1 )) ret |= LOOK_STYLE11;
    icon_theme( theme, ARRAY_SIZE(theme) );
    if (!wcsicmp( theme, L"We10X" )) ret |= LOOK_WE10X;
    return ret;
}

static void *startui( const char *name )
{
    HMODULE module = LoadLibraryW( L"startui.dll" );

    return module ? (void *)GetProcAddress( module, name ) : NULL;
}

static void set_modes( BOOL system_light, BOOL apps_light )
{
    void (WINAPI *set)( BOOL, BOOL ) = startui( "ArcticPersonalizeSetModes" );

    if (set) set( system_light, apps_light );
}

static int mode_now( DWORD modes )
{
    BOOL system_light = (modes & OVERLAY_SYSTEM_LIGHT) != 0, apps_light = (modes & OVERLAY_APPS_LIGHT) != 0;

    if (system_light != apps_light) return MODE_CUSTOM;
    return system_light ? MODE_LIGHT : MODE_DARK;
}

/* the icon themes: the folders of %WINDIR%\Resources\Icons, the system's own first */
static void fill_icon_themes( HWND combo )
{
    WCHAR dir[MAX_PATH], current[64];
    WIN32_FIND_DATAW data;
    HANDLE find;

    icon_theme( current, ARRAY_SIZE(current) );
    combo_add( combo, load_string( IDS_ICONS_STANDARD ), 0, !current[0] );
    GetWindowsDirectoryW( dir, ARRAY_SIZE(dir) );
    lstrcatW( dir, L"\\Resources\\Icons\\*" );
    if ((find = FindFirstFileW( dir, &data )) == INVALID_HANDLE_VALUE) return;
    do
    {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || data.cFileName[0] == '.') continue;
        combo_add( combo, data.cFileName, 1, !wcsicmp( data.cFileName, current ) );
    } while (FindNextFileW( find, &data ));
    FindClose( find );
}

static void link_card( struct view *view, UINT icon, UINT title, UINT hint, link_proc proc, UINT_PTR param )
{
    WCHAR name[128];

    lstrcpynW( name, load_string( title ), ARRAY_SIZE(name) );
    view_card_link( view, view_icon( icon ), name, load_string( hint ), proc, param );
}

void personalize_build( struct view *view )
{
    static const UINT names[MODE_COUNT] = { IDS_MODE_LIGHT, IDS_MODE_DARK, IDS_MODE_CUSTOM };
    /* Custom as Windows 11 first offers it: Windows dark, programs light */
    static const UINT_PTR looks[MODE_COUNT] = { OVERLAY_SYSTEM_LIGHT | OVERLAY_APPS_LIGHT, 0, OVERLAY_APPS_LIGHT };
    struct display_state *s = display_state( view );
    DWORD modes = modes_get();
    int mode = mode_now( modes ), pic_h = px( 84 ), pic_w = preview_width( pic_h ), frame = px( 10 );
    WCHAR name[128];
    HWND control;

    if (!s) return;
    nav_display( view, PAGE_PERSONALIZE );
    view_heading( view, load_string( IDS_PERSONALIZE ) );
    preview_init( &s->preview, pic_w, pic_h );

    /* the mode, each a desktop in small */
    view_section( view, load_string( IDS_MODE_GROUP ) );
    view_card_flow( view, px( 12 ) );
    for (int i = 0; i < MODE_COUNT; i++)
    {
        HWND tile = view_tile( view, pic_w + frame, pic_h + frame + px( 22 ), load_string( names[i] ), ID_MODE_FIRST + i );

        if (s->preview.picture) picture_to_tile( tile, s->preview.path, pic_w, pic_h, TRUE );
        else tile_set_color( tile, s->preview.color );
        tile_set_paint( tile, desktop_overlay, OVERLAY_WINDOW | looks[i] );
        tile_select( tile, i == mode );
    }
    if (mode == MODE_CUSTOM)
    {
        control = card_combo( view, ICON_MODE, IDS_MODE_WINDOWS, 0, ID_SYSTEM );
        combo_add( control, load_string( IDS_MODE_LIGHT ), 1, (modes & OVERLAY_SYSTEM_LIGHT) != 0 );
        combo_add( control, load_string( IDS_MODE_DARK ), 0, !(modes & OVERLAY_SYSTEM_LIGHT) );
        control = card_combo( view, ICON_MODE, IDS_MODE_APPS, 0, ID_APPS );
        combo_add( control, load_string( IDS_MODE_LIGHT ), 1, (modes & OVERLAY_APPS_LIGHT) != 0 );
        combo_add( control, load_string( IDS_MODE_DARK ), 0, !(modes & OVERLAY_APPS_LIGHT) );
    }

    view_section( view, load_string( IDS_LOOK_GROUP ) );
    control = card_combo( view, ICON_STYLE, IDS_CONTROLS_STYLE, IDS_CONTROLS_HINT, ID_STYLE );
    combo_add( control, L"Windows 10", 0, !(modes & LOOK_STYLE11) );
    combo_add( control, L"Windows 11", 1, (modes & LOOK_STYLE11) != 0 );
    lstrcpynW( name, load_string( IDS_CAPTIONS ), ARRAY_SIZE(name) );
    control = view_card( view, view_icon( ICON_CAPTIONS ), name, load_string( IDS_CAPTIONS_HINT ), L"BUTTON",
                         load_string( IDS_CUSTOMIZE ), BS_PUSHBUTTON | WS_TABSTOP, px( 130 ), px( 28 ), ID_CAPTIONS );
    EnableWindow( control, startui( "ArcticPersonalizeCaptions" ) != NULL );
    fill_icon_themes( card_combo( view, ICON_ICON_THEME, IDS_ICON_THEME, IDS_ICON_THEME_HINT, ID_ICONS ) );

    view_section( view, load_string( IDS_RELATED ) );
    link_card( view, ICON_BACKGROUND, IDS_BACKGROUND, IDS_BACKGROUND_HINT, go_page, PAGE_BACKGROUND );
    link_card( view, ICON_DESKTOP, IDS_DESKTOP, IDS_DESKTOP_HINT, go_page, PAGE_DESKTOP );
    link_card( view, ICON_SCREENSAVER, IDS_SCREENSAVER, IDS_SCREENSAVER_HINT, go_page, PAGE_SCREENSAVER );
    link_card( view, ICON_TASKBAR, IDS_TASKBAR_SHORT, IDS_TASKBAR_HINT, go_item, (UINT_PTR)ITEM_TASKBAR );
    link_card( view, ICON_START, IDS_START, IDS_START_HINT, go_item, (UINT_PTR)ITEM_START );
}

BOOL personalize_command( struct view *view, UINT id, UINT code, HWND control )
{
    HWND content = view_window( view );
    DWORD modes = modes_get();

    if (id >= ID_MODE_FIRST && id < ID_MODE_FIRST + MODE_COUNT)
    {
        if (code == BN_CLICKED) defer_command( view, ID_DO_MODE + id - ID_MODE_FIRST );
        return TRUE;
    }
    switch (id)
    {
    case ID_SYSTEM:
        if (code == CBN_SELCHANGE) defer_command( view, ID_DO_SYSTEM );
        return TRUE;
    case ID_APPS:
        if (code == CBN_SELCHANGE) defer_command( view, ID_DO_APPS );
        return TRUE;
    case ID_STYLE:
        if (code == CBN_SELCHANGE) defer_command( view, ID_DO_STYLE );
        return TRUE;
    case ID_ICONS:
        if (code == CBN_SELCHANGE) defer_command( view, ID_DO_ICONS );
        return TRUE;
    case ID_CAPTIONS:
        if (code == BN_CLICKED)
        {
            void (WINAPI *captions)( HWND ) = startui( "ArcticPersonalizeCaptions" );

            if (captions) captions( GetAncestor( content, GA_ROOT ) );
        }
        return TRUE;

    case ID_DO_MODE + MODE_LIGHT:
        set_modes( TRUE, TRUE );
        view_rebuild( view );
        return TRUE;
    case ID_DO_MODE + MODE_DARK:
        set_modes( FALSE, FALSE );
        view_rebuild( view );
        return TRUE;
    case ID_DO_MODE + MODE_CUSTOM:
        if (mode_now( modes ) != MODE_CUSTOM) set_modes( FALSE, TRUE );
        view_rebuild( view );
        return TRUE;
    case ID_DO_SYSTEM:
    {
        LPARAM light = combo_data( GetDlgItem( content, ID_SYSTEM ) );

        if (light != -1) set_modes( light != 0, (modes & OVERLAY_APPS_LIGHT) != 0 );
        view_rebuild( view );
        return TRUE;
    }
    case ID_DO_APPS:
    {
        LPARAM light = combo_data( GetDlgItem( content, ID_APPS ) );

        if (light != -1) set_modes( (modes & OVERLAY_SYSTEM_LIGHT) != 0, light != 0 );
        view_rebuild( view );
        return TRUE;
    }
    case ID_DO_STYLE:
    {
        void (WINAPI *set)( BOOL ) = startui( "ArcticPersonalizeSetStyle" );
        LPARAM win11 = combo_data( GetDlgItem( content, ID_STYLE ) );

        if (set && win11 != -1) set( win11 != 0 );
        return TRUE;
    }
    case ID_DO_ICONS:
    {
        void (WINAPI *set)( const WCHAR * ) = startui( "ArcticPersonalizeSetIconTheme" );
        HWND combo = GetDlgItem( content, ID_ICONS );
        LRESULT index = SendMessageW( combo, CB_GETCURSEL, 0, 0 );
        WCHAR name[64] = L"";

        if (!set || index == CB_ERR) return TRUE;
        /* the system's own is the first, with no folder */
        if (combo_data( combo ) && SendMessageW( combo, CB_GETLBTEXTLEN, index, 0 ) < ARRAY_SIZE(name))
            SendMessageW( combo, CB_GETLBTEXT, index, (LPARAM)name );
        set( name );
        return TRUE;
    }
    }
    return FALSE;
}
