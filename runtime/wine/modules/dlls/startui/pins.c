/*
 * Pins: the programs kept on the taskbar and in the Start menus
 *
 * The taskbar's are shortcuts in the folder Windows keeps them in
 * (%APPDATA%\Microsoft\Internet Explorer\Quick Launch\User Pinned\TaskBar),
 * in the order HKCU\Software\Arctic\Taskbar\Pinned names their files.
 * explorer's task band has a button for each (ReactOS's taskswnd.cpp, Arctic
 * patch): the program's windows take it when it runs, and it stays when they
 * close. The Start menus pin by the keys of items.c, the same for all of
 * them.
 *
 * What is dragged onto the task band, the Start button or a Start menu is
 * pinned there (explorer registers its two windows here).
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
#include "ole2.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(startui);

#define TASKBAR_KEY     L"Software\\Arctic\\Taskbar"
#define USER_PINNED     L"\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\"
#define MAX_PINS        64
#define HOVER_OPEN      500     /* ms over the Start button before it opens */
#define TWM_OPENSTARTMENU (WM_USER + 260)

/**********************************************************************
 *          Files and shortcuts
 */

const WCHAR *path_extension( const WCHAR *path )
{
    const WCHAR *slash = wcsrchr( path, '\\' ), *dot = wcsrchr( path, '.' );

    return dot && (!slash || dot > slash) ? dot : path + lstrlenW( path );
}

static const WCHAR *file_part( const WCHAR *path )
{
    const WCHAR *slash = wcsrchr( path, '\\' );

    return slash ? slash + 1 : path;
}

/* a folder of User Pinned, made when it is not there yet */
BOOL pinned_folder( const WCHAR *name, WCHAR *path )
{
    WCHAR appdata[MAX_PATH];

    if (FAILED(SHGetFolderPathW( NULL, CSIDL_APPDATA | CSIDL_FLAG_CREATE, NULL, SHGFP_TYPE_CURRENT, appdata ))) return FALSE;
    swprintf( path, MAX_PATH, L"%s" USER_PINNED L"%s", appdata, name );
    if (GetFileAttributesW( path ) == INVALID_FILE_ATTRIBUTES) SHCreateDirectoryExW( NULL, path, NULL );
    return GetFileAttributesW( path ) != INVALID_FILE_ATTRIBUTES;
}

/* what a shortcut starts */
BOOL link_target( const WCHAR *link, WCHAR *target, int size )
{
    IShellLinkW *shell_link;
    IPersistFile *file;
    BOOL ok = FALSE;

    target[0] = 0;
    if (FAILED(CoCreateInstance( &CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&shell_link )))
        return FALSE;
    if (SUCCEEDED(IShellLinkW_QueryInterface( shell_link, &IID_IPersistFile, (void **)&file )))
    {
        ok = SUCCEEDED(IPersistFile_Load( file, link, STGM_READ )) &&
             IShellLinkW_GetPath( shell_link, target, size, NULL, 0 ) == S_OK && target[0];
        IPersistFile_Release( file );
    }
    IShellLinkW_Release( shell_link );
    if (!ok) target[0] = 0;
    return ok;
}

BOOL link_make( const WCHAR *link, const WCHAR *target, const WCHAR *args, const WCHAR *icon )
{
    IShellLinkW *shell_link;
    IPersistFile *file;
    WCHAR dir[MAX_PATH], *slash;
    HRESULT hr;

    if (FAILED(CoCreateInstance( &CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&shell_link )))
        return FALSE;
    IShellLinkW_SetPath( shell_link, target );
    if (args && args[0]) IShellLinkW_SetArguments( shell_link, args );
    if (icon && icon[0]) IShellLinkW_SetIconLocation( shell_link, icon, 0 );
    lstrcpynW( dir, target, ARRAY_SIZE(dir) );
    if ((slash = wcsrchr( dir, '\\' )) && GetFileAttributesW( target ) != INVALID_FILE_ATTRIBUTES &&
        !(GetFileAttributesW( target ) & FILE_ATTRIBUTE_DIRECTORY))
    {
        *slash = 0;
        IShellLinkW_SetWorkingDirectory( shell_link, dir );
    }
    hr = IShellLinkW_QueryInterface( shell_link, &IID_IPersistFile, (void **)&file );
    if (SUCCEEDED(hr))
    {
        hr = IPersistFile_Save( file, link, TRUE );
        IPersistFile_Release( file );
    }
    IShellLinkW_Release( shell_link );
    return SUCCEEDED(hr);
}

/* a name in a folder nothing has yet: Name.lnk, Name (2).lnk */
void free_name( const WCHAR *folder, const WCHAR *base, const WCHAR *ext, WCHAR *path )
{
    WCHAR clean[MAX_PATH];
    int n = 0;

    /* what a file name cannot have goes */
    for (const WCHAR *p = base; *p && n < ARRAY_SIZE(clean) - 1; p++)
        if (!wcschr( L"\\/:*?\"<>|", *p )) clean[n++] = *p;
    clean[n] = 0;
    if (!clean[0]) lstrcpyW( clean, L"Program" );
    swprintf( path, MAX_PATH, L"%s\\%s%s", folder, clean, ext );
    for (int i = 2; GetFileAttributesW( path ) != INVALID_FILE_ATTRIBUTES && i < 100; i++)
        swprintf( path, MAX_PATH, L"%s\\%s (%d)%s", folder, clean, i, ext );
}

/* the program a file stands for: itself, or what its shortcut starts */
static BOOL program_of( const WCHAR *path, WCHAR *target, int size )
{
    const WCHAR *ext = path_extension( path );

    target[0] = 0;
    if (!wcsicmp( ext, L".exe" ) && GetFileAttributesW( path ) != INVALID_FILE_ATTRIBUTES)
        lstrcpynW( target, path, size );
    else if (!wcsicmp( ext, L".lnk" )) link_target( path, target, size );
    return target[0] && !wcsicmp( path_extension( target ), L".exe" );
}

/**********************************************************************
 *          The taskbar's pins
 */

static WCHAR *order_read( BOOL *present )
{
    DWORD size = 0, type;
    WCHAR *data;
    HKEY key;

    *present = FALSE;
    if (RegOpenKeyExW( HKEY_CURRENT_USER, TASKBAR_KEY, 0, KEY_READ, &key )) return NULL;
    if (RegQueryValueExW( key, L"Pinned", NULL, &type, NULL, &size ) || type != REG_MULTI_SZ ||
        !(data = calloc( 1, size + 2 * sizeof(WCHAR) )))
    {
        RegCloseKey( key );
        return NULL;
    }
    RegQueryValueExW( key, L"Pinned", NULL, NULL, (BYTE *)data, &size );
    RegCloseKey( key );
    *present = TRUE;
    return data;
}

static BOOL order_has( const WCHAR *order, const WCHAR *name )
{
    for (const WCHAR *p = order; p && *p; p += lstrlenW( p ) + 1)
        if (!wcsicmp( p, name )) return TRUE;
    return FALSE;
}

static void order_save( WCHAR (*links)[MAX_PATH], UINT count )
{
    DWORD chars = 1, used = 0;
    WCHAR *data;
    HKEY key;

    for (UINT i = 0; i < count; i++) chars += lstrlenW( file_part( links[i] ) ) + 1;
    if (!(data = calloc( chars, sizeof(WCHAR) ))) return;
    for (UINT i = 0; i < count; i++)
    {
        lstrcpyW( data + used, file_part( links[i] ) );
        used += lstrlenW( data + used ) + 1;
    }
    data[used++] = 0;
    if (!RegCreateKeyExW( HKEY_CURRENT_USER, TASKBAR_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL ))
    {
        RegSetValueExW( key, L"Pinned", 0, REG_MULTI_SZ, (const BYTE *)data, used * sizeof(WCHAR) );
        RegCloseKey( key );
    }
    free( data );
}

/* a shortcut of All programs onto the taskbar, under its own name */
static void default_from_start( const WCHAR *folder, const WCHAR *name, WCHAR (*links)[MAX_PATH], UINT *count )
{
    WCHAR programs[MAX_PATH], source[MAX_PATH];

    if (SHGetFolderPathW( NULL, CSIDL_COMMON_PROGRAMS, NULL, SHGFP_TYPE_CURRENT, programs )) return;
    swprintf( source, MAX_PATH, L"%s\\%s.lnk", programs, name );
    if (GetFileAttributesW( source ) == INVALID_FILE_ATTRIBUTES) return;
    free_name( folder, name, L".lnk", links[*count] );
    if (CopyFileW( source, links[*count], TRUE )) (*count)++;
}

/* a program of the system onto the taskbar */
static void default_program( const WCHAR *folder, UINT name, const WCHAR *file, WCHAR (*links)[MAX_PATH], UINT *count )
{
    WCHAR path[MAX_PATH];

    GetWindowsDirectoryW( path, ARRAY_SIZE(path) );
    lstrcatW( path, file );
    if (GetFileAttributesW( path ) == INVALID_FILE_ATTRIBUTES) return;
    free_name( folder, load_string( name ), L".lnk", links[*count] );
    if (link_make( links[*count], path, NULL, NULL )) (*count)++;
}

/* before anything is pinned: File Explorer, the browser, the Task Manager
 * and Arctic WordPad */
static void taskbar_defaults( const WCHAR *folder, WCHAR (*links)[MAX_PATH], UINT *count )
{
    WCHAR pattern[MAX_PATH];
    WIN32_FIND_DATAW data;
    HANDLE find;

    swprintf( pattern, MAX_PATH, L"%s\\*.lnk", folder );
    if ((find = FindFirstFileW( pattern, &data )) != INVALID_HANDLE_VALUE)
    {
        /* the folder had its own already (another Windows' profile) */
        FindClose( find );
        return;
    }
    default_program( folder, IDS_EXPLORER, L"\\explorer.exe", links, count );
    default_from_start( folder, L"Google Chrome", links, count );
    default_program( folder, IDS_TASKMGR, L"\\system32\\taskmgr.exe", links, count );
    default_from_start( folder, L"Arctic WordPad", links, count );
}

/* the shortcuts of the pins, in their order */
static UINT taskbar_pins( WCHAR (*links)[MAX_PATH], UINT max )
{
    WCHAR folder[MAX_PATH], pattern[MAX_PATH], *order;
    WIN32_FIND_DATAW data;
    HANDLE find;
    BOOL present, changed = FALSE;
    UINT count = 0;

    if (!pinned_folder( L"TaskBar", folder )) return 0;
    order = order_read( &present );
    if (!present)
    {
        taskbar_defaults( folder, links, &count );
        changed = TRUE;
    }
    for (const WCHAR *p = order; p && *p && count < max; p += lstrlenW( p ) + 1)
    {
        swprintf( links[count], MAX_PATH, L"%s\\%s", folder, p );
        if (GetFileAttributesW( links[count] ) != INVALID_FILE_ATTRIBUTES) count++;
        else changed = TRUE;
    }
    /* what others put in the folder comes after them */
    swprintf( pattern, MAX_PATH, L"%s\\*.lnk", folder );
    if ((find = FindFirstFileW( pattern, &data )) != INVALID_HANDLE_VALUE)
    {
        do
        {
            BOOL listed = order_has( order, data.cFileName );

            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY || count >= max) continue;
            for (UINT i = 0; i < count && !listed; i++) listed = !wcsicmp( file_part( links[i] ), data.cFileName );
            if (listed) continue;
            swprintf( links[count++], MAX_PATH, L"%s\\%s", folder, data.cFileName );
            changed = TRUE;
        } while (FindNextFileW( find, &data ));
        FindClose( find );
    }
    if (changed) order_save( links, count );
    free( order );
    return count;
}

/* the task band makes its buttons anew */
static void taskbar_pins_changed(void)
{
    HWND tray = FindWindowW( L"Shell_TrayWnd", NULL ), band = NULL;

    if (tray) band = FindWindowExW( tray, NULL, L"ReBarWindow32", NULL );
    if (band) band = FindWindowExW( band, NULL, L"MSTaskSwWClass", NULL );
    if (!band && tray)
    {
        /* wherever it is under the tray */
        HWND child = NULL;

        while (!band && (child = FindWindowExW( tray, child, NULL, NULL )))
            band = FindWindowExW( child, NULL, L"MSTaskSwWClass", NULL );
    }
    if (band) PostMessageW( band, RegisterWindowMessageW( L"ArcticTaskbarPinsChanged" ), 0, 0 );
}

/* the shortcut on the taskbar of a program, if it is there */
BOOL taskbar_pin_of( const WCHAR *program, WCHAR *link )
{
    WCHAR (*links)[MAX_PATH] = malloc( MAX_PINS * sizeof(*links) ), target[MAX_PATH];
    UINT count;
    BOOL found = FALSE;

    if (!links || !program[0]) return FALSE;
    count = taskbar_pins( links, MAX_PINS );
    for (UINT i = 0; i < count && !found; i++)
    {
        if (!link_target( links[i], target, ARRAY_SIZE(target) ) || wcsicmp( target, program )) continue;
        lstrcpyW( link, links[i] );
        found = TRUE;
    }
    free( links );
    return found;
}

/* the pins for explorer: "shortcut|program" each, in their order */
LPWSTR WINAPI ArcticTaskbarPinList(void)
{
    WCHAR (*links)[MAX_PATH] = malloc( MAX_PINS * sizeof(*links) ), target[MAX_PATH];
    UINT count, used = 0;
    WCHAR *list;

    if (!links) return NULL;
    count = taskbar_pins( links, MAX_PINS );
    if (!(list = LocalAlloc( LMEM_ZEROINIT, (count * 2 * MAX_PATH + 2) * sizeof(WCHAR) )))
    {
        free( links );
        return NULL;
    }
    for (UINT i = 0; i < count; i++)
    {
        link_target( links[i], target, ARRAY_SIZE(target) );
        swprintf( list + used, 2 * MAX_PATH, L"%s|%s", links[i], target );
        used += lstrlenW( list + used ) + 1;
    }
    list[used] = 0;
    free( links );
    return list;
}

/* a program, or its shortcut, onto the taskbar: the shortcut is kept among
 * the pins (the Start menu's own for a program that has one there) */
static BOOL taskbar_pin( const WCHAR *path, const WCHAR *name )
{
    WCHAR folder[MAX_PATH], target[MAX_PATH], dest[MAX_PATH], base[MAX_PATH], shortcut[MAX_PATH], *dot;
    WCHAR (*links)[MAX_PATH];
    UINT count;
    BOOL ok;

    if (!program_of( path, target, ARRAY_SIZE(target) ) || !pinned_folder( L"TaskBar", folder )) return FALSE;
    if (taskbar_pin_of( target, dest )) return TRUE;
    if (!(links = malloc( MAX_PINS * sizeof(*links) ))) return FALSE;
    count = taskbar_pins( links, MAX_PINS );
    if (!wcsicmp( path_extension( path ), L".lnk" )) lstrcpynW( shortcut, path, ARRAY_SIZE(shortcut) );
    else if (!items_shortcut_of( target, shortcut )) shortcut[0] = 0;
    if (shortcut[0])
    {
        lstrcpynW( base, name && name[0] ? name : file_part( shortcut ), ARRAY_SIZE(base) );
        if (!(name && name[0]) && (dot = wcsrchr( base, '.' ))) *dot = 0;
        free_name( folder, base, L".lnk", dest );
        ok = CopyFileW( shortcut, dest, TRUE );
    }
    else
    {
        if (name && name[0]) lstrcpynW( base, name, ARRAY_SIZE(base) );
        else program_name( target, base, ARRAY_SIZE(base) );
        free_name( folder, base, L".lnk", dest );
        ok = link_make( dest, target, NULL, NULL );
    }
    if (ok && count < MAX_PINS)
    {
        lstrcpyW( links[count++], dest );
        order_save( links, count );
    }
    free( links );
    if (ok) taskbar_pins_changed();
    TRACE( "%s -> %s: %d\n", debugstr_w(path), debugstr_w(dest), ok );
    return ok;
}

BOOL WINAPI ArcticTaskbarPinFile( LPCWSTR path )
{
    return path && taskbar_pin( path, NULL );
}

BOOL taskbar_pin_named( const WCHAR *path, const WCHAR *name )
{
    return taskbar_pin( path, name );
}

BOOL WINAPI ArcticTaskbarUnpinFile( LPCWSTR link )
{
    WCHAR (*links)[MAX_PATH] = malloc( MAX_PINS * sizeof(*links) );
    UINT count, kept = 0;

    if (!links) return FALSE;
    count = taskbar_pins( links, MAX_PINS );
    for (UINT i = 0; i < count; i++)
    {
        if (!wcsicmp( links[i], link )) continue;
        if (kept != i) lstrcpyW( links[kept], links[i] );
        kept++;
    }
    DeleteFileW( link );
    order_save( links, kept );
    free( links );
    taskbar_pins_changed();
    return TRUE;
}

/* the menu of a pinned program's button with no window: start it, or unpin it */
BOOL WINAPI ArcticTaskbarPinMenu( HWND owner, LPCWSTR link, const RECT *button )
{
    struct popup_item items[2];
    struct look look;
    MONITORINFO info = { sizeof(info) };
    WCHAR name[MAX_PATH], *dot;
    HWND previous = GetForegroundWindow(), root = GetAncestor( owner, GA_ROOT );
    HBITMAP icon = NULL;
    SHFILEINFOW file = { 0 };
    int style = taskbar_style11() ? POPUP_WIN11 : POPUP_WIN10;
    UINT align = TPM_CENTERALIGN, chosen;
    POINT pt;

    lstrcpynW( name, file_part( link ), ARRAY_SIZE(name) );
    if ((dot = wcsrchr( name, '.' ))) *dot = 0;
    if (SHGetFileInfoW( link, 0, &file, sizeof(file), SHGFI_ICON | SHGFI_SMALLICON ) && file.hIcon)
    {
        icon = icon_bitmap( file.hIcon, px( 16 ) );
        DestroyIcon( file.hIcon );
    }
    items[0] = (struct popup_item){ .id = 1, .text = name, .glyph = GLYPH_NONE, .icon = icon };
    items[1] = (struct popup_item){ .id = 2, .text = load_string( IDS_UNPIN_TASKBAR ), .glyph = GLYPH_UNPIN };

    GetMonitorInfoW( MonitorFromRect( button, MONITOR_DEFAULTTONEAREST ), &info );
    pt.x = (button->left + button->right) / 2;
    if ((button->top + button->bottom) / 2 > (info.rcMonitor.top + info.rcMonitor.bottom) / 2)
    {
        pt.y = button->top - (style == POPUP_WIN11 ? px( 8 ) : 0);
        align |= TPM_BOTTOMALIGN;
    }
    else pt.y = button->bottom + (style == POPUP_WIN11 ? px( 8 ) : 0);

    look_load_taskbar( &look );
    chosen = popup_track( root, pt, align, items, 2, style | POPUP_TASKBAR, &look );
    if (icon) DeleteObject( icon );
    if (previous && previous != root && IsWindow( previous )) SetForegroundWindow( previous );
    if (chosen == 1) ShellExecuteW( NULL, NULL, link, NULL, NULL, SW_SHOWNORMAL );
    else if (chosen == 2) ArcticTaskbarUnpinFile( link );
    return TRUE;
}

/**********************************************************************
 *          What is dropped
 */

struct drop
{
    IDropTarget IDropTarget_iface;
    LONG ref;
    HWND hwnd;
    HWND worker;                 /* in the thread of hwnd: OLE calls in the dragging one */
    DWORD where;                 /* PIN_TASKBAR, PIN_START */
    BOOL pinnable;               /* what is dragged can be pinned there */
    DWORD came;                  /* when the pointer came */
    BOOL opened;                 /* the Start menu opened for it */
    void (*dropped)( HWND hwnd );
};

static inline struct drop *impl_from_IDropTarget( IDropTarget *iface )
{
    return CONTAINING_RECORD( iface, struct drop, IDropTarget_iface );
}

/* what may be pinned where: programs on the taskbar; programs, shortcuts
 * of anything and folders in Start */
static BOOL can_pin( const WCHAR *path, DWORD where )
{
    WCHAR target[MAX_PATH];
    const WCHAR *ext = path_extension( path );
    DWORD attributes = GetFileAttributesW( path );

    if (attributes == INVALID_FILE_ATTRIBUTES) return FALSE;
    if (where == PIN_TASKBAR) return program_of( path, target, ARRAY_SIZE(target) );
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) || !wcsicmp( ext, L".lnk" ) || !wcsicmp( ext, L".exe" ) ||
           !wcsicmp( ext, L".url" );
}

/* the files dragged: CF_HDROP, or the shell's list of items */
static UINT dragged_files( IDataObject *data, WCHAR (*files)[MAX_PATH], UINT max )
{
    FORMATETC format = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium;
    UINT count = 0;

    if (SUCCEEDED(IDataObject_GetData( data, &format, &medium )))
    {
        HDROP drop = GlobalLock( medium.hGlobal );

        if (drop)
        {
            UINT total = DragQueryFileW( drop, ~0u, NULL, 0 );

            for (UINT i = 0; i < total && count < max; i++)
                if (DragQueryFileW( drop, i, files[count], MAX_PATH )) count++;
            GlobalUnlock( medium.hGlobal );
        }
        ReleaseStgMedium( &medium );
        if (count) return count;
    }
    format.cfFormat = RegisterClipboardFormatW( CFSTR_SHELLIDLISTW );
    if (SUCCEEDED(IDataObject_GetData( data, &format, &medium )))
    {
        CIDA *cida = GlobalLock( medium.hGlobal );

        if (cida)
        {
            LPCITEMIDLIST parent = (LPCITEMIDLIST)((BYTE *)cida + cida->aoffset[0]);

            for (UINT i = 0; i < cida->cidl && count < max; i++)
            {
                LPITEMIDLIST full = ILCombine( parent, (LPCITEMIDLIST)((BYTE *)cida + cida->aoffset[i + 1]) );

                if (full && SHGetPathFromIDListW( full, files[count] )) count++;
                ILFree( full );
            }
            GlobalUnlock( medium.hGlobal );
        }
        ReleaseStgMedium( &medium );
    }
    return count;
}

static BOOL any_pinnable( IDataObject *data, DWORD where )
{
    WCHAR (*files)[MAX_PATH] = malloc( 16 * sizeof(*files) );
    UINT count;
    BOOL found = FALSE;

    if (!files) return FALSE;
    count = dragged_files( data, files, 16 );
    for (UINT i = 0; i < count && !found; i++) found = can_pin( files[i], where );
    free( files );
    return found;
}

/* a link, never a move: the shortcut stays where it was */
static DWORD drop_effect( const struct drop *drop, DWORD allowed )
{
    if (!drop->pinnable) return DROPEFFECT_NONE;
    if (allowed & DROPEFFECT_LINK) return DROPEFFECT_LINK;
    return allowed & DROPEFFECT_COPY;
}

static void hover( struct drop *drop, POINTL pt, BOOL over )
{
    if (drop->where == PIN_TASKBAR)
    {
        /* the task band brings up the window of a button held over */
        PostMessageW( drop->hwnd, RegisterWindowMessageW( L"ArcticTaskbarDragHover" ), over,
                      MAKELPARAM( (SHORT)pt.x, (SHORT)pt.y ) );
        return;
    }
    if (drop->where == PIN_START_BUTTON && over && !drop->opened && GetTickCount() - drop->came >= HOVER_OPEN)
    {
        /* held over Start, the menu opens to be dropped in */
        drop->opened = TRUE;
        if (!ArcticStartMenuVisible()) PostMessageW( GetParent( drop->hwnd ), TWM_OPENSTARTMENU, 0, 0 );
    }
}

static HRESULT WINAPI drop_QueryInterface( IDropTarget *iface, REFIID riid, void **out )
{
    if (IsEqualIID( riid, &IID_IUnknown ) || IsEqualIID( riid, &IID_IDropTarget ))
    {
        *out = iface;
        IDropTarget_AddRef( iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI drop_AddRef( IDropTarget *iface )
{
    return InterlockedIncrement( &impl_from_IDropTarget( iface )->ref );
}

static ULONG WINAPI drop_Release( IDropTarget *iface )
{
    struct drop *drop = impl_from_IDropTarget( iface );
    ULONG ref = InterlockedDecrement( &drop->ref );

    if (!ref) free( drop );
    return ref;
}

static HRESULT WINAPI drop_DragEnter( IDropTarget *iface, IDataObject *data, DWORD keys, POINTL pt, DWORD *effect )
{
    struct drop *drop = impl_from_IDropTarget( iface );

    drop->pinnable = any_pinnable( data, drop->where == PIN_TASKBAR ? PIN_TASKBAR : PIN_START );
    drop->came = GetTickCount();
    drop->opened = FALSE;
    *effect = drop_effect( drop, *effect );
    hover( drop, pt, TRUE );
    return S_OK;
}

static HRESULT WINAPI drop_DragOver( IDropTarget *iface, DWORD keys, POINTL pt, DWORD *effect )
{
    struct drop *drop = impl_from_IDropTarget( iface );

    *effect = drop_effect( drop, *effect );
    hover( drop, pt, TRUE );
    return S_OK;
}

static HRESULT WINAPI drop_DragLeave( IDropTarget *iface )
{
    struct drop *drop = impl_from_IDropTarget( iface );
    POINTL pt = { 0, 0 };

    hover( drop, pt, FALSE );
    return S_OK;
}

struct drop_job
{
    struct drop *drop;
    WCHAR (*files)[MAX_PATH];
    UINT count;
};

#define WM_PIN_DROPPED (WM_APP + 1)

/* in the thread of the window dropped on: the Start menus and their
 * programs belong to it */
static BOOL pin_dropped( struct drop_job *job )
{
    struct drop *drop = job->drop;
    BOOL pinned = FALSE;

    for (UINT i = 0; i < job->count; i++)
    {
        if (!can_pin( job->files[i], drop->where == PIN_TASKBAR ? PIN_TASKBAR : PIN_START )) continue;
        if (drop->where == PIN_TASKBAR) pinned |= taskbar_pin( job->files[i], NULL );
        else pinned |= items_pin_file( job->files[i] );
    }
    if (pinned && drop->dropped) drop->dropped( drop->hwnd );
    return pinned;
}

static LRESULT WINAPI worker_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    if (msg == WM_PIN_DROPPED) return pin_dropped( (struct drop_job *)lparam );
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

static HRESULT WINAPI drop_Drop( IDropTarget *iface, IDataObject *data, DWORD keys, POINTL pt, DWORD *effect )
{
    struct drop *drop = impl_from_IDropTarget( iface );
    struct drop_job job = { drop, malloc( 16 * sizeof(*job.files) ), 0 };
    BOOL pinned = FALSE;

    *effect = drop_effect( drop, *effect );
    hover( drop, pt, FALSE );
    if (job.files) job.count = dragged_files( data, job.files, 16 );
    if (job.count)
    {
        if (drop->worker && GetWindowThreadProcessId( drop->worker, NULL ) != GetCurrentThreadId())
            pinned = SendMessageW( drop->worker, WM_PIN_DROPPED, 0, (LPARAM)&job );
        else pinned = pin_dropped( &job );
    }
    free( job.files );
    if (!pinned) *effect = DROPEFFECT_NONE;
    return S_OK;
}

static const IDropTargetVtbl drop_vtbl =
{
    drop_QueryInterface,
    drop_AddRef,
    drop_Release,
    drop_DragEnter,
    drop_DragOver,
    drop_DragLeave,
    drop_Drop,
};

BOOL pins_drop_register( HWND hwnd, DWORD where, void (*dropped)( HWND hwnd ) )
{
    static BOOL registered;
    struct drop *drop;
    HRESULT hr;

    if (!hwnd || !(drop = calloc( 1, sizeof(*drop) ))) return FALSE;
    if (!registered)
    {
        WNDCLASSW cls = { 0 };

        cls.lpfnWndProc = worker_proc;
        cls.hInstance = startui_instance;
        cls.lpszClassName = L"ArcticPinDrop";
        RegisterClassW( &cls );
        registered = TRUE;
    }
    drop->IDropTarget_iface.lpVtbl = &drop_vtbl;
    drop->ref = 1;
    drop->hwnd = hwnd;
    drop->where = where;
    drop->dropped = dropped;
    drop->worker = CreateWindowExW( 0, L"ArcticPinDrop", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, startui_instance, NULL );
    OleInitialize( NULL );
    hr = RegisterDragDrop( hwnd, &drop->IDropTarget_iface );
    IDropTarget_Release( &drop->IDropTarget_iface );
    if (FAILED(hr)) WARN( "window %p takes no drops: %#lx\n", hwnd, hr );
    return SUCCEEDED(hr);
}

/* explorer's task band (PIN_TASKBAR) and Start button (PIN_START_BUTTON) */
BOOL WINAPI ArcticPinDropRegister( HWND hwnd, DWORD where )
{
    return pins_drop_register( hwnd, where, NULL );
}
