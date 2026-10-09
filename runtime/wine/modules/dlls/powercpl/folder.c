/*
 * Power Options of the Control Panel: the shell folder
 *
 * As in Windows, Power Options ({025A5937-A6BE-4686-A844-36FE4BEC8B6D}) is an
 * item of the Control Panel's namespace that File Explorer browses into, and
 * its pages are its items: "Змінення настройок плану" of a plan, "Створення
 * плану живлення", "Настройки системи", and Arctic's pages of the batteries
 * and graphics cards and of the graphics settings. An item carries its page,
 * the plan it is about and whether the plan is being made.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS

#include <stdlib.h>

#include "powercpl.h"
#include "objbase.h"
#include "shlobj.h"
#include "shobjidl.h"
#include "shlwapi.h"
#include "shellapi.h"

#define PAGE_MAGIC 0x52574f50   /* POWR */

#include "pshpack1.h"
struct page_item
{
    WORD cb;
    DWORD magic;
    WORD page;
    WORD creating;
    GUID scheme;
};
#include "poppack.h"

const CLSID CLSID_PowerOptions = { 0x025a5937, 0xa6be, 0x4686, { 0xa8, 0x44, 0x36, 0xfe, 0x4b, 0xec, 0x8b, 0x6d } };

HRESULT view_create_for( enum page page, const GUID *scheme, BOOL creating, IShellFolder *folder,
                         const ITEMIDLIST *root, REFIID riid, void **out );

struct folder
{
    IShellFolder IShellFolder_iface;
    IPersistFolder2 IPersistFolder2_iface;
    LONG ref;
    enum page page;
    GUID scheme;
    BOOL creating;
    ITEMIDLIST *pidl;      /* where it is */
    ITEMIDLIST *root;      /* Power Options itself */
};

static const UINT page_names[PAGE_COUNT] =
{
    IDS_POWER_OPTIONS, IDS_PAGE_EDIT, IDS_PAGE_CREATE, IDS_PAGE_SYSTEM, IDS_BATTERY_TITLE, IDS_GRAPHICS_TITLE
};

const WCHAR *page_name( enum page page )
{
    return load_string( page_names[page < PAGE_COUNT ? page : 0] );
}

void *page_item_create( enum page page, const GUID *scheme, BOOL creating )
{
    struct page_item *item = CoTaskMemAlloc( sizeof(*item) + sizeof(WORD) );

    if (!item) return NULL;
    memset( item, 0, sizeof(*item) + sizeof(WORD) );
    item->cb = sizeof(*item);
    item->magic = PAGE_MAGIC;
    item->page = page;
    item->creating = creating;
    if (scheme) item->scheme = *scheme;
    return item;
}

static const struct page_item *as_page( const ITEMIDLIST *pidl )
{
    const struct page_item *item = (const struct page_item *)pidl;

    if (!pidl || pidl->mkid.cb < sizeof(*item) || item->magic != PAGE_MAGIC || item->page >= PAGE_COUNT) return NULL;
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
    }
    return ref;
}

static HRESULT WINAPI folder_ParseDisplayName( IShellFolder *iface, HWND hwnd, IBindCtx *ctx, LPWSTR name, ULONG *eaten,
                                               LPITEMIDLIST *pidl, ULONG *attributes )
{
    *pidl = NULL;
    return E_NOTIMPL;
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
    struct folder *folder = impl_from_IShellFolder( iface );
    const struct page_item *item = as_page( pidl );
    IPersistFolder *persist;
    ITEMIDLIST *where;
    HRESULT hr;

    *out = NULL;
    if (!item) return E_INVALIDARG;
    if (FAILED(hr = folder_create( item->page, &item->scheme, item->creating, &IID_IPersistFolder, (void **)&persist )))
        return hr;
    /* a page of a page is still an item of Power Options itself */
    where = ILCombine( folder->root ? folder->root : folder->pidl, (ITEMIDLIST *)item );
    if (where && ILGetNext( pidl ) && ILGetNext( pidl )->mkid.cb)
    {
        IPersistFolder_Release( persist );
        ILFree( where );
        return E_INVALIDARG;
    }
    IPersistFolder_Initialize( persist, where );
    {
        struct folder *child = impl_from_IPersistFolder2( (IPersistFolder2 *)persist );
        ILFree( child->root );
        child->root = ILClone( folder->root ? folder->root : folder->pidl );
    }
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
    diff = x->page != y->page ? (int)x->page - (int)y->page : memcmp( &x->scheme, &y->scheme, sizeof(GUID) );
    return MAKE_HRESULT( SEVERITY_SUCCESS, 0, (USHORT)(diff < 0 ? -1 : diff > 0 ? 1 : 0) );
}

static HRESULT WINAPI folder_CreateViewObject( IShellFolder *iface, HWND owner, REFIID riid, void **out )
{
    struct folder *folder = impl_from_IShellFolder( iface );

    *out = NULL;
    if (!IsEqualIID( riid, &IID_IShellView )) return E_NOINTERFACE;
    return view_create_for( folder->page, &folder->scheme, folder->creating, iface,
                            folder->root ? folder->root : folder->pidl, riid, out );
}

static HRESULT WINAPI folder_GetAttributesOf( IShellFolder *iface, UINT count, PCUITEMID_CHILD_ARRAY items, SFGAOF *flags )
{
    *flags &= SFGAO_FOLDER | SFGAO_BROWSABLE | SFGAO_HASSUBFOLDER;
    *flags &= ~SFGAO_HASSUBFOLDER;
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
    if (item->page == PAGE_EDIT && (flags & SHGDN_FORPARSING) && !(flags & SHGDN_FORADDRESSBAR))
    {
        WCHAR guid[40];
        StringFromGUID2( &item->scheme, guid, ARRAY_SIZE(guid) );
        swprintf( text, ARRAY_SIZE(text), L"pagePlanSettings%s", guid );
    }
    else lstrcpynW( text, page_name( item->page ), ARRAY_SIZE(text) );
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
    *clsid = CLSID_PowerOptions;
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

/* Power Options in File Explorer, as "control powercfg.cpl" opens it; with
 * "battery" or "graphics", one of Arctic's pages */
BOOL WINAPI ArcticOpenPowerOptions( const WCHAR *page )
{
    static const WCHAR path[] = L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\::{21EC2020-3AEA-1069-A2DD-08002B30309D}"
                                L"\\::{025A5937-A6BE-4686-A844-36FE4BEC8B6D}";
    SHELLEXECUTEINFOW info = { sizeof(info) };
    ITEMIDLIST *pidl;
    enum page which = PAGE_PLANS;
    BOOL ok;

    if (page && !wcsicmp( page, L"battery" )) which = PAGE_BATTERY;
    else if (page && !wcsicmp( page, L"graphics" )) which = PAGE_GRAPHICS;
    CoInitializeEx( NULL, COINIT_APARTMENTTHREADED );
    if (FAILED(SHParseDisplayName( path, NULL, &pidl, 0, NULL ))) return FALSE;
    /* File Explorer opened on the ID list of a page showed Power Options
     * itself: it opens there, and the window goes to the page as its links do */
    start_page_set( which );
    info.fMask = SEE_MASK_IDLIST | SEE_MASK_FLAG_NO_UI;
    info.lpIDList = pidl;
    info.nShow = SW_SHOWNORMAL;
    ok = ShellExecuteExW( &info );
    ILFree( pidl );
    return ok;
}

void start_page_set( enum page page )
{
    DWORD value[2] = { page, GetTickCount() };
    HKEY key;

    if (RegCreateKeyExW( HKEY_CURRENT_USER, START_PAGE_KEY, 0, NULL, REG_OPTION_VOLATILE, KEY_SET_VALUE, NULL, &key,
                         NULL ))
        return;
    if (page == PAGE_PLANS) RegDeleteValueW( key, L"StartPage" );
    else RegSetValueExW( key, L"StartPage", 0, REG_BINARY, (BYTE *)value, sizeof(value) );
    RegCloseKey( key );
}

/* asked for within the last few seconds: a window opened later by hand is not sent anywhere */
enum page start_page_take(void)
{
    DWORD value[2], size = sizeof(value);
    enum page page = PAGE_PLANS;
    HKEY key;

    if (RegOpenKeyExW( HKEY_CURRENT_USER, START_PAGE_KEY, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key )) return PAGE_PLANS;
    if (!RegQueryValueExW( key, L"StartPage", NULL, NULL, (BYTE *)value, &size ) && size == sizeof(value) &&
        GetTickCount() - value[1] < 15000 && value[0] < PAGE_COUNT)
        page = value[0];
    RegDeleteValueW( key, L"StartPage" );
    RegCloseKey( key );
    return page;
}

HRESULT folder_create( enum page page, const GUID *scheme, BOOL creating, REFIID riid, void **out )
{
    struct folder *folder;
    HRESULT hr;

    if (!(folder = calloc( 1, sizeof(*folder) ))) return E_OUTOFMEMORY;
    folder->IShellFolder_iface.lpVtbl = &folder_vtbl;
    folder->IPersistFolder2_iface.lpVtbl = &persist_vtbl;
    folder->ref = 1;
    folder->page = page;
    if (scheme) folder->scheme = *scheme;
    folder->creating = creating;
    hr = IShellFolder_QueryInterface( &folder->IShellFolder_iface, riid, out );
    IShellFolder_Release( &folder->IShellFolder_iface );
    return hr;
}
