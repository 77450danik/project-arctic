/*
 * The permissions of a share (the page of Windows' aclui.dll) and Select
 * Users or Groups (objsel.dll's dialog) behind its "Add...".
 *
 * Whoever opens a folder of Arctic from the network is a guest
 * (host/init/arctic-smbd.c): the one group there is to give rights to is
 * Everyone. It can be removed and added again, allowed and denied; another
 * name is not found, as a name nobody has is not found in Windows.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "ntshrui.h"
#include "shlwapi.h"

#define BIT_READ    1
#define BIT_CHANGE  2
#define BIT_FULL    4

struct permissions
{
    struct share *share;
    BOOL listed;            /* Everyone is in the list */
    UINT allow, deny;       /* BIT_*: the boxes checked */
    BOOL applied;
    HIMAGELIST icons;
};

static UINT bits_of( int right )
{
    return right == RIGHT_FULL ? 7 : right == RIGHT_CHANGE ? 3 : right == RIGHT_READ ? 1 : 0;
}

static int right_of( UINT bits )
{
    return bits & BIT_FULL ? RIGHT_FULL : bits & BIT_CHANGE ? RIGHT_CHANGE : bits & BIT_READ ? RIGHT_READ : RIGHT_NONE;
}

/**********************************************************************
 *          The list of permissions with its two columns of boxes: aclui's
 *          own window class; here a white box that holds ordinary controls
 */

static LRESULT CALLBACK checklist_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    switch (msg)
    {
    case WM_COMMAND:
        return SendMessageW( GetParent( hwnd ), msg, wparam, lparam );
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetBkColor( (HDC)wparam, GetSysColor( COLOR_WINDOW ) );
        SetTextColor( (HDC)wparam, GetSysColor( IsWindowEnabled( hwnd ) ? COLOR_WINDOWTEXT : COLOR_GRAYTEXT ) );
        return (LRESULT)GetSysColorBrush( COLOR_WINDOW );
    case WM_ERASEBKGND:
    {
        RECT rect;

        GetClientRect( hwnd, &rect );
        FillRect( (HDC)wparam, &rect, GetSysColorBrush( COLOR_WINDOW ) );
        return 1;
    }
    case WM_ENABLE:
        for (HWND child = GetWindow( hwnd, GW_CHILD ); child; child = GetWindow( child, GW_HWNDNEXT ))
            EnableWindow( child, wparam );
        break;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

static void checklist_register(void)
{
    WNDCLASSW cls = { 0 };

    cls.lpfnWndProc = checklist_proc;
    cls.hInstance = ntshrui_instance;
    cls.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_ARROW );
    cls.lpszClassName = L"CHECKLIST_ACLUI";
    RegisterClassW( &cls );
}

/* Full Control, Change, Read: a name and a box under "Allow" and under "Deny" each */
static void checklist_fill( HWND dialog )
{
    static const UINT names[] = { IDS_FULL_CONTROL, IDS_CHANGE, IDS_READ };
    HWND list = GetDlgItem( dialog, IDC_ACL_CHECKS );
    HFONT font = (HFONT)SendMessageW( dialog, WM_GETFONT, 0, 0 );
    RECT client, column[2], unit = { 0, 0, 4, 12 };
    int row, box;

    MapDialogRect( dialog, &unit );
    row = unit.bottom + unit.bottom / 6;
    box = unit.bottom;
    GetClientRect( list, &client );
    GetWindowRect( GetDlgItem( dialog, IDC_ACL_ALLOW ), &column[0] );
    GetWindowRect( GetDlgItem( dialog, IDC_ACL_DENY ), &column[1] );
    MapWindowPoints( NULL, list, (POINT *)column, 4 );
    for (int i = 0; i < 3; i++)
    {
        HWND control = CreateWindowW( L"Static", load_text( names[i] ), WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                                      unit.right, 2 + i * row + (row - unit.bottom * 2 / 3) / 2, column[0].left - unit.right,
                                      unit.bottom * 2 / 3 + 2, list, NULL, ntshrui_instance, NULL );
        SendMessageW( control, WM_SETFONT, (WPARAM)font, 0 );
        for (int j = 0; j < 2; j++)
        {
            control = CreateWindowW( L"Button", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                     (column[j].left + column[j].right - box) / 2, 2 + i * row + (row - box) / 2,
                                     box, box, list, (HMENU)(UINT_PTR)(IDC_ACL_CHECK_FIRST + j * 3 + i),
                                     ntshrui_instance, NULL );
            SendMessageW( control, WM_SETFONT, (WPARAM)font, 0 );
        }
    }
}

static void permissions_show( HWND hwnd, struct permissions *state )
{
    HWND list = GetDlgItem( hwnd, IDC_ACL_LIST ), checks = GetDlgItem( hwnd, IDC_ACL_CHECKS );
    WCHAR text[200];

    SendMessageW( list, LVM_DELETEALLITEMS, 0, 0 );
    if (state->listed)
    {
        LVITEMW item = { LVIF_TEXT | LVIF_IMAGE | LVIF_STATE };

        item.pszText = load_text( IDS_EVERYONE );
        item.state = item.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageW( list, LVM_INSERTITEMW, 0, (LPARAM)&item );
        swprintf( text, ARRAY_SIZE(text), load_text( IDS_PERMISSIONS_OF ), load_text( IDS_EVERYONE ) );
    }
    else lstrcpynW( text, load_text( IDS_PERMISSIONS ), ARRAY_SIZE(text) );
    SetDlgItemTextW( hwnd, IDC_ACL_FOR, text );
    EnableWindow( checks, state->listed );
    EnableWindow( GetDlgItem( hwnd, IDC_ACL_REMOVE ), state->listed );
    for (int i = 0; i < 3; i++)
    {
        UINT bit = BIT_FULL >> i;

        CheckDlgButton( checks, IDC_ACL_CHECK_FIRST + i, state->listed && (state->allow & bit) ? BST_CHECKED : 0 );
        CheckDlgButton( checks, IDC_ACL_CHECK_FIRST + 3 + i, state->listed && (state->deny & bit) ? BST_CHECKED : 0 );
    }
}

/* a box was clicked: the greater right brings the lesser, the lesser gone takes the
 * greater away, and what is allowed is not denied */
static void permissions_click( struct permissions *state, UINT index, BOOL checked )
{
    UINT bit = BIT_FULL >> (index % 3), with = bit * 2 - 1, *mine = index < 3 ? &state->allow : &state->deny;
    UINT *other = index < 3 ? &state->deny : &state->allow;

    if (checked)
    {
        *mine |= with;
        *other &= ~with;
    }
    else *mine &= bit - 1;
    /* what is left of the other column without the rights it took for granted */
    if (!(*other & BIT_READ)) *other = 0;
    else if (!(*other & BIT_CHANGE)) *other &= BIT_READ;
}

static INT_PTR CALLBACK permissions_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct permissions *state = (struct permissions *)GetWindowLongPtrW( hwnd, DWLP_USER );

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        static const UINT unused[] = { IDC_ACL_ADVANCED, IDC_ACL_ADVANCED_TEXT };
        HWND list = GetDlgItem( hwnd, IDC_ACL_LIST );
        LVCOLUMNW column = { LVCF_WIDTH };
        RECT rect;
        HICON icon;

        state = (struct permissions *)((PROPSHEETPAGEW *)lparam)->lParam;
        SetWindowLongPtrW( hwnd, DWLP_USER, (LONG_PTR)state );
        /* a share has no special permissions and nothing advanced */
        for (UINT i = 0; i < ARRAY_SIZE(unused); i++) ShowWindow( GetDlgItem( hwnd, unused[i] ), SW_HIDE );
        GetClientRect( list, &rect );
        column.cx = rect.right - GetSystemMetrics( SM_CXVSCROLL );
        SendMessageW( list, LVM_INSERTCOLUMNW, 0, (LPARAM)&column );
        state->icons = ImageList_Create( GetSystemMetrics( SM_CXSMICON ), GetSystemMetrics( SM_CYSMICON ),
                                         ILC_COLOR32 | ILC_MASK, 1, 1 );
        if ((icon = LoadImageW( GetModuleHandleW( L"shell32.dll" ), MAKEINTRESOURCEW( 269 ) /* users */, IMAGE_ICON,
                                GetSystemMetrics( SM_CXSMICON ), GetSystemMetrics( SM_CYSMICON ), 0 )))
        {
            ImageList_ReplaceIcon( state->icons, -1, icon );
            DestroyIcon( icon );
        }
        SendMessageW( list, LVM_SETIMAGELIST, LVSIL_SMALL, (LPARAM)state->icons );
        checklist_fill( hwnd );
        permissions_show( hwnd, state );
        return TRUE;
    }
    case WM_COMMAND:
    {
        UINT id = LOWORD(wparam);

        if (id >= IDC_ACL_CHECK_FIRST && id < IDC_ACL_CHECK_FIRST + 6 && HIWORD(wparam) == BN_CLICKED)
        {
            permissions_click( state, id - IDC_ACL_CHECK_FIRST,
                               SendMessageW( (HWND)lparam, BM_GETCHECK, 0, 0 ) == BST_CHECKED );
        }
        else if (id == IDC_ACL_ADD)
        {
            if (!pick_everyone( hwnd ) || state->listed) return TRUE;
            state->listed = TRUE;
            state->allow = BIT_READ;
            state->deny = 0;
        }
        else if (id == IDC_ACL_REMOVE)
        {
            state->listed = FALSE;
            state->allow = state->deny = 0;
        }
        else break;
        permissions_show( hwnd, state );
        SendMessageW( GetParent( hwnd ), PSM_CHANGED, (WPARAM)hwnd, 0 );
        return TRUE;
    }
    case WM_NOTIFY:
        if (((NMHDR *)lparam)->code == PSN_APPLY)
        {
            int allow = state->listed ? right_of( state->allow ) : RIGHT_NONE;
            int deny = state->listed ? right_of( state->deny ) : RIGHT_NONE;

            if (deny != RIGHT_NONE && deny != state->share->deny &&
                MessageBoxW( hwnd, load_text( IDS_DENY_WARNING ), load_text( IDS_SECURITY ),
                             MB_YESNO | MB_ICONEXCLAMATION ) != IDYES)
            {
                SetWindowLongPtrW( hwnd, DWLP_MSGRESULT, PSNRET_INVALID_NOCHANGEPAGE );
                return TRUE;
            }
            if (allow != state->share->allow || deny != state->share->deny) state->applied = TRUE;
            state->share->allow = allow;
            state->share->deny = deny;
            SetWindowLongPtrW( hwnd, DWLP_MSGRESULT, PSNRET_NOERROR );
            return TRUE;
        }
        break;
    case WM_DESTROY:
        if (state && state->icons) ImageList_Destroy( state->icons );
        break;
    }
    return FALSE;
}

/* "Permissions for <share>"; TRUE when they were changed */
BOOL edit_permissions( HWND owner, struct share *share )
{
    struct permissions state = { share };
    PROPSHEETHEADERW header = { sizeof(header) };
    PROPSHEETPAGEW page = { sizeof(page) };
    WCHAR title[200];

    checklist_register();
    state.listed = share->allow != RIGHT_NONE || share->deny != RIGHT_NONE;
    state.allow = bits_of( share->allow );
    state.deny = bits_of( share->deny );
    page.dwFlags = PSP_USETITLE;
    page.hInstance = ntshrui_instance;
    page.pszTemplate = MAKEINTRESOURCEW( IDD_PERMISSIONS );
    page.pszTitle = MAKEINTRESOURCEW( IDS_SHARE_PERMISSIONS );
    page.pfnDlgProc = permissions_proc;
    page.lParam = (LPARAM)&state;
    swprintf( title, ARRAY_SIZE(title), load_text( IDS_PERMISSIONS_FOR ), share->name );
    header.dwFlags = PSH_PROPSHEETPAGE | PSH_NOCONTEXTHELP;
    header.hwndParent = owner;
    header.hInstance = ntshrui_instance;
    header.pszCaption = title;
    header.nPages = 1;
    header.ppsp = &page;
    PropertySheetW( &header );
    return state.applied;
}

/**********************************************************************
 *          Select Users or Groups
 */

struct picker
{
    BOOL types[3];      /* built-in security principals, groups, users */
};

static const UINT type_names[] = { IDS_TYPE_BUILTIN, IDS_TYPE_GROUPS, IDS_TYPE_USERS };

/* "Users, Groups, or Built-in security principals" */
static void picker_show_types( HWND hwnd, const struct picker *picker )
{
    WCHAR text[300] = L"";
    int chosen[3], count = 0;

    for (int i = 2; i >= 0; i--)
        if (picker->types[i]) chosen[count++] = i;
    for (int i = 0; i < count; i++)
    {
        if (i) wcscat( text, i == count - 1 ? load_text( IDS_OBJECT_TYPES ) : L", " );
        wcscat( text, load_text( type_names[chosen[i]] ) );
    }
    SetDlgItemTextW( hwnd, IDC_OBJECT_TYPES, text );
}

static INT_PTR CALLBACK types_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct picker *picker = (struct picker *)GetWindowLongPtrW( hwnd, DWLP_USER );
    HWND list = GetDlgItem( hwnd, IDC_OBJECT_TYPE_LIST );

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        LVCOLUMNW column = { LVCF_WIDTH };
        RECT rect;

        picker = (struct picker *)lparam;
        SetWindowLongPtrW( hwnd, DWLP_USER, lparam );
        SendMessageW( list, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_CHECKBOXES, LVS_EX_CHECKBOXES );
        GetClientRect( list, &rect );
        column.cx = rect.right;
        SendMessageW( list, LVM_INSERTCOLUMNW, 0, (LPARAM)&column );
        for (int i = 0; i < 3; i++)
        {
            LVITEMW item = { LVIF_TEXT, i };

            item.pszText = load_text( type_names[i] );
            SendMessageW( list, LVM_INSERTITEMW, 0, (LPARAM)&item );
            item.mask = LVIF_STATE;
            item.stateMask = LVIS_STATEIMAGEMASK;
            item.state = INDEXTOSTATEIMAGEMASK( picker->types[i] ? 2 : 1 );
            SendMessageW( list, LVM_SETITEMSTATE, i, (LPARAM)&item );
        }
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == IDOK)
        {
            BOOL any = FALSE, types[3];

            for (int i = 0; i < 3; i++)
            {
                types[i] = (SendMessageW( list, LVM_GETITEMSTATE, i, LVIS_STATEIMAGEMASK ) >> 12) == 2;
                any |= types[i];
            }
            if (!any) return TRUE; /* something has to be looked for */
            memcpy( picker->types, types, sizeof(types) );
        }
        if (LOWORD(wparam) == IDOK || LOWORD(wparam) == IDCANCEL) EndDialog( hwnd, LOWORD(wparam) );
        return TRUE;
    }
    return FALSE;
}

/* the one place there is to look in: this computer */
static INT_PTR CALLBACK places_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        WCHAR computer[MAX_COMPUTERNAME_LENGTH + 1] = L"";
        DWORD size = ARRAY_SIZE(computer);
        TVINSERTSTRUCTW insert = { TVI_ROOT, TVI_LAST };
        HTREEITEM item;

        GetComputerNameW( computer, &size );
        insert.item.mask = TVIF_TEXT;
        insert.item.pszText = computer;
        item = (HTREEITEM)SendDlgItemMessageW( hwnd, IDC_OBJECT_PLACE_TREE, TVM_INSERTITEMW, 0, (LPARAM)&insert );
        SendDlgItemMessageW( hwnd, IDC_OBJECT_PLACE_TREE, TVM_SELECTITEM, TVGN_CARET, (LPARAM)item );
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == IDOK || LOWORD(wparam) == IDCANCEL) EndDialog( hwnd, LOWORD(wparam) );
        return TRUE;
    }
    return FALSE;
}

/* Is every name typed one there is? The names as Windows writes them go back
 * into the box; the first that nobody has is said */
static BOOL picker_check( HWND hwnd, const struct picker *picker )
{
    WCHAR typed[1024], computer[MAX_COMPUTERNAME_LENGTH + 1] = L"", *name, *next, *last;
    DWORD size = ARRAY_SIZE(computer);
    BOOL found = FALSE;

    GetComputerNameW( computer, &size );
    GetDlgItemTextW( hwnd, IDC_OBJECT_NAMES, typed, ARRAY_SIZE(typed) );
    for (name = typed; name; name = next)
    {
        if ((next = wcschr( name, ';' ))) *next++ = 0;
        StrTrimW( name, L" \r\n\t" );
        if (!name[0]) continue;
        /* COMPUTER\Everyone is Everyone */
        if ((last = wcsrchr( name, '\\' )) && (size_t)(last - name) == wcslen( computer ) &&
            !wcsnicmp( name, computer, last - name ))
            name = last + 1;
        if (picker->types[0] && (!lstrcmpiW( name, L"Everyone" ) || !lstrcmpiW( name, load_text( IDS_EVERYONE ) )))
        {
            found = TRUE;
            continue;
        }
        {
            WCHAR text[1400];

            swprintf( text, ARRAY_SIZE(text), L"%s", load_text( IDS_OBJECT_NOT_FOUND ) );
            if ((last = wcsstr( text, L"%1" )))
            {
                WCHAR rest[1024];

                lstrcpynW( rest, last + 2, ARRAY_SIZE(rest) );
                swprintf( last, ARRAY_SIZE(text) - (last - text), L"%s%s", name, rest );
            }
            MessageBoxW( hwnd, text, load_text( IDS_NAME_NOT_FOUND ), MB_ICONINFORMATION );
        }
        SetFocus( GetDlgItem( hwnd, IDC_OBJECT_NAMES ) );
        return FALSE;
    }
    if (found) SetDlgItemTextW( hwnd, IDC_OBJECT_NAMES, load_text( IDS_EVERYONE ) );
    return found;
}

static INT_PTR CALLBACK picker_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct picker *picker = (struct picker *)GetWindowLongPtrW( hwnd, DWLP_USER );

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        WCHAR computer[MAX_COMPUTERNAME_LENGTH + 1] = L"";
        DWORD size = ARRAY_SIZE(computer);

        picker = (struct picker *)lparam;
        SetWindowLongPtrW( hwnd, DWLP_USER, lparam );
        GetComputerNameW( computer, &size );
        SetDlgItemTextW( hwnd, IDC_OBJECT_PLACE, computer );
        picker_show_types( hwnd, picker );
        EnableWindow( GetDlgItem( hwnd, IDOK ), FALSE );
        SetFocus( GetDlgItem( hwnd, IDC_OBJECT_NAMES ) );
        return FALSE;
    }
    case WM_COMMAND:
        switch (LOWORD(wparam))
        {
        case IDC_OBJECT_TYPES_BUTTON:
            if (DialogBoxParamW( ntshrui_instance, MAKEINTRESOURCEW( IDD_OBJECT_TYPES ), hwnd, types_proc,
                                 (LPARAM)picker ) == IDOK)
                picker_show_types( hwnd, picker );
            return TRUE;
        case IDC_OBJECT_PLACE_BUTTON:
            DialogBoxParamW( ntshrui_instance, MAKEINTRESOURCEW( IDD_LOCATIONS ), hwnd, places_proc, 0 );
            return TRUE;
        case IDC_OBJECT_NAMES:
            if (HIWORD(wparam) == EN_CHANGE)
                EnableWindow( GetDlgItem( hwnd, IDOK ), GetWindowTextLengthW( (HWND)lparam ) > 0 );
            return TRUE;
        case IDC_OBJECT_CHECK:
            picker_check( hwnd, picker );
            return TRUE;
        case IDOK:
            if (picker_check( hwnd, picker )) EndDialog( hwnd, IDOK );
            return TRUE;
        case IDCANCEL:
            EndDialog( hwnd, IDCANCEL );
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* TRUE when Everyone was chosen: there is nobody else to choose */
BOOL pick_everyone( HWND owner )
{
    struct picker picker = { { TRUE, TRUE, TRUE } };

    return DialogBoxParamW( ntshrui_instance, MAKEINTRESOURCEW( IDD_SELECT_OBJECTS ), owner, picker_proc,
                            (LPARAM)&picker ) == IDOK;
}
