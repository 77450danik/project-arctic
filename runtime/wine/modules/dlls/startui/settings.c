/*
 * The Start menu: what it shows, and the dialog that chooses it
 *
 * As Windows 7's "Customize Start Menu" (the taskbar's properties, Start
 * menu, Customize...): each place of the right column shown as a link,
 * as a menu of what is in it, or not at all; how many of the most used
 * programs the left column lists; large or small icons there. Everything
 * lives in HKCU\Software\Arctic\StartMenu, read each time the menu opens.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdio.h>

#include "startui.h"
#include "winreg.h"

#define REG_KEY          L"Software\\Arctic\\StartMenu"
#define MAX_RECENT       16

#define ID_ITEM_FIRST    1000
#define ID_RECENT        1100
#define ID_LARGE         1101
#define ID_DEFAULTS      1102

static const struct
{
    const WCHAR *value;  /* Show.<value> */
    enum glyph glyph;
    UINT name;
    int group;
    BOOL menu;           /* can be shown as a menu */
    BYTE show;           /* by default */
}
places[R_COUNT] =
{
    { L"User",         GLYPH_PERSON,        IDS_USER_FOLDER,   0, TRUE,  SHOW_LINK },
    { L"Documents",    GLYPH_DOCUMENT,      IDS_DOCUMENTS,     0, TRUE,  SHOW_LINK },
    { L"Pictures",     GLYPH_PICTURE,       IDS_PICTURES,      0, TRUE,  SHOW_LINK },
    { L"Music",        GLYPH_MUSIC,         IDS_MUSIC,         0, TRUE,  SHOW_LINK },
    { L"Downloads",    GLYPH_DOWNLOAD,      IDS_DOWNLOADS,     0, TRUE,  SHOW_LINK },
    { L"Recent",       GLYPH_RECENT,        IDS_RECENT,        1, TRUE,  SHOW_MENU },
    { L"Computer",     GLYPH_PC,            IDS_THIS_PC,       1, TRUE,  SHOW_LINK },
    { L"ControlPanel", GLYPH_CONTROL_PANEL, IDS_CONTROL_PANEL, 2, TRUE,  SHOW_LINK },
    { L"Settings",     GLYPH_SETTINGS,      IDS_SETTINGS,      2, FALSE, SHOW_LINK },
    { L"Update",       GLYPH_UPDATE,        IDS_UPDATE,        2, FALSE, SHOW_LINK },
    { L"Run",          GLYPH_RUN,           IDS_RUN,           3, FALSE, SHOW_LINK },
};

BOOL right_can_be_menu( int action )
{
    return places[action].menu;
}

int right_group( int action )
{
    return places[action].group;
}

enum glyph right_glyph( int action )
{
    return places[action].glyph;
}

const WCHAR *right_name( int action )
{
    return load_string( places[action].name );
}

void settings_defaults( struct start_settings *settings )
{
    settings->recent = 8;
    settings->large_icons = TRUE;
    for (int i = 0; i < R_COUNT; i++) settings->show[i] = places[i].show;
}

static DWORD read_dword( HKEY key, const WCHAR *name, DWORD def )
{
    DWORD value, size = sizeof(value), type;

    if (key && !RegQueryValueExW( key, name, NULL, &type, (BYTE *)&value, &size ) && type == REG_DWORD) return value;
    return def;
}

void settings_load( struct start_settings *settings )
{
    HKEY key = NULL;

    settings_defaults( settings );
    if (RegOpenKeyExW( HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &key )) return;
    settings->recent = min( read_dword( key, L"RecentPrograms", settings->recent ), MAX_RECENT );
    settings->large_icons = !!read_dword( key, L"LargeIcons", settings->large_icons );
    for (int i = 0; i < R_COUNT; i++)
    {
        WCHAR name[64];
        DWORD show;

        swprintf( name, ARRAY_SIZE(name), L"Show.%s", places[i].value );
        show = read_dword( key, name, settings->show[i] );
        if (show > SHOW_MENU || (show == SHOW_MENU && !places[i].menu)) show = places[i].show;
        settings->show[i] = show;
    }
    RegCloseKey( key );
}

void settings_save( const struct start_settings *settings )
{
    DWORD value;
    HKEY key;

    if (RegCreateKeyExW( HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL )) return;
    value = settings->recent;
    RegSetValueExW( key, L"RecentPrograms", 0, REG_DWORD, (const BYTE *)&value, sizeof(value) );
    value = settings->large_icons;
    RegSetValueExW( key, L"LargeIcons", 0, REG_DWORD, (const BYTE *)&value, sizeof(value) );
    for (int i = 0; i < R_COUNT; i++)
    {
        WCHAR name[64];

        swprintf( name, ARRAY_SIZE(name), L"Show.%s", places[i].value );
        value = settings->show[i];
        RegSetValueExW( key, name, 0, REG_DWORD, (const BYTE *)&value, sizeof(value) );
    }
    RegCloseKey( key );
}

/**********************************************************************
 *          The dialog
 */

static HFONT dialog_font;

static HWND add_control( HWND dialog, const WCHAR *cls, const WCHAR *text, DWORD style, int x, int y, int width,
                         int height, int id )
{
    HWND hwnd = CreateWindowExW( 0, cls, text, WS_CHILD | WS_VISIBLE | style, px( x ), px( y ), px( width ),
                                 px( height ), dialog, (HMENU)(INT_PTR)id, startui_instance, NULL );

    SendMessageW( hwnd, WM_SETFONT, (WPARAM)dialog_font, FALSE );
    return hwnd;
}

static void select_data( HWND combo, LPARAM data )
{
    int count = SendMessageW( combo, CB_GETCOUNT, 0, 0 );

    for (int i = 0; i < count; i++)
    {
        if (SendMessageW( combo, CB_GETITEMDATA, i, 0 ) == data)
        {
            SendMessageW( combo, CB_SETCURSEL, i, 0 );
            return;
        }
    }
}

static void show_settings( HWND dialog, const struct start_settings *settings )
{
    for (int i = 0; i < R_COUNT; i++) select_data( GetDlgItem( dialog, ID_ITEM_FIRST + i ), settings->show[i] );
    SendDlgItemMessageW( dialog, ID_RECENT, CB_SETCURSEL, settings->recent, 0 );
    CheckDlgButton( dialog, ID_LARGE, settings->large_icons ? BST_CHECKED : BST_UNCHECKED );
}

static void init_dialog( HWND dialog )
{
    struct start_settings settings;
    NONCLIENTMETRICSW metrics = { sizeof(metrics) };
    RECT rect;
    int y = 40, bottom;

    SystemParametersInfoW( SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0 );
    if (!dialog_font) dialog_font = CreateFontIndirectW( &metrics.lfMessageFont );
    SetWindowTextW( dialog, load_string( IDS_CUSTOMIZE_TITLE ) );
    settings_load( &settings );

    add_control( dialog, L"STATIC", load_string( IDS_CUSTOMIZE_ITEMS ), 0, 12, 12, 396, 20, -1 );
    for (int i = 0; i < R_COUNT; i++, y += 30)
    {
        HWND combo;

        add_control( dialog, L"STATIC", right_name( i ), SS_CENTERIMAGE, 24, y, 196, 24, -1 );
        combo = add_control( dialog, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 224, y, 184, 120,
                             ID_ITEM_FIRST + i );
        SendMessageW( combo, CB_SETITEMDATA, SendMessageW( combo, CB_ADDSTRING, 0,
                      (LPARAM)load_string( IDS_SHOW_LINK ) ), SHOW_LINK );
        if (places[i].menu)
            SendMessageW( combo, CB_SETITEMDATA, SendMessageW( combo, CB_ADDSTRING, 0,
                          (LPARAM)load_string( IDS_SHOW_MENU ) ), SHOW_MENU );
        SendMessageW( combo, CB_SETITEMDATA, SendMessageW( combo, CB_ADDSTRING, 0,
                      (LPARAM)load_string( IDS_SHOW_HIDDEN ) ), SHOW_HIDDEN );
    }

    y += 10;
    add_control( dialog, L"STATIC", load_string( IDS_RECENT_COUNT ), SS_CENTERIMAGE, 12, y, 300, 24, -1 );
    {
        HWND combo = add_control( dialog, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 328, y, 80,
                                  200, ID_RECENT );
        for (int i = 0; i <= MAX_RECENT; i++)
        {
            WCHAR text[8];
            swprintf( text, ARRAY_SIZE(text), L"%d", i );
            SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)text );
        }
    }
    y += 34;
    add_control( dialog, L"BUTTON", load_string( IDS_LARGE_ICONS ), BS_AUTOCHECKBOX | WS_TABSTOP, 12, y, 396, 22,
                 ID_LARGE );
    y += 40;
    add_control( dialog, L"BUTTON", load_string( IDS_DEFAULTS ), BS_PUSHBUTTON | WS_TABSTOP, 12, y, 150, 26,
                 ID_DEFAULTS );
    add_control( dialog, L"BUTTON", load_string( IDS_OK ), BS_DEFPUSHBUTTON | WS_TABSTOP, 228, y, 86, 26, IDOK );
    add_control( dialog, L"BUTTON", load_string( IDS_CANCEL ), BS_PUSHBUTTON | WS_TABSTOP, 322, y, 86, 26, IDCANCEL );
    bottom = y + 26 + 12;
    show_settings( dialog, &settings );

    /* the client area the controls want, centred on the owner */
    SetRect( &rect, 0, 0, px( 420 ), px( bottom ) );
    AdjustWindowRectEx( &rect, GetWindowLongW( dialog, GWL_STYLE ), FALSE, GetWindowLongW( dialog, GWL_EXSTYLE ) );
    {
        RECT owner;
        HWND parent = GetWindow( dialog, GW_OWNER );
        int width = rect.right - rect.left, height = rect.bottom - rect.top;

        if (!parent || !GetWindowRect( parent, &owner )) SetRect( &owner, 0, 0, GetSystemMetrics( SM_CXSCREEN ),
                                                                     GetSystemMetrics( SM_CYSCREEN ) );
        SetWindowPos( dialog, NULL, (owner.left + owner.right - width) / 2, max( 0, (int)(owner.top + owner.bottom - height) / 2 ),
                      width, height, SWP_NOZORDER | SWP_NOACTIVATE );
    }
}

static INT_PTR WINAPI customize_proc( HWND dialog, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct start_settings settings;

    switch (msg)
    {
    case WM_INITDIALOG:
        init_dialog( dialog );
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD( wparam ))
        {
        case ID_DEFAULTS:
            settings_defaults( &settings );
            show_settings( dialog, &settings );
            return TRUE;
        case IDOK:
            settings_load( &settings );
            for (int i = 0; i < R_COUNT; i++)
            {
                HWND combo = GetDlgItem( dialog, ID_ITEM_FIRST + i );
                LRESULT sel = SendMessageW( combo, CB_GETCURSEL, 0, 0 );
                if (sel >= 0) settings.show[i] = (BYTE)SendMessageW( combo, CB_GETITEMDATA, sel, 0 );
            }
            settings.recent = max( 0, (int)SendDlgItemMessageW( dialog, ID_RECENT, CB_GETCURSEL, 0, 0 ) );
            settings.large_icons = IsDlgButtonChecked( dialog, ID_LARGE ) == BST_CHECKED;
            settings_save( &settings );
            EndDialog( dialog, IDOK );
            return TRUE;
        case IDCANCEL:
            EndDialog( dialog, IDCANCEL );
            return TRUE;
        }
        break;
    }
    return FALSE;
}

void customize_dialog( HWND owner )
{
    /* an empty template: the controls are made at run time, in the strings of the user's language */
    static const struct
    {
        DLGTEMPLATE header;
        WORD menu, cls, title;
    } DECLSPEC_ALIGN(4) template =
    {
        { DS_MODALFRAME | DS_SETFOREGROUND | WS_POPUP | WS_VISIBLE | WS_CAPTION | WS_SYSMENU, 0, 0, 0, 0, 200, 200 },
    };

    DialogBoxIndirectParamW( startui_instance, &template.header, owner, customize_proc, 0 );
}
