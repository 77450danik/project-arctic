/*
 * Network and Sharing Center: the properties of IPv4 and IPv6
 *
 * Windows' own dialogs (tcpipcfg.dll): "Загальні" with an address by DHCP
 * or by hand and the name servers, "Альтернативна конфігурація", and
 * "Додатково..." with the addresses and gateways, the name servers in their
 * order and NetBIOS. For IPv6 the same with the prefix length. What they
 * change goes back to the connection's properties, which apply it on OK, as
 * in Windows.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdio.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include "netcenter.h"
#include "commctrl.h"

struct tcpip
{
    const struct connection *conn;
    struct settings work;
    BOOL v6;
    BOOL ok;
};

static void error_box( HWND dlg, UINT id )
{
    MessageBoxW( dlg, load_string( id ), load_string( IDS_NETCENTER ), MB_OK | MB_ICONEXCLAMATION );
}

static void enable( HWND dlg, const UINT *ids, UINT count, BOOL on )
{
    for (UINT i = 0; i < count; i++) EnableWindow( GetDlgItem( dlg, ids[i] ), on );
}

/**********************************************************************
 *          The small dialogs: an address, a gateway, a name server
 */

struct entry
{
    BOOL v6;
    WCHAR first[48], second[48];   /* address and mask/prefix, gateway, name server */
};

static INT_PTR CALLBACK entry_proc( HWND dlg, UINT msg, WPARAM wp, LPARAM lp )
{
    struct entry *entry = (struct entry *)GetWindowLongPtrW( dlg, DWLP_USER );

    switch (msg)
    {
    case WM_INITDIALOG:
        SetWindowLongPtrW( dlg, DWLP_USER, lp );
        entry = (struct entry *)lp;
        if (GetDlgItem( dlg, IDC_EDIT_ADDRESS ))
        {
            ipaddr_set( dlg, IDC_EDIT_ADDRESS, entry->first );
            ipaddr_set( dlg, IDC_EDIT_MASK, entry->second );
        }
        if (GetDlgItem( dlg, IDC_EDIT_GATEWAY ))
        {
            ipaddr_set( dlg, IDC_EDIT_GATEWAY, entry->first );
            CheckDlgButton( dlg, IDC_EDIT_AUTO_METRIC, BST_CHECKED );
            EnableWindow( GetDlgItem( dlg, IDC_EDIT_METRIC ), FALSE );
            EnableWindow( GetDlgItem( dlg, 1091 ), FALSE );
        }
        if (GetDlgItem( dlg, IDC_EDIT_DNS )) ipaddr_set( dlg, IDC_EDIT_DNS, entry->first );
        if (!entry->first[0]) SetWindowTextW( GetDlgItem( dlg, IDOK ), load_string( IDS_ADD ) );
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD( wp ))
        {
        case IDC_EDIT_AUTO_METRIC:
            EnableWindow( GetDlgItem( dlg, IDC_EDIT_METRIC ), !IsDlgButtonChecked( dlg, IDC_EDIT_AUTO_METRIC ) );
            EnableWindow( GetDlgItem( dlg, 1091 ), !IsDlgButtonChecked( dlg, IDC_EDIT_AUTO_METRIC ) );
            return TRUE;
        case IDOK:
        {
            BOOL (*valid)( const WCHAR * ) = entry->v6 ? valid_ipv6 : valid_ipv4;

            if (GetDlgItem( dlg, IDC_EDIT_ADDRESS ))
            {
                ipaddr_get( dlg, IDC_EDIT_ADDRESS, entry->first, ARRAY_SIZE(entry->first) );
                ipaddr_get( dlg, IDC_EDIT_MASK, entry->second, ARRAY_SIZE(entry->second) );
                if (!valid( entry->first )) { error_box( dlg, IDS_BAD_ADDRESS ); return TRUE; }
                if (entry->v6 ? !(_wtoi( entry->second ) > 0 && _wtoi( entry->second ) <= 128) : !valid_ipv4( entry->second ))
                {
                    error_box( dlg, IDS_BAD_MASK );
                    return TRUE;
                }
            }
            else if (GetDlgItem( dlg, IDC_EDIT_GATEWAY ))
            {
                ipaddr_get( dlg, IDC_EDIT_GATEWAY, entry->first, ARRAY_SIZE(entry->first) );
                if (!valid( entry->first )) { error_box( dlg, IDS_BAD_GATEWAY ); return TRUE; }
            }
            else
            {
                ipaddr_get( dlg, IDC_EDIT_DNS, entry->first, ARRAY_SIZE(entry->first) );
                if (!valid_ipv4( entry->first ) && !valid_ipv6( entry->first )) { error_box( dlg, IDS_BAD_DNS ); return TRUE; }
            }
            EndDialog( dlg, IDOK );
            return TRUE;
        }
        case IDCANCEL:
            EndDialog( dlg, IDCANCEL );
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static BOOL edit_entry( HWND owner, UINT dialog, struct entry *entry )
{
    return DialogBoxParamW( cp_instance, MAKEINTRESOURCEW( dialog ), owner, entry_proc, (LPARAM)entry ) == IDOK;
}

/**********************************************************************
 *          Додатково: IP settings
 */

static void list_columns( HWND list, UINT first, UINT second )
{
    LVCOLUMNW column = { LVCF_TEXT | LVCF_WIDTH };
    RECT rect;

    GetClientRect( list, &rect );
    SendMessageW( list, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT );
    column.cx = rect.right * 6 / 10;
    column.pszText = load_string( first );
    SendMessageW( list, LVM_INSERTCOLUMNW, 0, (LPARAM)&column );
    column.cx = rect.right - column.cx - 4;
    column.pszText = load_string( second );
    SendMessageW( list, LVM_INSERTCOLUMNW, 1, (LPARAM)&column );
}

static void list_add( HWND list, const WCHAR *first, const WCHAR *second )
{
    LVITEMW item = { LVIF_TEXT };

    item.iItem = SendMessageW( list, LVM_GETITEMCOUNT, 0, 0 );
    item.pszText = (WCHAR *)first;
    item.iItem = SendMessageW( list, LVM_INSERTITEMW, 0, (LPARAM)&item );
    item.iSubItem = 1;
    item.pszText = (WCHAR *)second;
    SendMessageW( list, LVM_SETITEMTEXTW, item.iItem, (LPARAM)&item );
}

static void fill_ip_lists( HWND dlg, struct tcpip *t )
{
    HWND addresses = GetDlgItem( dlg, IDC_ADDRESSES ), gateways = GetDlgItem( dlg, IDC_GATEWAYS );
    struct settings *s = &t->work;
    BOOL manual = t->v6 ? s->static6 : s->static4;
    WCHAR prefix[16];

    SendMessageW( addresses, LVM_DELETEALLITEMS, 0, 0 );
    SendMessageW( gateways, LVM_DELETEALLITEMS, 0, 0 );
    if (!manual) list_add( addresses, load_string( IDS_DHCP_ENABLED ), L"" );
    else if (t->v6)
        for (UINT i = 0; i < s->n6; i++)
        {
            swprintf( prefix, ARRAY_SIZE(prefix), L"%u", s->prefix6[i] );
            list_add( addresses, s->addr6[i], prefix );
        }
    else
        for (UINT i = 0; i < s->n4; i++) list_add( addresses, s->addr4[i], s->mask4[i] );
    if (manual && (t->v6 ? s->gw6[0] : s->gw4[0]))
        list_add( gateways, t->v6 ? s->gw6 : s->gw4, load_string( IDS_AUTOMATIC ) );
    {
        static const UINT ids[] = { IDC_ADDRESS_ADD, IDC_ADDRESS_EDIT, IDC_ADDRESS_REMOVE };
        enable( dlg, ids, ARRAY_SIZE(ids), manual );
    }
}

static int selected( HWND list )
{
    return SendMessageW( list, LVM_GETNEXTITEM, -1, LVNI_SELECTED );
}

static INT_PTR CALLBACK advanced_ip_proc( HWND dlg, UINT msg, WPARAM wp, LPARAM lp )
{
    struct tcpip *t = (struct tcpip *)GetWindowLongPtrW( dlg, DWLP_USER );

    switch (msg)
    {
    case WM_INITDIALOG:
        t = (struct tcpip *)((PROPSHEETPAGEW *)lp)->lParam;
        SetWindowLongPtrW( dlg, DWLP_USER, (LONG_PTR)t );
        list_columns( GetDlgItem( dlg, IDC_ADDRESSES ), IDS_ADDRESS_COLUMN, t->v6 ? IDS_PREFIX_COLUMN : IDS_MASK_COLUMN );
        list_columns( GetDlgItem( dlg, IDC_GATEWAYS ), IDS_GATEWAY_COLUMN, IDS_METRIC_COLUMN );
        CheckDlgButton( dlg, IDC_AUTO_METRIC, BST_CHECKED );
        EnableWindow( GetDlgItem( dlg, IDC_METRIC ), FALSE );
        EnableWindow( GetDlgItem( dlg, IDC_METRIC_LABEL ), FALSE );
        fill_ip_lists( dlg, t );
        return TRUE;
    case WM_COMMAND:
    {
        struct settings *s = &t->work;
        HWND addresses = GetDlgItem( dlg, IDC_ADDRESSES );
        struct entry entry = { t->v6 };
        int sel;

        switch (LOWORD( wp ))
        {
        case IDC_AUTO_METRIC:
            EnableWindow( GetDlgItem( dlg, IDC_METRIC ), !IsDlgButtonChecked( dlg, IDC_AUTO_METRIC ) );
            EnableWindow( GetDlgItem( dlg, IDC_METRIC_LABEL ), !IsDlgButtonChecked( dlg, IDC_AUTO_METRIC ) );
            return TRUE;
        case IDC_ADDRESS_ADD:
        case IDC_ADDRESS_EDIT:
            sel = LOWORD( wp ) == IDC_ADDRESS_EDIT ? selected( addresses ) : -1;
            if (LOWORD( wp ) == IDC_ADDRESS_EDIT && sel < 0) return TRUE;
            if (sel < 0 && (t->v6 ? s->n6 : s->n4) >= MAX_ADDRESSES) return TRUE;
            if (sel >= 0)
            {
                lstrcpyW( entry.first, t->v6 ? s->addr6[sel] : s->addr4[sel] );
                if (t->v6) swprintf( entry.second, ARRAY_SIZE(entry.second), L"%u", s->prefix6[sel] );
                else lstrcpyW( entry.second, s->mask4[sel] );
            }
            else if (t->v6) lstrcpyW( entry.second, L"64" );
            if (!edit_entry( dlg, t->v6 ? IDD_ADDRESS6 : IDD_ADDRESS4, &entry )) return TRUE;
            if (sel < 0) sel = t->v6 ? s->n6++ : s->n4++;
            if (t->v6)
            {
                lstrcpynW( s->addr6[sel], entry.first, ARRAY_SIZE(s->addr6[0]) );
                s->prefix6[sel] = _wtoi( entry.second );
            }
            else
            {
                lstrcpynW( s->addr4[sel], entry.first, ARRAY_SIZE(s->addr4[0]) );
                lstrcpynW( s->mask4[sel], entry.second, ARRAY_SIZE(s->mask4[0]) );
            }
            fill_ip_lists( dlg, t );
            return TRUE;
        case IDC_ADDRESS_REMOVE:
            if ((sel = selected( addresses )) < 0) return TRUE;
            if (t->v6)
            {
                memmove( &s->addr6[sel], &s->addr6[sel + 1], (s->n6 - sel - 1) * sizeof(s->addr6[0]) );
                memmove( &s->prefix6[sel], &s->prefix6[sel + 1], (s->n6 - sel - 1) * sizeof(s->prefix6[0]) );
                s->n6--;
            }
            else
            {
                memmove( &s->addr4[sel], &s->addr4[sel + 1], (s->n4 - sel - 1) * sizeof(s->addr4[0]) );
                memmove( &s->mask4[sel], &s->mask4[sel + 1], (s->n4 - sel - 1) * sizeof(s->mask4[0]) );
                s->n4--;
            }
            fill_ip_lists( dlg, t );
            return TRUE;
        case IDC_GATEWAY_ADD:
        case IDC_GATEWAY_EDIT:
            /* one default gateway: what the host routes by */
            lstrcpyW( entry.first, t->v6 ? s->gw6 : s->gw4 );
            if (!edit_entry( dlg, t->v6 ? IDD_GATEWAY6 : IDD_GATEWAY4, &entry )) return TRUE;
            if (t->v6) lstrcpynW( s->gw6, entry.first, ARRAY_SIZE(s->gw6) );
            else lstrcpynW( s->gw4, entry.first, ARRAY_SIZE(s->gw4) );
            if (t->v6) s->static6 = TRUE; else s->static4 = TRUE;
            fill_ip_lists( dlg, t );
            return TRUE;
        case IDC_GATEWAY_REMOVE:
            if (t->v6) s->gw6[0] = 0; else s->gw4[0] = 0;
            fill_ip_lists( dlg, t );
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

/**********************************************************************
 *          Додатково: DNS
 */

static void fill_dns_list( HWND dlg, struct tcpip *t )
{
    HWND list = GetDlgItem( dlg, IDC_DNS_LIST );
    struct settings *s = &t->work;
    UINT count = t->v6 ? s->ndns6 : s->ndns4;

    SendMessageW( list, LB_RESETCONTENT, 0, 0 );
    for (UINT i = 0; i < count; i++)
        SendMessageW( list, LB_ADDSTRING, 0, (LPARAM)(t->v6 ? s->dns6[i] : s->dns4[i]) );
}

static WCHAR *dns_at( struct settings *s, BOOL v6, UINT i )
{
    return v6 ? s->dns6[i] : s->dns4[i];
}

static INT_PTR CALLBACK advanced_dns_proc( HWND dlg, UINT msg, WPARAM wp, LPARAM lp )
{
    struct tcpip *t = (struct tcpip *)GetWindowLongPtrW( dlg, DWLP_USER );

    switch (msg)
    {
    case WM_INITDIALOG:
        t = (struct tcpip *)((PROPSHEETPAGEW *)lp)->lParam;
        SetWindowLongPtrW( dlg, DWLP_USER, (LONG_PTR)t );
        CheckDlgButton( dlg, IDC_DNS_REGISTER, BST_CHECKED );
        CheckRadioButton( dlg, 1046, 1048, 1046 );
        fill_dns_list( dlg, t );
        return TRUE;
    case WM_COMMAND:
    {
        struct settings *s = &t->work;
        HWND list = GetDlgItem( dlg, IDC_DNS_LIST );
        UINT *count = t->v6 ? &s->ndns6 : &s->ndns4;
        int sel = SendMessageW( list, LB_GETCURSEL, 0, 0 );
        struct entry entry = { t->v6 };
        WCHAR swap[48];

        switch (LOWORD( wp ))
        {
        case IDC_DNS_ADD:
        case IDC_DNS_EDIT:
            if (LOWORD( wp ) == IDC_DNS_EDIT && sel < 0) return TRUE;
            if (LOWORD( wp ) == IDC_DNS_ADD) sel = -1;
            if (sel < 0 && *count >= MAX_ADDRESSES) return TRUE;
            if (sel >= 0) lstrcpyW( entry.first, dns_at( s, t->v6, sel ) );
            if (!edit_entry( dlg, IDD_DNS_SERVER, &entry )) return TRUE;
            if (sel < 0) sel = (*count)++;
            lstrcpynW( dns_at( s, t->v6, sel ), entry.first, t->v6 ? 48 : 16 );
            if (t->v6) s->static_dns6 = TRUE; else s->static_dns4 = TRUE;
            fill_dns_list( dlg, t );
            SendMessageW( list, LB_SETCURSEL, sel, 0 );
            return TRUE;
        case IDC_DNS_REMOVE:
            if (sel < 0) return TRUE;
            for (UINT i = sel; i + 1 < *count; i++) lstrcpyW( dns_at( s, t->v6, i ), dns_at( s, t->v6, i + 1 ) );
            (*count)--;
            if (!*count) { if (t->v6) s->static_dns6 = FALSE; else s->static_dns4 = FALSE; }
            fill_dns_list( dlg, t );
            return TRUE;
        case IDC_DNS_UP:
        case IDC_DNS_DOWN:
        {
            int other = LOWORD( wp ) == IDC_DNS_UP ? sel - 1 : sel + 1;
            if (sel < 0 || other < 0 || other >= (int)*count) return TRUE;
            lstrcpyW( swap, dns_at( s, t->v6, sel ) );
            lstrcpyW( dns_at( s, t->v6, sel ), dns_at( s, t->v6, other ) );
            lstrcpyW( dns_at( s, t->v6, other ), swap );
            fill_dns_list( dlg, t );
            SendMessageW( list, LB_SETCURSEL, other, 0 );
            return TRUE;
        }
        }
        break;
    }
    }
    return FALSE;
}

/**********************************************************************
 *          Додатково: WINS
 */

static INT_PTR CALLBACK advanced_wins_proc( HWND dlg, UINT msg, WPARAM wp, LPARAM lp )
{
    struct tcpip *t = (struct tcpip *)GetWindowLongPtrW( dlg, DWLP_USER );

    switch (msg)
    {
    case WM_INITDIALOG:
        t = (struct tcpip *)((PROPSHEETPAGEW *)lp)->lParam;
        SetWindowLongPtrW( dlg, DWLP_USER, (LONG_PTR)t );
        CheckDlgButton( dlg, IDC_LMHOSTS, BST_CHECKED );
        CheckDlgButton( dlg, t->work.netbios == 1 ? IDC_NETBIOS_ON : t->work.netbios == 2 ? IDC_NETBIOS_OFF
                                                                                        : IDC_NETBIOS_DEFAULT,
                        BST_CHECKED );
        return TRUE;
    case WM_NOTIFY:
        if (((NMHDR *)lp)->code == PSN_APPLY)
            t->work.netbios = IsDlgButtonChecked( dlg, IDC_NETBIOS_ON ) ? 1 : IsDlgButtonChecked( dlg, IDC_NETBIOS_OFF ) ? 2 : 0;
        break;
    }
    return FALSE;
}

static PROPSHEETPAGEW page( UINT dialog, DLGPROC proc, struct tcpip *t )
{
    PROPSHEETPAGEW page = { sizeof(page) };

    page.hInstance = cp_instance;
    page.pszTemplate = MAKEINTRESOURCEW( dialog );
    page.pfnDlgProc = proc;
    page.lParam = (LPARAM)t;
    return page;
}

/* "Додатково...": works on a copy, which the General page takes back on OK */
static BOOL show_advanced( HWND owner, struct tcpip *outer )
{
    struct tcpip t = *outer;
    PROPSHEETPAGEW pages[3];
    PROPSHEETHEADERW header = { sizeof(header) };
    UINT count = 0;

    pages[count++] = page( IDD_ADVANCED_IP, advanced_ip_proc, &t );
    pages[count++] = page( IDD_ADVANCED_DNS, advanced_dns_proc, &t );
    if (!t.v6) pages[count++] = page( IDD_ADVANCED_WINS, advanced_wins_proc, &t );
    header.dwFlags = PSH_PROPSHEETPAGE | PSH_NOAPPLYNOW | PSH_NOCONTEXTHELP;
    header.hwndParent = owner;
    header.hInstance = cp_instance;
    header.pszCaption = load_string( IDS_ADVANCED_TITLE );
    header.nPages = count;
    header.ppsp = pages;
    if (PropertySheetW( &header ) <= 0) return FALSE;
    outer->work = t.work;
    return TRUE;
}

/**********************************************************************
 *          Загальні
 */

static void general_update( HWND dlg, struct tcpip *t )
{
    static const UINT address_ids[] = { IDC_IP, IDC_MASK, IDC_GATEWAY, 1005, 1006, 1007 };
    static const UINT dns_ids[] = { IDC_DNS1, IDC_DNS2, 1013, 1014 };
    BOOL manual = IsDlgButtonChecked( dlg, IDC_STATIC_IP );

    enable( dlg, address_ids, ARRAY_SIZE(address_ids), manual );
    /* an address by hand: the name servers by hand too */
    if (manual) CheckRadioButton( dlg, IDC_AUTO_DNS, IDC_STATIC_DNS, IDC_STATIC_DNS );
    EnableWindow( GetDlgItem( dlg, IDC_AUTO_DNS ), !manual );
    enable( dlg, dns_ids, ARRAY_SIZE(dns_ids), IsDlgButtonChecked( dlg, IDC_STATIC_DNS ) );
}

static void general_fill( HWND dlg, struct tcpip *t )
{
    struct settings *s = &t->work;
    BOOL manual = t->v6 ? s->static6 : s->static4, manual_dns = t->v6 ? s->static_dns6 : s->static_dns4;

    CheckRadioButton( dlg, IDC_AUTO_IP, IDC_STATIC_IP, manual ? IDC_STATIC_IP : IDC_AUTO_IP );
    CheckRadioButton( dlg, IDC_AUTO_DNS, IDC_STATIC_DNS, manual || manual_dns ? IDC_STATIC_DNS : IDC_AUTO_DNS );
    if (t->v6)
    {
        WCHAR prefix[16] = L"";
        if (s->n6) swprintf( prefix, ARRAY_SIZE(prefix), L"%u", s->prefix6[0] );
        SetDlgItemTextW( dlg, IDC_IP, s->n6 ? s->addr6[0] : L"" );
        SetDlgItemTextW( dlg, IDC_MASK, prefix );
        SetDlgItemTextW( dlg, IDC_GATEWAY, s->gw6 );
        SetDlgItemTextW( dlg, IDC_DNS1, s->ndns6 > 0 ? s->dns6[0] : L"" );
        SetDlgItemTextW( dlg, IDC_DNS2, s->ndns6 > 1 ? s->dns6[1] : L"" );
    }
    else
    {
        ipaddr_set( dlg, IDC_IP, s->n4 ? s->addr4[0] : NULL );
        ipaddr_set( dlg, IDC_MASK, s->n4 ? s->mask4[0] : NULL );
        ipaddr_set( dlg, IDC_GATEWAY, s->gw4[0] ? s->gw4 : NULL );
        ipaddr_set( dlg, IDC_DNS1, s->ndns4 > 0 ? s->dns4[0] : NULL );
        ipaddr_set( dlg, IDC_DNS2, s->ndns4 > 1 ? s->dns4[1] : NULL );
    }
    general_update( dlg, t );
}

/* the page's fields into the settings; FALSE with a message when one is wrong */
static BOOL general_take( HWND dlg, struct tcpip *t, BOOL check )
{
    struct settings *s = &t->work;
    BOOL manual = IsDlgButtonChecked( dlg, IDC_STATIC_IP ), manual_dns = IsDlgButtonChecked( dlg, IDC_STATIC_DNS );
    BOOL (*valid)( const WCHAR * ) = t->v6 ? valid_ipv6 : valid_ipv4;
    WCHAR ip[48], mask[48], gateway[48], dns1[48], dns2[48];

    ipaddr_get( dlg, IDC_IP, ip, ARRAY_SIZE(ip) );
    ipaddr_get( dlg, IDC_MASK, mask, ARRAY_SIZE(mask) );
    ipaddr_get( dlg, IDC_GATEWAY, gateway, ARRAY_SIZE(gateway) );
    ipaddr_get( dlg, IDC_DNS1, dns1, ARRAY_SIZE(dns1) );
    ipaddr_get( dlg, IDC_DNS2, dns2, ARRAY_SIZE(dns2) );
    if (check && manual)
    {
        if (!valid( ip )) { error_box( dlg, IDS_BAD_ADDRESS ); SetFocus( GetDlgItem( dlg, IDC_IP ) ); return FALSE; }
        if (t->v6 ? !(_wtoi( mask ) > 0 && _wtoi( mask ) <= 128) : !valid_ipv4( mask ))
        {
            error_box( dlg, IDS_BAD_MASK );
            SetFocus( GetDlgItem( dlg, IDC_MASK ) );
            return FALSE;
        }
        if (gateway[0] && !valid( gateway )) { error_box( dlg, IDS_BAD_GATEWAY ); return FALSE; }
    }
    if (check && manual_dns && ((dns1[0] && !valid( dns1 )) || (dns2[0] && !valid( dns2 ))))
    {
        error_box( dlg, IDS_BAD_DNS );
        return FALSE;
    }

    if (t->v6)
    {
        s->static6 = manual;
        if (manual && ip[0])
        {
            lstrcpynW( s->addr6[0], ip, ARRAY_SIZE(s->addr6[0]) );
            s->prefix6[0] = _wtoi( mask ) ? _wtoi( mask ) : 64;
            s->n6 = max( s->n6, 1 );
        }
        lstrcpynW( s->gw6, manual ? gateway : L"", ARRAY_SIZE(s->gw6) );
        s->static_dns6 = manual_dns;
        s->ndns6 = 0;
        if (manual_dns && dns1[0]) lstrcpynW( s->dns6[s->ndns6++], dns1, ARRAY_SIZE(s->dns6[0]) );
        if (manual_dns && dns2[0]) lstrcpynW( s->dns6[s->ndns6++], dns2, ARRAY_SIZE(s->dns6[0]) );
    }
    else
    {
        s->static4 = manual;
        if (manual && ip[0])
        {
            lstrcpynW( s->addr4[0], ip, ARRAY_SIZE(s->addr4[0]) );
            lstrcpynW( s->mask4[0], mask, ARRAY_SIZE(s->mask4[0]) );
            s->n4 = max( s->n4, 1 );
        }
        lstrcpynW( s->gw4, manual ? gateway : L"", ARRAY_SIZE(s->gw4) );
        s->static_dns4 = manual_dns;
        s->ndns4 = 0;
        if (manual_dns && dns1[0]) lstrcpynW( s->dns4[s->ndns4++], dns1, ARRAY_SIZE(s->dns4[0]) );
        if (manual_dns && dns2[0]) lstrcpynW( s->dns4[s->ndns4++], dns2, ARRAY_SIZE(s->dns4[0]) );
    }
    return TRUE;
}

/* the mask Windows offers for an address typed by hand: by its class */
static void suggest_mask( HWND dlg )
{
    WCHAR ip[16], mask[16];
    int first;

    ipaddr_get( dlg, IDC_MASK, mask, ARRAY_SIZE(mask) );
    ipaddr_get( dlg, IDC_IP, ip, ARRAY_SIZE(ip) );
    if (mask[0] || !valid_ipv4( ip )) return;
    first = _wtoi( ip );
    ipaddr_set( dlg, IDC_MASK, first < 128 ? L"255.0.0.0" : first < 192 ? L"255.255.0.0" : L"255.255.255.0" );
}

static INT_PTR CALLBACK general_proc( HWND dlg, UINT msg, WPARAM wp, LPARAM lp )
{
    struct tcpip *t = (struct tcpip *)GetWindowLongPtrW( dlg, DWLP_USER );

    switch (msg)
    {
    case WM_INITDIALOG:
        t = (struct tcpip *)((PROPSHEETPAGEW *)lp)->lParam;
        SetWindowLongPtrW( dlg, DWLP_USER, (LONG_PTR)t );
        if (t->v6)
        {
            SendDlgItemMessageW( dlg, IDC_MASK, EM_LIMITTEXT, 3, 0 );
            SendDlgItemMessageW( dlg, IDC_IP, EM_LIMITTEXT, 45, 0 );
        }
        general_fill( dlg, t );
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD( wp ))
        {
        case IDC_AUTO_IP:
        case IDC_STATIC_IP:
        case IDC_AUTO_DNS:
        case IDC_STATIC_DNS:
            if (HIWORD( wp ) == BN_CLICKED)
            {
                CheckRadioButton( dlg, LOWORD( wp ) <= IDC_STATIC_IP ? IDC_AUTO_IP : IDC_AUTO_DNS,
                                  LOWORD( wp ) <= IDC_STATIC_IP ? IDC_STATIC_IP : IDC_STATIC_DNS, LOWORD( wp ) );
                general_update( dlg, t );
                prop_changed( dlg );
            }
            return TRUE;
        case IDC_MASK:
            if (!t->v6 && HIWORD( wp ) == EN_SETFOCUS) suggest_mask( dlg );
            return TRUE;
        case IDC_ADVANCED:
            if (!general_take( dlg, t, FALSE )) return TRUE;
            if (show_advanced( dlg, t )) general_fill( dlg, t );
            return TRUE;
        }
        break;
    case WM_NOTIFY:
        switch (((NMHDR *)lp)->code)
        {
        case PSN_KILLACTIVE:
            SetWindowLongPtrW( dlg, DWLP_MSGRESULT, !general_take( dlg, t, TRUE ) );
            return TRUE;
        case PSN_APPLY:
            if (!general_take( dlg, t, TRUE ))
            {
                SetWindowLongPtrW( dlg, DWLP_MSGRESULT, PSNRET_INVALID_NOCHANGEPAGE );
                return TRUE;
            }
            t->ok = TRUE;
            SetWindowLongPtrW( dlg, DWLP_MSGRESULT, PSNRET_NOERROR );
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/**********************************************************************
 *          Альтернативна конфігурація
 */

static void alternate_update( HWND dlg )
{
    static const UINT ids[] = { IDC_ALT_IP, IDC_ALT_MASK, IDC_ALT_GATEWAY, IDC_ALT_DNS1, IDC_ALT_DNS2, IDC_ALT_WINS1,
                                IDC_ALT_WINS2, 1072, 1074, 1076, 1078, 1080, 1082, 1084 };
    enable( dlg, ids, ARRAY_SIZE(ids), IsDlgButtonChecked( dlg, IDC_ALT_USER ) );
}

static INT_PTR CALLBACK alternate_proc( HWND dlg, UINT msg, WPARAM wp, LPARAM lp )
{
    struct tcpip *t = (struct tcpip *)GetWindowLongPtrW( dlg, DWLP_USER );

    switch (msg)
    {
    case WM_INITDIALOG:
        t = (struct tcpip *)((PROPSHEETPAGEW *)lp)->lParam;
        SetWindowLongPtrW( dlg, DWLP_USER, (LONG_PTR)t );
        CheckRadioButton( dlg, IDC_ALT_APIPA, IDC_ALT_USER, t->work.alt_user ? IDC_ALT_USER : IDC_ALT_APIPA );
        ipaddr_set( dlg, IDC_ALT_IP, t->work.alt_ip[0] ? t->work.alt_ip : NULL );
        ipaddr_set( dlg, IDC_ALT_MASK, t->work.alt_mask[0] ? t->work.alt_mask : NULL );
        ipaddr_set( dlg, IDC_ALT_GATEWAY, t->work.alt_gw[0] ? t->work.alt_gw : NULL );
        ipaddr_set( dlg, IDC_ALT_DNS1, t->work.alt_dns1[0] ? t->work.alt_dns1 : NULL );
        ipaddr_set( dlg, IDC_ALT_DNS2, t->work.alt_dns2[0] ? t->work.alt_dns2 : NULL );
        alternate_update( dlg );
        return TRUE;
    case WM_COMMAND:
        if (LOWORD( wp ) == IDC_ALT_APIPA || LOWORD( wp ) == IDC_ALT_USER)
        {
            CheckRadioButton( dlg, IDC_ALT_APIPA, IDC_ALT_USER, LOWORD( wp ) );
            alternate_update( dlg );
        }
        break;
    case WM_NOTIFY:
        if (((NMHDR *)lp)->code == PSN_APPLY)
        {
            t->work.alt_user = IsDlgButtonChecked( dlg, IDC_ALT_USER );
            ipaddr_get( dlg, IDC_ALT_IP, t->work.alt_ip, ARRAY_SIZE(t->work.alt_ip) );
            ipaddr_get( dlg, IDC_ALT_MASK, t->work.alt_mask, ARRAY_SIZE(t->work.alt_mask) );
            ipaddr_get( dlg, IDC_ALT_GATEWAY, t->work.alt_gw, ARRAY_SIZE(t->work.alt_gw) );
            ipaddr_get( dlg, IDC_ALT_DNS1, t->work.alt_dns1, ARRAY_SIZE(t->work.alt_dns1) );
            ipaddr_get( dlg, IDC_ALT_DNS2, t->work.alt_dns2, ARRAY_SIZE(t->work.alt_dns2) );
        }
        break;
    }
    return FALSE;
}

static BOOL show_tcpip( HWND owner, const struct connection *conn, struct settings *s, BOOL v6 )
{
    struct tcpip t = { conn, *s, v6 };
    PROPSHEETPAGEW pages[2];
    PROPSHEETHEADERW header = { sizeof(header) };
    UINT count = 0;

    pages[count++] = page( v6 ? IDD_IPV6_GENERAL : IDD_IPV4_GENERAL, general_proc, &t );
    if (!v6) pages[count++] = page( IDD_IPV4_ALTERNATE, alternate_proc, &t );
    header.dwFlags = PSH_PROPSHEETPAGE | PSH_NOAPPLYNOW | PSH_NOCONTEXTHELP;
    header.hwndParent = owner;
    header.hInstance = cp_instance;
    header.pszCaption = load_string( v6 ? IDS_IPV6_PROPERTIES : IDS_IPV4_PROPERTIES );
    header.nPages = count;
    header.ppsp = pages;
    if (PropertySheetW( &header ) <= 0 || !t.ok) return FALSE;
    *s = t.work;
    return TRUE;
}

BOOL show_ipv4( HWND owner, const struct connection *conn, struct settings *s )
{
    return show_tcpip( owner, conn, s, FALSE );
}

BOOL show_ipv6( HWND owner, const struct connection *conn, struct settings *s )
{
    return show_tcpip( owner, conn, s, TRUE );
}
