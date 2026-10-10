/*
 * A page of the Control Panel: the shell folder
 *
 * As in Windows, an item of the Control Panel's namespace that Explorer
 * browses into (System {BB06C0E4-…}, Network and Sharing Center
 * {8E908FC9-…}); its other pages are its items, so Back and the address bar
 * follow them. An item carries its page and what the page is about (a
 * connection, say).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS

#include <stdlib.h>

#include "cpanel.h"
#include "shobjidl.h"
#include "shlwapi.h"
#include "shellapi.h"

#define PAGE_MAGIC 0x45474150   /* PAGE */

#include "pshpack1.h"
struct page_item
{
    WORD cb;
    DWORD magic;
    CLSID owner;
    WORD page;
    DWORD param;
};
#include "poppack.h"

HRESULT cp_view_create( UINT page, DWORD param, IShellFolder *folder, const ITEMIDLIST *root, REFIID riid, void **out );

struct folder
{
    IShellFolder IShellFolder_iface;
    IPersistFolder2 IPersistFolder2_iface;
    LONG ref;
    UINT page;
    DWORD param;
    ITEMIDLIST *pidl;      /* where it is */
    ITEMIDLIST *root;      /* the item itself */
};

static LONG object_count;

void *cp_page_item( UINT page, DWORD param );

void *cp_page_item( UINT page, DWORD param )
{
    struct page_item *item = CoTaskMemAlloc( sizeof(*item) + sizeof(WORD) );

    if (!item) return NULL;
    memset( item, 0, sizeof(*item) + sizeof(WORD) );
    item->cb = sizeof(*item);
    item->magic = PAGE_MAGIC;
    item->owner = cp_clsid;
    item->page = page;
    item->param = param;
    return item;
}

static const struct page_item *as_page( const ITEMIDLIST *pidl )
{
    const struct page_item *item = (const struct page_item *)pidl;

    if (!pidl || pidl->mkid.cb < sizeof(*item) || item->magic != PAGE_MAGIC ||
        !IsEqualCLSID( &item->owner, &cp_clsid ) || item->page >= cp_page_count)
        return NULL;
    return item;
}

static inline struct folder *impl_from_IShellFolder( IShellFolder *iface )
{
    return CONTAINING_RECORD( iface, struct folder, IShellFolder_iface );
}

static inline struct folder *impl_from_IPersistFolder2( IPersistFolder2 *iface )
{
    return CONTAINING_RECORD( iface, struct folder, IPersistFolder2_iface );
}

/**********************************************************************
 *          An enumerator with nothing in it: a page has no items to list
 */

struct empty_enum
{
    IEnumIDList IEnumIDList_iface;
    LONG ref;
};

static HRESULT WINAPI enum_QueryInterface( IEnumIDList *iface, REFIID riid, void **out )
{
    if (IsEqualIID( riid, &IID_IUnknown ) || IsEqualIID( riid, &IID_IEnumIDList ))
    {
        *out = iface;
        IEnumIDList_AddRef( iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI enum_AddRef( IEnumIDList *iface )
{
    return InterlockedIncrement( &CONTAINING_RECORD( iface, struct empty_enum, IEnumIDList_iface )->ref );
}

static ULONG WINAPI enum_Release( IEnumIDList *iface )
{
    struct empty_enum *e = CONTAINING_RECORD( iface, struct empty_enum, IEnumIDList_iface );
    ULONG ref = InterlockedDecrement( &e->ref );

    if (!ref) free( e );
    return ref;
}

static HRESULT WINAPI enum_Next( IEnumIDList *iface, ULONG count, PITEMID_CHILD *items, ULONG *fetched )
{
    if (fetched) *fetched = 0;
    return S_FALSE;
}

static HRESULT WINAPI enum_Skip( IEnumIDList *iface, ULONG count )
{
    return S_FALSE;
}

static HRESULT WINAPI enum_Reset( IEnumIDList *iface )
{
    return S_OK;
}

static HRESULT WINAPI enum_Clone( IEnumIDList *iface, IEnumIDList **out )
{
    *out = NULL;
    return E_NOTIMPL;
}

static const IEnumIDListVtbl enum_vtbl =
{
    enum_QueryInterface, enum_AddRef, enum_Release, enum_Next, enum_Skip, enum_Reset, enum_Clone
};

/**********************************************************************
 *          IShellFolder
 */

static HRESULT WINAPI folder_QueryInterface( IShellFolder *iface, REFIID riid, void **out )
{
    struct folder *folder = impl_from_IShellFolder( iface );

    if (IsEqualIID( riid, &IID_IUnknown ) || IsEqualIID( riid, &IID_IShellFolder ))
        *out = &folder->IShellFolder_iface;
    else if (IsEqualIID( riid, &IID_IPersist ) || IsEqualIID( riid, &IID_IPersistFolder ) ||
             IsEqualIID( riid, &IID_IPersistFolder2 ))
        *out = &folder->IPersistFolder2_iface;
    else
    {
        *out = NULL;
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG WINAPI folder_AddRef( IShellFolder *iface )
{
    return InterlockedIncrement( &impl_from_IShellFolder( iface )->ref );
}

static ULONG WINAPI folder_Release( IShellFolder *iface )
{
    struct folder *folder = impl_from_IShellFolder( iface );
    ULONG ref = InterlockedDecrement( &folder->ref );

    if (!ref)
    {
        ILFree( folder->pidl );
        ILFree( folder->root );
        free( folder );
        InterlockedDecrement( &object_count );
    }
    return ref;
}

static HRESULT WINAPI folder_ParseDisplayName( IShellFolder *iface, HWND hwnd, IBindCtx *ctx, LPWSTR name, ULONG *eaten,
                                               LPITEMIDLIST *pidl, ULONG *attributes )
{
    *pidl = NULL;
    /* the pages by their canonical names, as the address bar gives them */
    for (UINT i = 1; i < cp_page_count; i++)
    {
        if (cp_pages[i].canonical && !wcsicmp( name, cp_pages[i].canonical ))
        {
            if (!(*pidl = cp_page_item( i, 0 ))) return E_OUTOFMEMORY;
            if (eaten) *eaten = wcslen( name );
            if (attributes) *attributes &= SFGAO_FOLDER | SFGAO_BROWSABLE;
            return S_OK;
        }
    }
    return E_INVALIDARG;
}

static HRESULT WINAPI folder_EnumObjects( IShellFolder *iface, HWND hwnd, SHCONTF flags, IEnumIDList **out )
{
    struct empty_enum *e;

    if (!(e = calloc( 1, sizeof(*e) ))) return E_OUTOFMEMORY;
    e->IEnumIDList_iface.lpVtbl = &enum_vtbl;
    e->ref = 1;
    *out = &e->IEnumIDList_iface;
    return S_OK;
}

static HRESULT WINAPI folder_BindToObject( IShellFolder *iface, PCUIDLIST_RELATIVE pidl, IBindCtx *ctx, REFIID riid,
                                           void **out )
{
    struct folder *folder = impl_from_IShellFolder( iface ), *child;
    const struct page_item *item = as_page( pidl );
    IPersistFolder *persist;
    ITEMIDLIST *where;
    HRESULT hr;

    *out = NULL;
    if (!item) return E_INVALIDARG;
    if (ILGetNext( pidl ) && ILGetNext( pidl )->mkid.cb) return E_INVALIDARG;
    if (FAILED(hr = cp_folder_create( item->page, item->param, &IID_IPersistFolder, (void **)&persist ))) return hr;
    /* a page of a page is still an item of the item itself */
    where = ILCombine( folder->root ? folder->root : folder->pidl, (ITEMIDLIST *)item );
    IPersistFolder_Initialize( persist, where );
    child = impl_from_IPersistFolder2( (IPersistFolder2 *)persist );
    ILFree( child->root );
    child->root = ILClone( folder->root ? folder->root : folder->pidl );
    ILFree( where );
    hr = IPersistFolder_QueryInterface( persist, riid, out );
    IPersistFolder_Release( persist );
    return hr;
}

static HRESULT WINAPI folder_BindToStorage( IShellFolder *iface, PCUIDLIST_RELATIVE pidl, IBindCtx *ctx, REFIID riid,
                                            void **out )
{
    *out = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI folder_CompareIDs( IShellFolder *iface, LPARAM param, PCUIDLIST_RELATIVE a, PCUIDLIST_RELATIVE b )
{
    const struct page_item *x = as_page( a ), *y = as_page( b );
    int diff;

    if (!x || !y) return E_INVALIDARG;
    diff = x->page != y->page ? (int)x->page - (int)y->page : (int)(x->param - y->param);
    return MAKE_HRESULT( SEVERITY_SUCCESS, 0, (USHORT)(diff < 0 ? -1 : diff > 0 ? 1 : 0) );
}

static HRESULT WINAPI folder_CreateViewObject( IShellFolder *iface, HWND owner, REFIID riid, void **out )
{
    struct folder *folder = impl_from_IShellFolder( iface );

    *out = NULL;
    if (!IsEqualIID( riid, &IID_IShellView )) return E_NOINTERFACE;
    return cp_view_create( folder->page, folder->param, iface, folder->root ? folder->root : folder->pidl, riid, out );
}

static HRESULT WINAPI folder_GetAttributesOf( IShellFolder *iface, UINT count, PCUITEMID_CHILD_ARRAY items, SFGAOF *flags )
{
    *flags &= SFGAO_FOLDER | SFGAO_BROWSABLE;
    *flags |= SFGAO_FOLDER | SFGAO_BROWSABLE;
    return S_OK;
}

static HRESULT WINAPI folder_GetUIObjectOf( IShellFolder *iface, HWND owner, UINT count, PCUITEMID_CHILD_ARRAY items,
                                            REFIID riid, UINT *reserved, void **out )
{
    *out = NULL;
    return E_NOINTERFACE;
}

static HRESULT WINAPI folder_GetDisplayNameOf( IShellFolder *iface, PCUITEMID_CHILD pidl, SHGDNF flags, STRRET *name )
{
    const struct page_item *item = as_page( pidl );
    WCHAR text[256];

    if (!item) return E_INVALIDARG;
    if ((flags & SHGDN_FORPARSING) && !(flags & SHGDN_FORADDRESSBAR) && cp_pages[item->page].canonical)
        lstrcpynW( text, cp_pages[item->page].canonical, ARRAY_SIZE(text) );
    else
        lstrcpynW( text, load_string( cp_pages[item->page].name ), ARRAY_SIZE(text) );
    name->uType = STRRET_WSTR;
    return SHStrDupW( text, &name->pOleStr );
}

static HRESULT WINAPI folder_SetNameOf( IShellFolder *iface, HWND owner, PCUITEMID_CHILD pidl, LPCOLESTR name,
                                        SHGDNF flags, PITEMID_CHILD *out )
{
    return E_NOTIMPL;
}

static const IShellFolderVtbl folder_vtbl =
{
    folder_QueryInterface,
    folder_AddRef,
    folder_Release,
    folder_ParseDisplayName,
    folder_EnumObjects,
    folder_BindToObject,
    folder_BindToStorage,
    folder_CompareIDs,
    folder_CreateViewObject,
    folder_GetAttributesOf,
    folder_GetUIObjectOf,
    folder_GetDisplayNameOf,
    folder_SetNameOf,
};

/**********************************************************************
 *          IPersistFolder2
 */

static HRESULT WINAPI persist_QueryInterface( IPersistFolder2 *iface, REFIID riid, void **out )
{
    return folder_QueryInterface( &impl_from_IPersistFolder2( iface )->IShellFolder_iface, riid, out );
}

static ULONG WINAPI persist_AddRef( IPersistFolder2 *iface )
{
    return folder_AddRef( &impl_from_IPersistFolder2( iface )->IShellFolder_iface );
}

static ULONG WINAPI persist_Release( IPersistFolder2 *iface )
{
    return folder_Release( &impl_from_IPersistFolder2( iface )->IShellFolder_iface );
}

static HRESULT WINAPI persist_GetClassID( IPersistFolder2 *iface, CLSID *clsid )
{
    *clsid = cp_clsid;
    return S_OK;
}

static HRESULT WINAPI persist_Initialize( IPersistFolder2 *iface, PCIDLIST_ABSOLUTE pidl )
{
    struct folder *folder = impl_from_IPersistFolder2( iface );

    ILFree( folder->pidl );
    folder->pidl = ILClone( pidl );
    return S_OK;
}

static HRESULT WINAPI persist_GetCurFolder( IPersistFolder2 *iface, PIDLIST_ABSOLUTE *pidl )
{
    struct folder *folder = impl_from_IPersistFolder2( iface );

    *pidl = ILClone( folder->pidl );
    return *pidl ? S_OK : S_FALSE;
}

static const IPersistFolder2Vtbl persist_vtbl =
{
    persist_QueryInterface,
    persist_AddRef,
    persist_Release,
    persist_GetClassID,
    persist_Initialize,
    persist_GetCurFolder,
};

HRESULT cp_folder_create( UINT page, DWORD param, REFIID riid, void **out )
{
    struct folder *folder;
    HRESULT hr;

    if (!(folder = calloc( 1, sizeof(*folder) ))) return E_OUTOFMEMORY;
    folder->IShellFolder_iface.lpVtbl = &folder_vtbl;
    folder->IPersistFolder2_iface.lpVtbl = &persist_vtbl;
    folder->ref = 1;
    folder->page = page;
    folder->param = param;
    InterlockedIncrement( &object_count );
    hr = IShellFolder_QueryInterface( &folder->IShellFolder_iface, riid, out );
    IShellFolder_Release( &folder->IShellFolder_iface );
    return hr;
}

/**********************************************************************
 *          Opening it
 */

/* where the window that opens goes, once (a window opened on the ID list of a
 * page showed the item itself): the item's page 0 takes it on its first build */
static void start_page_key( WCHAR *key, size_t count )
{
    swprintf( key, count, L"Software\\Arctic\\ControlPanel\\%s", cp_class_name );
}

static void start_page_set( UINT page, DWORD param )
{
    DWORD value[3] = { page, param, GetTickCount() };
    WCHAR name[128];
    HKEY key;

    start_page_key( name, ARRAY_SIZE(name) );
    if (RegCreateKeyExW( HKEY_CURRENT_USER, name, 0, NULL, REG_OPTION_VOLATILE, KEY_SET_VALUE, NULL, &key, NULL ))
        return;
    if (!page) RegDeleteValueW( key, L"StartPage" );
    else RegSetValueExW( key, L"StartPage", 0, REG_BINARY, (BYTE *)value, sizeof(value) );
    RegCloseKey( key );
}

BOOL cp_start_page_take( UINT *page, DWORD *param );

/* asked for within the last few seconds: a window opened later by hand is not sent anywhere */
BOOL cp_start_page_take( UINT *page, DWORD *param )
{
    DWORD value[3], size = sizeof(value);
    WCHAR name[128];
    BOOL ret = FALSE;
    HKEY key;

    start_page_key( name, ARRAY_SIZE(name) );
    if (RegOpenKeyExW( HKEY_CURRENT_USER, name, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key )) return FALSE;
    if (!RegQueryValueExW( key, L"StartPage", NULL, NULL, (BYTE *)value, &size ) && size == sizeof(value) &&
        GetTickCount() - value[2] < 15000 && value[0] && value[0] < cp_page_count)
    {
        *page = value[0];
        *param = value[1];
        ret = TRUE;
    }
    RegDeleteValueW( key, L"StartPage" );
    RegCloseKey( key );
    return ret;
}

BOOL cp_open( UINT page, DWORD param )
{
    SHELLEXECUTEINFOW info = { sizeof(info) };
    WCHAR path[200], guid[40];
    ITEMIDLIST *pidl;
    BOOL ok;

    StringFromGUID2( &cp_clsid, guid, ARRAY_SIZE(guid) );
    swprintf( path, ARRAY_SIZE(path), L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\::{21EC2020-3AEA-1069-A2DD-08002B30309D}"
              L"\\::%s", guid );
    CoInitializeEx( NULL, COINIT_APARTMENTTHREADED );
    if (FAILED(SHParseDisplayName( path, NULL, &pidl, 0, NULL ))) return FALSE;
    start_page_set( page, param );
    info.fMask = SEE_MASK_IDLIST | SEE_MASK_FLAG_NO_UI;
    info.lpIDList = pidl;
    info.nShow = SW_SHOWNORMAL;
    ok = ShellExecuteExW( &info );
    ILFree( pidl );
    return ok;
}

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
    *out = NULL;
    if (outer) return CLASS_E_NOAGGREGATION;
    return cp_folder_create( 0, 0, riid, out );
}

static HRESULT WINAPI factory_LockServer( IClassFactory *iface, BOOL lock )
{
    if (lock) InterlockedIncrement( &object_count );
    else InterlockedDecrement( &object_count );
    return S_OK;
}

static const IClassFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    factory_CreateInstance,
    factory_LockServer,
};

static IClassFactory factory = { &factory_vtbl };

HRESULT cp_class_object( REFCLSID clsid, REFIID riid, void **out )
{
    if (IsEqualCLSID( clsid, &cp_clsid )) return IClassFactory_QueryInterface( &factory, riid, out );
    *out = NULL;
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT cp_can_unload(void)
{
    return object_count ? S_FALSE : S_OK;
}
