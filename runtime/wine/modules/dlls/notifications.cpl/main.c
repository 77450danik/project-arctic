/*
 * Notifications applet (notifications.cpl)
 *
 * Windows 10's notification settings (Settings > System > Notifications &
 * actions, Focus assist, and Ease of Access' "Show notifications for"),
 * here as a Control Panel applet: "Сповіщення" (notifications from programs
 * and other senders, their sounds, how long they stay), "Відправники" (each
 * program that sent one: on or off, its banners, its sound, its priority)
 * and "Планувальник сповіщень" (focus assist: priority only, alarms only,
 * and the rule for a program in full screen). The texts are Windows 10's
 * where it has them. The settings apply at once, as in the Settings app;
 * the shell reads them for every notification (ReactOS patch 0060).
 *
 * "control notifications.cpl,,senders" and ",,focus" open those pages; a
 * program's path opens its settings (the settings button of a toast).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdlib.h>

#include "notifications.h"
#include "cpl.h"
#include "prsht.h"
#include "shellapi.h"
#include "uxtheme.h"

HINSTANCE instance;

#define WM_OPEN_SENDER (WM_APP + 1)
#define ACCESSIBILITY_KEY L"Control Panel\\Accessibility"

static WCHAR start_sender[MAX_PATH];

const WCHAR *load_string( UINT id )
{
    static WCHAR buffers[8][512];
    static UINT next;
    WCHAR *buf = buffers[next++ % ARRAY_SIZE(buffers)];

    if (!LoadStringW( instance, id, buf, ARRAY_SIZE(buffers[0]) )) buf[0] = 0;
    return buf;
}

static void set_big_font( HWND hwnd, UINT id, int percent, int weight )
{
    LOGFONTW font;
    HFONT big;

    GetObjectW( (HFONT)SendMessageW( hwnd, WM_GETFONT, 0, 0 ), sizeof(font), &font );
    font.lfHeight = font.lfHeight * percent / 100;
    font.lfWeight = weight;
    if ((big = CreateFontIndirectW( &font ))) SendDlgItemMessageW( hwnd, id, WM_SETFONT, (WPARAM)big, FALSE );
}

static void free_font( HWND hwnd, UINT id )
{
    HFONT font = (HFONT)SendDlgItemMessageW( hwnd, id, WM_GETFONT, 0, 0 );

    if (font && font != (HFONT)SendMessageW( hwnd, WM_GETFONT, 0, 0 )) DeleteObject( font );
}

/**********************************************************************
 *          "Сповіщення"
 */

static const DWORD durations[] = { 5, 7, 15, 30, 60, 300 };

static void enable_general( HWND hwnd )
{
    BOOL on = SendDlgItemMessageW( hwnd, IDC_TOASTS, BM_GETCHECK, 0, 0 ) == BST_CHECKED;

    EnableWindow( GetDlgItem( hwnd, IDC_SOUNDS ), on );
}

INT_PTR CALLBACK general_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        DWORD seconds = reg_dword( ACCESSIBILITY_KEY, L"MessageDuration", 5 );
        UINT nearest = 0;

        EnableThemeDialogTexture( hwnd, ETDT_ENABLETAB );
        set_big_font( hwnd, IDC_TITLE, 170, FW_NORMAL );
        SendDlgItemMessageW( hwnd, IDC_TOASTS, BM_SETCHECK,
                             reg_dword( PUSH_KEY, PUSH_TOASTS, 1 ) ? BST_CHECKED : BST_UNCHECKED, 0 );
        CheckDlgButton( hwnd, IDC_SOUNDS, reg_dword( SETTINGS_KEY, SETTINGS_SOUND, 1 ) ? BST_CHECKED : BST_UNCHECKED );
        for (UINT i = 0; i < ARRAY_SIZE(durations); i++)
        {
            SendDlgItemMessageW( hwnd, IDC_DURATION, CB_ADDSTRING, 0, (LPARAM)load_string( IDS_5_SECONDS + i ) );
            if (seconds >= durations[i]) nearest = i;
        }
        SendDlgItemMessageW( hwnd, IDC_DURATION, CB_SETCURSEL, nearest, 0 );
        enable_general( hwnd );
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD( wp ))
        {
        case IDC_TOASTS:
            set_reg_dword( PUSH_KEY, PUSH_TOASTS, SendDlgItemMessageW( hwnd, IDC_TOASTS, BM_GETCHECK, 0, 0 ) == BST_CHECKED );
            enable_general( hwnd );
            return TRUE;
        case IDC_SOUNDS:
            set_reg_dword( SETTINGS_KEY, SETTINGS_SOUND, IsDlgButtonChecked( hwnd, IDC_SOUNDS ) == BST_CHECKED );
            return TRUE;
        case IDC_DURATION:
            if (HIWORD( wp ) == CBN_SELCHANGE)
            {
                LRESULT sel = SendDlgItemMessageW( hwnd, IDC_DURATION, CB_GETCURSEL, 0, 0 );
                if (sel >= 0 && sel < ARRAY_SIZE(durations))
                {
                    /* Windows keeps it there for SPI_GETMESSAGEDURATION */
                    set_reg_dword( ACCESSIBILITY_KEY, L"MessageDuration", durations[sel] );
                    SystemParametersInfoW( 0x2017 /* SPI_SETMESSAGEDURATION */, 0, (void *)(ULONG_PTR)durations[sel],
                                           SPIF_SENDCHANGE );
                }
            }
            return TRUE;
        }
        break;
    case WM_DESTROY:
        free_font( hwnd, IDC_TITLE );
        break;
    }
    return FALSE;
}

/**********************************************************************
 *          Lists of senders, with their icons
 */

struct sender_list
{
    struct sender *senders;
    UINT count;
    BOOL filling;
};

static void set_check( HWND list, int index, BOOL on )
{
    LVITEMW item = { 0 };

    item.stateMask = LVIS_STATEIMAGEMASK;
    item.state = INDEXTOSTATEIMAGEMASK( on ? 2 : 1 );
    SendMessageW( list, LVM_SETITEMSTATE, index, (LPARAM)&item );
}

static BOOL get_check( HWND list, int index )
{
    return ((SendMessageW( list, LVM_GETITEMSTATE, index, LVIS_STATEIMAGEMASK ) >> 12) & 0xf) == 2;
}

static void select_item( HWND list, int index )
{
    LVITEMW item = { 0 };

    item.stateMask = item.state = LVIS_SELECTED | LVIS_FOCUSED;
    SendMessageW( list, LVM_SETITEMSTATE, index, (LPARAM)&item );
}

static void fill_list( HWND list, struct sender_list *data, BOOL by_name, BOOL priority, BOOL state_column )
{
    HIMAGELIST images = (HIMAGELIST)SendMessageW( list, LVM_GETIMAGELIST, LVSIL_SMALL, 0 );

    data->filling = TRUE;
    free( data->senders );
    data->count = load_senders( &data->senders, by_name );
    SendMessageW( list, LVM_DELETEALLITEMS, 0, 0 );
    if (images) ImageList_RemoveAll( images );
    for (UINT i = 0; i < data->count; i++)
    {
        struct sender *s = &data->senders[i];
        LVITEMW item = { LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE };
        HICON icon = NULL;

        if (s->icon[0]) ExtractIconExW( s->icon, 0, NULL, &icon, 1 );
        if (!icon) ExtractIconExW( L"shell32.dll", 2, NULL, &icon, 1 );
        item.iImage = icon && images ? ImageList_AddIcon( images, icon ) : -1;
        if (icon) DestroyIcon( icon );
        item.iItem = i;
        item.pszText = s->name;
        item.lParam = i;
        SendMessageW( list, LVM_INSERTITEMW, 0, (LPARAM)&item );
        if (state_column)
        {
            item.mask = LVIF_TEXT;
            item.iSubItem = 1;
            item.pszText = (WCHAR *)sender_state( s );
            SendMessageW( list, LVM_SETITEMTEXTW, i, (LPARAM)&item );
        }
        set_check( list, i, priority ? s->priority : s->enabled );
    }
    data->filling = FALSE;
}

static void init_list( HWND list, BOOL state_column )
{
    LVCOLUMNW column = { LVCF_TEXT | LVCF_WIDTH };
    RECT rc;
    int width;

    SendMessageW( list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER );
    SendMessageW( list, LVM_SETIMAGELIST, LVSIL_SMALL,
                  (LPARAM)ImageList_Create( GetSystemMetrics( SM_CXSMICON ), GetSystemMetrics( SM_CYSMICON ),
                                            ILC_COLOR32 | ILC_MASK, 8, 8 ) );
    GetClientRect( list, &rc );
    width = rc.right - GetSystemMetrics( SM_CXVSCROLL );
    column.pszText = (WCHAR *)load_string( IDS_COLUMN_SENDER );
    column.cx = state_column ? width * 3 / 5 : width;
    SendMessageW( list, LVM_INSERTCOLUMNW, 0, (LPARAM)&column );
    if (state_column)
    {
        column.pszText = (WCHAR *)load_string( IDS_COLUMN_STATE );
        column.cx = width - width * 3 / 5;
        SendMessageW( list, LVM_INSERTCOLUMNW, 1, (LPARAM)&column );
    }
}

static int selected( HWND list )
{
    return (int)SendMessageW( list, LVM_GETNEXTITEM, -1, LVNI_SELECTED );
}

/**********************************************************************
 *          "Відправники"
 */

static void refresh_senders( HWND hwnd, struct sender_list *data )
{
    HWND list = GetDlgItem( hwnd, IDC_LIST );

    fill_list( list, data, SendDlgItemMessageW( hwnd, IDC_SORT, CB_GETCURSEL, 0, 0 ) == 1, FALSE, TRUE );
    ShowWindow( GetDlgItem( hwnd, IDC_EMPTY ), data->count ? SW_HIDE : SW_SHOW );
    EnableWindow( GetDlgItem( hwnd, IDC_OPTIONS ), selected( list ) >= 0 );
}

static void open_sender( HWND hwnd, struct sender_list *data, int index )
{
    if (index < 0 || index >= data->count) return;
    if (edit_sender( GetAncestor( hwnd, GA_ROOT ), &data->senders[index] )) refresh_senders( hwnd, data );
}

INT_PTR CALLBACK senders_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    struct sender_list *data = (struct sender_list *)GetWindowLongPtrW( hwnd, DWLP_USER );

    switch (msg)
    {
    case WM_INITDIALOG:
        if (!(data = calloc( 1, sizeof(*data) ))) return TRUE;
        SetWindowLongPtrW( hwnd, DWLP_USER, (LONG_PTR)data );
        EnableThemeDialogTexture( hwnd, ETDT_ENABLETAB );
        set_big_font( hwnd, IDC_TITLE, 140, FW_NORMAL );
        SendDlgItemMessageW( hwnd, IDC_SORT, CB_ADDSTRING, 0, (LPARAM)load_string( IDS_RECENT ) );
        SendDlgItemMessageW( hwnd, IDC_SORT, CB_ADDSTRING, 0, (LPARAM)load_string( IDS_BY_NAME ) );
        SendDlgItemMessageW( hwnd, IDC_SORT, CB_SETCURSEL, 0, 0 );
        init_list( GetDlgItem( hwnd, IDC_LIST ), TRUE );
        refresh_senders( hwnd, data );
        if (start_sender[0]) PostMessageW( hwnd, WM_OPEN_SENDER, 0, 0 );
        return TRUE;
    case WM_OPEN_SENDER:
        for (UINT i = 0; i < data->count; i++)
        {
            WCHAR path[MAX_PATH];

            sender_path( &data->senders[i], path, ARRAY_SIZE(path) );
            if (lstrcmpiW( path, start_sender )) continue;
            select_item( GetDlgItem( hwnd, IDC_LIST ), i );
            start_sender[0] = 0;
            open_sender( hwnd, data, i );
            break;
        }
        return TRUE;
    case WM_COMMAND:
        if (LOWORD( wp ) == IDC_SORT && HIWORD( wp ) == CBN_SELCHANGE) refresh_senders( hwnd, data );
        else if (LOWORD( wp ) == IDC_OPTIONS) open_sender( hwnd, data, selected( GetDlgItem( hwnd, IDC_LIST ) ) );
        return TRUE;
    case WM_NOTIFY:
    {
        NMHDR *hdr = (NMHDR *)lp;

        if (hdr->idFrom == IDC_LIST && hdr->code == NM_DBLCLK)
            open_sender( hwnd, data, ((NMITEMACTIVATE *)lp)->iItem );
        else if (hdr->idFrom == IDC_LIST && hdr->code == LVN_ITEMCHANGED && data && !data->filling)
        {
            NMLISTVIEW *nm = (NMLISTVIEW *)lp;

            EnableWindow( GetDlgItem( hwnd, IDC_OPTIONS ), selected( hdr->hwndFrom ) >= 0 );
            /* the check box is "on or off" for the sender */
            if ((nm->uChanged & LVIF_STATE) && ((nm->uNewState ^ nm->uOldState) & LVIS_STATEIMAGEMASK) &&
                nm->iItem >= 0 && nm->iItem < data->count)
            {
                struct sender *s = &data->senders[nm->iItem];
                LVITEMW item = { LVIF_TEXT };

                s->enabled = get_check( hdr->hwndFrom, nm->iItem );
                save_sender( s );
                item.iSubItem = 1;
                item.pszText = (WCHAR *)sender_state( s );
                SendMessageW( hdr->hwndFrom, LVM_SETITEMTEXTW, nm->iItem, (LPARAM)&item );
            }
        }
        else if (hdr->code == PSN_SETACTIVE && data) refresh_senders( hwnd, data );
        break;
    }
    case WM_DESTROY:
        free_font( hwnd, IDC_TITLE );
        if (data)
        {
            ImageList_Destroy( (HIMAGELIST)SendDlgItemMessageW( hwnd, IDC_LIST, LVM_GETIMAGELIST, LVSIL_SMALL, 0 ) );
            free( data->senders );
            free( data );
        }
        break;
    }
    return FALSE;
}

/**********************************************************************
 *          "Планувальник сповіщень"
 */

static void enable_focus( HWND hwnd )
{
    EnableWindow( GetDlgItem( hwnd, IDC_F_FSMODE ), IsDlgButtonChecked( hwnd, IDC_F_FULLSCREEN ) == BST_CHECKED );
}

static void save_fullscreen( HWND hwnd )
{
    DWORD mode = 0;

    if (IsDlgButtonChecked( hwnd, IDC_F_FULLSCREEN ) == BST_CHECKED)
        mode = SendDlgItemMessageW( hwnd, IDC_F_FSMODE, CB_GETCURSEL, 0, 0 ) == 0 ? 1 : 2;
    set_reg_dword( FOCUS_KEY, FOCUS_FULLSCREEN, mode );
}

INT_PTR CALLBACK focus_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    struct sender_list *data = (struct sender_list *)GetWindowLongPtrW( hwnd, DWLP_USER );

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        DWORD mode = reg_dword( FOCUS_KEY, FOCUS_MODE, 0 ), fullscreen = reg_dword( FOCUS_KEY, FOCUS_FULLSCREEN, 2 );

        if (!(data = calloc( 1, sizeof(*data) ))) return TRUE;
        SetWindowLongPtrW( hwnd, DWLP_USER, (LONG_PTR)data );
        EnableThemeDialogTexture( hwnd, ETDT_ENABLETAB );
        set_big_font( hwnd, IDC_TITLE, 140, FW_NORMAL );
        CheckRadioButton( hwnd, IDC_F_OFF, IDC_F_ALARMS, mode == 1 ? IDC_F_PRIORITY : mode == 2 ? IDC_F_ALARMS : IDC_F_OFF );
        CheckDlgButton( hwnd, IDC_F_FULLSCREEN, fullscreen ? BST_CHECKED : BST_UNCHECKED );
        SendDlgItemMessageW( hwnd, IDC_F_FSMODE, CB_ADDSTRING, 0, (LPARAM)load_string( IDS_PRIORITY_ONLY ) );
        SendDlgItemMessageW( hwnd, IDC_F_FSMODE, CB_ADDSTRING, 0, (LPARAM)load_string( IDS_ALARMS_ONLY ) );
        SendDlgItemMessageW( hwnd, IDC_F_FSMODE, CB_SETCURSEL, fullscreen == 1 ? 0 : 1, 0 );
        init_list( GetDlgItem( hwnd, IDC_F_LIST ), FALSE );
        fill_list( GetDlgItem( hwnd, IDC_F_LIST ), data, TRUE, TRUE, FALSE );
        enable_focus( hwnd );
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD( wp ))
        {
        case IDC_F_OFF:
        case IDC_F_PRIORITY:
        case IDC_F_ALARMS:
            set_reg_dword( FOCUS_KEY, FOCUS_MODE, LOWORD( wp ) - IDC_F_OFF );
            return TRUE;
        case IDC_F_FULLSCREEN:
            enable_focus( hwnd );
            save_fullscreen( hwnd );
            return TRUE;
        case IDC_F_FSMODE:
            if (HIWORD( wp ) == CBN_SELCHANGE) save_fullscreen( hwnd );
            return TRUE;
        }
        break;
    case WM_NOTIFY:
    {
        NMHDR *hdr = (NMHDR *)lp;

        if (hdr->idFrom == IDC_F_LIST && hdr->code == LVN_ITEMCHANGED && data && !data->filling)
        {
            NMLISTVIEW *nm = (NMLISTVIEW *)lp;

            if ((nm->uChanged & LVIF_STATE) && ((nm->uNewState ^ nm->uOldState) & LVIS_STATEIMAGEMASK) &&
                nm->iItem >= 0 && nm->iItem < data->count)
            {
                data->senders[nm->iItem].priority = get_check( hdr->hwndFrom, nm->iItem );
                save_sender( &data->senders[nm->iItem] );
            }
        }
        else if (hdr->code == PSN_SETACTIVE && data) fill_list( GetDlgItem( hwnd, IDC_F_LIST ), data, TRUE, TRUE, FALSE );
        break;
    }
    case WM_DESTROY:
        free_font( hwnd, IDC_TITLE );
        if (data)
        {
            ImageList_Destroy( (HIMAGELIST)SendDlgItemMessageW( hwnd, IDC_F_LIST, LVM_GETIMAGELIST, LVSIL_SMALL, 0 ) );
            free( data->senders );
            free( data );
        }
        break;
    }
    return FALSE;
}

/**********************************************************************
 *          The applet
 */

/* the settings apply at once: one "Закрити" where Cancel was */
static int CALLBACK sheet_callback( HWND hwnd, UINT msg, LPARAM lp )
{
    if (msg == PSCB_INITIALIZED)
    {
        HWND ok = GetDlgItem( hwnd, IDOK ), cancel = GetDlgItem( hwnd, IDCANCEL );
        RECT rc;

        GetWindowRect( cancel, &rc );
        MapWindowPoints( NULL, hwnd, (POINT *)&rc, 2 );
        ShowWindow( cancel, SW_HIDE );
        SetWindowPos( ok, NULL, rc.left, rc.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER );
        SetWindowTextW( ok, load_string( IDS_CLOSE ) );
    }
    return 0;
}

static void show_sheet( HWND parent, const WCHAR *start )
{
    static const struct { UINT id; DLGPROC proc; } pages[] =
    {
        { IDD_GENERAL, general_proc }, { IDD_SENDERS, senders_proc }, { IDD_FOCUS, focus_proc },
    };
    HPROPSHEETPAGE handles[ARRAY_SIZE(pages)];
    PROPSHEETHEADERW header = { sizeof(header) };
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES };
    UINT count = 0, first = 0;

    InitCommonControlsEx( &icc );
    register_toggle();
    for (UINT i = 0; i < ARRAY_SIZE(pages); i++)
    {
        PROPSHEETPAGEW page = { sizeof(page) };

        page.hInstance = instance;
        page.pszTemplate = MAKEINTRESOURCEW( pages[i].id );
        page.pfnDlgProc = pages[i].proc;
        if ((handles[count] = CreatePropertySheetPageW( &page ))) count++;
    }
    if (start && start[0])
    {
        if (!wcsicmp( start, L"senders" )) first = 1;
        else if (!wcsicmp( start, L"focus" )) first = 2;
        else if (wcschr( start, '\\' ))
        {
            const WCHAR *p = start;
            size_t len;

            if (*p == '"') p++;
            lstrcpynW( start_sender, p, ARRAY_SIZE(start_sender) );
            if ((len = wcslen( start_sender )) && start_sender[len - 1] == '"') start_sender[len - 1] = 0;
            first = 1;
        }
    }

    header.dwFlags = PSH_NOAPPLYNOW | PSH_USEICONID | PSH_USECALLBACK;
    header.hwndParent = parent;
    header.hInstance = instance;
    header.pszIcon = MAKEINTRESOURCEW( IDI_NOTIFICATIONS );
    header.pszCaption = load_string( IDS_CAPTION );
    header.nPages = count;
    header.nStartPage = first;
    header.phpage = handles;
    header.pfnCallback = sheet_callback;
    PropertySheetW( &header );
}

LONG CALLBACK CPlApplet( HWND hwnd, UINT msg, LPARAM lparam1, LPARAM lparam2 )
{
    switch (msg)
    {
    case CPL_INIT:
        return TRUE;
    case CPL_GETCOUNT:
        return 1;
    case CPL_INQUIRE:
    {
        CPLINFO *info = (CPLINFO *)lparam2;
        info->idIcon = IDI_NOTIFICATIONS;
        info->idName = IDS_NAME;
        info->idInfo = IDS_INFO;
        info->lData = 0;
        return 0;
    }
    case CPL_STARTWPARMSW:
        show_sheet( hwnd, (const WCHAR *)lparam2 );
        return TRUE;
    case CPL_DBLCLK:
        show_sheet( hwnd, NULL );
        return 0;
    }
    return 0;
}

BOOL WINAPI DllMain( HINSTANCE inst, DWORD reason, void *reserved )
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        instance = inst;
        DisableThreadLibraryCalls( inst );
    }
    return TRUE;
}
