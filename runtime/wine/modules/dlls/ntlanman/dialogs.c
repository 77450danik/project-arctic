/*
 * What the user is asked about the network: the name and password for a
 * computer, Map Network Drive and Disconnect Network Drives (the dialogs of
 * Windows' netplwiz.dll).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "ntlanman.h"
#include "wincred.h"
#include "commctrl.h"
#include "shlobj.h"
#include "shellapi.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(ntlanman);

#define MRU_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Map Network Drive MRU"

static void load_string( UINT id, WCHAR *text, UINT count )
{
    if (!LoadStringW( ntlanman_instance, id, text, count )) text[0] = 0;
}

/**********************************************************************
 *          The name and password for a computer
 */

BOOL ask_logon( HWND owner, const WCHAR *server, struct logon *logon, BOOL wrong, BOOL *save )
{
    WCHAR caption[128], message[400], format[200], user[CREDUI_MAX_USERNAME_LENGTH + 1] = L"";
    WCHAR password[CREDUI_MAX_PASSWORD_LENGTH + 1] = L"";
    CREDUI_INFOW info = { sizeof(info), owner, message, caption };
    DWORD flags = CREDUI_FLAGS_GENERIC_CREDENTIALS | CREDUI_FLAGS_DO_NOT_PERSIST | CREDUI_FLAGS_ALWAYS_SHOW_UI |
                  CREDUI_FLAGS_SHOW_SAVE_CHECK_BOX | CREDUI_FLAGS_EXCLUDE_CERTIFICATES;
    const WCHAR *sep;
    DWORD err;

    load_string( IDS_ENTER_PASSWORD, caption, ARRAY_SIZE(caption) );
    load_string( IDS_CONNECT_TO, format, ARRAY_SIZE(format) );
    swprintf( message, ARRAY_SIZE(message), L"%s%s", format, server );
    if (wrong)
    {
        flags |= CREDUI_FLAGS_INCORRECT_PASSWORD;
        if (logon->domain[0]) swprintf( user, ARRAY_SIZE(user), L"%s\\%s", logon->domain, logon->user );
        else lstrcpynW( user, logon->user, ARRAY_SIZE(user) );
    }
    *save = FALSE;
    err = CredUIPromptForCredentialsW( &info, server, NULL, wrong ? ERROR_LOGON_FAILURE : 0, user, ARRAY_SIZE(user),
                                       password, ARRAY_SIZE(password), save, flags );
    if (err || !user[0])
    {
        SecureZeroMemory( password, sizeof(password) );
        return FALSE;
    }
    memset( logon, 0, sizeof(*logon) );
    if ((sep = wcschr( user, '\\' )))
    {
        lstrcpynW( logon->domain, user, min( ARRAY_SIZE(logon->domain), (UINT)(sep - user) + 1 ) );
        lstrcpynW( logon->user, sep + 1, ARRAY_SIZE(logon->user) );
    }
    else lstrcpynW( logon->user, user, ARRAY_SIZE(logon->user) );
    lstrcpynW( logon->password, password, ARRAY_SIZE(logon->password) );
    SecureZeroMemory( password, sizeof(password) );
    return TRUE;
}

/* "Remember my credentials": in the Credential Manager, as Windows keeps them */
void save_logon( const WCHAR *server, const struct logon *logon )
{
    WCHAR user[520];
    CREDENTIALW cred = { 0 };

    if (logon->domain[0]) swprintf( user, ARRAY_SIZE(user), L"%s\\%s", logon->domain, logon->user );
    else wcscpy( user, logon->user );
    cred.Type = CRED_TYPE_DOMAIN_PASSWORD;
    cred.TargetName = (WCHAR *)server;
    cred.UserName = user;
    cred.CredentialBlob = (BYTE *)logon->password;
    cred.CredentialBlobSize = wcslen( logon->password ) * sizeof(WCHAR);
    cred.Persist = CRED_PERSIST_ENTERPRISE;
    if (!CredWriteW( &cred, 0 )) WARN( "not kept: %lu\n", GetLastError() );
}

static void logon_of_credential( const CREDENTIALW *cred, struct logon *logon )
{
    const WCHAR *sep;
    UINT chars;

    memset( logon, 0, sizeof(*logon) );
    if (!cred->UserName) return;
    if ((sep = wcschr( cred->UserName, '\\' )))
    {
        lstrcpynW( logon->domain, cred->UserName, min( ARRAY_SIZE(logon->domain), (UINT)(sep - cred->UserName) + 1 ) );
        lstrcpynW( logon->user, sep + 1, ARRAY_SIZE(logon->user) );
    }
    else lstrcpynW( logon->user, cred->UserName, ARRAY_SIZE(logon->user) );
    chars = min( cred->CredentialBlobSize / sizeof(WCHAR), ARRAY_SIZE(logon->password) - 1 );
    if (cred->CredentialBlob) memcpy( logon->password, cred->CredentialBlob, chars * sizeof(WCHAR) );
}

BOOL saved_logon( const WCHAR *server, struct logon *logon )
{
    CREDENTIALW *cred;

    if (!CredReadW( server, CRED_TYPE_DOMAIN_PASSWORD, 0, &cred )) return FALSE;
    logon_of_credential( cred, logon );
    CredFree( cred );
    return logon->user[0] != 0;
}

/* at logon: the host learns every kept logon, so that a program which names
 * \\server\share by itself gets in as Explorer would */
void push_saved_logons(void)
{
    CREDENTIALW **creds;
    struct logon logon;
    DWORD count, i;

    if (!CredEnumerateW( NULL, 0, &count, &creds )) return;
    for (i = 0; i < count; i++)
    {
        if (creds[i]->Type != CRED_TYPE_DOMAIN_PASSWORD || !creds[i]->TargetName) continue;
        logon_of_credential( creds[i], &logon );
        if (logon.user[0]) host_set_logon( creds[i]->TargetName, &logon );
    }
    SecureZeroMemory( &logon, sizeof(logon) );
    CredFree( creds );
}

/* rundll32 ntlanman.dll,ForgetLogons: the link of the Network and Sharing Center */
void WINAPI ForgetLogonsW( HWND hwnd, HINSTANCE instance, WCHAR *cmdline, int show )
{
    WCHAR title[128], format[500], text[600];
    CREDENTIALW **creds = NULL;
    struct logon nobody = { L"" };
    DWORD count = 0, i, ours = 0;

    SetThreadDpiAwarenessContext( DPI_AWARENESS_CONTEXT_SYSTEM_AWARE );
    load_string( IDS_FORGET_TITLE, title, ARRAY_SIZE(title) );
    if (CredEnumerateW( NULL, 0, &count, &creds ))
        for (i = 0; i < count; i++) if (creds[i]->Type == CRED_TYPE_DOMAIN_PASSWORD) ours++;
    if (!ours)
    {
        load_string( IDS_FORGET_NONE, text, ARRAY_SIZE(text) );
        MessageBoxW( hwnd, text, title, MB_OK | MB_ICONINFORMATION );
    }
    else
    {
        load_string( IDS_FORGET_ASK, format, ARRAY_SIZE(format) );
        swprintf( text, ARRAY_SIZE(text), format, ours );
        if (MessageBoxW( hwnd, text, title, MB_YESNO | MB_ICONQUESTION ) == IDYES)
        {
            for (i = 0; i < count; i++)
            {
                if (creds[i]->Type != CRED_TYPE_DOMAIN_PASSWORD || !creds[i]->TargetName) continue;
                /* the host forgets it too: the folders already open stay open */
                host_set_logon( creds[i]->TargetName, &nobody );
                CredDeleteW( creds[i]->TargetName, CRED_TYPE_DOMAIN_PASSWORD, 0 );
            }
        }
    }
    if (creds) CredFree( creds );
}

/**********************************************************************
 *          Map Network Drive
 */

struct map_params
{
    const WCHAR *remote;
    BOOL read_only;
    WCHAR letter;       /* the drive it became */
    HFONT heading;
};

static void remember_profile( WCHAR letter, const WCHAR *remote )
{
    WCHAR subkey[32], provider[128] = L"Microsoft Windows Network";
    DWORD value, size = sizeof(provider);
    HKEY key;

    RegGetValueW( HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\LanmanWorkstation\\NetworkProvider",
                  L"Name", RRF_RT_REG_SZ, NULL, provider, &size );
    swprintf( subkey, ARRAY_SIZE(subkey), L"Network\\%c", towupper( letter ) );
    if (RegCreateKeyExW( HKEY_CURRENT_USER, subkey, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL )) return;
    value = RESOURCETYPE_DISK;
    RegSetValueExW( key, L"ConnectionType", 0, REG_DWORD, (const BYTE *)&value, sizeof(value) );
    value = WNNC_NET_LANMAN;
    RegSetValueExW( key, L"ProviderType", 0, REG_DWORD, (const BYTE *)&value, sizeof(value) );
    value = 0;
    RegSetValueExW( key, L"DeferFlags", 0, REG_DWORD, (const BYTE *)&value, sizeof(value) );
    RegSetValueExW( key, L"ProviderName", 0, REG_SZ, (const BYTE *)provider,
                    (wcslen( provider ) + 1) * sizeof(WCHAR) );
    RegSetValueExW( key, L"RemotePath", 0, REG_SZ, (const BYTE *)remote, (wcslen( remote ) + 1) * sizeof(WCHAR) );
    RegSetValueExW( key, L"UserName", 0, REG_SZ, (const BYTE *)L"", sizeof(WCHAR) );
    RegCloseKey( key );
}

void forget_profile( WCHAR letter )
{
    WCHAR subkey[32];

    swprintf( subkey, ARRAY_SIZE(subkey), L"Network\\%c", towupper( letter ) );
    RegDeleteKeyW( HKEY_CURRENT_USER, subkey );
    swprintf( subkey, ARRAY_SIZE(subkey), L"Network\\%c", towlower( letter ) );
    RegDeleteKeyW( HKEY_CURRENT_USER, subkey );
}

/* the folders mapped before, the last one first (Explorer's own list) */
static void fill_recent( HWND combo )
{
    WCHAR order[32], name[2] = { 0 }, path[MAX_PATH];
    DWORD size = sizeof(order), i;
    HKEY key;

    if (RegOpenKeyExW( HKEY_CURRENT_USER, MRU_KEY, 0, KEY_READ, &key )) return;
    if (!RegGetValueW( key, NULL, L"MRUList", RRF_RT_REG_SZ, NULL, order, &size ))
    {
        for (i = 0; order[i]; i++)
        {
            name[0] = order[i];
            size = sizeof(path);
            if (!RegGetValueW( key, NULL, name, RRF_RT_REG_SZ, NULL, path, &size ))
                SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)path );
        }
    }
    RegCloseKey( key );
}

static void add_recent( const WCHAR *remote )
{
    WCHAR order[32] = L"", fresh[32], name[2] = { 0 }, path[MAX_PATH];
    DWORD size = sizeof(order), i, n = 0;
    WCHAR slot = 0;
    HKEY key;

    if (RegCreateKeyExW( HKEY_CURRENT_USER, MRU_KEY, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL )) return;
    RegGetValueW( key, NULL, L"MRUList", RRF_RT_REG_SZ, NULL, order, &size );
    for (i = 0; order[i]; i++)
    {
        name[0] = order[i];
        size = sizeof(path);
        if (!RegGetValueW( key, NULL, name, RRF_RT_REG_SZ, NULL, path, &size ) && !wcsicmp( path, remote ))
            slot = order[i];
    }
    if (!slot)
    {
        /* a letter not used yet, or the oldest one's */
        if (wcslen( order ) < 10)
        {
            for (slot = 'a'; wcschr( order, slot ); slot++) ;
        }
        else
        {
            slot = order[wcslen( order ) - 1];
            order[wcslen( order ) - 1] = 0;
        }
        name[0] = slot;
        RegSetValueExW( key, name, 0, REG_SZ, (const BYTE *)remote, (wcslen( remote ) + 1) * sizeof(WCHAR) );
    }
    fresh[n++] = slot;
    for (i = 0; order[i] && n < ARRAY_SIZE(fresh) - 1; i++)
        if (order[i] != slot) fresh[n++] = order[i];
    fresh[n] = 0;
    RegSetValueExW( key, L"MRUList", 0, REG_SZ, (const BYTE *)fresh, (n + 1) * sizeof(WCHAR) );
    RegCloseKey( key );
}

static void fill_letters( HWND combo )
{
    DWORD used = GetLogicalDrives();
    WCHAR text[8];
    int letter;

    for (letter = 'Z'; letter >= 'A'; letter--)
    {
        int index;

        if (used & (1u << (letter - 'A'))) continue;
        swprintf( text, ARRAY_SIZE(text), L"%c:", letter );
        index = SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)text );
        SendMessageW( combo, CB_SETITEMDATA, index, letter );
    }
    SendMessageW( combo, CB_SETCURSEL, 0, 0 );
}

static void browse_network( HWND dialog )
{
    WCHAR title[128], path[MAX_PATH];
    BROWSEINFOW info = { dialog };
    ITEMIDLIST *root = NULL, *picked;

    load_string( IDS_BROWSE_TITLE, title, ARRAY_SIZE(title) );
    SHGetSpecialFolderLocation( dialog, CSIDL_NETWORK, &root );
    info.pidlRoot = root;
    info.lpszTitle = title;
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_NONEWFOLDERBUTTON;
    if ((picked = SHBrowseForFolderW( &info )))
    {
        if (SHGetPathFromIDListW( picked, path ) && path[0] == '\\')
            SetDlgItemTextW( dialog, IDC_FOLDER, path );
        CoTaskMemFree( picked );
    }
    CoTaskMemFree( root );
}

static void show_error( HWND dialog, UINT format_id, const WCHAR *path, DWORD err )
{
    WCHAR format[300], text[900], reason[400] = L"", title[128];

    load_string( format_id, format, ARRAY_SIZE(format) );
    load_string( IDS_MAP_TITLE, title, ARRAY_SIZE(title) );
    if (err)
    {
        FormatMessageW( FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err, 0, reason,
                        ARRAY_SIZE(reason), NULL );
        swprintf( text, ARRAY_SIZE(text), format, reason );
    }
    else swprintf( text, ARRAY_SIZE(text), format, path );
    MessageBoxW( dialog, text, title, MB_OK | MB_ICONERROR );
}

static BOOL map_drive( HWND dialog, struct map_params *params )
{
    WCHAR remote[MAX_PATH], server[64], share[84], local[3] = L"Z:", root[4], clean[MAX_PATH];
    int index = SendDlgItemMessageW( dialog, IDC_DRIVE, CB_GETCURSEL, 0, 0 );
    DWORD flags = CONNECT_INTERACTIVE, err;
    HCURSOR old;
    UINT len;

    GetDlgItemTextW( dialog, IDC_FOLDER, remote, ARRAY_SIZE(remote) );
    for (len = wcslen( remote ); len && (remote[len - 1] == ' ' || remote[len - 1] == '\\'); len--) remote[len - 1] = 0;
    if (index < 0 || !split_unc( remote, server, ARRAY_SIZE(server), share, ARRAY_SIZE(share), NULL ) || !share[0])
    {
        show_error( dialog, IDS_NOT_FOUND, remote, 0 );
        SetFocus( GetDlgItem( dialog, IDC_FOLDER ) );
        return FALSE;
    }
    local[0] = SendDlgItemMessageW( dialog, IDC_DRIVE, CB_GETITEMDATA, index, 0 );
    if (IsDlgButtonChecked( dialog, IDC_OTHER_USER ) == BST_CHECKED) flags |= CONNECT_PROMPT;
    swprintf( clean, ARRAY_SIZE(clean), L"\\\\%s\\%s", server, share );

    old = SetCursor( LoadCursorW( NULL, (const WCHAR *)IDC_WAIT ) );
    err = connect_resource( dialog, clean, local, NULL, NULL, flags );
    SetCursor( old );
    if (err == WN_CANCEL) return FALSE;
    if (err)
    {
        show_error( dialog, IDS_CANNOT_CONNECT, clean, err );
        return FALSE;
    }
    if (IsDlgButtonChecked( dialog, IDC_RECONNECT ) == BST_CHECKED) remember_profile( local[0], clean );
    add_recent( clean );
    params->letter = local[0];
    swprintf( root, ARRAY_SIZE(root), L"%c:\\", local[0] );
    SHChangeNotify( SHCNE_DRIVEADD, SHCNF_PATHW, root, NULL );
    ShellExecuteW( NULL, L"open", root, NULL, NULL, SW_SHOWNORMAL );
    return TRUE;
}

static INT_PTR CALLBACK map_proc( HWND dialog, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct map_params *params = (struct map_params *)GetWindowLongPtrW( dialog, DWLP_USER );

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        LOGFONTW font;

        params = (struct map_params *)lparam;
        SetWindowLongPtrW( dialog, DWLP_USER, lparam );
        GetObjectW( (HFONT)SendMessageW( dialog, WM_GETFONT, 0, 0 ), sizeof(font), &font );
        font.lfHeight = font.lfHeight * 4 / 3;
        params->heading = CreateFontIndirectW( &font );
        SendDlgItemMessageW( dialog, IDC_BANNER, WM_SETFONT, (WPARAM)params->heading, FALSE );
        fill_letters( GetDlgItem( dialog, IDC_DRIVE ) );
        fill_recent( GetDlgItem( dialog, IDC_FOLDER ) );
        SendDlgItemMessageW( dialog, IDC_FOLDER, CB_LIMITTEXT, MAX_PATH - 1, 0 );
        CheckDlgButton( dialog, IDC_RECONNECT, BST_CHECKED );
        if (params->remote)
        {
            SetDlgItemTextW( dialog, IDC_FOLDER, params->remote );
            if (params->read_only)
            {
                EnableWindow( GetDlgItem( dialog, IDC_FOLDER ), FALSE );
                EnableWindow( GetDlgItem( dialog, IDC_BROWSE ), FALSE );
            }
        }
        return TRUE;
    }
    case WM_CTLCOLORSTATIC:
        if (GetDlgCtrlID( (HWND)lparam ) == IDC_BANNER)
        {
            SetTextColor( (HDC)wparam, RGB( 0, 51, 153 ) );
            SetBkMode( (HDC)wparam, TRANSPARENT );
            return (INT_PTR)GetSysColorBrush( COLOR_3DFACE );
        }
        break;
    case WM_COMMAND:
        switch (LOWORD(wparam))
        {
        case IDC_BROWSE:
            browse_network( dialog );
            return TRUE;
        case IDOK:
            if (map_drive( dialog, params )) EndDialog( dialog, IDOK );
            return TRUE;
        case IDCANCEL:
            EndDialog( dialog, IDCANCEL );
            return TRUE;
        }
        break;
    case WM_DESTROY:
        if (params && params->heading) DeleteObject( params->heading );
        break;
    }
    return FALSE;
}

/* what mpr.dll's WNetConnectionDialog1 shows */
DWORD WINAPI ConnectionDialog1W( CONNECTDLGSTRUCTW *dlg )
{
    struct map_params params = { 0 };
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES };

    if (!dlg || !dlg->lpConnRes) return WN_BAD_POINTER;
    if (dlg->lpConnRes->dwType == RESOURCETYPE_PRINT) return WN_BAD_DEV_TYPE;
    InitCommonControlsEx( &icc );
    if (dlg->lpConnRes->lpRemoteName && dlg->lpConnRes->lpRemoteName[0])
    {
        params.remote = dlg->lpConnRes->lpRemoteName;
        params.read_only = (dlg->dwFlags & CONNDLG_RO_PATH) != 0;
    }
    if (DialogBoxParamW( ntlanman_instance, MAKEINTRESOURCEW( IDD_MAP_DRIVE ), dlg->hwndOwner, map_proc,
                         (LPARAM)&params ) != IDOK)
        return ~0u;
    dlg->dwDevNum = towupper( params.letter ) - 'A' + 1;
    return WN_SUCCESS;
}

/* rundll32 ntlanman.dll,MapNetworkDrive [\\server\share]: the verb of This PC and of Network */
void WINAPI MapNetworkDriveW( HWND hwnd, HINSTANCE instance, WCHAR *cmdline, int show )
{
    NETRESOURCEW resource = { 0 };
    CONNECTDLGSTRUCTW dlg = { sizeof(dlg), hwnd, &resource };

    /* rundll32 is not DPI aware: the dialog draws at the screen's scale, as in Windows */
    SetThreadDpiAwarenessContext( DPI_AWARENESS_CONTEXT_SYSTEM_AWARE );
    resource.dwType = RESOURCETYPE_DISK;
    if (cmdline && cmdline[0] == '\\' && cmdline[1] == '\\') resource.lpRemoteName = cmdline;
    ConnectionDialog1W( &dlg );
}

/**********************************************************************
 *          Disconnect Network Drives
 */

static void fill_drives( HWND list )
{
    WCHAR remote[MAX_PATH], text[MAX_PATH + 16], root[4];
    SHFILEINFOW info;
    LVITEMW item = { LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM };
    HIMAGELIST images;
    WCHAR letter;

    wcscpy( root, L"C:\\" );
    images = (HIMAGELIST)SHGetFileInfoW( root, 0, &info, sizeof(info), SHGFI_SYSICONINDEX | SHGFI_SMALLICON );
    SendMessageW( list, LVM_SETIMAGELIST, LVSIL_SMALL, (LPARAM)images );
    for (letter = 'a'; letter <= 'z'; letter++)
    {
        if (!drive_remote( letter, remote, ARRAY_SIZE(remote) )) continue;
        swprintf( root, ARRAY_SIZE(root), L"%c:\\", towupper( letter ) );
        swprintf( text, ARRAY_SIZE(text), L"%c:   %s", towupper( letter ), remote );
        memset( &info, 0, sizeof(info) );
        SHGetFileInfoW( root, FILE_ATTRIBUTE_DIRECTORY, &info, sizeof(info),
                        SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES );
        item.iItem = 0x7fffffff;
        item.pszText = text;
        item.iImage = info.iIcon;
        item.lParam = letter;
        SendMessageW( list, LVM_INSERTITEMW, 0, (LPARAM)&item );
    }
}

static INT_PTR CALLBACK disconnect_proc( HWND dialog, UINT msg, WPARAM wparam, LPARAM lparam )
{
    HWND list = GetDlgItem( dialog, IDC_DRIVES );

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        LVCOLUMNW column = { LVCF_WIDTH };
        RECT rect;

        GetClientRect( list, &rect );
        column.cx = rect.right - GetSystemMetrics( SM_CXVSCROLL );
        SendMessageW( list, LVM_INSERTCOLUMNW, 0, (LPARAM)&column );
        fill_drives( list );
        EnableWindow( GetDlgItem( dialog, IDOK ), FALSE );
        return TRUE;
    }
    case WM_NOTIFY:
        if (((NMHDR *)lparam)->idFrom == IDC_DRIVES && ((NMHDR *)lparam)->code == LVN_ITEMCHANGED)
            EnableWindow( GetDlgItem( dialog, IDOK ), SendMessageW( list, LVM_GETSELECTEDCOUNT, 0, 0 ) != 0 );
        break;
    case WM_COMMAND:
        if (LOWORD(wparam) == IDOK)
        {
            int index = -1;

            while ((index = SendMessageW( list, LVM_GETNEXTITEM, index, LVNI_SELECTED )) >= 0)
            {
                LVITEMW item = { LVIF_PARAM, index };
                WCHAR root[4];

                SendMessageW( list, LVM_GETITEMW, 0, (LPARAM)&item );
                if (drive_remove( item.lParam )) continue;
                forget_profile( item.lParam );
                swprintf( root, ARRAY_SIZE(root), L"%c:\\", towupper( (WCHAR)item.lParam ) );
                SHChangeNotify( SHCNE_DRIVEREMOVED, SHCNF_PATHW, root, NULL );
            }
            EndDialog( dialog, IDOK );
            return TRUE;
        }
        if (LOWORD(wparam) == IDCANCEL)
        {
            EndDialog( dialog, IDCANCEL );
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* what mpr.dll's WNetDisconnectDialog shows */
DWORD WINAPI DisconnectDialog( HWND owner, DWORD type )
{
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES };
    WCHAR remote[MAX_PATH], text[300], title[128], letter;

    if (type == RESOURCETYPE_PRINT) return WN_BAD_DEV_TYPE;
    for (letter = 'a'; letter <= 'z'; letter++)
        if (drive_remote( letter, remote, ARRAY_SIZE(remote) )) break;
    if (letter > 'z')
    {
        load_string( IDS_DISCONNECT_NONE, text, ARRAY_SIZE(text) );
        load_string( IDS_NETWORK_DRIVE, title, ARRAY_SIZE(title) );
        MessageBoxW( owner, text, title, MB_OK | MB_ICONINFORMATION );
        return WN_SUCCESS;
    }
    InitCommonControlsEx( &icc );
    return DialogBoxW( ntlanman_instance, MAKEINTRESOURCEW( IDD_DISCONNECT ), owner, disconnect_proc ) == IDOK
           ? WN_SUCCESS : ~0u;
}

void WINAPI DisconnectNetworkDrivesW( HWND hwnd, HINSTANCE instance, WCHAR *cmdline, int show )
{
    SetThreadDpiAwarenessContext( DPI_AWARENESS_CONTEXT_SYSTEM_AWARE );
    DisconnectDialog( hwnd, RESOURCETYPE_DISK );
}
