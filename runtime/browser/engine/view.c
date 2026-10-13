/*
 * Arctic web engine: the view, and the window it owns
 *
 * The engine owns its window, as the engine of Windows does: the class is
 * named "Internet Explorer_Server", so that what looks for the window of a
 * page finds it where it is used to. The host only says where the window
 * goes and where to navigate; the drawing, the scrolling and the input are
 * the engine's own.
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

#define LINE_STEP   40      /* an arrow key */
#define WHEEL_STEP  100     /* a notch of the wheel, as browsers on Windows scroll */

struct arctic_html_view
{
    HWND                 window;
    HWND                 host;       /* where the title goes */
    struct surface       surface;
    struct display_list  list;
    struct present      *present;
    struct document     *doc;
    WCHAR                url[2084];
    WCHAR                title[256];
    BOOL                 relayout;   /* the list is not for this width any more */
    int                  scroll_y;
    int                  wheel_rest; /* a part of a notch from a precise wheel */
};

static void set_title( struct arctic_html_view *view, const WCHAR *title )
{
    lstrcpynW( view->title, title, ARRAY_SIZE(view->title) );
    if (view->host) SendMessageW( view->host, ARCTIC_HTML_WM_TITLE, 0, (LPARAM)view->title );
}

static int client_height( struct arctic_html_view *view )
{
    RECT rect;

    GetClientRect( view->window, &rect );
    return rect.bottom;
}

static void update_scrollbar( struct arctic_html_view *view )
{
    SCROLLINFO info = { .cbSize = sizeof(info), .fMask = SIF_RANGE | SIF_PAGE | SIF_POS };
    int page = client_height( view ), limit = max( 0, view->list.content_height - page );

    view->scroll_y = max( 0, min( view->scroll_y, limit ) );
    info.nMax = view->list.content_height > page ? view->list.content_height - 1 : 0;
    info.nPage = page;
    info.nPos = view->scroll_y;
    SetScrollInfo( view->window, SB_VERT, &info, TRUE );
}

static void scroll_to( struct arctic_html_view *view, int y )
{
    int limit = max( 0, view->list.content_height - client_height( view ) );

    y = max( 0, min( y, limit ) );
    if (y == view->scroll_y) return;
    view->scroll_y = y;
    SetScrollPos( view->window, SB_VERT, y, TRUE );
    InvalidateRect( view->window, NULL, FALSE );
}

static void load( struct arctic_html_view *view )
{
    struct document *doc = document_load( view->url );

    /* the old list points at fonts of the old page: both go together */
    display_list_reset( &view->list );
    font_cache_clear();
    document_free( view->doc );
    view->doc = doc;
    view->scroll_y = 0;
    view->relayout = TRUE;
    set_title( view, doc && doc->title[0] ? doc->title : view->url );
    InvalidateRect( view->window, NULL, FALSE );
}

static void layout( struct arctic_html_view *view )
{
    RECT rect;

    GetClientRect( view->window, &rect );
    layout_document( view->doc, &view->list, rect.right, rect.bottom );
    view->relayout = FALSE;
    update_scrollbar( view );
}

static void paint( struct arctic_html_view *view )
{
    RECT rect;
    HDC dc;

    if (view->relayout) layout( view );
    GetClientRect( view->window, &rect );
    if (rect.right <= 0 || rect.bottom <= 0) return;

    if (view->surface.width != rect.right || view->surface.height != rect.bottom)
    {
        surface_destroy( &view->surface );
        if (!surface_create( &view->surface, rect.right, rect.bottom )) return;
    }
    raster_paint( &view->surface, &view->list, view->scroll_y );

    if (present_surface( view->present, &view->surface )) return;

    /* the software path, and the one every failure above falls back to */
    if ((dc = GetDC( view->window )))
    {
        BitBlt( dc, 0, 0, view->surface.width, view->surface.height, view->surface.dc, 0, 0, SRCCOPY );
        ReleaseDC( view->window, dc );
    }
}

static void on_scroll( struct arctic_html_view *view, int code )
{
    SCROLLINFO info = { .cbSize = sizeof(info), .fMask = SIF_TRACKPOS };
    int page = client_height( view );

    switch (code)
    {
    case SB_LINEUP:    scroll_to( view, view->scroll_y - LINE_STEP ); break;
    case SB_LINEDOWN:  scroll_to( view, view->scroll_y + LINE_STEP ); break;
    case SB_PAGEUP:    scroll_to( view, view->scroll_y - max( page - LINE_STEP, LINE_STEP ) ); break;
    case SB_PAGEDOWN:  scroll_to( view, view->scroll_y + max( page - LINE_STEP, LINE_STEP ) ); break;
    case SB_TOP:       scroll_to( view, 0 ); break;
    case SB_BOTTOM:    scroll_to( view, view->list.content_height ); break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION:
        if (GetScrollInfo( view->window, SB_VERT, &info )) scroll_to( view, info.nTrackPos );
        break;
    }
}

static LRESULT WINAPI view_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    struct arctic_html_view *view = (struct arctic_html_view *)GetWindowLongPtrW( hwnd, GWLP_USERDATA );

    if (!view) return DefWindowProcW( hwnd, msg, wp, lp );

    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;   /* the page covers every pixel: no flicker */

    case WM_PAINT:
    {
        PAINTSTRUCT ps;

        BeginPaint( hwnd, &ps );
        paint( view );
        EndPaint( hwnd, &ps );
        return 0;
    }
    case WM_SIZE:
        /* a new width lays the page out again; a new height only shows more of it */
        if (LOWORD(lp) != view->list.width) view->relayout = TRUE;
        else update_scrollbar( view );
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;

    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        view->relayout = TRUE;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;

    case WM_VSCROLL:
        on_scroll( view, LOWORD(wp) );
        return 0;

    case WM_MOUSEWHEEL:
    {
        int delta = GET_WHEEL_DELTA_WPARAM( wp ) + view->wheel_rest;
        int notches = delta / WHEEL_DELTA;

        view->wheel_rest = delta - notches * WHEEL_DELTA;
        scroll_to( view, view->scroll_y - notches * WHEEL_STEP );
        return 0;
    }
    case WM_KEYDOWN:
        switch (wp)
        {
        case VK_UP:    on_scroll( view, SB_LINEUP ); return 0;
        case VK_DOWN:  on_scroll( view, SB_LINEDOWN ); return 0;
        case VK_PRIOR: on_scroll( view, SB_PAGEUP ); return 0;
        case VK_NEXT:
        case VK_SPACE: on_scroll( view, GetKeyState( VK_SHIFT ) < 0 && wp == VK_SPACE ? SB_PAGEUP : SB_PAGEDOWN );
                       return 0;
        case VK_HOME:  on_scroll( view, SB_TOP ); return 0;
        case VK_END:   on_scroll( view, SB_BOTTOM ); return 0;
        }
        break;

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        SetFocus( hwnd );   /* the keys scroll the page from now on */
        return 0;

    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    }
    return DefWindowProcW( hwnd, msg, wp, lp );
}

BOOL view_register_class(void)
{
    WNDCLASSW class = { .lpfnWndProc = view_proc, .hInstance = arctichtml_instance,
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
    view->window = CreateWindowExW( 0, view_class, NULL, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_VSCROLL,
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
    TRACE( "view %p, window %p, %s\n", view, view->window, present_on_gpu( view->present ) ? "Vulkan" : "GDI" );

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
    document_free( view->doc );
    free( view );
}

void view_set_rect( struct arctic_html_view *view, const RECT *rect )
{
    if (!view || !rect) return;
    SetWindowPos( view->window, 0, rect->left, rect->top, rect->right - rect->left,
                  rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE );
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
