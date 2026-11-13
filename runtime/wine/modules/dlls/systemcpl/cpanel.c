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
#define NAV_TOP_COLOR     (cp_dark() ? RGB( 0x2c, 0x2c, 0x2c ) : RGB( 0xd6, 0xe3, 0xf3 ))
#define NAV_BOTTOM_COLOR  (cp_dark() ? RGB( 0x20, 0x20, 0x20 ) : RGB( 0xf3, 0xf7, 0xfc ))

#define TIMER_PAGE     1
#define TIMER_REBUILD  2
#define WM_APP_START_PAGE (WM_APP + 40)

BOOL cp_start_page_take( UINT *page, DWORD *param );

/* in the module's string table: the task pane's words */
#define IDS_CP_HOME     0xf000
#define IDS_CP_SEE_ALSO 0xf001

enum item_kind { ITEM_TEXT, ITEM_LINK, ITEM_SPACE, ITEM_GROUP, ITEM_CONTROL, ITEM_PAINT, ITEM_ROW_BEGIN, ITEM_ROW_END,
                 ITEM_HEADING, ITEM_SECTION, ITEM_CARD };

/* a card's style: a row, or an area the page draws in and puts controls in */
#define CARD_AREA 1

struct item
{
    enum item_kind kind;
    int style, indent, width, height, x;
    int line;                /* what it takes on the page: a combo box's list is not */
    BOOL single;             /* on a row: one line */
    BOOL right;              /* on the right edge */
    BOOL inside;             /* in the paint area before it, at x and height from its corner */
    BOOL extra;              /* in the card before it, at its right, left of its controls */
    int flow;                /* a card: its controls flow, this far apart */
    WCHAR *text;
    WCHAR *sub;              /* a card's line under its title */
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
    int nav_x;                   /* where the task pane begins: on the right beside Explorer's own list */
    HWND sheet, sheet_apply;     /* a property page of another module, and its Apply (view_sheet) */
    BOOL sheet_changed;          /* it has changes not applied yet */
    const NMHDR *notify;         /* the WM_NOTIFY the page is told of, while it is */
    BOOL win11;                  /* drawn as Windows 11's Settings: cards on a grey page */
};

/* what the WM_NOTIFY given to the page's command carries (a list's column) */
const NMHDR *view_notify( struct view *view )
{
    return view->notify;
}

/* File Explorer shows the Control Panel's items on the left (startui.dll):
 * the task pane goes to the right then */
static BOOL nav_on_right( struct view *view )
{
    HWND root = GetAncestor( view->hwnd, GA_ROOT );

    return root && GetPropW( root, L"ArcticExplorerSide" ) != NULL;
}

static HFONT fonts[STYLE_COUNT];
static HFONT font_link_hot;

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
    /* Windows 11's cards stand wider */
    return max( min( rect.right - 2 * CONTENT_PAD, (view->win11 ? px( 1000 ) : CONTENT_MAX) - 2 * CONTENT_PAD ), px( 200 ) );
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
    /* left with changes not applied: what Cancel of its dialog did (a
     * preview of the taskbar's look goes back) */
    if (view->sheet && view->sheet_changed) SendMessageW( view->sheet, PSM_PRESSBUTTON, PSBTN_CANCEL, 0 );
    view->sheet = view->sheet_apply = NULL;
    view->sheet_changed = FALSE;
    for (UINT i = 0; i < view->count; i++)
    {
        if (view->items[i].hwnd) DestroyWindow( view->items[i].hwnd );
        free( view->items[i].text );
        free( view->items[i].sub );
    }
    view->count = 0;
    view->win11 = FALSE;
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

/**********************************************************************
 *          Windows 11's Settings: cards
 *
 * A page drawn as Windows 11's Settings: its title large, a heading over a
 * few cards, each card a rounded row with a large icon (the module's own, so
 * that the icon theme gives its picture: user32 takes it from the theme's
 * folder), its name and a line under it, its control at its right edge; a
 * card that goes to another page has a chevron instead and lights up under
 * the pointer.
 */

static HFONT font_heading, font_section, font_card, font_card_sub, font_chevron;

static void card_fonts(void)
{
    if (font_card) return;
    font_heading = make_font( px( 28 ), FW_SEMIBOLD, FALSE );
    font_section = make_font( px( 14 ), FW_SEMIBOLD, FALSE );
    font_card = make_font( px( 14 ), FW_NORMAL, FALSE );
    font_card_sub = make_font( px( 12 ), FW_NORMAL, FALSE );
    font_chevron = make_font( px( 20 ), FW_NORMAL, FALSE );
}

static HBRUSH cached_brush( COLORREF color )
{
    static struct { COLORREF color; HBRUSH brush; } cache[8];
    static int next;
    HBRUSH brush;

    for (int i = 0; i < ARRAY_SIZE(cache); i++)
        if (cache[i].brush && cache[i].color == color) return cache[i].brush;
    if (cache[next].brush) DeleteObject( cache[next].brush );
    cache[next].color = color;
    brush = cache[next].brush = CreateSolidBrush( color );
    next = (next + 1) % ARRAY_SIZE(cache);
    return brush;
}

static COLORREF back_color( struct view *view )
{
    return view && view->win11 ? COLOR_PAGE11 : COLOR_PAGE;
}

void view_heading( struct view *view, const WCHAR *text )
{
    struct item *item = add_item( view, ITEM_HEADING );

    view->win11 = TRUE;
    card_fonts();
    if (item) item->text = wcsdup( text ? text : L"" );
}

void view_section( struct view *view, const WCHAR *text )
{
    struct item *item = add_item( view, ITEM_SECTION );

    card_fonts();
    if (item) item->text = wcsdup( text ? text : L"" );
}

static struct { UINT id; int size; HICON icon; } icon_cache[64];

static BOOL icons_stale;

/* the icons of the theme there was: those of the new one are loaded anew */
static void icons_flush(void)
{
    for (int i = 0; i < ARRAY_SIZE(icon_cache); i++)
    {
        if (icon_cache[i].icon) DestroyIcon( icon_cache[i].icon );
        icon_cache[i].icon = NULL;
    }
    icons_stale = FALSE;
}

HICON view_icon( UINT id )
{
    int size = px( 32 );
    HICON icon;

    for (int i = 0; i < ARRAY_SIZE(icon_cache); i++)
        if (icon_cache[i].icon && icon_cache[i].id == id && icon_cache[i].size == size) return icon_cache[i].icon;
    if (!(icon = LoadImageW( cp_instance, MAKEINTRESOURCEW( id ), IMAGE_ICON, size, size, LR_DEFAULTCOLOR ))) return NULL;
    for (int i = 0; i < ARRAY_SIZE(icon_cache); i++)
        if (!icon_cache[i].icon)
        {
            icon_cache[i].id = id;
            icon_cache[i].size = size;
            icon_cache[i].icon = icon;
            break;
        }
    return icon;
}

static struct item *add_card( struct view *view, HICON icon, const WCHAR *title, const WCHAR *text )
{
    struct item *item = add_item( view, ITEM_CARD );

    card_fonts();
    if (!item) return NULL;
    item->icon = icon;
    item->text = wcsdup( title ? title : L"" );
    item->sub = text && text[0] ? wcsdup( text ) : NULL;
    return item;
}

HWND view_card( struct view *view, HICON icon, const WCHAR *title, const WCHAR *text, const WCHAR *cls,
                const WCHAR *control_text, DWORD style, int width, int height, UINT id )
{
    struct item *item = add_card( view, icon, title, text );
    HWND hwnd;

    if (!item) return NULL;
    if (!cls) return NULL;
    hwnd = CreateWindowExW( 0, cls, control_text ? control_text : L"", WS_CHILD | WS_VISIBLE | style, 0, 0, width, height,
                            view->content, (HMENU)(UINT_PTR)id, cp_instance, NULL );
    SendMessageW( hwnd, WM_SETFONT, (WPARAM)view_font( STYLE_BODY ), FALSE );
    item->hwnd = hwnd;
    item->width = width;
    item->height = height;
    item->line = !wcsicmp( cls, WC_COMBOBOXW ) ? px( 23 ) : height;
    return hwnd;
}

void view_card_link( struct view *view, HICON icon, const WCHAR *title, const WCHAR *text, link_proc proc, UINT_PTR param )
{
    struct item *item = add_card( view, icon, title, text );

    if (!item) return;
    item->proc = proc;
    item->param = param;
}

void view_card_area( struct view *view, int height, paint_proc proc, UINT_PTR param )
{
    struct item *item = add_card( view, NULL, L"", NULL );

    if (!item) return;
    item->style = CARD_AREA;
    item->paint = proc;
    item->param = param;
    item->height = height;
}

static void tile_register(void);

/* a control laid out inside the last card area, at x, y from its inner corner */
HWND view_card_control( struct view *view, const WCHAR *cls, const WCHAR *text, DWORD style, int x, int y, int width,
                        int height, UINT id )
{
    struct item *item;
    HWND hwnd;

    /* a tile may be the first of the page */
    if (!wcsicmp( cls, L"ArcticTile" )) tile_register();
    if (!(item = add_item( view, ITEM_CONTROL ))) return NULL;
    hwnd = CreateWindowExW( 0, cls, text ? text : L"", WS_CHILD | WS_VISIBLE | style, 0, 0, width, height, view->content,
                            (HMENU)(UINT_PTR)id, cp_instance, NULL );
    SendMessageW( hwnd, WM_SETFONT, (WPARAM)view_font( STYLE_BODY ), FALSE );
    item->hwnd = hwnd;
    item->width = width;
    item->height = item->line = height;
    item->inside = TRUE;
    item->x = x;
    item->indent = y;
    return hwnd;
}

static struct item *last_card( struct view *view )
{
    for (UINT i = view->count; i > 0; i--)
        if (view->items[i - 1].kind == ITEM_CARD) return &view->items[i - 1];
    return NULL;
}

HWND view_card_extra( struct view *view, const WCHAR *cls, const WCHAR *text, DWORD style, int width, int height,
                      UINT id )
{
    struct item *card = last_card( view );
    HWND hwnd = card ? view_card_control( view, cls, text, style, 0, 0, width, height, id ) : NULL;

    if (!hwnd) return NULL;
    view->items[view->count - 1].extra = TRUE;
    view->items[view->count - 1].line = !wcsicmp( cls, WC_COMBOBOXW ) ? px( 23 ) : height;
    /* the card's words end before it (paint_card) */
    card = last_card( view );
    card->x += width + px( 8 );
    return hwnd;
}

void view_card_flow( struct view *view, int gap )
{
    struct item *item = add_card( view, NULL, L"", NULL );

    if (item) item->flow = max( gap, 1 );
}

HBRUSH view_brush( COLORREF color )
{
    return cached_brush( color );
}

COLORREF view_accent(void)
{
    static HRESULT (WINAPI *get_color)( DWORD *, BOOL * );
    static BOOL loaded;
    DWORD color;
    BOOL opaque;

    if (!loaded)
    {
        HMODULE dwmapi = LoadLibraryW( L"dwmapi.dll" );

        if (dwmapi) get_color = (void *)GetProcAddress( dwmapi, "DwmGetColorizationColor" );
        loaded = TRUE;
    }
    if (get_color && SUCCEEDED(get_color( &color, &opaque )) && (color & 0xffffff))
        return RGB( (color >> 16) & 0xff, (color >> 8) & 0xff, color & 0xff );
    return RGB( 0x00, 0x78, 0xd4 );
}

/* the card a control stands in, for its colours */
static const struct item *card_of( struct view *view, HWND hwnd )
{
    const struct item *card = NULL;

    for (UINT i = 0; i < view->count; i++)
    {
        const struct item *item = &view->items[i];
        BOOL in_card = item->kind == ITEM_CONTROL && item->inside;

        if (item->kind == ITEM_CARD) card = item;
        else if (!in_card) card = NULL;
        if (item->hwnd == hwnd) return item->kind == ITEM_CARD || in_card ? card : NULL;
    }
    return NULL;
}

/**********************************************************************
 *          The switch of Windows 11
 *
 * "Увімкнуто" or "Вимкнуто" and the switch at the right, as a check box: it
 * answers BM_GETCHECK and BM_SETCHECK and says BN_CLICKED.
 */

#define TOGGLE_CLASS    L"ArcticToggle"
#define IDS_CP_ON       0xf003
#define IDS_CP_OFF      0xf004

static LRESULT WINAPI toggle_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    BOOL on = GetWindowLongPtrW( hwnd, GWLP_USERDATA ) != 0;

    switch (msg)
    {
    case BM_GETCHECK:
        return on ? BST_CHECKED : BST_UNCHECKED;
    case BM_SETCHECK:
        SetWindowLongPtrW( hwnd, GWLP_USERDATA, wp == BST_CHECKED );
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    case WM_GETDLGCODE:
        return DLGC_BUTTON;
    case WM_LBUTTONDOWN:
        SetFocus( hwnd );
        SetCapture( hwnd );
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() != hwnd) return 0;
        ReleaseCapture();
        if (IsWindowEnabled( hwnd ))
        {
            SetWindowLongPtrW( hwnd, GWLP_USERDATA, !on );
            InvalidateRect( hwnd, NULL, FALSE );
            SendMessageW( GetParent( hwnd ), WM_COMMAND, MAKEWPARAM( GetDlgCtrlID( hwnd ), BN_CLICKED ), (LPARAM)hwnd );
        }
        return 0;
    case WM_KEYUP:
        if (wp == VK_SPACE && IsWindowEnabled( hwnd ))
        {
            SetWindowLongPtrW( hwnd, GWLP_USERDATA, !on );
            InvalidateRect( hwnd, NULL, FALSE );
            SendMessageW( GetParent( hwnd ), WM_COMMAND, MAKEWPARAM( GetDlgCtrlID( hwnd ), BN_CLICKED ), (LPARAM)hwnd );
        }
        return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
        InvalidateRect( hwnd, NULL, FALSE );
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint( hwnd, &ps );
        RECT client, label;
        int w = px( 40 ), h = px( 20 ), x, y, knob;
        BOOL enabled = IsWindowEnabled( hwnd );
        COLORREF accent = view_accent(), ink = cp_dark() ? RGB( 0xcf, 0xcf, 0xcf ) : RGB( 0x5d, 0x5d, 0x5d );
        HGDIOBJ old_brush, old_pen;
        HPEN pen;

        GetClientRect( hwnd, &client );
        FillRect( hdc, &client, cached_brush( COLOR_CARD ) );
        x = client.right - w - px( 2 );
        y = (client.bottom - h) / 2;
        label = client;
        label.right = x - px( 10 );
        SetBkMode( hdc, TRANSPARENT );
        SetTextColor( hdc, enabled ? COLOR_TEXT : COLOR_GRAY );
        SelectObject( hdc, font_card ? font_card : view_font( STYLE_BODY ) );
        DrawTextW( hdc, load_string( on ? IDS_CP_ON : IDS_CP_OFF ), -1, &label, DT_RIGHT | DT_VCENTER | DT_SINGLELINE );

        pen = CreatePen( PS_SOLID, 1, on && enabled ? accent : ink );
        old_pen = SelectObject( hdc, pen );
        old_brush = SelectObject( hdc, cached_brush( on && enabled ? accent : COLOR_CARD ) );
        RoundRect( hdc, x, y, x + w, y + h, h, h );
        knob = h - px( 8 );
        SelectObject( hdc, cached_brush( on && enabled ? RGB( 0xff, 0xff, 0xff ) : ink ) );
        SelectObject( hdc, GetStockObject( NULL_PEN ) );
        {
            int kx = on ? x + w - px( 4 ) - knob : x + px( 4 ), ky = y + px( 4 );
            Ellipse( hdc, kx, ky, kx + knob + 1, ky + knob + 1 );
        }
        SelectObject( hdc, old_brush );
        SelectObject( hdc, old_pen );
        DeleteObject( pen );
        if (GetFocus() == hwnd)
        {
            RECT focus = { x - px( 3 ), y - px( 3 ), x + w + px( 3 ), y + h + px( 3 ) };
            DrawFocusRect( hdc, &focus );
        }
        EndPaint( hwnd, &ps );
        return 0;
    }
    }
    return DefWindowProcW( hwnd, msg, wp, lp );
}

HWND view_card_toggle( struct view *view, HICON icon, const WCHAR *title, const WCHAR *text, BOOL on, UINT id )
{
    static BOOL registered;
    HWND hwnd;

    if (!registered)
    {
        WNDCLASSW cls = { 0 };

        cls.lpfnWndProc = toggle_proc;
        cls.hInstance = cp_instance;
        cls.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_ARROW );
        cls.lpszClassName = TOGGLE_CLASS;
        registered = RegisterClassW( &cls ) != 0;
    }
    hwnd = view_card( view, icon, title, text, TOGGLE_CLASS, L"", WS_TABSTOP, px( 130 ), px( 26 ), id );
    if (hwnd) SetWindowLongPtrW( hwnd, GWLP_USERDATA, on );
    return hwnd;
}

/**********************************************************************
 *          A picture to choose
 */

#define TILE_CLASS      L"ArcticTile"

struct tile
{
    HBITMAP bitmap;
    HICON icon;
    int icon_size;
    COLORREF color;
    BOOL has_color, selected, hot;
    tile_paint_proc paint;
    UINT_PTR param;
};

static void tile_click( HWND hwnd )
{
    SendMessageW( GetParent( hwnd ), WM_COMMAND, MAKEWPARAM( GetDlgCtrlID( hwnd ), BN_CLICKED ), (LPARAM)hwnd );
}

static void tile_paint( HWND hwnd, struct tile *tile, HDC hdc )
{
    RECT client, picture, label;
    WCHAR text[128];
    int radius = px( 6 ), ring = px( 2 ), gap = px( 3 );
    COLORREF accent = view_accent();
    HRGN region;

    GetClientRect( hwnd, &client );
    FillRect( hdc, &client, cached_brush( COLOR_CARD ) );
    picture = client;
    GetWindowTextW( hwnd, text, ARRAY_SIZE(text) );
    if (text[0])
    {
        label = client;
        label.top = client.bottom - px( 22 );
        picture.bottom = label.top;
        SetBkMode( hdc, TRANSPARENT );
        SelectObject( hdc, font_card_sub ? font_card_sub : view_font( STYLE_BODY ) );
        SetTextColor( hdc, COLOR_TEXT );
        DrawTextW( hdc, text, -1, &label, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX );
    }
    /* the ring of the one chosen, or of the one under the pointer */
    if (tile->selected || tile->hot || GetFocus() == hwnd)
    {
        HPEN pen = CreatePen( PS_INSIDEFRAME, ring, tile->selected ? accent : COLOR_GRAY );
        HGDIOBJ old_pen = SelectObject( hdc, pen ), old_brush = SelectObject( hdc, GetStockObject( NULL_BRUSH ) );

        RoundRect( hdc, picture.left, picture.top, picture.right, picture.bottom, radius + gap, radius + gap );
        SelectObject( hdc, old_brush );
        SelectObject( hdc, old_pen );
        DeleteObject( pen );
    }
    InflateRect( &picture, -(ring + gap), -(ring + gap) );
    if (tile->icon)
    {
        int size = tile->icon_size ? tile->icon_size : px( 32 );

        DrawIconEx( hdc, (picture.left + picture.right - size) / 2, (picture.top + picture.bottom - size) / 2,
                    tile->icon, size, size, 0, NULL, DI_NORMAL );
        return;
    }
    /* the picture with round corners */
    region = CreateRoundRectRgn( picture.left, picture.top, picture.right + 1, picture.bottom + 1, radius, radius );
    SelectClipRgn( hdc, region );
    if (tile->bitmap)
    {
        HDC mem = CreateCompatibleDC( hdc );
        HGDIOBJ old = SelectObject( mem, tile->bitmap );
        BITMAP bm;

        GetObjectW( tile->bitmap, sizeof(bm), &bm );
        SetStretchBltMode( hdc, HALFTONE );
        SetBrushOrgEx( hdc, 0, 0, NULL );
        StretchBlt( hdc, picture.left, picture.top, picture.right - picture.left, picture.bottom - picture.top, mem,
                    0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY );
        SelectObject( mem, old );
        DeleteDC( mem );
    }
    else FillRect( hdc, &picture, cached_brush( tile->has_color ? tile->color : COLOR_CARD_BORDER ) );
    if (tile->paint) tile->paint( hdc, &picture, tile->param );
    SelectClipRgn( hdc, NULL );
    DeleteObject( region );
    /* a colour's edge, faint, so that one like the card's shows */
    if (tile->has_color && !tile->bitmap)
    {
        HPEN pen = CreatePen( PS_SOLID, 1, COLOR_CARD_BORDER );
        HGDIOBJ old_pen = SelectObject( hdc, pen ), old_brush = SelectObject( hdc, GetStockObject( NULL_BRUSH ) );

        RoundRect( hdc, picture.left, picture.top, picture.right, picture.bottom, radius, radius );
        SelectObject( hdc, old_brush );
        SelectObject( hdc, old_pen );
        DeleteObject( pen );
    }
}

static LRESULT WINAPI tile_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    struct tile *tile = (struct tile *)GetWindowLongPtrW( hwnd, GWLP_USERDATA );

    switch (msg)
    {
    case WM_NCCREATE:
        if (!(tile = calloc( 1, sizeof(*tile) ))) return FALSE;
        SetWindowLongPtrW( hwnd, GWLP_USERDATA, (LONG_PTR)tile );
        break;
    case WM_NCDESTROY:
        free( tile );
        SetWindowLongPtrW( hwnd, GWLP_USERDATA, 0 );
        break;
    case TM_SETBITMAP:
        if (!tile) return 0;
        tile->bitmap = (HBITMAP)wp;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    case WM_GETDLGCODE:
        return DLGC_BUTTON;
    case WM_MOUSEMOVE:
        if (tile && !tile->hot)
        {
            TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, hwnd };

            tile->hot = TRUE;
            TrackMouseEvent( &track );
            InvalidateRect( hwnd, NULL, FALSE );
        }
        return 0;
    case WM_MOUSELEAVE:
        if (tile)
        {
            tile->hot = FALSE;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus( hwnd );
        SetCapture( hwnd );
        return 0;
    case WM_LBUTTONUP:
    {
        POINT pt = { (short)LOWORD( lp ), (short)HIWORD( lp ) };
        RECT client;

        if (GetCapture() != hwnd) return 0;
        ReleaseCapture();
        GetClientRect( hwnd, &client );
        if (PtInRect( &client, pt ) && IsWindowEnabled( hwnd )) tile_click( hwnd );
        return 0;
    }
    case WM_KEYUP:
        if ((wp == VK_SPACE || wp == VK_RETURN) && IsWindowEnabled( hwnd )) tile_click( hwnd );
        return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_SETTEXT:
        InvalidateRect( hwnd, NULL, FALSE );
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint( hwnd, &ps ), mem = CreateCompatibleDC( hdc );
        RECT client;
        HBITMAP bitmap;

        GetClientRect( hwnd, &client );
        if (tile && (bitmap = CreateCompatibleBitmap( hdc, max( client.right, 1 ), max( client.bottom, 1 ) )))
        {
            HGDIOBJ old = SelectObject( mem, bitmap );

            tile_paint( hwnd, tile, mem );
            BitBlt( hdc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY );
            SelectObject( mem, old );
            DeleteObject( bitmap );
        }
        DeleteDC( mem );
        EndPaint( hwnd, &ps );
        return 0;
    }
    }
    return DefWindowProcW( hwnd, msg, wp, lp );
}

static void tile_register(void)
{
    static BOOL registered;
    WNDCLASSW cls = { 0 };

    card_fonts();
    if (registered) return;
    cls.lpfnWndProc = tile_proc;
    cls.hInstance = cp_instance;
    cls.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_ARROW );
    cls.lpszClassName = TILE_CLASS;
    registered = RegisterClassW( &cls ) != 0;
}

HWND view_tile( struct view *view, int width, int height, const WCHAR *label, UINT id )
{
    return view_card_control( view, TILE_CLASS, label ? label : L"", WS_TABSTOP, 0, 0, width, height, id );
}

static struct tile *tile_of( HWND hwnd )
{
    return hwnd ? (struct tile *)GetWindowLongPtrW( hwnd, GWLP_USERDATA ) : NULL;
}

void tile_set_bitmap( HWND hwnd, HBITMAP bitmap )
{
    if (hwnd) SendMessageW( hwnd, TM_SETBITMAP, (WPARAM)bitmap, 0 );
}

void tile_set_icon( HWND hwnd, HICON icon, int size )
{
    struct tile *tile = tile_of( hwnd );

    if (!tile) return;
    tile->icon = icon;
    tile->icon_size = size;
    InvalidateRect( hwnd, NULL, FALSE );
}

void tile_set_color( HWND hwnd, COLORREF color )
{
    struct tile *tile = tile_of( hwnd );

    if (!tile) return;
    tile->color = color;
    tile->has_color = TRUE;
    InvalidateRect( hwnd, NULL, FALSE );
}

void tile_set_paint( HWND hwnd, tile_paint_proc proc, UINT_PTR param )
{
    struct tile *tile = tile_of( hwnd );

    if (!tile) return;
    tile->paint = proc;
    tile->param = param;
    InvalidateRect( hwnd, NULL, FALSE );
}

void tile_select( HWND hwnd, BOOL selected )
{
    struct tile *tile = tile_of( hwnd );

    if (!tile) return;
    tile->selected = selected;
    InvalidateRect( hwnd, NULL, FALSE );
}

UINT_PTR tile_param( HWND hwnd )
{
    struct tile *tile = tile_of( hwnd );

    return tile ? tile->param : 0;
}

HWND view_embed( struct view *view, HWND hwnd, int width, int height, int indent )
{
    struct item *item;

    if (!hwnd || !(item = add_item( view, ITEM_CONTROL ))) return NULL;
    item->hwnd = hwnd;
    item->width = width;
    item->height = item->line = height;
    item->indent = indent;
    return hwnd;
}

/**********************************************************************
 *          A property page of another module
 *
 * comctl32's own property sheet, modeless and a child of the page: its
 * template loses the caption and the frame before it is made (PSCB_PRECREATE),
 * its tab and buttons are hidden, and the one page stands at its corner. So
 * a page works as in its dialog, PSM_CHANGED, PSN_APPLY and PSN_RESET
 * included, without anything of it written anew.
 */

#define ID_CP_APPLY         0xf0f0
#define IDC_SHEET_APPLY     0x3021      /* comctl32's Apply */

static int CALLBACK sheet_callback( HWND hwnd, UINT msg, LPARAM lparam )
{
    if (msg == PSCB_PRECREATE && lparam)
    {
        WORD *words = (WORD *)lparam;
        DWORD *style, *ex_style;

        /* DLGTEMPLATEEX: version, 0xffff, the help id, then the styles */
        if (words[1] == 0xffff)
        {
            ex_style = (DWORD *)(words + 4);
            style = (DWORD *)(words + 6);
        }
        else
        {
            style = (DWORD *)lparam;
            ex_style = style + 1;
        }
        *style = (*style & ~(WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_VISIBLE | DS_MODALFRAME |
                             DS_CONTEXTHELP | DS_CENTER)) | WS_CHILD | WS_CLIPCHILDREN | DS_CONTROL;
        *ex_style = (*ex_style & ~(WS_EX_DLGMODALFRAME | WS_EX_CONTEXTHELP | WS_EX_APPWINDOW | WS_EX_WINDOWEDGE)) |
                    WS_EX_CONTROLPARENT;
    }
    return 0;
}

static LRESULT CALLBACK sheet_subclass( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data )
{
    struct view *view = (struct view *)data;
    LRESULT ret = DefSubclassProc( hwnd, msg, wp, lp );

    if (view->sheet != hwnd) return ret;
    switch (msg)
    {
    case PSM_CHANGED:
        view->sheet_changed = TRUE;
        EnableWindow( view->sheet_apply, TRUE );
        break;
    case PSM_UNCHANGED:
        view->sheet_changed = FALSE;
        EnableWindow( view->sheet_apply, FALSE );
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass( hwnd, sheet_subclass, id );
        break;
    }
    return ret;
}

HWND view_sheet( struct view *view, HPROPSHEETPAGE page )
{
    static const UINT hidden[] = { IDOK, IDCANCEL, IDC_SHEET_APPLY, IDHELP };
    PROPSHEETHEADERW header = { sizeof(header) };
    HWND sheet, dialog;
    RECT rect;
    int width, height;

    if (!page) return NULL;
    header.dwFlags = PSH_MODELESS | PSH_USECALLBACK;
    header.hwndParent = view->content;
    header.hInstance = cp_instance;
    header.nPages = 1;
    header.phpage = &page;
    header.pfnCallback = sheet_callback;
    if (!(sheet = (HWND)PropertySheetW( &header ))) return NULL;

    ShowWindow( (HWND)SendMessageW( sheet, PSM_GETTABCONTROL, 0, 0 ), SW_HIDE );
    for (UINT i = 0; i < ARRAY_SIZE(hidden); i++)
    {
        HWND button = GetDlgItem( sheet, hidden[i] );

        ShowWindow( button, SW_HIDE );
        /* Enter or Esc must not end it as OK and Cancel did */
        EnableWindow( button, FALSE );
    }
    /* the page at the sheet's corner, the sheet as large as the page */
    if ((dialog = (HWND)SendMessageW( sheet, PSM_GETCURRENTPAGEHWND, 0, 0 )) && GetWindowRect( dialog, &rect ))
    {
        width = rect.right - rect.left;
        height = rect.bottom - rect.top;
        SetWindowPos( dialog, NULL, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE );
    }
    else
    {
        GetWindowRect( sheet, &rect );
        width = rect.right - rect.left;
        height = rect.bottom - rect.top;
    }
    SetWindowPos( sheet, NULL, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW );

    view->sheet = sheet;
    view->sheet_changed = FALSE;
    SetWindowSubclass( sheet, sheet_subclass, 1, (DWORD_PTR)view );
    view_embed( view, sheet, width, height, 0 );

    /* "Застосувати" under its right edge, greyed until something changes */
    view_space( view, px( 10 ) );
    view->sheet_apply = view_control( view, L"BUTTON", load_string( IDS_CP_APPLY ), BS_PUSHBUTTON | WS_TABSTOP | WS_DISABLED,
                                      px( 88 ), px( 24 ), max( width - px( 88 ), 0 ), ID_CP_APPLY );
    return sheet;
}

static BOOL sheet_apply( struct view *view )
{
    if (!view->sheet) return FALSE;
    if (SendMessageW( view->sheet, PSM_APPLY, 0, 0 ))
    {
        view->sheet_changed = FALSE;
        EnableWindow( view->sheet_apply, FALSE );
    }
    return TRUE;
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
    HDC hdc;
    int width, y = CONTENT_TOP, row_top = 0, row_height = 0, paint_top = 0, card_right = 0;
    BOOL in_row = FALSE;
    RECT client, card = { 0 };
    SCROLLINFO info = { sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS };

    /* No window yet, or no more: nothing to lay out, and nothing may be
     * asked of a window that is not there. InvalidateRect( NULL ) is every
     * window of the system: Windows erases and draws them all again. */
    if (!view->hwnd || !view->content) return;
    hdc = GetDC( view->content );
    width = view_content_width( view );

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

            /* in the card before it, at its right, in its middle */
            if (item->inside && item->extra)
            {
                int cy = card.top + (card.bottom - card.top - item->line) / 2;

                left = card_right - w;
                card_right = left - px( 8 );
                SetRect( &item->rect, left, cy, left + w, cy + item->line );
                SetWindowPos( item->hwnd, NULL, left, cy - view->scroll, w, item->height, SWP_NOZORDER | SWP_NOACTIVATE );
                continue;
            }
            /* in the card area before it, at x, y from its inner corner; as
             * wide as the card inside without a width of its own */
            if (item->inside)
            {
                if (!item->width) w = max( px( 20 ), card.right - card.left - px( 32 ) - item->x );
                left = card.left + px( 16 ) + item->x;
                SetRect( &item->rect, left, card.top + px( 16 ) + item->indent, left + w,
                         card.top + px( 16 ) + item->indent + item->line );
                SetWindowPos( item->hwnd, NULL, left, item->rect.top - view->scroll, w, item->height,
                              SWP_NOZORDER | SWP_NOACTIVATE );
                continue;
            }
            height = item->line;
            SetRect( &item->rect, left, top, left + w, top + height );
            SetWindowPos( item->hwnd, NULL, left, top - view->scroll, w, item->height, SWP_NOZORDER | SWP_NOACTIVATE );
            break;
        }
        case ITEM_HEADING:
            height = text_height( hdc, item->text, font_heading, width, FALSE );
            SetRect( &item->rect, CONTENT_PAD, top, CONTENT_PAD + width, top + height );
            height += px( 18 );
            break;
        case ITEM_SECTION:
            height = text_height( hdc, item->text, font_section, width, FALSE );
            /* room above it, after the cards of the section before */
            SetRect( &item->rect, CONTENT_PAD, top + px( 14 ), CONTENT_PAD + width, top + px( 14 ) + height );
            height += px( 22 );
            break;
        case ITEM_CARD:
        {
            int h = item->style == CARD_AREA ? item->height + px( 32 ) : item->sub ? px( 68 ) : px( 56 );

            /* its controls in rows, as many as the width needs */
            if (item->flow)
            {
                int inner = width - px( 32 ), fx = 0, fy = 0, row = 0;

                for (UINT k = i + 1; k < view->count && view->items[k].kind == ITEM_CONTROL && view->items[k].inside &&
                                     !view->items[k].extra; k++)
                {
                    struct item *c = &view->items[k];

                    if (fx && fx + c->width > inner)
                    {
                        fx = 0;
                        fy += row + item->flow;
                        row = 0;
                    }
                    c->x = fx;
                    c->indent = fy;
                    fx += c->width + item->flow;
                    row = max( row, c->height );
                }
                h = fy + row + px( 32 );
            }
            SetRect( &item->rect, CONTENT_PAD, top, CONTENT_PAD + width, top + h );
            card = item->rect;
            card_right = item->rect.right - px( 16 ) - (item->hwnd ? item->width + px( 8 ) : 0);
            if (item->hwnd)
            {
                int line = item->line ? item->line : item->height;
                int x = item->rect.right - px( 16 ) - item->width, cy = top + (h - line) / 2;

                SetWindowPos( item->hwnd, NULL, x, cy - view->scroll, item->width, item->height,
                              SWP_NOZORDER | SWP_NOACTIVATE );
            }
            /* the cards a little apart */
            height = h + px( 4 );
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

    if (!view->hwnd || !view->content) return;

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

/* another item of the Control Panel, or one of its pages by its canonical
 * name ("::{CLSID}\pageName"), in this window */
void view_browse_item( struct view *view, const WCHAR *item )
{
    WCHAR path[300];
    ITEMIDLIST *pidl;

    swprintf( path, ARRAY_SIZE(path),
              L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\::{21EC2020-3AEA-1069-A2DD-08002B30309D}\\%s", item );
    if (!view->browser || FAILED(SHParseDisplayName( path, NULL, &pidl, 0, NULL ))) return;
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

static void draw_line( HDC hdc, const WCHAR *text, HFONT font, COLORREF color, RECT *rect, UINT flags )
{
    HGDIOBJ old = SelectObject( hdc, font );

    SetTextColor( hdc, color );
    DrawTextW( hdc, text, -1, rect, DT_NOPREFIX | DT_SINGLELINE | DT_END_ELLIPSIS | flags );
    SelectObject( hdc, old );
}

static void paint_card( struct view *view, HDC hdc, const struct item *item, const RECT *where, BOOL hot )
{
    RECT rect = *where, text;
    HPEN pen = CreatePen( PS_SOLID, 1, COLOR_CARD_BORDER );
    HGDIOBJ old_brush = SelectObject( hdc, cached_brush( hot ? COLOR_CARD_HOT : COLOR_CARD ) ), old_pen = SelectObject( hdc, pen );
    int left = rect.left + px( 16 ), size = px( 32 ), right = rect.right - px( 16 );

    RoundRect( hdc, rect.left, rect.top, rect.right, rect.bottom, px( 8 ), px( 8 ) );
    SelectObject( hdc, old_pen );
    SelectObject( hdc, old_brush );
    DeleteObject( pen );
    if (item->style == CARD_AREA || item->flow)
    {
        RECT inner = rect;

        InflateRect( &inner, -px( 16 ), -px( 16 ) );
        if (item->paint) item->paint( view, hdc, &inner, item->param );
        return;
    }
    if (item->icon)
    {
        DrawIconEx( hdc, left, rect.top + (rect.bottom - rect.top - size) / 2, item->icon, size, size, 0, NULL, DI_NORMAL );
        left += size + px( 16 );
    }
    if (item->hwnd) right -= item->width + px( 16 );
    else if (item->proc) right -= px( 28 );
    /* the controls beside its own (view_card_extra) */
    if (!item->flow) right -= item->x;
    if (item->sub)
    {
        int title = text_height( hdc, item->text, font_card, 100, FALSE );
        int sub = text_height( hdc, item->sub, font_card_sub, 100, FALSE );
        int top = rect.top + (rect.bottom - rect.top - title - sub - px( 2 )) / 2;

        SetRect( &text, left, top, right, top + title );
        draw_line( hdc, item->text, font_card, COLOR_TEXT, &text, 0 );
        SetRect( &text, left, top + title + px( 2 ), right, top + title + px( 2 ) + sub );
        draw_line( hdc, item->sub, font_card_sub, COLOR_GRAY, &text, 0 );
    }
    else
    {
        SetRect( &text, left, rect.top, right, rect.bottom );
        draw_line( hdc, item->text, font_card, COLOR_TEXT, &text, DT_VCENTER );
    }
    /* a card that goes somewhere */
    if (item->proc && !item->hwnd)
    {
        RECT chevron = { rect.right - px( 40 ), rect.top, rect.right - px( 12 ), rect.bottom };

        draw_line( hdc, L"\x203a", font_chevron, COLOR_GRAY, &chevron, DT_CENTER | DT_VCENTER );
    }
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
        case ITEM_HEADING:
            draw_line( hdc, item->text, font_heading, COLOR_TEXT, &rect, 0 );
            break;
        case ITEM_SECTION:
            draw_line( hdc, item->text, font_section, COLOR_TEXT, &rect, 0 );
            break;
        case ITEM_CARD:
            paint_card( view, hdc, item, &rect, view->hot == (int)i && item->proc );
            break;
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
        int left = view->nav_x + NAV_LEFT, width = NAV_WIDTH - NAV_LEFT - px( 8 ), height;

        if (item->see_also && !see_also_shown)
        {
            int bottom = 0;
            for (UINT k = i; k < view->nav_count; k++)
                bottom += text_height( hdc, view->nav[k].text, view_font( STYLE_BODY ), width, TRUE ) + px( 8 );
            y = max( y + px( 24 ), client.bottom - bottom - px( 40 ) );
            {
                RECT rect = { left, y, left + width, y + px( 16 ) };
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
        if ((view->items[i].kind == ITEM_LINK || (view->items[i].kind == ITEM_CARD && view->items[i].proc)) &&
            PtInRect( &view->items[i].rect, pt ))
            return i;
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
        view = ((CREATESTRUCTW *)lp)->lpCreateParams;
        SetWindowLongPtrW( hwnd, GWLP_USERDATA, (LONG_PTR)view );
        if (view) view->content = hwnd;  /* known before its first WM_SIZE */
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
            FillRect( mem, &client, cached_brush( back_color( view ) ) );
            if (view) paint_content( view, mem );
            BitBlt( hdc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY );
            SelectObject( mem, old );
            DeleteObject( bitmap );
        }
        DeleteDC( mem );
        EndPaint( hwnd, &ps );
        return 0;
    }
    case WM_THEMECHANGED:
    case WM_SYSCOLORCHANGE:
        /* the programs' mode changed (desk.cpl, Personalization): the cards'
         * own controls stand on the cards' colours too */
        RedrawWindow( hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN );
        break;
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
        if (view && view->hot >= 0 && (HWND)wp == hwnd && view->items[view->hot].kind == ITEM_LINK)
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
        if (view && lp && LOWORD( wp ) == ID_CP_APPLY && HIWORD( wp ) == BN_CLICKED && sheet_apply( view )) return 0;
        if (view && lp) page_command( view, LOWORD( wp ), HIWORD( wp ), (HWND)lp );
        return 0;
    case WM_HSCROLL:
        if (view && lp) page_command( view, GetDlgCtrlID( (HWND)lp ), LOWORD( wp ), (HWND)lp );
        return 0;
    case WM_NOTIFY:
        if (view)
        {
            const NMHDR *previous = view->notify;

            view->notify = (const NMHDR *)lp;
            page_command( view, ((NMHDR *)lp)->idFrom, ((NMHDR *)lp)->code, ((NMHDR *)lp)->hwndFrom );
            view->notify = previous;
        }
        return 0;
    case WM_TIMER:
        if (view && wp == TIMER_PAGE && view->state.page < cp_page_count && cp_pages[view->state.page].timer)
            cp_pages[view->state.page].timer( view );
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    {
        /* a control in a card stands on the card's colour */
        COLORREF color = view && card_of( view, (HWND)lp ) ? COLOR_CARD : back_color( view );

        SetBkColor( (HDC)wp, color );
        SetTextColor( (HDC)wp, COLOR_TEXT );
        return (LRESULT)cached_brush( color );
    }
    }
    return DefWindowProcW( hwnd, msg, wp, lp );
}

static LRESULT WINAPI view_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    struct view *view = (struct view *)GetWindowLongPtrW( hwnd, GWLP_USERDATA );

    switch (msg)
    {
    case WM_NCCREATE:
        view = ((CREATESTRUCTW *)lp)->lpCreateParams;
        SetWindowLongPtrW( hwnd, GWLP_USERDATA, (LONG_PTR)view );
        if (view) view->hwnd = hwnd;
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
            int nav = min( NAV_WIDTH, LOWORD( lp ) / 3 ), width = max( LOWORD( lp ) - nav, 0 );

            view->nav_x = nav_on_right( view ) ? width : 0;
            MoveWindow( view->content, view->nav_x ? 0 : nav, 0, width, HIWORD( lp ), TRUE );
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
    case WM_THEMECHANGED:
    case WM_SYSCOLORCHANGE:
        /* the programs' mode changed (desk.cpl, Personalization) */
        InvalidateRect( hwnd, NULL, TRUE );
        break;
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
        /* A page of another module stays as it is: it says WM_SETTINGCHANGE
         * itself while it applies (desk.cpl's wallpaper), and must not be
         * made anew in the middle of that. The page is made anew after
         * what changed has been said to everyone, once for several: a page
         * that changes a setting hears of it while its control's handler
         * still runs. The icons of another theme: user32 takes them after
         * a moment (its folder is read again). */
        if (!view || view->sheet) return 0;
        if (lp && !IsBadStringPtrW( (const WCHAR *)lp, 64 ) && !wcsicmp( (const WCHAR *)lp, L"ArcticIcons" ))
        {
            icons_stale = TRUE;
            SetTimer( hwnd, TIMER_REBUILD, 800, NULL );
        }
        else if (!icons_stale) SetTimer( hwnd, TIMER_REBUILD, 60, NULL );
        return 0;
    case WM_TIMER:
        if (wp != TIMER_REBUILD) break;
        KillTimer( hwnd, TIMER_REBUILD );
        if (view && !view->sheet)
        {
            if (icons_stale) icons_flush();
            view_rebuild( view );
        }
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
    /* in a page of another module, all a dialog's keys: the arrows between
     * its radio buttons, Alt and a letter */
    if (view->sheet && IsChild( view->sheet, msg->hwnd ) &&
        (msg->message == WM_KEYDOWN || msg->message == WM_SYSKEYDOWN || msg->message == WM_CHAR ||
         msg->message == WM_SYSCHAR) &&
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
