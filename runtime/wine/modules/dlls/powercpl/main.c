/*
 * Power Options of the Control Panel (powercpl.dll)
 *
 * Windows 10's Power Options: a shell folder of the Control Panel that File
 * Explorer browses into (folder.c), its pages drawn as Windows draws them
 * (view.c, page_*.c), the Advanced settings dialog (advanced.c); and two
 * pages of Arctic's: the batteries and graphics cards, and Windows 10's
 * graphics settings, which there are in Settings. The schemes are
 * powrprof.dll's; the power policy in winlogon applies them. docs/power.md.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS

#include "powercpl.h"
#include "objbase.h"
#include "docobj.h"
#include "shlguid.h"

HINSTANCE powercpl_instance;

extern const CLSID CLSID_PowerOptions;

static LONG object_count;

WCHAR *load_string( UINT id )
{
    static WCHAR buffers[8][1024];
    static int next;
    WCHAR *buf = buffers[next++ % ARRAY_SIZE(buffers)];

    if (!LoadStringW( powercpl_instance, id, buf, ARRAY_SIZE(buffers[0]) )) buf[0] = 0;
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

/* every size is given at 96 DPI and drawn at the DPI of the system */
int px( int n )
{
    return MulDiv( n, GetDpiForSystem(), 96 );
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
    return folder_create( PAGE_PLANS, NULL, FALSE, riid, out );
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
    if (IsEqualCLSID( clsid, &CLSID_PowerOptions )) return IClassFactory_QueryInterface( &factory, riid, out );
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
        powercpl_instance = instance;
        DisableThreadLibraryCalls( instance );
    }
    return TRUE;
}
