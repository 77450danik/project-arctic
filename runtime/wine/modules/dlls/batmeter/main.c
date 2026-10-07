/*
 * Battery icon of the notification area (batmeter.dll)
 *
 * In Windows 10 the battery icon is the power service of the notification
 * area and its flyout is BatteryFlyoutExperience. Here the notification area
 * (stobject.dll) creates this shell service object for its power service and
 * starts it with IOleCommandTarget::Exec(CGID_ShellServiceObject,
 * OLECMDID_NEW), as it starts the volume icon of sndvolsso.dll. The icon and
 * the flyout of Windows 10 run on a thread of their own inside explorer
 * (tray.c, flyout.c); what they show comes from powrprof.dll (power.c).
 * docs/power.md.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS

#include "batmeter.h"
#include "objbase.h"
#include "docobj.h"
#include "shlguid.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(batmeter);

HINSTANCE batmeter_instance;

/* {7B9C3E2A-41D8-4F6B-9A2E-6C1D5B8F0A34} */
static const CLSID CLSID_BatMeterSSO =
    { 0x7b9c3e2a, 0x41d8, 0x4f6b, { 0x9a, 0x2e, 0x6c, 0x1d, 0x5b, 0x8f, 0x0a, 0x34 } };

static LONG object_count;

struct battery_tray
{
    IOleCommandTarget IOleCommandTarget_iface;
    LONG refcount;
};

WCHAR *load_string( UINT id )
{
    static WCHAR buffers[8][256];
    static int next;
    WCHAR *buf = buffers[next++ % ARRAY_SIZE(buffers)];

    if (!LoadStringW( batmeter_instance, id, buf, ARRAY_SIZE(buffers[0]) )) buf[0] = 0;
    return buf;
}

/* Windows' own strings, with their inserts: %1!u!, %n... */
WCHAR *format_string( UINT id, ... )
{
    static WCHAR buffers[4][512];
    static int next;
    WCHAR *buf = buffers[next++ % ARRAY_SIZE(buffers)];
    va_list args;

    va_start( args, id );
    if (!FormatMessageW( FORMAT_MESSAGE_FROM_STRING, load_string( id ), 0, 0, buf, ARRAY_SIZE(buffers[0]), &args ))
        buf[0] = 0;
    va_end( args );
    return buf;
}

/* every size is given at 96 DPI and drawn at the DPI of the system: the
 * shell's, which is the primary monitor's scale when the session starts */
int px( int n )
{
    return MulDiv( n, GetDpiForSystem(), 96 );
}

/* the message font of the theme, at a height of our own */
HFONT shell_font( int height, int weight )
{
    NONCLIENTMETRICSW metrics = { sizeof(metrics) };

    SystemParametersInfoW( SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0 );
    metrics.lfMessageFont.lfHeight = -height;
    metrics.lfMessageFont.lfWeight = weight;
    metrics.lfMessageFont.lfQuality = CLEARTYPE_QUALITY;
    return CreateFontIndirectW( &metrics.lfMessageFont );
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

static struct battery_tray *impl_from_IOleCommandTarget( IOleCommandTarget *iface )
{
    return CONTAINING_RECORD( iface, struct battery_tray, IOleCommandTarget_iface );
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
    struct battery_tray *tray = impl_from_IOleCommandTarget( iface );
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
    struct battery_tray *tray;
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
    if (IsEqualCLSID( clsid, &CLSID_BatMeterSSO )) return IClassFactory_QueryInterface( &factory, riid, out );
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
        batmeter_instance = instance;
        DisableThreadLibraryCalls( instance );
    }
    return TRUE;
}
