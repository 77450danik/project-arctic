/*
 * Network and Sharing Center: Change advanced sharing settings
 *
 * What Arctic has of Windows 7's page: network discovery, which Network of
 * Explorer looks with (ntlanman.dll), the network drives and the passwords
 * kept for the computers of the network. docs/M5-network.md.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "netcenter.h"

#define INDENT          px(20)

#define IDC_DISCOVERY_ON    100
#define IDC_DISCOVERY_OFF   101
#define IDC_SAVE            102
#define IDC_CANCEL          103

static const WCHAR network_key[] = L"SOFTWARE\\Arctic\\Network";

static BOOL discovery_enabled(void)
{
    DWORD value = 1, size = sizeof(value);

    RegGetValueW( HKEY_LOCAL_MACHINE, network_key, L"NetworkDiscovery", RRF_RT_REG_DWORD, NULL, &value, &size );
    return value != 0;
}

static void map_drive( struct view *view, UINT_PTR param )
{
    cp_run( L"rundll32.exe", L"ntlanman.dll,MapNetworkDrive" );
}

static void disconnect_drive( struct view *view, UINT_PTR param )
{
    cp_run( L"rundll32.exe", L"ntlanman.dll,DisconnectNetworkDrives" );
}

static void forget_passwords( struct view *view, UINT_PTR param )
{
    cp_run( L"rundll32.exe", L"ntlanman.dll,ForgetLogons" );
}

static void open_network( struct view *view, UINT_PTR param )
{
    cp_run( L"explorer.exe", L"::{208D2C60-3AEA-1069-A2D7-08002B30309D}" );
}

BOOL sharing_command( struct view *view, UINT id, UINT code, HWND control )
{
    if (code != BN_CLICKED) return FALSE;
    switch (id)
    {
    case IDC_DISCOVERY_ON:
    case IDC_DISCOVERY_OFF:
        EnableWindow( GetDlgItem( GetParent( control ), IDC_SAVE ), TRUE );
        return TRUE;
    case IDC_SAVE:
    {
        DWORD value = SendMessageW( GetDlgItem( GetParent( control ), IDC_DISCOVERY_ON ), BM_GETCHECK, 0, 0 )
                      == BST_CHECKED;
        HKEY key;

        if (!RegCreateKeyExW( HKEY_LOCAL_MACHINE, network_key, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL ))
        {
            RegSetValueExW( key, L"NetworkDiscovery", 0, REG_DWORD, (const BYTE *)&value, sizeof(value) );
            RegCloseKey( key );
        }
        view_back( view );
        return TRUE;
    }
    case IDC_CANCEL:
        view_back( view );
        return TRUE;
    }
    return FALSE;
}

void sharing_page( struct view *view )
{
    BOOL on = discovery_enabled();
    HWND save;

    view_nav_home( view );
    view_nav_see_also( view, load_string( IDS_NETWORK ), open_network, 0 );

    view_text( view, STYLE_TITLE, load_string( IDS_SHARING_TITLE ) );
    view_space( view, px( 6 ) );
    view_text( view, STYLE_BODY, load_string( IDS_SHARING_INTRO ) );
    view_space( view, px( 14 ) );

    view_group( view, load_string( IDS_DISCOVERY ) );
    view_space( view, px( 6 ) );
    view_text_at( view, STYLE_BODY, load_string( IDS_DISCOVERY_TEXT ), INDENT );
    view_space( view, px( 8 ) );
    SendMessageW( view_control( view, L"Button", load_string( IDS_DISCOVERY_ON ),
                                BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, px( 420 ), px( 20 ), INDENT + px( 16 ),
                                IDC_DISCOVERY_ON ),
                  BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0 );
    SendMessageW( view_control( view, L"Button", load_string( IDS_DISCOVERY_OFF ), BS_AUTORADIOBUTTON, px( 420 ),
                                px( 20 ), INDENT + px( 16 ), IDC_DISCOVERY_OFF ),
                  BM_SETCHECK, on ? BST_UNCHECKED : BST_CHECKED, 0 );
    view_space( view, px( 14 ) );

    view_group( view, load_string( IDS_FILE_SHARING ) );
    view_space( view, px( 6 ) );
    view_text_at( view, STYLE_GRAY, load_string( IDS_FILE_SHARING_TEXT ), INDENT );
    view_space( view, px( 14 ) );

    view_group( view, load_string( IDS_DRIVES_PASSWORDS ) );
    view_space( view, px( 6 ) );
    view_text_at( view, STYLE_BODY, load_string( IDS_DRIVES_PASSWORDS_TEXT ), INDENT );
    view_space( view, px( 6 ) );
    view_link( view, load_string( IDS_MAP_DRIVE ), map_drive, 0, INDENT + px( 16 ) );
    view_space( view, px( 4 ) );
    view_link( view, load_string( IDS_DISCONNECT_DRIVE ), disconnect_drive, 0, INDENT + px( 16 ) );
    view_space( view, px( 4 ) );
    view_link( view, load_string( IDS_FORGET_PASSWORDS ), forget_passwords, 0, INDENT + px( 16 ) );
    view_space( view, px( 24 ) );

    view_row_begin( view );
    save = view_control_beside( view, L"Button", load_string( IDS_SAVE_CHANGES ), BS_PUSHBUTTON | WS_TABSTOP,
                                INDENT, px( 130 ), px( 26 ), IDC_SAVE );
    view_control_beside( view, L"Button", load_string( IDS_CANCEL ), BS_PUSHBUTTON | WS_TABSTOP,
                         INDENT + px( 140 ), px( 90 ), px( 26 ), IDC_CANCEL );
    view_row_end( view );
    EnableWindow( save, FALSE );
    view_space( view, px( 20 ) );
}
