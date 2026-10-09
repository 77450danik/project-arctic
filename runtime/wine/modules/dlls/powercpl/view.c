/*
 * Power Options of the Control Panel: the shell view of a page
 *
 * Windows 10 draws Power Options with DirectUI inside File Explorer
 * (powercpl.dll's UIFILEs 101-104): a task pane on the left (white, links in
 * the Control Panel's blue, "Див. також" at the bottom) and the page beside
 * it, at most 600 wide with 10 of padding, 19 from the top: the title, the
 * text, groups with a line after their header, radio buttons, combo boxes,
 * links and buttons. Here it is a shell view of the browser, so the address
 * bar, Back and the window's title are File Explorer's; a page is laid out
 * top to bottom from what it adds (the pages build themselves, page_*.c),
 * with real controls for what takes input.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS

#include <stdlib.h>

#include "powercpl.h"
#include "commctrl.h"
#include "objbase.h"
#include "shlobj.h"
#include "shobjidl.h"

#define NAV_WIDTH      px(208)
#define NAV_LEFT       px(18)
#define NAV_TOP        px(17)
#define CONTENT_TOP    px(19)
#define CONTENT_MAX    px(600)
#define CONTENT_PAD    px(10)

#define COLOR_TITLE    RGB( 0x00, 0x33, 0x99 )   /* the main instruction's blue */
#define COLOR_TEXT     RGB( 0x00, 0x00, 0x00 )
#define COLOR_GRAY     RGB( 0x6d, 0x6d, 0x6d )
#define COLOR_LINK     RGB( 0x00, 0x66, 0xcc )
#define COLOR_LINK_HOT RGB( 0x33, 0x99, 0xff )
#define COLOR_LINE     RGB( 0xe2, 0xe2, 0xe2 )

#define TIMER_PAGE     1

enum item_kind { ITEM_TEXT, ITEM_LINK, ITEM_SPACE, ITEM_GROUP, ITEM_CONTROL, ITEM_PAINT, ITEM_ROW_BEGIN, ITEM_ROW_END };

struct item
{
    enum item_kind kind;
    int style, indent, width, height, x;
    int line;                /* what it takes on the page: a combo box's list is not */
    BOOL single;             /* on a row: one line */
    WCHAR *text;
    link_proc proc;
    UINT_PTR param;
    HWND hwnd;
    paint_proc paint;
    RECT rect;               /* content coordinates, before scrolling */
};

struct nav_item
{
    WCHAR *text;
    link_proc proc;
    UINT_PTR param;
    BOOL see_also;
    RECT rect;
};

struct view
{
    IShellView IShellView_iface;
    LONG ref;
    IShellFolder *folder;
    IShellBrowser *browser;
    ITEMIDLIST *root;            /* Power Options in the namespace */
    FOLDERSETTINGS settings;
    HWND hwnd, content;
    struct page_state state;
    struct item *items;
    UINT count, capacity;
    struct nav_item nav[16];
    UINT nav_count;
    int scroll, content_height, hot, pressed, nav_hot;
    BOOL in_row;
};

#define WM_APP_START_PAGE (WM_APP + 40)

static HFONT fonts[STYLE_BIG + 1];
static HFONT font_link_hot;
static HBRUSH white_brush;

static inline struct view *impl_from_IShellView( IShellView *iface )
{
    return CONTAINING_RECORD( iface, struct view, IShellView_iface );
}

/**********************************************************************
 *          Fonts and helpers
 */

static HFONT make_font( int height, int weight, BOOL underline )
{
    NONCLIENTMETRICSW metrics = { sizeof(metrics) };

    SystemParametersInfoW( SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0 );
    metrics.lfMessageFont.lfHeight = -height;
    metrics.lfMessageFont.lfWeight = weight;
    metrics.lfMessageFont.lfUnderline = underline;
    metrics.lfMessageFont.lfQuality = CLEARTYPE_QUALITY;
    return CreateFontIndirectW( &metrics.lfMessageFont );
}

HFONT view_font( int style )
{
    if (!fonts[STYLE_BODY])
    {
        fonts[STYLE_TITLE] = make_font( px( 16 ), FW_NORMAL, FALSE );
        fonts[STYLE_BODY] = make_font( px( 12 ), FW_NORMAL, FALSE );
        fonts[STYLE_BOLD] = make_font( px( 12 ), FW_BOLD, FALSE );
        fonts[STYLE_GROUP] = make_font( px( 12 ), FW_NORMAL, FALSE );
        fonts[STYLE_SMALL] = make_font( px( 12 ), FW_NORMAL, FALSE );
        fonts[STYLE_GRAY] = make_font( px( 12 ), FW_NORMAL, FALSE );
        fonts[STYLE_BIG] = make_font( px( 40 ), FW_LIGHT, FALSE );
        font_link_hot = make_font( px( 12 ), FW_NORMAL, TRUE );
        white_brush = CreateSolidBrush( RGB( 255, 255, 255 ) );
    }
    return fonts[style];
}

static COLORREF style_color( int style )
{
    switch (style)
    {
    case STYLE_TITLE: return COLOR_TITLE;
    case STYLE_GROUP: return COLOR_TITLE;
    case STYLE_GRAY:
    case STYLE_SMALL: return COLOR_GRAY;
    default: return COLOR_TEXT;
    }
}

static int text_height( HDC hdc, const WCHAR *text, HFONT font, int width, BOOL wrap )
{
    RECT rect = { 0, 0, max( width, 1 ), 0 };
    HGDIOBJ old = SelectObject( hdc, font );

    DrawTextW( hdc, text, -1, &rect, DT_CALCRECT | DT_NOPREFIX | (wrap ? DT_WORDBREAK : DT_SINGLELINE) );
    SelectObject( hdc, old );
    return rect.bottom;
}

static int text_width( HDC hdc, const WCHAR *text, HFONT font )
{
    HGDIOBJ old = SelectObject( hdc, font );
    SIZE size = { 0 };

    GetTextExtentPoint32W( hdc, text, lstrlenW( text ), &size );
    SelectObject( hdc, old );
    return size.cx;
}

struct page_state *view_state( struct view *view )
{
    return &view->state;
}

HWND view_window( struct view *view )
{
    return view->content;
}

int view_content_width( struct view *view )
{
    RECT rect;

    /* the scroll bar may come with the layout: room for it always */
    GetClientRect( view->content, &rect );
    if (!(GetWindowLongW( view->content, GWL_STYLE ) & WS_VSCROLL) || !rect.right) rect.right -= GetSystemMetrics( SM_CXVSCROLL );
    return max( min( rect.right - 2 * CONTENT_PAD, CONTENT_MAX - 2 * CONTENT_PAD ), px( 200 ) );
}

/**********************************************************************
 *          What a page adds
 */

static struct item *add_item( struct view *view, enum item_kind kind )
{
    struct item *item;

    if (view->count == view->capacity)
    {
        UINT capacity = view->capacity ? view->capacity * 2 : 64;
        struct item *items = realloc( view->items, capacity * sizeof(*items) );
        if (!items) return NULL;
        view->items = items;
        view->capacity = capacity;
    }
    item = &view->items[view->count++];
    memset( item, 0, sizeof(*item) );
    item->kind = kind;
    return item;
}

void view_clear( struct view *view )
{
    for (UINT i = 0; i < view->count; i++)
    {
        if (view->items[i].hwnd) DestroyWindow( view->items[i].hwnd );
        free( view->items[i].text );
    }
    view->count = 0;
    for (UINT i = 0; i < view->nav_count; i++) free( view->nav[i].text );
    view->nav_count = 0;
    view->hot = view->pressed = view->nav_hot = -1;
}

void view_text_at( struct view *view, int style, const WCHAR *text, int indent )
{
    struct item *item = add_item( view, ITEM_TEXT );

    if (!item) return;
    item->style = style;
    item->indent = indent;
    item->text = wcsdup( text );
}

void view_text( struct view *view, int style, const WCHAR *text )
{
    view_text_at( view, style, text, 0 );
}

void view_link( struct view *view, const WCHAR *text, link_proc proc, UINT_PTR param, int indent )
{
    struct item *item = add_item( view, ITEM_LINK );

    if (!item) return;
    item->style = STYLE_BODY;
    item->indent = indent;
    item->text = wcsdup( text );
    item->proc = proc;
    item->param = param;
}

void view_space( struct view *view, int height )
{
    struct item *item = add_item( view, ITEM_SPACE );

    if (item) item->height = height;
}

void view_group( struct view *view, const WCHAR *text )
{
    struct item *item = add_item( view, ITEM_GROUP );

    if (!item) return;
    item->style = STYLE_GROUP;
    item->text = wcsdup( text );
}

HWND view_control( struct view *view, const WCHAR *cls, const WCHAR *text, DWORD style, int width, int height,
                   int indent, UINT id )
{
    struct item *item = add_item( view, ITEM_CONTROL );
    HWND hwnd;

    if (!item) return NULL;
    hwnd = CreateWindowExW( 0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, width ? width : 10, height,
                            view->content, (HMENU)(UINT_PTR)id, powercpl_instance, NULL );
    SendMessageW( hwnd, WM_SETFONT, (WPARAM)view_font( STYLE_BODY ), FALSE );
    item->hwnd = hwnd;
    item->width = width;
    item->height = height;
    item->line = !wcsicmp( cls, WC_COMBOBOXW ) ? px( 23 ) : height;
    item->indent = indent;
    return hwnd;
}

/* on the row: at x from the content's left edge */
HWND view_control_beside( struct view *view, const WCHAR *cls, const WCHAR *text, DWORD style, int x, int width,
                          int height, UINT id )
{
    HWND hwnd = view_control( view, cls, text, style, width, height, 0, id );

    if (hwnd) view->items[view->count - 1].x = x;
    return hwnd;
}

void view_paint_area( struct view *view, int height, paint_proc proc )
{
    struct item *item = add_item( view, ITEM_PAINT );

    if (!item) return;
    item->height = height;
    item->paint = proc;
}

void view_row_begin( struct view *view )
{
    add_item( view, ITEM_ROW_BEGIN );
}

void view_row_end( struct view *view )
{
    add_item( view, ITEM_ROW_END );
}

static void add_nav( struct view *view, const WCHAR *text, link_proc proc, UINT_PTR param, BOOL see_also )
{
    struct nav_item *item;

    if (view->nav_count >= ARRAY_SIZE(view->nav)) return;
    item = &view->nav[view->nav_count++];
    memset( item, 0, sizeof(*item) );
    item->text = wcsdup( text );
    item->proc = proc;
    item->param = param;
    item->see_also = see_also;
}

void view_nav_link( struct view *view, const WCHAR *text, link_proc proc, UINT_PTR param )
{
    add_nav( view, text, proc, param, FALSE );
}

void view_nav_see_also( struct view *view, const WCHAR *text, link_proc proc, UINT_PTR param )
{
    add_nav( view, text, proc, param, TRUE );
}

/**********************************************************************
 *          Layout
 */

void view_layout( struct view *view )
{
    HDC hdc = GetDC( view->content );
    int width = view_content_width( view ), y = CONTENT_TOP, row_top = 0, row_height = 0;
    BOOL in_row = FALSE;
    RECT client;
    SCROLLINFO info = { sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS };

    for (UINT i = 0; i < view->count; i++)
    {
        struct item *item = &view->items[i];
        int top = in_row ? row_top : y, height = 0;

        switch (item->kind)
        {
        case ITEM_ROW_BEGIN:
            in_row = TRUE;
            row_top = y;
            row_height = 0;
            continue;
        case ITEM_ROW_END:
            in_row = FALSE;
            y = row_top + row_height;
            /* the labels of the row stand in its middle */
            for (UINT k = i; k > 0 && view->items[k - 1].kind != ITEM_ROW_BEGIN; k--)
            {
                struct item *it = &view->items[k - 1];
                if (it->kind == ITEM_TEXT || it->kind == ITEM_LINK)
                    OffsetRect( &it->rect, 0, (row_height - (it->rect.bottom - it->rect.top)) / 2 );
            }
            continue;
        case ITEM_TEXT:
        case ITEM_LINK:
        {
            HFONT font = view_font( item->style );
            int left = CONTENT_PAD + item->indent;
            int avail = in_row ? text_width( hdc, item->text, font ) + px( 2 ) : width - item->indent;

            height = text_height( hdc, item->text, font, avail, !in_row );
            if (item->kind == ITEM_LINK && !in_row)
                avail = min( avail, text_width( hdc, item->text, font ) + px( 2 ) );
            SetRect( &item->rect, left, top, left + avail, top + height );
            item->single = in_row;
            if (!in_row && item->kind == ITEM_TEXT && item->style == STYLE_TITLE) height += px( 7 );
            break;
        }
        case ITEM_SPACE:
            height = item->height;
            SetRect( &item->rect, CONTENT_PAD, top, CONTENT_PAD + width, top + height );
            break;
        case ITEM_GROUP:
            height = text_height( hdc, item->text, view_font( STYLE_GROUP ), width, FALSE );
            SetRect( &item->rect, CONTENT_PAD, top, CONTENT_PAD + width, top + height );
            height += px( 6 );
            break;
        case ITEM_CONTROL:
        {
            int left = CONTENT_PAD + (in_row ? item->x : item->indent);
            int w = item->width ? item->width : width - item->indent;

            height = item->line;
            SetRect( &item->rect, left, top, left + w, top + height );
            SetWindowPos( item->hwnd, NULL, left, top - view->scroll, w, item->height, SWP_NOZORDER | SWP_NOACTIVATE );
            break;
        }
        case ITEM_PAINT:
            height = item->height;
            SetRect( &item->rect, CONTENT_PAD, top, CONTENT_PAD + width, top + height );
            break;
        }
        if (in_row) row_height = max( row_height, height );
        else y += height;
    }
    ReleaseDC( view->content, hdc );

    view->content_height = y + px( 20 );
    GetClientRect( view->content, &client );
    view->scroll = max( 0, min( view->scroll, view->content_height - client.bottom ) );
    info.nMin = 0;
    info.nMax = view->content_height - 1;
    info.nPage = client.bottom;
    info.nPos = view->scroll;
    SetScrollInfo( view->content, SB_VERT, &info, TRUE );
    InvalidateRect( view->content, NULL, FALSE );
    InvalidateRect( view->hwnd, NULL, TRUE );
}

static void build( struct view *view )
{
    static const page_build_proc builders[PAGE_COUNT] =
    {
        plans_build, edit_build, create_build, system_build, battery_build, graphics_build
    };

    view_clear( view );
    builders[view->state.page]( view );
    view_layout( view );
}

void view_rebuild( struct view *view )
{
    int scroll = view->scroll;

    SendMessageW( view->content, WM_SETREDRAW, FALSE, 0 );
    build( view );
    view->scroll = scroll;
    view_layout( view );
    SendMessageW( view->content, WM_SETREDRAW, TRUE, 0 );
    RedrawWindow( view->content, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN );
}

void view_set_timer( struct view *view, UINT ms )
{
    SetTimer( view->content, TIMER_PAGE, ms, NULL );
}

/**********************************************************************
 *          Navigation
 */

/* the pages are items of the Power Options folder: Back and the address bar follow */
void view_navigate( struct view *view, enum page page, const GUID *scheme, BOOL creating )
{
    ITEMIDLIST *item, *pidl;

    if (!view->browser || !view->root) return;
    if (page == PAGE_PLANS)
    {
        IShellBrowser_BrowseObject( view->browser, view->root, SBSP_ABSOLUTE | SBSP_SAMEBROWSER );
        return;
    }
    if (!(item = page_item_create( page, scheme, creating ))) return;
    if ((pidl = ILCombine( view->root, item )))
    {
        IShellBrowser_BrowseObject( view->browser, pidl, SBSP_ABSOLUTE | SBSP_SAMEBROWSER );
        ILFree( pidl );
    }
    CoTaskMemFree( item );
}

void view_browse_control_panel( struct view *view )
{
    ITEMIDLIST *pidl;

    if (!view->browser || FAILED(SHGetSpecialFolderLocation( NULL, CSIDL_CONTROLS, &pidl ))) return;
    IShellBrowser_BrowseObject( view->browser, pidl, SBSP_ABSOLUTE | SBSP_SAMEBROWSER );
    ILFree( pidl );
}

void view_back( struct view *view )
{
    if (view->browser) IShellBrowser_BrowseObject( view->browser, NULL, SBSP_NAVIGATEBACK | SBSP_SAMEBROWSER );
}

/**********************************************************************
 *          Painting and input
 */

static void paint_content( struct view *view, HDC hdc )
{
    SetBkMode( hdc, TRANSPARENT );
    for (UINT i = 0; i < view->count; i++)
    {
        struct item *item = &view->items[i];
        RECT rect = item->rect;

        OffsetRect( &rect, 0, -view->scroll );
        switch (item->kind)
        {
        case ITEM_TEXT:
        {
            HGDIOBJ old = SelectObject( hdc, view_font( item->style ) );
            SetTextColor( hdc, style_color( item->style ) );
            DrawTextW( hdc, item->text, -1, &rect, DT_NOPREFIX | (item->single ? DT_SINGLELINE : DT_WORDBREAK) );
            SelectObject( hdc, old );
            break;
        }
        case ITEM_LINK:
        {
            BOOL hot = view->hot == (int)i;
            HGDIOBJ old = SelectObject( hdc, hot ? font_link_hot : view_font( STYLE_BODY ) );
            SetTextColor( hdc, hot ? COLOR_LINK_HOT : COLOR_LINK );
            DrawTextW( hdc, item->text, -1, &rect, DT_NOPREFIX | (item->single ? DT_SINGLELINE : DT_WORDBREAK) );
            SelectObject( hdc, old );
            break;
        }
        case ITEM_GROUP:
        {
            HGDIOBJ old = SelectObject( hdc, view_font( STYLE_GROUP ) );
            int width = text_width( hdc, item->text, view_font( STYLE_GROUP ) );
            RECT line = { rect.left + width + px( 6 ), (rect.top + rect.bottom) / 2, rect.right, (rect.top + rect.bottom) / 2 + 1 };
            HBRUSH brush = CreateSolidBrush( COLOR_LINE );

            SetTextColor( hdc, COLOR_TITLE );
            DrawTextW( hdc, item->text, -1, &rect, DT_NOPREFIX | DT_SINGLELINE );
            if (line.left < line.right) FillRect( hdc, &line, brush );
            DeleteObject( brush );
            SelectObject( hdc, old );
            break;
        }
        case ITEM_PAINT:
            if (item->paint) item->paint( view, hdc, &rect );
            break;
        default:
            break;
        }
    }
}

static void paint_nav( struct view *view, HDC hdc )
{
    RECT client;
    int y = NAV_TOP;
    BOOL see_also_shown = FALSE;

    GetClientRect( view->hwnd, &client );
    SetBkMode( hdc, TRANSPARENT );
    /* "Див. також" and its links sit at the bottom */
    for (UINT i = 0; i < view->nav_count; i++)
    {
        struct nav_item *item = &view->nav[i];
        int width = NAV_WIDTH - NAV_LEFT - px( 8 ), height;

        if (item->see_also && !see_also_shown)
        {
            int bottom = 0;
            for (UINT k = i; k < view->nav_count; k++)
                bottom += text_height( hdc, view->nav[k].text, view_font( STYLE_BODY ), width, TRUE ) + px( 8 );
            y = max( y + px( 24 ), client.bottom - bottom - px( 40 ) );
            {
                RECT rect = { NAV_LEFT, y, NAV_LEFT + width, y + px( 16 ) };
                HGDIOBJ old = SelectObject( hdc, view_font( STYLE_BODY ) );
                SetTextColor( hdc, COLOR_GRAY );
                DrawTextW( hdc, load_string( IDS_SEE_ALSO ), -1, &rect, DT_NOPREFIX | DT_SINGLELINE );
                SelectObject( hdc, old );
            }
            y += px( 22 );
            see_also_shown = TRUE;
        }
        height = text_height( hdc, item->text, view_font( STYLE_BODY ), width, TRUE );
        SetRect( &item->rect, NAV_LEFT, y, NAV_LEFT + width, y + height );
        {
            BOOL hot = view->nav_hot == (int)i;
            RECT rect = item->rect;
            HGDIOBJ old = SelectObject( hdc, hot ? font_link_hot : view_font( STYLE_BODY ) );
            SetTextColor( hdc, hot ? COLOR_LINK_HOT : COLOR_LINK );
            DrawTextW( hdc, item->text, -1, &rect, DT_NOPREFIX | DT_WORDBREAK );
            SelectObject( hdc, old );
        }
        y += height + px( 8 );
    }
}

static int link_at( struct view *view, POINT pt )
{
    pt.y += view->scroll;
    for (UINT i = 0; i < view->count; i++)
        if (view->items[i].kind == ITEM_LINK && PtInRect( &view->items[i].rect, pt )) return i;
    return -1;
}

static int nav_at( struct view *view, POINT pt )
{
    for (UINT i = 0; i < view->nav_count; i++)
        if (PtInRect( &view->nav[i].rect, pt )) return i;
    return -1;
}

static void scroll_to( struct view *view, int pos )
{
    RECT client;
    int old = view->scroll;

    GetClientRect( view->content, &client );
    view->scroll = max( 0, min( pos, view->content_height - client.bottom ) );
    if (view->scroll == old) return;
    SetScrollPos( view->content, SB_VERT, view->scroll, TRUE );
    ScrollWindowEx( view->content, 0, old - view->scroll, NULL, NULL, NULL, NULL,
                    SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE );
}

static BOOL page_command( struct view *view, UINT id, UINT code, HWND control )
{
    switch (view->state.page)
    {
    case PAGE_PLANS: return plans_command( view, id, code, control );
    case PAGE_EDIT: return edit_command( view, id, code, control );
    case PAGE_CREATE: return create_command( view, id, code, control );
    case PAGE_SYSTEM: return system_command( view, id, code, control );
    case PAGE_BATTERY: return battery_command( view, id, code, control );
    case PAGE_GRAPHICS: return graphics_command( view, id, code, control );
    default: return FALSE;
    }
}

static LRESULT WINAPI content_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    struct view *view = (struct view *)GetWindowLongPtrW( hwnd, GWLP_USERDATA );

    switch (msg)
    {
    case WM_NCCREATE:
        SetWindowLongPtrW( hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams );
        break;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint( hwnd, &ps ), mem = CreateCompatibleDC( hdc );
        RECT client;
        HBITMAP bitmap;

        /* drawn off screen: the battery page refreshes every two seconds */
        GetClientRect( hwnd, &client );
        if ((bitmap = CreateCompatibleBitmap( hdc, max( client.right, 1 ), max( client.bottom, 1 ) )))
        {
            HGDIOBJ old = SelectObject( mem, bitmap );
            FillRect( mem, &client, white_brush );
            if (view) paint_content( view, mem );
            BitBlt( hdc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY );
            SelectObject( mem, old );
            DeleteObject( bitmap );
        }
        DeleteDC( mem );
        EndPaint( hwnd, &ps );
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (view) view_layout( view );
        return 0;
    case WM_VSCROLL:
    {
        SCROLLINFO info = { sizeof(info), SIF_ALL };
        RECT client;
        int pos;

        if (!view) break;
        GetScrollInfo( hwnd, SB_VERT, &info );
        GetClientRect( hwnd, &client );
        pos = view->scroll;
        switch (LOWORD( wp ))
        {
        case SB_LINEUP: pos -= px( 20 ); break;
        case SB_LINEDOWN: pos += px( 20 ); break;
        case SB_PAGEUP: pos -= client.bottom; break;
        case SB_PAGEDOWN: pos += client.bottom; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = info.nTrackPos; break;
        case SB_TOP: pos = 0; break;
        case SB_BOTTOM: pos = view->content_height; break;
        }
        scroll_to( view, pos );
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (view) scroll_to( view, view->scroll - GET_WHEEL_DELTA_WPARAM( wp ) * px( 60 ) / WHEEL_DELTA );
        return 0;
    case WM_MOUSEMOVE:
    {
        POINT pt = { (short)LOWORD( lp ), (short)HIWORD( lp ) };
        TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, hwnd };
        int hot;

        if (!view) break;
        if ((hot = link_at( view, pt )) != view->hot)
        {
            view->hot = hot;
            InvalidateRect( hwnd, NULL, TRUE );
        }
        TrackMouseEvent( &track );
        return 0;
    }
    case WM_MOUSELEAVE:
        if (view && view->hot >= 0)
        {
            view->hot = -1;
            InvalidateRect( hwnd, NULL, TRUE );
        }
        return 0;
    case WM_SETCURSOR:
        if (view && view->hot >= 0 && (HWND)wp == hwnd)
        {
            SetCursor( LoadCursorW( NULL, (const WCHAR *)IDC_HAND ) );
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
    {
        POINT pt = { (short)LOWORD( lp ), (short)HIWORD( lp ) };
        if (view) view->pressed = link_at( view, pt );
        SetFocus( hwnd );
        return 0;
    }
    case WM_LBUTTONUP:
    {
        POINT pt = { (short)LOWORD( lp ), (short)HIWORD( lp ) };
        int link;

        if (!view) break;
        link = link_at( view, pt );
        if (link >= 0 && link == view->pressed && view->items[link].proc)
            view->items[link].proc( view, view->items[link].param );
        if (view) view->pressed = -1;
        return 0;
    }
    case WM_COMMAND:
        if (view && lp) page_command( view, LOWORD( wp ), HIWORD( wp ), (HWND)lp );
        return 0;
    case WM_HSCROLL:
        /* the brightness slider */
        if (view && lp) page_command( view, GetDlgCtrlID( (HWND)lp ), LOWORD( wp ), (HWND)lp );
        return 0;
    case WM_NOTIFY:
        if (view) page_command( view, ((NMHDR *)lp)->idFrom, ((NMHDR *)lp)->code, ((NMHDR *)lp)->hwndFrom );
        return 0;
    case WM_TIMER:
        if (view && wp == TIMER_PAGE && view->state.page == PAGE_BATTERY) battery_timer( view );
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetBkColor( (HDC)wp, RGB( 255, 255, 255 ) );
        SetTextColor( (HDC)wp, COLOR_TEXT );
        return (LRESULT)white_brush;
    }
    return DefWindowProcW( hwnd, msg, wp, lp );
}

static LRESULT WINAPI view_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    struct view *view = (struct view *)GetWindowLongPtrW( hwnd, GWLP_USERDATA );

    switch (msg)
    {
    case WM_NCCREATE:
        SetWindowLongPtrW( hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams );
        break;
    case WM_APP_START_PAGE:
        if (view && wp != PAGE_PLANS) view_navigate( view, wp, NULL, FALSE );
        return 0;
    case WM_SIZE:
        if (view && view->content)
        {
            int left = min( NAV_WIDTH, LOWORD( lp ) / 3 );
            MoveWindow( view->content, left, 0, max( LOWORD( lp ) - left, 0 ), HIWORD( lp ), TRUE );
            InvalidateRect( hwnd, NULL, TRUE );
        }
        return 0;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint( hwnd, &ps );
        if (view) paint_nav( view, hdc );
        EndPaint( hwnd, &ps );
        return 0;
    }
    case WM_ERASEBKGND:
    {
        RECT rect;
        GetClientRect( hwnd, &rect );
        FillRect( (HDC)wp, &rect, white_brush );
        return 1;
    }
    case WM_MOUSEMOVE:
    {
        POINT pt = { (short)LOWORD( lp ), (short)HIWORD( lp ) };
        TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, hwnd };
        int hot;

        if (!view) break;
        if ((hot = nav_at( view, pt )) != view->nav_hot)
        {
            view->nav_hot = hot;
            InvalidateRect( hwnd, NULL, TRUE );
        }
        TrackMouseEvent( &track );
        return 0;
    }
    case WM_MOUSELEAVE:
        if (view && view->nav_hot >= 0)
        {
            view->nav_hot = -1;
            InvalidateRect( hwnd, NULL, TRUE );
        }
        return 0;
    case WM_SETCURSOR:
        if (view && view->nav_hot >= 0 && (HWND)wp == hwnd)
        {
            SetCursor( LoadCursorW( NULL, (const WCHAR *)IDC_HAND ) );
            return TRUE;
        }
        break;
    case WM_LBUTTONUP:
    {
        POINT pt = { (short)LOWORD( lp ), (short)HIWORD( lp ) };
        int link;

        if (view && (link = nav_at( view, pt )) >= 0 && view->nav[link].proc)
            view->nav[link].proc( view, view->nav[link].param );
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (view) view_rebuild( view );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wp, lp );
}

/**********************************************************************
 *          IShellView
 */

static HRESULT WINAPI view_QueryInterface( IShellView *iface, REFIID riid, void **out )
{
    struct view *view = impl_from_IShellView( iface );

    if (IsEqualIID( riid, &IID_IUnknown ) || IsEqualIID( riid, &IID_IOleWindow ) || IsEqualIID( riid, &IID_IShellView ))
    {
        *out = &view->IShellView_iface;
        IShellView_AddRef( iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI view_AddRef( IShellView *iface )
{
    return InterlockedIncrement( &impl_from_IShellView( iface )->ref );
}

static ULONG WINAPI view_Release( IShellView *iface )
{
    struct view *view = impl_from_IShellView( iface );
    ULONG ref = InterlockedDecrement( &view->ref );

    if (!ref)
    {
        if (view->hwnd) DestroyWindow( view->hwnd );
        view_clear( view );
        free( view->items );
        free( view->state.data );
        if (view->browser) IShellBrowser_Release( view->browser );
        if (view->folder) IShellFolder_Release( view->folder );
        ILFree( view->root );
        free( view );
    }
    return ref;
}

static HRESULT WINAPI view_GetWindow( IShellView *iface, HWND *hwnd )
{
    *hwnd = impl_from_IShellView( iface )->hwnd;
    return *hwnd ? S_OK : E_FAIL;
}

static HRESULT WINAPI view_ContextSensitiveHelp( IShellView *iface, BOOL mode )
{
    return E_NOTIMPL;
}

/* Tab and the arrows move between the page's controls */
static HRESULT WINAPI view_TranslateAccelerator( IShellView *iface, MSG *msg )
{
    struct view *view = impl_from_IShellView( iface );

    if (view->content && (msg->hwnd == view->content || IsChild( view->content, msg->hwnd )) &&
        msg->message == WM_KEYDOWN && (msg->wParam == VK_TAB || msg->wParam == VK_RETURN) &&
        IsDialogMessageW( view->content, msg ))
        return S_OK;
    return S_FALSE;
}

static HRESULT WINAPI view_EnableModeless( IShellView *iface, BOOL enable )
{
    return S_OK;
}

static HRESULT WINAPI view_UIActivate( IShellView *iface, UINT state )
{
    struct view *view = impl_from_IShellView( iface );

    if (state == SVUIA_ACTIVATE_FOCUS && view->content) SetFocus( view->content );
    return S_OK;
}

static HRESULT WINAPI view_Refresh( IShellView *iface )
{
    view_rebuild( impl_from_IShellView( iface ) );
    return S_OK;
}

static HRESULT WINAPI view_CreateViewWindow( IShellView *iface, IShellView *previous, const FOLDERSETTINGS *settings,
                                             IShellBrowser *browser, RECT *rect, HWND *hwnd )
{
    struct view *view = impl_from_IShellView( iface );
    WNDCLASSW cls = { 0 };
    HWND parent;

    *hwnd = NULL;
    if (!browser || FAILED(IShellBrowser_GetWindow( browser, &parent ))) return E_FAIL;
    view->browser = browser;
    IShellBrowser_AddRef( browser );
    if (settings) view->settings = *settings;
    view_font( STYLE_BODY );

    cls.lpfnWndProc = view_proc;
    cls.hInstance = powercpl_instance;
    cls.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_ARROW );
    cls.lpszClassName = L"ArcticPowerOptionsView";
    RegisterClassW( &cls );
    cls.lpfnWndProc = content_proc;
    cls.lpszClassName = L"ArcticPowerOptionsPage";
    RegisterClassW( &cls );

    view->hwnd = CreateWindowExW( 0, L"ArcticPowerOptionsView", NULL, WS_CHILD | WS_CLIPCHILDREN | WS_TABSTOP,
                                  rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, parent,
                                  NULL, powercpl_instance, view );
    if (!view->hwnd) return E_FAIL;
    view->content = CreateWindowExW( WS_EX_CONTROLPARENT, L"ArcticPowerOptionsPage", NULL,
                                     WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_VSCROLL | WS_TABSTOP, 0, 0,
                                     10, 10, view->hwnd, NULL, powercpl_instance, view );
    SendMessageW( view->hwnd, WM_SIZE, 0, MAKELPARAM( rect->right - rect->left, rect->bottom - rect->top ) );
    build( view );
    ShowWindow( view->hwnd, SW_SHOWNA );
    *hwnd = view->hwnd;
    /* "control powercfg.cpl,,battery": on to that page once the window is up */
    if (view->state.page == PAGE_PLANS) PostMessageW( view->hwnd, WM_APP_START_PAGE, start_page_take(), 0 );
    return S_OK;
}

static HRESULT WINAPI view_DestroyViewWindow( IShellView *iface )
{
    struct view *view = impl_from_IShellView( iface );

    view_clear( view );
    if (view->hwnd) DestroyWindow( view->hwnd );
    view->hwnd = view->content = NULL;
    if (view->browser) IShellBrowser_Release( view->browser );
    view->browser = NULL;
    return S_OK;
}

static HRESULT WINAPI view_GetCurrentInfo( IShellView *iface, FOLDERSETTINGS *settings )
{
    *settings = impl_from_IShellView( iface )->settings;
    return S_OK;
}

static HRESULT WINAPI view_AddPropertySheetPages( IShellView *iface, DWORD reserved, LPFNSVADDPROPSHEETPAGE proc, LPARAM lparam )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI view_SaveViewState( IShellView *iface )
{
    return S_OK;
}

static HRESULT WINAPI view_SelectItem( IShellView *iface, PCUITEMID_CHILD item, SVSIF flags )
{
    return E_NOTIMPL;
}

static HRESULT WINAPI view_GetItemObject( IShellView *iface, UINT item, REFIID riid, void **out )
{
    *out = NULL;
    return E_NOINTERFACE;
}

static const IShellViewVtbl view_vtbl =
{
    view_QueryInterface,
    view_AddRef,
    view_Release,
    view_GetWindow,
    view_ContextSensitiveHelp,
    view_TranslateAccelerator,
    view_EnableModeless,
    view_UIActivate,
    view_Refresh,
    view_CreateViewWindow,
    view_DestroyViewWindow,
    view_GetCurrentInfo,
    view_AddPropertySheetPages,
    view_SaveViewState,
    view_SelectItem,
    view_GetItemObject,
};

HRESULT view_create_for( enum page page, const GUID *scheme, BOOL creating, IShellFolder *folder,
                         const ITEMIDLIST *root, REFIID riid, void **out );

HRESULT view_create_for( enum page page, const GUID *scheme, BOOL creating, IShellFolder *folder,
                         const ITEMIDLIST *root, REFIID riid, void **out )
{
    struct view *view;
    HRESULT hr;

    if (!(view = calloc( 1, sizeof(*view) ))) return E_OUTOFMEMORY;
    view->IShellView_iface.lpVtbl = &view_vtbl;
    view->ref = 1;
    view->state.page = page;
    if (scheme) view->state.scheme = *scheme;
    view->state.creating = creating;
    view->hot = view->pressed = view->nav_hot = -1;
    view->folder = folder;
    if (folder) IShellFolder_AddRef( folder );
    view->root = root ? ILClone( root ) : NULL;
    hr = IShellView_QueryInterface( &view->IShellView_iface, riid, out );
    IShellView_Release( &view->IShellView_iface );
    return hr;
}
