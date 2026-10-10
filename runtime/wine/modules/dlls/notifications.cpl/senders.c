/*
 * Notifications applet: the senders and their settings
 *
 * Each program that sent a notification has a key of its own under
 * HKCU\...\Notifications\Settings (the shell makes it): its name and icon,
 * when it last sent one, and what the user allows it, with the values
 * Windows has (Enabled, ShowBanner, PlaySound, Rank; Priority puts it on
 * the priority list of focus assist).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "notifications.h"
#include "shellapi.h"

DWORD reg_dword( const WCHAR *path, const WCHAR *name, DWORD fallback )
{
    DWORD value, size = sizeof(value);

    if (RegGetValueW( HKEY_CURRENT_USER, path, name, RRF_RT_REG_DWORD, NULL, &value, &size )) return fallback;
    return value;
}

void set_reg_dword( const WCHAR *path, const WCHAR *name, DWORD value )
{
    HKEY key;

    if (RegCreateKeyExW( HKEY_CURRENT_USER, path, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL )) return;
    RegSetValueExW( key, name, 0, REG_DWORD, (BYTE *)&value, sizeof(value) );
    RegCloseKey( key );
}

static int compare_recent( const void *a, const void *b )
{
    const struct sender *x = a, *y = b;

    if (x->last != y->last) return x->last > y->last ? -1 : 1;
    return lstrcmpiW( x->name, y->name );
}

static int compare_name( const void *a, const void *b )
{
    const struct sender *x = a, *y = b;

    return lstrcmpiW( x->name, y->name );
}

/* the senders the shell knows, the newest first or by name */
UINT load_senders( struct sender **list, BOOL by_name )
{
    UINT count = 0, max = 0;
    WCHAR sub[MAX_PATH];
    DWORD len;
    HKEY key, item;

    *list = NULL;
    if (RegOpenKeyExW( HKEY_CURRENT_USER, SETTINGS_KEY, 0, KEY_READ, &key )) return 0;
    for (DWORD i = 0; len = ARRAY_SIZE(sub), !RegEnumKeyExW( key, i, sub, &len, NULL, NULL, NULL, NULL ); i++)
    {
        struct sender *s;
        DWORD size;

        if (RegOpenKeyExW( key, sub, 0, KEY_READ, &item )) continue;
        if (count == max)
        {
            struct sender *grown = realloc( *list, (max = max ? max * 2 : 16) * sizeof(**list) );
            if (!grown) { RegCloseKey( item ); break; }
            *list = grown;
        }
        s = &(*list)[count];
        memset( s, 0, sizeof(*s) );
        lstrcpynW( s->key, sub, ARRAY_SIZE(s->key) );
        size = sizeof(s->name);
        /* only programs that sent something: a key the shell made */
        if (RegGetValueW( item, NULL, L"DisplayName", RRF_RT_REG_SZ, NULL, s->name, &size ))
        {
            RegCloseKey( item );
            continue;
        }
        size = sizeof(s->icon);
        if (RegGetValueW( item, NULL, L"IconPath", RRF_RT_REG_SZ, NULL, s->icon, &size )) s->icon[0] = 0;
        size = sizeof(s->last);
        if (RegGetValueW( item, NULL, L"LastNotification", RRF_RT_REG_QWORD, NULL, &s->last, &size )) s->last = 0;
#define FLAG(field, name, fallback) \
        size = sizeof(s->field); \
        if (RegGetValueW( item, NULL, name, RRF_RT_REG_DWORD, NULL, &s->field, &size )) s->field = fallback;
        FLAG( enabled, L"Enabled", 1 )
        FLAG( banner, L"ShowBanner", 1 )
        FLAG( sound, L"PlaySound", 1 )
        FLAG( rank, L"Rank", RANK_NORMAL )
        FLAG( priority, L"Priority", 0 )
#undef FLAG
        RegCloseKey( item );
        count++;
    }
    RegCloseKey( key );
    if (count) qsort( *list, count, sizeof(**list), by_name ? compare_name : compare_recent );
    return count;
}

void save_sender( const struct sender *s )
{
    WCHAR path[MAX_PATH * 2];

    swprintf( path, ARRAY_SIZE(path), L"%s\\%s", SETTINGS_KEY, s->key );
    set_reg_dword( path, L"Enabled", s->enabled );
    set_reg_dword( path, L"ShowBanner", s->banner );
    set_reg_dword( path, L"PlaySound", s->sound );
    set_reg_dword( path, L"Rank", s->rank );
    set_reg_dword( path, L"Priority", s->priority );
}

void sender_path( const struct sender *s, WCHAR *path, DWORD count )
{
    lstrcpynW( path, s->key, count );
    for (WCHAR *p = path; *p; p++) if (*p == '/') *p = '\\';
}

/* "Увімк.: банери, звуки", as the list of senders of Windows 10 says it */
const WCHAR *sender_state( const struct sender *s )
{
    if (!s->enabled) return load_string( IDS_OFF );
    if (s->banner && s->sound) return load_string( IDS_ON_BANNERS_SOUNDS );
    if (s->banner) return load_string( IDS_ON_BANNERS );
    if (s->sound) return load_string( IDS_ON_SOUNDS );
    return load_string( IDS_ON_NOTHING );
}

/**********************************************************************
 *          The settings of one sender
 */

static void enable_sender_controls( HWND hwnd )
{
    BOOL on = SendDlgItemMessageW( hwnd, IDC_S_ENABLED, BM_GETCHECK, 0, 0 ) == BST_CHECKED;
    static const UINT ids[] = { IDC_S_BANNER, IDC_S_SOUND, IDC_S_TOP, IDC_S_HIGH, IDC_S_NORMAL };

    for (UINT i = 0; i < ARRAY_SIZE(ids); i++) EnableWindow( GetDlgItem( hwnd, ids[i] ), on );
}

static INT_PTR CALLBACK sender_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    struct sender *s = (struct sender *)GetWindowLongPtrW( hwnd, DWLP_USER );

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        WCHAR caption[256];
        HICON icon = NULL;
        LOGFONTW font;
        HFONT bold;

        s = (struct sender *)lp;
        SetWindowLongPtrW( hwnd, DWLP_USER, lp );
        swprintf( caption, ARRAY_SIZE(caption), load_string( IDS_SENDER_CAPTION ), s->name );
        SetWindowTextW( hwnd, caption );
        SetDlgItemTextW( hwnd, IDC_S_NAME, s->name );
        GetObjectW( (HFONT)SendMessageW( hwnd, WM_GETFONT, 0, 0 ), sizeof(font), &font );
        font.lfHeight = font.lfHeight * 3 / 2;
        if ((bold = CreateFontIndirectW( &font ))) SendDlgItemMessageW( hwnd, IDC_S_NAME, WM_SETFONT, (WPARAM)bold, FALSE );
        if (s->icon[0]) ExtractIconExW( s->icon, 0, &icon, NULL, 1 );
        if (icon) SendDlgItemMessageW( hwnd, IDC_S_ICON, STM_SETICON, (WPARAM)icon, 0 );
        SendDlgItemMessageW( hwnd, IDC_S_ENABLED, BM_SETCHECK, s->enabled ? BST_CHECKED : BST_UNCHECKED, 0 );
        CheckDlgButton( hwnd, IDC_S_BANNER, s->banner ? BST_CHECKED : BST_UNCHECKED );
        CheckDlgButton( hwnd, IDC_S_SOUND, s->sound ? BST_CHECKED : BST_UNCHECKED );
        CheckRadioButton( hwnd, IDC_S_TOP, IDC_S_NORMAL,
                          s->rank >= RANK_TOP ? IDC_S_TOP : s->rank >= RANK_HIGH ? IDC_S_HIGH : IDC_S_NORMAL );
        enable_sender_controls( hwnd );
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD( wp ))
        {
        case IDC_S_ENABLED:
            enable_sender_controls( hwnd );
            return TRUE;
        case IDOK:
            s->enabled = SendDlgItemMessageW( hwnd, IDC_S_ENABLED, BM_GETCHECK, 0, 0 ) == BST_CHECKED;
            s->banner = IsDlgButtonChecked( hwnd, IDC_S_BANNER ) == BST_CHECKED;
            s->sound = IsDlgButtonChecked( hwnd, IDC_S_SOUND ) == BST_CHECKED;
            s->rank = IsDlgButtonChecked( hwnd, IDC_S_TOP ) ? RANK_TOP : IsDlgButtonChecked( hwnd, IDC_S_HIGH ) ? RANK_HIGH : RANK_NORMAL;
            EndDialog( hwnd, IDOK );
            return TRUE;
        case IDCANCEL:
            EndDialog( hwnd, IDCANCEL );
            return TRUE;
        }
        break;
    case WM_DESTROY:
    {
        HICON icon = (HICON)SendDlgItemMessageW( hwnd, IDC_S_ICON, STM_GETICON, 0, 0 );
        HFONT font = (HFONT)SendDlgItemMessageW( hwnd, IDC_S_NAME, WM_GETFONT, 0, 0 );

        if (icon) DestroyIcon( icon );
        if (font && font != (HFONT)SendMessageW( hwnd, WM_GETFONT, 0, 0 )) DeleteObject( font );
        break;
    }
    }
    return FALSE;
}

BOOL edit_sender( HWND owner, struct sender *s )
{
    if (DialogBoxParamW( instance, MAKEINTRESOURCEW( IDD_SENDER ), owner, sender_proc, (LPARAM)s ) != IDOK) return FALSE;
    save_sender( s );
    return TRUE;
}
