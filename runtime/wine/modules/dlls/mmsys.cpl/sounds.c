/*
 * Sound of the Control Panel: "Звуки" and "Комунікації"
 *
 * Windows' pages (mmsys.cpl's dialogs 112, 113, 115). The sound scheme is
 * the registry's, as winmm plays it: HKCU\AppEvents\Schemes\Apps\<program>
 * \<event>\.Current is the sound of an event now, \<scheme> what a saved
 * scheme has for it; EventLabels names the events, Schemes\Names the
 * schemes. The tree lists the programs' events, the one with a sound with
 * a speaker; a sound chosen for an event is the scheme "(змінено)" until it
 * is saved under a name. OK and Apply write the sounds; what communications
 * do to the other sounds is kept as Windows keeps it (UserDuckingPreference).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdio.h>

#include "mmsys.h"
#include "prsht.h"
#include "commdlg.h"
#include "mmsystem.h"
#include "shlwapi.h"

#define MAX_EVENTS  256
#define MAX_SCHEMES 32

#define prop_changed( dlg ) SendMessageW( GetParent( dlg ), PSM_CHANGED, (WPARAM)(dlg), 0 )

static const WCHAR apps_key[] = L"AppEvents\\Schemes\\Apps";
static const WCHAR names_key[] = L"AppEvents\\Schemes\\Names";
static const WCHAR labels_key[] = L"AppEvents\\EventLabels";

struct event
{
    WCHAR app[64], name[64];
    WCHAR label[128];
    WCHAR sound[MAX_PATH];      /* what the page has for it now */
    WCHAR saved[MAX_PATH];      /* what the registry has */
    HTREEITEM item;
};

struct scheme
{
    WCHAR key[64], name[128];
};

struct sounds
{
    struct event events[MAX_EVENTS];
    UINT count;
    struct scheme schemes[MAX_SCHEMES];
    UINT scheme_count;
    WCHAR current[64];          /* the scheme the sounds are of; empty once one was changed */
    BOOL filling;
};

/* a name the registry gives as "@module,-id" or as it is */
static void indirect_name( HKEY key, const WCHAR *fallback, WCHAR *out, DWORD count )
{
    WCHAR disp[MAX_PATH];
    DWORD size = sizeof(disp);

    if (!RegGetValueW( key, NULL, L"DispFileName", RRF_RT_REG_SZ, NULL, disp, &size ) && disp[0] == '@' &&
        SUCCEEDED(SHLoadIndirectString( disp, out, count, NULL )) && out[0])
        return;
    size = count * sizeof(WCHAR);
    if (!RegGetValueW( key, NULL, NULL, RRF_RT_REG_SZ, NULL, out, &size ) && out[0])
    {
        if (out[0] == '@')
        {
            lstrcpynW( disp, out, ARRAY_SIZE(disp) );
            if (FAILED(SHLoadIndirectString( disp, out, count, NULL ))) lstrcpynW( out, fallback, count );
        }
        return;
    }
    lstrcpynW( out, fallback, count );
}

static void read_sound( HKEY event_key, const WCHAR *scheme, WCHAR *out )
{
    WCHAR raw[MAX_PATH];
    DWORD size = sizeof(raw);

    out[0] = 0;
    if (RegGetValueW( event_key, scheme, NULL, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, NULL, raw, &size ))
        return;
    ExpandEnvironmentStringsW( raw, out, MAX_PATH );
}

static void load_events( struct sounds *s )
{
    HKEY apps, app, event, labels;
    WCHAR app_name[64], event_name[64];

    s->count = 0;
    if (RegOpenKeyExW( HKEY_CURRENT_USER, apps_key, 0, KEY_READ, &apps )) return;
    RegOpenKeyExW( HKEY_CURRENT_USER, labels_key, 0, KEY_READ, &labels );
    for (DWORD a = 0; !RegEnumKeyW( apps, a, app_name, ARRAY_SIZE(app_name) ); a++)
    {
        if (RegOpenKeyExW( apps, app_name, 0, KEY_READ, &app )) continue;
        for (DWORD e = 0; s->count < MAX_EVENTS && !RegEnumKeyW( app, e, event_name, ARRAY_SIZE(event_name) ); e++)
        {
            struct event *ev = &s->events[s->count];
            HKEY label;

            if (RegOpenKeyExW( app, event_name, 0, KEY_READ, &event )) continue;
            memset( ev, 0, sizeof(*ev) );
            lstrcpyW( ev->app, app_name );
            lstrcpyW( ev->name, event_name );
            lstrcpyW( ev->label, event_name );
            if (labels && !RegOpenKeyExW( labels, event_name, 0, KEY_READ, &label ))
            {
                indirect_name( label, event_name, ev->label, ARRAY_SIZE(ev->label) );
                RegCloseKey( label );
            }
            read_sound( event, L".Current", ev->sound );
            lstrcpyW( ev->saved, ev->sound );
            RegCloseKey( event );
            s->count++;
        }
        RegCloseKey( app );
    }
    if (labels) RegCloseKey( labels );
    RegCloseKey( apps );
}

static void load_schemes( struct sounds *s )
{
    WCHAR key_name[64];
    DWORD size = sizeof(s->current);
    HKEY names, key;

    s->scheme_count = 0;
    if (RegGetValueW( HKEY_CURRENT_USER, L"AppEvents\\Schemes", NULL, RRF_RT_REG_SZ, NULL, s->current, &size ))
        lstrcpyW( s->current, L".Default" );
    if (RegOpenKeyExW( HKEY_CURRENT_USER, names_key, 0, KEY_READ, &names )) return;
    for (DWORD i = 0; s->scheme_count < MAX_SCHEMES && !RegEnumKeyW( names, i, key_name, ARRAY_SIZE(key_name) ); i++)
    {
        struct scheme *scheme = &s->schemes[s->scheme_count];

        if (RegOpenKeyExW( names, key_name, 0, KEY_READ, &key )) continue;
        lstrcpyW( scheme->key, key_name );
        indirect_name( key, key_name, scheme->name, ARRAY_SIZE(scheme->name) );
        RegCloseKey( key );
        s->scheme_count++;
    }
    RegCloseKey( names );
}

/**********************************************************************
 *          The controls
 */

static void fill_schemes( HWND dlg, struct sounds *s )
{
    HWND combo = GetDlgItem( dlg, IDC_SCHEMES );
    int sel = -1;

    SendMessageW( combo, CB_RESETCONTENT, 0, 0 );
    for (UINT i = 0; i < s->scheme_count; i++)
    {
        int index = SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)s->schemes[i].name );
        SendMessageW( combo, CB_SETITEMDATA, index, i );
        if (!wcsicmp( s->schemes[i].key, s->current )) sel = index;
    }
    SendMessageW( combo, CB_SETCURSEL, sel, 0 );
    EnableWindow( GetDlgItem( dlg, IDC_DELETE_SCHEME ), sel >= 0 && s->current[0] != '.' );
}

static struct scheme *chosen_scheme( HWND dlg, struct sounds *s )
{
    int sel = SendDlgItemMessageW( dlg, IDC_SCHEMES, CB_GETCURSEL, 0, 0 ), index;

    if (sel < 0) return NULL;
    index = SendDlgItemMessageW( dlg, IDC_SCHEMES, CB_GETITEMDATA, sel, 0 );
    return index >= 0 && index < (int)s->scheme_count ? &s->schemes[index] : NULL;
}

static void set_item_image( HWND tree, struct event *ev )
{
    TVITEMW item = { TVIF_IMAGE | TVIF_SELECTEDIMAGE };

    item.hItem = ev->item;
    item.iImage = item.iSelectedImage = ev->sound[0] ? 1 : 0;
    SendMessageW( tree, TVM_SETITEMW, 0, (LPARAM)&item );
}

static void fill_tree( HWND dlg, struct sounds *s )
{
    HWND tree = GetDlgItem( dlg, IDC_EVENTS );
    int size = GetSystemMetrics( SM_CXSMICON );
    HIMAGELIST images = ImageList_Create( size, size, ILC_COLOR32 | ILC_MASK, 2, 0 );
    HICON blank;
    HKEY apps, app;
    WCHAR last[64] = L"";
    HTREEITEM parent = NULL, first = NULL;

    /* no icon, and the speaker of an event that has a sound */
    {
        BYTE and_mask[32 * 32 / 8], xor_mask[32 * 32 / 8];
        memset( and_mask, 0xff, sizeof(and_mask) );
        memset( xor_mask, 0, sizeof(xor_mask) );
        blank = CreateIcon( mmsys_instance, 32, 32, 1, 1, and_mask, xor_mask );
    }
    ImageList_AddIcon( images, blank );
    ImageList_AddIcon( images, LoadImageW( mmsys_instance, MAKEINTRESOURCEW( IDI_SOUND ), IMAGE_ICON, size, size, LR_SHARED ) );
    DestroyIcon( blank );
    SendMessageW( tree, TVM_SETIMAGELIST, TVSIL_NORMAL, (LPARAM)images );

    s->filling = TRUE;
    SendMessageW( tree, TVM_DELETEITEM, 0, (LPARAM)TVI_ROOT );
    RegOpenKeyExW( HKEY_CURRENT_USER, apps_key, 0, KEY_READ, &apps );
    for (UINT i = 0; i < s->count; i++)
    {
        struct event *ev = &s->events[i];
        TVINSERTSTRUCTW insert = { 0 };

        if (wcscmp( last, ev->app ))
        {
            WCHAR name[128];

            lstrcpyW( last, ev->app );
            lstrcpynW( name, ev->app, ARRAY_SIZE(name) );
            if (apps && !RegOpenKeyExW( apps, ev->app, 0, KEY_READ, &app ))
            {
                indirect_name( app, ev->app, name, ARRAY_SIZE(name) );
                RegCloseKey( app );
            }
            insert.hParent = TVI_ROOT;
            insert.hInsertAfter = TVI_LAST;
            insert.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_STATE;
            insert.item.pszText = name;
            insert.item.lParam = -1;
            insert.item.state = insert.item.stateMask = TVIS_EXPANDED | TVIS_BOLD;
            parent = (HTREEITEM)SendMessageW( tree, TVM_INSERTITEMW, 0, (LPARAM)&insert );
            if (!first) first = parent;
        }
        memset( &insert, 0, sizeof(insert) );
        insert.hParent = parent;
        insert.hInsertAfter = TVI_SORT;
        insert.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
        insert.item.pszText = ev->label;
        insert.item.lParam = i;
        insert.item.iImage = insert.item.iSelectedImage = ev->sound[0] ? 1 : 0;
        ev->item = (HTREEITEM)SendMessageW( tree, TVM_INSERTITEMW, 0, (LPARAM)&insert );
    }
    if (apps) RegCloseKey( apps );
    if (first) SendMessageW( tree, TVM_ENSUREVISIBLE, 0, (LPARAM)first );
    s->filling = FALSE;
}

static struct event *chosen_event( HWND dlg, struct sounds *s )
{
    HWND tree = GetDlgItem( dlg, IDC_EVENTS );
    TVITEMW item = { TVIF_PARAM };

    item.hItem = (HTREEITEM)SendMessageW( tree, TVM_GETNEXTITEM, TVGN_CARET, 0 );
    if (!item.hItem || !SendMessageW( tree, TVM_GETITEMW, 0, (LPARAM)&item )) return NULL;
    return item.lParam >= 0 && item.lParam < (LPARAM)s->count ? &s->events[item.lParam] : NULL;
}

static void media_dir( WCHAR *dir )
{
    GetWindowsDirectoryW( dir, MAX_PATH );
    lstrcatW( dir, L"\\Media" );
}

/* the sounds one can choose: "(Немає)", those of Windows\Media, and the
 * event's own when it is from elsewhere */
static void fill_files( HWND dlg, const struct event *ev )
{
    HWND combo = GetDlgItem( dlg, IDC_SOUND_FILE );
    WCHAR dir[MAX_PATH], pattern[MAX_PATH];
    WIN32_FIND_DATAW data;
    HANDLE find;
    int sel = 0;

    SendMessageW( combo, CB_RESETCONTENT, 0, 0 );
    SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)load_string( IDS_NONE ) );
    media_dir( dir );
    swprintf( pattern, ARRAY_SIZE(pattern), L"%s\\*.wav", dir );
    if ((find = FindFirstFileW( pattern, &data )) != INVALID_HANDLE_VALUE)
    {
        do
        {
            WCHAR *dot = wcsrchr( data.cFileName, '.' );
            if (dot) *dot = 0;
            SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)data.cFileName );
        } while (FindNextFileW( find, &data ));
        FindClose( find );
    }
    if (ev && ev->sound[0])
    {
        WCHAR name[MAX_PATH], *file = wcsrchr( ev->sound, '\\' ), *dot;
        int len = lstrlenW( dir );

        if (file && file - ev->sound == len && !wcsnicmp( ev->sound, dir, len ))
        {
            lstrcpynW( name, file + 1, ARRAY_SIZE(name) );
            if ((dot = wcsrchr( name, '.' ))) *dot = 0;
        }
        else lstrcpynW( name, ev->sound, ARRAY_SIZE(name) );
        sel = SendMessageW( combo, CB_FINDSTRINGEXACT, -1, (LPARAM)name );
        if (sel < 0) sel = SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)name );
    }
    SendMessageW( combo, CB_SETCURSEL, sel, 0 );
}

static void event_selected( HWND dlg, struct sounds *s )
{
    struct event *ev = chosen_event( dlg, s );

    fill_files( dlg, ev );
    EnableWindow( GetDlgItem( dlg, IDC_SOUND_FILE ), ev != NULL );
    EnableWindow( GetDlgItem( dlg, IDC_SOUND_LABEL ), ev != NULL );
    EnableWindow( GetDlgItem( dlg, IDC_BROWSE ), ev != NULL );
    EnableWindow( GetDlgItem( dlg, IDC_TEST ), ev && ev->sound[0] );
}

/* a sound of the page's own choosing: the scheme is not the saved one any more */
static void set_event_sound( HWND dlg, struct sounds *s, struct event *ev, const WCHAR *path )
{
    lstrcpynW( ev->sound, path, ARRAY_SIZE(ev->sound) );
    set_item_image( GetDlgItem( dlg, IDC_EVENTS ), ev );
    if (s->current[0])
    {
        struct scheme *scheme = chosen_scheme( dlg, s );
        WCHAR text[160];

        if (scheme)
        {
            int index;

            swprintf( text, ARRAY_SIZE(text), load_string( IDS_CHANGED ), scheme->name );
            index = SendDlgItemMessageW( dlg, IDC_SCHEMES, CB_ADDSTRING, 0, (LPARAM)text );
            SendDlgItemMessageW( dlg, IDC_SCHEMES, CB_SETITEMDATA, index, -1 );
            SendDlgItemMessageW( dlg, IDC_SCHEMES, CB_SETCURSEL, index, 0 );
        }
        s->current[0] = 0;
        EnableWindow( GetDlgItem( dlg, IDC_DELETE_SCHEME ), FALSE );
    }
    EnableWindow( GetDlgItem( dlg, IDC_TEST ), ev->sound[0] != 0 );
    prop_changed( dlg );
}

static void file_chosen( HWND dlg, struct sounds *s )
{
    struct event *ev = chosen_event( dlg, s );
    HWND combo = GetDlgItem( dlg, IDC_SOUND_FILE );
    int sel = SendMessageW( combo, CB_GETCURSEL, 0, 0 );
    WCHAR name[MAX_PATH], path[MAX_PATH], dir[MAX_PATH];

    if (!ev || sel < 0) return;
    if (!sel)
    {
        set_event_sound( dlg, s, ev, L"" );
        return;
    }
    SendMessageW( combo, CB_GETLBTEXT, sel, (LPARAM)name );
    if (wcschr( name, '\\' )) lstrcpyW( path, name );
    else
    {
        media_dir( dir );
        swprintf( path, ARRAY_SIZE(path), L"%s\\%s.wav", dir, name );
    }
    set_event_sound( dlg, s, ev, path );
}

static void browse( HWND dlg, struct sounds *s )
{
    struct event *ev = chosen_event( dlg, s );
    WCHAR path[MAX_PATH] = L"", dir[MAX_PATH], filter[128], title[256];
    OPENFILENAMEW ofn = { sizeof(ofn) };

    if (!ev) return;
    media_dir( dir );
    /* "Звукові файли (*.wav)\0*.wav\0" */
    memset( filter, 0, sizeof(filter) );
    lstrcpynW( filter, load_string( IDS_WAV_FILES ), ARRAY_SIZE(filter) - 8 );
    lstrcpyW( filter + lstrlenW( filter ) + 1, L"*.wav" );
    swprintf( title, ARRAY_SIZE(title), load_string( IDS_BROWSE_TITLE ), ev->label );
    ofn.hwndOwner = dlg;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = ARRAY_SIZE(path);
    ofn.lpstrInitialDir = dir;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
    if (!GetOpenFileNameW( &ofn )) return;
    set_event_sound( dlg, s, ev, path );
    fill_files( dlg, ev );
}

/* every event's sound in a scheme: what the registry keeps under its key */
static void scheme_chosen( HWND dlg, struct sounds *s )
{
    struct scheme *scheme = chosen_scheme( dlg, s );
    HKEY apps, event;
    WCHAR path[200];
    int stale;

    if (!scheme) return;
    /* the "(змінено)" line goes */
    stale = SendDlgItemMessageW( dlg, IDC_SCHEMES, CB_GETCOUNT, 0, 0 ) - 1;
    if (stale >= 0 && SendDlgItemMessageW( dlg, IDC_SCHEMES, CB_GETITEMDATA, stale, 0 ) == -1)
        SendDlgItemMessageW( dlg, IDC_SCHEMES, CB_DELETESTRING, stale, 0 );
    lstrcpyW( s->current, scheme->key );
    if (RegOpenKeyExW( HKEY_CURRENT_USER, apps_key, 0, KEY_READ, &apps )) return;
    for (UINT i = 0; i < s->count; i++)
    {
        struct event *ev = &s->events[i];

        ev->sound[0] = 0;
        swprintf( path, ARRAY_SIZE(path), L"%s\\%s", ev->app, ev->name );
        if (wcsicmp( scheme->key, L".None" ) && !RegOpenKeyExW( apps, path, 0, KEY_READ, &event ))
        {
            read_sound( event, scheme->key, ev->sound );
            RegCloseKey( event );
        }
        set_item_image( GetDlgItem( dlg, IDC_EVENTS ), ev );
    }
    RegCloseKey( apps );
    EnableWindow( GetDlgItem( dlg, IDC_DELETE_SCHEME ), scheme->key[0] != '.' );
    event_selected( dlg, s );
    prop_changed( dlg );
}

static void write_sounds( struct sounds *s, const WCHAR *scheme )
{
    HKEY apps, key;
    WCHAR path[260];

    if (RegOpenKeyExW( HKEY_CURRENT_USER, apps_key, 0, KEY_READ, &apps )) return;
    for (UINT i = 0; i < s->count; i++)
    {
        struct event *ev = &s->events[i];

        swprintf( path, ARRAY_SIZE(path), L"%s\\%s\\%s", ev->app, ev->name, scheme );
        if (RegCreateKeyExW( apps, path, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL )) continue;
        RegSetValueExW( key, NULL, 0, REG_SZ, (const BYTE *)ev->sound, (lstrlenW( ev->sound ) + 1) * sizeof(WCHAR) );
        RegCloseKey( key );
        if (!wcscmp( scheme, L".Current" )) lstrcpyW( ev->saved, ev->sound );
    }
    RegCloseKey( apps );
}

static void apply( struct sounds *s )
{
    HKEY key;

    write_sounds( s, L".Current" );
    if (!RegCreateKeyExW( HKEY_CURRENT_USER, L"AppEvents\\Schemes", 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL ))
    {
        const WCHAR *name = s->current[0] ? s->current : L".Modified";
        RegSetValueExW( key, NULL, 0, REG_SZ, (const BYTE *)name, (lstrlenW( name ) + 1) * sizeof(WCHAR) );
        RegCloseKey( key );
    }
}

static INT_PTR CALLBACK save_proc( HWND dlg, UINT msg, WPARAM wp, LPARAM lp )
{
    switch (msg)
    {
    case WM_INITDIALOG:
        SetWindowLongPtrW( dlg, DWLP_USER, lp );
        SendDlgItemMessageW( dlg, IDC_SCHEME_NAME, EM_LIMITTEXT, 60, 0 );
        return TRUE;
    case WM_COMMAND:
        if (LOWORD( wp ) == IDOK)
        {
            WCHAR *name = (WCHAR *)GetWindowLongPtrW( dlg, DWLP_USER );

            GetDlgItemTextW( dlg, IDC_SCHEME_NAME, name, 61 );
            if (name[0]) EndDialog( dlg, IDOK );
            return TRUE;
        }
        if (LOWORD( wp ) == IDCANCEL) EndDialog( dlg, IDCANCEL );
        return TRUE;
    }
    return FALSE;
}

static void save_scheme( HWND dlg, struct sounds *s )
{
    WCHAR name[64], key_name[64], path[160];
    HKEY key;
    UINT k = 0;

    if (DialogBoxParamW( mmsys_instance, MAKEINTRESOURCEW( IDD_SAVE_SCHEME ), dlg, save_proc, (LPARAM)name ) != IDOK)
        return;
    /* its key: the name's letters and digits */
    for (const WCHAR *p = name; *p && k < ARRAY_SIZE(key_name) - 1; p++)
        if (iswalnum( *p )) key_name[k++] = *p;
    key_name[k] = 0;
    if (!k) lstrcpyW( key_name, L"Scheme" );
    for (UINT i = 0; i < s->scheme_count; i++)
    {
        if (wcsicmp( s->schemes[i].name, name ) && wcsicmp( s->schemes[i].key, key_name )) continue;
        if (s->schemes[i].key[0] == '.' ||
            MessageBoxW( dlg, load_string( IDS_SCHEME_EXISTS ), load_string( IDS_CHANGE_SCHEME ),
                         MB_YESNO | MB_ICONQUESTION ) != IDYES)
            return;
        lstrcpyW( key_name, s->schemes[i].key );
    }
    swprintf( path, ARRAY_SIZE(path), L"%s\\%s", names_key, key_name );
    if (RegCreateKeyExW( HKEY_CURRENT_USER, path, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL )) return;
    RegSetValueExW( key, NULL, 0, REG_SZ, (const BYTE *)name, (lstrlenW( name ) + 1) * sizeof(WCHAR) );
    RegCloseKey( key );
    write_sounds( s, key_name );
    load_schemes( s );
    lstrcpyW( s->current, key_name );
    fill_schemes( dlg, s );
    prop_changed( dlg );
}

static void delete_scheme( HWND dlg, struct sounds *s )
{
    struct scheme *scheme = chosen_scheme( dlg, s );
    WCHAR text[256], path[260];
    HKEY apps;

    if (!scheme || scheme->key[0] == '.') return;
    swprintf( text, ARRAY_SIZE(text), load_string( IDS_DELETE_SCHEME_Q ), scheme->name );
    if (MessageBoxW( dlg, text, load_string( IDS_CHANGE_SCHEME ), MB_YESNO | MB_ICONQUESTION ) != IDYES) return;
    if (!RegOpenKeyExW( HKEY_CURRENT_USER, apps_key, 0, KEY_READ, &apps ))
    {
        for (UINT i = 0; i < s->count; i++)
        {
            swprintf( path, ARRAY_SIZE(path), L"%s\\%s\\%s", s->events[i].app, s->events[i].name, scheme->key );
            RegDeleteKeyW( apps, path );
        }
        RegCloseKey( apps );
    }
    swprintf( path, ARRAY_SIZE(path), L"%s\\%s", names_key, scheme->key );
    RegDeleteKeyW( HKEY_CURRENT_USER, path );
    load_schemes( s );
    lstrcpyW( s->current, L".Default" );
    fill_schemes( dlg, s );
    scheme_chosen( dlg, s );
}

INT_PTR CALLBACK sounds_proc( HWND dlg, UINT msg, WPARAM wp, LPARAM lp )
{
    struct sounds *s = (struct sounds *)GetWindowLongPtrW( dlg, DWLP_USER );

    switch (msg)
    {
    case WM_INITDIALOG:
        if (!(s = calloc( 1, sizeof(*s) ))) return FALSE;
        SetWindowLongPtrW( dlg, DWLP_USER, (LONG_PTR)s );
        load_events( s );
        load_schemes( s );
        fill_schemes( dlg, s );
        fill_tree( dlg, s );
        event_selected( dlg, s );
        return TRUE;
    case WM_COMMAND:
        if (!s) break;
        switch (LOWORD( wp ))
        {
        case IDC_SCHEMES:
            if (HIWORD( wp ) == CBN_SELCHANGE) scheme_chosen( dlg, s );
            return TRUE;
        case IDC_SOUND_FILE:
            if (HIWORD( wp ) == CBN_SELCHANGE) file_chosen( dlg, s );
            return TRUE;
        case IDC_TEST:
        {
            struct event *ev = chosen_event( dlg, s );
            if (ev && ev->sound[0]) PlaySoundW( ev->sound, NULL, SND_FILENAME | SND_ASYNC | SND_NODEFAULT );
            return TRUE;
        }
        case IDC_BROWSE:
            browse( dlg, s );
            return TRUE;
        case IDC_SAVE_AS:
            save_scheme( dlg, s );
            return TRUE;
        case IDC_DELETE_SCHEME:
            delete_scheme( dlg, s );
            return TRUE;
        }
        break;
    case WM_NOTIFY:
    {
        NMHDR *hdr = (NMHDR *)lp;

        if (!s) break;
        if (hdr->idFrom == IDC_EVENTS && hdr->code == TVN_SELCHANGEDW && !s->filling)
        {
            event_selected( dlg, s );
            return TRUE;
        }
        if (hdr->idFrom == IDC_EVENTS && hdr->code == NM_DBLCLK)
        {
            struct event *ev = chosen_event( dlg, s );
            if (ev && ev->sound[0]) PlaySoundW( ev->sound, NULL, SND_FILENAME | SND_ASYNC | SND_NODEFAULT );
            return TRUE;
        }
        if (hdr->code == PSN_APPLY)
        {
            apply( s );
            SetWindowLongPtrW( dlg, DWLP_MSGRESULT, PSNRET_NOERROR );
            return TRUE;
        }
        break;
    }
    case WM_DESTROY:
        PlaySoundW( NULL, NULL, 0 );
        free( s );
        SetWindowLongPtrW( dlg, DWLP_USER, 0 );
        break;
    }
    return FALSE;
}

/**********************************************************************
 *          Комунікації
 */

static const WCHAR ducking_key[] = L"Software\\Microsoft\\Multimedia\\Audio";

INT_PTR CALLBACK communications_proc( HWND dlg, UINT msg, WPARAM wp, LPARAM lp )
{
    /* UserDuckingPreference: 0 mute, 1 by 80 %, 2 by 50 %, 3 nothing */
    static const UINT buttons[] = { IDC_COMM_MUTE, IDC_COMM_80, IDC_COMM_50, IDC_COMM_NOTHING };

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        DWORD value = 1, size = sizeof(value);

        RegGetValueW( HKEY_CURRENT_USER, ducking_key, L"UserDuckingPreference", RRF_RT_REG_DWORD, NULL, &value, &size );
        for (UINT i = 0; i < ARRAY_SIZE(buttons); i++)
            CheckDlgButton( dlg, buttons[i], i == min( value, 3 ) ? BST_CHECKED : BST_UNCHECKED );
        SendDlgItemMessageW( dlg, IDC_COMM_ICON, STM_SETICON,
                             (WPARAM)LoadImageW( mmsys_instance, MAKEINTRESOURCEW( IDI_COMMUNICATIONS ), IMAGE_ICON,
                                                 px( 32 ), px( 32 ), LR_SHARED ), 0 );
        return TRUE;
    }
    case WM_COMMAND:
        for (UINT i = 0; i < ARRAY_SIZE(buttons); i++)
        {
            if (LOWORD( wp ) != buttons[i]) continue;
            for (UINT k = 0; k < ARRAY_SIZE(buttons); k++)
                CheckDlgButton( dlg, buttons[k], k == i ? BST_CHECKED : BST_UNCHECKED );
            prop_changed( dlg );
            return TRUE;
        }
        break;
    case WM_NOTIFY:
        if (((NMHDR *)lp)->code == PSN_APPLY)
        {
            HKEY key;

            for (DWORD i = 0; i < ARRAY_SIZE(buttons); i++)
            {
                if (IsDlgButtonChecked( dlg, buttons[i] ) != BST_CHECKED) continue;
                if (!RegCreateKeyExW( HKEY_CURRENT_USER, ducking_key, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL ))
                {
                    RegSetValueExW( key, L"UserDuckingPreference", 0, REG_DWORD, (BYTE *)&i, sizeof(i) );
                    RegCloseKey( key );
                }
            }
            SetWindowLongPtrW( dlg, DWLP_MSGRESULT, PSNRET_NOERROR );
            return TRUE;
        }
        break;
    }
    return FALSE;
}
