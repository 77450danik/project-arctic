/*
 * Task Manager: the Startup tab
 *
 * The programs the Run keys and the Startup folders start at sign-in.
 * Turning one off is kept as Windows keeps it, under
 * Explorer\StartupApproved: a value of the item's name whose first byte is
 * odd, then the time it was turned off. Explorer leaves those out
 * (ReactOS 0081).
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS
#include "taskmgr.h"
#include "shlobj.h"
#include "shellapi.h"
#include "shlwapi.h"
#include "winver.h"

#define APPROVED_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\"

struct item
{
    HKEY     root;                 /* where its approval is kept */
    WCHAR    list[16];             /* "Run" or "StartupFolder" */
    WCHAR    name[MAX_PATH];       /* the value's or the shortcut's name */
    WCHAR    command[MAX_PATH * 2];
    WCHAR    path[MAX_PATH];       /* the program it starts */
    WCHAR    title[256], publisher[256];
    BOOL     enabled;
    int      icon;
};

static HWND page;
static struct grid *grid;
static struct item *items;
static UINT item_count;
static UINT ticks;

static BOOL approved( HKEY root, const WCHAR *list, const WCHAR *name )
{
    WCHAR key[128];
    BYTE data[12];
    DWORD size = sizeof(data);

    swprintf( key, ARRAY_SIZE(key), L"%s%s", APPROVED_KEY, list );
    if (RegGetValueW( root, key, name, RRF_RT_REG_BINARY, NULL, data, &size ) || !size) return TRUE;
    return !(data[0] & 1);
}

static void version_strings( const WCHAR *path, WCHAR *title, size_t tlen, WCHAR *publisher, size_t plen )
{
    DWORD handle, size;
    void *data;
    struct { WORD lang, cp; } *tr;
    UINT trlen, vlen;
    WCHAR key[80], *value;

    publisher[0] = 0;
    if (!path[0] || !(size = GetFileVersionInfoSizeW( path, &handle )) || !(data = malloc( size ))) return;
    if (GetFileVersionInfoW( path, 0, size, data ) &&
        VerQueryValueW( data, L"\\VarFileInfo\\Translation", (void **)&tr, &trlen ) && trlen >= sizeof(*tr))
    {
        swprintf( key, ARRAY_SIZE(key), L"\\StringFileInfo\\%04x%04x\\FileDescription", tr->lang, tr->cp );
        if (VerQueryValueW( data, key, (void **)&value, &vlen ) && vlen > 1 && value[0]) lstrcpynW( title, value, tlen );
        swprintf( key, ARRAY_SIZE(key), L"\\StringFileInfo\\%04x%04x\\CompanyName", tr->lang, tr->cp );
        if (VerQueryValueW( data, key, (void **)&value, &vlen ) && vlen > 1) lstrcpynW( publisher, value, plen );
    }
    free( data );
}

/* the program of a command line: the first word, quoted or not, with ".exe" found */
static void program_of( const WCHAR *command, WCHAR *path, size_t len )
{
    WCHAR buf[MAX_PATH * 2], expanded[MAX_PATH * 2], *p;

    ExpandEnvironmentStringsW( command, expanded, ARRAY_SIZE(expanded) );
    lstrcpynW( buf, expanded, ARRAY_SIZE(buf) );
    if (buf[0] == '"')
    {
        lstrcpynW( path, buf + 1, len );
        if ((p = wcschr( path, '"' ))) *p = 0;
        return;
    }
    if ((p = wcsstr( buf, L".exe" )) || (p = wcsstr( buf, L".EXE" ))) p[4] = 0;
    else if ((p = wcschr( buf, ' ' ))) *p = 0;
    lstrcpynW( path, buf, len );
}

static void add_item( HKEY root, const WCHAR *list, const WCHAR *name, const WCHAR *command, const WCHAR *path )
{
    struct item *it;

    if (!(items = realloc( items, (item_count + 1) * sizeof(*items) ))) { item_count = 0; return; }
    it = &items[item_count++];
    memset( it, 0, sizeof(*it) );
    it->root = root;
    lstrcpynW( it->list, list, ARRAY_SIZE(it->list) );
    lstrcpynW( it->name, name, ARRAY_SIZE(it->name) );
    lstrcpynW( it->command, command, ARRAY_SIZE(it->command) );
    lstrcpynW( it->path, path, ARRAY_SIZE(it->path) );
    lstrcpynW( it->title, name, ARRAY_SIZE(it->title) );
    PathRemoveExtensionW( it->title );
    version_strings( it->path, it->title, ARRAY_SIZE(it->title), it->publisher, ARRAY_SIZE(it->publisher) );
    it->enabled = approved( root, list, name );
    it->icon = -1;
    if (path[0])
    {
        HICON icon = NULL;
        if (ExtractIconExW( path, 0, NULL, &icon, 1 ) && icon)
        {
            it->icon = ImageList_AddIcon( small_icons, icon );
            DestroyIcon( icon );
        }
    }
}

static void read_run_key( HKEY root, const WCHAR *subkey )
{
    HKEY key;
    WCHAR name[256], data[MAX_PATH * 2], path[MAX_PATH];
    DWORD nlen, dlen, type;

    if (RegOpenKeyExW( root, subkey, 0, KEY_READ, &key )) return;
    for (DWORD i = 0;; i++)
    {
        nlen = ARRAY_SIZE(name);
        dlen = sizeof(data);
        if (RegEnumValueW( key, i, name, &nlen, NULL, &type, (BYTE *)data, &dlen )) break;
        if (type != REG_SZ && type != REG_EXPAND_SZ) continue;
        program_of( data, path, ARRAY_SIZE(path) );
        add_item( root, L"Run", name, data, path );
    }
    RegCloseKey( key );
}

static void read_folder( int csidl, HKEY root )
{
    WCHAR dir[MAX_PATH], pattern[MAX_PATH], file[MAX_PATH], target[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE find;

    if (FAILED( SHGetFolderPathW( NULL, csidl, NULL, SHGFP_TYPE_CURRENT, dir ) )) return;
    PathCombineW( pattern, dir, L"*" );
    if ((find = FindFirstFileW( pattern, &fd )) == INVALID_HANDLE_VALUE) return;
    do
    {
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN)) continue;
        if (!wcsicmp( fd.cFileName, L"desktop.ini" )) continue;
        PathCombineW( file, dir, fd.cFileName );
        lstrcpynW( target, file, ARRAY_SIZE(target) );
        /* a shortcut's own program, for its name and icon */
        if (!wcsicmp( PathFindExtensionW( file ), L".lnk" ))
        {
            IShellLinkW *link;
            IPersistFile *pf;
            if (SUCCEEDED( CoCreateInstance( &CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link ) ))
            {
                if (SUCCEEDED( IShellLinkW_QueryInterface( link, &IID_IPersistFile, (void **)&pf ) ))
                {
                    if (SUCCEEDED( IPersistFile_Load( pf, file, STGM_READ ) ))
                        IShellLinkW_GetPath( link, target, ARRAY_SIZE(target), NULL, 0 );
                    IPersistFile_Release( pf );
                }
                IShellLinkW_Release( link );
            }
        }
        add_item( root, L"StartupFolder", fd.cFileName, file, target );
    } while (FindNextFileW( find, &fd ));
    FindClose( find );
}

static void read_items(void)
{
    free( items );
    items = NULL;
    item_count = 0;
    read_run_key( HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run" );
    read_run_key( HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run" );
    read_folder( CSIDL_STARTUP, HKEY_CURRENT_USER );
    read_folder( CSIDL_COMMON_STARTUP, HKEY_LOCAL_MACHINE );
}

static void fill(void)
{
    grid_begin( grid );
    for (UINT i = 0; i < item_count; i++)
    {
        struct grid_row *row = grid_add_row( grid );
        if (!row) break;
        grid_set_cell( row, 0, items[i].title );
        grid_set_cell( row, 1, items[i].publisher );
        grid_set_cell( row, 2, load_string( items[i].enabled ? IDS_ENABLED : IDS_DISABLED ) );
        grid_set_cell( row, 3, load_string( items[i].enabled ? IDS_IMPACT_NOT_MEASURED : IDS_IMPACT_NOT_MEASURED ) );
        row->icon = items[i].icon;
        row->param = i + 1;
    }
    grid_end( grid );
}

void startup_refresh(void)
{
    if (!grid) return;
    /* the list changes rarely: read again every few seconds, not each tick */
    if (!items || !(ticks++ % 5)) read_items();
    fill();
}

static struct item *selected(void)
{
    struct grid_row *row = grid ? grid_selected( grid ) : NULL;
    if (!row || row->param < 1 || (UINT)row->param > item_count) return NULL;
    return &items[row->param - 1];
}

BOOL startup_can_end(void) { return startup_can_toggle() != 0; }

int startup_can_toggle(void)
{
    struct item *it = selected();
    if (!it) return 0;
    return it->enabled ? 1 : 2;
}

void startup_toggle(void)
{
    struct item *it = selected();
    WCHAR key[128];
    BYTE data[12] = { 0 };
    FILETIME ft;

    if (!it) return;
    it->enabled = !it->enabled;
    data[0] = it->enabled ? 0x02 : 0x03;
    if (!it->enabled)
    {
        GetSystemTimeAsFileTime( &ft );
        memcpy( data + 4, &ft, sizeof(ft) );
    }
    swprintf( key, ARRAY_SIZE(key), L"%s%s", APPROVED_KEY, it->list );
    RegSetKeyValueW( it->root, key, it->name, REG_BINARY, data, sizeof(data) );
    fill();
    update_end_button();
}

static void context_menu(void)
{
    HMENU popup = LoadMenuW( instance, MAKEINTRESOURCEW(IDR_POPUP) ), sub = GetSubMenu( popup, 3 );
    struct item *it = selected();
    POINT pt;
    UINT cmd;

    if (!it) { DestroyMenu( popup ); return; }
    ModifyMenuW( sub, IDM_STARTUP_TOGGLE, MF_BYCOMMAND | MF_STRING, IDM_STARTUP_TOGGLE,
                 load_string( it->enabled ? IDS_DISABLE : IDS_ENABLE ) );
    GetCursorPos( &pt );
    cmd = TrackPopupMenu( sub, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, main_window, NULL );
    DestroyMenu( popup );
    switch (cmd)
    {
    case IDM_STARTUP_TOGGLE: startup_toggle(); break;
    case IDM_OPEN_LOCATION:
        if (it->path[0])
        {
            WCHAR args[MAX_PATH + 16];
            swprintf( args, ARRAY_SIZE(args), L"/select,\"%s\"", it->path );
            ShellExecuteW( main_window, NULL, L"explorer.exe", args, NULL, SW_SHOWNORMAL );
        }
        break;
    case IDM_SEARCH_ONLINE:
    {
        WCHAR url[512];
        swprintf( url, ARRAY_SIZE(url), L"https://www.google.com/search?q=%s", PathFindFileNameW( it->path ) );
        ShellExecuteW( main_window, NULL, url, NULL, NULL, SW_SHOWNORMAL );
        break;
    }
    case IDM_PROPERTIES:
        if (it->path[0])
        {
            SHELLEXECUTEINFOW sei = { sizeof(sei), SEE_MASK_INVOKEIDLIST };
            sei.lpVerb = L"properties";
            sei.lpFile = it->path;
            sei.nShow = SW_SHOW;
            ShellExecuteExW( &sei );
        }
        break;
    }
}

static LRESULT CALLBACK page_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    if (msg == WM_COMMAND && LOWORD(wparam) == IDC_GRID)
    {
        switch (HIWORD(wparam))
        {
        case GN_SELCHANGE: update_end_button(); break;
        case GN_CONTEXT: context_menu(); break;
        }
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

void startup_create( HWND parent )
{
    WNDCLASSW wc = { 0, page_proc, 0, 0, instance };

    CoInitialize( NULL );
    wc.hbrBackground = GetStockObject( WHITE_BRUSH );
    wc.lpszClassName = L"ArcticTaskStartup";
    RegisterClassW( &wc );
    page = CreateWindowExW( 0, wc.lpszClassName, NULL, WS_CHILD | WS_CLIPCHILDREN, 0, 0, 0, 0, parent, NULL, instance, NULL );
    grid = grid_create( page, IDC_GRID, FALSE );
    grid->icons = TRUE;
    grid_add_col( grid, IDS_COL_NAME, 240, FALSE, FALSE );
    grid_add_col( grid, IDS_COL_PUBLISHER, 200, FALSE, FALSE );
    grid_add_col( grid, IDS_COL_STARTUP_STATE, 110, FALSE, FALSE );
    grid_add_col( grid, IDS_COL_STARTUP_IMPACT, 140, FALSE, FALSE );
    grid->sort_col = 0;
    set_page_hwnd( IDS_TAB_STARTUP, page );
}

void startup_resize( int w, int h )
{
    if (grid) SetWindowPos( grid->hwnd, NULL, 0, 0, w, h, SWP_NOZORDER );
}
