/*
 * Sharing of folders, as Explorer shows it (ntshrui.dll)
 *
 * The Sharing page of a folder's and a drive's Properties and "Give access
 * to" of their menu: a shell extension, found by shell32 where Windows has
 * ntshrui.dll's (shellex\PropertySheetHandlers and ContextMenuHandlers of
 * Directory and Drive, runtime/registry/smbserver.reg). The page and its
 * dialogs are page.c, the permissions perms.c, the wizard wizard.c; the
 * shares themselves are files the host serves (shares.c, host/init
 * /arctic-smbd.c). docs/M5-network.md.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS

#include "ntshrui.h"
#include "shlobj.h"
#include "shlwapi.h"
#include "shellapi.h"
#include "initguid.h"

HINSTANCE ntshrui_instance;

/* Arctic's own: nothing looks this extension up by a class id of Windows */
DEFINE_GUID( CLSID_Sharing, 0xc4c92a37, 0x23b6, 0x415f, 0xa2, 0xd9, 0xf8, 0x36, 0x71, 0x6c, 0xc3, 0x33 );

enum { VERB_STOP, VERB_PEOPLE, VERB_COUNT };

struct sharing
{
    IShellExtInit IShellExtInit_iface;
    IShellPropSheetExt IShellPropSheetExt_iface;
    IContextMenu IContextMenu_iface;
    LONG ref;
    WCHAR folder[MAX_PATH];     /* "" when what was chosen is not a folder of this computer */
};

static inline struct sharing *impl_from_IShellExtInit( IShellExtInit *iface )
{
    return CONTAINING_RECORD( iface, struct sharing, IShellExtInit_iface );
}

static inline struct sharing *impl_from_IShellPropSheetExt( IShellPropSheetExt *iface )
{
    return CONTAINING_RECORD( iface, struct sharing, IShellPropSheetExt_iface );
}

static inline struct sharing *impl_from_IContextMenu( IContextMenu *iface )
{
    return CONTAINING_RECORD( iface, struct sharing, IContextMenu_iface );
}

static HRESULT WINAPI init_QueryInterface( IShellExtInit *iface, REFIID riid, void **out )
{
    struct sharing *sharing = impl_from_IShellExtInit( iface );

    if (IsEqualIID( riid, &IID_IUnknown ) || IsEqualIID( riid, &IID_IShellExtInit ))
        *out = &sharing->IShellExtInit_iface;
    else if (IsEqualIID( riid, &IID_IShellPropSheetExt ))
        *out = &sharing->IShellPropSheetExt_iface;
    else if (IsEqualIID( riid, &IID_IContextMenu ))
        *out = &sharing->IContextMenu_iface;
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG WINAPI init_AddRef( IShellExtInit *iface )
{
    return InterlockedIncrement( &impl_from_IShellExtInit( iface )->ref );
}

static ULONG WINAPI init_Release( IShellExtInit *iface )
{
    struct sharing *sharing = impl_from_IShellExtInit( iface );
    ULONG ref = InterlockedDecrement( &sharing->ref );

    if (!ref) free( sharing );
    return ref;
}

/* one folder or one drive of this computer: a file, several things, a folder
 * of the network are not shared from here */
static HRESULT WINAPI init_Initialize( IShellExtInit *iface, LPCITEMIDLIST pidl, IDataObject *data, HKEY key )
{
    struct sharing *sharing = impl_from_IShellExtInit( iface );
    FORMATETC format = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    WCHAR root[4] = L"C:\\";
    STGMEDIUM medium;
    DWORD attributes;

    sharing->folder[0] = 0;
    if (!data || FAILED(IDataObject_GetData( data, &format, &medium ))) return S_OK;
    if (DragQueryFileW( medium.hGlobal, ~0u, NULL, 0 ) == 1)
        DragQueryFileW( medium.hGlobal, 0, sharing->folder, ARRAY_SIZE(sharing->folder) );
    ReleaseStgMedium( &medium );

    attributes = GetFileAttributesW( sharing->folder );
    root[0] = sharing->folder[0];
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        sharing->folder[1] != ':' || GetDriveTypeW( root ) == DRIVE_REMOTE)
        sharing->folder[0] = 0;
    return S_OK;
}

static const IShellExtInitVtbl init_vtbl = { init_QueryInterface, init_AddRef, init_Release, init_Initialize };

static HRESULT WINAPI page_QueryInterface( IShellPropSheetExt *iface, REFIID riid, void **out )
{
    return init_QueryInterface( &impl_from_IShellPropSheetExt( iface )->IShellExtInit_iface, riid, out );
}

static ULONG WINAPI page_AddRef( IShellPropSheetExt *iface )
{
    return init_AddRef( &impl_from_IShellPropSheetExt( iface )->IShellExtInit_iface );
}

static ULONG WINAPI page_Release( IShellPropSheetExt *iface )
{
    return init_Release( &impl_from_IShellPropSheetExt( iface )->IShellExtInit_iface );
}

static HRESULT WINAPI page_AddPages( IShellPropSheetExt *iface, LPFNSVADDPROPSHEETPAGE add, LPARAM param )
{
    struct sharing *sharing = impl_from_IShellPropSheetExt( iface );
    HPROPSHEETPAGE page;

    if (!sharing->folder[0]) return S_OK;
    if (!(page = sharing_page_create( sharing->folder ))) return E_OUTOFMEMORY;
    if (!add( page, param )) DestroyPropertySheetPage( page );
    return S_OK;
}

static HRESULT WINAPI page_ReplacePage( IShellPropSheetExt *iface, EXPPS id, LPFNSVADDPROPSHEETPAGE replace,
                                        LPARAM param )
{
    return E_NOTIMPL;
}

static const IShellPropSheetExtVtbl page_vtbl =
{
    page_QueryInterface, page_AddRef, page_Release, page_AddPages, page_ReplacePage
};

static HRESULT WINAPI menu_QueryInterface( IContextMenu *iface, REFIID riid, void **out )
{
    return init_QueryInterface( &impl_from_IContextMenu( iface )->IShellExtInit_iface, riid, out );
}

static ULONG WINAPI menu_AddRef( IContextMenu *iface )
{
    return init_AddRef( &impl_from_IContextMenu( iface )->IShellExtInit_iface );
}

static ULONG WINAPI menu_Release( IContextMenu *iface )
{
    return init_Release( &impl_from_IContextMenu( iface )->IShellExtInit_iface );
}

/* "Give access to": Remove access, Specific people... */
static HRESULT WINAPI menu_QueryContextMenu( IContextMenu *iface, HMENU menu, UINT index, UINT first, UINT last,
                                             UINT flags )
{
    struct sharing *sharing = impl_from_IContextMenu( iface );
    MENUITEMINFOW item = { sizeof(item), MIIM_SUBMENU | MIIM_STRING | MIIM_ID };
    struct share shares[1];
    HMENU sub;

    if (!sharing->folder[0] || (flags & (CMF_DEFAULTONLY | CMF_VERBSONLY)) || PathIsRootW( sharing->folder ) ||
        first + VERB_COUNT > last)
        return MAKE_HRESULT( SEVERITY_SUCCESS, 0, 0 );
    if (!(sub = CreatePopupMenu())) return E_OUTOFMEMORY;
    AppendMenuW( sub, MF_STRING | (shares_of_folder( sharing->folder, shares, 1 ) ? 0 : MF_GRAYED), first + VERB_STOP,
                 load_text( IDS_REMOVE_ACCESS ) );
    AppendMenuW( sub, MF_STRING, first + VERB_PEOPLE, load_text( IDS_SPECIFIC_PEOPLE ) );
    item.wID = first + VERB_COUNT;
    item.hSubMenu = sub;
    item.dwTypeData = load_text( IDS_GIVE_ACCESS );
    InsertMenuItemW( menu, index, TRUE, &item );
    return MAKE_HRESULT( SEVERITY_SUCCESS, 0, VERB_COUNT + 1 );
}

static HRESULT WINAPI menu_InvokeCommand( IContextMenu *iface, CMINVOKECOMMANDINFO *info )
{
    struct sharing *sharing = impl_from_IContextMenu( iface );

    if (!sharing->folder[0] || !IS_INTRESOURCE( info->lpVerb )) return E_INVALIDARG;
    switch (LOWORD( (UINT_PTR)info->lpVerb ))
    {
    case VERB_STOP:
        stop_sharing( info->hwnd, sharing->folder );
        return S_OK;
    case VERB_PEOPLE:
        sharing_wizard( info->hwnd, sharing->folder );
        return S_OK;
    }
    return E_INVALIDARG;
}

static HRESULT WINAPI menu_GetCommandString( IContextMenu *iface, UINT_PTR id, UINT type, UINT *reserved, char *name,
                                             UINT count )
{
    return E_NOTIMPL;
}

static const IContextMenuVtbl menu_vtbl =
{
    menu_QueryInterface, menu_AddRef, menu_Release, menu_QueryContextMenu, menu_InvokeCommand, menu_GetCommandString
};

/**********************************************************************
 *          The class factory
 */

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
    struct sharing *sharing;
    HRESULT hr;

    *out = NULL;
    if (outer) return CLASS_E_NOAGGREGATION;
    if (!(sharing = calloc( 1, sizeof(*sharing) ))) return E_OUTOFMEMORY;
    sharing->IShellExtInit_iface.lpVtbl = &init_vtbl;
    sharing->IShellPropSheetExt_iface.lpVtbl = &page_vtbl;
    sharing->IContextMenu_iface.lpVtbl = &menu_vtbl;
    sharing->ref = 1;
    hr = init_QueryInterface( &sharing->IShellExtInit_iface, riid, out );
    init_Release( &sharing->IShellExtInit_iface );
    return hr;
}

static HRESULT WINAPI factory_LockServer( IClassFactory *iface, BOOL lock )
{
    return S_OK;
}

static const IClassFactoryVtbl factory_vtbl =
{
    factory_QueryInterface, factory_AddRef, factory_Release, factory_CreateInstance, factory_LockServer
};

static IClassFactory factory = { &factory_vtbl };

HRESULT WINAPI DllGetClassObject( REFCLSID clsid, REFIID riid, void **out )
{
    if (IsEqualCLSID( clsid, &CLSID_Sharing )) return IClassFactory_QueryInterface( &factory, riid, out );
    *out = NULL;
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    return S_FALSE; /* a page or a wizard of it may be open */
}

BOOL WINAPI DllMain( HINSTANCE instance, DWORD reason, void *reserved )
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        ntshrui_instance = instance;
        DisableThreadLibraryCalls( instance );
    }
    return TRUE;
}
