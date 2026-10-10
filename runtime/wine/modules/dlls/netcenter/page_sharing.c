/*
 * Network and Sharing Center: Change advanced sharing settings
 *
 * Windows' page (hgcpl.dll's, its texts kept): network discovery, file and
 * printer sharing, Public folder sharing, password protected sharing; and the
 * network drives and the passwords kept for the computers of the network.
 * The switches are kept in the registry and told to the host, which serves
 * the folders and answers those who look for computers (host/init/arctic
 * -smbd.c): C:\ProgramData\Arctic\Smb\config, and the Public folder as a share
 * like the ones of the Sharing page of a folder (ntshrui.dll). Password
 * protected sharing is kept and shown and changes nothing yet: whoever comes
 * from the network is a guest, there is no account he could be asked the
 * password of. docs/M5-network.md.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdio.h>

#include "netcenter.h"
#include "shlobj.h"

#define INDENT          px(20)

#define IDC_DISCOVERY_ON    100
#define IDC_DISCOVERY_OFF   101
#define IDC_SAVE            102
#define IDC_CANCEL          103
#define IDC_SHARING_ON      104
#define IDC_SHARING_OFF     105
#define IDC_PUBLIC_ON       106
#define IDC_PUBLIC_OFF      107
#define IDC_PASSWORD_ON     108
#define IDC_PASSWORD_OFF    109

static const WCHAR network_key[] = L"SOFTWARE\\Arctic\\Network";
static const WCHAR server_key[] = L"SYSTEM\\CurrentControlSet\\Services\\LanmanServer\\Parameters";
static const WCHAR smb_dir[] = L"C:\\ProgramData\\Arctic\\Smb";

static BOOL setting( const WCHAR *key, const WCHAR *name, DWORD value )
{
    DWORD size = sizeof(value);

    RegGetValueW( HKEY_LOCAL_MACHINE, key, name, RRF_RT_REG_DWORD, NULL, &value, &size );
    return value != 0;
}

static void set_setting( const WCHAR *key, const WCHAR *name, DWORD value )
{
    HKEY hkey;

    if (RegCreateKeyExW( HKEY_LOCAL_MACHINE, key, 0, NULL, 0, KEY_SET_VALUE, NULL, &hkey, NULL )) return;
    RegSetValueExW( hkey, name, 0, REG_DWORD, (const BYTE *)&value, sizeof(value) );
    RegCloseKey( hkey );
}

/* written beside and moved over: the host reads a whole file or the one before */
static void write_file( const WCHAR *name, const char *text )
{
    WCHAR path[MAX_PATH], temp[MAX_PATH];
    DWORD written;
    HANDLE file;

    swprintf( path, ARRAY_SIZE(path), L"%s\\%s", smb_dir, name );
    swprintf( temp, ARRAY_SIZE(temp), L"%s.new", path );
    file = CreateFileW( temp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
    if (file == INVALID_HANDLE_VALUE) return;
    WriteFile( file, text, strlen( text ), &written, NULL );
    CloseHandle( file );
    MoveFileExW( temp, path, MOVEFILE_REPLACE_EXISTING );
}

static const WCHAR public_share[] = L"C:\\ProgramData\\Arctic\\Smb\\shares\\Public.share";
static const char public_path[] = "path=C:\\Users\\Public\r\n";

/* The switch of the Public folder is that folder shared as "Public": the same
 * share its own Sharing page shows, makes and removes, so the page and the
 * switch never say different things. A share of another folder that took the
 * name is not the switch's. */
static BOOL public_shared(void)
{
    char text[64] = "";
    DWORD size = 0;
    HANDLE file = CreateFileW( public_share, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL );

    if (file == INVALID_HANDLE_VALUE) return FALSE;
    ReadFile( file, text, sizeof(text) - 1, &size, NULL );
    CloseHandle( file );
    return !_strnicmp( text, public_path, strlen( public_path ) );
}

/* anyone on the network reads and writes there, as the switch says */
static void share_public( BOOL share )
{
    char text[128];

    if (share == public_shared()) return;
    if (!share)
    {
        DeleteFileW( public_share );
        return;
    }
    if (GetFileAttributesW( public_share ) != INVALID_FILE_ATTRIBUTES) return; /* the name is taken */
    SHCreateDirectoryExW( NULL, L"C:\\Users\\Public", NULL );
    sprintf( text, "%scomment=\r\nallow=full\r\ndeny=none\r\nlimit=20\r\ncaching=manual\r\n", public_path );
    write_file( L"shares\\Public.share", text );
}

/* What the registry says, told to the host: the two switches it serves by.
 * At every logon as well (a registry that came back from a copy, a C:
 * without the host's files yet). */
static void apply_settings(void)
{
    char config[64];

    SHCreateDirectoryExW( NULL, L"C:\\ProgramData\\Arctic\\Smb\\shares", NULL );
    sprintf( config, "sharing=%d\r\ndiscovery=%d\r\n", setting( server_key, L"FileSharing", 0 ),
             setting( network_key, L"NetworkDiscovery", 1 ) );
    write_file( L"config", config );
}

/* rundll32 netcenter.dll,ApplySharingSettings */
void WINAPI ApplySharingSettings( HWND hwnd, HINSTANCE instance, LPSTR cmdline, int show )
{
    apply_settings();
}

/* rundll32 netcenter.dll,ShowAdvancedSharing */
void WINAPI ShowAdvancedSharing( HWND hwnd, HINSTANCE instance, LPSTR cmdline, int show )
{
    cp_open( 1, 0 );
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

static BOOL checked( HWND parent, UINT id )
{
    return SendMessageW( GetDlgItem( parent, id ), BM_GETCHECK, 0, 0 ) == BST_CHECKED;
}

BOOL sharing_command( struct view *view, UINT id, UINT code, HWND control )
{
    if (code != BN_CLICKED) return FALSE;
    switch (id)
    {
    case IDC_DISCOVERY_ON:
    case IDC_DISCOVERY_OFF:
    case IDC_SHARING_ON:
    case IDC_SHARING_OFF:
    case IDC_PUBLIC_ON:
    case IDC_PUBLIC_OFF:
    case IDC_PASSWORD_ON:
    case IDC_PASSWORD_OFF:
        EnableWindow( GetDlgItem( GetParent( control ), IDC_SAVE ), TRUE );
        return TRUE;
    case IDC_SAVE:
    {
        HWND parent = GetParent( control );

        set_setting( network_key, L"NetworkDiscovery", checked( parent, IDC_DISCOVERY_ON ) );
        set_setting( server_key, L"FileSharing", checked( parent, IDC_SHARING_ON ) );
        set_setting( server_key, L"PasswordProtectedSharing", checked( parent, IDC_PASSWORD_ON ) );
        apply_settings();
        share_public( checked( parent, IDC_PUBLIC_ON ) );
        view_back( view );
        return TRUE;
    }
    case IDC_CANCEL:
        view_back( view );
        return TRUE;
    }
    return FALSE;
}

/* a group of the page: its heading, what it is about, on or off */
static void radio_group( struct view *view, const WCHAR *title, const WCHAR *text, const WCHAR *on_text,
                         const WCHAR *off_text, UINT id_on, UINT id_off, BOOL on )
{
    const WCHAR *texts[2] = { on_text, off_text };

    view_group( view, title );
    view_space( view, px( 6 ) );
    view_text_at( view, STYLE_BODY, text, INDENT );
    view_space( view, px( 8 ) );
    for (int i = 0; i < 2; i++)
    {
        /* Windows' longer choices take two lines */
        BOOL lines = wcslen( texts[i] ) > 70;
        HWND radio = view_control( view, L"Button", texts[i],
                                   BS_AUTORADIOBUTTON | (i ? 0 : WS_GROUP | WS_TABSTOP) | (lines ? BS_MULTILINE | BS_TOP : 0),
                                   px( 520 ), px( lines ? 36 : 20 ), INDENT + px( 16 ), i ? id_off : id_on );

        SendMessageW( radio, BM_SETCHECK, on == !i ? BST_CHECKED : BST_UNCHECKED, 0 );
    }
    view_space( view, px( 14 ) );
}

void sharing_page( struct view *view )
{
    HWND save;

    view_nav_home( view );
    view_nav_see_also( view, load_string( IDS_NETWORK ), open_network, 0 );

    view_text( view, STYLE_TITLE, load_string( IDS_SHARING_TITLE ) );
    view_space( view, px( 6 ) );
    view_text( view, STYLE_BODY, load_string( IDS_SHARING_INTRO ) );
    view_space( view, px( 14 ) );

    radio_group( view, load_string( IDS_DISCOVERY ), load_string( IDS_DISCOVERY_TEXT ),
                load_string( IDS_DISCOVERY_ON ), load_string( IDS_DISCOVERY_OFF ), IDC_DISCOVERY_ON,
                IDC_DISCOVERY_OFF, setting( network_key, L"NetworkDiscovery", 1 ) );

    radio_group( view, load_string( IDS_FILE_SHARING ), load_string( IDS_FILE_SHARING_TEXT ),
                load_string( IDS_FILE_SHARING_ON ), load_string( IDS_FILE_SHARING_OFF ), IDC_SHARING_ON,
                IDC_SHARING_OFF, setting( server_key, L"FileSharing", 0 ) );

    radio_group( view, load_string( IDS_PUBLIC_SHARING ), load_string( IDS_PUBLIC_SHARING_TEXT ),
                load_string( IDS_PUBLIC_SHARING_ON ), load_string( IDS_PUBLIC_SHARING_OFF ), IDC_PUBLIC_ON,
                IDC_PUBLIC_OFF, public_shared() );

    radio_group( view, load_string( IDS_PASSWORD_SHARING ), load_string( IDS_PASSWORD_SHARING_TEXT ),
                load_string( IDS_PASSWORD_SHARING_ON ), load_string( IDS_PASSWORD_SHARING_OFF ), IDC_PASSWORD_ON,
                IDC_PASSWORD_OFF, setting( server_key, L"PasswordProtectedSharing", 1 ) );

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
