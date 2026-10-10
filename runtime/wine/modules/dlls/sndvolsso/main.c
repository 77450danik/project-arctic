/*
 * Volume icon of the notification area (sndvolsso.dll)
 *
 * In Windows the volume icon and its flyout are a shell service object of
 * explorer, SndVolSSO. Here the notification area (stobject.dll) creates it
 * for its volume service and starts it with
 * IOleCommandTarget::Exec(CGID_ShellServiceObject, OLECMDID_NEW), as it
 * starts the network icon of pnidui.dll. The icon and the flyout of Windows
 * 10 run on a thread of their own inside explorer (tray.c, flyout.c).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS

#include "sndvolsso.h"
#include "objbase.h"
#include "docobj.h"
#include "shlguid.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(sndvolsso);

HINSTANCE sndvolsso_instance;

/* {A6B0E3C1-5F2D-4B8A-9C7E-31D4F0A2B6E8} */
static const CLSID CLSID_SndVolSSO =
    { 0xa6b0e3c1, 0x5f2d, 0x4b8a, { 0x9c, 0x7e, 0x31, 0xd4, 0xf0, 0xa2, 0xb6, 0xe8 } };

static LONG object_count;

struct volume_tray
{
    IOleCommandTarget IOleCommandTarget_iface;
    LONG refcount;
};

WCHAR *load_string( UINT id )
{
    static WCHAR buffers[8][256];
    static int next;
    WCHAR *buf = buffers[next++ % ARRAY_SIZE(buffers)];

    if (!LoadStringW( sndvolsso_instance, id, buf, ARRAY_SIZE(buffers[0]) )) buf[0] = 0;
    return buf;
}

void shell_look( HWND hwnd )
{
    static void (WINAPI *apply)( HWND );
    static BOOL looked;

    if (!looked)
    {
        HMODULE module = LoadLibraryW( L"startui.dll" );
        if (module) apply = (void *)GetProcAddress( module, "ArcticApplyShellLook" );
        looked = TRUE;
    }
    /* without it the flyout keeps the acrylic it was made with */
    if (apply) apply( hwnd );
}

static struct volume_tray *impl_from_IOleCommandTarget( IOleCommandTarget *iface )
{
    return CONTAINING_RECORD( iface, struct volume_tray, IOleCommandTarget_iface );
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
    struct volume_tray *tray = impl_from_IOleCommandTarget( iface );
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
        listen_start();
        return S_OK;
    case OLECMDID_SAVE:
        listen_stop();
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
    struct volume_tray *tray;
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
    if (IsEqualCLSID( clsid, &CLSID_SndVolSSO )) return IClassFactory_QueryInterface( &factory, riid, out );
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
        sndvolsso_instance = instance;
        DisableThreadLibraryCalls( instance );
    }
    return TRUE;
}
