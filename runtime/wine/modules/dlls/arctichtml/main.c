/*
 * Arctic web engine
 *
 * The engine of the system's Internet Explorer, and later of the WebBrowser
 * control: our own HTML engine, written here, with the plan, the stages and
 * the code it borrows from in docs/browser.md.
 *
 * It replaces nothing of Wine: Wine's mshtml is only a shell over
 * wine-gecko, which this image does not carry.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>

#include "arctichtml_private.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(arctichtml);

HINSTANCE arctichtml_instance;

BOOL WINAPI DllMain( HINSTANCE instance, DWORD reason, void *reserved )
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        arctichtml_instance = instance;
        DisableThreadLibraryCalls( instance );
        break;
    }
    return TRUE;
}

HRESULT WINAPI ArcticHtmlCreateView( HWND parent, const RECT *rect, ARCTIC_HTML_VIEW *view )
{
    TRACE( "%p %s %p\n", parent, wine_dbgstr_rect(rect), view );

    if (!view) return E_POINTER;
    *view = view_create( parent, rect );
    return *view ? S_OK : E_FAIL;
}

void WINAPI ArcticHtmlDestroyView( ARCTIC_HTML_VIEW view )
{
    TRACE( "%p\n", view );
    view_destroy( view );
}

void WINAPI ArcticHtmlSetRect( ARCTIC_HTML_VIEW view, const RECT *rect )
{
    TRACE( "%p %s\n", view, wine_dbgstr_rect(rect) );
    view_set_rect( view, rect );
}

HWND WINAPI ArcticHtmlGetWindow( ARCTIC_HTML_VIEW view )
{
    return view_window( view );
}

HRESULT WINAPI ArcticHtmlNavigate( ARCTIC_HTML_VIEW view, const WCHAR *url )
{
    return view_navigate( view, url );
}

HRESULT WINAPI ArcticHtmlGetTitle( ARCTIC_HTML_VIEW view, WCHAR *buffer, UINT count )
{
    return view_title( view, buffer, count );
}

BOOL WINAPI ArcticHtmlOnGpu( ARCTIC_HTML_VIEW view )
{
    return view_on_gpu( view );
}
