/*
 * Power Options of the Control Panel: "Настройки графіки"
 *
 * Windows 10's Graphics settings (Settings > System > Display > Graphics
 * settings, 22H2), here in the Control Panel: the default graphics settings
 * (variable refresh rate, hardware-accelerated GPU scheduling), then the
 * graphics performance preference: "Класична програма" or "Програма
 * Microsoft Store", "Огляд", and the programs added, each with its icon,
 * its name, its preference and its path, and "Параметри" / "Видалити".
 * "Параметри" asks "Дозволити Windows вирішувати", "Економія енергії" or
 * "Висока продуктивність", with the card each means. The choice is kept as
 * Windows keeps it (HKCU\Software\Microsoft\DirectX\UserGpuPreferences,
 * "GpuPreference=N;") and a program gets that card when it starts (kernelbase,
 * docs/power.md).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdlib.h>

#include "powercpl.h"
#include "commctrl.h"
#include "commdlg.h"
#include "winver.h"
#include "shellapi.h"

#define ID_VRR           100
#define ID_HAGS          101
#define ID_KIND          102
#define ID_BROWSE        103
#define ID_OPTIONS_FIRST 200
#define ID_REMOVE_FIRST  400
#define ID_ICON_FIRST    600
#define MAX_APPS         64

static const WCHAR prefs_key[] = L"Software\\Microsoft\\DirectX\\UserGpuPreferences";
static const WCHAR global_value[] = L"DirectXUserGlobalSettings";
static const WCHAR drivers_key[] = L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers";

struct app
{
    WCHAR path[MAX_PATH];
    DWORD pref;
    HICON icon;
};

struct graphics_data
{
    struct app apps[MAX_APPS];
    UINT count;
    HFONT bold;
};

static DWORD read_pref( const WCHAR *value )
{
    const WCHAR *p = wcsstr( value, L"GpuPreference=" );

    return p ? wcstoul( p + 14, NULL, 10 ) : 0;
}

static void write_pref( const WCHAR *path, DWORD pref )
{
    WCHAR value[64];
    HKEY key;

    if (RegCreateKeyExW( HKEY_CURRENT_USER, prefs_key, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL )) return;
    swprintf( value, ARRAY_SIZE(value), L"GpuPreference=%lu;", pref );
    RegSetValueExW( key, path, 0, REG_SZ, (BYTE *)value, (wcslen( value ) + 1) * sizeof(WCHAR) );
    RegCloseKey( key );
}

static void load_apps( struct graphics_data *data )
{
    WCHAR name[MAX_PATH], value[128];
    DWORD name_len, value_size, type;
    HKEY key;

    for (UINT i = 0; i < data->count; i++) if (data->apps[i].icon) DestroyIcon( data->apps[i].icon );
    data->count = 0;
    if (RegOpenKeyExW( HKEY_CURRENT_USER, prefs_key, 0, KEY_READ, &key )) return;
    for (DWORD i = 0; data->count < MAX_APPS; i++)
    {
        name_len = ARRAY_SIZE(name);
        value_size = sizeof(value) - sizeof(WCHAR);
        if (RegEnumValueW( key, i, name, &name_len, NULL, &type, (BYTE *)value, &value_size )) break;
        if (type != REG_SZ || !wcsicmp( name, global_value )) continue;
        value[value_size / sizeof(WCHAR)] = 0;
        lstrcpynW( data->apps[data->count].path, name, MAX_PATH );
        data->apps[data->count].pref = read_pref( value );
        ExtractIconExW( name, 0, &data->apps[data->count].icon, NULL, 1 );
        data->count++;
    }
    RegCloseKey( key );
}

/* the program's own name (its FileDescription), or its file's */
static void app_name( const WCHAR *path, WCHAR *name, DWORD count )
{
    DWORD handle, size = GetFileVersionInfoSizeW( path, &handle );
    const WCHAR *base = wcsrchr( path, '\\' );
    void *info;

    lstrcpynW( name, base ? base + 1 : path, count );
    if (size && (info = malloc( size )))
    {
        struct { WORD lang, codepage; } *trans;
        UINT len;
        WCHAR query[64], *desc;

        if (GetFileVersionInfoW( path, 0, size, info ) &&
            VerQueryValueW( info, L"\\VarFileInfo\\Translation", (void **)&trans, &len ) && len >= sizeof(*trans))
        {
            swprintf( query, ARRAY_SIZE(query), L"\\StringFileInfo\\%04x%04x\\FileDescription", trans->lang, trans->codepage );
            if (VerQueryValueW( info, query, (void **)&desc, &len ) && len > 1 && desc[0]) lstrcpynW( name, desc, count );
        }
        free( info );
    }
}

static const WCHAR *pref_text( DWORD pref )
{
    return load_string( pref == 1 ? IDS_POWER_SAVING : pref == 2 ? IDS_HIGH_PERFORMANCE : IDS_LET_WINDOWS_DECIDE );
}

static BOOL reg_flag( HKEY root, const WCHAR *path, const WCHAR *name, const WCHAR *text_key, BOOL fallback )
{
    WCHAR value[256];
    DWORD size = sizeof(value), dword;

    if (text_key)
    {
        if (RegGetValueW( root, path, name, RRF_RT_REG_SZ, NULL, value, &size )) return fallback;
        return !wcsstr( value, text_key );
    }
    size = sizeof(dword);
    if (RegGetValueW( root, path, name, RRF_RT_REG_DWORD, NULL, &dword, &size )) return fallback;
    return dword == 2;
}

void graphics_build( struct view *view )
{
    struct page_state *state = view_state( view );
    struct graphics_data *data = state->data;
    int width = view_content_width( view );
    HKEY key;
    HWND combo, check;

    if (!data)
    {
        LOGFONTW font;

        if (!(data = state->data = calloc( 1, sizeof(*data) ))) return;
        GetObjectW( view_font( STYLE_BOLD ), sizeof(font), &font );
        data->bold = CreateFontIndirectW( &font );
    }
    load_apps( data );

    nav_power_tasks( view );
    view_text( view, STYLE_TITLE, load_string( IDS_GRAPHICS_TITLE ) );
    if (RegOpenKeyExW( HKEY_LOCAL_MACHINE, ARCTIC_GPU_KEY, 0, KEY_READ, &key )) view_text( view, STYLE_BODY, load_string( IDS_ONE_GPU ) );
    else RegCloseKey( key );
    view_space( view, px( 16 ) );

    /* the default graphics settings */
    view_group( view, load_string( IDS_DEFAULT_GRAPHICS ) );
    view_space( view, px( 8 ) );
    check = view_control( view, WC_BUTTONW, load_string( IDS_VRR ), BS_AUTOCHECKBOX | WS_TABSTOP, width, px( 20 ), 0, ID_VRR );
    SendMessageW( check, BM_SETCHECK, reg_flag( HKEY_CURRENT_USER, prefs_key, global_value, L"VRROptimizeEnable=0", TRUE ), 0 );
    view_text_at( view, STYLE_SMALL, load_string( IDS_VRR_TEXT ), px( 18 ) );
    view_space( view, px( 8 ) );
    check = view_control( view, WC_BUTTONW, load_string( IDS_HAGS ), BS_AUTOCHECKBOX | WS_TABSTOP, width, px( 20 ), 0, ID_HAGS );
    SendMessageW( check, BM_SETCHECK, reg_flag( HKEY_LOCAL_MACHINE, drivers_key, L"HwSchMode", NULL, FALSE ), 0 );
    view_text_at( view, STYLE_SMALL, load_string( IDS_HAGS_TEXT ), px( 18 ) );
    view_space( view, px( 18 ) );

    /* the graphics performance preference */
    view_group( view, load_string( IDS_GRAPHICS_PREFERENCE ) );
    view_space( view, px( 6 ) );
    view_text( view, STYLE_BODY, load_string( IDS_GRAPHICS_TEXT ) );
    view_space( view, px( 12 ) );
    view_text( view, STYLE_BODY, load_string( IDS_CHOOSE_APP ) );
    view_space( view, px( 4 ) );
    view_row_begin( view );
    combo = view_control_beside( view, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, 0, px( 240 ), px( 120 ), ID_KIND );
    SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)load_string( IDS_CLASSIC_APP ) );
    SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)load_string( IDS_STORE_APP ) );
    SendMessageW( combo, CB_SETCURSEL, 0, 0 );
    view_control_beside( view, WC_BUTTONW, load_string( IDS_BROWSE ), BS_PUSHBUTTON | WS_TABSTOP, px( 252 ), px( 98 ),
                         px( 24 ), ID_BROWSE );
    view_row_end( view );
    view_space( view, px( 16 ) );

    if (!data->count) view_text( view, STYLE_GRAY, load_string( IDS_NO_APPS ) );
    for (UINT i = 0; i < data->count; i++)
    {
        const struct app *app = &data->apps[i];
        WCHAR name[256];
        HWND icon;

        app_name( app->path, name, ARRAY_SIZE(name) );
        view_row_begin( view );
        icon = view_control_beside( view, WC_STATICW, L"", SS_ICON | SS_CENTERIMAGE | SS_REALSIZECONTROL, 0, px( 32 ),
                                    px( 32 ), ID_ICON_FIRST + i );
        if (app->icon) SendMessageW( icon, STM_SETICON, (WPARAM)app->icon, 0 );
        view_text_at( view, STYLE_BOLD, name, px( 44 ) );
        view_row_end( view );
        view_text_at( view, STYLE_BODY, pref_text( app->pref ), px( 44 ) );
        view_text_at( view, STYLE_GRAY, app->path, px( 44 ) );
        view_space( view, px( 6 ) );
        view_row_begin( view );
        view_control_beside( view, WC_BUTTONW, load_string( IDS_OPTIONS ), BS_PUSHBUTTON | WS_TABSTOP, px( 44 ), px( 98 ),
                             px( 24 ), ID_OPTIONS_FIRST + i );
        view_control_beside( view, WC_BUTTONW, load_string( IDS_REMOVE ), BS_PUSHBUTTON | WS_TABSTOP, px( 150 ), px( 98 ),
                             px( 24 ), ID_REMOVE_FIRST + i );
        view_row_end( view );
        view_space( view, px( 16 ) );
    }
}

/**********************************************************************
 *          "Параметри графіки"
 */

struct options
{
    const WCHAR *path;
    DWORD pref;
};

static INT_PTR CALLBACK options_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    struct options *options = (struct options *)GetWindowLongPtrW( hwnd, DWLP_USER );

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        WCHAR name[256], gpu[256];
        DWORD size;
        LOGFONTW font;
        HFONT bold;

        options = (struct options *)lp;
        SetWindowLongPtrW( hwnd, DWLP_USER, lp );
        app_name( options->path, name, ARRAY_SIZE(name) );
        SetDlgItemTextW( hwnd, IDC_GFX_QUESTION, name );
        GetObjectW( (HFONT)SendMessageW( hwnd, WM_GETFONT, 0, 0 ), sizeof(font), &font );
        font.lfWeight = FW_BOLD;
        if ((bold = CreateFontIndirectW( &font ))) SendDlgItemMessageW( hwnd, IDC_GFX_QUESTION, WM_SETFONT, (WPARAM)bold, FALSE );
        size = sizeof(gpu);
        if (RegGetValueW( HKEY_LOCAL_MACHINE, ARCTIC_GPU_KEY, ARCTIC_GPU_POWER_SAVING_NAME, RRF_RT_REG_SZ, NULL, gpu, &size ))
            gpu[0] = 0;
        SetDlgItemTextW( hwnd, IDC_GFX_SAVING_GPU, gpu[0] ? format_string( IDS_GPU_LINE, gpu ) : L"" );
        size = sizeof(gpu);
        if (RegGetValueW( HKEY_LOCAL_MACHINE, ARCTIC_GPU_KEY, ARCTIC_GPU_HIGH_PERF_NAME, RRF_RT_REG_SZ, NULL, gpu, &size ))
            gpu[0] = 0;
        SetDlgItemTextW( hwnd, IDC_GFX_FAST_GPU, gpu[0] ? format_string( IDS_GPU_LINE, gpu ) : L"" );
        CheckRadioButton( hwnd, IDC_GFX_DECIDE, IDC_GFX_FAST,
                          options->pref == 1 ? IDC_GFX_SAVING : options->pref == 2 ? IDC_GFX_FAST : IDC_GFX_DECIDE );
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD( wp ))
        {
        case IDOK:
            options->pref = IsDlgButtonChecked( hwnd, IDC_GFX_SAVING ) ? 1 : IsDlgButtonChecked( hwnd, IDC_GFX_FAST ) ? 2 : 0;
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

static void browse( struct view *view, struct graphics_data *data )
{
    WCHAR file[MAX_PATH] = L"", filter[256];
    OPENFILENAMEW ofn = { sizeof(ofn) };
    HKEY key;

    /* "Програми (*.exe)" */
    swprintf( filter, ARRAY_SIZE(filter), L"%s%c*.exe%c", load_string( IDS_PROGRAMS_FILTER ), 0, 0 );
    ofn.hwndOwner = GetAncestor( view_window( view ), GA_ROOT );
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = ARRAY_SIZE(file);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW( &ofn )) return;
    if (!RegOpenKeyExW( HKEY_CURRENT_USER, prefs_key, 0, KEY_READ, &key ))
    {
        BOOL there = !RegQueryValueExW( key, file, NULL, NULL, NULL, NULL );
        RegCloseKey( key );
        if (there)
        {
            MessageBoxW( ofn.hwndOwner, load_string( IDS_ALREADY_ADDED ), load_string( IDS_GRAPHICS_TITLE ), MB_ICONINFORMATION );
            return;
        }
    }
    write_pref( file, 0 );
    view_rebuild( view );
}

BOOL graphics_command( struct view *view, UINT id, UINT code, HWND control )
{
    struct graphics_data *data = view_state( view )->data;
    HKEY key;

    if (!data) return FALSE;
    if (id == ID_KIND && code == CBN_SELCHANGE)
    {
        /* there are no Store programs to choose from here */
        EnableWindow( GetDlgItem( view_window( view ), ID_BROWSE ), !SendMessageW( control, CB_GETCURSEL, 0, 0 ) );
        return TRUE;
    }
    if (code != BN_CLICKED) return FALSE;
    if (id == ID_BROWSE)
    {
        browse( view, data );
        return TRUE;
    }
    if (id == ID_VRR)
    {
        const WCHAR *value = SendMessageW( control, BM_GETCHECK, 0, 0 ) == BST_CHECKED ? L"VRROptimizeEnable=1;" : L"VRROptimizeEnable=0;";
        if (!RegCreateKeyExW( HKEY_CURRENT_USER, prefs_key, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL ))
        {
            RegSetValueExW( key, global_value, 0, REG_SZ, (const BYTE *)value, (wcslen( value ) + 1) * sizeof(WCHAR) );
            RegCloseKey( key );
        }
        return TRUE;
    }
    if (id == ID_HAGS)
    {
        DWORD mode = SendMessageW( control, BM_GETCHECK, 0, 0 ) == BST_CHECKED ? 2 : 1;
        if (!RegCreateKeyExW( HKEY_LOCAL_MACHINE, drivers_key, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL ))
        {
            RegSetValueExW( key, L"HwSchMode", 0, REG_DWORD, (BYTE *)&mode, sizeof(mode) );
            RegCloseKey( key );
        }
        MessageBoxW( GetAncestor( control, GA_ROOT ), load_string( IDS_RESTART_NEEDED ), load_string( IDS_GRAPHICS_TITLE ),
                     MB_ICONINFORMATION );
        return TRUE;
    }
    if (id >= ID_OPTIONS_FIRST && id < ID_OPTIONS_FIRST + data->count)
    {
        struct options options = { data->apps[id - ID_OPTIONS_FIRST].path, data->apps[id - ID_OPTIONS_FIRST].pref };

        if (DialogBoxParamW( powercpl_instance, MAKEINTRESOURCEW( IDD_GRAPHICS_OPTIONS ), GetAncestor( control, GA_ROOT ),
                             options_proc, (LPARAM)&options ) == IDOK)
        {
            write_pref( options.path, options.pref );
            view_rebuild( view );
        }
        return TRUE;
    }
    if (id >= ID_REMOVE_FIRST && id < ID_REMOVE_FIRST + data->count)
    {
        if (!RegOpenKeyExW( HKEY_CURRENT_USER, prefs_key, 0, KEY_SET_VALUE, &key ))
        {
            RegDeleteValueW( key, data->apps[id - ID_REMOVE_FIRST].path );
            RegCloseKey( key );
        }
        view_rebuild( view );
        return TRUE;
    }
    return FALSE;
}
