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

#define COBJMACROS

#include <stdlib.h>

#include "startui.h"
#include "shellapi.h"
#include "shlobj.h"
#include "shlguid.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(startui);

#define REG_KEY        L"Software\\Arctic\\StartMenu"
#define MAX_RESULTS    12
#define MAX_SETTINGS   6
#define MAX_FILES      6

struct builtin
{
    UINT name;
    const WCHAR *path;   /* environment strings expanded */
    const WCHAR *args;
    const WCHAR *aliases;  /* as Windows' search also knows them, in English */
};

static const struct builtin builtins[] =
{
    { IDS_EXPLORER,      L"%SystemRoot%\\explorer.exe", NULL, L"file explorer files folders" },
    { IDS_NOTEPAD,       L"%SystemRoot%\\system32\\notepad.exe", NULL, L"notepad text editor" },
    { IDS_CMD,           L"%SystemRoot%\\system32\\cmd.exe", NULL, L"command prompt cmd console terminal" },
    { IDS_POWERSHELL,    L"%ProgramFiles%\\PowerShell\\7\\pwsh.exe", NULL, L"powershell pwsh terminal" },
    { IDS_TASKMGR,       L"%SystemRoot%\\system32\\taskmgr.exe", NULL, L"task manager taskmgr processes" },
    { IDS_SNIPPING,      L"%SystemRoot%\\system32\\snippingtool.exe", NULL, L"snipping tool screenshot" },
    { IDS_REGEDIT,       L"%SystemRoot%\\regedit.exe", NULL, L"registry editor regedit" },
    { IDS_CONTROL_PANEL, L"%SystemRoot%\\system32\\control.exe", NULL, L"control panel settings" },
};

static const struct builtin applets[] =
{
    { IDS_CPL_DISPLAY,  L"desk.cpl", NULL, L"display screen resolution scale monitor" },
    { IDS_CPL_SOUND,    L"mmsys.cpl", NULL, L"sound audio speakers microphone" },
    { IDS_CPL_NETWORK,  L"ncpa.cpl", NULL, L"network connections ethernet wi-fi wifi adapter" },
    { IDS_CPL_PROGRAMS, L"appwiz.cpl", NULL, L"programs and features uninstall apps" },
    { IDS_CPL_REGION,   L"intl.cpl", NULL, L"region language format" },
    { IDS_CPL_MOUSE,    L"main.cpl", NULL, L"mouse pointer cursor" },
    { IDS_CPL_KEYBOARD, L"main.cpl", L"@1", L"keyboard" },
    { IDS_CPL_DATETIME, L"timedate.cpl", NULL, L"date and time clock" },
    { IDS_CPL_SYSTEM,   L"sysdm.cpl", NULL, L"system about computer name" },
    { IDS_CPL_POWER,    L"powercfg.cpl", NULL, L"power options battery sleep" },
};

static struct item builtin_items[ARRAY_SIZE(builtins)], applet_items[ARRAY_SIZE(applets)];
static UINT builtin_count, applet_count;
static struct item external_items[32];     /* pinned files of no Programs folder */
static UINT external_count;
static struct item *root;              /* All programs */
static UINT root_count;
static struct item results_programs, results_settings, results_files;
static struct item file_items[MAX_FILES];   /* the files a search found */
static struct item **view;             /* what the last call returned */
static UINT view_size;

static void free_items( struct item *items, UINT count )
{
    for (UINT i = 0; i < count; i++)
    {
        if (items[i].icon_large) DeleteObject( items[i].icon_large );
        if (items[i].icon_small) DeleteObject( items[i].icon_small );
        if (items[i].icon_other) DeleteObject( items[i].icon_other );
        if (items[i].children != builtin_items) free_items( items[i].children, items[i].child_count );
    }
    if (items != builtin_items && items != applet_items && items != external_items) free( items );
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
        item->added = data.ftCreationTime;
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
        if (builtin_items[i].icon_other) DeleteObject( builtin_items[i].icon_other );
    }
    for (UINT i = 0; i < applet_count; i++)
    {
        if (applet_items[i].icon_large) DeleteObject( applet_items[i].icon_large );
        if (applet_items[i].icon_small) DeleteObject( applet_items[i].icon_small );
        if (applet_items[i].icon_other) DeleteObject( applet_items[i].icon_other );
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
        lstrcpynW( item->aliases, builtins[i].aliases, ARRAY_SIZE(item->aliases) );
        item->aliased = TRUE;
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
        lstrcpynW( item->aliases, applets[i].aliases, ARRAY_SIZE(item->aliases) );
        item->aliased = TRUE;
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
    free_items( external_items, external_count );
    external_count = 0;
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
    memset( &results_files, 0, sizeof(results_files) );
    results_files.kind = ITEM_HEADER;
    lstrcpynW( results_files.name, load_string( IDS_RESULTS_FILES ), ARRAY_SIZE(results_files.name) );
    items_sync_classic();
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

/* a pinned file of no Programs folder: a shortcut of User Pinned\StartMenu,
 * or one in the Start Menu folder itself (the classic menu's top) */
static struct item *external_item( const WCHAR *key )
{
    WIN32_FIND_DATAW data;
    struct item *item;
    HANDLE find;
    WCHAR *slash;

    for (UINT i = 0; i < external_count; i++)
        if (!wcscmp( external_items[i].key, key )) return &external_items[i];
    if (external_count == ARRAY_SIZE(external_items) || !key[0] || key[1] != ':' || key[2] != '\\') return NULL;
    if ((find = FindFirstFileW( key, &data )) == INVALID_HANDLE_VALUE) return NULL;
    FindClose( find );
    item = &external_items[external_count++];
    memset( item, 0, sizeof(*item) );
    item->kind = ITEM_PROGRAM;
    lstrcpynW( item->key, key, ARRAY_SIZE(item->key) );
    /* the folder as the key has it, the file's name as it is written */
    lstrcpynW( item->path, key, ARRAY_SIZE(item->path) );
    if ((slash = wcsrchr( item->path, '\\' ))) lstrcpynW( slash + 1, data.cFileName, MAX_PATH - (slash + 1 - item->path) );
    lstrcpynW( item->name, data.cFileName, ARRAY_SIZE(item->name) );
    if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcsrchr( item->name, '.' )) *wcsrchr( item->name, '.' ) = 0;
    item->added = data.ftCreationTime;
    return item;
}

static struct item *find_item( const WCHAR *key )
{
    struct item *item;

    if ((item = find_in( builtin_items, builtin_count, key, FALSE ))) return item;
    if ((item = find_in( applet_items, applet_count, key, FALSE ))) return item;
    if ((item = find_in( root, root_count, key, TRUE ))) return item;
    return external_item( key );
}

/* what an item starts: its shortcut's program, or its own file */
static BOOL item_program( struct item *item, WCHAR *program, int size )
{
    program[0] = 0;
    if (item->kind != ITEM_PROGRAM) return FALSE;
    if (!wcsicmp( path_extension( item->path ), L".lnk" )) return link_target( item->path, program, size );
    if (item->args[0]) return FALSE;    /* control.exe of an applet is not the applet */
    lstrcpynW( program, item->path, size );
    return TRUE;
}

static struct item *find_program_in( struct item *items, UINT count, const WCHAR *program )
{
    WCHAR target[MAX_PATH];

    for (UINT i = 0; i < count; i++)
    {
        struct item *found;

        if (items[i].kind == ITEM_FOLDER)
        {
            load_children( &items[i] );
            if ((found = find_program_in( items[i].children, items[i].child_count, program ))) return found;
        }
        else if (item_program( &items[i], target, ARRAY_SIZE(target) ) && !wcsicmp( target, program )) return &items[i];
    }
    return NULL;
}

/* the menu's program that starts this one, a shortcut of All programs first */
static struct item *find_program( const WCHAR *program )
{
    struct item *item;

    if (!program[0]) return NULL;
    if (!root) items_reload();
    if ((item = find_program_in( root, root_count, program ))) return item;
    return find_program_in( builtin_items, builtin_count, program );
}

/* the shortcut All programs has of a program: what the taskbar copies */
BOOL items_shortcut_of( const WCHAR *program, WCHAR *link )
{
    struct item *item;

    if (!root) items_reload();
    if (!(item = find_program_in( root, root_count, program ))) return FALSE;
    if (wcsicmp( path_extension( item->path ), L".lnk" )) return FALSE;
    lstrcpynW( link, item->path, MAX_PATH );
    return TRUE;
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

/* a shortcut of the common All programs, by its key, if the image has it */
static const WCHAR *common_program( WCHAR *key, const WCHAR *name )
{
    if (SHGetFolderPathW( NULL, CSIDL_COMMON_PROGRAMS, NULL, SHGFP_TYPE_CURRENT, key )) return NULL;
    lstrcatW( key, L"\\" );
    lstrcatW( key, name );
    lstrcatW( key, L".lnk" );
    CharLowerW( key );
    return GetFileAttributesW( key ) != INVALID_FILE_ATTRIBUTES ? key : NULL;
}

/* what is pinned before anybody pins anything: the programs of the
 * taskbar (File Explorer, the browser, the Task Manager, Arctic WordPad),
 * Paint and Photos, then Windows' tools the reference menu shows */
static WCHAR *default_pins(void)
{
    WCHAR chrome[MAX_PATH], wordpad[MAX_PATH], paint[MAX_PATH], photos[MAX_PATH], *data, *p;
    const WCHAR *keys[] = { L"builtin:explorer.exe", common_program( chrome, L"Google Chrome" ),
                            L"builtin:taskmgr.exe", common_program( wordpad, L"Arctic WordPad" ),
                            common_program( paint, L"Paint" ), common_program( photos, L"Фотографії" ),
                            L"builtin:notepad.exe", L"builtin:pwsh.exe", L"builtin:snippingtool.exe" };
    DWORD chars = 1;

    if (!find_item( L"builtin:pwsh.exe" )) keys[7] = L"builtin:cmd.exe";
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

/* Windows 11's menu keeps pins in folders: "folder:Name|key|key" among the
 * keys. The other menus have no folders and show what is in them in their
 * place. */
#define FOLDER_PREFIX L"folder:"

BOOL items_pin_is_folder( const WCHAR *entry )
{
    return !wcsncmp( entry, FOLDER_PREFIX, ARRAY_SIZE(FOLDER_PREFIX) - 1 );
}

/* the list as kept, or the defaults when nothing was ever pinned */
static WCHAR *pins_raw(void)
{
    BOOL present;
    WCHAR *pins = read_multi( L"Pinned", &present );

    if (!present) pins = default_pins();
    return pins;
}

static DWORD multi_chars( const WCHAR *multi )
{
    DWORD chars = 1;

    for (const WCHAR *p = multi; p && *p; p += lstrlenW( p ) + 1) chars += lstrlenW( p ) + 1;
    return chars;
}

/* the keys, those in folders where the folder is */
static WCHAR *pins_flat(void)
{
    WCHAR *raw = pins_raw(), *flat, *out;

    if (!(out = flat = calloc( multi_chars( raw ) + 1, sizeof(WCHAR) )))
    {
        free( raw );
        return NULL;
    }
    for (const WCHAR *p = raw; p && *p; p += lstrlenW( p ) + 1)
    {
        if (!items_pin_is_folder( p ))
        {
            lstrcpyW( out, p );
            out += lstrlenW( p ) + 1;
            continue;
        }
        for (const WCHAR *k = wcschr( p, '|' ); k; k = wcschr( k + 1, '|' ))
        {
            const WCHAR *next = wcschr( k + 1, '|' );
            int n = next ? next - (k + 1) : lstrlenW( k + 1 );

            if (!n) continue;
            memcpy( out, k + 1, n * sizeof(WCHAR) );
            out += n;
            *out++ = 0;
        }
    }
    *out = 0;
    free( raw );
    return flat;
}

/* a key unpinned: from the list and from the folders; a folder left with one
 * pin is that pin again, as in Windows 11 */
static void pins_remove( const WCHAR *key )
{
    WCHAR *raw = pins_raw(), *new, *out, *entry;
    DWORD chars = multi_chars( raw );

    if (!(out = new = calloc( chars + 1, sizeof(WCHAR) )) || !(entry = malloc( chars * sizeof(WCHAR) )))
    {
        free( new );
        free( raw );
        return;
    }
    for (const WCHAR *p = raw; p && *p; p += lstrlenW( p ) + 1)
    {
        const WCHAR *bar = wcschr( p, '|' ), *last = NULL;
        int kept = 0, len;

        if (!items_pin_is_folder( p ) || !bar)
        {
            if (!wcscmp( p, key )) continue;
            lstrcpyW( out, p );
            out += lstrlenW( p ) + 1;
            continue;
        }
        len = bar - p;
        memcpy( entry, p, len * sizeof(WCHAR) );
        for (const WCHAR *k = bar; k; k = wcschr( k + 1, '|' ))
        {
            const WCHAR *next = wcschr( k + 1, '|' );
            int n = next ? next - (k + 1) : lstrlenW( k + 1 );

            if (!n || ((int)lstrlenW( key ) == n && !wcsncmp( k + 1, key, n ))) continue;
            entry[len++] = '|';
            memcpy( entry + len, k + 1, n * sizeof(WCHAR) );
            last = entry + len;
            len += n;
            kept++;
        }
        entry[len] = 0;
        if (kept >= 2) lstrcpyW( out, entry );
        else if (kept == 1) lstrcpyW( out, last );
        else continue;
        out += lstrlenW( out ) + 1;
    }
    *out++ = 0;
    write_multi( L"Pinned", new, out - new );
    free( entry );
    free( new );
    free( raw );
}

/* for Windows 11's menu: the list as kept, folders and all; written back
 * whole */
WCHAR *items_pin_entries(void)
{
    if (!root) items_reload();
    return pins_raw();
}

void items_pin_entries_save( const WCHAR *multi )
{
    write_multi( L"Pinned", multi, multi_chars( multi ) );
    items_sync_classic();
}

struct item *items_find( const WCHAR *key )
{
    if (!root) items_reload();
    return find_item( key );
}

UINT items_pinned( struct item ***list )
{
    WCHAR *pins = pins_flat();
    UINT count = 0;

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
    WCHAR *pins = pins_flat();
    BOOL pinned;

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
    if (pin) multi_change( L"Pinned", item->key, TRUE );
    else pins_remove( item->key );
    items_sync_classic();
}

/* the list of pins written out, so that it can change */
static void pins_materialize(void)
{
    BOOL present;
    WCHAR *pins = read_multi( L"Pinned", &present );

    if (!present && (pins = default_pins()))
    {
        DWORD chars = 1;
        for (const WCHAR *p = pins; *p; p += lstrlenW( p ) + 1) chars += lstrlenW( p ) + 1;
        write_multi( L"Pinned", pins, chars );
    }
    free( pins );
}

/* a file dropped on a Start menu or the Start button: the menu's program
 * when it is one, by its shortcut or by what that starts; else a shortcut of
 * its own in User Pinned\StartMenu, which stays when the original goes */
BOOL items_pin_file( const WCHAR *path )
{
    WCHAR key[MAX_PATH], program[MAX_PATH] = L"", folder[MAX_PATH], dest[MAX_PATH], name[MAX_PATH], *dot;
    const WCHAR *ext = path_extension( path );
    struct item *item;
    BOOL ok;

    if (!root) items_reload();
    lstrcpynW( key, path, ARRAY_SIZE(key) );
    CharLowerW( key );
    if (!(item = find_in( root, root_count, key, TRUE )))
    {
        if (!wcsicmp( ext, L".lnk" )) link_target( path, program, ARRAY_SIZE(program) );
        else if (!wcsicmp( ext, L".exe" )) lstrcpynW( program, path, ARRAY_SIZE(program) );
        item = find_program( program );
    }
    if (!item)
    {
        if (!pinned_folder( L"StartMenu", folder )) return FALSE;
        if (!wcsicmp( ext, L".exe" )) program_name( path, name, ARRAY_SIZE(name) );
        else
        {
            lstrcpynW( name, wcsrchr( path, '\\' ) ? wcsrchr( path, '\\' ) + 1 : path, ARRAY_SIZE(name) );
            if ((dot = wcsrchr( name, '.' )) && !(GetFileAttributesW( path ) & FILE_ATTRIBUTE_DIRECTORY)) *dot = 0;
        }
        if (!wcsicmp( ext, L".lnk" ) || !wcsicmp( ext, L".url" ))
        {
            free_name( folder, name, ext, dest );
            ok = CopyFileW( path, dest, TRUE );
        }
        else
        {
            /* a program, a folder: a shortcut to it */
            free_name( folder, name, L".lnk", dest );
            ok = link_make( dest, path, NULL, NULL );
        }
        if (!ok) return FALSE;
        CharLowerW( dest );
        if (!(item = external_item( dest ))) return FALSE;
    }
    if (!items_is_pinned( item )) items_pin( item, TRUE );
    return TRUE;
}

/* what a file stands for in the menu: what its shortcut starts, or itself */
static void file_meaning( const WCHAR *path, WCHAR *out, int size )
{
    WCHAR expanded[MAX_PATH];

    if (!ExpandEnvironmentStringsW( path, expanded, ARRAY_SIZE(expanded) )) lstrcpynW( expanded, path, ARRAY_SIZE(expanded) );
    if (!wcsicmp( path_extension( expanded ), L".lnk" ) && link_target( expanded, out, size )) return;
    lstrcpynW( out, expanded, size );
}

/* the pin a file stands for: the file itself, a pin of the same program or
 * folder, or the copy of it pinned from elsewhere */
static struct item *pinned_item_of_file( const WCHAR *path )
{
    WCHAR want[MAX_PATH], have[MAX_PATH];
    struct item *found = NULL;
    WCHAR *pins;

    if (!root) items_reload();
    file_meaning( path, want, ARRAY_SIZE(want) );
    pins = pins_flat();
    for (const WCHAR *p = pins; p && *p && !found; p += lstrlenW( p ) + 1)
    {
        struct item *item = find_item( p );

        if (!item || item->kind != ITEM_PROGRAM) continue;
        if (!wcsicmp( item->path, path )) found = item;
        else if (!item->args[0])
        {
            file_meaning( item->path, have, ARRAY_SIZE(have) );
            if (!wcsicmp( have, want )) found = item;
        }
    }
    free( pins );
    return found;
}

BOOL items_file_pinned( const WCHAR *path )
{
    return pinned_item_of_file( path ) != NULL;
}

BOOL items_unpin_file( const WCHAR *path )
{
    struct item *item = pinned_item_of_file( path );

    if (!item) return FALSE;
    items_pin( item, FALSE );
    return TRUE;
}

/* the taskbar's shortcut of an item's program, if it is pinned there */
BOOL items_taskbar_pinned( struct item *item, WCHAR *link )
{
    WCHAR program[MAX_PATH];

    return item_program( item, program, ARRAY_SIZE(program) ) && taskbar_pin_of( program, link );
}

void items_pin_taskbar( struct item *item, BOOL pin )
{
    WCHAR link[MAX_PATH];

    if (!pin)
    {
        if (items_taskbar_pinned( item, link )) ArcticTaskbarUnpinFile( link );
        return;
    }
    taskbar_pin_named( item->path, wcsicmp( path_extension( item->path ), L".lnk" ) ? item->name : NULL );
}

/* The classic menu (ReactOS's) shows what is in the Start Menu folder at its
 * top, as Windows 2000 did: the same pins are kept there as shortcuts, both
 * ways. ClassicCopies lists them, "file name|key" each: one gone from there
 * was taken off the classic menu, one found there and not listed was put on
 * it, a pin with none gets one. */
#define CLASSIC_COPIES L"ClassicCopies"

static BOOL copy_listed( const WCHAR *copies, const WCHAR *name, const WCHAR *key )
{
    for (const WCHAR *p = copies; p && *p; p += lstrlenW( p ) + 1)
    {
        const WCHAR *bar = wcschr( p, '|' );

        if (!bar) continue;
        if (name && (int)(bar - p) == lstrlenW( name ) && !wcsnicmp( p, name, bar - p )) return TRUE;
        if (key && !wcscmp( bar + 1, key )) return TRUE;
    }
    return FALSE;
}

void items_sync_classic(void)
{
    static BOOL busy;
    WCHAR menu[MAX_PATH], path[MAX_PATH], entry[2 * MAX_PATH + 2], pattern[MAX_PATH];
    WCHAR *copies, *pins, *kept;
    DWORD kept_size = 4096, used = 0;
    WIN32_FIND_DATAW data;
    HANDLE find;
    BOOL present;

    if (busy || FAILED(SHGetFolderPathW( NULL, CSIDL_STARTMENU, NULL, SHGFP_TYPE_CURRENT, menu ))) return;
    if (!(kept = calloc( kept_size, sizeof(WCHAR) ))) return;
    busy = TRUE;
    copies = read_multi( CLASSIC_COPIES, &present );
    pins = pins_flat();

#define KEEP( name, key ) \
    do { \
        swprintf( entry, ARRAY_SIZE(entry), L"%s|%s", (name), (key) ); \
        if (used + lstrlenW( entry ) + 2 < kept_size) { lstrcpyW( kept + used, entry ); used += lstrlenW( entry ) + 1; } \
    } while (0)

    /* the copies made before */
    for (const WCHAR *p = copies; p && *p; p += lstrlenW( p ) + 1)
    {
        const WCHAR *bar = wcschr( p, '|' );
        WCHAR name[MAX_PATH];

        if (!bar || bar - p >= MAX_PATH) continue;
        lstrcpynW( name, p, bar - p + 1 );
        swprintf( path, ARRAY_SIZE(path), L"%s\\%s", menu, name );
        if (GetFileAttributesW( path ) == INVALID_FILE_ATTRIBUTES)
        {
            /* deleted from the classic menu: unpinned */
            if (multi_has( pins, bar + 1 ))
            {
                pins_materialize();
                pins_remove( bar + 1 );
            }
            continue;
        }
        if (!multi_has( pins, bar + 1 ))
        {
            /* unpinned from another menu */
            DeleteFileW( path );
            continue;
        }
        KEEP( name, bar + 1 );
    }

    /* put on the classic menu by hand: pinned */
    swprintf( pattern, ARRAY_SIZE(pattern), L"%s\\*", menu );
    if ((find = FindFirstFileW( pattern, &data )) != INVALID_HANDLE_VALUE)
    {
        do
        {
            const WCHAR *ext = path_extension( data.cFileName );
            WCHAR key[MAX_PATH];

            if (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
            if (wcsicmp( ext, L".lnk" ) && wcsicmp( ext, L".url" ) && wcsicmp( ext, L".exe" )) continue;
            if (copy_listed( kept, data.cFileName, NULL )) continue;
            swprintf( key, ARRAY_SIZE(key), L"%s\\%s", menu, data.cFileName );
            CharLowerW( key );
            pins_materialize();
            multi_change( L"Pinned", key, TRUE );
            KEEP( data.cFileName, key );
        } while (FindNextFileW( find, &data ));
        FindClose( find );
    }

    /* the pins with no copy get one */
    free( pins );
    pins = pins_flat();
    for (const WCHAR *p = pins; p && *p; p += lstrlenW( p ) + 1)
    {
        struct item *item;
        const WCHAR *ext;
        BOOL ok;

        if (copy_listed( kept, NULL, p ) || !(item = find_item( p ))) continue;
        ext = path_extension( item->path );
        if (!wcsicmp( ext, L".lnk" ) || !wcsicmp( ext, L".url" ))
        {
            free_name( menu, item->name, ext, path );
            ok = CopyFileW( item->path, path, TRUE );
        }
        else
        {
            free_name( menu, item->name, L".lnk", path );
            ok = link_make( path, item->path, item->args, item->icon_path );
        }
        if (ok) KEEP( wcsrchr( path, '\\' ) + 1, p );
    }
#undef KEEP

    kept[used++] = 0;
    write_multi( CLASSIC_COPIES, kept, used );
    free( kept );
    free( copies );
    free( pins );
    busy = FALSE;
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

/* the file name of a program, without its extension: notepad of notepad.exe */
static void add_file_name( WCHAR *aliases, int size, const WCHAR *path )
{
    const WCHAR *name = wcsrchr( path, '\\' ) ? wcsrchr( path, '\\' ) + 1 : path;
    WCHAR base[MAX_PATH], *dot;
    int len = lstrlenW( aliases );

    lstrcpynW( base, name, ARRAY_SIZE(base) );
    if ((dot = wcsrchr( base, '.' ))) *dot = 0;
    if (!base[0] || len + lstrlenW( base ) + 2 >= size) return;
    if (len) aliases[len++] = ' ';
    lstrcpynW( aliases + len, base, size - len );
}

/* a shortcut is found by the program it starts too, as in Windows: what
 * Chrome's shortcut points at is chrome.exe */
static void look_up_aliases( struct item *item )
{
    IShellLinkW *link;
    IPersistFile *file;
    WCHAR target[MAX_PATH];
    const WCHAR *dot;

    if (item->aliased) return;
    item->aliased = TRUE;
    dot = wcsrchr( item->path, '.' );
    if (!dot || wcsicmp( dot, L".lnk" ))
    {
        add_file_name( item->aliases, ARRAY_SIZE(item->aliases), item->path );
        return;
    }
    if (FAILED(CoCreateInstance( &CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link )))
        return;
    if (SUCCEEDED(IShellLinkW_QueryInterface( link, &IID_IPersistFile, (void **)&file )))
    {
        if (SUCCEEDED(IPersistFile_Load( file, item->path, STGM_READ )) &&
            IShellLinkW_GetPath( link, target, ARRAY_SIZE(target), NULL, SLGP_RAWPATH ) == S_OK)
            add_file_name( item->aliases, ARRAY_SIZE(item->aliases), target );
        IPersistFile_Release( file );
    }
    IShellLinkW_Release( link );
}

/* by its name, an English name Windows knows it by, or its program's file */
static BOOL item_matches( struct item *item, const WCHAR *text )
{
    if (matches( item->name, text )) return TRUE;
    look_up_aliases( item );
    return item->aliases[0] && matches( item->aliases, text );
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
        else if (item_matches( &items[i], text ))
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
        if (item_matches( &applet_items[i], text ) && view_add( &count, &applet_items[i] )) found++;
    if (!found) count = header;

    /* the files opened lately whose names match */
    {
        struct recent_file files[32];
        UINT n = items_recent_files( files, ARRAY_SIZE(files) );

        found = 0;
        header = count;
        view_add( &count, &results_files );
        for (UINT i = 0; i < n && found < MAX_FILES; i++)
        {
            struct item *item = &file_items[found];

            if (!matches( files[i].name, text )) continue;
            if (item->icon_large) DeleteObject( item->icon_large );
            if (item->icon_small) DeleteObject( item->icon_small );
            if (item->icon_other) DeleteObject( item->icon_other );
            memset( item, 0, sizeof(*item) );
            item->kind = ITEM_PROGRAM;
            lstrcpynW( item->name, files[i].name, ARRAY_SIZE(item->name) );
            lstrcpynW( item->path, files[i].path, ARRAY_SIZE(item->path) );
            swprintf( item->key, ARRAY_SIZE(item->key), L"file:%s", files[i].name );
            item->aliased = TRUE;
            if (view_add( &count, item )) found++;
        }
        if (!found) count = header;
    }

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

/* the icon at any size, for the tiles and the lists of Windows 10 and 11 */
HBITMAP items_icon_sized( struct item *item, int size )
{
    HICON icon = NULL;
    SHFILEINFOW info;
    const WCHAR *path;

    if (size == px( 32 )) return items_icon( item, TRUE );
    if (size == px( 16 )) return items_icon( item, FALSE );
    if (item->icon_other && item->other_size == size) return item->icon_other;
    if (item->kind == ITEM_HEADER) return NULL;
    if (item->icon_other) DeleteObject( item->icon_other );
    item->icon_other = NULL;
    if (item->icon_path[0] && item->kind == ITEM_PROGRAM)
        PrivateExtractIconsW( item->icon_path, 0, size, size, &icon, NULL, 1, 0 );
    if (!icon)
    {
        path = item->icon_path[0] ? item->icon_path : item->path;
        /* the largest the shell has, which DrawIconEx scales */
        if (SHGetFileInfoW( path, 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON )) icon = info.hIcon;
    }
    if (!icon) return NULL;
    item->icon_other = icon_bitmap( icon, size );
    item->other_size = size;
    DestroyIcon( icon );
    return item->icon_other;
}

static int __cdecl compare_names( const void *a, const void *b )
{
    const struct item *x = *(const struct item **)a, *y = *(const struct item **)b;
    return CompareStringW( LOCALE_USER_DEFAULT, NORM_IGNORECASE, x->name, -1, y->name, -1 ) - CSTR_EQUAL;
}

/* All apps of Windows 10 and 11: programs and folders together, by name;
 * an expanded folder's programs under it */
UINT items_apps( struct item ***list )
{
    struct item **sorted = malloc( max( 1, root_count ) * sizeof(*sorted) );
    UINT count = 0;

    if (!sorted) return items_all_programs( list );
    for (UINT i = 0; i < root_count; i++) sorted[i] = &root[i];
    qsort( sorted, root_count, sizeof(*sorted), compare_names );
    for (UINT i = 0; i < root_count; i++)
    {
        view_add( &count, sorted[i] );
        if (sorted[i]->kind == ITEM_FOLDER && sorted[i]->expanded)
        {
            load_children( sorted[i] );
            add_tree( &count, sorted[i]->children, sorted[i]->child_count );
        }
    }
    free( sorted );
    *list = view;
    return count;
}

static void collect_added( struct item *items, UINT n, struct item **found, UINT *count, UINT max, ULONGLONG since )
{
    for (UINT i = 0; i < n; i++)
    {
        ULONGLONG time = ((ULONGLONG)items[i].added.dwHighDateTime << 32) | items[i].added.dwLowDateTime;

        if (items[i].kind == ITEM_FOLDER && !items[i].builtin)
        {
            load_children( &items[i] );
            collect_added( items[i].children, items[i].child_count, found, count, max, since );
            continue;
        }
        if (items[i].kind != ITEM_PROGRAM || time < since) continue;
        if (*count < max) found[(*count)++] = &items[i];
        else
        {
            /* the oldest of them makes room */
            UINT oldest = 0;
            for (UINT j = 1; j < max; j++)
                if (CompareFileTime( &found[j]->added, &found[oldest]->added ) < 0) oldest = j;
            if (CompareFileTime( &items[i].added, &found[oldest]->added ) > 0) found[oldest] = &items[i];
        }
    }
}

static int __cdecl compare_added( const void *a, const void *b )
{
    const struct item *x = *(const struct item **)a, *y = *(const struct item **)b;
    return -CompareFileTime( &x->added, &y->added );
}

/* "Нещодавно додані": shortcuts made in the last two weeks, newest first */
UINT items_recently_added( struct item ***list, UINT max )
{
    struct item *found[16];
    FILETIME now_ft;
    ULONGLONG now;
    UINT count = 0, n = 0;

    GetSystemTimeAsFileTime( &now_ft );
    now = ((ULONGLONG)now_ft.dwHighDateTime << 32) | now_ft.dwLowDateTime;
    max = min( max, ARRAY_SIZE(found) );
    if (max) collect_added( root, root_count, found, &n, max, now - 14ull * 24 * 3600 * 10000000 );
    qsort( found, n, sizeof(*found), compare_added );
    for (UINT i = 0; i < n; i++) view_add( &count, found[i] );
    *list = view;
    return count;
}

static int __cdecl compare_files( const void *a, const void *b )
{
    const struct recent_file *x = a, *y = b;
    return -CompareFileTime( &x->time, &y->time );
}

/* the newest files of Recent, for Windows 11's Recommended */
UINT items_recent_files( struct recent_file *files, UINT max )
{
    WCHAR dir[MAX_PATH], pattern[MAX_PATH];
    WIN32_FIND_DATAW data;
    HANDLE find;
    UINT count = 0;

    if (!max || FAILED(SHGetFolderPathW( NULL, CSIDL_RECENT, NULL, SHGFP_TYPE_CURRENT, dir ))) return 0;
    swprintf( pattern, ARRAY_SIZE(pattern), L"%s\\*.lnk", dir );
    if ((find = FindFirstFileW( pattern, &data )) == INVALID_HANDLE_VALUE) return 0;
    do
    {
        struct recent_file *file;
        WCHAR *dot;

        if (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN)) continue;
        if (count == max)
        {
            qsort( files, count, sizeof(*files), compare_files );
            if (CompareFileTime( &data.ftLastWriteTime, &files[count - 1].time ) <= 0) continue;
            count--;
        }
        file = &files[count++];
        swprintf( file->path, MAX_PATH, L"%s\\%s", dir, data.cFileName );
        lstrcpynW( file->name, data.cFileName, MAX_PATH );
        if ((dot = wcsrchr( file->name, '.' ))) *dot = 0;
        file->time = data.ftLastWriteTime;
    } while (FindNextFileW( find, &data ));
    FindClose( find );
    qsort( files, count, sizeof(*files), compare_files );
    return count;
}

HBITMAP recent_file_icon( const struct recent_file *file, int size )
{
    SHFILEINFOW info;
    HBITMAP bitmap;

    if (!SHGetFileInfoW( file->path, 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON )) return NULL;
    bitmap = icon_bitmap( info.hIcon, size );
    DestroyIcon( info.hIcon );
    return bitmap;
}

/* the settings of the Control Panel, for its menu in the right column */
UINT items_applets( struct item ***list )
{
    UINT count = 0;

    for (UINT i = 0; i < applet_count; i++) view_add( &count, &applet_items[i] );
    *list = view;
    return count;
}
