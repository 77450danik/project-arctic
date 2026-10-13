/*
 * Arctic web engine: the view, and the window it owns
 *
 * The engine owns its window, as the engine of Windows does: the class is
 * named "Internet Explorer_Server", so that what looks for the window of a
 * page finds it where it is used to. The host only says where the window
 * goes and where to navigate; the drawing, and later the scrolling and the
 * input, are the engine's own.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "arctichtml_private.h"

static const WCHAR view_class[] = L"Internet Explorer_Server";

struct arctic_html_view
{
    HWND                 window;
    HWND                 host;       /* where the title and the navigation go */
    struct surface       surface;
    struct display_list  list;
    struct present      *present;
    WCHAR                url[2084];
    WCHAR                title[256];
    BOOL                 relayout;   /* the list is not for this size any more */
};

static void set_title( struct arctic_html_view *view, const WCHAR *title )
{
    lstrcpynW( view->title, title, ARRAY_SIZE(view->title) );
    if (view->host) SendMessageW( view->host, ARCTIC_HTML_WM_TITLE, 0, (LPARAM)view->title );
}

/* Stage 0 has no parser: every address shows the engine's own page, which
 * says so and shows the address it was given. Stage 1 loads a document
 * here instead, and the rest of the view does not change. */
static void load( struct arctic_html_view *view )
{
    view->relayout = TRUE;
    set_title( view, L"Arctic" );
    InvalidateRect( view->window, NULL, FALSE );
}

static void layout( struct arctic_html_view *view )
{
    RECT rect;

    GetClientRect( view->window, &rect );
    build_start_page( &view->list, rect.right, rect.bottom, view->url );
    view->relayout = FALSE;
}

static void paint( struct arctic_html_view *view )
{
    RECT rect;
    HDC dc;

    GetClientRect( view->window, &rect );
    if (rect.right <= 0 || rect.bottom <= 0) return;

    if (view->surface.width != rect.right || view->surface.height != rect.bottom)
    {
        surface_destroy( &view->surface );
        if (!surface_create( &view->surface, rect.right, rect.bottom )) return;
        view->relayout = TRUE;
    }
    if (view->relayout) layout( view );

    raster_paint( &view->surface, &view->list );

    if (present_surface( view->present, &view->surface )) return;

    /* the software path, and the one every failure above falls back to */
    if ((dc = GetDC( view->window )))
    {
        BitBlt( dc, 0, 0, view->surface.width, view->surface.height, view->surface.dc, 0, 0, SRCCOPY );
        ReleaseDC( view->window, dc );
    }
}

static LRESULT WINAPI view_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    struct arctic_html_view *view = (struct arctic_html_view *)GetWindowLongPtrW( hwnd, GWLP_USERDATA );

    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;   /* the page covers every pixel: no flicker */

    case WM_PAINT:
    {
        PAINTSTRUCT ps;

        BeginPaint( hwnd, &ps );
        if (view) paint( view );
        EndPaint( hwnd, &ps );
        return 0;
    }
    case WM_SIZE:
        if (view) view->relayout = TRUE;
        return 0;

    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        if (view)
        {
            view->relayout = TRUE;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wp, lp );
}

BOOL view_register_class(void)
{
    WNDCLASSW class = { .style = CS_HREDRAW | CS_VREDRAW, .lpfnWndProc = view_proc,
                        .hInstance = arctichtml_instance,
                        .hCursor = LoadCursorW( 0, (LPCWSTR)IDC_ARROW ),
                        .lpszClassName = view_class };

    if (!RegisterClassW( &class ) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        ERR( "no window class for the engine: %lu\n", GetLastError() );
        return FALSE;
    }
    return TRUE;
}

struct arctic_html_view *view_create( HWND parent, const RECT *rect )
{
    struct arctic_html_view *view;
    RECT where = { 0, 0, 0, 0 };

    if (!view_register_class()) return NULL;
    if (!(view = calloc( 1, sizeof(*view) ))) return NULL;

    if (rect) where = *rect;
    else if (parent) GetClientRect( parent, &where );

    view->host = parent;
    view->window = CreateWindowExW( 0, view_class, NULL, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                                    where.left, where.top, where.right - where.left,
                                    where.bottom - where.top, parent, NULL, arctichtml_instance, NULL );
    if (!view->window)
    {
        ERR( "no window for the engine: %lu\n", GetLastError() );
        free( view );
        return NULL;
    }
    SetWindowLongPtrW( view->window, GWLP_USERDATA, (LONG_PTR)view );

    view->present = present_create( view->window );
    TRACE( "view %p, window %p, %s\n", view, view->window,
           present_on_gpu( view->present ) ? "Vulkan" : "GDI" );

    lstrcpynW( view->url, L"about:blank", ARRAY_SIZE(view->url) );
    load( view );
    return view;
}

void view_destroy( struct arctic_html_view *view )
{
    if (!view) return;
    if (view->window)
    {
        SetWindowLongPtrW( view->window, GWLP_USERDATA, 0 );
        DestroyWindow( view->window );
    }
    present_destroy( view->present );
    surface_destroy( &view->surface );
    display_list_free( &view->list );
    free( view );
}

void view_set_rect( struct arctic_html_view *view, const RECT *rect )
{
    if (!view || !rect) return;
    SetWindowPos( view->window, 0, rect->left, rect->top, rect->right - rect->left,
                  rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE );
    view->relayout = TRUE;
    InvalidateRect( view->window, NULL, FALSE );
}

HWND view_window( struct arctic_html_view *view )
{
    return view ? view->window : NULL;
}

HRESULT view_navigate( struct arctic_html_view *view, const WCHAR *url )
{
    if (!view || !url) return E_INVALIDARG;
    TRACE( "%p %s\n", view, debugstr_w(url) );
    lstrcpynW( view->url, url, ARRAY_SIZE(view->url) );
    load( view );
    return S_OK;
}

HRESULT view_title( struct arctic_html_view *view, WCHAR *buffer, UINT count )
{
    if (!view || !buffer || !count) return E_INVALIDARG;
    lstrcpynW( buffer, view->title, count );
    return S_OK;
}

BOOL view_on_gpu( struct arctic_html_view *view )
{
    return view && present_on_gpu( view->present );
}
