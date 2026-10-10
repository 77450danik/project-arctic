/*
 * The Sharing page of a folder's Properties and what it opens: Advanced
 * Sharing, New Share, Offline Settings. The dialogs are Windows' own
 * (dialogs_uk.rcinc); what they set is written as the folder's shares
 * (shares.c) and served by the host at once.
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
#include "uxtheme.h"

#define MAX_NAMES 16    /* the names one folder is shared under */

WCHAR *load_text( UINT id )
{
    static WCHAR texts[4][1024];
    static LONG next;
    WCHAR *text = texts[InterlockedIncrement( &next ) & 3];

    if (!LoadStringW( ntshrui_instance, id, text, ARRAY_SIZE(texts[0]) )) text[0] = 0;
    return text;
}

/* a message of the sharing, its %1 or %s filled in */
void message( HWND owner, UINT id, const WCHAR *arg, UINT flags )
{
    WCHAR text[1200], *format = load_text( id ), *place;

    if (arg && (place = wcsstr( format, L"%1" )))
    {
        *place = 0;
        swprintf( text, ARRAY_SIZE(text), L"%s%s%s", format, arg, place + 2 );
    }
    else lstrcpynW( text, format, ARRAY_SIZE(text) );
    MessageBoxW( owner, text, load_text( IDS_SHARING ), flags );
}

/* the folder as Explorer calls it, and its icon */
static HICON folder_look( const WCHAR *folder, WCHAR *title, UINT count )
{
    SHFILEINFOW info = { 0 };

    SHGetFileInfoW( folder, 0, &info, sizeof(info), SHGFI_DISPLAYNAME | SHGFI_ICON | SHGFI_LARGEICON );
    if (title) lstrcpynW( title, info.szDisplayName[0] ? info.szDisplayName : PathFindFileNameW( folder ), count );
    return info.hIcon;
}

void folder_title( const WCHAR *folder, WCHAR *title, UINT count )
{
    HICON icon = folder_look( folder, title, count );

    if (icon) DestroyIcon( icon );
}

/* the name a folder gets when it is shared: its own, without what a share's name cannot have */
static void default_name( const WCHAR *folder, WCHAR *name, UINT count )
{
    const WCHAR *last = PathFindFileNameW( folder );
    UINT i;

    if (PathIsRootW( folder ))
    {
        swprintf( name, count, L"%c", towupper( folder[0] ) );
        return;
    }
    lstrcpynW( name, last, count );
    for (i = 0; name[i]; i++)
        if (name[i] < ' ' || (name[i] != ' ' && wcschr( L"\\/:*?\"<>|[]+=;,", name[i] ))) name[i] = '_';
    while (i && (name[i - 1] == ' ' || name[i - 1] == '.')) name[--i] = 0;
    if (!name[0]) lstrcpynW( name, L"Share", count );
}

/* the share a folder becomes when nothing else is said: its name, Everyone reading */
void share_default( const WCHAR *folder, struct share *share )
{
    memset( share, 0, sizeof(*share) );
    default_name( folder, share->name, ARRAY_SIZE(share->name) );
    share->allow = RIGHT_READ;
    share->limit = MAX_LIMIT;
}

/* is the name one this folder may be shared under? Says why when it is not */
static BOOL name_fits( HWND hwnd, const WCHAR *folder, const WCHAR *name )
{
    WCHAR theirs[MAX_PATH];
    const WCHAR *bad;

    if (!name[0])
    {
        message( hwnd, IDS_NAME_EMPTY, NULL, MB_ICONEXCLAMATION );
        return FALSE;
    }
    if ((bad = share_bad_characters( name )))
    {
        message( hwnd, IDS_BAD_CHARACTERS, bad, MB_ICONEXCLAMATION );
        return FALSE;
    }
    if (share_owner( name, theirs, ARRAY_SIZE(theirs) ) && lstrcmpiW( theirs, folder ))
    {
        message( hwnd, IDS_NAME_TAKEN, name, MB_ICONEXCLAMATION );
        return FALSE;
    }
    return TRUE;
}

static void get_trimmed( HWND hwnd, UINT id, WCHAR *text, UINT count )
{
    UINT len;

    GetDlgItemTextW( hwnd, id, text, count );
    for (len = 0; text[len]; len++)
        if (text[len] == '\r' || text[len] == '\n') text[len] = ' ';
    StrTrimW( text, L" " );
}

/**********************************************************************
 *          Offline Settings
 */

static INT_PTR CALLBACK caching_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    int *caching = (int *)GetWindowLongPtrW( hwnd, DWLP_USER );

    switch (msg)
    {
    case WM_INITDIALOG:
        caching = (int *)lparam;
        SetWindowLongPtrW( hwnd, DWLP_USER, lparam );
        CheckRadioButton( hwnd, IDC_CACHE_MANUAL, IDC_CACHE_AUTO,
                          *caching == CACHE_NONE ? IDC_CACHE_NONE : *caching == CACHE_MANUAL ? IDC_CACHE_MANUAL
                                                                                           : IDC_CACHE_AUTO );
        CheckDlgButton( hwnd, IDC_CACHE_FAST, *caching == CACHE_VDO ? BST_CHECKED : BST_UNCHECKED );
        EnableWindow( GetDlgItem( hwnd, IDC_CACHE_FAST ), *caching == CACHE_AUTO || *caching == CACHE_VDO );
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wparam))
        {
        case IDC_CACHE_MANUAL:
        case IDC_CACHE_NONE:
        case IDC_CACHE_AUTO:
            EnableWindow( GetDlgItem( hwnd, IDC_CACHE_FAST ), IsDlgButtonChecked( hwnd, IDC_CACHE_AUTO ) );
            return TRUE;
        case IDOK:
            if (IsDlgButtonChecked( hwnd, IDC_CACHE_NONE )) *caching = CACHE_NONE;
            else if (IsDlgButtonChecked( hwnd, IDC_CACHE_MANUAL )) *caching = CACHE_MANUAL;
            else *caching = IsDlgButtonChecked( hwnd, IDC_CACHE_FAST ) ? CACHE_VDO : CACHE_AUTO;
            EndDialog( hwnd, IDOK );
            return TRUE;
        case IDCANCEL:
            EndDialog( hwnd, IDCANCEL );
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/**********************************************************************
 *          New Share
 */

struct new_name
{
    const WCHAR *folder;
    struct share share;
};

static INT_PTR CALLBACK new_share_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct new_name *state = (struct new_name *)GetWindowLongPtrW( hwnd, DWLP_USER );

    switch (msg)
    {
    case WM_INITDIALOG:
        SetWindowLongPtrW( hwnd, DWLP_USER, lparam );
        SendDlgItemMessageW( hwnd, IDC_NAME_EDIT, EM_SETLIMITTEXT, 80, 0 );
        SendDlgItemMessageW( hwnd, IDC_COMMENT, EM_SETLIMITTEXT, 256, 0 );
        SendDlgItemMessageW( hwnd, IDC_LIMIT_SPIN, UDM_SETRANGE32, 1, MAX_LIMIT );
        SetDlgItemInt( hwnd, IDC_LIMIT, MAX_LIMIT, FALSE );
        CheckRadioButton( hwnd, IDC_LIMIT_MAXIMUM, IDC_LIMIT_ALLOW, IDC_LIMIT_MAXIMUM );
        EnableWindow( GetDlgItem( hwnd, IDC_LIMIT ), FALSE );
        EnableWindow( GetDlgItem( hwnd, IDC_LIMIT_SPIN ), FALSE );
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wparam))
        {
        case IDC_LIMIT_MAXIMUM:
        case IDC_LIMIT_ALLOW:
            EnableWindow( GetDlgItem( hwnd, IDC_LIMIT ), IsDlgButtonChecked( hwnd, IDC_LIMIT_ALLOW ) );
            EnableWindow( GetDlgItem( hwnd, IDC_LIMIT_SPIN ), IsDlgButtonChecked( hwnd, IDC_LIMIT_ALLOW ) );
            return TRUE;
        case IDC_PERMISSIONS:
            get_trimmed( hwnd, IDC_NAME_EDIT, state->share.name, ARRAY_SIZE(state->share.name) );
            edit_permissions( hwnd, &state->share );
            return TRUE;
        case IDOK:
        {
            UINT limit = GetDlgItemInt( hwnd, IDC_LIMIT, NULL, FALSE );

            get_trimmed( hwnd, IDC_NAME_EDIT, state->share.name, ARRAY_SIZE(state->share.name) );
            if (!name_fits( hwnd, state->folder, state->share.name ))
            {
                SetFocus( GetDlgItem( hwnd, IDC_NAME_EDIT ) );
                return TRUE;
            }
            get_trimmed( hwnd, IDC_COMMENT, state->share.comment, ARRAY_SIZE(state->share.comment) );
            state->share.limit = IsDlgButtonChecked( hwnd, IDC_LIMIT_ALLOW ) && limit >= 1 && limit <= MAX_LIMIT
                                 ? limit : MAX_LIMIT;
            EndDialog( hwnd, IDOK );
            return TRUE;
        }
        case IDCANCEL:
            EndDialog( hwnd, IDCANCEL );
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/**********************************************************************
 *          Advanced Sharing
 */

struct advanced
{
    const WCHAR *folder;
    struct share shares[MAX_NAMES];     /* as the dialog has them */
    struct share saved[MAX_NAMES];      /* as they are served */
    UINT count, saved_count, current;
    BOOL filling, changed;
};

static BOOL advanced_shared( HWND hwnd )
{
    return IsDlgButtonChecked( hwnd, IDC_SHARE_THIS ) == BST_CHECKED;
}

/* one name: typed; several, or a folder already shared: chosen from a list */
static BOOL advanced_listed( const struct advanced *state )
{
    return state->saved_count || state->count > 1;
}

static void advanced_enable( HWND hwnd, const struct advanced *state )
{
    static const UINT ids[] = { IDC_NAME_LABEL, IDC_NAME_EDIT, IDC_NAME_COMBO, IDC_NAME_ADD, IDC_LIMIT_LABEL,
                                IDC_LIMIT, IDC_LIMIT_SPIN, IDC_COMMENT_LABEL, IDC_COMMENT, IDC_PERMISSIONS,
                                IDC_CACHING };
    BOOL shared = advanced_shared( hwnd );

    for (UINT i = 0; i < ARRAY_SIZE(ids); i++) EnableWindow( GetDlgItem( hwnd, ids[i] ), shared );
    EnableWindow( GetDlgItem( hwnd, IDC_NAME_REMOVE ), shared && state->count > 1 );
    ShowWindow( GetDlgItem( hwnd, IDC_NAME_EDIT ), advanced_listed( state ) ? SW_HIDE : SW_SHOW );
    ShowWindow( GetDlgItem( hwnd, IDC_NAME_COMBO ), advanced_listed( state ) ? SW_SHOW : SW_HIDE );
}

/* the share being looked at, into the dialog */
static void advanced_show( HWND hwnd, struct advanced *state )
{
    const struct share *share = &state->shares[state->current];
    HWND combo = GetDlgItem( hwnd, IDC_NAME_COMBO );

    state->filling = TRUE;
    SendMessageW( combo, CB_RESETCONTENT, 0, 0 );
    for (UINT i = 0; i < state->count; i++)
    {
        /* the list sorts its names: each remembers which share it is */
        LRESULT at = SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)state->shares[i].name );

        SendMessageW( combo, CB_SETITEMDATA, at, i );
    }
    for (UINT i = 0; i < state->count; i++)
        if ((UINT)SendMessageW( combo, CB_GETITEMDATA, i, 0 ) == state->current) SendMessageW( combo, CB_SETCURSEL, i, 0 );
    SetDlgItemTextW( hwnd, IDC_NAME_EDIT, share->name );
    SetDlgItemTextW( hwnd, IDC_COMMENT, share->comment );
    SetDlgItemInt( hwnd, IDC_LIMIT, share->limit, FALSE );
    state->filling = FALSE;
    advanced_enable( hwnd, state );
}

/* and what the dialog says of it, back */
static void advanced_take( HWND hwnd, struct advanced *state )
{
    struct share *share = &state->shares[state->current];
    UINT limit = GetDlgItemInt( hwnd, IDC_LIMIT, NULL, FALSE );

    if (!advanced_listed( state )) get_trimmed( hwnd, IDC_NAME_EDIT, share->name, ARRAY_SIZE(share->name) );
    get_trimmed( hwnd, IDC_COMMENT, share->comment, ARRAY_SIZE(share->comment) );
    share->limit = limit < 1 ? 1 : limit > MAX_LIMIT ? MAX_LIMIT : limit;
}

static void advanced_changed( HWND hwnd, struct advanced *state )
{
    if (state->filling) return;
    state->changed = TRUE;
    EnableWindow( GetDlgItem( hwnd, IDC_APPLY ), TRUE );
}

static BOOL advanced_apply( HWND hwnd, struct advanced *state )
{
    UINT i, j;

    if (!advanced_shared( hwnd ))
    {
        /* not shared any more: nothing of it is remembered, as in Windows */
        for (i = 0; i < state->saved_count; i++) share_delete( state->saved[i].name );
        state->saved_count = 0;
        state->count = 1;
        state->current = 0;
        share_default( state->folder, &state->shares[0] );
    }
    else
    {
        advanced_take( hwnd, state );
        for (i = 0; i < state->count; i++)
        {
            if (name_fits( hwnd, state->folder, state->shares[i].name )) continue;
            state->current = i;
            advanced_show( hwnd, state );
            SetFocus( GetDlgItem( hwnd, advanced_listed( state ) ? IDC_NAME_COMBO : IDC_NAME_EDIT ) );
            return FALSE;
        }
        for (i = 0; i < state->saved_count; i++)
        {
            for (j = 0; j < state->count; j++)
                if (!lstrcmpiW( state->saved[i].name, state->shares[j].name )) break;
            if (j == state->count) share_delete( state->saved[i].name );
        }
        for (i = 0; i < state->count; i++)
        {
            if (share_write( state->folder, &state->shares[i] )) continue;
            message( hwnd, IDS_CANNOT_SHARE, NULL, MB_ICONERROR );
            return FALSE;
        }
        memcpy( state->saved, state->shares, state->count * sizeof(state->shares[0]) );
        state->saved_count = state->count;
        file_sharing_on();
    }
    state->changed = FALSE;
    advanced_show( hwnd, state );
    EnableWindow( GetDlgItem( hwnd, IDC_APPLY ), FALSE );
    return TRUE;
}

static INT_PTR CALLBACK advanced_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct advanced *state = (struct advanced *)GetWindowLongPtrW( hwnd, DWLP_USER );

    switch (msg)
    {
    case WM_INITDIALOG:
        state = (struct advanced *)lparam;
        SetWindowLongPtrW( hwnd, DWLP_USER, lparam );
        SendDlgItemMessageW( hwnd, IDC_NAME_EDIT, EM_SETLIMITTEXT, 80, 0 );
        SendDlgItemMessageW( hwnd, IDC_COMMENT, EM_SETLIMITTEXT, 256, 0 );
        SendDlgItemMessageW( hwnd, IDC_LIMIT_SPIN, UDM_SETRANGE32, 1, MAX_LIMIT );
        CheckDlgButton( hwnd, IDC_SHARE_THIS, state->saved_count ? BST_CHECKED : BST_UNCHECKED );
        advanced_show( hwnd, state );
        EnableWindow( GetDlgItem( hwnd, IDC_APPLY ), FALSE );
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wparam))
        {
        case IDC_SHARE_THIS:
            advanced_enable( hwnd, state );
            advanced_changed( hwnd, state );
            return TRUE;
        case IDC_NAME_EDIT:
        case IDC_COMMENT:
        case IDC_LIMIT:
            if (HIWORD(wparam) == EN_CHANGE) advanced_changed( hwnd, state );
            return TRUE;
        case IDC_NAME_COMBO:
            if (HIWORD(wparam) == CBN_SELCHANGE && !state->filling)
            {
                UINT chosen = SendDlgItemMessageW( hwnd, IDC_NAME_COMBO, CB_GETITEMDATA,
                                                   SendDlgItemMessageW( hwnd, IDC_NAME_COMBO, CB_GETCURSEL, 0, 0 ), 0 );

                advanced_take( hwnd, state );
                if (chosen < state->count) state->current = chosen;
                advanced_show( hwnd, state );
            }
            return TRUE;
        case IDC_NAME_ADD:
        {
            struct new_name added = { state->folder };

            if (state->count == MAX_NAMES) return TRUE;
            added.share.allow = RIGHT_READ;
            if (DialogBoxParamW( ntshrui_instance, MAKEINTRESOURCEW( IDD_NEW_SHARE ), hwnd, new_share_proc,
                                 (LPARAM)&added ) != IDOK)
                return TRUE;
            advanced_take( hwnd, state );
            for (UINT i = 0; i < state->count; i++)
            {
                if (lstrcmpiW( state->shares[i].name, added.share.name )) continue;
                message( hwnd, IDS_NAME_TAKEN, added.share.name, MB_ICONEXCLAMATION );
                return TRUE;
            }
            state->current = state->count;
            state->shares[state->count++] = added.share;
            advanced_show( hwnd, state );
            advanced_changed( hwnd, state );
            return TRUE;
        }
        case IDC_NAME_REMOVE:
            if (state->count < 2) return TRUE;
            memmove( &state->shares[state->current], &state->shares[state->current + 1],
                     (state->count - state->current - 1) * sizeof(state->shares[0]) );
            state->count--;
            if (state->current == state->count) state->current--;
            advanced_show( hwnd, state );
            advanced_changed( hwnd, state );
            return TRUE;
        case IDC_PERMISSIONS:
            advanced_take( hwnd, state );
            if (edit_permissions( hwnd, &state->shares[state->current] )) advanced_changed( hwnd, state );
            return TRUE;
        case IDC_CACHING:
            if (DialogBoxParamW( ntshrui_instance, MAKEINTRESOURCEW( IDD_CACHING ), hwnd, caching_proc,
                                 (LPARAM)&state->shares[state->current].caching ) == IDOK)
                advanced_changed( hwnd, state );
            return TRUE;
        case IDC_APPLY:
            advanced_apply( hwnd, state );
            return TRUE;
        case IDOK:
            if (!state->changed || advanced_apply( hwnd, state )) EndDialog( hwnd, IDOK );
            return TRUE;
        case IDCANCEL:
            EndDialog( hwnd, IDCANCEL );
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* TRUE when the dialog was not cancelled */
BOOL advanced_sharing( HWND owner, const WCHAR *folder )
{
    struct advanced *state = calloc( 1, sizeof(*state) );
    BOOL ret;

    if (!state) return FALSE;
    state->folder = folder;
    state->saved_count = shares_of_folder( folder, state->saved, MAX_NAMES );
    memcpy( state->shares, state->saved, sizeof(state->saved) );
    if (!(state->count = state->saved_count))
    {
        state->count = 1;
        share_default( folder, &state->shares[0] );
    }
    ret = DialogBoxParamW( ntshrui_instance, MAKEINTRESOURCEW( IDD_ADVANCED ), owner, advanced_proc,
                           (LPARAM)state ) == IDOK;
    free( state );
    return ret;
}

/**********************************************************************
 *          The Sharing page
 */

struct page
{
    WCHAR folder[MAX_PATH];
    HICON icon;
};

static void page_refresh( HWND hwnd, const struct page *page )
{
    WCHAR computer[MAX_COMPUTERNAME_LENGTH + 1] = L"", path[200];
    DWORD size = ARRAY_SIZE(computer);
    struct share shares[MAX_NAMES];
    UINT count = shares_of_folder( page->folder, shares, MAX_NAMES );

    GetComputerNameW( computer, &size );
    if (count) swprintf( path, ARRAY_SIZE(path), L"\\\\%s\\%s", computer, shares[0].name );
    SetDlgItemTextW( hwnd, IDC_PAGE_STATE, load_text( count ? IDS_SHARED : IDS_NOT_SHARED ) );
    SetDlgItemTextW( hwnd, IDC_PAGE_PATH, count ? path : load_text( IDS_NOT_SHARED ) );
    SetDlgItemTextW( hwnd, IDC_PAGE_PASSWORD, load_text( password_protected() ? IDS_PASSWORD : IDS_NO_PASSWORD ) );
}

static INT_PTR CALLBACK page_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct page *page = (struct page *)GetWindowLongPtrW( hwnd, DWLP_USER );

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        WCHAR title[MAX_PATH];

        page = (struct page *)((PROPSHEETPAGEW *)lparam)->lParam;
        SetWindowLongPtrW( hwnd, DWLP_USER, (LONG_PTR)page );
        page->icon = folder_look( page->folder, title, ARRAY_SIZE(title) );
        SendDlgItemMessageW( hwnd, IDC_PAGE_ICON, STM_SETICON, (WPARAM)page->icon, 0 );
        SetDlgItemTextW( hwnd, IDC_PAGE_NAME, title );
        /* the wizard shares a folder under its own name; a drive has none */
        EnableWindow( GetDlgItem( hwnd, IDC_PAGE_SHARE ), !PathIsRootW( page->folder ) );
        EnableThemeDialogTexture( hwnd, ETDT_ENABLETAB );
        page_refresh( hwnd, page );
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wparam))
        {
        case IDC_PAGE_SHARE:
            sharing_wizard( GetAncestor( hwnd, GA_ROOT ), page->folder );
            page_refresh( hwnd, page );
            return TRUE;
        case IDC_PAGE_ADVANCED:
            advanced_sharing( GetAncestor( hwnd, GA_ROOT ), page->folder );
            page_refresh( hwnd, page );
            return TRUE;
        }
        break;
    case WM_NOTIFY:
    {
        NMHDR *hdr = (NMHDR *)lparam;

        if (hdr->idFrom == IDC_PAGE_PASSWORD && (hdr->code == NM_CLICK || hdr->code == NM_RETURN))
        {
            /* "use the Network and Sharing Center": its page of these settings */
            ShellExecuteW( hwnd, NULL, L"rundll32.exe", L"netcenter.dll,ShowAdvancedSharing", NULL, SW_SHOWNORMAL );
            return TRUE;
        }
        if (hdr->code == PSN_SETACTIVE) page_refresh( hwnd, page );
        break;
    }
    case WM_DESTROY:
        if (page && page->icon) DestroyIcon( page->icon );
        break;
    }
    return FALSE;
}

static UINT CALLBACK page_callback( HWND hwnd, UINT msg, PROPSHEETPAGEW *psp )
{
    if (msg == PSPCB_RELEASE) free( (void *)psp->lParam );
    return 1;
}

HPROPSHEETPAGE sharing_page_create( const WCHAR *folder )
{
    PROPSHEETPAGEW psp = { sizeof(psp) };
    struct page *page;
    HPROPSHEETPAGE ret;

    if (!(page = calloc( 1, sizeof(*page) ))) return NULL;
    lstrcpynW( page->folder, folder, ARRAY_SIZE(page->folder) );
    psp.dwFlags = PSP_USECALLBACK | PSP_USETITLE;
    psp.hInstance = ntshrui_instance;
    psp.pszTemplate = MAKEINTRESOURCEW( IDD_SHARING_PAGE );
    psp.pszTitle = MAKEINTRESOURCEW( IDS_SHARING );
    psp.pfnDlgProc = page_proc;
    psp.pfnCallback = page_callback;
    psp.lParam = (LPARAM)page;
    if (!(ret = CreatePropertySheetPageW( &psp ))) free( page );
    return ret;
}
