/*
 * Network and Sharing Center: the properties of a connection
 *
 * Windows' "Мережа" page (netshell.dll's dialog 16004): the adapter, the
 * components the connection uses with their check boxes, Install, Remove,
 * Properties and the description of the one selected. IPv4 and IPv6 are
 * Arctic's TCP/IP: unticked, the host turns the protocol off on the
 * adapter; their Properties are tcpip.c's. The rest of Windows' list is
 * what Windows 7 shows there, always on. OK applies the settings.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "netcenter.h"
#include "commctrl.h"

enum component { COMP_CLIENT, COMP_QOS, COMP_SHARING, COMP_IPV6, COMP_IPV4, COMP_LLTD_MAPPER, COMP_LLTD_RESPONDER,
                 COMP_COUNT };

static const UINT component_names[COMP_COUNT] =
{
    IDS_COMP_CLIENT, IDS_COMP_QOS, IDS_COMP_SHARING, IDS_COMP_IPV6, IDS_COMP_IPV4, IDS_COMP_LLTD_MAPPER,
    IDS_COMP_LLTD_RESPONDER
};

struct props
{
    struct connection conn;
    struct settings saved, work;
    BOOL filling;
};

static BOOL component_checked( struct props *p, enum component c )
{
    if (c == COMP_IPV4) return p->work.ipv4_on;
    if (c == COMP_IPV6) return p->work.ipv6_on;
    return TRUE;
}

static void fill_components( HWND dlg, struct props *p )
{
    HWND list = GetDlgItem( dlg, IDC_COMPONENTS );
    LVCOLUMNW column = { LVCF_WIDTH };
    HIMAGELIST images;
    RECT rect;

    p->filling = TRUE;
    GetClientRect( list, &rect );
    SendMessageW( list, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT,
                  LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT );
    column.cx = rect.right;
    SendMessageW( list, LVM_INSERTCOLUMNW, 0, (LPARAM)&column );
    images = ImageList_Create( GetSystemMetrics( SM_CXSMICON ), GetSystemMetrics( SM_CYSMICON ), ILC_COLOR32 | ILC_MASK, 2, 0 );
    ImageList_AddIcon( images, load_icon( IDI_NETWORK, GetSystemMetrics( SM_CXSMICON ) ) );
    ImageList_AddIcon( images, load_icon( IDI_ETHERNET, GetSystemMetrics( SM_CXSMICON ) ) );
    SendMessageW( list, LVM_SETIMAGELIST, LVSIL_SMALL, (LPARAM)images );
    for (UINT i = 0; i < COMP_COUNT; i++)
    {
        LVITEMW item = { LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM };

        item.iItem = i;
        item.pszText = load_string( component_names[i] );
        item.iImage = i == COMP_IPV4 || i == COMP_IPV6 || i >= COMP_LLTD_MAPPER ? 1 : 0;
        item.lParam = i;
        SendMessageW( list, LVM_INSERTITEMW, 0, (LPARAM)&item );
        lv_set_check( list, i, component_checked( p, i ) );
    }
    lv_set_state( list, COMP_IPV4, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED );
    p->filling = FALSE;
}

static int selected_component( HWND dlg )
{
    return SendDlgItemMessageW( dlg, IDC_COMPONENTS, LVM_GETNEXTITEM, -1, LVNI_SELECTED );
}

static void selection_changed( HWND dlg )
{
    int sel = selected_component( dlg );

    SetDlgItemTextW( dlg, IDC_DESCRIPTION, sel >= 0 ? load_string( component_names[sel] + 1 ) : L"" );
    EnableWindow( GetDlgItem( dlg, IDC_COMPONENT_PROPS ), sel == COMP_IPV4 || sel == COMP_IPV6 );
    EnableWindow( GetDlgItem( dlg, IDC_UNINSTALL ), FALSE );
    EnableWindow( GetDlgItem( dlg, IDC_INSTALL ), FALSE );
}

static void component_properties( HWND dlg, struct props *p )
{
    int sel = selected_component( dlg );

    if (sel == COMP_IPV4 && show_ipv4( GetParent( dlg ), &p->conn, &p->work ))
        prop_changed( dlg );
    if (sel == COMP_IPV6 && show_ipv6( GetParent( dlg ), &p->conn, &p->work ))
        prop_changed( dlg );
}

static INT_PTR CALLBACK network_proc( HWND dlg, UINT msg, WPARAM wp, LPARAM lp )
{
    struct props *p = (struct props *)GetWindowLongPtrW( dlg, DWLP_USER );

    switch (msg)
    {
    case WM_INITDIALOG:
        p = (struct props *)((PROPSHEETPAGEW *)lp)->lParam;
        SetWindowLongPtrW( dlg, DWLP_USER, (LONG_PTR)p );
        SetDlgItemTextW( dlg, IDC_ADAPTER, p->conn.adapter[0] ? p->conn.adapter : p->conn.ifname );
        fill_components( dlg, p );
        selection_changed( dlg );
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD( wp ))
        {
        case IDC_COMPONENT_PROPS:
            component_properties( dlg, p );
            return TRUE;
        case IDC_CONFIGURE:
            cp_run( L"rundll32.exe", L"devmgr.dll,DeviceManager_ExecuteW" );
            return TRUE;
        }
        break;
    case WM_NOTIFY:
    {
        NMHDR *hdr = (NMHDR *)lp;

        if (hdr->idFrom == IDC_COMPONENTS && hdr->code == LVN_ITEMCHANGED)
        {
            NMLISTVIEW *nm = (NMLISTVIEW *)lp;

            if (nm->uChanged & LVIF_STATE && (nm->uNewState ^ nm->uOldState) & LVIS_SELECTED) selection_changed( dlg );
            if (!p->filling && nm->uChanged & LVIF_STATE && (nm->uNewState ^ nm->uOldState) & LVIS_STATEIMAGEMASK)
            {
                BOOL on = lv_get_check( hdr->hwndFrom, nm->iItem );

                if (nm->iItem == COMP_IPV4) p->work.ipv4_on = on;
                else if (nm->iItem == COMP_IPV6) p->work.ipv6_on = on;
                /* the rest are always there */
                else if (!on)
                {
                    p->filling = TRUE;
                    lv_set_check( hdr->hwndFrom, nm->iItem, TRUE );
                    p->filling = FALSE;
                }
                prop_changed( dlg );
            }
            return TRUE;
        }
        if (hdr->idFrom == IDC_COMPONENTS && hdr->code == NM_DBLCLK)
        {
            component_properties( dlg, p );
            return TRUE;
        }
        if (hdr->code == PSN_APPLY)
        {
            if (memcmp( &p->saved, &p->work, sizeof(p->work) ) && !settings_save( dlg, &p->conn, &p->work ))
            {
                SetWindowLongPtrW( dlg, DWLP_MSGRESULT, PSNRET_INVALID_NOCHANGEPAGE );
                return TRUE;
            }
            p->saved = p->work;
            SetWindowLongPtrW( dlg, DWLP_MSGRESULT, PSNRET_NOERROR );
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

void show_properties( HWND owner, const struct connection *conn )
{
    PROPSHEETPAGEW page = { sizeof(page) };
    PROPSHEETHEADERW header = { sizeof(header) };
    struct props *p = calloc( 1, sizeof(*p) );

    if (!p) return;
    p->conn = *conn;
    settings_load( conn, &p->saved );
    p->work = p->saved;
    page.hInstance = cp_instance;
    page.pszTemplate = MAKEINTRESOURCEW( IDD_PROPERTIES );
    page.pfnDlgProc = network_proc;
    page.lParam = (LPARAM)p;
    header.dwFlags = PSH_PROPSHEETPAGE | PSH_NOAPPLYNOW | PSH_NOCONTEXTHELP | PSH_USEHICON;
    header.hwndParent = owner;
    header.hInstance = cp_instance;
    header.hIcon = load_icon( IDI_NETCENTER, GetSystemMetrics( SM_CXSMICON ) );
    header.pszCaption = format_string( IDS_PROPERTIES_TITLE, conn->name );
    header.nPages = 1;
    header.ppsp = &page;
    PropertySheetW( &header );
    free( p );
}
