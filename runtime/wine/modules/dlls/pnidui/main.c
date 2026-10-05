/*
 * Network icon of the notification area (pnidui.dll)
 *
 * The notification area of the taskbar (stobject.dll) creates the network
 * connections tray object, CLSID_ConnectionTray, and starts it with
 * IOleCommandTarget::Exec(CGID_ShellServiceObject, OLECMDID_NEW), as on
 * Windows 7, where this object lives in pnidui.dll. The icon and its
 * flyout run on a thread of their own inside explorer (tray.c).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS
#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "objbase.h"
#include "wlanapi.h"
#include "docobj.h"
#include "shlguid.h"
#include "wine/debug.h"

#include "pnidui.h"

WINE_DEFAULT_DEBUG_CHANNEL(pnidui);

HINSTANCE pnidui_instance;

static const CLSID CLSID_ConnectionTray =
    { 0x7007accf, 0x3202, 0x11d1, { 0xaa, 0xd2, 0x00, 0x80, 0x5f, 0xc1, 0x27, 0x0e } };

static LONG object_count;

struct connection_tray
{
    IOleCommandTarget IOleCommandTarget_iface;
    LONG refcount;
};

static struct connection_tray *impl_from_IOleCommandTarget( IOleCommandTarget *iface )
{
    return CONTAINING_RECORD( iface, struct connection_tray, IOleCommandTarget_iface );
}

static HRESULT WINAPI tray_QueryInterface( IOleCommandTarget *iface, REFIID riid, void **out )
{
    if (IsEqualIID( riid, &IID_IUnknown ) || IsEqualIID( riid, &IID_IOleCommandTarget ))
    {
        *out = iface;
        IOleCommandTarget_AddRef( iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI tray_AddRef( IOleCommandTarget *iface )
{
    return InterlockedIncrement( &impl_from_IOleCommandTarget( iface )->refcount );
}

static ULONG WINAPI tray_Release( IOleCommandTarget *iface )
{
    struct connection_tray *tray = impl_from_IOleCommandTarget( iface );
    ULONG ref = InterlockedDecrement( &tray->refcount );

    if (!ref)
    {
        free( tray );
        InterlockedDecrement( &object_count );
    }
    return ref;
}

static HRESULT WINAPI tray_QueryStatus( IOleCommandTarget *iface, const GUID *group, ULONG count,
                                        OLECMD *cmds, OLECMDTEXT *text )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI tray_Exec( IOleCommandTarget *iface, const GUID *group, DWORD id, DWORD option,
                                 VARIANT *in, VARIANT *out )
{
    TRACE( "%s %lu\n", debugstr_guid( group ), id );

    if (!group || !IsEqualGUID( group, &CGID_ShellServiceObject )) return OLECMDERR_E_UNKNOWNGROUP;
    switch (id)
    {
    case OLECMDID_NEW:
        tray_start();
        return S_OK;
    case OLECMDID_SAVE:
        tray_stop();
        return S_OK;
    }
    return S_OK;
}

static const IOleCommandTargetVtbl tray_vtbl =
{
    tray_QueryInterface,
    tray_AddRef,
    tray_Release,
    tray_QueryStatus,
    tray_Exec,
};

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
    struct connection_tray *tray;
    HRESULT hr;

    *out = NULL;
    if (outer) return CLASS_E_NOAGGREGATION;
    if (!(tray = calloc( 1, sizeof(*tray) ))) return E_OUTOFMEMORY;
    tray->IOleCommandTarget_iface.lpVtbl = &tray_vtbl;
    tray->refcount = 1;
    InterlockedIncrement( &object_count );
    hr = IOleCommandTarget_QueryInterface( &tray->IOleCommandTarget_iface, riid, out );
    IOleCommandTarget_Release( &tray->IOleCommandTarget_iface );
    return hr;
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

HRESULT WINAPI DllGetClassObject( REFCLSID clsid, REFIID riid, void **out )
{
    if (IsEqualCLSID( clsid, &CLSID_ConnectionTray )) return IClassFactory_QueryInterface( &factory, riid, out );
    *out = NULL;
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    return object_count ? S_FALSE : S_OK;
}

BOOL WINAPI DllMain( HINSTANCE instance, DWORD reason, void *reserved )
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        pnidui_instance = instance;
        DisableThreadLibraryCalls( instance );
    }
    return TRUE;
}
