/*
 * A page of the Control Panel: the shell view
 *
 * Windows 7 draws the Control Panel's pages with DirectUI inside Explorer
 * (systemcpl.dll's UIFILE 1001, netcenter.dll's 110-114): the task pane on
 * the left, in the light blue gradient of Windows 7, with its links in dark
 * blue, "Див. також" at its bottom; the page beside it, 10 of padding, 19
 * from the top: the title in the Control Panel's blue, sections with a line
 * after their header, labels and values side by side, links. Here it is a
 * shell view of the browser, so the address bar, Back and the window's title
 * are Explorer's; a page is laid out top to bottom from what it adds, with
 * real controls for what takes input. Power Options (powercpl.dll) is drawn
 * the same way, with Windows 10's white task pane.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS

#include <stdlib.h>

#include "cpanel.h"
#include "commctrl.h"
#include "shobjidl.h"
#include "shellapi.h"

#define NAV_WIDTH      px(208)
#define NAV_LEFT       px(18)
#define NAV_TOP        px(17)
#define CONTENT_TOP    px(19)
#define CONTENT_MAX    px(800)
#define CONTENT_PAD    px(10)

/* the task pane of Windows 7: light blue at the top, nearly white below */
#define NAV_TOP_COLOR     RGB( 0xd6, 0xe3, 0xf3 )
#define NAV_BOTTOM_COLOR  RGB( 0xf3, 0xf7, 0xfc )

#define TIMER_PAGE     1
#define WM_APP_START_PAGE (WM_APP + 40)

BOOL cp_start_page_take( UINT *page, DWORD *param );

/* in the module's string table: the task pane's words */
#define IDS_CP_HOME     0xf000
#define IDS_CP_SEE_ALSO 0xf001

enum item_kind { ITEM_TEXT, ITEM_LINK, ITEM_SPACE, ITEM_GROUP, ITEM_CONTROL, ITEM_PAINT, ITEM_ROW_BEGIN, ITEM_ROW_END };

struct item
{
    enum item_kind kind;
    int style, indent, width, height, x;
    int line;                /* what it takes on the page: a combo box's list is not */
    BOOL single;             /* on a row: one line */
    BOOL right;              /* on the right edge */
    BOOL inside;             /* in the paint area before it, at x and height from its corner */
    WCHAR *text;
    HICON icon;
    link_proc proc;
    UINT_PTR param;
    HWND hwnd;
    paint_proc paint;
    RECT rect;               /* content coordinates, before scrolling */
};

struct nav_item
{
    WCHAR *text;
    HICON icon;
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
    ITEMIDLIST *root;            /* the item in the namespace */
    FOLDERSETTINGS settings;
    HWND hwnd, content;
    struct page_state state;
    struct item *items;
    UINT count, capacity;
    struct nav_item nav[16];
    UINT nav_count;
    int scroll, content_height, hot, pressed, nav_hot;
};

static HFONT fonts[STYLE_COUNT];
static HFONT font_link_hot;
static HBRUSH white_brush;

static inline struct view *impl_from_IShellView( IShellView *iface )
{
    return CONTAINING_RECORD( iface, struct view, IShellView_iface );
}

/**********************************************************************
 *          Strings and sizes
 */

WCHAR *load_string( UINT id )
{
    static WCHAR buffers[8][1024];
    static int next;
    WCHAR *buf = buffers[next++ % ARRAY_SIZE(buffers)];

    if (!LoadStringW( cp_instance, id, buf, ARRAY_SIZE(buffers[0]) )) buf[0] = 0;
    return buf;
}

/* Windows' own strings, with their inserts: %1, %1!u!... */
WCHAR *format_string( UINT id, ... )
{
    static WCHAR buffers[4][1024];
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
        fonts[STYLE_SMALL] = make_font( px( 11 ), FW_NORMAL, FALSE );
        fonts[STYLE_GRAY] = make_font( px( 12 ), FW_NORMAL, FALSE );
        fonts[STYLE_BIG] = make_font( px( 26 ), FW_LIGHT, FALSE );
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
    case STYLE_GROUP: return COLOR_GROUP;
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

static int icon_size(void)
{
    return GetSystemMetrics( SM_CXSMICON );
}

HICON cp_shield_icon(void)
{
    static HICON shield;

    if (!shield)
    {
        SHSTOCKICONINFO info = { sizeof(info) };

        if (SUCCEEDED(SHGetStockIconInfo( SIID_SHIELD, SHGSI_ICON | SHGSI_SMALLICON, &info ))) shield = info.hIcon;
        if (!shield) shield = LoadImageW( NULL, (const WCHAR *)IDI_SHIELD, IMAGE_ICON, icon_size(), icon_size(), LR_SHARED );
    }
    return shield;
}

void cp_run( const WCHAR *file, const WCHAR *args )
{
    ShellExecuteW( NULL, NULL, file, args, NULL, SW_SHOWNORMAL );
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
    item->text = wcsdup( text ? text : L"" );
}

void view_text( struct view *view, int style, const WCHAR *text )
{
    view_text_at( view, style, text, 0 );
}

void view_icon_link( struct view *view, HICON icon, const WCHAR *text, link_proc proc, UINT_PTR param, int indent,
                     BOOL right )
{
    struct item *item = add_item( view, ITEM_LINK );

    if (!item) return;
    item->style = STYLE_BODY;
    item->indent = indent;
    item->text = wcsdup( text );
    item->icon = icon;
    item->right = right;
    item->proc = proc;
    item->param = param;
}

void view_link( struct view *view, const WCHAR *text, link_proc proc, UINT_PTR param, int indent )
{
    view_icon_link( view, NULL, text, proc, param, indent, FALSE );
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
                            view->content, (HMENU)(UINT_PTR)id, cp_instance, NULL );
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

void view_paint_area( struct view *view, int height, paint_proc proc, UINT_PTR param )
{
    struct item *item = add_item( view, ITEM_PAINT );

    if (!item) return;
    item->height = height;
    item->paint = proc;
    item->param = param;
}

void view_link_in( struct view *view, HICON icon, const WCHAR *text, link_proc proc, UINT_PTR param, int x, int y )
{
    struct item *item;

    view_icon_link( view, icon, text, proc, param, 0, FALSE );
    item = &view->items[view->count - 1];
    item->inside = TRUE;
    item->x = x;
    item->height = y;
}

void view_row_begin( struct view *view )
{
    add_item( view, ITEM_ROW_BEGIN );
}

void view_row_end( struct view *view )
{
    add_item( view, ITEM_ROW_END );
}

void view_pair( struct view *view, const WCHAR *label, const WCHAR *value, int indent, int label_width )
{
    struct item *item;

    view_text_at( view, STYLE_BODY, label, indent );
    view->items[view->count - 1].width = label_width;
    if (!(item = add_item( view, ITEM_TEXT ))) return;
    item->style = STYLE_BODY;
    item->indent = indent + label_width;
    item->text = wcsdup( value ? value : L"" );
    /* the value goes beside the label, not under it */
    item->x = -1;
}

static void add_nav( struct view *view, HICON icon, const WCHAR *text, link_proc proc, UINT_PTR param, BOOL see_also )
{
    struct nav_item *item;

    if (view->nav_count >= ARRAY_SIZE(view->nav)) return;
    item = &view->nav[view->nav_count++];
    memset( item, 0, sizeof(*item) );
    item->text = wcsdup( text );
    item->icon = icon;
    item->proc = proc;
    item->param = param;
    item->see_also = see_also;
}

void view_nav_link( struct view *view, const WCHAR *text, link_proc proc, UINT_PTR param )
{
    add_nav( view, NULL, text, proc, param, FALSE );
}

void view_nav_icon_link( struct view *view, HICON icon, const WCHAR *text, link_proc proc, UINT_PTR param )
{
    add_nav( view, icon, text, proc, param, FALSE );
}

void view_nav_see_also( struct view *view, const WCHAR *text, link_proc proc, UINT_PTR param )
{
    add_nav( view, NULL, text, proc, param, TRUE );
}

static void go_home( struct view *view, UINT_PTR param )
{
    view_browse_control_panel( view );
}

void view_nav_home( struct view *view )
{
    view_nav_link( view, load_string( IDS_CP_HOME ), go_home, 0 );
}

/**********************************************************************
 *          Layout
 */

void view_layout( struct view *view )
{
    HDC hdc = GetDC( view->content );
    int width = view_content_width( view ), y = CONTENT_TOP, row_top = 0, row_height = 0, paint_top = 0;
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
            int extra = item->icon ? icon_size() + px( 4 ) : 0;
            int left = CONTENT_PAD + item->indent;
            int avail;

            /* a link in a paint area: where the area's painting leaves room for it */
            if (item->inside)
            {
                int left = CONTENT_PAD + item->x, w = text_width( hdc, item->text, font ) + px( 2 ) + extra;
                int h = max( text_height( hdc, item->text, font, w, FALSE ), item->icon ? icon_size() : 0 );

                SetRect( &item->rect, left, paint_top + item->height, left + min( w, width - item->x ),
                         paint_top + item->height + h );
                item->single = TRUE;
                continue;
            }
            /* the value of a pair: beside its label, on the label's line */
            if (item->kind == ITEM_TEXT && item->x == -1 && i > 0)
            {
                struct item *label = &view->items[i - 1];
                int label_height = label->rect.bottom - label->rect.top;

                top = label->rect.top;
                avail = width - item->indent;
                height = text_height( hdc, item->text, font, avail, TRUE );
                SetRect( &item->rect, left, top, left + avail, top + height );
                /* the pair takes what the taller of the two needs */
                if (!in_row && height > label_height) y += height - label_height;
                if (in_row) row_height = max( row_height, height );
                continue;
            }
            if (item->width && item->kind == ITEM_TEXT) avail = item->width - px( 4 );
            else avail = in_row ? text_width( hdc, item->text, font ) + px( 2 ) : width - item->indent - extra;

            height = text_height( hdc, item->text, font, avail, !in_row );
            if (item->kind == ITEM_LINK && !in_row)
                avail = min( avail, text_width( hdc, item->text, font ) + px( 2 ) );
            height = max( height, item->icon ? icon_size() : 0 );
            if (item->right) left = CONTENT_PAD + width - avail - extra;
            SetRect( &item->rect, left, top, left + extra + avail, top + height );
            item->single = in_row;
            if (!in_row && item->kind == ITEM_TEXT && item->style == STYLE_TITLE) height += px( 7 );
            /* a link on the right edge stands beside what comes after it */
            if (item->right && !in_row) height = 0;
            break;
        }
        case ITEM_SPACE:
            height = item->height;
            SetRect( &item->rect, CONTENT_PAD, top, CONTENT_PAD + width, top + height );
            break;
        case ITEM_GROUP:
            height = text_height( hdc, item->text, view_font( STYLE_GROUP ), width, FALSE );
            SetRect( &item->rect, CONTENT_PAD, top, CONTENT_PAD + width, top + height );
            height += px( 10 );
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
            paint_top = top;
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
    view_clear( view );
    if (view->state.page < cp_page_count && cp_pages[view->state.page].build)
        cp_pages[view->state.page].build( view );
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

void *cp_page_item( UINT page, DWORD param );

/* the pages are items of the folder: Back and the address bar follow */
void view_navigate( struct view *view, UINT page, DWORD param )
{
    ITEMIDLIST *item, *pidl;

    if (!view->browser || !view->root) return;
    if (!page)
    {
        IShellBrowser_BrowseObject( view->browser, view->root, SBSP_ABSOLUTE | SBSP_SAMEBROWSER );
        return;
    }
    if (!(item = cp_page_item( page, param ))) return;
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

static void draw_link( HDC hdc, const struct item *item, const RECT *where, BOOL hot )
{
    RECT rect = *where;
    HGDIOBJ old = SelectObject( hdc, hot ? font_link_hot : view_font( STYLE_BODY ) );

    if (item->icon)
    {
        int size = icon_size();
        DrawIconEx( hdc, rect.left, rect.top + (rect.bottom - rect.top - size) / 2, item->icon, size, size, 0, NULL,
                    DI_NORMAL );
        rect.left += size + px( 4 );
        if (item->single || rect.bottom - rect.top <= size)
        {
            int text = text_height( hdc, item->text, view_font( STYLE_BODY ), rect.right - rect.left, FALSE );
            rect.top += (rect.bottom - rect.top - text) / 2;
        }
    }
    SetTextColor( hdc, hot ? COLOR_LINK_HOT : COLOR_LINK );
    DrawTextW( hdc, item->text, -1, &rect, DT_NOPREFIX | (item->single ? DT_SINGLELINE : DT_WORDBREAK) );
    SelectObject( hdc, old );
}

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
            draw_link( hdc, item, &rect, view->hot == (int)i );
            break;
        case ITEM_GROUP:
        {
            HGDIOBJ old = SelectObject( hdc, view_font( STYLE_GROUP ) );
            int width = text_width( hdc, item->text, view_font( STYLE_GROUP ) );
            RECT line = { rect.left + width + px( 6 ), (rect.top + rect.bottom) / 2, rect.right, (rect.top + rect.bottom) / 2 + 1 };
            HBRUSH brush = CreateSolidBrush( COLOR_LINE );

            SetTextColor( hdc, COLOR_GROUP );
            DrawTextW( hdc, item->text, -1, &rect, DT_NOPREFIX | DT_SINGLELINE );
            if (line.left < line.right) FillRect( hdc, &line, brush );
            DeleteObject( brush );
            SelectObject( hdc, old );
            break;
        }
        case ITEM_PAINT:
            if (item->paint) item->paint( view, hdc, &rect, item->param );
            break;
        default:
            break;
        }
    }
}

static void paint_nav_background( HDC hdc, const RECT *rect )
{
    TRIVERTEX vertex[2] =
    {
        { rect->left, rect->top, GetRValue( NAV_TOP_COLOR ) << 8, GetGValue( NAV_TOP_COLOR ) << 8,
          GetBValue( NAV_TOP_COLOR ) << 8, 0 },
        { rect->right, rect->bottom, GetRValue( NAV_BOTTOM_COLOR ) << 8, GetGValue( NAV_BOTTOM_COLOR ) << 8,
          GetBValue( NAV_BOTTOM_COLOR ) << 8, 0 },
    };
    GRADIENT_RECT gradient = { 0, 1 };

    GdiGradientFill( hdc, vertex, 2, &gradient, 1, GRADIENT_FILL_RECT_V );
}

static void paint_nav( struct view *view, HDC hdc )
{
    RECT client;
    int y = NAV_TOP, size = icon_size();
    BOOL see_also_shown = FALSE;

    GetClientRect( view->hwnd, &client );
    SetBkMode( hdc, TRANSPARENT );
    /* "Див. також" and its links sit at the bottom */
    for (UINT i = 0; i < view->nav_count; i++)
    {
        struct nav_item *item = &view->nav[i];
        int left = NAV_LEFT, width = NAV_WIDTH - NAV_LEFT - px( 8 ), height;

        if (item->see_also && !see_also_shown)
        {
            int bottom = 0;
            for (UINT k = i; k < view->nav_count; k++)
                bottom += text_height( hdc, view->nav[k].text, view_font( STYLE_BODY ), width, TRUE ) + px( 8 );
            y = max( y + px( 24 ), client.bottom - bottom - px( 40 ) );
            {
                RECT rect = { NAV_LEFT, y, NAV_LEFT + width, y + px( 16 ) };
                HGDIOBJ old = SelectObject( hdc, view_font( STYLE_BODY ) );
                SetTextColor( hdc, COLOR_GROUP );
                DrawTextW( hdc, load_string( IDS_CP_SEE_ALSO ), -1, &rect, DT_NOPREFIX | DT_SINGLELINE );
                SelectObject( hdc, old );
            }
            y += px( 22 );
            see_also_shown = TRUE;
        }
        /* a task with a shield: the icon left of its words */
        if (item->icon)
        {
            DrawIconEx( hdc, left, y, item->icon, size, size, 0, NULL, DI_NORMAL );
            left += size + px( 4 );
            width -= size + px( 4 );
        }
        height = text_height( hdc, item->text, view_font( STYLE_BODY ), width, TRUE );
        SetRect( &item->rect, left, y, left + width, y + height );
        {
            BOOL hot = view->nav_hot == (int)i;
            RECT rect = item->rect;
            HGDIOBJ old = SelectObject( hdc, hot ? font_link_hot : view_font( STYLE_BODY ) );
            SetTextColor( hdc, hot ? COLOR_LINK_HOT : COLOR_NAV_LINK );
            DrawTextW( hdc, item->text, -1, &rect, DT_NOPREFIX | DT_WORDBREAK );
            SelectObject( hdc, old );
        }
        y += max( height, item->icon ? size : 0 ) + px( 8 );
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
    if (view->state.page >= cp_page_count || !cp_pages[view->state.page].command) return FALSE;
    return cp_pages[view->state.page].command( view, id, code, control );
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

        /* drawn off screen: pages that refresh themselves do not flicker */
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
        if (view && lp) page_command( view, GetDlgCtrlID( (HWND)lp ), LOWORD( wp ), (HWND)lp );
        return 0;
    case WM_NOTIFY:
        if (view) page_command( view, ((NMHDR *)lp)->idFrom, ((NMHDR *)lp)->code, ((NMHDR *)lp)->hwndFrom );
        return 0;
    case WM_TIMER:
        if (view && wp == TIMER_PAGE && view->state.page < cp_page_count && cp_pages[view->state.page].timer)
            cp_pages[view->state.page].timer( view );
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
    {
        UINT page;
        DWORD param;

        if (view && cp_start_page_take( &page, &param )) view_navigate( view, page, param );
        return 0;
    }
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
        HDC hdc = BeginPaint( hwnd, &ps ), mem = CreateCompatibleDC( hdc );
        RECT client;
        HBITMAP bitmap;

        GetClientRect( hwnd, &client );
        if ((bitmap = CreateCompatibleBitmap( hdc, max( client.right, 1 ), max( client.bottom, 1 ) )))
        {
            HGDIOBJ old = SelectObject( mem, bitmap );
            paint_nav_background( mem, &client );
            if (view) paint_nav( view, mem );
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
    case WM_MOUSEMOVE:
    {
        POINT pt = { (short)LOWORD( lp ), (short)HIWORD( lp ) };
        TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, hwnd };
        int hot;

        if (!view) break;
        if ((hot = nav_at( view, pt )) != view->nav_hot)
        {
            view->nav_hot = hot;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        TrackMouseEvent( &track );
        return 0;
    }
    case WM_MOUSELEAVE:
        if (view && view->nav_hot >= 0)
        {
            view->nav_hot = -1;
            InvalidateRect( hwnd, NULL, FALSE );
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

/* Tab and Enter move between and press the page's controls */
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
    WCHAR view_class[64], page_class[64];
    WNDCLASSW cls = { 0 };
    HWND parent;

    *hwnd = NULL;
    if (!browser || FAILED(IShellBrowser_GetWindow( browser, &parent ))) return E_FAIL;
    view->browser = browser;
    IShellBrowser_AddRef( browser );
    if (settings) view->settings = *settings;
    view_font( STYLE_BODY );

    swprintf( view_class, ARRAY_SIZE(view_class), L"Arctic%sView", cp_class_name );
    swprintf( page_class, ARRAY_SIZE(page_class), L"Arctic%sPage", cp_class_name );
    cls.lpfnWndProc = view_proc;
    cls.hInstance = cp_instance;
    cls.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_ARROW );
    cls.lpszClassName = view_class;
    RegisterClassW( &cls );
    cls.lpfnWndProc = content_proc;
    cls.lpszClassName = page_class;
    RegisterClassW( &cls );

    view->hwnd = CreateWindowExW( 0, view_class, NULL, WS_CHILD | WS_CLIPCHILDREN | WS_TABSTOP,
                                  rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, parent,
                                  NULL, cp_instance, view );
    if (!view->hwnd) return E_FAIL;
    view->content = CreateWindowExW( WS_EX_CONTROLPARENT, page_class, NULL,
                                     WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_VSCROLL | WS_TABSTOP, 0, 0,
                                     10, 10, view->hwnd, NULL, cp_instance, view );
    SendMessageW( view->hwnd, WM_SIZE, 0, MAKELPARAM( rect->right - rect->left, rect->bottom - rect->top ) );
    build( view );
    ShowWindow( view->hwnd, SW_SHOWNA );
    *hwnd = view->hwnd;
    /* opened for one of its pages (cp_open): on to it once the window is up */
    if (!view->state.page) PostMessageW( view->hwnd, WM_APP_START_PAGE, 0, 0 );
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

HRESULT cp_view_create( UINT page, DWORD param, IShellFolder *folder, const ITEMIDLIST *root, REFIID riid, void **out );

HRESULT cp_view_create( UINT page, DWORD param, IShellFolder *folder, const ITEMIDLIST *root, REFIID riid, void **out )
{
    struct view *view;
    HRESULT hr;

    if (!(view = calloc( 1, sizeof(*view) ))) return E_OUTOFMEMORY;
    view->IShellView_iface.lpVtbl = &view_vtbl;
    view->ref = 1;
    view->state.page = page;
    view->state.param = param;
    view->hot = view->pressed = view->nav_hot = -1;
    view->folder = folder;
    if (folder) IShellFolder_AddRef( folder );
    view->root = root ? ILClone( root ) : NULL;
    hr = IShellView_QueryInterface( &view->IShellView_iface, riid, out );
    IShellView_Release( &view->IShellView_iface );
    return hr;
}
