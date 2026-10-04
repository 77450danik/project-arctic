/*
 * PROJECT:     Arctic
 * LICENSE:     LGPL-2.1-or-later
 * PURPOSE:     Class factories for the shell32 classes taken from Wine
 */

/* The Common Item Dialog of Wine's comdlg32 needs CLSID_ExplorerBrowser,
 * Chromium's downloads need CLSID_FileOperation; ReactOS has neither. Both
 * are C objects, so they get a plain class factory here instead of a place
 * in the ATL object map of shell32.cpp. */

#include <stdarg.h>

#define WIN32_NO_STATUS
#define _INC_WINDOWS
#define COBJMACROS

#include <windef.h>
#include <winbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <debughlp.h>

#include <wine/debug.h>

#include "arctic_classes.h"

WINE_DEFAULT_DEBUG_CHANNEL(shell);

typedef HRESULT (WINAPI *create_func)(IUnknown *outer, REFIID riid, void **ppv);

struct class_factory
{
    IClassFactory IClassFactory_iface;
    create_func create;
};

static inline struct class_factory *impl_from_IClassFactory(IClassFactory *iface)
{
    return CONTAINING_RECORD(iface, struct class_factory, IClassFactory_iface);
}

static HRESULT WINAPI factory_QueryInterface(IClassFactory *iface, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IClassFactory))
    {
        *ppv = iface;
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

/* the factories are static, like Wine's own */
static ULONG WINAPI factory_AddRef(IClassFactory *iface)
{
    return 2;
}

static ULONG WINAPI factory_Release(IClassFactory *iface)
{
    return 1;
}

static HRESULT WINAPI factory_CreateInstance(IClassFactory *iface, IUnknown *outer, REFIID riid, void **ppv)
{
    struct class_factory *factory = impl_from_IClassFactory(iface);

    if (!ppv)
        return E_POINTER;
    *ppv = NULL;
    if (outer)
        return CLASS_E_NOAGGREGATION;
    return factory->create(NULL, riid, ppv);
}

static HRESULT WINAPI factory_LockServer(IClassFactory *iface, BOOL lock)
{
    return S_OK;
}

static const IClassFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    factory_CreateInstance,
    factory_LockServer
};

static struct class_factory explorer_browser_factory = { { &factory_vtbl }, ExplorerBrowser_Constructor };
static struct class_factory file_operation_factory = { { &factory_vtbl }, IFileOperation_Constructor };

/* CLASS_E_CLASSNOTAVAILABLE means "not one of these", ATL's map comes next */
HRESULT Arctic_DllGetClassObject(REFCLSID rclsid, REFIID riid, void **ppv)
{
    struct class_factory *factory;

    if (IsEqualCLSID(rclsid, &CLSID_ExplorerBrowser))
        factory = &explorer_browser_factory;
    else if (IsEqualCLSID(rclsid, &CLSID_FileOperation))
        factory = &file_operation_factory;
    else
        return CLASS_E_CLASSNOTAVAILABLE;

    TRACE("%s\n", debugstr_guid(rclsid));
    return IClassFactory_QueryInterface(&factory->IClassFactory_iface, riid, ppv);
}
