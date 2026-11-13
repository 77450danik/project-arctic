/*
 * Windows.System.Launcher
 *
 * Programs open a link or a file with Launcher.LaunchUriAsync and
 * LaunchFileAsync, as Windows 10 apps do; Spotify's "Log in" opens the
 * browser this way. Here both go to ShellExecuteEx, so the default browser
 * (UrlAssociations UserChoice) or the file's program opens it, as it does in
 * Windows. The operation completes with TRUE once the program is started.
 *
 * Copyright 2026 Project Arctic
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "initguid.h"
#include "private.h"
#include "objbase.h"
#include "shellapi.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(launcher);

/* starts what ShellExecuteEx opens for target: a URI or a path */
static BOOL launch( const WCHAR *target )
{
    SHELLEXECUTEINFOW info = { sizeof(info) };
    HRESULT init = CoInitializeEx( NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE );
    BOOL ret;

    info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = NULL;     /* the default verb, as Windows' launcher takes */
    info.lpFile = target;
    info.nShow = SW_SHOWNORMAL;
    ret = ShellExecuteExW( &info );
    TRACE( "%s: %s, error %lu\n", debugstr_w( target ), ret ? "started" : "failed", ret ? 0 : GetLastError() );
    if (SUCCEEDED(init)) CoUninitialize();
    return ret;
}

static HRESULT launch_result( PROPVARIANT *result, BOOL launched )
{
    result->vt = VT_BOOL;
    result->boolVal = launched ? TRUE : FALSE;   /* GetResults hands it on as a WinRT boolean */
    return S_OK;
}

/* the async's work: param is the Uri */
static HRESULT launch_uri_async( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    IUriRuntimeClass *uri;
    HSTRING string = NULL;
    BOOL launched = FALSE;

    if (!called_async) return STATUS_PENDING;   /* on the thread pool, not in the caller */

    if (SUCCEEDED(IUnknown_QueryInterface( param, &IID_IUriRuntimeClass, (void **)&uri )))
    {
        if (FAILED(IUriRuntimeClass_get_AbsoluteUri( uri, &string )) || !WindowsGetStringLen( string ))
        {
            WindowsDeleteString( string );
            string = NULL;
            IUriRuntimeClass_get_RawUri( uri, &string );
        }
        IUriRuntimeClass_Release( uri );
    }
    if (WindowsGetStringLen( string ))
    {
        WCHAR *target;
        UINT32 len;
        const WCHAR *buffer = WindowsGetStringRawBuffer( string, &len );

        if ((target = malloc( (len + 1) * sizeof(WCHAR) )))
        {
            memcpy( target, buffer, len * sizeof(WCHAR) );
            target[len] = 0;
            launched = launch( target );
            free( target );
        }
    }
    WindowsDeleteString( string );
    return launch_result( result, launched );
}

/* the async's work: param is the IStorageFile */
static HRESULT launch_file_async( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    IStorageItem *item;
    HSTRING path = NULL;
    BOOL launched = FALSE;

    if (!called_async) return STATUS_PENDING;

    if (SUCCEEDED(IUnknown_QueryInterface( param, &IID_IStorageItem, (void **)&item )))
    {
        IStorageItem_get_Path( item, &path );
        IStorageItem_Release( item );
    }
    if (WindowsGetStringLen( path ))
    {
        WCHAR *target;
        UINT32 len;
        const WCHAR *buffer = WindowsGetStringRawBuffer( path, &len );

        if ((target = malloc( (len + 1) * sizeof(WCHAR) )))
        {
            memcpy( target, buffer, len * sizeof(WCHAR) );
            target[len] = 0;
            launched = launch( target );
            free( target );
        }
    }
    WindowsDeleteString( path );
    return launch_result( result, launched );
}

struct launcher_statics
{
    IActivationFactory IActivationFactory_iface;
    ILauncherStatics ILauncherStatics_iface;
};

static inline struct launcher_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct launcher_statics, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct launcher_statics *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        *out = &impl->IActivationFactory_iface;
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_ILauncherStatics ))
    {
        *out = &impl->ILauncherStatics_iface;
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

/* a static object, alive as long as the module */
static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    return 2;
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    return 1;
}

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_System_Launcher,
                                wcslen( RuntimeClass_Windows_System_Launcher ), class_name );
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    /* Launcher has only statics */
    *instance = NULL;
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    /* IInspectable methods */
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,
    /* IActivationFactory methods */
    factory_ActivateInstance,
};

static inline struct launcher_statics *impl_from_ILauncherStatics( ILauncherStatics *iface )
{
    return CONTAINING_RECORD( iface, struct launcher_statics, ILauncherStatics_iface );
}

static HRESULT WINAPI statics_QueryInterface( ILauncherStatics *iface, REFIID iid, void **out )
{
    return IActivationFactory_QueryInterface( &impl_from_ILauncherStatics( iface )->IActivationFactory_iface, iid, out );
}

static ULONG WINAPI statics_AddRef( ILauncherStatics *iface )
{
    return 2;
}

static ULONG WINAPI statics_Release( ILauncherStatics *iface )
{
    return 1;
}

static HRESULT WINAPI statics_GetIids( ILauncherStatics *iface, ULONG *iid_count, IID **iids )
{
    return factory_GetIids( &impl_from_ILauncherStatics( iface )->IActivationFactory_iface, iid_count, iids );
}

static HRESULT WINAPI statics_GetRuntimeClassName( ILauncherStatics *iface, HSTRING *class_name )
{
    return factory_GetRuntimeClassName( &impl_from_ILauncherStatics( iface )->IActivationFactory_iface, class_name );
}

static HRESULT WINAPI statics_GetTrustLevel( ILauncherStatics *iface, TrustLevel *trust_level )
{
    return factory_GetTrustLevel( &impl_from_ILauncherStatics( iface )->IActivationFactory_iface, trust_level );
}

static HRESULT WINAPI statics_LaunchFileAsync( ILauncherStatics *iface, IStorageFile *file, IAsyncOperation_boolean **operation )
{
    TRACE( "iface %p, file %p, operation %p\n", iface, file, operation );
    if (!file || !operation) return E_POINTER;
    return async_operation_boolean_create( (IUnknown *)iface, (IUnknown *)file, launch_file_async, operation );
}

static HRESULT WINAPI statics_LaunchFileWithOptionsAsync( ILauncherStatics *iface, IStorageFile *file, IInspectable *options,
                                                          IAsyncOperation_boolean **operation )
{
    TRACE( "iface %p, file %p, options %p, operation %p\n", iface, file, options, operation );
    return statics_LaunchFileAsync( iface, file, operation );
}

static HRESULT WINAPI statics_LaunchUriAsync( ILauncherStatics *iface, IUriRuntimeClass *uri, IAsyncOperation_boolean **operation )
{
    TRACE( "iface %p, uri %p, operation %p\n", iface, uri, operation );
    if (!uri || !operation) return E_POINTER;
    return async_operation_boolean_create( (IUnknown *)iface, (IUnknown *)uri, launch_uri_async, operation );
}

static HRESULT WINAPI statics_LaunchUriWithOptionsAsync( ILauncherStatics *iface, IUriRuntimeClass *uri, IInspectable *options,
                                                         IAsyncOperation_boolean **operation )
{
    TRACE( "iface %p, uri %p, options %p, operation %p\n", iface, uri, options, operation );
    return statics_LaunchUriAsync( iface, uri, operation );
}

static const struct ILauncherStaticsVtbl statics_vtbl =
{
    statics_QueryInterface,
    statics_AddRef,
    statics_Release,
    /* IInspectable methods */
    statics_GetIids,
    statics_GetRuntimeClassName,
    statics_GetTrustLevel,
    /* ILauncherStatics methods */
    statics_LaunchFileAsync,
    statics_LaunchFileWithOptionsAsync,
    statics_LaunchUriAsync,
    statics_LaunchUriWithOptionsAsync,
};

static struct launcher_statics launcher_statics =
{
    {&factory_vtbl},
    {&statics_vtbl},
};

HRESULT WINAPI DllCanUnloadNow(void)
{
    return S_FALSE;
}

HRESULT WINAPI DllGetClassObject( REFCLSID clsid, REFIID riid, void **out )
{
    FIXME( "clsid %s, riid %s, out %p stub!\n", debugstr_guid( clsid ), debugstr_guid( riid ), out );
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllGetActivationFactory( HSTRING classid, IActivationFactory **factory )
{
    const WCHAR *name = WindowsGetStringRawBuffer( classid, NULL );

    TRACE( "class %s, factory %p.\n", debugstr_hstring( classid ), factory );

    *factory = NULL;
    if (!wcscmp( name, RuntimeClass_Windows_System_Launcher ))
    {
        *factory = &launcher_statics.IActivationFactory_iface;
        return S_OK;
    }
    return CLASS_E_CLASSNOTAVAILABLE;
}
