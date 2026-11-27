/*
 * The shell's extensions of the Start menu's library
 *
 * Pin to Start and Pin to taskbar in the context menu of a program, a
 * shortcut, a folder or a drive, as Windows has them (shellex\
 * ContextMenuHandlers of exefile, lnkfile, Directory and Drive,
 * runtime/registry/shell-ext.reg): pins.c does the pinning, in explorer in
 * the thread of the Start menus.
 *
 * Send to's "Desktop (create shortcut)": the drop handler of .DeskLink
 * files, the class Windows gives it, which makes a shortcut on the desktop
 * of each thing sent. Send to's folder gets it, and "Compressed (zipped)
 * folder", once.
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
#include "shlwapi.h"
#include "ole2.h"
#include "initguid.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(startui);

/* Arctic's own: nothing looks this extension up by a class id of Windows */
DEFINE_GUID( CLSID_ArcticPinMenu, 0x5b4f2c1e, 0x8d3a, 0x4e7b, 0x9c, 0x61, 0x2a, 0x7d, 0x0e, 0x4f, 0x9b, 0x13 );
/* Windows' "Desktop (create shortcut)" (sendmail.dll there) */
DEFINE_GUID( CLSID_DeskLink, 0x9e56be61, 0xc50f, 0x11cf, 0x9a, 0x2c, 0x00, 0xa0, 0xc9, 0x0a, 0x90, 0xce );

#define SHELL_KEY L"Software\\Arctic\\Shell"

/* the pictures of Windows 11's menus, from shell32 (which gives them only
 * with the style of Windows 11) */
static void set_picture( HMENU menu, UINT item, BOOL by_position, const char *name )
{
    static BOOL (WINAPI *set_icon)( HMENU, UINT, BOOL, const char * );

    if (!set_icon)
    {
        HMODULE shell32 = GetModuleHandleW( L"shell32.dll" );
        if (shell32) set_icon = (void *)GetProcAddress( shell32, "ArcticSetMenuIcon" );
    }
    if (set_icon) set_icon( menu, item, by_position, name );
}

/* the one file chosen, or "" */
static void chosen_file( IDataObject *data, WCHAR *path )
{
    FORMATETC format = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium;

    path[0] = 0;
    if (!data || FAILED(IDataObject_GetData( data, &format, &medium ))) return;
    if (DragQueryFileW( medium.hGlobal, ~0u, NULL, 0 ) == 1) DragQueryFileW( medium.hGlobal, 0, path, MAX_PATH );
    ReleaseStgMedium( &medium );
    if (GetFileAttributesW( path ) == INVALID_FILE_ATTRIBUTES) path[0] = 0;
}

/**********************************************************************
 *          Pin to Start, Pin to taskbar
 */

enum { VERB_START, VERB_TASKBAR, VERB_COUNT };

struct pin_menu
{
    IShellExtInit IShellExtInit_iface;
    IContextMenu IContextMenu_iface;
    LONG ref;
    WCHAR path[MAX_PATH];
    DWORD state;                 /* PINS_*, as the menu was made */
};

static inline struct pin_menu *pin_from_init( IShellExtInit *iface )
{
    return CONTAINING_RECORD( iface, struct pin_menu, IShellExtInit_iface );
}

static inline struct pin_menu *pin_from_menu( IContextMenu *iface )
{
    return CONTAINING_RECORD( iface, struct pin_menu, IContextMenu_iface );
}

static HRESULT WINAPI pin_init_QueryInterface( IShellExtInit *iface, REFIID riid, void **out )
{
    struct pin_menu *pin = pin_from_init( iface );

    if (IsEqualIID( riid, &IID_IUnknown ) || IsEqualIID( riid, &IID_IShellExtInit ))
        *out = &pin->IShellExtInit_iface;
    else if (IsEqualIID( riid, &IID_IContextMenu ))
        *out = &pin->IContextMenu_iface;
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG WINAPI pin_init_AddRef( IShellExtInit *iface )
{
    return InterlockedIncrement( &pin_from_init( iface )->ref );
}

static ULONG WINAPI pin_init_Release( IShellExtInit *iface )
{
    struct pin_menu *pin = pin_from_init( iface );
    ULONG ref = InterlockedDecrement( &pin->ref );

    if (!ref) free( pin );
    return ref;
}

static HRESULT WINAPI pin_init_Initialize( IShellExtInit *iface, LPCITEMIDLIST folder, IDataObject *data, HKEY key )
{
    chosen_file( data, pin_from_init( iface )->path );
    return S_OK;
}

static const IShellExtInitVtbl pin_init_vtbl =
{
    pin_init_QueryInterface, pin_init_AddRef, pin_init_Release, pin_init_Initialize
};

static HRESULT WINAPI pin_menu_QueryInterface( IContextMenu *iface, REFIID riid, void **out )
{
    return pin_init_QueryInterface( &pin_from_menu( iface )->IShellExtInit_iface, riid, out );
}

static ULONG WINAPI pin_menu_AddRef( IContextMenu *iface )
{
    return pin_init_AddRef( &pin_from_menu( iface )->IShellExtInit_iface );
}

static ULONG WINAPI pin_menu_Release( IContextMenu *iface )
{
    return pin_init_Release( &pin_from_menu( iface )->IShellExtInit_iface );
}

static HRESULT WINAPI pin_menu_QueryContextMenu( IContextMenu *iface, HMENU menu, UINT index, UINT first, UINT last,
                                                 UINT flags )
{
    struct pin_menu *pin = pin_from_menu( iface );
    UINT added = 0;

    if (!pin->path[0] || (flags & (CMF_DEFAULTONLY | CMF_VERBSONLY)) || first + VERB_COUNT > last)
        return MAKE_HRESULT( SEVERITY_SUCCESS, 0, 0 );
    pin->state = pins_file( PINS_QUERY, pin->path );
    if (pin->state & (PINS_START_CAN | PINS_ON_START))
    {
        BOOL on = (pin->state & PINS_ON_START) != 0;

        InsertMenuW( menu, index + added, MF_BYPOSITION | MF_STRING, first + VERB_START,
                     load_string( on ? IDS_UNPIN_START10 : IDS_PIN_START10 ) );
        set_picture( menu, index + added, TRUE, on ? "unpin" : "pin" );
        added++;
    }
    if (pin->state & (PINS_TASKBAR_CAN | PINS_ON_TASKBAR))
    {
        BOOL on = (pin->state & PINS_ON_TASKBAR) != 0;

        InsertMenuW( menu, index + added, MF_BYPOSITION | MF_STRING, first + VERB_TASKBAR,
                     load_string( on ? IDS_UNPIN_TASKBAR : IDS_PIN_TASKBAR ) );
        set_picture( menu, index + added, TRUE, on ? "unpin" : "pin" );
        added++;
    }
    return MAKE_HRESULT( SEVERITY_SUCCESS, 0, added ? VERB_COUNT : 0 );
}

static const char *pin_verb( const struct pin_menu *pin, UINT id )
{
    if (id == VERB_START) return pin->state & PINS_ON_START ? "unpinfromstart" : "pintostartscreen";
    if (id == VERB_TASKBAR) return pin->state & PINS_ON_TASKBAR ? "taskbarunpin" : "taskbarpin";
    return NULL;
}

static HRESULT WINAPI pin_menu_InvokeCommand( IContextMenu *iface, CMINVOKECOMMANDINFO *info )
{
    struct pin_menu *pin = pin_from_menu( iface );
    UINT id = VERB_COUNT;

    if (!pin->path[0]) return E_INVALIDARG;
    if (IS_INTRESOURCE( info->lpVerb )) id = LOWORD( (UINT_PTR)info->lpVerb );
    else
    {
        for (UINT i = 0; i < VERB_COUNT; i++)
            if (pin_verb( pin, i ) && !lstrcmpiA( info->lpVerb, pin_verb( pin, i ) )) id = i;
    }
    switch (id)
    {
    case VERB_START:
        pins_file( pin->state & PINS_ON_START ? PINS_START_REMOVE : PINS_START_ADD, pin->path );
        return S_OK;
    case VERB_TASKBAR:
        pins_file( pin->state & PINS_ON_TASKBAR ? PINS_TASKBAR_REMOVE : PINS_TASKBAR_ADD, pin->path );
        return S_OK;
    }
    return E_INVALIDARG;
}

static HRESULT WINAPI pin_menu_GetCommandString( IContextMenu *iface, UINT_PTR id, UINT type, UINT *reserved,
                                                 char *name, UINT count )
{
    const char *verb = pin_verb( pin_from_menu( iface ), id );

    if (!verb || !count) return E_INVALIDARG;
    switch (type)
    {
    case GCS_VERBA:
        lstrcpynA( name, verb, count );
        return S_OK;
    case GCS_VERBW:
        MultiByteToWideChar( CP_ACP, 0, verb, -1, (WCHAR *)name, count );
        ((WCHAR *)name)[count - 1] = 0;
        return S_OK;
    case GCS_VALIDATEA:
    case GCS_VALIDATEW:
        return S_OK;
    }
    return E_NOTIMPL;
}

static const IContextMenuVtbl pin_menu_vtbl =
{
    pin_menu_QueryInterface, pin_menu_AddRef, pin_menu_Release,
    pin_menu_QueryContextMenu, pin_menu_InvokeCommand, pin_menu_GetCommandString
};

static HRESULT pin_menu_create( REFIID riid, void **out )
{
    struct pin_menu *pin = calloc( 1, sizeof(*pin) );
    HRESULT hr;

    if (!pin) return E_OUTOFMEMORY;
    pin->IShellExtInit_iface.lpVtbl = &pin_init_vtbl;
    pin->IContextMenu_iface.lpVtbl = &pin_menu_vtbl;
    pin->ref = 1;
    hr = pin_init_QueryInterface( &pin->IShellExtInit_iface, riid, out );
    pin_init_Release( &pin->IShellExtInit_iface );
    return hr;
}

/**********************************************************************
 *          Desktop (create shortcut)
 */

struct desk_link
{
    IPersistFile IPersistFile_iface;
    IDropTarget IDropTarget_iface;
    LONG ref;
};

static inline struct desk_link *desk_from_file( IPersistFile *iface )
{
    return CONTAINING_RECORD( iface, struct desk_link, IPersistFile_iface );
}

static inline struct desk_link *desk_from_drop( IDropTarget *iface )
{
    return CONTAINING_RECORD( iface, struct desk_link, IDropTarget_iface );
}

static HRESULT WINAPI desk_file_QueryInterface( IPersistFile *iface, REFIID riid, void **out )
{
    struct desk_link *desk = desk_from_file( iface );

    if (IsEqualIID( riid, &IID_IUnknown ) || IsEqualIID( riid, &IID_IPersist ) ||
        IsEqualIID( riid, &IID_IPersistFile ))
        *out = &desk->IPersistFile_iface;
    else if (IsEqualIID( riid, &IID_IDropTarget ))
        *out = &desk->IDropTarget_iface;
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG WINAPI desk_file_AddRef( IPersistFile *iface )
{
    return InterlockedIncrement( &desk_from_file( iface )->ref );
}

static ULONG WINAPI desk_file_Release( IPersistFile *iface )
{
    struct desk_link *desk = desk_from_file( iface );
    ULONG ref = InterlockedDecrement( &desk->ref );

    if (!ref) free( desk );
    return ref;
}

static HRESULT WINAPI desk_file_GetClassID( IPersistFile *iface, CLSID *clsid )
{
    *clsid = CLSID_DeskLink;
    return S_OK;
}

static HRESULT WINAPI desk_file_IsDirty( IPersistFile *iface )
{
    return S_FALSE;
}

/* the .DeskLink file itself says nothing */
static HRESULT WINAPI desk_file_Load( IPersistFile *iface, LPCOLESTR name, DWORD mode )
{
    return S_OK;
}

static HRESULT WINAPI desk_file_Save( IPersistFile *iface, LPCOLESTR name, BOOL remember )
{
    return S_OK;
}

static HRESULT WINAPI desk_file_SaveCompleted( IPersistFile *iface, LPCOLESTR name )
{
    return S_OK;
}

static HRESULT WINAPI desk_file_GetCurFile( IPersistFile *iface, LPOLESTR *name )
{
    *name = NULL;
    return E_NOTIMPL;
}

static const IPersistFileVtbl desk_file_vtbl =
{
    desk_file_QueryInterface, desk_file_AddRef, desk_file_Release, desk_file_GetClassID,
    desk_file_IsDirty, desk_file_Load, desk_file_Save, desk_file_SaveCompleted, desk_file_GetCurFile
};

static HRESULT WINAPI desk_drop_QueryInterface( IDropTarget *iface, REFIID riid, void **out )
{
    return desk_file_QueryInterface( &desk_from_drop( iface )->IPersistFile_iface, riid, out );
}

static ULONG WINAPI desk_drop_AddRef( IDropTarget *iface )
{
    return desk_file_AddRef( &desk_from_drop( iface )->IPersistFile_iface );
}

static ULONG WINAPI desk_drop_Release( IDropTarget *iface )
{
    return desk_file_Release( &desk_from_drop( iface )->IPersistFile_iface );
}

static DWORD link_effect( DWORD allowed )
{
    if (allowed & DROPEFFECT_LINK) return DROPEFFECT_LINK;
    return allowed & DROPEFFECT_COPY;
}

static HRESULT WINAPI desk_drop_DragEnter( IDropTarget *iface, IDataObject *data, DWORD keys, POINTL pt, DWORD *effect )
{
    *effect = link_effect( *effect );
    return S_OK;
}

static HRESULT WINAPI desk_drop_DragOver( IDropTarget *iface, DWORD keys, POINTL pt, DWORD *effect )
{
    *effect = link_effect( *effect );
    return S_OK;
}

static HRESULT WINAPI desk_drop_DragLeave( IDropTarget *iface )
{
    return S_OK;
}

/* a shortcut on the desktop: "Name – ярлик" of a file or a folder, a copy of
 * a shortcut under its own name */
static BOOL desk_link_one( const WCHAR *desktop, const WCHAR *path )
{
    const WCHAR *ext = path_extension( path ), *file = wcsrchr( path, '\\' );
    WCHAR base[MAX_PATH], name[MAX_PATH], dest[MAX_PATH], *dot;
    SHFILEINFOW info = { 0 };
    BOOL ok;

    if (GetFileAttributesW( path ) == INVALID_FILE_ATTRIBUTES) return FALSE;
    if (!wcsicmp( ext, L".lnk" ) || !wcsicmp( ext, L".url" ))
    {
        lstrcpynW( base, file ? file + 1 : path, ARRAY_SIZE(base) );
        if ((dot = wcsrchr( base, '.' ))) *dot = 0;
        free_name( desktop, base, ext, dest );
        ok = CopyFileW( path, dest, TRUE );
    }
    else
    {
        /* the name Explorer shows: a drive's label, no extension when hidden */
        if (SHGetFileInfoW( path, 0, &info, sizeof(info), SHGFI_DISPLAYNAME ) && info.szDisplayName[0])
            lstrcpynW( name, info.szDisplayName, ARRAY_SIZE(name) );
        else lstrcpynW( name, file && file[1] ? file + 1 : path, ARRAY_SIZE(name) );
        swprintf( base, ARRAY_SIZE(base), load_string( IDS_SHORTCUT_NAME ), name );
        free_name( desktop, base, L".lnk", dest );
        ok = link_make( dest, path, NULL, NULL );
    }
    if (ok) SHChangeNotify( SHCNE_CREATE, SHCNF_PATHW, dest, NULL );
    TRACE( "%s -> %s: %d\n", debugstr_w(path), debugstr_w(dest), ok );
    return ok;
}

static HRESULT WINAPI desk_drop_Drop( IDropTarget *iface, IDataObject *data, DWORD keys, POINTL pt, DWORD *effect )
{
    WCHAR (*files)[MAX_PATH] = malloc( 64 * sizeof(*files) ), desktop[MAX_PATH];
    UINT count = 0;
    BOOL any = FALSE;

    if (files && !SHGetFolderPathW( NULL, CSIDL_DESKTOPDIRECTORY | CSIDL_FLAG_CREATE, NULL, SHGFP_TYPE_CURRENT, desktop ))
    {
        count = dragged_files( data, files, 64 );
        for (UINT i = 0; i < count; i++) any |= desk_link_one( desktop, files[i] );
    }
    free( files );
    *effect = any ? DROPEFFECT_LINK : DROPEFFECT_NONE;
    return S_OK;
}

static const IDropTargetVtbl desk_drop_vtbl =
{
    desk_drop_QueryInterface, desk_drop_AddRef, desk_drop_Release,
    desk_drop_DragEnter, desk_drop_DragOver, desk_drop_DragLeave, desk_drop_Drop
};

static HRESULT desk_link_create( REFIID riid, void **out )
{
    struct desk_link *desk = calloc( 1, sizeof(*desk) );
    HRESULT hr;

    if (!desk) return E_OUTOFMEMORY;
    desk->IPersistFile_iface.lpVtbl = &desk_file_vtbl;
    desk->IDropTarget_iface.lpVtbl = &desk_drop_vtbl;
    desk->ref = 1;
    hr = desk_file_QueryInterface( &desk->IPersistFile_iface, riid, out );
    desk_file_Release( &desk->IPersistFile_iface );
    return hr;
}

/* an empty file of a type Send to shows by its drop handler, unless the
 * folder has one of that type already */
static void sendto_item( const WCHAR *folder, UINT name, const WCHAR *ext )
{
    WCHAR pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW data;
    HANDLE file;

    swprintf( pattern, MAX_PATH, L"%s\\*%s", folder, ext );
    if ((file = FindFirstFileW( pattern, &data )) != INVALID_HANDLE_VALUE)
    {
        FindClose( file );
        return;
    }
    swprintf( path, MAX_PATH, L"%s\\%s%s", folder, load_string( name ), ext );
    file = CreateFileW( path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL );
    if (file != INVALID_HANDLE_VALUE) CloseHandle( file );
}

/* Windows' Send to has these from the start; once, so that one deleted stays
 * deleted */
void sendto_defaults(void)
{
    WCHAR folder[MAX_PATH];
    DWORD done = 0, size = sizeof(done);
    HKEY key;

    if (!RegGetValueW( HKEY_CURRENT_USER, SHELL_KEY, L"SendToDefaults", RRF_RT_REG_DWORD, NULL, &done, &size ) && done)
        return;
    if (FAILED(SHGetFolderPathW( NULL, CSIDL_SENDTO | CSIDL_FLAG_CREATE, NULL, SHGFP_TYPE_CURRENT, folder ))) return;
    if (!RegOpenKeyExW( HKEY_CLASSES_ROOT, L".ZFSendToTarget", 0, KEY_QUERY_VALUE, &key ))
    {
        RegCloseKey( key );
        sendto_item( folder, IDS_SENDTO_ZIP, L".ZFSendToTarget" );
    }
    sendto_item( folder, IDS_SENDTO_DESKTOP, L".DeskLink" );
    done = 1;
    RegSetKeyValueW( HKEY_CURRENT_USER, SHELL_KEY, L"SendToDefaults", REG_DWORD, &done, sizeof(done) );
}

/**********************************************************************
 *          The class factory
 */

struct factory
{
    IClassFactory IClassFactory_iface;
    HRESULT (*create)( REFIID riid, void **out );
};

static inline struct factory *impl_from_IClassFactory( IClassFactory *iface )
{
    return CONTAINING_RECORD( iface, struct factory, IClassFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IClassFactory *iface, REFIID riid, void **out )
{
    if (IsEqualIID( riid, &IID_IUnknown ) || IsEqualIID( riid, &IID_IClassFactory ))
    {
        *out = iface;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef( IClassFactory *iface )
{
    return 2;
}

static ULONG WINAPI factory_Release( IClassFactory *iface )
{
    return 1;
}

static HRESULT WINAPI factory_CreateInstance( IClassFactory *iface, IUnknown *outer, REFIID riid, void **out )
{
    *out = NULL;
    if (outer) return CLASS_E_NOAGGREGATION;
    return impl_from_IClassFactory( iface )->create( riid, out );
}

static HRESULT WINAPI factory_LockServer( IClassFactory *iface, BOOL lock )
{
    return S_OK;
}

static const IClassFactoryVtbl factory_vtbl =
{
    factory_QueryInterface, factory_AddRef, factory_Release, factory_CreateInstance, factory_LockServer
};

static struct factory pin_factory = { { &factory_vtbl }, pin_menu_create };
static struct factory desk_factory = { { &factory_vtbl }, desk_link_create };

HRESULT WINAPI DllGetClassObject( REFCLSID clsid, REFIID riid, void **out )
{
    if (IsEqualCLSID( clsid, &CLSID_ArcticPinMenu ))
        return IClassFactory_QueryInterface( &pin_factory.IClassFactory_iface, riid, out );
    if (IsEqualCLSID( clsid, &CLSID_DeskLink ))
        return IClassFactory_QueryInterface( &desk_factory.IClassFactory_iface, riid, out );
    *out = NULL;
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    return S_FALSE; /* the Start menus live in explorer */
}
