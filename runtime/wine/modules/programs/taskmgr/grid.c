/*
 * Task Manager: the list of its pages, drawn as Windows 10's
 *
 * One control for the processes, the details, the services, the startup
 * programs and the users: a header whose numeric columns carry their total
 * over the title in a larger face ("12%" over "ЦП"), rows in groups
 * ("Програми (3)"), programs that open into their processes, and the
 * numbers of the busy columns on a background that warms from pale yellow to
 * orange with their value.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "taskmgr.h"

#define ROW_HEIGHT     24          /* at 96 DPI */
#define HEADER_SMALL   26
#define HEADER_BIG     48
#define PAD            6
#define INDENT         22
#define CHEVRON        16

static const COLORREF clr_select      = RGB(204, 232, 255);
static const COLORREF clr_select_edge = RGB(153, 209, 255);
static const COLORREF clr_hover       = RGB(229, 243, 255);
static const COLORREF clr_group       = RGB(30, 66, 150);
static const COLORREF clr_header_line = RGB(229, 229, 229);
static const COLORREF clr_header_text = RGB(76, 96, 122);

/* the heat of a cell: pale yellow at nothing, orange at full */
static COLORREF heat_color( double t )
{
    static const struct { double at; BYTE r, g, b; } stops[] =
    {
        { 0.00, 255, 244, 196 },
        { 0.25, 255, 228, 160 },
        { 0.50, 255, 205, 115 },
        { 0.75, 255, 170,  75 },
        { 1.00, 249, 134,  40 },
    };
    UINT i = 1;
    double f;

    if (t <= 0) return RGB(stops[0].r, stops[0].g, stops[0].b);
    if (t >= 1) return RGB(stops[4].r, stops[4].g, stops[4].b);
    while (i < ARRAY_SIZE(stops) - 1 && t > stops[i].at) i++;
    f = (t - stops[i - 1].at) / (stops[i].at - stops[i - 1].at);
    return RGB( stops[i - 1].r + (stops[i].r - stops[i - 1].r) * f,
                stops[i - 1].g + (stops[i].g - stops[i - 1].g) * f,
                stops[i - 1].b + (stops[i].b - stops[i - 1].b) * f );
}

static int row_height(void) { return scale( ROW_HEIGHT ); }

static int header_height( struct grid *g )
{
    if (g->no_header) return 0;
    return scale( g->big_header ? HEADER_BIG : HEADER_SMALL );
}

static int visible_rows( struct grid *g )
{
    RECT rc;
    GetClientRect( g->hwnd, &rc );
    return max( 1, (rc.bottom - header_height( g )) / row_height() );
}

static int col_width( struct grid *g, UINT col )
{
    HDITEMW item = { HDI_WIDTH };
    SendMessageW( g->header, HDM_GETITEMW, col, (LPARAM)&item );
    return item.cxy;
}

static void update_scrollbar( struct grid *g )
{
    SCROLLINFO si = { sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL };
    int page = visible_rows( g );

    if (g->scroll > (int)g->row_count - page) g->scroll = max( 0, (int)g->row_count - page );
    si.nMin = 0;
    si.nMax = max( 0, (int)g->row_count - 1 );
    si.nPage = page;
    si.nPos = g->scroll;
    SetScrollInfo( g->hwnd, SB_VERT, &si, TRUE );
}

static void layout( struct grid *g )
{
    RECT rc;
    GetClientRect( g->hwnd, &rc );
    ShowWindow( g->header, g->no_header ? SW_HIDE : SW_SHOWNA );
    SetWindowPos( g->header, NULL, 0, 0, rc.right, header_height( g ), SWP_NOZORDER | SWP_NOACTIVATE );
    /* a plain list has one column, as wide as the window */
    if (g->no_header && g->col_count)
    {
        HDITEMW item = { HDI_WIDTH };
        item.cxy = rc.right;
        SendMessageW( g->header, HDM_SETITEMW, 0, (LPARAM)&item );
    }
    update_scrollbar( g );
}

/* ------------------------------------------------------------------ rows */

void grid_begin( struct grid *g )
{
    struct grid_row *sel = grid_selected( g );

    if (sel) g->selected_param = sel->param;
    for (UINT i = 0; i < g->row_count; i++)
        for (UINT c = 0; c < GRID_MAX_COLS; c++) free( g->rows[i].cells[c] );
    g->row_count = 0;
    g->selected = -1;
}

struct grid_row *grid_add_row( struct grid *g )
{
    struct grid_row *row;

    if (g->row_count == g->row_alloc)
    {
        UINT n = g->row_alloc ? g->row_alloc * 2 : 64;
        struct grid_row *p = realloc( g->rows, n * sizeof(*p) );
        if (!p) return NULL;
        g->rows = p;
        g->row_alloc = n;
    }
    row = &g->rows[g->row_count++];
    memset( row, 0, sizeof(*row) );
    row->icon = -1;
    return row;
}

void grid_set_cell( struct grid_row *row, UINT col, const WCHAR *text )
{
    if (col >= GRID_MAX_COLS) return;
    free( row->cells[col] );
    row->cells[col] = text ? wcsdup( text ) : NULL;
}

/* blocks of a parent row and its children sort together */
struct block { UINT first, count; };
static struct grid *sorting;

static int compare_blocks( const void *a, const void *b )
{
    const struct grid_row *ra = &sorting->rows[((const struct block *)a)->first];
    const struct grid_row *rb = &sorting->rows[((const struct block *)b)->first];
    int col = sorting->sort_col, ret;

    if (ra->group_id != rb->group_id) return ra->group_id - rb->group_id;
    if (ra->group != rb->group) return ra->group ? -1 : 1;
    if (col < 0) return 0;
    if (sorting->cols[col].right)
        ret = ra->sort[col] < rb->sort[col] ? -1 : ra->sort[col] > rb->sort[col] ? 1 : 0;
    else
        ret = lstrcmpiW( ra->cells[col] ? ra->cells[col] : L"", rb->cells[col] ? rb->cells[col] : L"" );
    return sorting->sort_down ? -ret : ret;
}

static void sort_rows( struct grid *g )
{
    struct block *blocks;
    struct grid_row *sorted;
    UINT count = 0, pos = 0;

    if (!g->row_count) return;
    if (!(blocks = malloc( g->row_count * sizeof(*blocks) ))) return;
    for (UINT i = 0; i < g->row_count; i++)
    {
        if (g->rows[i].indent && count) blocks[count - 1].count++;
        else { blocks[count].first = i; blocks[count].count = 1; count++; }
    }
    sorting = g;
    qsort( blocks, count, sizeof(*blocks), compare_blocks );
    if ((sorted = malloc( g->row_alloc * sizeof(*sorted) )))
    {
        for (UINT b = 0; b < count; b++)
            for (UINT i = 0; i < blocks[b].count; i++) sorted[pos++] = g->rows[blocks[b].first + i];
        free( g->rows );
        g->rows = sorted;
    }
    free( blocks );
}

void grid_end( struct grid *g )
{
    sort_rows( g );
    g->selected = -1;
    if (g->selected_param)
        for (UINT i = 0; i < g->row_count; i++)
            if (!g->rows[i].group && g->rows[i].param == g->selected_param) { g->selected = i; break; }
    update_scrollbar( g );
    grid_update_header( g );
    InvalidateRect( g->hwnd, NULL, FALSE );
}

struct grid_row *grid_selected( struct grid *g )
{
    if (g->selected < 0 || g->selected >= (int)g->row_count) return NULL;
    return &g->rows[g->selected];
}

void grid_update_header( struct grid *g )
{
    InvalidateRect( g->header, NULL, FALSE );
}

/* ------------------------------------------------------------------ painting */

static void draw_chevron( HDC hdc, int x, int y, int size, BOOL open, COLORREF color )
{
    HPEN pen = CreatePen( PS_SOLID, max( 1, size / 8 ), color ), old = SelectObject( hdc, pen );
    int h = size / 4;
    POINT pts[3];

    if (open)
    {
        pts[0].x = x - h; pts[0].y = y - h / 2;
        pts[1].x = x;     pts[1].y = y + h / 2;
        pts[2].x = x + h; pts[2].y = y - h / 2;
    }
    else
    {
        pts[0].x = x - h / 2; pts[0].y = y - h;
        pts[1].x = x + h / 2; pts[1].y = y;
        pts[2].x = x - h / 2; pts[2].y = y + h;
    }
    Polyline( hdc, pts, 3 );
    SelectObject( hdc, old );
    DeleteObject( pen );
}

static void paint_rows( struct grid *g, HDC hdc, const RECT *client, int hover )
{
    int top = header_height( g ), h = row_height(), icon = GetSystemMetrics( SM_CXSMICON );
    HFONT old = SelectObject( hdc, ui_font );

    SetBkMode( hdc, TRANSPARENT );
    for (int r = g->scroll; r < (int)g->row_count; r++)
    {
        struct grid_row *row = &g->rows[r];
        RECT rr = { 0, top + (r - g->scroll) * h, client->right, top + (r - g->scroll + 1) * h };
        int x = -g->hscroll;

        if (rr.top >= client->bottom) break;
        if (row->group)
        {
            /* "Програми (3)": the group's title in its blue, across the row */
            RECT tr = rr;
            SelectObject( hdc, ui_bold );
            SetTextColor( hdc, clr_group );
            tr.left = scale( PAD );
            tr.top += scale( 4 );
            DrawTextW( hdc, row->cells[0] ? row->cells[0] : L"", -1, &tr, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX );
            SelectObject( hdc, ui_font );
            continue;
        }
        if (r == g->selected)
        {
            HBRUSH b = CreateSolidBrush( clr_select ), e = CreateSolidBrush( clr_select_edge );
            FillRect( hdc, &rr, b );
            FrameRect( hdc, &rr, e );
            DeleteObject( b );
            DeleteObject( e );
        }
        else if (r == hover)
        {
            HBRUSH b = CreateSolidBrush( clr_hover );
            FillRect( hdc, &rr, b );
            DeleteObject( b );
        }

        for (UINT c = 0; c < g->col_count; c++)
        {
            int w = col_width( g, c );
            RECT cell = { x, rr.top, x + w, rr.bottom };
            const WCHAR *text = row->cells[c] ? row->cells[c] : L"";

            x += w;
            if (cell.right <= 0) continue;
            if (g->cols[c].heat && r != g->selected)
            {
                RECT hc = cell;
                HBRUSH b = CreateSolidBrush( heat_color( row->heat[c] ) );
                hc.left += 1;
                FillRect( hdc, &hc, b );
                DeleteObject( b );
            }
            SetTextColor( hdc, RGB(0, 0, 0) );
            if (c == 0)
            {
                int ix = cell.left + scale( PAD ) + row->indent * scale( INDENT );
                if (row->expandable)
                    draw_chevron( hdc, ix + scale( CHEVRON ) / 2, (rr.top + rr.bottom) / 2, scale( CHEVRON ),
                                  row->expanded, RGB(80, 80, 80) );
                if (g->tree) ix += scale( CHEVRON ) + scale( 2 );
                if (row->icon >= 0)
                    ImageList_Draw( small_icons, row->icon, hdc, ix, (rr.top + rr.bottom - icon) / 2, ILD_TRANSPARENT );
                if (g->icons) ix += icon + scale( PAD );
                cell.left = ix;
            }
            else cell.left += scale( PAD );
            cell.right -= scale( PAD );
            DrawTextW( hdc, text, -1, &cell, (g->cols[c].right ? DT_RIGHT : DT_LEFT) |
                       DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS );
        }
    }
    SelectObject( hdc, old );
}

static int hover_row = -1;

static void paint( struct grid *g )
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint( g->hwnd, &ps ), mem;
    RECT rc;
    HBITMAP bmp, oldbmp;

    GetClientRect( g->hwnd, &rc );
    mem = CreateCompatibleDC( hdc );
    bmp = CreateCompatibleBitmap( hdc, max( 1, rc.right ), max( 1, rc.bottom ) );
    oldbmp = SelectObject( mem, bmp );
    FillRect( mem, &rc, GetStockObject( WHITE_BRUSH ) );
    paint_rows( g, mem, &rc, hover_row );
    BitBlt( hdc, 0, header_height( g ), rc.right, rc.bottom - header_height( g ), mem, 0, header_height( g ), SRCCOPY );
    SelectObject( mem, oldbmp );
    DeleteObject( bmp );
    DeleteDC( mem );
    EndPaint( g->hwnd, &ps );
}

/* a header item: the total in the larger face over the title, the sort
 * chevron at the top, as Windows 10 draws the Processes columns */
static void draw_header_item( struct grid *g, DRAWITEMSTRUCT *di )
{
    UINT c = di->itemID;
    RECT rc = di->rcItem, tr;
    HDC hdc = di->hDC;
    HPEN pen = CreatePen( PS_SOLID, 1, clr_header_line ), old = SelectObject( hdc, pen );
    UINT align = g->cols[c].right ? DT_RIGHT : DT_LEFT;

    FillRect( hdc, &rc, GetStockObject( WHITE_BRUSH ) );
    MoveToEx( hdc, rc.right - 1, rc.top, NULL );
    LineTo( hdc, rc.right - 1, rc.bottom );
    MoveToEx( hdc, rc.left, rc.bottom - 1, NULL );
    LineTo( hdc, rc.right, rc.bottom - 1 );
    SelectObject( hdc, old );
    DeleteObject( pen );
    SetBkMode( hdc, TRANSPARENT );

    if ((int)c == g->sort_col)
    {
        /* Windows 10's sort mark: a small chevron at the top, up or down */
        HPEN sp = CreatePen( PS_SOLID, 1, RGB(130, 130, 130) ), oldp = SelectObject( hdc, sp );
        int cx = (rc.left + rc.right) / 2, cy = rc.top + scale( 4 ), w = scale( 4 );
        POINT pts[3] = { { cx - w, cy + (g->sort_down ? 0 : w / 2 + 1) }, { cx, cy + (g->sort_down ? w / 2 + 1 : 0) },
                         { cx + w, cy + (g->sort_down ? 0 : w / 2 + 1) } };
        Polyline( hdc, pts, 3 );
        SelectObject( hdc, oldp );
        DeleteObject( sp );
    }

    tr = rc;
    tr.left += scale( PAD );
    tr.right -= scale( PAD );
    if (g->big_header)
    {
        RECT top = tr;
        top.top += scale( 8 );
        top.bottom = rc.top + scale( 30 );
        if (g->cols[c].total[0])
        {
            SelectObject( hdc, title_font );
            SetTextColor( hdc, RGB(0, 0, 0) );
            DrawTextW( hdc, g->cols[c].total, -1, &top, align | DT_SINGLELINE | DT_BOTTOM | DT_NOPREFIX );
        }
        tr.top = rc.top + scale( 28 );
    }
    SelectObject( hdc, ui_font );
    SetTextColor( hdc, clr_header_text );
    DrawTextW( hdc, load_string( g->cols[c].title ), -1, &tr, align | DT_SINGLELINE |
               (g->big_header ? DT_TOP : DT_VCENTER) | DT_NOPREFIX | DT_END_ELLIPSIS );
}

/* ------------------------------------------------------------------ input */

static int row_at( struct grid *g, int y )
{
    int r;
    if (y < header_height( g )) return -1;
    r = g->scroll + (y - header_height( g )) / row_height();
    return r < (int)g->row_count ? r : -1;
}

static void notify( struct grid *g, UINT code )
{
    SendMessageW( GetParent( g->hwnd ), WM_COMMAND, MAKEWPARAM( g->notify_id, code ), (LPARAM)g->hwnd );
}

static void select_row( struct grid *g, int r )
{
    if (r < 0 || r >= (int)g->row_count) return;
    /* a group's title is not a row to select: the next one is */
    while (r < (int)g->row_count && g->rows[r].group) r++;
    if (r >= (int)g->row_count) return;
    g->selected = r;
    g->selected_param = g->rows[r].param;
    if (r < g->scroll) g->scroll = r;
    if (r >= g->scroll + visible_rows( g )) g->scroll = r - visible_rows( g ) + 1;
    update_scrollbar( g );
    InvalidateRect( g->hwnd, NULL, FALSE );
    notify( g, GN_SELCHANGE );
}

static void scroll_to( struct grid *g, int pos )
{
    int max_pos = max( 0, (int)g->row_count - visible_rows( g ) );
    g->scroll = max( 0, min( pos, max_pos ) );
    update_scrollbar( g );
    InvalidateRect( g->hwnd, NULL, FALSE );
}

static LRESULT CALLBACK grid_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct grid *g = (struct grid *)GetWindowLongPtrW( hwnd, GWLP_USERDATA );

    switch (msg)
    {
    case WM_NCCREATE:
        g = ((CREATESTRUCTW *)lparam)->lpCreateParams;
        g->hwnd = hwnd;
        SetWindowLongPtrW( hwnd, GWLP_USERDATA, (LONG_PTR)g );
        break;
    case WM_SIZE:
        if (g) layout( g );
        return 0;
    case WM_PAINT:
        paint( g );
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_DRAWITEM:
        draw_header_item( g, (DRAWITEMSTRUCT *)lparam );
        return TRUE;
    case WM_NOTIFY:
    {
        NMHDR *nm = (NMHDR *)lparam;
        if (nm->hwndFrom != g->header) break;
        if (nm->code == HDN_ITEMCLICKW || nm->code == HDN_ITEMCLICKA)
        {
            int c = ((NMHEADERW *)lparam)->iItem;
            if (g->sort_col == c) g->sort_down = !g->sort_down;
            else { g->sort_col = c; g->sort_down = g->cols[c].right; }
            sort_rows( g );
            grid_end( g );
        }
        else if (nm->code == HDN_ITEMCHANGEDW || nm->code == HDN_ITEMCHANGEDA || nm->code == HDN_TRACKW)
            InvalidateRect( hwnd, NULL, FALSE );
        break;
    }
    case WM_VSCROLL:
    {
        SCROLLINFO si = { sizeof(si), SIF_TRACKPOS };
        int page = visible_rows( g );
        GetScrollInfo( hwnd, SB_VERT, &si );
        switch (LOWORD(wparam))
        {
        case SB_LINEUP: scroll_to( g, g->scroll - 1 ); break;
        case SB_LINEDOWN: scroll_to( g, g->scroll + 1 ); break;
        case SB_PAGEUP: scroll_to( g, g->scroll - page ); break;
        case SB_PAGEDOWN: scroll_to( g, g->scroll + page ); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: scroll_to( g, si.nTrackPos ); break;
        case SB_TOP: scroll_to( g, 0 ); break;
        case SB_BOTTOM: scroll_to( g, g->row_count ); break;
        }
        return 0;
    }
    case WM_MOUSEWHEEL:
        scroll_to( g, g->scroll - GET_WHEEL_DELTA_WPARAM( wparam ) / WHEEL_DELTA * 3 );
        return 0;
    case WM_MOUSEMOVE:
    {
        int r = row_at( g, GET_Y_LPARAM( lparam ) );
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd };
        if (r != hover_row) { hover_row = r; InvalidateRect( hwnd, NULL, FALSE ); }
        TrackMouseEvent( &tme );
        return 0;
    }
    case WM_MOUSELEAVE:
        hover_row = -1;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    {
        int r = row_at( g, GET_Y_LPARAM( lparam ) );
        SetFocus( hwnd );
        if (r < 0 || g->rows[r].group) return 0;
        /* the chevron opens and closes a program's processes */
        if (msg == WM_LBUTTONDOWN && g->tree && g->rows[r].expandable &&
            GET_X_LPARAM( lparam ) < scale( PAD ) + g->rows[r].indent * scale( INDENT ) + scale( CHEVRON ) + scale( 4 ))
        {
            g->selected = r;
            g->selected_param = g->rows[r].param;
            notify( g, GN_EXPAND );
            return 0;
        }
        select_row( g, r );
        return 0;
    }
    case WM_LBUTTONDBLCLK:
    {
        int r = row_at( g, GET_Y_LPARAM( lparam ) );
        if (r < 0 || g->rows[r].group) return 0;
        select_row( g, r );
        notify( g, g->rows[r].expandable ? GN_EXPAND : GN_DBLCLK );
        return 0;
    }
    case WM_CONTEXTMENU:
        if (grid_selected( g )) notify( g, GN_CONTEXT );
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_KEYDOWN:
        switch (wparam)
        {
        case VK_UP:
        {
            int r = g->selected - 1;
            while (r >= 0 && g->rows[r].group) r--;
            if (r >= 0) select_row( g, r );
            break;
        }
        case VK_DOWN: select_row( g, g->selected + 1 ); break;
        case VK_HOME: select_row( g, 0 ); break;
        case VK_END: select_row( g, g->row_count - 1 ); break;
        case VK_PRIOR: select_row( g, max( 0, g->selected - visible_rows( g ) ) ); break;
        case VK_NEXT: select_row( g, min( (int)g->row_count - 1, g->selected + visible_rows( g ) ) ); break;
        case VK_DELETE: notify( g, GN_KEYDELETE ); break;
        case VK_RETURN: if (grid_selected( g )) notify( g, GN_DBLCLK ); break;
        case VK_RIGHT:
        case VK_LEFT:
            if (grid_selected( g ) && grid_selected( g )->expandable &&
                grid_selected( g )->expanded == (wparam == VK_LEFT)) notify( g, GN_EXPAND );
            break;
        }
        return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

void grid_register( HINSTANCE inst )
{
    WNDCLASSW wc = { CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW, grid_proc, 0, 0, inst };
    wc.hCursor = LoadCursorW( NULL, (LPCWSTR)IDC_ARROW );
    wc.lpszClassName = GRID_CLASS;
    RegisterClassW( &wc );
}

struct grid *grid_create( HWND parent, UINT id, BOOL big_header )
{
    struct grid *g = calloc( 1, sizeof(*g) );

    if (!g) return NULL;
    g->big_header = big_header;
    g->selected = -1;
    g->sort_col = -1;
    g->notify_id = id;
    CreateWindowExW( 0, GRID_CLASS, NULL, WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | WS_CLIPCHILDREN,
                     0, 0, 0, 0, parent, (HMENU)(UINT_PTR)id, instance, g );
    g->header = CreateWindowExW( 0, WC_HEADERW, NULL, WS_CHILD | WS_VISIBLE | HDS_BUTTONS | HDS_FULLDRAG,
                                 0, 0, 0, 0, g->hwnd, NULL, instance, NULL );
    SendMessageW( g->header, WM_SETFONT, (WPARAM)ui_font, FALSE );
    return g;
}

void grid_add_col( struct grid *g, UINT title, int width, BOOL right, BOOL heat )
{
    HDITEMW item = { HDI_WIDTH | HDI_FORMAT | HDI_TEXT };
    struct grid_col *c;

    if (g->col_count >= GRID_MAX_COLS) return;
    c = &g->cols[g->col_count];
    c->title = title;
    c->width = width;
    c->right = right;
    c->heat = heat;
    item.cxy = scale( width );
    item.fmt = HDF_OWNERDRAW;
    item.pszText = (WCHAR *)load_string( title );
    SendMessageW( g->header, HDM_INSERTITEMW, g->col_count, (LPARAM)&item );
    g->col_count++;
}

void grid_destroy( struct grid *g )
{
    if (!g) return;
    grid_begin( g );
    free( g->rows );
    free( g );
}
