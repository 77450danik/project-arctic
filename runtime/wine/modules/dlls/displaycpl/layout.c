/*
 * Display of the Control Panel: the arrangement of the monitors
 *
 * Windows 11's box at the top of Display: each monitor on the desktop as a
 * rounded tile at the scale of the whole, its number in it, the chosen one
 * in the accent's colour. A click chooses one (the page below shows its
 * settings), a drag moves it; let go, it comes to the nearest edge of
 * another monitor, so that none is left apart. The page applies the places.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <math.h>

#include "displaycpl.h"

struct layout
{
    struct displays *displays;
    int selected;              /* index in the list */
    int dragging;              /* -1 when nothing is dragged */
    POINT grab;                /* where in the tile it was taken, in pixels of the box */
    POINT start;               /* where the monitor stood when the drag began */
    BOOL moved;
    double scale;              /* box pixels a desktop pixel */
    POINT origin;              /* the desktop point at the box's corner */
};

static RECT monitor_rect( const struct monitor *m )
{
    RECT rect = { m->pos.x, m->pos.y, m->pos.x + (LONG)m->current.dmPelsWidth, m->pos.y + (LONG)m->current.dmPelsHeight };
    return rect;
}

/* the scale and the corner: the monitors fit the box with room around them */
static void layout_fit( HWND hwnd, struct layout *layout )
{
    RECT client, bounds = { 0 };
    BOOL first = TRUE;
    double sx, sy;
    int width, height;

    GetClientRect( hwnd, &client );
    for (UINT i = 0; i < layout->displays->count; i++)
    {
        const struct monitor *m = &layout->displays->list[i];
        RECT rect;

        if (!m->attached) continue;
        rect = monitor_rect( m );
        if (first) bounds = rect;
        else UnionRect( &bounds, &bounds, &rect );
        first = FALSE;
    }
    width = max( bounds.right - bounds.left, 1 );
    height = max( bounds.bottom - bounds.top, 1 );
    sx = (client.right - px( 40 )) / (double)width;
    sy = (client.bottom - px( 40 )) / (double)height;
    layout->scale = max( min( min( sx, sy ), 0.25 ), 0.01 );
    /* in the middle of the box */
    layout->origin.x = bounds.left - (LONG)((client.right / layout->scale - width) / 2);
    layout->origin.y = bounds.top - (LONG)((client.bottom / layout->scale - height) / 2);
}

static RECT tile_rect( const struct layout *layout, const struct monitor *m )
{
    RECT rect = monitor_rect( m ), tile;
    int gap = px( 2 );

    tile.left = (LONG)floor( (rect.left - layout->origin.x) * layout->scale ) + gap;
    tile.top = (LONG)floor( (rect.top - layout->origin.y) * layout->scale ) + gap;
    tile.right = (LONG)floor( (rect.right - layout->origin.x) * layout->scale ) - gap;
    tile.bottom = (LONG)floor( (rect.bottom - layout->origin.y) * layout->scale ) - gap;
    return tile;
}

static int tile_at( const struct layout *layout, POINT pt )
{
    for (int i = layout->displays->count - 1; i >= 0; i--)
    {
        const struct monitor *m = &layout->displays->list[i];
        RECT tile;

        if (!m->attached) continue;
        tile = tile_rect( layout, m );
        if (PtInRect( &tile, pt )) return i;
    }
    return -1;
}

/* next to the nearest other monitor, touching it along one edge */
static void snap( struct layout *layout, struct monitor *dragged )
{
    RECT r = monitor_rect( dragged );
    LONG w = r.right - r.left, h = r.bottom - r.top;
    LONGLONG best = -1;
    POINT best_pos = dragged->pos;

    for (UINT i = 0; i < layout->displays->count; i++)
    {
        const struct monitor *other = &layout->displays->list[i];
        RECT o;
        POINT candidates[4];

        if (other == dragged || !other->attached) continue;
        o = monitor_rect( other );
        /* beside it: left, right; above or below it: along it, overlapping */
        candidates[0].x = o.left - w;
        candidates[1].x = o.right;
        candidates[0].y = candidates[1].y = max( o.top - h + 1, min( r.top, o.bottom - 1 ) );
        candidates[2].y = o.top - h;
        candidates[3].y = o.bottom;
        candidates[2].x = candidates[3].x = max( o.left - w + 1, min( r.left, o.right - 1 ) );
        for (int k = 0; k < 4; k++)
        {
            LONGLONG dx = candidates[k].x - r.left, dy = candidates[k].y - r.top, d = dx * dx + dy * dy;

            if (best < 0 || d < best)
            {
                best = d;
                best_pos = candidates[k];
            }
        }
    }
    dragged->pos = best_pos;
}

static void notify( HWND hwnd, UINT code )
{
    SendMessageW( GetParent( hwnd ), WM_COMMAND, MAKEWPARAM( GetDlgCtrlID( hwnd ), code ), (LPARAM)hwnd );
}

static void paint( HWND hwnd, struct layout *layout, HDC hdc )
{
    RECT client;
    /* on the card it stands in */
    HBRUSH back = CreateSolidBrush( COLOR_CARD );
    NONCLIENTMETRICSW metrics = { sizeof(metrics) };
    COLORREF accent = view_accent();
    HFONT font;

    GetClientRect( hwnd, &client );
    FillRect( hdc, &client, back );
    DeleteObject( back );
    if (!layout->displays) return;
    /* the box keeps its scale while a tile is dragged: it stays under the pointer */
    if (layout->dragging < 0) layout_fit( hwnd, layout );
    SystemParametersInfoW( SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0 );
    SetBkMode( hdc, TRANSPARENT );
    for (UINT i = 0; i < layout->displays->count; i++)
    {
        const struct monitor *m = &layout->displays->list[i];
        BOOL chosen = (int)i == layout->selected;
        RECT tile;
        HBRUSH fill;
        HPEN pen;
        HGDIOBJ old_brush, old_pen, old_font;
        WCHAR text[8];

        if (!m->attached) continue;
        tile = tile_rect( layout, m );
        fill = CreateSolidBrush( chosen ? accent : (cp_dark() ? RGB( 0x45, 0x45, 0x45 ) : RGB( 0xd8, 0xd8, 0xd8 )) );
        pen = CreatePen( PS_SOLID, 1, chosen ? accent : (cp_dark() ? RGB( 0x5a, 0x5a, 0x5a ) : RGB( 0xb8, 0xb8, 0xb8 )) );
        old_brush = SelectObject( hdc, fill );
        old_pen = SelectObject( hdc, pen );
        RoundRect( hdc, tile.left, tile.top, tile.right, tile.bottom, px( 8 ), px( 8 ) );
        SelectObject( hdc, old_brush );
        SelectObject( hdc, old_pen );
        DeleteObject( fill );
        DeleteObject( pen );

        metrics.lfMessageFont.lfHeight = -max( min( (tile.bottom - tile.top) / 2, px( 40 ) ), px( 10 ) );
        metrics.lfMessageFont.lfWeight = FW_SEMIBOLD;
        metrics.lfMessageFont.lfQuality = CLEARTYPE_QUALITY;
        font = CreateFontIndirectW( &metrics.lfMessageFont );
        old_font = SelectObject( hdc, font );
        SetTextColor( hdc, chosen ? RGB( 255, 255, 255 ) : (cp_dark() ? RGB( 255, 255, 255 ) : RGB( 0, 0, 0 )) );
        swprintf( text, ARRAY_SIZE(text), L"%u", m->number );
        DrawTextW( hdc, text, -1, &tile, DT_CENTER | DT_VCENTER | DT_SINGLELINE );
        SelectObject( hdc, old_font );
        DeleteObject( font );
    }
}

static LRESULT WINAPI layout_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    struct layout *layout = (struct layout *)GetWindowLongPtrW( hwnd, GWLP_USERDATA );

    switch (msg)
    {
    case WM_NCCREATE:
        if (!(layout = calloc( 1, sizeof(*layout) ))) return FALSE;
        layout->selected = layout->dragging = -1;
        SetWindowLongPtrW( hwnd, GWLP_USERDATA, (LONG_PTR)layout );
        break;
    case WM_NCDESTROY:
        free( layout );
        SetWindowLongPtrW( hwnd, GWLP_USERDATA, 0 );
        break;
    case LM_SETDISPLAYS:
        layout->displays = (struct displays *)lp;
        layout->selected = (int)wp;
        InvalidateRect( hwnd, NULL, TRUE );
        return 0;
    case LM_GETSELECTED:
        return layout->selected;
    case WM_ERASEBKGND:
        return 1;
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
            paint( hwnd, layout, mem );
            BitBlt( hdc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY );
            SelectObject( mem, old );
            DeleteObject( bitmap );
        }
        DeleteDC( mem );
        EndPaint( hwnd, &ps );
        return 0;
    }
    case WM_LBUTTONDOWN:
    {
        POINT pt = { (short)LOWORD( lp ), (short)HIWORD( lp ) };
        int index;

        if (!layout->displays || (index = tile_at( layout, pt )) < 0) return 0;
        SetFocus( hwnd );
        /* the page hears of the choice once the button is let go: it lays
         * itself out anew then, which must not end a drag */
        layout->moved = index != layout->selected;
        layout->selected = index;
        InvalidateRect( hwnd, NULL, FALSE );
        {
            RECT tile = tile_rect( layout, &layout->displays->list[index] );

            layout->dragging = index;
            layout->grab.x = pt.x - tile.left;
            layout->grab.y = pt.y - tile.top;
            layout->start = layout->displays->list[index].pos;
            SetCapture( hwnd );
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        /* one monitor alone has nowhere to go */
        if (layout->dragging >= 0 && GetCapture() == hwnd && layout->displays &&
            displays_attached( layout->displays ) >= 2)
        {
            struct monitor *m = &layout->displays->list[layout->dragging];
            POINT pt = { (short)LOWORD( lp ), (short)HIWORD( lp ) };
            int gap = px( 2 );

            m->pos.x = layout->origin.x + (LONG)((pt.x - layout->grab.x - gap) / layout->scale);
            m->pos.y = layout->origin.y + (LONG)((pt.y - layout->grab.y - gap) / layout->scale);
            InvalidateRect( hwnd, NULL, FALSE );
        }
        return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (layout->dragging >= 0 && layout->displays)
        {
            struct monitor *m = &layout->displays->list[layout->dragging];
            BOOL chosen = layout->moved;

            layout->dragging = -1;
            layout->moved = FALSE;
            if (msg == WM_LBUTTONUP) ReleaseCapture();
            if (displays_attached( layout->displays ) >= 2) snap( layout, m );
            InvalidateRect( hwnd, NULL, FALSE );
            if (m->pos.x != layout->start.x || m->pos.y != layout->start.y) notify( hwnd, LAYOUT_MOVED );
            else if (chosen) notify( hwnd, LAYOUT_SELECTED );
        }
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_KEYDOWN:
        /* the arrows choose the monitor before or after */
        if (layout->displays && (wp == VK_LEFT || wp == VK_RIGHT || wp == VK_UP || wp == VK_DOWN))
        {
            int step = (wp == VK_LEFT || wp == VK_UP) ? -1 : 1, count = layout->displays->count, index = layout->selected;

            for (int k = 0; k < count; k++)
            {
                index = (index + step + count) % count;
                if (layout->displays->list[index].attached) break;
            }
            if (index != layout->selected)
            {
                layout->selected = index;
                InvalidateRect( hwnd, NULL, FALSE );
                notify( hwnd, LAYOUT_SELECTED );
            }
        }
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wp, lp );
}

void layout_register(void)
{
    static BOOL done;
    WNDCLASSW cls = { 0 };

    if (done) return;
    cls.lpfnWndProc = layout_proc;
    cls.hInstance = cp_instance;
    cls.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_ARROW );
    cls.lpszClassName = LAYOUT_CLASS;
    done = RegisterClassW( &cls ) != 0;
}
