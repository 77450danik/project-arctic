/*
 * Power Options of the Control Panel: the Advanced settings dialog
 *
 * Windows 10's "Електроживлення" sheet with its page "Додаткові настройки"
 * (powercfg.cpl's dialog 1000, its controls where Windows has them): the
 * plan to change, "[активний]" after the active one; a tree of every setting
 * that is not hidden, by subgroup, each with "Робота від акумулятора: ..." and
 * "Від мережі: ..." under it (on a desktop "Параметр: ..."); choosing one of
 * those puts a list (settings with values to choose) or a number with
 * arrows (settings with a range; seconds in minutes, as Windows shows them,
 * 0 being "Ніколи") on the line itself. "Відновити встановлені за
 * промовчанням параметри плану"; OK and Apply write what changed.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdlib.h>

#include "powercpl.h"
#include "commctrl.h"
#include "prsht.h"
#include "winternl.h"
#include "powrprof.h"

#define ATTRIBUTE_HIDE   1
#define MAX_VALUES       2048

/* a value of a setting on one power source, as the tree shows it */
struct value
{
    GUID sub, setting;
    BOOL ac;
    BOOL list;              /* possible values to choose from */
    BOOL minutes;           /* seconds, shown in minutes */
    BOOL percent;
    DWORD min, max, step;
    DWORD current, original;
    HTREEITEM item;
};

struct advanced
{
    HWND dialog, tree, editor, spin;
    struct plan plans[32];
    UINT plan_count;
    GUID scheme;
    struct value *values;
    UINT count;
    int editing;            /* index into values, -1 */
    BOOL two_columns, lid;
};

static WCHAR *units_of( const GUID *sub, const GUID *setting, WCHAR *buf, DWORD size )
{
    buf[0] = 0;
    PowerReadValueUnitsSpecifier( NULL, sub, setting, (UCHAR *)buf, &size );
    return buf;
}

/* the label before a value: "Робота від акумулятора: ", or with the units,
 * "Робота від акумулятора (Хвилин): " */
static void value_prefix( struct advanced *adv, const struct value *v, WCHAR *text, size_t count )
{
    WCHAR units[64];
    UINT plain = adv->two_columns ? (v->ac ? IDS_ADV_PLUGGED_IN : IDS_ADV_ON_BATTERY) : IDS_ADV_SETTING;
    UINT with = adv->two_columns ? (v->ac ? IDS_ADV_PLUGGED_IN_UNITS : IDS_ADV_ON_BATTERY_UNITS) : IDS_ADV_SETTING_UNITS;

    if (v->minutes) lstrcpynW( text, format_string( with, load_string( IDS_ADV_MINUTES ) ), count );
    else if (!v->list && !v->percent && units_of( &v->sub, &v->setting, units, sizeof(units) )[0])
        lstrcpynW( text, format_string( with, units ), count );
    else lstrcpynW( text, load_string( plain ), count );
}

/* "Робота від акумулятора (Хвилин): 5", "Від мережі: Сон", "Параметр: 50%" */
static void value_text( struct advanced *adv, const struct value *v, WCHAR *text, size_t count )
{
    WCHAR shown[128], label[128];

    shown[0] = 0;
    if (v->list)
    {
        /* a plan keeps the place in the list of values */
        DWORD size = sizeof(shown);
        PowerReadPossibleFriendlyName( NULL, &v->sub, &v->setting, v->current, (UCHAR *)shown, &size );
    }
    else if (v->minutes)
    {
        if (!v->current) lstrcpynW( shown, load_string( IDS_ADV_NEVER ), ARRAY_SIZE(shown) );
        else swprintf( shown, ARRAY_SIZE(shown), L"%lu", (v->current + 59) / 60 );
    }
    else if (v->percent) swprintf( shown, ARRAY_SIZE(shown), L"%lu%%", v->current );
    else swprintf( shown, ARRAY_SIZE(shown), L"%lu", v->current );
    value_prefix( adv, v, label, ARRAY_SIZE(label) );
    swprintf( text, count, L"%s%s", label, shown );
}

static void close_editor( struct advanced *adv, BOOL keep )
{
    struct value *v;

    if (adv->editing < 0) return;
    v = &adv->values[adv->editing];
    if (keep && adv->editor)
    {
        if (v->list)
        {
            int index = SendMessageW( adv->editor, CB_GETCURSEL, 0, 0 );
            if (index >= 0) v->current = SendMessageW( adv->editor, CB_GETITEMDATA, index, 0 );
        }
        else
        {
            WCHAR text[64], *end;
            UINT number;
            BOOL ok;

            GetWindowTextW( adv->editor, text, ARRAY_SIZE(text) );
            number = wcstoul( text, &end, 10 );
            ok = end != text;
            if (v->minutes && !wcsicmp( text, load_string( IDS_ADV_NEVER ) )) { number = 0; ok = TRUE; }
            if (ok)
            {
                DWORD value = v->minutes ? number * 60 : number;
                v->current = max( v->min, min( value, v->max ) );
            }
        }
        if (v->current != v->original) SendMessageW( GetParent( adv->dialog ), PSM_CHANGED, (WPARAM)adv->dialog, 0 );
    }
    if (adv->editor) DestroyWindow( adv->editor );
    if (adv->spin) DestroyWindow( adv->spin );
    adv->editor = adv->spin = NULL;
    {
        WCHAR text[256];
        TVITEMW item = { TVIF_TEXT, v->item };
        value_text( adv, v, text, ARRAY_SIZE(text) );
        item.pszText = text;
        SendMessageW( adv->tree, TVM_SETITEMW, 0, (LPARAM)&item );
    }
    adv->editing = -1;
}

/* the list or the number, on the line of the value, after its label */
static void open_editor( struct advanced *adv, int index )
{
    struct value *v = &adv->values[index];
    RECT rect;
    WCHAR label[128];
    HDC hdc;
    SIZE size;
    HFONT font = (HFONT)SendMessageW( adv->tree, WM_GETFONT, 0, 0 );
    HGDIOBJ old;
    int left;

    close_editor( adv, TRUE );
    *(HTREEITEM *)&rect = v->item;
    if (!SendMessageW( adv->tree, TVM_GETITEMRECT, TRUE, (LPARAM)&rect )) return;
    value_prefix( adv, v, label, ARRAY_SIZE(label) );
    hdc = GetDC( adv->tree );
    old = SelectObject( hdc, font );
    GetTextExtentPoint32W( hdc, label, lstrlenW( label ), &size );
    SelectObject( hdc, old );
    ReleaseDC( adv->tree, hdc );
    left = rect.left + size.cx + 4;
    adv->editing = index;

    if (v->list)
    {
        adv->editor = CreateWindowExW( 0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
                                       left, rect.top - 2, px( 150 ), px( 200 ), adv->tree, (HMENU)3000, powercpl_instance, NULL );
        SendMessageW( adv->editor, WM_SETFONT, (WPARAM)font, FALSE );
        for (ULONG i = 0; i < 32; i++)
        {
            WCHAR name[128];
            DWORD nsize = sizeof(name);
            int at;

            if (PowerReadPossibleFriendlyName( NULL, &v->sub, &v->setting, i, (UCHAR *)name, &nsize )) break;
            at = SendMessageW( adv->editor, CB_ADDSTRING, 0, (LPARAM)name );
            SendMessageW( adv->editor, CB_SETITEMDATA, at, i );
            if (i == v->current) SendMessageW( adv->editor, CB_SETCURSEL, at, 0 );
        }
    }
    else
    {
        WCHAR text[32];
        UINT shown = v->minutes ? (v->current + 59) / 60 : v->current;
        UINT top = v->minutes ? v->max / 60 : v->max;

        adv->editor = CreateWindowExW( WS_EX_CLIENTEDGE, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_AUTOHSCROLL,
                                       left, rect.top - 1, px( 70 ), rect.bottom - rect.top + 2, adv->tree, (HMENU)3000,
                                       powercpl_instance, NULL );
        SendMessageW( adv->editor, WM_SETFONT, (WPARAM)font, FALSE );
        if (v->minutes && !v->current) lstrcpyW( text, load_string( IDS_ADV_NEVER ) );
        else swprintf( text, ARRAY_SIZE(text), L"%u", shown );
        SetWindowTextW( adv->editor, text );
        adv->spin = CreateWindowExW( 0, UPDOWN_CLASSW, NULL, WS_CHILD | WS_VISIBLE | UDS_ALIGNRIGHT | UDS_ARROWKEYS |
                                     UDS_SETBUDDYINT | UDS_NOTHOUSANDS, 0, 0, 0, 0, adv->tree, NULL, powercpl_instance, NULL );
        SendMessageW( adv->spin, UDM_SETBUDDY, (WPARAM)adv->editor, 0 );
        SendMessageW( adv->spin, UDM_SETRANGE32, v->minutes ? v->min / 60 : v->min, min( top, 0x7fffffff ) );
        SendMessageW( adv->spin, UDM_SETPOS32, 0, shown );
        if (v->minutes && !v->current) SetWindowTextW( adv->editor, load_string( IDS_ADV_NEVER ) );
    }
    SetFocus( adv->editor );
}

static BOOL setting_shown( struct advanced *adv, const GUID *sub, const GUID *setting )
{
    if (PowerReadSettingAttributes( sub, setting ) & ATTRIBUTE_HIDE) return FALSE;
    /* what the machine does not have: the lid's action without a lid, the
     * battery's levels without a battery */
    if (IsEqualGUID( setting, &set_lid ) && !adv->lid) return FALSE;
    if (IsEqualGUID( sub, &sub_battery ) && !adv->two_columns) return FALSE;
    return TRUE;
}

static void add_values( struct advanced *adv, HTREEITEM parent, const GUID *sub, const GUID *setting )
{
    WCHAR text[256], units[64];
    DWORD dummy, size;
    int sides = adv->two_columns ? 2 : 1;

    for (int s = 0; s < sides && adv->count < MAX_VALUES; s++)
    {
        struct value *v = &adv->values[adv->count];
        TVINSERTSTRUCTW insert = { .hParent = parent, .hInsertAfter = TVI_LAST };

        memset( v, 0, sizeof(*v) );
        if (sub) v->sub = *sub;
        v->setting = *setting;
        v->ac = sides == 1 || s == 1;
        size = sizeof(dummy);
        v->list = !PowerReadPossibleValue( NULL, sub, setting, NULL, 0, (UCHAR *)&dummy, &size );
        if (!v->list)
        {
            if (PowerReadValueMin( NULL, sub, setting, &v->min )) v->min = 0;
            if (PowerReadValueMax( NULL, sub, setting, &v->max )) v->max = 0xffffffff;
            if (PowerReadValueIncrement( NULL, sub, setting, &v->step ) || !v->step) v->step = 1;
            units_of( sub, setting, units, sizeof(units) );
            /* Windows' own words: powrprof.dll's 80 "Секунди", 81 "%" */
            v->minutes = !wcsicmp( units, L"Секунди" ) || !wcsicmp( units, L"Seconds" );
            v->percent = !wcscmp( units, L"%" );
        }
        v->current = v->original = plan_value( &adv->scheme, sub, setting, v->ac, 0 );
        value_text( adv, v, text, ARRAY_SIZE(text) );
        insert.item.mask = TVIF_TEXT | TVIF_PARAM;
        insert.item.pszText = text;
        insert.item.lParam = adv->count;
        v->item = (HTREEITEM)SendMessageW( adv->tree, TVM_INSERTITEMW, 0, (LPARAM)&insert );
        adv->count++;
    }
}

static void fill_tree( struct advanced *adv )
{
    GUID sub, setting;
    DWORD size;

    close_editor( adv, FALSE );
    SendMessageW( adv->tree, TVM_DELETEITEM, 0, (LPARAM)TVI_ROOT );
    adv->count = 0;

    /* the settings that belong to no subgroup come first, as in Windows */
    for (ULONG s = 0; ; s++)
    {
        size = sizeof(setting);
        if (PowerEnumerate( NULL, NULL, NULL, ACCESS_INDIVIDUAL_SETTING, s, (UCHAR *)&setting, &size )) break;
        if (setting_shown( adv, NULL, &setting ))
        {
            WCHAR name[256];
            DWORD nsize = sizeof(name);
            TVINSERTSTRUCTW insert = { .hParent = TVI_ROOT, .hInsertAfter = TVI_LAST, .item.mask = TVIF_TEXT | TVIF_PARAM };
            HTREEITEM node;

            if (PowerReadFriendlyName( NULL, NULL, NULL, &setting, (UCHAR *)name, &nsize )) continue;
            insert.item.pszText = name;
            insert.item.lParam = -1;
            node = (HTREEITEM)SendMessageW( adv->tree, TVM_INSERTITEMW, 0, (LPARAM)&insert );
            add_values( adv, node, NULL, &setting );
        }
    }
    for (ULONG g = 0; ; g++)
    {
        WCHAR name[256];
        DWORD nsize = sizeof(name);
        TVINSERTSTRUCTW insert = { .hParent = TVI_ROOT, .hInsertAfter = TVI_LAST, .item.mask = TVIF_TEXT | TVIF_PARAM };
        HTREEITEM group = NULL;

        size = sizeof(sub);
        if (PowerEnumerate( NULL, NULL, NULL, ACCESS_SUBGROUP, g, (UCHAR *)&sub, &size )) break;
        if (PowerReadSettingAttributes( &sub, NULL ) & ATTRIBUTE_HIDE) continue;
        if (PowerReadFriendlyName( NULL, NULL, &sub, NULL, (UCHAR *)name, &nsize )) continue;

        for (ULONG s = 0; ; s++)
        {
            WCHAR setting_name[256];
            DWORD snsize = sizeof(setting_name);
            TVINSERTSTRUCTW child = { .hInsertAfter = TVI_LAST, .item.mask = TVIF_TEXT | TVIF_PARAM };
            HTREEITEM node;

            size = sizeof(setting);
            if (PowerEnumerate( NULL, NULL, &sub, ACCESS_INDIVIDUAL_SETTING, s, (UCHAR *)&setting, &size )) break;
            if (!setting_shown( adv, &sub, &setting )) continue;
            if (PowerReadFriendlyName( NULL, NULL, &sub, &setting, (UCHAR *)setting_name, &snsize )) continue;
            if (!group)
            {
                insert.item.pszText = name;
                insert.item.lParam = -1;
                group = (HTREEITEM)SendMessageW( adv->tree, TVM_INSERTITEMW, 0, (LPARAM)&insert );
            }
            child.hParent = group;
            child.item.pszText = setting_name;
            child.item.lParam = -1;
            node = (HTREEITEM)SendMessageW( adv->tree, TVM_INSERTITEMW, 0, (LPARAM)&child );
            add_values( adv, node, &sub, &setting );
        }
    }
}

static void fill_schemes( struct advanced *adv )
{
    HWND combo = GetDlgItem( adv->dialog, IDC_ADV_SCHEME );

    SendMessageW( combo, CB_RESETCONTENT, 0, 0 );
    adv->plan_count = plans_list( adv->plans, ARRAY_SIZE(adv->plans) );
    for (UINT i = 0; i < adv->plan_count; i++)
    {
        WCHAR name[160];
        int at;

        if (adv->plans[i].active) lstrcpynW( name, format_string( IDS_ADV_ACTIVE, adv->plans[i].name ), ARRAY_SIZE(name) );
        else lstrcpynW( name, adv->plans[i].name, ARRAY_SIZE(name) );
        at = SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)name );
        SendMessageW( combo, CB_SETITEMDATA, at, i );
        if (IsEqualGUID( &adv->plans[i].guid, &adv->scheme )) SendMessageW( combo, CB_SETCURSEL, at, 0 );
    }
}

static BOOL apply( struct advanced *adv )
{
    close_editor( adv, TRUE );
    for (UINT i = 0; i < adv->count; i++)
    {
        struct value *v = &adv->values[i];
        if (v->current == v->original) continue;
        plan_set_value( &adv->scheme, &v->sub, &v->setting, v->ac, v->current );
        v->original = v->current;
    }
    return TRUE;
}

static INT_PTR CALLBACK page_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    struct advanced *adv = (struct advanced *)GetWindowLongPtrW( hwnd, DWLP_USER );

    switch (msg)
    {
    case WM_INITDIALOG:
        adv = (struct advanced *)((PROPSHEETPAGEW *)lp)->lParam;
        SetWindowLongPtrW( hwnd, DWLP_USER, (LONG_PTR)adv );
        adv->dialog = hwnd;
        adv->tree = GetDlgItem( hwnd, IDC_ADV_TREE );
        adv->editing = -1;
        SendDlgItemMessageW( hwnd, IDC_ADV_ICON, STM_SETICON,
                             (WPARAM)LoadImageW( powercpl_instance, MAKEINTRESOURCEW( IDI_POWER ), IMAGE_ICON, 32, 32, 0 ), 0 );
        fill_schemes( adv );
        fill_tree( adv );
        return TRUE;

    case WM_COMMAND:
        if (LOWORD( wp ) == IDC_ADV_SCHEME && HIWORD( wp ) == CBN_SELCHANGE)
        {
            int at = SendMessageW( (HWND)lp, CB_GETCURSEL, 0, 0 );
            if (at >= 0)
            {
                apply( adv );
                adv->scheme = adv->plans[SendMessageW( (HWND)lp, CB_GETITEMDATA, at, 0 )].guid;
                fill_tree( adv );
            }
            return TRUE;
        }
        if (LOWORD( wp ) == IDC_ADV_RESTORE && HIWORD( wp ) == BN_CLICKED)
        {
            WCHAR text[1024];
            swprintf( text, ARRAY_SIZE(text), L"%s\n\n%s", load_string( IDS_ADV_RESTORE_CONFIRM ), load_string( IDS_ADV_RESTORE_TEXT ) );
            if (MessageBoxW( hwnd, text, load_string( IDS_ADV_SHEET ), MB_YESNO | MB_ICONWARNING ) == IDYES)
            {
                PowerRestoreIndividualDefaultPowerScheme( &adv->scheme );
                fill_tree( adv );
            }
            return TRUE;
        }
        break;

    case WM_NOTIFY:
    {
        NMHDR *hdr = (NMHDR *)lp;

        if (hdr->idFrom == IDC_ADV_TREE && hdr->code == TVN_SELCHANGEDW)
        {
            NMTREEVIEWW *tv = (NMTREEVIEWW *)lp;
            if ((int)tv->itemNew.lParam >= 0 && (UINT)tv->itemNew.lParam < adv->count) open_editor( adv, tv->itemNew.lParam );
            else close_editor( adv, TRUE );
            return TRUE;
        }
        if (hdr->idFrom == IDC_ADV_TREE && (hdr->code == TVN_ITEMEXPANDINGW || hdr->code == NM_CLICK))
        {
            close_editor( adv, TRUE );
            return FALSE;
        }
        if (hdr->code == PSN_APPLY)
        {
            apply( adv );
            SetWindowLongPtrW( hwnd, DWLP_MSGRESULT, PSNRET_NOERROR );
            return TRUE;
        }
        if (hdr->code == PSN_KILLACTIVE)
        {
            close_editor( adv, TRUE );
            SetWindowLongPtrW( hwnd, DWLP_MSGRESULT, FALSE );
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

void open_advanced_settings( HWND owner, const GUID *scheme )
{
    struct advanced adv = { 0 };
    PROPSHEETPAGEW page = { sizeof(page) };
    PROPSHEETHEADERW header = { sizeof(header) };
    HPROPSHEETPAGE pages[1];

    if (!(adv.values = calloc( MAX_VALUES, sizeof(*adv.values) ))) return;
    if (scheme) adv.scheme = *scheme;
    else plan_active( &adv.scheme );
    adv.two_columns = on_battery_machine();
    adv.lid = lid_present();

    page.hInstance = powercpl_instance;
    page.pszTemplate = MAKEINTRESOURCEW( IDD_ADVANCED );
    page.pfnDlgProc = page_proc;
    page.lParam = (LPARAM)&adv;
    pages[0] = CreatePropertySheetPageW( &page );
    header.dwFlags = PSH_NOCONTEXTHELP;
    header.hwndParent = owner;
    header.hInstance = powercpl_instance;
    header.pszCaption = load_string( IDS_ADV_SHEET );
    header.nPages = 1;
    header.phpage = pages;
    PropertySheetW( &header );
    free( adv.values );
}
