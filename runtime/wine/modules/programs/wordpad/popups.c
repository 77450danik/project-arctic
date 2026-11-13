/*
 * Arctic WordPad: what opens under the ribbon's buttons
 *
 * Lists of commands are menus of the system, with the pictures of the
 * ribbon in them. The colours, the size of a new table, the symbols and the
 * fonts are popups drawn here, as Word has them.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdio.h>
#include "wordpad.h"

/**********************************************************************
 *          Menus
 */

static HBITMAP menu_bitmap( const char *icon )
{
    struct surface s = { 0 };
    BITMAPINFO info = { { sizeof(info.bmiHeader) } };
    HBITMAP bitmap;
    UINT32 *bits;
    int size = px( 16 );
    COLORREF back = th.dark ? RGB( 0x2c, 0x2c, 0x2c ) : RGB( 0xf9, 0xf9, 0xf9 );

    if (!icon || !icon_exists( icon )) return NULL;
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    if (!(bitmap = CreateDIBSection( NULL, &info, DIB_RGB_COLORS, (void **)&bits, NULL, 0 ))) return NULL;
    /* drawn over black and over the menu's colour: the difference is the alpha */
    if (surface_size( &s, size, size ))
    {
        RECT r = { 0, 0, size, size };
        UINT32 *over = malloc( size * size * sizeof(UINT32) );

        fill( &s, &r, RGB( 0, 0, 0 ) );
        draw_icon( &s, icon, 0, 0, size, IS_NORMAL );
        GdiFlush();
        for (int y = 0; y < size && over; y++) memcpy( over + y * size, s.bits + y * s.width, size * 4 );
        fill( &s, &r, RGB( 255, 255, 255 ) );
        draw_icon( &s, icon, 0, 0, size, IS_NORMAL );
        GdiFlush();
        for (int y = 0; y < size && over; y++)
            for (int x = 0; x < size; x++)
            {
                UINT32 b = over[y * size + x], w = s.bits[y * s.width + x];
                int a = 255 - (((w >> 8) & 0xff) - ((b >> 8) & 0xff));
                a = max( 0, min( 255, a ) );
                bits[y * size + x] = (a << 24) | (b & 0xffffff);
            }
        free( over );
    }
    surface_free( &s );
    (void)back;
    return bitmap;
}

/* a menu of commands under a button; the one chosen, or 0. A 0 in ids is a
 * line between items. checked: the command marked as chosen */
int menu_popup( HWND owner, const RECT *anchor, const int *ids, const char *const *icons, int count, int checked )
{
    HMENU menu = CreatePopupMenu();
    HBITMAP bitmaps[64] = { 0 };
    TPMPARAMS params = { sizeof(params) };
    int id;

    for (int i = 0; i < count && i < ARRAY_SIZE(bitmaps); i++)
    {
        MENUITEMINFOW item = { sizeof(item) };

        if (!ids[i])
        {
            AppendMenuW( menu, MF_SEPARATOR, 0, NULL );
            continue;
        }
        item.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE;
        item.wID = ids[i];
        if (ids[i] >= IDM_STYLE_FIRST && ids[i] < IDM_STYLE_FIRST + STYLE_COUNT)
            item.dwTypeData = (WCHAR *)string( styles[ids[i] - IDM_STYLE_FIRST].name );
        else item.dwTypeData = (WCHAR *)string( IDS_CMD( ids[i] ) );
        item.fState = (ids[i] == checked || command_checked( ids[i] ) ? MFS_CHECKED : 0) |
                      (command_enabled( ids[i] ) ? 0 : MFS_DISABLED);
        if (icons && icons[i] && (bitmaps[i] = menu_bitmap( icons[i] )))
        {
            item.fMask |= MIIM_BITMAP;
            item.hbmpItem = bitmaps[i];
        }
        InsertMenuItemW( menu, i, TRUE, &item );
    }
    params.rcExclude = *anchor;
    id = TrackPopupMenuEx( menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_VERTICAL, anchor->left,
                           anchor->bottom, owner, &params );
    DestroyMenu( menu );
    for (int i = 0; i < ARRAY_SIZE(bitmaps); i++) if (bitmaps[i]) DeleteObject( bitmaps[i] );
    return id;
}

/**********************************************************************
 *          Popups of our own
 */

struct popup
{
    HWND hwnd;
    struct surface surf;
    int width, height;
    int hot, result;
    BOOL done;
    void (*paint)( struct popup *p );
    int (*hit)( struct popup *p, POINT pt );
    void *data;
};

static LRESULT CALLBACK popup_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct popup *p = (struct popup *)GetWindowLongPtrW( hwnd, GWLP_USERDATA );
    POINT pt = { GET_X( lparam ), GET_Y( lparam ) };

    switch (msg)
    {
    case WM_NCCREATE:
        SetWindowLongPtrW( hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lparam)->lpCreateParams );
        break;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint( hwnd, &ps );
        RECT r = { 0, 0, p->width, p->height };

        if (surface_size( &p->surf, p->width, p->height ))
        {
            fill( &p->surf, &r, th.popup );
            p->paint( p );
            GdiFlush();
            BitBlt( hdc, 0, 0, p->width, p->height, p->surf.dc, 0, 0, SRCCOPY );
        }
        EndPaint( hwnd, &ps );
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE:
    {
        int hot = p->hit( p, pt );
        if (hot != p->hot)
        {
            p->hot = hot;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        return 0;
    }
    case WM_LBUTTONUP:
    {
        RECT r;
        /* the popup opens on the press of its button: that release, over
         * the button, closes nothing. A click outside closes it on its press. */
        GetClientRect( hwnd, &r );
        if (!PtInRect( &r, pt )) return 0;
        if ((p->result = p->hit( p, pt )) >= 0) p->done = TRUE;
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    {
        RECT r;
        GetClientRect( hwnd, &r );
        if (!PtInRect( &r, pt ))
        {
            p->result = -1;
            p->done = TRUE;
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE)
        {
            p->result = -1;
            p->done = TRUE;
        }
        return 0;
    case WM_CAPTURECHANGED:
        if ((HWND)lparam != hwnd) p->done = TRUE;
        return 0;
    case WM_ACTIVATE:
        if (LOWORD( wparam ) == WA_INACTIVE) p->done = TRUE;
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

static int run_popup( HWND owner, const RECT *anchor, struct popup *p )
{
    static BOOL registered;
    MONITORINFO mon = { sizeof(mon) };
    int x = anchor->left, y = anchor->bottom + px( 2 );
    MSG msg;

    if (!registered)
    {
        WNDCLASSW cls = { CS_DROPSHADOW, popup_proc, 0, 0, instance, NULL, NULL, NULL, NULL, L"ArcticWordPadPopup" };
        cls.hCursor = LoadCursorW( NULL, (LPCWSTR)IDC_ARROW );
        RegisterClassW( &cls );
        registered = TRUE;
    }
    GetMonitorInfoW( MonitorFromRect( anchor, MONITOR_DEFAULTTONEAREST ), &mon );
    if (x + p->width > mon.rcWork.right) x = mon.rcWork.right - p->width;
    if (y + p->height > mon.rcWork.bottom) y = anchor->top - p->height - px( 2 );
    p->hot = p->result = -1;
    p->hwnd = CreateWindowExW( WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"ArcticWordPadPopup", NULL, WS_POPUP | WS_BORDER,
                               x, y, p->width + 2, p->height + 2, owner, NULL, instance, p );
    if (!p->hwnd) return -1;
    ShowWindow( p->hwnd, SW_SHOWNOACTIVATE );
    SetCapture( p->hwnd );
    while (!p->done && GetMessageW( &msg, 0, 0, 0 ) > 0)
    {
        if (msg.message == WM_KEYDOWN || msg.message == WM_CHAR) msg.hwnd = p->hwnd;
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
        if (GetCapture() != p->hwnd && !p->done) p->done = TRUE;
    }
    if (GetCapture() == p->hwnd) ReleaseCapture();
    DestroyWindow( p->hwnd );
    surface_free( &p->surf );
    return p->result;
}

/**********************************************************************
 *          Colours
 */

static const COLORREF theme_base[10] =
{
    RGB( 0xff, 0xff, 0xff ), RGB( 0x00, 0x00, 0x00 ), RGB( 0xe7, 0xe6, 0xe6 ), RGB( 0x44, 0x54, 0x6a ),
    RGB( 0x44, 0x72, 0xc4 ), RGB( 0xed, 0x7d, 0x31 ), RGB( 0xa5, 0xa5, 0xa5 ), RGB( 0xff, 0xc0, 0x00 ),
    RGB( 0x5b, 0x9b, 0xd5 ), RGB( 0x70, 0xad, 0x47 ),
};
static const COLORREF standard[10] =
{
    RGB( 0xc0, 0x00, 0x00 ), RGB( 0xff, 0x00, 0x00 ), RGB( 0xff, 0xc0, 0x00 ), RGB( 0xff, 0xff, 0x00 ),
    RGB( 0x92, 0xd0, 0x50 ), RGB( 0x00, 0xb0, 0x50 ), RGB( 0x00, 0xb0, 0xf0 ), RGB( 0x00, 0x70, 0xc0 ),
    RGB( 0x00, 0x20, 0x60 ), RGB( 0x70, 0x30, 0xa0 ),
};
/* the colours a highlight can have in Word (and in DOCX) */
static const COLORREF highlights[15] =
{
    RGB( 0xff, 0xff, 0x00 ), RGB( 0x00, 0xff, 0x00 ), RGB( 0x00, 0xff, 0xff ), RGB( 0xff, 0x00, 0xff ),
    RGB( 0x00, 0x00, 0xff ), RGB( 0xff, 0x00, 0x00 ), RGB( 0x00, 0x00, 0x80 ), RGB( 0x00, 0x80, 0x80 ),
    RGB( 0x00, 0x80, 0x00 ), RGB( 0x80, 0x00, 0x80 ), RGB( 0x80, 0x00, 0x00 ), RGB( 0x80, 0x80, 0x00 ),
    RGB( 0x80, 0x80, 0x80 ), RGB( 0xc0, 0xc0, 0xc0 ), RGB( 0x00, 0x00, 0x00 ),
};

static COLORREF tint( COLORREF c, int step )
{
    /* the five rows under a theme colour: 80, 60, 40 % lighter, 25, 50 % darker
     * (black and white: lighter or darker greys) */
    static const int light[5] = { 80, 60, 40, -25, -50 };
    int t = light[step];
    BOOL black = c == RGB( 0, 0, 0 ), white = c == RGB( 255, 255, 255 );

    if (white) return mix( c, RGB( 0, 0, 0 ), (5 + step * 10) * 255 / 100 + (step >= 3 ? 40 : 0) );
    if (black) return mix( c, RGB( 255, 255, 255 ), (50 - step * 8) * 255 / 100 );
    if (t > 0) return mix( c, RGB( 255, 255, 255 ), t * 255 / 100 );
    return mix( c, RGB( 0, 0, 0 ), -t * 255 / 100 );
}

struct color_data
{
    BOOL highlight;
    COLORREF current;
    COLORREF cells[80];
    RECT rects[80];
    int count;
    RECT automatic, more;
};

#define CELL px(18)
#define CELL_GAP px(4)

static void color_layout( struct popup *p )
{
    struct color_data *d = p->data;
    int x0 = px( 10 ), y = px( 8 ), n = 0;

    SetRect( &d->automatic, px( 4 ), y, p->width - px( 4 ), y + px( 28 ) );
    y += px( 34 );
    if (d->highlight)
    {
        for (int i = 0; i < 15; i++, n++)
        {
            int col = i % 5, row = i / 5;
            d->cells[n] = highlights[i];
            SetRect( &d->rects[n], x0 + col * (CELL + CELL_GAP) * 2, y + row * (CELL + CELL_GAP) * 1,
                     x0 + col * (CELL + CELL_GAP) * 2 + CELL * 2, y + row * (CELL + CELL_GAP) + CELL );
        }
        y += 3 * (CELL + CELL_GAP) + px( 6 );
    }
    else
    {
        y += px( 20 );   /* "Theme colours" */
        for (int col = 0; col < 10; col++)
        {
            d->cells[n] = theme_base[col];
            SetRect( &d->rects[n], x0 + col * (CELL + CELL_GAP), y, x0 + col * (CELL + CELL_GAP) + CELL, y + CELL );
            n++;
            for (int row = 0; row < 5; row++, n++)
            {
                int top = y + CELL + px( 6 ) + row * CELL;
                d->cells[n] = tint( theme_base[col], row );
                SetRect( &d->rects[n], x0 + col * (CELL + CELL_GAP), top, x0 + col * (CELL + CELL_GAP) + CELL, top + CELL );
            }
        }
        y += CELL + px( 6 ) + 5 * CELL + px( 8 );
        y += px( 20 );   /* "Standard colours" */
        for (int col = 0; col < 10; col++, n++)
        {
            d->cells[n] = standard[col];
            SetRect( &d->rects[n], x0 + col * (CELL + CELL_GAP), y, x0 + col * (CELL + CELL_GAP) + CELL, y + CELL );
        }
        y += CELL + px( 8 );
        SetRect( &d->more, px( 4 ), y, p->width - px( 4 ), y + px( 28 ) );
        y += px( 32 );
    }
    d->count = n;
    p->height = y + px( 4 );
}

static void color_paint( struct popup *p )
{
    struct color_data *d = p->data;
    RECT r;

    if (p->hot == 100) fill_round( &p->surf, &d->automatic, (float)px( 4 ), th.hover, 255 );
    r = d->automatic;
    {
        RECT sw = { r.left + px( 8 ), r.top + px( 6 ), r.left + px( 24 ), r.bottom - px( 6 ) };
        if (d->highlight) outline_round( &p->surf, &sw, 0, th.dim, 255 );
        else fill( &p->surf, &sw, RGB( 0, 0, 0 ) );
    }
    r.left += px( 34 );
    draw_text( &p->surf, string( d->highlight ? IDS_NO_COLOR : IDS_AUTOMATIC ), -1, &r, get_font( F_UI ), th.text, DT_LEFT );

    if (!d->highlight)
    {
        RECT t = { px( 10 ), d->automatic.bottom + px( 6 ), p->width, d->automatic.bottom + px( 24 ) };
        draw_text( &p->surf, string( IDS_THEME_COLORS ), -1, &t, get_font( F_UI_SEMI ), th.text, DT_LEFT );
        t.top = d->rects[59].bottom + px( 6 );
        t.bottom = t.top + px( 20 );
        draw_text( &p->surf, string( IDS_STANDARD_COLORS ), -1, &t, get_font( F_UI_SEMI ), th.text, DT_LEFT );
    }
    for (int i = 0; i < d->count; i++)
    {
        RECT c = d->rects[i];
        fill( &p->surf, &c, d->cells[i] );
        if (d->cells[i] == RGB( 255, 255, 255 ) || (!d->highlight && i % 6 == 0 && i < 60))
            outline_round( &p->surf, &c, 0, th.dark ? th.popup_line : RGB( 0xe0, 0xe0, 0xe0 ), 255 );
        if (p->hot == i)
        {
            InflateRect( &c, px( 1 ), px( 1 ) );
            outline_round( &p->surf, &c, 0, RGB( 0xf2, 0x9b, 0x3e ), 255 );
            InflateRect( &c, -px( 1 ), -px( 1 ) );
            outline_round( &p->surf, &c, 0, RGB( 0xff, 0xff, 0xff ), 255 );
        }
    }
    if (!d->highlight)
    {
        if (p->hot == 101) fill_round( &p->surf, &d->more, (float)px( 4 ), th.hover, 255 );
        r = d->more;
        r.left += px( 34 );
        draw_text( &p->surf, string( IDS_MORE_COLORS ), -1, &r, get_font( F_UI ), th.text, DT_LEFT );
    }
}

static int color_hit( struct popup *p, POINT pt )
{
    struct color_data *d = p->data;

    if (PtInRect( &d->automatic, pt )) return 100;
    if (!d->highlight && PtInRect( &d->more, pt )) return 101;
    for (int i = 0; i < d->count; i++)
    {
        RECT r = d->rects[i];
        InflateRect( &r, px( 1 ), px( 1 ) );
        if (PtInRect( &r, pt )) return i;
    }
    return -2;
}

/* CLR_INVALID: nothing chosen */
COLORREF pick_color( HWND owner, const RECT *anchor, COLORREF current, BOOL highlight, BOOL *automatic )
{
    struct color_data data = { highlight, current };
    struct popup p = { 0 };
    int result;

    p.width = highlight ? px( 10 ) * 2 + 5 * (CELL + CELL_GAP) * 2 - CELL_GAP : px( 20 ) + 10 * (CELL + CELL_GAP) - CELL_GAP;
    p.width = max( p.width, px( 220 ) );
    p.data = &data;
    p.paint = color_paint;
    p.hit = color_hit;
    color_layout( &p );
    *automatic = FALSE;
    result = run_popup( owner, anchor, &p );
    if (result == 100)
    {
        *automatic = TRUE;
        return 0;
    }
    if (result == 101)
    {
        static COLORREF custom[16];
        CHOOSECOLORW cc = { sizeof(cc), owner };

        cc.rgbResult = current;
        cc.lpCustColors = custom;
        cc.Flags = CC_RGBINIT | CC_FULLOPEN;
        return ChooseColorW( &cc ) ? cc.rgbResult : CLR_INVALID;
    }
    if (result >= 0 && result < data.count) return data.cells[result];
    return CLR_INVALID;
}

/**********************************************************************
 *          The size of a new table
 */

#define TCOLS 10
#define TROWS 8

static void table_paint( struct popup *p )
{
    int cols = p->hot >= 0 ? p->hot % TCOLS + 1 : 0, rows = p->hot >= 0 ? p->hot / TCOLS + 1 : 0;
    WCHAR text[64];
    RECT t = { px( 10 ), px( 4 ), p->width, px( 28 ) };

    if (cols) swprintf( text, ARRAY_SIZE(text), string( IDS_TABLE_SIZE ), cols, rows );
    else lstrcpynW( text, string( IDS_INSERT_TABLE ), ARRAY_SIZE(text) );
    draw_text( &p->surf, text, -1, &t, get_font( F_UI_SEMI ), th.text, DT_LEFT );
    for (int r = 0; r < TROWS; r++)
        for (int c = 0; c < TCOLS; c++)
        {
            RECT cell = { px( 10 ) + c * px( 20 ), px( 32 ) + r * px( 20 ), px( 10 ) + c * px( 20 ) + px( 17 ), px( 32 ) + r * px( 20 ) + px( 17 ) };
            BOOL on = c < cols && r < rows;

            fill( &p->surf, &cell, on ? mix( th.popup, th.accent, 60 ) : th.popup );
            outline_round( &p->surf, &cell, 0, on ? th.accent : th.dim, 255 );
        }
}

static int table_hit( struct popup *p, POINT pt )
{
    int c = (pt.x - px( 10 )) / px( 20 ), r = (pt.y - px( 32 )) / px( 20 );

    if (pt.x < px( 10 ) || pt.y < px( 32 ) || c >= TCOLS || r >= TROWS) return -2;
    return r * TCOLS + c;
}

int pick_table( HWND owner, const RECT *anchor )
{
    struct popup p = { 0 };
    int result;

    p.width = px( 20 ) + TCOLS * px( 20 );
    p.height = px( 40 ) + TROWS * px( 20 );
    p.paint = table_paint;
    p.hit = table_hit;
    result = run_popup( owner, anchor, &p );
    if (result < 0) return 0;
    return MAKELONG( result % TCOLS + 1, result / TCOLS + 1 );
}

/**********************************************************************
 *          Symbols
 */

static const WCHAR symbols[] = L"\x20ac\x00a3\x00a5\x00a9\x00ae\x2122\x00b1\x2260\x2264\x2265\x00f7\x00d7\x221e\x00b5"
                               L"\x03b1\x03b2\x03c0\x03a9\x2211\x2026\x2014\x2013\x00ab\x00bb\x201e\x201c\x201d\x2018"
                               L"\x2019\x2022\x00a7\x00b6\x00b0\x2116\x2190\x2192\x2191\x2193\x2194\x2713\x2717\x263a";
#define SCOLS 7

static void symbol_paint( struct popup *p )
{
    int n = lstrlenW( symbols );
    LOGFONTW lf = { 0 };
    HFONT font;

    lf.lfHeight = -px( 18 );
    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpyW( lf.lfFaceName, body_face );
    font = CreateFontIndirectW( &lf );
    for (int i = 0; i < n; i++)
    {
        RECT r = { px( 6 ) + (i % SCOLS) * px( 34 ), px( 6 ) + (i / SCOLS) * px( 34 ), 0, 0 };
        r.right = r.left + px( 32 );
        r.bottom = r.top + px( 32 );
        if (p->hot == i) fill_round( &p->surf, &r, (float)px( 4 ), th.hover, 255 );
        outline_round( &p->surf, &r, (float)px( 4 ), th.popup_line, 255 );
        draw_text( &p->surf, symbols + i, 1, &r, font, th.text, DT_CENTER );
    }
    DeleteObject( font );
}

static int symbol_hit( struct popup *p, POINT pt )
{
    int c = (pt.x - px( 6 )) / px( 34 ), r = (pt.y - px( 6 )) / px( 34 ), i = r * SCOLS + c;

    if (pt.x < px( 6 ) || pt.y < px( 6 ) || c >= SCOLS || i >= lstrlenW( symbols )) return -2;
    return i;
}

WCHAR pick_symbol( HWND owner, const RECT *anchor )
{
    struct popup p = { 0 };
    int n = lstrlenW( symbols ), result;

    p.width = px( 12 ) + SCOLS * px( 34 );
    p.height = px( 12 ) + (n + SCOLS - 1) / SCOLS * px( 34 );
    p.paint = symbol_paint;
    p.hit = symbol_hit;
    result = run_popup( owner, anchor, &p );
    return result >= 0 ? symbols[result] : 0;
}

/**********************************************************************
 *          Fonts: a list, each in its own face
 */

struct font_list
{
    WCHAR (*names)[LF_FACESIZE];
    int count, size;
};

static int CALLBACK enum_font( const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM lparam )
{
    struct font_list *list = (struct font_list *)lparam;

    if (lf->lfFaceName[0] == '@') return 1;
    for (int i = 0; i < list->count; i++)
        if (!wcscmp( list->names[i], lf->lfFaceName )) return 1;
    if (list->count == list->size)
    {
        int size = max( 64, list->size * 2 );
        void *n = realloc( list->names, size * sizeof(*list->names) );
        if (!n) return 0;
        list->names = n;
        list->size = size;
    }
    lstrcpynW( list->names[list->count++], lf->lfFaceName, LF_FACESIZE );
    return 1;
}

static int __cdecl compare_names( const void *a, const void *b )
{
    return lstrcmpiW( a, b );
}

static struct font_list fonts;
static HFONT *font_cache;

static LRESULT CALLBACK font_popup_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    switch (msg)
    {
    case WM_MEASUREITEM:
        ((MEASUREITEMSTRUCT *)lparam)->itemHeight = px( 28 );
        return TRUE;
    case WM_DRAWITEM:
    {
        DRAWITEMSTRUCT *di = (DRAWITEMSTRUCT *)lparam;
        HBRUSH brush;
        RECT r = di->rcItem;

        if (di->itemID == (UINT)-1) return TRUE;
        brush = CreateSolidBrush( di->itemState & ODS_SELECTED ? th.hover : th.popup );
        FillRect( di->hDC, &r, brush );
        DeleteObject( brush );
        if (!font_cache[di->itemID])
        {
            LOGFONTW lf = { 0 };
            lf.lfHeight = -px( 15 );
            lf.lfCharSet = DEFAULT_CHARSET;
            lf.lfQuality = CLEARTYPE_QUALITY;
            lstrcpynW( lf.lfFaceName, fonts.names[di->itemID], LF_FACESIZE );
            font_cache[di->itemID] = CreateFontIndirectW( &lf );
        }
        {
            HGDIOBJ old = SelectObject( di->hDC, font_cache[di->itemID] );
            SetBkMode( di->hDC, TRANSPARENT );
            SetTextColor( di->hDC, th.text );
            r.left += px( 10 );
            DrawTextW( di->hDC, fonts.names[di->itemID], -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX );
            SelectObject( di->hDC, old );
        }
        return TRUE;
    }
    case WM_COMMAND:
        if (HIWORD( wparam ) == LBN_SELCHANGE && GetKeyState( VK_LBUTTON ) < 0) return 0;
        break;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

BOOL pick_font( HWND owner, const RECT *anchor, WCHAR *face, int len )
{
    static BOOL registered;
    MONITORINFO mon = { sizeof(mon) };
    HWND popup, list;
    int x = anchor->left, y = anchor->bottom + px( 2 ), w = px( 280 ), h = px( 420 ), chosen = -1;
    BOOL done = FALSE;
    MSG msg;

    if (!fonts.count)
    {
        LOGFONTW lf = { 0 };
        HDC hdc = GetDC( 0 );

        lf.lfCharSet = DEFAULT_CHARSET;
        EnumFontFamiliesExW( hdc, &lf, enum_font, (LPARAM)&fonts, 0 );
        ReleaseDC( 0, hdc );
        qsort( fonts.names, fonts.count, sizeof(*fonts.names), compare_names );
        font_cache = calloc( fonts.count, sizeof(HFONT) );
    }
    if (!fonts.count || !font_cache) return FALSE;
    if (!registered)
    {
        WNDCLASSW cls = { CS_DROPSHADOW, font_popup_proc, 0, 0, instance, NULL, NULL, NULL, NULL, L"ArcticWordPadFonts" };
        cls.hCursor = LoadCursorW( NULL, (LPCWSTR)IDC_ARROW );
        RegisterClassW( &cls );
        registered = TRUE;
    }
    GetMonitorInfoW( MonitorFromRect( anchor, MONITOR_DEFAULTTONEAREST ), &mon );
    if (y + h > mon.rcWork.bottom) h = max( px( 160 ), mon.rcWork.bottom - y );
    popup = CreateWindowExW( WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"ArcticWordPadFonts", NULL, WS_POPUP | WS_BORDER,
                             x, y, w, h, owner, NULL, instance, NULL );
    list = CreateWindowExW( 0, L"LISTBOX", NULL, WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS |
                            LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, 0, 0, w - 2, h - 2, popup, (HMENU)1, instance, NULL );
    for (int i = 0; i < fonts.count; i++) SendMessageW( list, LB_ADDSTRING, 0, (LPARAM)fonts.names[i] );
    {
        int sel = (int)SendMessageW( list, LB_FINDSTRINGEXACT, -1, (LPARAM)face );
        if (sel >= 0)
        {
            SendMessageW( list, LB_SETCURSEL, sel, 0 );
            SendMessageW( list, LB_SETTOPINDEX, max( sel - 3, 0 ), 0 );
        }
    }
    ShowWindow( popup, SW_SHOW );
    SetFocus( list );
    while (!done && GetMessageW( &msg, 0, 0, 0 ) > 0)
    {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) done = TRUE;
        else if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN)
        {
            chosen = (int)SendMessageW( list, LB_GETCURSEL, 0, 0 );
            done = TRUE;
        }
        else if (msg.message == WM_LBUTTONUP && msg.hwnd == list)
        {
            DispatchMessageW( &msg );
            chosen = (int)SendMessageW( list, LB_GETCURSEL, 0, 0 );
            done = TRUE;
            continue;
        }
        else if ((msg.message == WM_LBUTTONDOWN || msg.message == WM_NCLBUTTONDOWN || msg.message == WM_RBUTTONDOWN) &&
                 msg.hwnd != list && msg.hwnd != popup)
        {
            done = TRUE;
            continue;
        }
        if (!done)
        {
            TranslateMessage( &msg );
            DispatchMessageW( &msg );
        }
        if (GetForegroundWindow() != popup && GetForegroundWindow() != owner) done = TRUE;
    }
    DestroyWindow( popup );
    if (chosen < 0 || chosen >= fonts.count) return FALSE;
    lstrcpynW( face, fonts.names[chosen], len );
    return TRUE;
}
