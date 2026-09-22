/*
 * Windows.UI.Notifications and Windows.Data.Xml.Dom
 *
 * wpnapps.dll of Windows 10 holds both: a program builds its toast as an
 * XmlDocument and hands it to a ToastNotifier. Here the toast goes to the
 * shell, which shows it as it shows the notification area's balloons.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "initguid.h"
#include "private.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(toast);

HRESULT WINAPI DllGetClassObject( REFCLSID clsid, REFIID riid, void **out )
{
    FIXME( "clsid %s, riid %s, out %p stub!\n", debugstr_guid(clsid), debugstr_guid(riid), out );
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllGetActivationFactory( HSTRING classid, IActivationFactory **factory )
{
    const WCHAR *buffer = WindowsGetStringRawBuffer( classid, NULL );

    TRACE( "class %s, factory %p.\n", debugstr_hstring(classid), factory );

    *factory = NULL;

    if (!wcscmp( buffer, RuntimeClass_Windows_Data_Xml_Dom_XmlDocument ))
        IActivationFactory_QueryInterface( xml_document_factory, &IID_IActivationFactory, (void **)factory );
    if (!wcscmp( buffer, RuntimeClass_Windows_UI_Notifications_ToastNotificationManager ))
        IActivationFactory_QueryInterface( toast_manager_factory, &IID_IActivationFactory, (void **)factory );
    if (!wcscmp( buffer, RuntimeClass_Windows_UI_Notifications_ToastNotification ))
        IActivationFactory_QueryInterface( toast_factory, &IID_IActivationFactory, (void **)factory );

    if (*factory) return S_OK;
    return CLASS_E_CLASSNOTAVAILABLE;
}
