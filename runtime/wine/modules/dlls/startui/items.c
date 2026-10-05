/*
 * The Start menu: what it lists
 *
 * As in Windows 7, the left column has the programs pinned to the menu, then
 * the ones used most, and All programs: the shortcuts of the Programs folders
 * of all users and of the user, merged, shortcuts first and folders after
 * them, each alphabetical. Windows keeps its own programs as shortcuts in
 * "Стандартні — Windows"; an Arctic installed before its images had them
 * has none, so that folder is made here from the programs that are there.
 *
 * Pins and how often each program was started from the menu live in
 * HKCU\Software\Arctic\StartMenu (Pinned, Hidden, Usage), as Windows keeps
 * its own in UserAssist.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdlib.h>

#include "startui.h"
#include "shellapi.h"
#include "shlobj.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(startui);

#define REG_KEY        L"Software\\Arctic\\StartMenu"
#define MAX_RESULTS    12
#define MAX_SETTINGS   6

struct builtin
{
    UINT name;
    const WCHAR *path;   /* environment strings expanded */
    const WCHAR *args;
};

static const struct builtin builtins[] =
{
    { IDS_EXPLORER,      L"%SystemRoot%\\explorer.exe" },
    { IDS_NOTEPAD,       L"%SystemRoot%\\system32\\notepad.exe" },
    { IDS_CMD,           L"%SystemRoot%\\system32\\cmd.exe" },
    { IDS_POWERSHELL,    L"%ProgramFiles%\\PowerShell\\7\\pwsh.exe" },
    { IDS_TASKMGR,       L"%SystemRoot%\\system32\\taskmgr.exe" },
    { IDS_SNIPPING,      L"%SystemRoot%\\system32\\snippingtool.exe" },
    { IDS_REGEDIT,       L"%SystemRoot%\\regedit.exe" },
    { IDS_CONTROL_PANEL, L"%SystemRoot%\\system32\\control.exe" },
};

static const struct builtin applets[] =
{
    { IDS_CPL_DISPLAY,  L"desk.cpl" },
    { IDS_CPL_SOUND,    L"mmsys.cpl" },
    { IDS_CPL_NETWORK,  L"ncpa.cpl" },
    { IDS_CPL_PROGRAMS, L"appwiz.cpl" },
    { IDS_CPL_REGION,   L"intl.cpl" },
    { IDS_CPL_MOUSE,    L"main.cpl" },
    { IDS_CPL_KEYBOARD, L"main.cpl", L"@1" },
    { IDS_CPL_DATETIME, L"timedate.cpl" },
    { IDS_CPL_SYSTEM,   L"sysdm.cpl" },
    { IDS_CPL_POWER,    L"powercfg.cpl" },
};

static struct item builtin_items[ARRAY_SIZE(builtins)], applet_items[ARRAY_SIZE(applets)];
static UINT builtin_count, applet_count;
static struct item *root;              /* All programs */
static UINT root_count;
static struct item results_programs, results_settings;
static struct item **view;             /* what the last call returned */
static UINT view_size;

static void free_items( struct item *items, UINT count )
{
    for (UINT i = 0; i < count; i++)
    {
        if (items[i].icon_large) DeleteObject( items[i].icon_large );
        if (items[i].icon_small) DeleteObject( items[i].icon_small );
        if (items[i].children != builtin_items) free_items( items[i].children, items[i].child_count );
    }
    if (items != builtin_items && items != applet_items) free( items );
}

static void make_key( struct item *item )
{
    lstrcpynW( item->key, item->path, ARRAY_SIZE(item->key) );
    CharLowerW( item->key );
}

static int __cdecl compare_items( const void *a, const void *b )
{
    const struct item *x = a, *y = b;

    if (x->kind != y->kind) return x->kind == ITEM_FOLDER ? 1 : -1;
    return CompareStringW( LOCALE_USER_DEFAULT, NORM_IGNORECASE, x->name, -1, y->name, -1 ) - CSTR_EQUAL;
}

/* the shortcuts and folders of one folder, added to a list; a folder already
 * there by name takes this one as its twin */
static void read_folder( const WCHAR *dir, struct item **items, UINT *count, UINT *capacity )
{
    WCHAR pattern[MAX_PATH];
    WIN32_FIND_DATAW data;
    HANDLE find;

    if (!dir[0]) return;
    swprintf( pattern, ARRAY_SIZE(pattern), L"%s\\*", dir );
    if ((find = FindFirstFileW( pattern, &data )) == INVALID_HANDLE_VALUE) return;
    do
    {
        BOOL folder = !!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        struct item *item;
        WCHAR *dot;
        UINT i;

        if (!wcscmp( data.cFileName, L"." ) || !wcscmp( data.cFileName, L".." )) continue;
        if (data.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        if (!wcsicmp( data.cFileName, L"desktop.ini" )) continue;
        if (!folder)
        {
            if (!(dot = wcsrchr( data.cFileName, '.' ))) continue;
            if (wcsicmp( dot, L".lnk" ) && wcsicmp( dot, L".url" ) && wcsicmp( dot, L".exe" )) continue;
        }
        if (folder)
        {
            for (i = 0; i < *count; i++)
            {
                if ((*items)[i].kind == ITEM_FOLDER && !wcsicmp( wcsrchr( (*items)[i].path, '\\' ) + 1, data.cFileName )) break;
            }
            if (i < *count)
            {
                swprintf( (*items)[i].path2, MAX_PATH, L"%s\\%s", dir, data.cFileName );
                continue;
            }
        }
        if (*count == *capacity)
        {
            struct item *grown = realloc( *items, (*capacity = max( 16, *capacity * 2 )) * sizeof(**items) );
            if (!grown) break;
            *items = grown;
        }
        item = &(*items)[(*count)++];
        memset( item, 0, sizeof(*item) );
        item->kind = folder ? ITEM_FOLDER : ITEM_PROGRAM;
        lstrcpynW( item->name, data.cFileName, ARRAY_SIZE(item->name) );
        if (!folder && (dot = wcsrchr( item->name, '.' ))) *dot = 0;
        swprintf( item->path, MAX_PATH, L"%s\\%s", dir, data.cFileName );
        if (folder)
        {
            /* the name desktop.ini gives it (StartUp is Автозавантаження) */
            SHFILEINFOW info;
            if (SHGetFileInfoW( item->path, 0, &info, sizeof(info), SHGFI_DISPLAYNAME ) && info.szDisplayName[0])
                lstrcpynW( item->name, info.szDisplayName, ARRAY_SIZE(item->name) );
        }
        make_key( item );
    } while (FindNextFileW( find, &data ));
    FindClose( find );
}

static void load_children( struct item *folder );

/* as Windows 7, All programs leaves out folders with nothing in them */
static void drop_empty_folders( struct item *items, UINT *count )
{
    UINT kept = 0;

    for (UINT i = 0; i < *count; i++)
    {
        if (items[i].kind == ITEM_FOLDER && !items[i].builtin)
        {
            load_children( &items[i] );
            if (!items[i].child_count)
            {
                free( items[i].children );
                continue;
            }
        }
        if (kept != i) items[kept] = items[i];
        kept++;
    }
    *count = kept;
}

static void load_children( struct item *folder )
{
    UINT capacity = 0;

    if (folder->children || folder->kind != ITEM_FOLDER) return;
    read_folder( folder->path, &folder->children, &folder->child_count, &capacity );
    if (folder->path2[0]) read_folder( folder->path2, &folder->children, &folder->child_count, &capacity );
    for (UINT i = 0; i < folder->child_count; i++) folder->children[i].depth = folder->depth + 1;
    drop_empty_folders( folder->children, &folder->child_count );
    if (folder->children) qsort( folder->children, folder->child_count, sizeof(*folder->children), compare_items );
}

static void load_builtins(void)
{
    WCHAR path[MAX_PATH], system[MAX_PATH];

    for (UINT i = 0; i < builtin_count; i++)
    {
        if (builtin_items[i].icon_large) DeleteObject( builtin_items[i].icon_large );
        if (builtin_items[i].icon_small) DeleteObject( builtin_items[i].icon_small );
    }
    for (UINT i = 0; i < applet_count; i++)
    {
        if (applet_items[i].icon_large) DeleteObject( applet_items[i].icon_large );
        if (applet_items[i].icon_small) DeleteObject( applet_items[i].icon_small );
    }
    builtin_count = applet_count = 0;
    for (UINT i = 0; i < ARRAY_SIZE(builtins); i++)
    {
        struct item *item = &builtin_items[builtin_count];

        if (!ExpandEnvironmentStringsW( builtins[i].path, path, ARRAY_SIZE(path) )) continue;
        if (GetFileAttributesW( path ) == INVALID_FILE_ATTRIBUTES) continue;
        memset( item, 0, sizeof(*item) );
        item->kind = ITEM_PROGRAM;
        item->builtin = TRUE;
        lstrcpynW( item->name, load_string( builtins[i].name ), ARRAY_SIZE(item->name) );
        lstrcpynW( item->path, path, ARRAY_SIZE(item->path) );
        swprintf( item->key, ARRAY_SIZE(item->key), L"builtin:%s", wcsrchr( path, '\\' ) + 1 );
        CharLowerW( item->key );
        item->depth = 1;
        builtin_count++;
    }

    GetSystemDirectoryW( system, ARRAY_SIZE(system) );
    for (UINT i = 0; i < ARRAY_SIZE(applets); i++)
    {
        struct item *item = &applet_items[applet_count];

        swprintf( path, ARRAY_SIZE(path), L"%s\\%s", system, applets[i].path );
        if (GetFileAttributesW( path ) == INVALID_FILE_ATTRIBUTES) continue;
        memset( item, 0, sizeof(*item) );
        item->kind = ITEM_PROGRAM;
        item->builtin = TRUE;
        lstrcpynW( item->name, load_string( applets[i].name ), ARRAY_SIZE(item->name) );
        swprintf( item->path, ARRAY_SIZE(item->path), L"%s\\control.exe", system );
        swprintf( item->args, ARRAY_SIZE(item->args), applets[i].args ? L"%s,%s" : L"%s", applets[i].path,
                  applets[i].args );
        lstrcpynW( item->icon_path, path, ARRAY_SIZE(item->icon_path) );
        swprintf( item->key, ARRAY_SIZE(item->key), L"cpl:%s", item->args );
        CharLowerW( item->key );
        applet_count++;
    }
}

void items_reload(void)
{
    WCHAR common[MAX_PATH] = L"", user[MAX_PATH] = L"";
    UINT capacity = 0;
    struct item *accessories;

    if (root) free_items( root, root_count );
    root = NULL;
    root_count = 0;
    load_builtins();

    SHGetFolderPathW( NULL, CSIDL_COMMON_PROGRAMS, NULL, SHGFP_TYPE_CURRENT, common );
    SHGetFolderPathW( NULL, CSIDL_PROGRAMS, NULL, SHGFP_TYPE_CURRENT, user );
    read_folder( common, &root, &root_count, &capacity );
    if (wcsicmp( common, user )) read_folder( user, &root, &root_count, &capacity );

    /* Windows' own programs, in their folder */
    if (builtin_count)
    {
        if (root_count == capacity)
        {
            struct item *grown = realloc( root, (capacity + 1) * sizeof(*root) );
            if (!grown) goto done;
            root = grown;
        }
        accessories = &root[root_count++];
        memset( accessories, 0, sizeof(*accessories) );
        accessories->kind = ITEM_FOLDER;
        accessories->builtin = TRUE;
        lstrcpynW( accessories->name, load_string( IDS_ACCESSORIES ), ARRAY_SIZE(accessories->name) );
        GetWindowsDirectoryW( accessories->icon_path, ARRAY_SIZE(accessories->icon_path) );
        lstrcpyW( accessories->key, L"builtin:accessories" );
        accessories->children = builtin_items;
        accessories->child_count = builtin_count;
    }
done:
    drop_empty_folders( root, &root_count );
    if (root) qsort( root, root_count, sizeof(*root), compare_items );

    memset( &results_programs, 0, sizeof(results_programs) );
    results_programs.kind = ITEM_HEADER;
    lstrcpynW( results_programs.name, load_string( IDS_RESULTS_PROGRAMS ), ARRAY_SIZE(results_programs.name) );
    memset( &results_settings, 0, sizeof(results_settings) );
    results_settings.kind = ITEM_HEADER;
    lstrcpynW( results_settings.name, load_string( IDS_RESULTS_SETTINGS ), ARRAY_SIZE(results_settings.name) );
}

/**********************************************************************
 *          Lists
 */

static BOOL view_add( UINT *count, struct item *item )
{
    if (*count == view_size)
    {
        struct item **grown = realloc( view, (view_size = max( 32, view_size * 2 )) * sizeof(*view) );
        if (!grown) return FALSE;
        view = grown;
    }
    view[(*count)++] = item;
    return TRUE;
}

static struct item *find_in( struct item *items, UINT count, const WCHAR *key, BOOL deep )
{
    for (UINT i = 0; i < count; i++)
    {
        struct item *found;

        if (items[i].kind == ITEM_PROGRAM && !wcscmp( items[i].key, key )) return &items[i];
        if (deep && items[i].kind == ITEM_FOLDER)
        {
            load_children( &items[i] );
            if ((found = find_in( items[i].children, items[i].child_count, key, TRUE ))) return found;
        }
    }
    return NULL;
}

static struct item *find_item( const WCHAR *key )
{
    struct item *item;

    if ((item = find_in( builtin_items, builtin_count, key, FALSE ))) return item;
    if ((item = find_in( applet_items, applet_count, key, FALSE ))) return item;
    return find_in( root, root_count, key, TRUE );
}

/* a REG_MULTI_SZ of keys, or NULL */
static WCHAR *read_multi( const WCHAR *name, BOOL *present )
{
    DWORD size = 0, type;
    WCHAR *data;
    HKEY key;

    *present = FALSE;
    if (RegOpenKeyExW( HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &key )) return NULL;
    if (RegQueryValueExW( key, name, NULL, &type, NULL, &size ) || type != REG_MULTI_SZ ||
        !(data = calloc( 1, size + 2 * sizeof(WCHAR) )))
    {
        RegCloseKey( key );
        return NULL;
    }
    RegQueryValueExW( key, name, NULL, NULL, (BYTE *)data, &size );
    RegCloseKey( key );
    *present = TRUE;
    return data;
}

static void write_multi( const WCHAR *name, const WCHAR *data, DWORD chars )
{
    HKEY key;

    if (RegCreateKeyExW( HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL )) return;
    RegSetValueExW( key, name, 0, REG_MULTI_SZ, (const BYTE *)data, chars * sizeof(WCHAR) );
    RegCloseKey( key );
}

static BOOL multi_has( const WCHAR *multi, const WCHAR *key )
{
    for (const WCHAR *p = multi; p && *p; p += lstrlenW( p ) + 1)
        if (!wcscmp( p, key )) return TRUE;
    return FALSE;
}

/* the list with one key added or taken out */
static void multi_change( const WCHAR *name, const WCHAR *change, BOOL add )
{
    BOOL present;
    WCHAR *old = read_multi( name, &present ), *new;
    DWORD chars = 1, used = 0;

    for (const WCHAR *p = old; p && *p; p += lstrlenW( p ) + 1) chars += lstrlenW( p ) + 1;
    chars += lstrlenW( change ) + 1;
    if (!(new = calloc( chars, sizeof(WCHAR) )))
    {
        free( old );
        return;
    }
    for (const WCHAR *p = old; p && *p; p += lstrlenW( p ) + 1)
    {
        if (!wcscmp( p, change )) continue;
        lstrcpyW( new + used, p );
        used += lstrlenW( p ) + 1;
    }
    if (add)
    {
        lstrcpyW( new + used, change );
        used += lstrlenW( change ) + 1;
    }
    new[used++] = 0;
    write_multi( name, new, used );
    free( new );
    free( old );
}

/* what is pinned before anybody pins anything: the file manager, the
 * browser, and Windows' tools the reference menu shows */
static WCHAR *default_pins(void)
{
    const WCHAR *keys[] = { L"builtin:explorer.exe", NULL, L"builtin:notepad.exe", L"builtin:pwsh.exe",
                                   L"builtin:snippingtool.exe" };
    WCHAR chrome[MAX_PATH] = L"", *data, *p;
    DWORD chars = 1;

    if (!SHGetFolderPathW( NULL, CSIDL_COMMON_PROGRAMS, NULL, SHGFP_TYPE_CURRENT, chrome ))
    {
        lstrcatW( chrome, L"\\Google Chrome.lnk" );
        CharLowerW( chrome );
        if (GetFileAttributesW( chrome ) != INVALID_FILE_ATTRIBUTES) keys[1] = chrome;
    }
    if (!find_item( L"builtin:pwsh.exe" )) keys[3] = L"builtin:cmd.exe";
    for (UINT i = 0; i < ARRAY_SIZE(keys); i++) if (keys[i]) chars += lstrlenW( keys[i] ) + 1;
    if (!(p = data = calloc( chars, sizeof(WCHAR) ))) return NULL;
    for (UINT i = 0; i < ARRAY_SIZE(keys); i++)
    {
        if (!keys[i]) continue;
        lstrcpyW( p, keys[i] );
        p += lstrlenW( p ) + 1;
    }
    *p = 0;
    return data;
}

UINT items_pinned( struct item ***list )
{
    BOOL present;
    WCHAR *pins = read_multi( L"Pinned", &present );
    UINT count = 0;

    if (!present) pins = default_pins();
    for (const WCHAR *p = pins; p && *p; p += lstrlenW( p ) + 1)
    {
        struct item *item = find_item( p );
        if (item) view_add( &count, item );
    }
    free( pins );
    *list = view;
    return count;
}

BOOL items_is_pinned( const struct item *item )
{
    BOOL present, pinned;
    WCHAR *pins = read_multi( L"Pinned", &present );

    if (!present) pins = default_pins();
    pinned = multi_has( pins, item->key );
    free( pins );
    return pinned;
}

void items_pin( struct item *item, BOOL pin )
{
    BOOL present;
    WCHAR *pins = read_multi( L"Pinned", &present );

    /* the defaults become the list the first time it changes */
    if (!present && (pins = default_pins()))
    {
        DWORD chars = 1;
        for (const WCHAR *p = pins; *p; p += lstrlenW( p ) + 1) chars += lstrlenW( p ) + 1;
        write_multi( L"Pinned", pins, chars );
    }
    free( pins );
    multi_change( L"Pinned", item->key, pin );
}

/* taken out of the most used, until it is started again */
void items_forget( struct item *item )
{
    multi_change( L"Hidden", item->key, TRUE );
}

struct usage
{
    WCHAR key[MAX_PATH];
    DWORD count;
};

static int __cdecl compare_usage( const void *a, const void *b )
{
    const struct usage *x = a, *y = b;
    return x->count < y->count ? 1 : x->count > y->count ? -1 : 0;
}

UINT items_frequent( struct item ***list, UINT max )
{
    struct usage *usage = NULL;
    UINT used = 0, count = 0, capacity = 0;
    WCHAR *pins, *hidden;
    BOOL present;
    HKEY key;

    pins = read_multi( L"Pinned", &present );
    if (!present) pins = default_pins();
    hidden = read_multi( L"Hidden", &present );
    if (!RegOpenKeyExW( HKEY_CURRENT_USER, REG_KEY L"\\Usage", 0, KEY_READ, &key ))
    {
        for (DWORD i = 0;; i++)
        {
            WCHAR name[MAX_PATH];
            DWORD name_len = ARRAY_SIZE(name), value, size = sizeof(value), type;

            if (RegEnumValueW( key, i, name, &name_len, NULL, &type, (BYTE *)&value, &size )) break;
            if (type != REG_DWORD || multi_has( pins, name ) || multi_has( hidden, name )) continue;
            if (used == capacity)
            {
                struct usage *grown = realloc( usage, (capacity = max( 16, capacity * 2 )) * sizeof(*usage) );
                if (!grown) break;
                usage = grown;
            }
            lstrcpynW( usage[used].key, name, MAX_PATH );
            usage[used++].count = value;
        }
        RegCloseKey( key );
    }
    if (usage) qsort( usage, used, sizeof(*usage), compare_usage );
    for (UINT i = 0; i < used && count < max; i++)
    {
        struct item *item = find_item( usage[i].key );
        if (item) view_add( &count, item );
    }
    free( usage );
    free( pins );
    free( hidden );
    *list = view;
    return count;
}

static void add_tree( UINT *count, struct item *items, UINT n )
{
    for (UINT i = 0; i < n; i++)
    {
        view_add( count, &items[i] );
        if (items[i].kind == ITEM_FOLDER && items[i].expanded)
        {
            load_children( &items[i] );
            add_tree( count, items[i].children, items[i].child_count );
        }
    }
}

UINT items_all_programs( struct item ***list )
{
    UINT count = 0;

    add_tree( &count, root, root_count );
    *list = view;
    return count;
}

static BOOL matches( const WCHAR *name, const WCHAR *text )
{
    WCHAR a[MAX_PATH], b[MAX_PATH];

    lstrcpynW( a, name, ARRAY_SIZE(a) );
    lstrcpynW( b, text, ARRAY_SIZE(b) );
    CharLowerW( a );
    CharLowerW( b );
    return wcsstr( a, b ) != NULL;
}

static void search_tree( UINT *count, UINT *found, struct item *items, UINT n, const WCHAR *text )
{
    for (UINT i = 0; i < n && *found < MAX_RESULTS; i++)
    {
        if (items[i].kind == ITEM_FOLDER)
        {
            load_children( &items[i] );
            search_tree( count, found, items[i].children, items[i].child_count, text );
        }
        else if (matches( items[i].name, text ))
        {
            /* once, though a program is also among Windows' own */
            BOOL seen = FALSE;
            for (UINT j = *count - *found; j < *count; j++)
                if (!wcsicmp( view[j]->name, items[i].name )) seen = TRUE;
            if (!seen && view_add( count, &items[i] )) (*found)++;
        }
    }
}

UINT items_search( const WCHAR *text, struct item ***list )
{
    UINT count = 0, found = 0, header;

    header = count;
    view_add( &count, &results_programs );
    search_tree( &count, &found, root, root_count, text );
    if (!found) count = header;

    found = 0;
    header = count;
    view_add( &count, &results_settings );
    for (UINT i = 0; i < applet_count && found < MAX_SETTINGS; i++)
        if (matches( applet_items[i].name, text ) && view_add( &count, &applet_items[i] )) found++;
    if (!found) count = header;

    *list = view;
    return count;
}

/**********************************************************************
 *          Acting
 */

static void count_use( struct item *item )
{
    DWORD value = 0, size = sizeof(value);
    HKEY key;

    if (RegCreateKeyExW( HKEY_CURRENT_USER, REG_KEY L"\\Usage", 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &key, NULL ))
        return;
    RegQueryValueExW( key, item->key, NULL, NULL, (BYTE *)&value, &size );
    value++;
    RegSetValueExW( key, item->key, 0, REG_DWORD, (const BYTE *)&value, sizeof(value) );
    RegCloseKey( key );
    multi_change( L"Hidden", item->key, FALSE );
}

void items_launch( struct item *item )
{
    SHELLEXECUTEINFOW info = { sizeof(info) };
    WCHAR profile[MAX_PATH] = L"";

    if (item->kind != ITEM_PROGRAM) return;
    SHGetFolderPathW( NULL, CSIDL_PROFILE, NULL, SHGFP_TYPE_CURRENT, profile );
    info.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    info.lpFile = item->path;
    info.lpParameters = item->args[0] ? item->args : NULL;
    /* Windows' own programs start in the user's folder, as from Run */
    info.lpDirectory = item->builtin && profile[0] ? profile : NULL;
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW( &info ))
    {
        WARN( "could not start %s: %lu\n", debugstr_w(item->path), GetLastError() );
        return;
    }
    count_use( item );
}

void items_open_location( struct item *item )
{
    WCHAR args[MAX_PATH + 16];

    swprintf( args, ARRAY_SIZE(args), L"/select,\"%s\"", item->path );
    ShellExecuteW( NULL, NULL, L"explorer.exe", args, NULL, SW_SHOWNORMAL );
}

HBITMAP items_icon( struct item *item, BOOL large )
{
    HBITMAP *cache = large ? &item->icon_large : &item->icon_small;
    int size = large ? px( 32 ) : px( 16 );
    HICON icon = NULL;

    if (*cache || item->kind == ITEM_HEADER) return *cache;
    if (item->icon_path[0] && item->kind == ITEM_PROGRAM)
    {
        HICON big = NULL, little = NULL;
        ExtractIconExW( item->icon_path, 0, &big, &little, 1 );
        icon = large ? big : little;
        if (icon == big && little) DestroyIcon( little );
        if (icon == little && big) DestroyIcon( big );
    }
    if (!icon)
    {
        SHFILEINFOW info;
        const WCHAR *path = item->icon_path[0] ? item->icon_path : item->path;

        if (SHGetFileInfoW( path, 0, &info, sizeof(info), SHGFI_ICON | (large ? SHGFI_LARGEICON : SHGFI_SMALLICON) ))
            icon = info.hIcon;
    }
    if (!icon) return NULL;
    *cache = icon_bitmap( icon, size );
    DestroyIcon( icon );
    return *cache;
}

/* the settings of the Control Panel, for its menu in the right column */
UINT items_applets( struct item ***list )
{
    UINT count = 0;

    for (UINT i = 0; i < applet_count; i++) view_add( &count, &applet_items[i] );
    *list = view;
    return count;
}
