/*
 * Power Options of the Control Panel: "Визначити кнопки живлення й
 * увімкнути захист паролем"
 *
 * Windows 10's pageGlobalSettings (powercpl.dll's UIFILE 104): what the
 * power button and the lid do, on battery and on the mains, for every plan
 * at once; then the shutdown settings, greyed until "Змінити параметри, які
 * зараз недоступні": fast startup (HiberbootEnabled, docs/hibernation.md),
 * and Sleep, Hibernate and Lock in the power menu (FlyoutMenuSettings).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdlib.h>

#include "powercpl.h"
#include "commctrl.h"
#include "winternl.h"
#include "powrprof.h"

#define ID_POWER_BUTTON  200   /* + column */
#define ID_LID           210
#define ID_FAST_STARTUP  220
#define ID_SHOW_SLEEP    221
#define ID_SHOW_HIBERNATE 222
#define ID_SHOW_LOCK     223
#define ID_SAVE          230
#define ID_CANCEL        231

#define LABEL_WIDTH      px(230)
#define COLUMN_WIDTH     px(170)

static const WCHAR flyout_key[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FlyoutMenuSettings";
static const WCHAR session_power_key[] = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power";

static int columns(void)
{
    return on_battery_machine() ? 2 : 1;
}

static BOOL column_ac( int column )
{
    return columns() == 1 || column == 1;
}

static DWORD reg_dword( HKEY root, const WCHAR *path, const WCHAR *name, DWORD fallback )
{
    DWORD value = fallback, size = sizeof(value);

    RegGetValueW( root, path, name, RRF_RT_REG_DWORD, NULL, &value, &size );
    return value;
}

static void set_reg_dword( HKEY root, const WCHAR *path, const WCHAR *name, DWORD value )
{
    HKEY key;

    if (RegCreateKeyExW( root, path, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL )) return;
    RegSetValueExW( key, name, 0, REG_DWORD, (BYTE *)&value, sizeof(value) );
    RegCloseKey( key );
}

/* the actions as the setting lists them, without those the machine cannot do;
 * a plan keeps the place in that list (0 nothing, 1 sleep, 2 hibernate,
 * 3 shut down, 4 turn off the display), not the value */
static void fill_actions( HWND combo, const GUID *setting, DWORD current )
{
    for (ULONG i = 0; i < 8; i++)
    {
        WCHAR name[128];
        DWORD name_size = sizeof(name);
        int index;

        if (PowerReadPossibleFriendlyName( NULL, &sub_buttons, setting, i, (UCHAR *)name, &name_size )) break;
        if (i == 1 && !IsPwrSuspendAllowed() && current != 1) continue;
        if (i == 2 && !IsPwrHibernateAllowed() && current != 2) continue;
        index = SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)name );
        SendMessageW( combo, CB_SETITEMDATA, index, i );
        if (i == current) SendMessageW( combo, CB_SETCURSEL, index, 0 );
    }
}

static void unlock( struct view *view, UINT_PTR param )
{
    view_state( view )->elevated = TRUE;
    for (UINT id = ID_FAST_STARTUP; id <= ID_SHOW_LOCK; id++)
    {
        HWND check = GetDlgItem( view_window( view ), id );
        if (check) EnableWindow( check, TRUE );
    }
}

static void add_row( struct view *view, const WCHAR *label, const GUID *setting, UINT id )
{
    GUID active;

    plan_active( &active );
    view_row_begin( view );
    view_text_at( view, STYLE_BODY, label, px( 4 ) );
    for (int c = 0; c < columns(); c++)
    {
        HWND combo = view_control_beside( view, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                                          LABEL_WIDTH + c * COLUMN_WIDTH, COLUMN_WIDTH - px( 16 ), px( 200 ), id + c );
        fill_actions( combo, setting, plan_value( &active, &sub_buttons, setting, column_ac( c ), 0 ) );
    }
    view_row_end( view );
    view_space( view, px( 10 ) );
}

static void add_check( struct view *view, struct page_state *state, UINT id, UINT name, UINT text, BOOL checked )
{
    HWND check = view_control( view, WC_BUTTONW, load_string( name ), BS_AUTOCHECKBOX | WS_TABSTOP,
                               view_content_width( view ), px( 20 ), 0, id );
    SendMessageW( check, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0 );
    EnableWindow( check, state->elevated );
    view_text_at( view, STYLE_SMALL, load_string( text ), px( 18 ) );
    view_space( view, px( 8 ) );
}

void system_build( struct view *view )
{
    struct page_state *state = view_state( view );
    BOOL lid = lid_present();
    int width = view_content_width( view );
    DWORD hibernate = 0;

    view_text( view, STYLE_TITLE, load_string( IDS_SYSTEM_TITLE ) );
    view_text( view, STYLE_BODY, load_string( IDS_SYSTEM_TEXT ) );
    view_space( view, px( 18 ) );

    view_group( view, load_string( lid ? IDS_BUTTONS_LID : IDS_BUTTONS_POWER ) );
    view_space( view, px( 10 ) );
    if (columns() == 2)
    {
        view_row_begin( view );
        view_text_at( view, STYLE_BODY, L" ", 0 );
        view_text_at( view, STYLE_BODY, load_string( IDS_BATTERY_COLUMN ), LABEL_WIDTH );
        view_text_at( view, STYLE_BODY, load_string( IDS_AC_COLUMN ), LABEL_WIDTH + COLUMN_WIDTH );
        view_row_end( view );
        view_space( view, px( 10 ) );
    }
    add_row( view, load_string( IDS_WHEN_POWER_BUTTON ), &set_pbutton, ID_POWER_BUTTON );
    if (lid) add_row( view, load_string( IDS_WHEN_LID ), &set_lid, ID_LID );
    view_space( view, px( 14 ) );

    if (!state->elevated) view_link( view, load_string( IDS_CHANGE_UNAVAILABLE ), unlock, 0, 0 );
    view_space( view, px( 14 ) );

    view_group( view, load_string( IDS_SHUTDOWN_SETTINGS ) );
    view_space( view, px( 10 ) );
    hibernate = reg_dword( HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Power", L"HibernateEnabled", 0 );
    add_check( view, state, ID_FAST_STARTUP, IDS_FAST_STARTUP, IDS_FAST_STARTUP_TEXT,
               reg_dword( HKEY_LOCAL_MACHINE, session_power_key, L"HiberbootEnabled", 0 ) );
    if (IsPwrSuspendAllowed())
        add_check( view, state, ID_SHOW_SLEEP, IDS_SHOW_SLEEP, IDS_SHOW_SLEEP_TEXT,
                   reg_dword( HKEY_LOCAL_MACHINE, flyout_key, L"ShowSleepOption", 1 ) );
    if (hibernate)
        add_check( view, state, ID_SHOW_HIBERNATE, IDS_SHOW_HIBERNATE, IDS_SHOW_HIBERNATE_TEXT,
                   reg_dword( HKEY_LOCAL_MACHINE, flyout_key, L"ShowHibernateOption", 1 ) );
    add_check( view, state, ID_SHOW_LOCK, IDS_SHOW_LOCK, IDS_SHOW_LOCK_TEXT,
               reg_dword( HKEY_LOCAL_MACHINE, flyout_key, L"ShowLockOption", 1 ) );
    view_space( view, px( 24 ) );

    view_row_begin( view );
    view_control_beside( view, WC_BUTTONW, load_string( IDS_SAVE_CHANGES ), BS_DEFPUSHBUTTON | WS_TABSTOP,
                         width - px( 2 * 98 + 8 ), px( 98 ), px( 24 ), ID_SAVE );
    view_control_beside( view, WC_BUTTONW, load_string( IDS_CANCEL ), BS_PUSHBUTTON | WS_TABSTOP, width - px( 98 ),
                         px( 98 ), px( 24 ), ID_CANCEL );
    view_row_end( view );
}

static void save( struct view *view )
{
    HWND content = view_window( view );
    struct plan plans[32];
    UINT count = plans_list( plans, ARRAY_SIZE(plans) );
    static const struct { UINT id; const WCHAR *name; } checks[] =
    {
        { ID_SHOW_SLEEP, L"ShowSleepOption" }, { ID_SHOW_HIBERNATE, L"ShowHibernateOption" }, { ID_SHOW_LOCK, L"ShowLockOption" },
    };

    /* the buttons and the lid: every plan, as the page says */
    for (int c = 0; c < columns(); c++)
    {
        static const struct { UINT id; const GUID *setting; } rows[] = { { ID_POWER_BUTTON, &set_pbutton }, { ID_LID, &set_lid } };

        for (UINT r = 0; r < ARRAY_SIZE(rows); r++)
        {
            HWND combo = GetDlgItem( content, rows[r].id + c );
            int index;

            if (!combo || (index = SendMessageW( combo, CB_GETCURSEL, 0, 0 )) < 0) continue;
            for (UINT p = 0; p < count; p++)
                plan_set_value( &plans[p].guid, &sub_buttons, rows[r].setting, column_ac( c ),
                                SendMessageW( combo, CB_GETITEMDATA, index, 0 ) );
        }
    }
    if (GetDlgItem( content, ID_FAST_STARTUP ))
        set_reg_dword( HKEY_LOCAL_MACHINE, session_power_key, L"HiberbootEnabled",
                       SendMessageW( GetDlgItem( content, ID_FAST_STARTUP ), BM_GETCHECK, 0, 0 ) == BST_CHECKED );
    for (UINT i = 0; i < ARRAY_SIZE(checks); i++)
        if (GetDlgItem( content, checks[i].id ))
            set_reg_dword( HKEY_LOCAL_MACHINE, flyout_key, checks[i].name,
                           SendMessageW( GetDlgItem( content, checks[i].id ), BM_GETCHECK, 0, 0 ) == BST_CHECKED );
}

BOOL system_command( struct view *view, UINT id, UINT code, HWND control )
{
    if (code != BN_CLICKED) return FALSE;
    switch (id)
    {
    case ID_SAVE:
        save( view );
        view_navigate( view, PAGE_PLANS, NULL, FALSE );
        return TRUE;
    case ID_CANCEL:
        view_navigate( view, PAGE_PLANS, NULL, FALSE );
        return TRUE;
    }
    return FALSE;
}
