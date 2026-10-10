/*
 * "Share...": the wizard that asks whom a folder is shared with, and says
 * where the others find it. Its two pages are Windows' own (dialogs_uk
 * .rcinc); the window around them stands in for the Aero wizard of
 * Windows, which the common controls here do not have.
 *
 * The people to share with are the owner, who is there already, and
 * Everyone (perms.c says why nobody else): reading, or reading and writing.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "ntshrui.h"
#include "shlobj.h"
#include "shlwapi.h"
#include "shellapi.h"
#include "lmcons.h"

#define MENU_READ   1
#define MENU_WRITE  2
#define MENU_REMOVE 3

struct wizard
{
    const WCHAR *folder;
    HWND frame, page;
    HFONT title_font;
    BOOL everyone, done;
    int level;                  /* RIGHT_READ, or RIGHT_FULL for reading and writing */
    struct share share;         /* what the folder is shared as, or will be */
    BOOL shared;
};

static void people_show( HWND hwnd, const struct wizard *wizard )
{
    HWND list = GetDlgItem( hwnd, IDC_WIZARD_PEOPLE );
    WCHAR user[UNLEN + 1] = L"", level[100];
    DWORD size = ARRAY_SIZE(user);
    LVITEMW item = { LVIF_TEXT };

    GetUserNameW( user, &size );
    SendMessageW( list, LVM_DELETEALLITEMS, 0, 0 );
    item.pszText = user;
    SendMessageW( list, LVM_INSERTITEMW, 0, (LPARAM)&item );
    item.iSubItem = 1;
    item.pszText = load_text( IDS_LEVEL_OWNER );
    SendMessageW( list, LVM_SETITEMW, 0, (LPARAM)&item );
    if (!wizard->everyone) return;
    item.iItem = 1;
    item.iSubItem = 0;
    item.pszText = load_text( IDS_EVERYONE );
    SendMessageW( list, LVM_INSERTITEMW, 0, (LPARAM)&item );
    /* the level, and the arrow that says it can be changed */
    swprintf( level, ARRAY_SIZE(level), L"%s \x25bc",
              load_text( wizard->level == RIGHT_READ ? IDS_LEVEL_READ : IDS_LEVEL_WRITE ) );
    item.iSubItem = 1;
    item.pszText = level;
    SendMessageW( list, LVM_SETITEMW, 0, (LPARAM)&item );
}

static void people_add( HWND hwnd, struct wizard *wizard )
{
    WCHAR typed[200], user[UNLEN + 1] = L"";
    DWORD size = ARRAY_SIZE(user);

    GetDlgItemTextW( hwnd, IDC_WIZARD_WHO, typed, ARRAY_SIZE(typed) );
    StrTrimW( typed, L" " );
    if (!typed[0]) return;
    GetUserNameW( user, &size );
    if (!lstrcmpiW( typed, L"Everyone" ) || !lstrcmpiW( typed, load_text( IDS_EVERYONE ) ))
    {
        if (!wizard->everyone)
        {
            wizard->everyone = TRUE;
            wizard->level = RIGHT_READ;
        }
    }
    else if (lstrcmpiW( typed, user )) /* the owner is in the list already; anybody else is nobody */
    {
        message( wizard->frame, IDS_NOT_FOUND, typed, MB_ICONINFORMATION );
        return;
    }
    SetDlgItemTextW( hwnd, IDC_WIZARD_WHO, L"" );
    people_show( hwnd, wizard );
}

/* the level of Everyone was clicked: reading, reading and writing, or not at all */
static void people_level( HWND hwnd, struct wizard *wizard )
{
    HMENU menu = CreatePopupMenu();
    POINT pt;

    GetCursorPos( &pt );
    AppendMenuW( menu, MF_STRING | (wizard->level == RIGHT_READ ? MF_CHECKED : 0), MENU_READ,
                 load_text( IDS_LEVEL_READ ) );
    AppendMenuW( menu, MF_STRING | (wizard->level != RIGHT_READ ? MF_CHECKED : 0), MENU_WRITE,
                 load_text( IDS_LEVEL_WRITE ) );
    AppendMenuW( menu, MF_SEPARATOR, 0, NULL );
    AppendMenuW( menu, MF_STRING, MENU_REMOVE, load_text( IDS_REMOVE_LEVEL ) );
    switch (TrackPopupMenu( menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, NULL ))
    {
    case MENU_READ:
        wizard->level = RIGHT_READ;
        break;
    case MENU_WRITE:
        wizard->level = RIGHT_FULL;
        break;
    case MENU_REMOVE:
        wizard->everyone = FALSE;
        break;
    }
    DestroyMenu( menu );
    people_show( hwnd, wizard );
}

static INT_PTR white( WPARAM wparam )
{
    SetBkColor( (HDC)wparam, GetSysColor( COLOR_WINDOW ) );
    return (INT_PTR)GetSysColorBrush( COLOR_WINDOW );
}

static INT_PTR CALLBACK people_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct wizard *wizard = (struct wizard *)GetWindowLongPtrW( hwnd, DWLP_USER );

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        HWND list = GetDlgItem( hwnd, IDC_WIZARD_PEOPLE ), who = GetDlgItem( hwnd, IDC_WIZARD_WHO );
        WCHAR user[UNLEN + 1] = L"";
        DWORD size = ARRAY_SIZE(user);
        LVCOLUMNW column = { LVCF_TEXT | LVCF_WIDTH };
        RECT rect;

        wizard = (struct wizard *)lparam;
        SetWindowLongPtrW( hwnd, DWLP_USER, lparam );
        SetDlgItemTextW( hwnd, IDC_WIZARD_INTRO, load_text( IDS_TYPE_A_NAME ) );
        ShowWindow( GetDlgItem( hwnd, IDC_WIZARD_HELP ), SW_HIDE ); /* there is no help to open */
        GetUserNameW( user, &size );
        SendMessageW( who, CB_ADDSTRING, 0, (LPARAM)user );
        SendMessageW( who, CB_ADDSTRING, 0, (LPARAM)load_text( IDS_EVERYONE ) );
        SendMessageW( list, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT );
        GetClientRect( list, &rect );
        column.cx = rect.right * 3 / 5;
        column.pszText = load_text( IDS_COLUMN_NAME );
        SendMessageW( list, LVM_INSERTCOLUMNW, 0, (LPARAM)&column );
        column.cx = rect.right - column.cx;
        column.pszText = load_text( IDS_COLUMN_LEVEL );
        SendMessageW( list, LVM_INSERTCOLUMNW, 1, (LPARAM)&column );
        people_show( hwnd, wizard );
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == IDC_WIZARD_ADD)
        {
            people_add( hwnd, wizard );
            return TRUE;
        }
        break;
    case WM_NOTIFY:
    {
        NMITEMACTIVATE *click = (NMITEMACTIVATE *)lparam;

        if (click->hdr.idFrom == IDC_WIZARD_PEOPLE && click->hdr.code == NM_CLICK && click->iItem == 1 &&
            click->iSubItem == 1)
            people_level( hwnd, wizard );
        break;
    }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
        return white( wparam );
    }
    return FALSE;
}

/* \\COMPUTER\share */
static void share_path( const struct share *share, WCHAR *path, UINT count )
{
    WCHAR computer[MAX_COMPUTERNAME_LENGTH + 1] = L"";
    DWORD size = ARRAY_SIZE(computer);

    GetComputerNameW( computer, &size );
    swprintf( path, count, L"\\\\%s\\%s", computer, share->name );
}

static void copy_text( HWND hwnd, const WCHAR *text )
{
    UINT bytes = (wcslen( text ) + 1) * sizeof(WCHAR);
    HGLOBAL memory;

    if (!OpenClipboard( hwnd )) return;
    EmptyClipboard();
    if ((memory = GlobalAlloc( GMEM_MOVEABLE, bytes )))
    {
        memcpy( GlobalLock( memory ), text, bytes );
        GlobalUnlock( memory );
        if (!SetClipboardData( CF_UNICODETEXT, memory )) GlobalFree( memory );
    }
    CloseClipboard();
}

static INT_PTR CALLBACK done_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct wizard *wizard = (struct wizard *)GetWindowLongPtrW( hwnd, DWLP_USER );
    WCHAR path[200], title[MAX_PATH];

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        HWND list = GetDlgItem( hwnd, IDC_DONE_LIST );
        LVCOLUMNW column = { LVCF_WIDTH };
        LVITEMW item = { LVIF_TEXT | LVIF_IMAGE };
        SHFILEINFOW info = { 0 };
        HIMAGELIST icons;
        RECT rect;

        wizard = (struct wizard *)lparam;
        SetWindowLongPtrW( hwnd, DWLP_USER, lparam );
        SetDlgItemTextW( hwnd, IDC_DONE_TEXT, load_text( IDS_LINKS ) );
        SetDlgItemTextW( hwnd, IDC_DONE_SLEEP, load_text( IDS_SLEEP_NOTE ) );
        SetDlgItemTextW( hwnd, IDC_DONE_SHARES, load_text( IDS_SHOW_SHARES ) );
        SendDlgItemMessageW( hwnd, IDC_DONE_ICON, STM_SETICON,
                             (WPARAM)LoadImageW( NULL, (const WCHAR *)IDI_INFORMATION, IMAGE_ICON,
                                                 GetSystemMetrics( SM_CXSMICON ), GetSystemMetrics( SM_CYSMICON ),
                                                 LR_SHARED ), 0 );
        /* the folder and where the others find it */
        SetWindowLongW( list, GWL_STYLE, (GetWindowLongW( list, GWL_STYLE ) & ~LVS_TYPEMASK) | LVS_REPORT |
                                         LVS_NOCOLUMNHEADER | LVS_SHAREIMAGELISTS );
        icons = (HIMAGELIST)SHGetFileInfoW( wizard->folder, 0, &info, sizeof(info), SHGFI_SYSICONINDEX | SHGFI_SMALLICON );
        SendMessageW( list, LVM_SETIMAGELIST, LVSIL_SMALL, (LPARAM)icons );
        GetClientRect( list, &rect );
        column.cx = rect.right * 2 / 5;
        SendMessageW( list, LVM_INSERTCOLUMNW, 0, (LPARAM)&column );
        column.cx = rect.right - column.cx;
        SendMessageW( list, LVM_INSERTCOLUMNW, 1, (LPARAM)&column );
        folder_title( wizard->folder, title, ARRAY_SIZE(title) );
        share_path( &wizard->share, path, ARRAY_SIZE(path) );
        item.pszText = title;
        item.iImage = info.iIcon;
        SendMessageW( list, LVM_INSERTITEMW, 0, (LPARAM)&item );
        item.mask = LVIF_TEXT;
        item.iSubItem = 1;
        item.pszText = path;
        SendMessageW( list, LVM_SETITEMW, 0, (LPARAM)&item );
        return TRUE;
    }
    case WM_NOTIFY:
    {
        NMLINK *link = (NMLINK *)lparam;

        if (link->hdr.code != NM_CLICK && link->hdr.code != NM_RETURN) break;
        share_path( &wizard->share, path, ARRAY_SIZE(path) );
        if (link->hdr.idFrom == IDC_DONE_TEXT && !wcscmp( link->item.szUrl, L"CopyLinks" ))
        {
            copy_text( hwnd, path );
        }
        else if (link->hdr.idFrom == IDC_DONE_TEXT)
        {
            /* a letter with the link, for the mail program to send */
            WCHAR letter[1200], *slash;

            folder_title( wizard->folder, title, ARRAY_SIZE(title) );
            while ((slash = wcschr( path, '\\' ))) *slash = '/';
            swprintf( letter, ARRAY_SIZE(letter), L"mailto:?subject=%s&body=%s%%0D%%0A%%0D%%0A%s (file:%s)",
                      load_text( IDS_MAIL_SUBJECT ), load_text( IDS_MAIL_BODY ), title, path );
            ShellExecuteW( hwnd, NULL, letter, NULL, NULL, SW_SHOWNORMAL );
        }
        else if (link->hdr.idFrom == IDC_DONE_SLEEP)
        {
            ShellExecuteW( hwnd, NULL, L"control.exe", L"powercfg.cpl", NULL, SW_SHOWNORMAL );
        }
        else if (link->hdr.idFrom == IDC_DONE_SHARES)
        {
            WCHAR computer[MAX_COMPUTERNAME_LENGTH + 3] = L"\\\\";
            DWORD size = MAX_COMPUTERNAME_LENGTH + 1;

            GetComputerNameW( computer + 2, &size );
            ShellExecuteW( hwnd, NULL, computer, NULL, NULL, SW_SHOWNORMAL );
        }
        return TRUE;
    }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
        return white( wparam );
    }
    return FALSE;
}

/* the folder is shared with whom the list has now */
static BOOL wizard_share( struct wizard *wizard )
{
    WCHAR theirs[MAX_PATH], base[81];
    struct share *share = &wizard->share;

    if (!wizard->shared)
    {
        /* its own name; "Name2" when another folder is shared under it */
        lstrcpynW( base, share->name, ARRAY_SIZE(base) - 4 );
        for (UINT n = 2; n < 100 && share_owner( share->name, theirs, ARRAY_SIZE(theirs) ) &&
                         lstrcmpiW( theirs, wizard->folder ); n++)
            swprintf( share->name, ARRAY_SIZE(share->name), L"%s%u", base, n );
    }
    share->allow = wizard->everyone ? wizard->level : RIGHT_NONE;
    share->deny = RIGHT_NONE;
    if (!share_write( wizard->folder, share ))
    {
        message( wizard->frame, IDS_CANNOT_SHARE, NULL, MB_ICONERROR );
        return FALSE;
    }
    wizard->shared = TRUE;
    file_sharing_on();
    return TRUE;
}

static void wizard_page( struct wizard *wizard, UINT dialog, DLGPROC proc, UINT title, UINT button )
{
    RECT rect;

    if (wizard->page) DestroyWindow( wizard->page );
    wizard->page = CreateDialogParamW( ntshrui_instance, MAKEINTRESOURCEW( dialog ), wizard->frame, proc,
                                       (LPARAM)wizard );
    GetWindowRect( GetDlgItem( wizard->frame, IDC_WIZARD_PLACE ), &rect );
    MapWindowPoints( NULL, wizard->frame, (POINT *)&rect, 2 );
    SetWindowPos( wizard->page, HWND_TOP, rect.left, rect.top, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW );
    SetDlgItemTextW( wizard->frame, IDC_WIZARD_TITLE, load_text( title ) );
    SetDlgItemTextW( wizard->frame, IDOK, load_text( button ) );
}

static INT_PTR CALLBACK wizard_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct wizard *wizard = (struct wizard *)GetWindowLongPtrW( hwnd, DWLP_USER );

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        LOGFONTW font;

        wizard = (struct wizard *)lparam;
        SetWindowLongPtrW( hwnd, DWLP_USER, lparam );
        wizard->frame = hwnd;
        /* the heading of an Aero wizard: larger, and blue */
        GetObjectW( (HFONT)SendMessageW( hwnd, WM_GETFONT, 0, 0 ), sizeof(font), &font );
        font.lfHeight = font.lfHeight * 4 / 3;
        wizard->title_font = CreateFontIndirectW( &font );
        SendDlgItemMessageW( hwnd, IDC_WIZARD_TITLE, WM_SETFONT, (WPARAM)wizard->title_font, 0 );
        wizard_page( wizard, IDD_WIZARD_PEOPLE, people_proc, IDS_CHOOSE_PEOPLE, IDS_SHARE_BUTTON );
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wparam))
        {
        case IDOK:
            if (wizard->done)
            {
                EndDialog( hwnd, IDOK );
            }
            else if (wizard_share( wizard ))
            {
                RECT rect;

                /* one button is left, where the last one of a wizard is */
                wizard->done = TRUE;
                wizard_page( wizard, IDD_WIZARD_DONE, done_proc, IDS_FOLDER_SHARED, IDS_DONE );
                GetWindowRect( GetDlgItem( hwnd, IDCANCEL ), &rect );
                MapWindowPoints( NULL, hwnd, (POINT *)&rect, 2 );
                ShowWindow( GetDlgItem( hwnd, IDCANCEL ), SW_HIDE );
                SetWindowPos( GetDlgItem( hwnd, IDOK ), NULL, rect.left, rect.top, rect.right - rect.left,
                              rect.bottom - rect.top, SWP_NOZORDER );
            }
            return TRUE;
        case IDCANCEL:
            EndDialog( hwnd, IDCANCEL );
            return TRUE;
        }
        break;
    case WM_CTLCOLORSTATIC:
        if ((HWND)lparam == GetDlgItem( hwnd, IDC_WIZARD_TITLE )) SetTextColor( (HDC)wparam, RGB( 0, 51, 153 ) );
        return white( wparam );
    case WM_ERASEBKGND:
    {
        /* white, and the band of the buttons below */
        RECT client, band;

        GetClientRect( hwnd, &client );
        GetWindowRect( GetDlgItem( hwnd, IDC_WIZARD_BAND ), &band );
        MapWindowPoints( NULL, hwnd, (POINT *)&band, 2 );
        band.left = 0;
        band.right = client.right;
        band.bottom = client.bottom;
        client.bottom = band.top;
        FillRect( (HDC)wparam, &client, GetSysColorBrush( COLOR_WINDOW ) );
        FillRect( (HDC)wparam, &band, GetSysColorBrush( COLOR_3DFACE ) );
        band.bottom = band.top + 1;
        FillRect( (HDC)wparam, &band, GetSysColorBrush( COLOR_3DLIGHT ) );
        SetWindowLongPtrW( hwnd, DWLP_MSGRESULT, 1 );
        return TRUE;
    }
    case WM_DESTROY:
        if (wizard && wizard->title_font) DeleteObject( wizard->title_font );
        break;
    }
    return FALSE;
}

void sharing_wizard( HWND owner, const WCHAR *folder )
{
    struct wizard wizard = { folder };
    struct share shares[16];

    if (PathIsRootW( folder ))
    {
        message( owner, IDS_ROOT_NOT_SHARED, NULL, MB_ICONINFORMATION );
        return;
    }
    if (shares_of_folder( folder, shares, ARRAY_SIZE(shares) ))
    {
        wizard.share = shares[0];
        wizard.shared = TRUE;
        wizard.everyone = shares[0].allow != RIGHT_NONE && shares[0].deny == RIGHT_NONE;
        wizard.level = shares[0].allow == RIGHT_READ ? RIGHT_READ : RIGHT_FULL;
    }
    else share_default( folder, &wizard.share );
    DialogBoxParamW( ntshrui_instance, MAKEINTRESOURCEW( IDD_WIZARD ), owner, wizard_proc, (LPARAM)&wizard );
}

/* "Remove access": the folder is nobody's but its owner's again */
void stop_sharing( HWND owner, const WCHAR *folder )
{
    struct share shares[16];
    UINT count = shares_of_folder( folder, shares, ARRAY_SIZE(shares) );

    for (UINT i = 0; i < count; i++) share_delete( shares[i].name );
}
