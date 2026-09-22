/*
 * Windows.UI.Notifications: toasts, shown by the shell
 *
 * ToastNotifier.Show reads the toast's XML the way Windows 10 lays a toast
 * out (the first text its title, the others its message, the attribution
 * text and the app logo override apart) and hands it to the shell. A thread
 * of this process keeps a window for what the shell says back, and raises
 * the toast's Activated and Dismissed from there.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "private.h"
#include "shlwapi.h"
#include "arctic_toast.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(toast);

typedef __FITypedEventHandler_2_Windows__CUI__CNotifications__CToastNotification_IInspectable activated_handler;
typedef __FITypedEventHandler_2_Windows__CUI__CNotifications__CToastNotification_Windows__CUI__CNotifications__CToastDismissedEventArgs dismissed_handler;
typedef __FITypedEventHandler_2_Windows__CUI__CNotifications__CToastNotification_Windows__CUI__CNotifications__CToastFailedEventArgs failed_handler;
typedef __FIReference_1_DateTime date_reference;

struct handler
{
    struct list entry;
    EventRegistrationToken token;
    IUnknown *handler;
};

static LONG next_token;

static HRESULT handler_add( struct list *list, CRITICAL_SECTION *cs, IUnknown *handler, EventRegistrationToken *token )
{
    struct handler *entry;

    if (!handler) return E_INVALIDARG;
    if (!(entry = calloc( 1, sizeof(*entry) ))) return E_OUTOFMEMORY;
    entry->token.value = InterlockedIncrement( &next_token );
    entry->handler = handler;
    IUnknown_AddRef( handler );
    EnterCriticalSection( cs );
    list_add_tail( list, &entry->entry );
    LeaveCriticalSection( cs );
    *token = entry->token;
    return S_OK;
}

static HRESULT handler_remove( struct list *list, CRITICAL_SECTION *cs, EventRegistrationToken token )
{
    struct handler *entry, *found = NULL;

    EnterCriticalSection( cs );
    LIST_FOR_EACH_ENTRY( entry, list, struct handler, entry )
    {
        if (entry->token.value != token.value) continue;
        list_remove( &entry->entry );
        found = entry;
        break;
    }
    LeaveCriticalSection( cs );
    if (!found) return S_OK;
    IUnknown_Release( found->handler );
    free( found );
    return S_OK;
}

static void handlers_free( struct list *list )
{
    struct handler *entry, *next;

    LIST_FOR_EACH_ENTRY_SAFE( entry, next, list, struct handler, entry )
    {
        list_remove( &entry->entry );
        IUnknown_Release( entry->handler );
        free( entry );
    }
}

/* the handlers as they are now, to be called outside the lock */
static IUnknown **handlers_copy( struct list *list, CRITICAL_SECTION *cs, UINT *count )
{
    struct handler *entry;
    IUnknown **copy;
    UINT i = 0;

    EnterCriticalSection( cs );
    *count = list_count( list );
    if ((copy = calloc( *count + 1, sizeof(*copy) )))
    {
        LIST_FOR_EACH_ENTRY( entry, list, struct handler, entry )
        {
            IUnknown_AddRef( entry->handler );
            copy[i++] = entry->handler;
        }
    }
    else *count = 0;
    LeaveCriticalSection( cs );
    return copy;
}

struct toast
{
    IToastNotification IToastNotification_iface;
    IToastNotification2 IToastNotification2_iface;
    LONG ref;

    CRITICAL_SECTION cs;
    IXmlDocument *content;
    date_reference *expiration;
    HSTRING tag;
    HSTRING group;
    boolean suppress_popup;
    struct list activated;
    struct list dismissed;
    struct list failed;

    /* while the shell has it */
    struct list shown_entry;
    DWORD id;
    HSTRING launch;
};

static inline struct toast *impl_from_IToastNotification( IToastNotification *iface )
{
    return CONTAINING_RECORD( iface, struct toast, IToastNotification_iface );
}

static HRESULT WINAPI toast_QueryInterface( IToastNotification *iface, REFIID iid, void **out )
{
    struct toast *impl = impl_from_IToastNotification( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IToastNotification ))
        *out = &impl->IToastNotification_iface;
    else if (IsEqualGUID( iid, &IID_IToastNotification2 ))
        *out = &impl->IToastNotification2_iface;

    if (!*out)
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG WINAPI toast_AddRef( IToastNotification *iface )
{
    struct toast *impl = impl_from_IToastNotification( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI toast_Release( IToastNotification *iface )
{
    struct toast *impl = impl_from_IToastNotification( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    TRACE( "iface %p, ref %lu.\n", iface, ref );

    if (!ref)
    {
        handlers_free( &impl->activated );
        handlers_free( &impl->dismissed );
        handlers_free( &impl->failed );
        if (impl->content) IXmlDocument_Release( impl->content );
        if (impl->expiration) __FIReference_1_DateTime_Release( impl->expiration );
        WindowsDeleteString( impl->tag );
        WindowsDeleteString( impl->group );
        WindowsDeleteString( impl->launch );
        DeleteCriticalSection( &impl->cs );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI toast_GetIids( IToastNotification *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI toast_GetRuntimeClassName( IToastNotification *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_UI_Notifications_ToastNotification,
                                wcslen( RuntimeClass_Windows_UI_Notifications_ToastNotification ), class_name );
}

static HRESULT WINAPI toast_GetTrustLevel( IToastNotification *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI toast_get_Content( IToastNotification *iface, IXmlDocument **value )
{
    struct toast *impl = impl_from_IToastNotification( iface );
    if ((*value = impl->content)) IXmlDocument_AddRef( *value );
    return S_OK;
}

static HRESULT WINAPI toast_put_ExpirationTime( IToastNotification *iface, date_reference *value )
{
    struct toast *impl = impl_from_IToastNotification( iface );
    date_reference *old;

    if (value) __FIReference_1_DateTime_AddRef( value );
    EnterCriticalSection( &impl->cs );
    old = impl->expiration;
    impl->expiration = value;
    LeaveCriticalSection( &impl->cs );
    if (old) __FIReference_1_DateTime_Release( old );
    return S_OK;
}

static HRESULT WINAPI toast_get_ExpirationTime( IToastNotification *iface, date_reference **value )
{
    struct toast *impl = impl_from_IToastNotification( iface );

    EnterCriticalSection( &impl->cs );
    if ((*value = impl->expiration)) __FIReference_1_DateTime_AddRef( *value );
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static HRESULT WINAPI toast_add_Dismissed( IToastNotification *iface, dismissed_handler *handler,
                                           EventRegistrationToken *token )
{
    struct toast *impl = impl_from_IToastNotification( iface );
    return handler_add( &impl->dismissed, &impl->cs, (IUnknown *)handler, token );
}

static HRESULT WINAPI toast_remove_Dismissed( IToastNotification *iface, EventRegistrationToken token )
{
    struct toast *impl = impl_from_IToastNotification( iface );
    return handler_remove( &impl->dismissed, &impl->cs, token );
}

static HRESULT WINAPI toast_add_Activated( IToastNotification *iface, activated_handler *handler,
                                           EventRegistrationToken *token )
{
    struct toast *impl = impl_from_IToastNotification( iface );
    return handler_add( &impl->activated, &impl->cs, (IUnknown *)handler, token );
}

static HRESULT WINAPI toast_remove_Activated( IToastNotification *iface, EventRegistrationToken token )
{
    struct toast *impl = impl_from_IToastNotification( iface );
    return handler_remove( &impl->activated, &impl->cs, token );
}

static HRESULT WINAPI toast_add_Failed( IToastNotification *iface, failed_handler *handler,
                                        EventRegistrationToken *token )
{
    struct toast *impl = impl_from_IToastNotification( iface );
    return handler_add( &impl->failed, &impl->cs, (IUnknown *)handler, token );
}

static HRESULT WINAPI toast_remove_Failed( IToastNotification *iface, EventRegistrationToken token )
{
    struct toast *impl = impl_from_IToastNotification( iface );
    return handler_remove( &impl->failed, &impl->cs, token );
}

static const struct IToastNotificationVtbl toast_vtbl =
{
    toast_QueryInterface,
    toast_AddRef,
    toast_Release,
    /* IInspectable methods */
    toast_GetIids,
    toast_GetRuntimeClassName,
    toast_GetTrustLevel,
    /* IToastNotification methods */
    toast_get_Content,
    toast_put_ExpirationTime,
    toast_get_ExpirationTime,
    toast_add_Dismissed,
    toast_remove_Dismissed,
    toast_add_Activated,
    toast_remove_Activated,
    toast_add_Failed,
    toast_remove_Failed,
};

DEFINE_IINSPECTABLE( toast2, IToastNotification2, struct toast, IToastNotification_iface )

static HRESULT set_string( struct toast *impl, HSTRING *field, HSTRING value )
{
    HSTRING copy, old;
    HRESULT hr;

    if (FAILED(hr = WindowsDuplicateString( value, &copy ))) return hr;
    EnterCriticalSection( &impl->cs );
    old = *field;
    *field = copy;
    LeaveCriticalSection( &impl->cs );
    WindowsDeleteString( old );
    return S_OK;
}

static HRESULT get_string( struct toast *impl, HSTRING *field, HSTRING *value )
{
    HRESULT hr;

    EnterCriticalSection( &impl->cs );
    hr = WindowsDuplicateString( *field, value );
    LeaveCriticalSection( &impl->cs );
    return hr;
}

static HRESULT WINAPI toast2_put_Tag( IToastNotification2 *iface, HSTRING value )
{
    struct toast *impl = impl_from_IToastNotification2( iface );
    return set_string( impl, &impl->tag, value );
}

static HRESULT WINAPI toast2_get_Tag( IToastNotification2 *iface, HSTRING *value )
{
    struct toast *impl = impl_from_IToastNotification2( iface );
    return get_string( impl, &impl->tag, value );
}

static HRESULT WINAPI toast2_put_Group( IToastNotification2 *iface, HSTRING value )
{
    struct toast *impl = impl_from_IToastNotification2( iface );
    return set_string( impl, &impl->group, value );
}

static HRESULT WINAPI toast2_get_Group( IToastNotification2 *iface, HSTRING *value )
{
    struct toast *impl = impl_from_IToastNotification2( iface );
    return get_string( impl, &impl->group, value );
}

static HRESULT WINAPI toast2_put_SuppressPopup( IToastNotification2 *iface, boolean value )
{
    struct toast *impl = impl_from_IToastNotification2( iface );
    impl->suppress_popup = value;
    return S_OK;
}

static HRESULT WINAPI toast2_get_SuppressPopup( IToastNotification2 *iface, boolean *value )
{
    struct toast *impl = impl_from_IToastNotification2( iface );
    *value = impl->suppress_popup;
    return S_OK;
}

static const struct IToastNotification2Vtbl toast2_vtbl =
{
    toast2_QueryInterface,
    toast2_AddRef,
    toast2_Release,
    /* IInspectable methods */
    toast2_GetIids,
    toast2_GetRuntimeClassName,
    toast2_GetTrustLevel,
    /* IToastNotification2 methods */
    toast2_put_Tag,
    toast2_get_Tag,
    toast2_put_Group,
    toast2_get_Group,
    toast2_put_SuppressPopup,
    toast2_get_SuppressPopup,
};

static HRESULT toast_create( IXmlDocument *content, IToastNotification **out )
{
    struct toast *impl;

    *out = NULL;
    if (!content) return E_POINTER;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IToastNotification_iface.lpVtbl = &toast_vtbl;
    impl->IToastNotification2_iface.lpVtbl = &toast2_vtbl;
    impl->ref = 1;
    InitializeCriticalSection( &impl->cs );
    impl->content = content;
    IXmlDocument_AddRef( content );
    list_init( &impl->activated );
    list_init( &impl->dismissed );
    list_init( &impl->failed );
    list_init( &impl->shown_entry );
    *out = &impl->IToastNotification_iface;
    return S_OK;
}

/* what a toast's handlers get */
struct dismissed_args
{
    IToastDismissedEventArgs IToastDismissedEventArgs_iface;
    LONG ref;
    ToastDismissalReason reason;
};

static inline struct dismissed_args *impl_from_IToastDismissedEventArgs( IToastDismissedEventArgs *iface )
{
    return CONTAINING_RECORD( iface, struct dismissed_args, IToastDismissedEventArgs_iface );
}

static HRESULT WINAPI dismissed_args_QueryInterface( IToastDismissedEventArgs *iface, REFIID iid, void **out )
{
    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IToastDismissedEventArgs ))
    {
        *out = iface;
        IUnknown_AddRef( (IUnknown *)*out );
        return S_OK;
    }
    return E_NOINTERFACE;
}

static ULONG WINAPI dismissed_args_AddRef( IToastDismissedEventArgs *iface )
{
    struct dismissed_args *impl = impl_from_IToastDismissedEventArgs( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI dismissed_args_Release( IToastDismissedEventArgs *iface )
{
    struct dismissed_args *impl = impl_from_IToastDismissedEventArgs( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI dismissed_args_GetIids( IToastDismissedEventArgs *iface, ULONG *iid_count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI dismissed_args_GetRuntimeClassName( IToastDismissedEventArgs *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_UI_Notifications_ToastDismissedEventArgs,
                                wcslen( RuntimeClass_Windows_UI_Notifications_ToastDismissedEventArgs ), class_name );
}

static HRESULT WINAPI dismissed_args_GetTrustLevel( IToastDismissedEventArgs *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI dismissed_args_get_Reason( IToastDismissedEventArgs *iface, ToastDismissalReason *value )
{
    struct dismissed_args *impl = impl_from_IToastDismissedEventArgs( iface );
    *value = impl->reason;
    return S_OK;
}

static const struct IToastDismissedEventArgsVtbl dismissed_args_vtbl =
{
    dismissed_args_QueryInterface,
    dismissed_args_AddRef,
    dismissed_args_Release,
    /* IInspectable methods */
    dismissed_args_GetIids,
    dismissed_args_GetRuntimeClassName,
    dismissed_args_GetTrustLevel,
    /* IToastDismissedEventArgs methods */
    dismissed_args_get_Reason,
};

struct activated_args
{
    IToastActivatedEventArgs IToastActivatedEventArgs_iface;
    LONG ref;
    HSTRING arguments;
};

static inline struct activated_args *impl_from_IToastActivatedEventArgs( IToastActivatedEventArgs *iface )
{
    return CONTAINING_RECORD( iface, struct activated_args, IToastActivatedEventArgs_iface );
}

static HRESULT WINAPI activated_args_QueryInterface( IToastActivatedEventArgs *iface, REFIID iid, void **out )
{
    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IToastActivatedEventArgs ))
    {
        *out = iface;
        IUnknown_AddRef( (IUnknown *)*out );
        return S_OK;
    }
    return E_NOINTERFACE;
}

static ULONG WINAPI activated_args_AddRef( IToastActivatedEventArgs *iface )
{
    struct activated_args *impl = impl_from_IToastActivatedEventArgs( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI activated_args_Release( IToastActivatedEventArgs *iface )
{
    struct activated_args *impl = impl_from_IToastActivatedEventArgs( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        WindowsDeleteString( impl->arguments );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI activated_args_GetIids( IToastActivatedEventArgs *iface, ULONG *iid_count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI activated_args_GetRuntimeClassName( IToastActivatedEventArgs *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_UI_Notifications_ToastActivatedEventArgs,
                                wcslen( RuntimeClass_Windows_UI_Notifications_ToastActivatedEventArgs ), class_name );
}

static HRESULT WINAPI activated_args_GetTrustLevel( IToastActivatedEventArgs *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI activated_args_get_Arguments( IToastActivatedEventArgs *iface, HSTRING *value )
{
    struct activated_args *impl = impl_from_IToastActivatedEventArgs( iface );
    return WindowsDuplicateString( impl->arguments, value );
}

static const struct IToastActivatedEventArgsVtbl activated_args_vtbl =
{
    activated_args_QueryInterface,
    activated_args_AddRef,
    activated_args_Release,
    /* IInspectable methods */
    activated_args_GetIids,
    activated_args_GetRuntimeClassName,
    activated_args_GetTrustLevel,
    /* IToastActivatedEventArgs methods */
    activated_args_get_Arguments,
};

/* the toasts the shell has, and the window it answers to */
static CRITICAL_SECTION shown_cs;
static CRITICAL_SECTION_DEBUG shown_cs_debug =
{
    0, 0, &shown_cs,
    { &shown_cs_debug.ProcessLocksList, &shown_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": shown_cs") }
};
static CRITICAL_SECTION shown_cs = { &shown_cs_debug, -1, 0, 0, 0, 0 };
static struct list shown = LIST_INIT( shown );
static LONG next_id;
static HWND reply_window;
static UINT event_message;

/* the toast is no longer the shell's; the caller has its reference */
static struct toast *shown_remove( DWORD id )
{
    struct toast *impl, *found = NULL;

    EnterCriticalSection( &shown_cs );
    LIST_FOR_EACH_ENTRY( impl, &shown, struct toast, shown_entry )
    {
        if (impl->id != id) continue;
        list_remove( &impl->shown_entry );
        list_init( &impl->shown_entry );
        found = impl;
        break;
    }
    LeaveCriticalSection( &shown_cs );
    return found;
}

static void raise_activated( struct toast *impl )
{
    struct activated_args *args;
    IUnknown **handlers;
    UINT count, i;

    if (!(args = calloc( 1, sizeof(*args) ))) return;
    args->IToastActivatedEventArgs_iface.lpVtbl = &activated_args_vtbl;
    args->ref = 1;
    WindowsDuplicateString( impl->launch, &args->arguments );

    handlers = handlers_copy( &impl->activated, &impl->cs, &count );
    for (i = 0; i < count; i++)
    {
        activated_handler *handler = (activated_handler *)handlers[i];
        __FITypedEventHandler_2_Windows__CUI__CNotifications__CToastNotification_IInspectable_Invoke(
            handler, &impl->IToastNotification_iface, (IInspectable *)&args->IToastActivatedEventArgs_iface );
        IUnknown_Release( handlers[i] );
    }
    free( handlers );
    IToastActivatedEventArgs_Release( &args->IToastActivatedEventArgs_iface );
}

static void raise_dismissed( struct toast *impl, ToastDismissalReason reason )
{
    struct dismissed_args *args;
    IUnknown **handlers;
    UINT count, i;

    if (!(args = calloc( 1, sizeof(*args) ))) return;
    args->IToastDismissedEventArgs_iface.lpVtbl = &dismissed_args_vtbl;
    args->ref = 1;
    args->reason = reason;

    handlers = handlers_copy( &impl->dismissed, &impl->cs, &count );
    for (i = 0; i < count; i++)
    {
        dismissed_handler *handler = (dismissed_handler *)handlers[i];
        __FITypedEventHandler_2_Windows__CUI__CNotifications__CToastNotification_Windows__CUI__CNotifications__CToastDismissedEventArgs_Invoke(
            handler, &impl->IToastNotification_iface, &args->IToastDismissedEventArgs_iface );
        IUnknown_Release( handlers[i] );
    }
    free( handlers );
    IToastDismissedEventArgs_Release( &args->IToastDismissedEventArgs_iface );
}

static LRESULT CALLBACK reply_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct toast *impl;

    if (msg != event_message || !event_message) return DefWindowProcW( hwnd, msg, wparam, lparam );
    TRACE( "toast %Iu: %Id\n", wparam, lparam );
    if (!(impl = shown_remove( wparam ))) return 0;
    if (lparam == ARCTIC_TOAST_ACTIVATED) raise_activated( impl );
    else raise_dismissed( impl, (ToastDismissalReason)lparam );
    IToastNotification_Release( &impl->IToastNotification_iface );
    return 0;
}

static DWORD WINAPI reply_thread( void *arg )
{
    HANDLE ready = arg;
    WNDCLASSW class = { .lpfnWndProc = reply_proc, .lpszClassName = L"ArcticToastReply" };
    MSG msg;

    SetThreadDescription( GetCurrentThread(), L"wpnapps_toasts" );
    CoInitializeEx( NULL, COINIT_MULTITHREADED );
    class.hInstance = GetModuleHandleW( L"wpnapps.dll" );
    RegisterClassW( &class );
    event_message = RegisterWindowMessageW( ARCTIC_TOAST_EVENT );
    reply_window = CreateWindowExW( 0, class.lpszClassName, NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL,
                                    class.hInstance, NULL );
    /* posted from the shell, another process's lower integrity is no reason to drop it */
    ChangeWindowMessageFilterEx( reply_window, event_message, MSGFLT_ALLOW, NULL );
    SetEvent( ready );
    while (GetMessageW( &msg, NULL, 0, 0 )) DispatchMessageW( &msg );
    return 0;
}

static BOOL WINAPI start_reply_thread( INIT_ONCE *once, void *param, void **context )
{
    HANDLE ready = CreateEventW( NULL, TRUE, FALSE, NULL ), thread;

    if ((thread = CreateThread( NULL, 0, reply_thread, ready, 0, NULL )))
    {
        WaitForSingleObject( ready, 5000 );
        CloseHandle( thread );
    }
    CloseHandle( ready );
    return TRUE;
}

/* reading the toast's XML */
static BSTR element_attribute( IXMLDOMNode *node, const WCHAR *name )
{
    IXMLDOMElement *element;
    BSTR str = SysAllocString( name ), ret = NULL;
    VARIANT var;

    if (node && SUCCEEDED(IXMLDOMNode_QueryInterface( node, &IID_IXMLDOMElement, (void **)&element )))
    {
        VariantInit( &var );
        if (SUCCEEDED(IXMLDOMElement_getAttribute( element, str, &var )) && V_VT( &var ) == VT_BSTR)
        {
            ret = V_BSTR( &var );
            V_VT( &var ) = VT_EMPTY;
        }
        VariantClear( &var );
        IXMLDOMElement_Release( element );
    }
    SysFreeString( str );
    return ret;
}

static IXMLDOMNode *select_node( IXMLDOMNode *node, const WCHAR *path )
{
    BSTR str = SysAllocString( path );
    IXMLDOMNode *ret = NULL;

    if (FAILED(IXMLDOMNode_selectSingleNode( node, str, &ret ))) ret = NULL;
    SysFreeString( str );
    return ret;
}

static void append_text( WCHAR *buffer, size_t size, const WCHAR *text )
{
    size_t len = wcslen( buffer );

    if (!text || !*text) return;
    if (len && len + 1 < size) buffer[len++] = '\n';
    lstrcpynW( buffer + len, text, size - len );
}

/* an image of the toast's as a file: a path, or a file: URL */
static void image_path( const WCHAR *src, WCHAR *path, DWORD size )
{
    DWORD len = size;

    path[0] = 0;
    if (!src || !*src) return;
    if (!wcsnicmp( src, L"file:", 5 ))
    {
        if (FAILED(PathCreateFromUrlW( src, path, &len, 0 ))) path[0] = 0;
    }
    else if (wcsstr( src, L":\\" ) || (src[0] == '\\' && src[1] == '\\'))
        lstrcpynW( path, src, size );
    else FIXME( "image %s is not a file\n", debugstr_w(src) );
}

static void read_content( IXmlDocument *content, struct arctic_toast *data, HSTRING *launch )
{
    IXMLDOMNode *doc = xml_node_unwrap( (IUnknown *)content ), *toast, *binding, *item;
    IXMLDOMNodeList *items = NULL;
    BSTR str, template = NULL;
    BOOL image_and_text;
    LONG count = 0, i;

    *launch = NULL;
    if (!doc || !(toast = select_node( doc, L"/toast" ))) return;

    if ((str = element_attribute( toast, L"launch" )))
    {
        WindowsCreateString( str, SysStringLen( str ), launch );
        SysFreeString( str );
    }
    /* how long Windows 10 shows it: 5 seconds, 25 if long, reminders and calls until dismissed */
    if ((str = element_attribute( toast, L"duration" )))
    {
        if (!wcsicmp( str, L"long" )) data->duration = 25000;
        SysFreeString( str );
    }
    if ((str = element_attribute( toast, L"scenario" )))
    {
        if (!wcsicmp( str, L"reminder" ) || !wcsicmp( str, L"alarm" ) || !wcsicmp( str, L"incomingCall" ))
            data->duration = ARCTIC_TOAST_FOREVER;
        SysFreeString( str );
    }

    if (!(binding = select_node( toast, L"visual/binding[@template='ToastGeneric']" )))
        binding = select_node( toast, L"visual/binding" );
    IXMLDOMNode_Release( toast );
    if (!binding) return;
    template = element_attribute( binding, L"template" );
    image_and_text = template && !wcsnicmp( template, L"ToastImageAndText", 17 );
    SysFreeString( template );

    str = SysAllocString( L"text|image" );
    if (SUCCEEDED(IXMLDOMNode_selectNodes( binding, str, &items )) && items) IXMLDOMNodeList_get_length( items, &count );
    SysFreeString( str );
    for (i = 0; i < count; i++)
    {
        BSTR name = NULL, placement, text = NULL;

        if (FAILED(IXMLDOMNodeList_get_item( items, i, &item )) || !item) continue;
        IXMLDOMNode_get_nodeName( item, &name );
        placement = element_attribute( item, L"placement" );
        if (name && !wcscmp( name, L"text" ))
        {
            IXMLDOMNode_get_text( item, &text );
            if (placement && !wcsicmp( placement, L"attribution" ))
                lstrcpynW( data->attribution, text ? text : L"", ARRAY_SIZE(data->attribution) );
            else if (!data->title[0])
                lstrcpynW( data->title, text ? text : L"", ARRAY_SIZE(data->title) );
            else
                append_text( data->text, ARRAY_SIZE(data->text), text );
        }
        /* the picture beside the text: the app logo override, or the image of a legacy template */
        else if (name && !wcscmp( name, L"image" ) && !data->image[0] &&
                 ((placement && !wcsicmp( placement, L"appLogoOverride" )) || (!placement && image_and_text)))
        {
            BSTR src = element_attribute( item, L"src" ), crop = element_attribute( item, L"hint-crop" );
            image_path( src, data->image, ARRAY_SIZE(data->image) );
            data->image_circle = crop && !wcsicmp( crop, L"circle" );
            SysFreeString( src );
            SysFreeString( crop );
        }
        SysFreeString( text );
        SysFreeString( placement );
        SysFreeString( name );
        IXMLDOMNode_Release( item );
    }
    if (items) IXMLDOMNodeList_Release( items );
    IXMLDOMNode_Release( binding );
}

/* who the toast is from: the AppUserModelID's registration, else the program */
static void read_sender( HSTRING app_id, struct arctic_toast *data )
{
    static const WCHAR *roots[] = { L"Software\\Classes\\AppUserModelId\\" };
    WCHAR key[512], exe[MAX_PATH], value[MAX_PATH];
    DWORD size, handle, len;
    const WCHAR *id = WindowsGetStringRawBuffer( app_id, NULL );
    void *info;

    GetModuleFileNameW( NULL, exe, ARRAY_SIZE(exe) );
    lstrcpynW( data->icon, exe, ARRAY_SIZE(data->icon) );

    if (id && *id)
    {
        HKEY hives[] = { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE };
        UINT h;

        swprintf( key, ARRAY_SIZE(key), L"%s%s", roots[0], id );
        for (h = 0; h < ARRAY_SIZE(hives) && !data->app[0]; h++)
        {
            size = sizeof(value);
            if (!RegGetValueW( hives[h], key, L"DisplayName", RRF_RT_REG_SZ, NULL, value, &size ))
                lstrcpynW( data->app, value, ARRAY_SIZE(data->app) );
            size = sizeof(value);
            if (!RegGetValueW( hives[h], key, L"IconUri", RRF_RT_REG_SZ, NULL, value, &size ))
                image_path( value, data->icon, ARRAY_SIZE(data->icon) );
        }
        if (!data->icon[0]) lstrcpynW( data->icon, exe, ARRAY_SIZE(data->icon) );
    }
    if (data->app[0]) return;

    /* the name Windows 10 gives a desktop program: its file description */
    if ((size = GetFileVersionInfoSizeW( exe, &handle )) && (info = malloc( size )))
    {
        struct { WORD language, codepage; } *translation;
        WCHAR *description;
        UINT length;

        if (GetFileVersionInfoW( exe, 0, size, info ) &&
            VerQueryValueW( info, L"\\VarFileInfo\\Translation", (void **)&translation, &length ) && length)
        {
            swprintf( key, ARRAY_SIZE(key), L"\\StringFileInfo\\%04x%04x\\FileDescription",
                      translation->language, translation->codepage );
            if (VerQueryValueW( info, key, (void **)&description, &length ) && length && *description)
                lstrcpynW( data->app, description, ARRAY_SIZE(data->app) );
        }
        free( info );
    }
    if (data->app[0]) return;
    lstrcpynW( data->app, PathFindFileNameW( exe ), ARRAY_SIZE(data->app) );
    if ((len = wcslen( data->app )) > 4 && !wcsicmp( data->app + len - 4, L".exe" )) data->app[len - 4] = 0;
}

static BOOL send_to_shell( DWORD type, struct arctic_toast *data )
{
    COPYDATASTRUCT copy = { .dwData = type, .cbData = sizeof(*data), .lpData = data };
    HWND tray = FindWindowW( L"Shell_TrayWnd", NULL );
    DWORD_PTR result = 0;

    if (!tray)
    {
        WARN( "no shell to show the toast\n" );
        return FALSE;
    }
    return SendMessageTimeoutW( tray, WM_COPYDATA, (WPARAM)reply_window, (LPARAM)&copy, SMTO_ABORTIFHUNG,
                                5000, &result ) && result;
}

struct notifier
{
    IToastNotifier IToastNotifier_iface;
    LONG ref;
    HSTRING app_id;
};

static inline struct notifier *impl_from_IToastNotifier( IToastNotifier *iface )
{
    return CONTAINING_RECORD( iface, struct notifier, IToastNotifier_iface );
}

static HRESULT WINAPI notifier_QueryInterface( IToastNotifier *iface, REFIID iid, void **out )
{
    struct notifier *impl = impl_from_IToastNotifier( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IToastNotifier ))
    {
        *out = &impl->IToastNotifier_iface;
        IUnknown_AddRef( (IUnknown *)*out );
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI notifier_AddRef( IToastNotifier *iface )
{
    struct notifier *impl = impl_from_IToastNotifier( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI notifier_Release( IToastNotifier *iface )
{
    struct notifier *impl = impl_from_IToastNotifier( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        WindowsDeleteString( impl->app_id );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI notifier_GetIids( IToastNotifier *iface, ULONG *iid_count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI notifier_GetRuntimeClassName( IToastNotifier *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_UI_Notifications_ToastNotifier,
                                wcslen( RuntimeClass_Windows_UI_Notifications_ToastNotifier ), class_name );
}

static HRESULT WINAPI notifier_GetTrustLevel( IToastNotifier *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static INIT_ONCE reply_once = INIT_ONCE_STATIC_INIT;

static HRESULT WINAPI notifier_Show( IToastNotifier *iface, IToastNotification *notification )
{
    struct notifier *impl = impl_from_IToastNotifier( iface );
    struct arctic_toast *data;
    struct toast *toast;

    TRACE( "iface %p, notification %p.\n", iface, notification );

    if (!notification) return E_POINTER;
    if (notification->lpVtbl != &toast_vtbl) return E_INVALIDARG;
    toast = impl_from_IToastNotification( notification );
    if (toast->suppress_popup) return S_OK; /* only for the action center, which is not there */

    InitOnceExecuteOnce( &reply_once, start_reply_thread, NULL, NULL );
    if (!(data = calloc( 1, sizeof(*data) ))) return E_OUTOFMEMORY;
    data->size = sizeof(*data);
    data->reply = HandleToULong( reply_window );
    WindowsDeleteString( toast->launch );
    read_content( toast->content, data, &toast->launch );
    read_sender( impl->app_id, data );
    TRACE( "from %s: %s / %s\n", debugstr_w(data->app), debugstr_w(data->title), debugstr_w(data->text) );

    /* a toast shown again is a new one for the shell */
    if ((toast = shown_remove( toast->id ))) IToastNotification_Release( &toast->IToastNotification_iface );
    toast = impl_from_IToastNotification( notification );
    data->id = toast->id = InterlockedIncrement( &next_id );
    IToastNotification_AddRef( notification );
    EnterCriticalSection( &shown_cs );
    list_add_tail( &shown, &toast->shown_entry );
    LeaveCriticalSection( &shown_cs );

    if (!send_to_shell( ARCTIC_TOAST_SHOW, data ) && (toast = shown_remove( data->id )))
        IToastNotification_Release( &toast->IToastNotification_iface );
    free( data );
    return S_OK;
}

static HRESULT WINAPI notifier_Hide( IToastNotifier *iface, IToastNotification *notification )
{
    struct arctic_toast data = { .size = sizeof(data) };
    struct toast *toast;

    TRACE( "iface %p, notification %p.\n", iface, notification );

    if (!notification) return E_POINTER;
    if (notification->lpVtbl != &toast_vtbl) return E_INVALIDARG;
    toast = impl_from_IToastNotification( notification );
    if (!toast->id) return S_OK;
    data.reply = HandleToULong( reply_window );
    data.id = toast->id;
    send_to_shell( ARCTIC_TOAST_HIDE, &data );
    return S_OK;
}

static HRESULT WINAPI notifier_get_Setting( IToastNotifier *iface, NotificationSetting *value )
{
    *value = (NotificationSetting)0; /* Enabled */
    return S_OK;
}

static HRESULT WINAPI notifier_AddToSchedule( IToastNotifier *iface, IScheduledToastNotification *scheduled_toast )
{
    FIXME( "iface %p, scheduled_toast %p stub!\n", iface, scheduled_toast );
    return E_NOTIMPL;
}

static HRESULT WINAPI notifier_RemoveFromSchedule( IToastNotifier *iface, IScheduledToastNotification *scheduled_toast )
{
    FIXME( "iface %p, scheduled_toast %p stub!\n", iface, scheduled_toast );
    return E_NOTIMPL;
}

static HRESULT WINAPI notifier_GetScheduledToastNotifications( IToastNotifier *iface,
                                                               __FIVectorView_1_Windows__CUI__CNotifications__CScheduledToastNotification **result )
{
    FIXME( "iface %p, result %p stub!\n", iface, result );
    *result = NULL;
    return E_NOTIMPL;
}

static const struct IToastNotifierVtbl notifier_vtbl =
{
    notifier_QueryInterface,
    notifier_AddRef,
    notifier_Release,
    /* IInspectable methods */
    notifier_GetIids,
    notifier_GetRuntimeClassName,
    notifier_GetTrustLevel,
    /* IToastNotifier methods */
    notifier_Show,
    notifier_Hide,
    notifier_get_Setting,
    notifier_AddToSchedule,
    notifier_RemoveFromSchedule,
    notifier_GetScheduledToastNotifications,
};

static HRESULT notifier_create( HSTRING app_id, IToastNotifier **out )
{
    struct notifier *impl;
    HRESULT hr;

    *out = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IToastNotifier_iface.lpVtbl = &notifier_vtbl;
    impl->ref = 1;
    if (FAILED(hr = WindowsDuplicateString( app_id, &impl->app_id )))
    {
        free( impl );
        return hr;
    }
    *out = &impl->IToastNotifier_iface;
    return S_OK;
}

/* the action center is not there: the history has nothing to remove */
struct history
{
    IToastNotificationHistory IToastNotificationHistory_iface;
    LONG ref;
};

static inline struct history *impl_from_IToastNotificationHistory( IToastNotificationHistory *iface )
{
    return CONTAINING_RECORD( iface, struct history, IToastNotificationHistory_iface );
}

static HRESULT WINAPI history_QueryInterface( IToastNotificationHistory *iface, REFIID iid, void **out )
{
    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IToastNotificationHistory ))
    {
        *out = iface;
        IUnknown_AddRef( (IUnknown *)*out );
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI history_AddRef( IToastNotificationHistory *iface )
{
    struct history *impl = impl_from_IToastNotificationHistory( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI history_Release( IToastNotificationHistory *iface )
{
    struct history *impl = impl_from_IToastNotificationHistory( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI history_GetIids( IToastNotificationHistory *iface, ULONG *iid_count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI history_GetRuntimeClassName( IToastNotificationHistory *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_UI_Notifications_ToastNotificationHistory,
                                wcslen( RuntimeClass_Windows_UI_Notifications_ToastNotificationHistory ), class_name );
}

static HRESULT WINAPI history_GetTrustLevel( IToastNotificationHistory *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI history_RemoveGroup( IToastNotificationHistory *iface, HSTRING group )
{
    return S_OK;
}

static HRESULT WINAPI history_RemoveGroupWithId( IToastNotificationHistory *iface, HSTRING group, HSTRING app_id )
{
    return S_OK;
}

static HRESULT WINAPI history_RemoveGroupedTagWithId( IToastNotificationHistory *iface, HSTRING tag, HSTRING group,
                                                      HSTRING app_id )
{
    return S_OK;
}

static HRESULT WINAPI history_RemoveGroupedTag( IToastNotificationHistory *iface, HSTRING tag, HSTRING group )
{
    return S_OK;
}

static HRESULT WINAPI history_Remove( IToastNotificationHistory *iface, HSTRING tag )
{
    return S_OK;
}

static HRESULT WINAPI history_Clear( IToastNotificationHistory *iface )
{
    return S_OK;
}

static HRESULT WINAPI history_ClearWithId( IToastNotificationHistory *iface, HSTRING app_id )
{
    return S_OK;
}

static const struct IToastNotificationHistoryVtbl history_vtbl =
{
    history_QueryInterface,
    history_AddRef,
    history_Release,
    /* IInspectable methods */
    history_GetIids,
    history_GetRuntimeClassName,
    history_GetTrustLevel,
    /* IToastNotificationHistory methods */
    history_RemoveGroup,
    history_RemoveGroupWithId,
    history_RemoveGroupedTagWithId,
    history_RemoveGroupedTag,
    history_Remove,
    history_Clear,
    history_ClearWithId,
};

/* ToastNotificationManager */
struct manager_statics
{
    IActivationFactory IActivationFactory_iface;
    IToastNotificationManagerStatics IToastNotificationManagerStatics_iface;
    IToastNotificationManagerStatics2 IToastNotificationManagerStatics2_iface;
    LONG ref;
};

static inline struct manager_statics *manager_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct manager_statics, IActivationFactory_iface );
}

static HRESULT WINAPI manager_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct manager_statics *impl = manager_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &impl->IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_IToastNotificationManagerStatics ))
        *out = &impl->IToastNotificationManagerStatics_iface;
    else if (IsEqualGUID( iid, &IID_IToastNotificationManagerStatics2 ))
        *out = &impl->IToastNotificationManagerStatics2_iface;

    if (!*out)
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG WINAPI manager_AddRef( IActivationFactory *iface )
{
    struct manager_statics *impl = manager_from_IActivationFactory( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI manager_Release( IActivationFactory *iface )
{
    struct manager_statics *impl = manager_from_IActivationFactory( iface );
    return InterlockedDecrement( &impl->ref );
}

static HRESULT WINAPI manager_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI manager_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI manager_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI manager_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    *instance = NULL;
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl manager_vtbl =
{
    manager_QueryInterface,
    manager_AddRef,
    manager_Release,
    /* IInspectable methods */
    manager_GetIids,
    manager_GetRuntimeClassName,
    manager_GetTrustLevel,
    /* IActivationFactory methods */
    manager_ActivateInstance,
};

DEFINE_IINSPECTABLE( manager_statics, IToastNotificationManagerStatics, struct manager_statics, IActivationFactory_iface )

static HRESULT WINAPI manager_statics_CreateToastNotifier( IToastNotificationManagerStatics *iface,
                                                           IToastNotifier **result )
{
    TRACE( "iface %p, result %p.\n", iface, result );
    return notifier_create( NULL, result );
}

static HRESULT WINAPI manager_statics_CreateToastNotifierWithId( IToastNotificationManagerStatics *iface,
                                                                 HSTRING application_id, IToastNotifier **result )
{
    TRACE( "iface %p, application_id %s, result %p.\n", iface, debugstr_hstring( application_id ), result );
    return notifier_create( application_id, result );
}

/* Windows 10's own, from wpnapps.dll */
static const WCHAR *templates[] =
{
    L"<toast><visual><binding template=\"ToastImageAndText01\"><image id=\"1\" src=\"\"/><text id=\"1\"></text></binding></visual></toast>",
    L"<toast><visual><binding template=\"ToastImageAndText02\"><image id=\"1\" src=\"\"/><text id=\"1\"></text><text id=\"2\"></text></binding></visual></toast>",
    L"<toast><visual><binding template=\"ToastImageAndText03\"><image id=\"1\" src=\"\"/><text id=\"1\"></text><text id=\"2\"></text></binding></visual></toast>",
    L"<toast><visual><binding template=\"ToastImageAndText04\"><image id=\"1\" src=\"\"/><text id=\"1\"></text><text id=\"2\"></text><text id=\"3\"></text></binding></visual></toast>",
    L"<toast><visual><binding template=\"ToastText01\"><text id=\"1\"></text></binding></visual></toast>",
    L"<toast><visual><binding template=\"ToastText02\"><text id=\"1\"></text><text id=\"2\"></text></binding></visual></toast>",
    L"<toast><visual><binding template=\"ToastText03\"><text id=\"1\"></text><text id=\"2\"></text></binding></visual></toast>",
    L"<toast><visual><binding template=\"ToastText04\"><text id=\"1\"></text><text id=\"2\"></text><text id=\"3\"></text></binding></visual></toast>",
};

static HRESULT WINAPI manager_statics_GetTemplateContent( IToastNotificationManagerStatics *iface,
                                                          ToastTemplateType type, IXmlDocument **result )
{
    TRACE( "iface %p, type %d, result %p.\n", iface, type, result );

    *result = NULL;
    if ((UINT)type >= ARRAY_SIZE(templates)) return E_INVALIDARG;
    return xml_document_load( templates[type], result );
}

static const struct IToastNotificationManagerStaticsVtbl manager_statics_vtbl =
{
    manager_statics_QueryInterface,
    manager_statics_AddRef,
    manager_statics_Release,
    /* IInspectable methods */
    manager_statics_GetIids,
    manager_statics_GetRuntimeClassName,
    manager_statics_GetTrustLevel,
    /* IToastNotificationManagerStatics methods */
    manager_statics_CreateToastNotifier,
    manager_statics_CreateToastNotifierWithId,
    manager_statics_GetTemplateContent,
};

DEFINE_IINSPECTABLE( manager_statics2, IToastNotificationManagerStatics2, struct manager_statics, IActivationFactory_iface )

static HRESULT WINAPI manager_statics2_get_History( IToastNotificationManagerStatics2 *iface,
                                                    IToastNotificationHistory **value )
{
    struct history *impl;

    *value = NULL;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IToastNotificationHistory_iface.lpVtbl = &history_vtbl;
    impl->ref = 1;
    *value = &impl->IToastNotificationHistory_iface;
    return S_OK;
}

static const struct IToastNotificationManagerStatics2Vtbl manager_statics2_vtbl =
{
    manager_statics2_QueryInterface,
    manager_statics2_AddRef,
    manager_statics2_Release,
    /* IInspectable methods */
    manager_statics2_GetIids,
    manager_statics2_GetRuntimeClassName,
    manager_statics2_GetTrustLevel,
    /* IToastNotificationManagerStatics2 methods */
    manager_statics2_get_History,
};

static struct manager_statics manager_statics =
{
    {&manager_vtbl},
    {&manager_statics_vtbl},
    {&manager_statics2_vtbl},
    1,
};

IActivationFactory *toast_manager_factory = &manager_statics.IActivationFactory_iface;

/* ToastNotification: made from its XML */
struct toast_statics
{
    IActivationFactory IActivationFactory_iface;
    IToastNotificationFactory IToastNotificationFactory_iface;
    LONG ref;
};

static inline struct toast_statics *toast_statics_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct toast_statics, IActivationFactory_iface );
}

static HRESULT WINAPI toast_statics_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct toast_statics *impl = toast_statics_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &impl->IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_IToastNotificationFactory ))
        *out = &impl->IToastNotificationFactory_iface;

    if (!*out)
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        return E_NOINTERFACE;
    }
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG WINAPI toast_statics_AddRef( IActivationFactory *iface )
{
    struct toast_statics *impl = toast_statics_from_IActivationFactory( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI toast_statics_Release( IActivationFactory *iface )
{
    struct toast_statics *impl = toast_statics_from_IActivationFactory( iface );
    return InterlockedDecrement( &impl->ref );
}

static HRESULT WINAPI toast_statics_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI toast_statics_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI toast_statics_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI toast_statics_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    *instance = NULL;
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl toast_statics_vtbl =
{
    toast_statics_QueryInterface,
    toast_statics_AddRef,
    toast_statics_Release,
    /* IInspectable methods */
    toast_statics_GetIids,
    toast_statics_GetRuntimeClassName,
    toast_statics_GetTrustLevel,
    /* IActivationFactory methods */
    toast_statics_ActivateInstance,
};

DEFINE_IINSPECTABLE( toast_factory, IToastNotificationFactory, struct toast_statics, IActivationFactory_iface )

static HRESULT WINAPI toast_factory_CreateToastNotification( IToastNotificationFactory *iface, IXmlDocument *content,
                                                             IToastNotification **value )
{
    TRACE( "iface %p, content %p, value %p.\n", iface, content, value );
    return toast_create( content, value );
}

static const struct IToastNotificationFactoryVtbl toast_factory_vtbl =
{
    toast_factory_QueryInterface,
    toast_factory_AddRef,
    toast_factory_Release,
    /* IInspectable methods */
    toast_factory_GetIids,
    toast_factory_GetRuntimeClassName,
    toast_factory_GetTrustLevel,
    /* IToastNotificationFactory methods */
    toast_factory_CreateToastNotification,
};

static struct toast_statics toast_statics =
{
    {&toast_statics_vtbl},
    {&toast_factory_vtbl},
    1,
};

IActivationFactory *toast_factory = &toast_statics.IActivationFactory_iface;
