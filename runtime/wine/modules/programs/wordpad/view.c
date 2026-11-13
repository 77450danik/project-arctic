/*
 * Arctic WordPad: the pages
 *
 * The text is riched20's, without a window of its own: this file is its
 * host (ITextHost). riched20 lays the text out as one long column the width
 * of a page's text; the column is cut into pages between its lines (and the
 * rows of its tables), and each page shows its piece at its place, as Word
 * shows its pages one under the other on the grey.
 *
 * The text's coordinates are the column's, from its top: riched20 never
 * scrolls (its client rectangle is taller than any text), this window does.
 * The mouse is not passed on, as its 16 bits would not reach past a few
 * dozen pages: a click becomes a character (EM_CHARFROMPOS takes a POINTL)
 * and a selection here.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdio.h>
#include "wordpad.h"
#include <imm.h>

/**********************************************************************
 *          ITextServices and ITextHost, as riched20 has them
 */

#if defined(__i386__)
#define TXCALL __thiscall
#else
#define TXCALL
#endif

typedef struct text_services text_services;
struct text_services_vtbl
{
    HRESULT (STDMETHODCALLTYPE *QueryInterface)( text_services *, REFIID, void ** );
    ULONG (STDMETHODCALLTYPE *AddRef)( text_services * );
    ULONG (STDMETHODCALLTYPE *Release)( text_services * );
    HRESULT (TXCALL *TxSendMessage)( text_services *, UINT, WPARAM, LPARAM, LRESULT * );
    HRESULT (TXCALL *TxDraw)( text_services *, DWORD, LONG, void *, void *, HDC, HDC, const RECTL *, const RECTL *,
                              RECT *, BOOL (CALLBACK *)(DWORD), DWORD, LONG );
    HRESULT (TXCALL *TxGetHScroll)( text_services *, LONG *, LONG *, LONG *, LONG *, BOOL * );
    HRESULT (TXCALL *TxGetVScroll)( text_services *, LONG *, LONG *, LONG *, LONG *, BOOL * );
    HRESULT (TXCALL *OnTxSetCursor)( text_services *, DWORD, LONG, void *, void *, HDC, HDC, const RECT *, INT, INT );
    HRESULT (TXCALL *TxQueryHitPoint)( text_services *, DWORD, LONG, void *, void *, HDC, HDC, const RECT *, INT, INT, DWORD * );
    HRESULT (TXCALL *OnTxInPlaceActivate)( text_services *, const RECT * );
    HRESULT (TXCALL *OnTxInPlaceDeactivate)( text_services * );
    HRESULT (TXCALL *OnTxUIActivate)( text_services * );
    HRESULT (TXCALL *OnTxUIDeactivate)( text_services * );
    HRESULT (TXCALL *TxGetText)( text_services *, BSTR * );
    HRESULT (TXCALL *TxSetText)( text_services *, const WCHAR * );
    HRESULT (TXCALL *TxGetCurTargetX)( text_services *, LONG * );
    HRESULT (TXCALL *TxGetBaseLinePos)( text_services *, LONG * );
    HRESULT (TXCALL *TxGetNaturalSize)( text_services *, DWORD, HDC, HDC, void *, DWORD, const SIZEL *, LONG *, LONG * );
    HRESULT (TXCALL *TxGetDropTarget)( text_services *, void ** );
    HRESULT (TXCALL *OnTxPropertyBitsChange)( text_services *, DWORD, DWORD );
    HRESULT (TXCALL *TxGetCachedSize)( text_services *, DWORD *, DWORD * );
};
struct text_services { const struct text_services_vtbl *vtbl; };

struct text_host;
struct text_host_vtbl
{
    HRESULT (STDMETHODCALLTYPE *QueryInterface)( struct text_host *, REFIID, void ** );
    ULONG (STDMETHODCALLTYPE *AddRef)( struct text_host * );
    ULONG (STDMETHODCALLTYPE *Release)( struct text_host * );
    HDC (TXCALL *TxGetDC)( struct text_host * );
    INT (TXCALL *TxReleaseDC)( struct text_host *, HDC );
    BOOL (TXCALL *TxShowScrollBar)( struct text_host *, INT, BOOL );
    BOOL (TXCALL *TxEnableScrollBar)( struct text_host *, INT, INT );
    BOOL (TXCALL *TxSetScrollRange)( struct text_host *, INT, LONG, INT, BOOL );
    BOOL (TXCALL *TxSetScrollPos)( struct text_host *, INT, INT, BOOL );
    void (TXCALL *TxInvalidateRect)( struct text_host *, const RECT *, BOOL );
    void (TXCALL *TxViewChange)( struct text_host *, BOOL );
    BOOL (TXCALL *TxCreateCaret)( struct text_host *, HBITMAP, INT, INT );
    BOOL (TXCALL *TxShowCaret)( struct text_host *, BOOL );
    BOOL (TXCALL *TxSetCaretPos)( struct text_host *, INT, INT );
    BOOL (TXCALL *TxSetTimer)( struct text_host *, UINT, UINT );
    void (TXCALL *TxKillTimer)( struct text_host *, UINT );
    void (TXCALL *TxScrollWindowEx)( struct text_host *, INT, INT, const RECT *, const RECT *, HRGN, RECT *, UINT );
    void (TXCALL *TxSetCapture)( struct text_host *, BOOL );
    void (TXCALL *TxSetFocus)( struct text_host * );
    void (TXCALL *TxSetCursor)( struct text_host *, HCURSOR, BOOL );
    BOOL (TXCALL *TxScreenToClient)( struct text_host *, POINT * );
    BOOL (TXCALL *TxClientToScreen)( struct text_host *, POINT * );
    HRESULT (TXCALL *TxActivate)( struct text_host *, LONG * );
    HRESULT (TXCALL *TxDeactivate)( struct text_host *, LONG );
    HRESULT (TXCALL *TxGetClientRect)( struct text_host *, RECT * );
    HRESULT (TXCALL *TxGetViewInset)( struct text_host *, RECT * );
    HRESULT (TXCALL *TxGetCharFormat)( struct text_host *, const CHARFORMATW ** );
    HRESULT (TXCALL *TxGetParaFormat)( struct text_host *, const PARAFORMAT ** );
    COLORREF (TXCALL *TxGetSysColor)( struct text_host *, int );
    HRESULT (TXCALL *TxGetBackStyle)( struct text_host *, int * );
    HRESULT (TXCALL *TxGetMaxLength)( struct text_host *, DWORD * );
    HRESULT (TXCALL *TxGetScrollBars)( struct text_host *, DWORD * );
    HRESULT (TXCALL *TxGetPasswordChar)( struct text_host *, WCHAR * );
    HRESULT (TXCALL *TxGetAcceleratorPos)( struct text_host *, LONG * );
    HRESULT (TXCALL *TxGetExtent)( struct text_host *, SIZEL * );
    HRESULT (TXCALL *OnTxCharFormatChange)( struct text_host *, const CHARFORMATW * );
    HRESULT (TXCALL *OnTxParaFormatChange)( struct text_host *, const PARAFORMAT * );
    HRESULT (TXCALL *TxGetPropertyBits)( struct text_host *, DWORD, DWORD * );
    HRESULT (TXCALL *TxNotify)( struct text_host *, DWORD, void * );
    HIMC (TXCALL *TxImmGetContext)( struct text_host * );
    void (TXCALL *TxImmReleaseContext)( struct text_host *, HIMC );
    HRESULT (TXCALL *TxGetSelectionBarWidth)( struct text_host *, LONG * );
};
struct text_host
{
    const struct text_host_vtbl *vtbl;
    HDC  print_dc;           /* a host that prints: no window */
    RECT format;
};

static const GUID iid_text_services = { 0x8d33f740, 0xcf58, 0x11ce, { 0xa8, 0x9d, 0x00, 0xaa, 0x00, 0x6c, 0xad, 0xc5 } };
static const GUID iid_text_host = { 0x13e670f4, 0x1a5a, 0x11cf, { 0xab, 0xeb, 0x00, 0xaa, 0x00, 0xb6, 0x5e, 0xa1 } };

#define TEXT_TALL   (1 << 28)        /* the client rectangle's height: no text reaches it */
#define TIMER_PAGINATE  0x100
#define TIMER_STATUS    0x101
#define TIMER_DRAGSCROLL 0x102

struct page_setup page = { 11906, 16838, 1440, 1440, 1440, 1440 };   /* A4, margins of 2.54 cm */
BOOL web_layout;

static text_services *services;
static IUnknown *services_unk;
static HWND view, status_bar;
static struct surface surf, status_surf;
static int zoom = 100;
static int scroll_x, scroll_y, doc_width, doc_height;
static RECT format_rect;                  /* riched20's client rectangle */
static int text_height;                   /* of the whole column */

struct band { LONG top, bottom; };        /* a page's piece of the column */
static struct band *bands;
static int band_count, band_size;
static int columns = 1;

static POINT caret_pos;                   /* in the column */
static int caret_height;
static BOOL caret_made;

static BOOL selecting;
static LONG anchor_start, anchor_end;     /* the word or paragraph a double or triple click took */
static int click_count;
static DWORD click_time;
static POINT click_point;
static int frozen;

/* WORDPAD_LOG=<file>: what the view does, for finding out */
void trace( const char *format, ... )
{
    static int enabled = -1;
    static char path[MAX_PATH];
    char line[512];
    va_list args;
    FILE *f;

    if (enabled < 0) enabled = GetEnvironmentVariableA( "WORDPAD_LOG", path, sizeof(path) ) > 0;
    if (!enabled || !(f = fopen( path, "a" ))) return;
    va_start( args, format );
    vsnprintf( line, sizeof(line), format, args );
    va_end( args );
    fputs( line, f );
    fclose( f );
}

LRESULT ed( UINT msg, WPARAM wparam, LPARAM lparam )
{
    LRESULT result = 0;

    if (services) services->vtbl->TxSendMessage( services, msg, wparam, lparam, &result );
    return result;
}

/**********************************************************************
 *          The geometry of the pages, in the window's pixels
 */

static int tw( int twips ) { return MulDiv( twips, dpi * zoom, 1440 * 100 ); }
static int gap(void) { return px( 16 ); }
static int page_w(void) { return tw( page.width ); }
static int page_h(void) { return tw( page.height ); }

static int content_w(void)
{
    RECT client;

    if (web_layout)
    {
        GetClientRect( view, &client );
        return max( client.right - 2 * px( 32 ), px( 120 ) );
    }
    return max( tw( page.width - page.left - page.right ), px( 40 ) );
}

static int content_h(void)
{
    return max( tw( page.height - page.top - page.bottom ), px( 40 ) );
}

static void page_rect( int index, RECT *rect )
{
    RECT client;
    int row = index / columns, col = index % columns, row_w;

    GetClientRect( view, &client );
    if (web_layout)
    {
        SetRect( rect, -scroll_x, -scroll_y, client.right - scroll_x,
                 max( client.bottom, text_height + 2 * px( 24 ) ) - scroll_y );
        return;
    }
    row_w = columns * page_w() + (columns - 1) * gap();
    rect->left = max( gap(), (client.right - row_w) / 2 ) + col * (page_w() + gap()) - scroll_x;
    rect->top = gap() + row * (page_h() + gap()) - scroll_y;
    rect->right = rect->left + page_w();
    rect->bottom = rect->top + page_h();
}

/* where a page's text begins */
static void text_origin( int index, POINT *pt )
{
    RECT r;

    page_rect( index, &r );
    if (web_layout)
    {
        pt->x = r.left + px( 32 );
        pt->y = r.top + px( 24 );
        return;
    }
    pt->x = r.left + tw( page.left );
    pt->y = r.top + tw( page.top );
}

static int band_of( LONG y )
{
    int lo = 0, hi = band_count - 1;

    if (band_count <= 1) return 0;
    while (lo < hi)
    {
        int mid = (lo + hi + 1) / 2;
        if (bands[mid].top <= y) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

static void to_window( LONG x, LONG y, POINT *out )
{
    int index = band_of( y );
    POINT o;

    text_origin( index, &o );
    out->x = o.x + x;
    out->y = o.y + y - (band_count ? bands[index].top : 0);
}

/* the column's point under a window's point: on the page under it, or the nearest */
static void to_text( int wx, int wy, POINTL *out )
{
    int best = 0, best_dist = INT_MAX;
    POINT o;
    RECT r;

    for (int i = 0; i < max( band_count, 1 ); i++)
    {
        int dx, dy, dist;

        page_rect( i, &r );
        dx = wx < r.left ? r.left - wx : wx >= r.right ? wx - r.right : 0;
        dy = wy < r.top ? r.top - wy : wy >= r.bottom ? wy - r.bottom : 0;
        dist = dx + dy * 4;
        if (dist < best_dist)
        {
            best_dist = dist;
            best = i;
        }
        if (!dist) break;
    }
    text_origin( best, &o );
    out->x = wx - o.x;
    out->y = wy - o.y;
    if (band_count)
    {
        LONG height = bands[best].bottom - bands[best].top;

        if (out->y < 0) out->y = 0;
        if (best < band_count - 1 && out->y >= height) out->y = max( height - 1, 0 );
        out->y += bands[best].top;
    }
}

/**********************************************************************
 *          The host
 */

static HRESULT STDMETHODCALLTYPE host_QueryInterface( struct text_host *iface, REFIID iid, void **out )
{
    if (IsEqualIID( iid, &IID_IUnknown ) || IsEqualIID( iid, &iid_text_host ))
    {
        *out = iface;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE host_AddRef( struct text_host *iface ) { return 2; }
static ULONG STDMETHODCALLTYPE host_Release( struct text_host *iface ) { return 1; }

static HDC TXCALL host_TxGetDC( struct text_host *iface ) { return iface->print_dc ? iface->print_dc : GetDC( view ); }
static INT TXCALL host_TxReleaseDC( struct text_host *iface, HDC hdc ) { return iface->print_dc ? 1 : ReleaseDC( view, hdc ); }
static BOOL TXCALL host_TxShowScrollBar( struct text_host *iface, INT bar, BOOL show ) { return TRUE; }
static BOOL TXCALL host_TxEnableScrollBar( struct text_host *iface, INT bar, INT arrows ) { return TRUE; }
static BOOL TXCALL host_TxSetScrollRange( struct text_host *iface, INT bar, LONG min, INT max, BOOL redraw ) { return TRUE; }
static BOOL TXCALL host_TxSetScrollPos( struct text_host *iface, INT bar, INT pos, BOOL redraw ) { return TRUE; }

static void invalidate_text( LONG top, LONG bottom, LONG left, LONG right )
{
    RECT client;

    GetClientRect( view, &client );
    for (int i = band_of( top ); i < max( band_count, 1 ); i++)
    {
        LONG band_top = band_count ? bands[i].top : 0;
        LONG band_bottom = band_count && i < band_count - 1 ? bands[i].bottom : TEXT_TALL;
        LONG t = max( top, band_top ), b = min( bottom, band_bottom );
        POINT o;
        RECT r;

        if (band_top >= bottom) break;
        if (t >= b) continue;
        text_origin( i, &o );
        SetRect( &r, o.x + left, o.y + t - band_top, o.x + right, o.y + b - band_top );
        if (r.top > client.bottom) break;
        InflateRect( &r, px( 2 ), 0 );
        InvalidateRect( view, &r, FALSE );
    }
}

static void TXCALL host_TxInvalidateRect( struct text_host *iface, const RECT *rect, BOOL mode )
{
    if (iface->print_dc) return;
    if (!rect) InvalidateRect( view, NULL, FALSE );
    else invalidate_text( rect->top, rect->bottom, rect->left - px( 40 ), rect->right + px( 40 ) );
}

static void TXCALL host_TxViewChange( struct text_host *iface, BOOL update )
{
    if (update && !frozen && !iface->print_dc) UpdateWindow( view );
}

static BOOL TXCALL host_TxCreateCaret( struct text_host *iface, HBITMAP bitmap, INT width, INT height )
{
    if (iface->print_dc) return TRUE;
    caret_height = height;
    caret_made = CreateCaret( view, bitmap, max( width, px( 1 ) ), height );
    return caret_made;
}

static BOOL TXCALL host_TxShowCaret( struct text_host *iface, BOOL show )
{
    if (iface->print_dc) return TRUE;
    return show ? ShowCaret( view ) : HideCaret( view );
}

static void place_caret(void)
{
    POINT pt;

    to_window( caret_pos.x, caret_pos.y, &pt );
    SetCaretPos( pt.x, pt.y );
}

static BOOL TXCALL host_TxSetCaretPos( struct text_host *iface, INT x, INT y )
{
    if (iface->print_dc) return TRUE;
    caret_pos.x = x;
    caret_pos.y = y;
    place_caret();
    return TRUE;
}

static BOOL TXCALL host_TxSetTimer( struct text_host *iface, UINT id, UINT timeout )
{
    if (iface->print_dc) return TRUE;
    return SetTimer( view, id & 0xff, timeout, NULL ) != 0;
}

static void TXCALL host_TxKillTimer( struct text_host *iface, UINT id ) { if (!iface->print_dc) KillTimer( view, id & 0xff ); }

static void TXCALL host_TxScrollWindowEx( struct text_host *iface, INT dx, INT dy, const RECT *scroll, const RECT *clip,
                                          HRGN update, RECT *updated, UINT flags )
{
    if (!iface->print_dc) InvalidateRect( view, NULL, FALSE );
}

static void TXCALL host_TxSetCapture( struct text_host *iface, BOOL capture )
{
    if (iface->print_dc) return;
    if (capture) SetCapture( view );
    else if (GetCapture() == view) ReleaseCapture();
}

static void TXCALL host_TxSetFocus( struct text_host *iface ) { if (!iface->print_dc) SetFocus( view ); }
static void TXCALL host_TxSetCursor( struct text_host *iface, HCURSOR cursor, BOOL text ) { if (!iface->print_dc) SetCursor( cursor ); }

static BOOL TXCALL host_TxScreenToClient( struct text_host *iface, POINT *pt )
{
    POINTL t;

    ScreenToClient( view, pt );
    to_text( pt->x, pt->y, &t );
    pt->x = t.x;
    pt->y = t.y;
    return TRUE;
}

static BOOL TXCALL host_TxClientToScreen( struct text_host *iface, POINT *pt )
{
    to_window( pt->x, pt->y, pt );
    return ClientToScreen( view, pt );
}

static HRESULT TXCALL host_TxActivate( struct text_host *iface, LONG *old ) { return S_OK; }
static HRESULT TXCALL host_TxDeactivate( struct text_host *iface, LONG state ) { return S_OK; }

static HRESULT TXCALL host_TxGetClientRect( struct text_host *iface, RECT *rect )
{
    *rect = iface->print_dc ? iface->format : format_rect;
    return S_OK;
}

static HRESULT TXCALL host_TxGetViewInset( struct text_host *iface, RECT *rect )
{
    SetRectEmpty( rect );
    return S_OK;
}

static CHARFORMAT2W host_char_format;
static PARAFORMAT2 host_para_format;

static HRESULT TXCALL host_TxGetCharFormat( struct text_host *iface, const CHARFORMATW **format )
{
    if (!host_char_format.cbSize)
    {
        host_char_format.cbSize = sizeof(host_char_format);
        host_char_format.dwMask = CFM_ALL2 & ~(CFM_BACKCOLOR);
        host_char_format.dwMask = CFM_BOLD | CFM_ITALIC | CFM_UNDERLINE | CFM_STRIKEOUT | CFM_PROTECTED | CFM_LINK |
                                  CFM_SIZE | CFM_COLOR | CFM_FACE | CFM_OFFSET | CFM_CHARSET;
        host_char_format.dwEffects = CFE_AUTOCOLOR;
        host_char_format.yHeight = 11 * 20;
        host_char_format.bCharSet = DEFAULT_CHARSET;
        lstrcpyW( host_char_format.szFaceName, body_face );
    }
    *format = (const CHARFORMATW *)&host_char_format;
    return S_OK;
}

static HRESULT TXCALL host_TxGetParaFormat( struct text_host *iface, const PARAFORMAT **format )
{
    if (!host_para_format.cbSize)
    {
        host_para_format.cbSize = sizeof(host_para_format);
        host_para_format.dwMask = PFM_ALL;
        host_para_format.wAlignment = PFA_LEFT;
        host_para_format.cTabCount = 1;
        host_para_format.rgxTabs[0] = 720;
    }
    *format = (const PARAFORMAT *)&host_para_format;
    return S_OK;
}

static COLORREF TXCALL host_TxGetSysColor( struct text_host *iface, int index )
{
    switch (index)
    {
    case COLOR_WINDOW: return RGB( 255, 255, 255 );
    case COLOR_WINDOWTEXT: return RGB( 0, 0, 0 );
    case COLOR_HIGHLIGHT: return th.sel_back;
    case COLOR_HIGHLIGHTTEXT: return RGB( 0, 0, 0 );
    case COLOR_GRAYTEXT: return RGB( 0x80, 0x80, 0x80 );
    }
    return GetSysColor( index );
}

static HRESULT TXCALL host_TxGetBackStyle( struct text_host *iface, int *style )
{
    *style = 1;   /* TXTBACK_OPAQUE */
    return S_OK;
}

static HRESULT TXCALL host_TxGetMaxLength( struct text_host *iface, DWORD *length )
{
    *length = 0x7fffffff;
    return S_OK;
}

static HRESULT TXCALL host_TxGetScrollBars( struct text_host *iface, DWORD *bars )
{
    *bars = 0;
    return S_OK;
}

static HRESULT TXCALL host_TxGetPasswordChar( struct text_host *iface, WCHAR *c )
{
    *c = 0;
    return S_FALSE;
}

static HRESULT TXCALL host_TxGetAcceleratorPos( struct text_host *iface, LONG *pos )
{
    *pos = -1;
    return S_OK;
}

static HRESULT TXCALL host_TxGetExtent( struct text_host *iface, SIZEL *extent ) { return E_NOTIMPL; }
static HRESULT TXCALL host_OnTxCharFormatChange( struct text_host *iface, const CHARFORMATW *format ) { return S_OK; }
static HRESULT TXCALL host_OnTxParaFormatChange( struct text_host *iface, const PARAFORMAT *format ) { return S_OK; }

#define TXTBIT_RICHTEXT     0x1
#define TXTBIT_MULTILINE    0x2
#define TXTBIT_WORDWRAP     0x400
#define TXTBIT_DISABLEDRAG  0x1000
#define TXTBIT_EXTENTCHANGE 0x80000
#define TXTBIT_CLIENTRECTCHANGE 0x100000

static HRESULT TXCALL host_TxGetPropertyBits( struct text_host *iface, DWORD mask, DWORD *bits )
{
    *bits = (TXTBIT_RICHTEXT | TXTBIT_MULTILINE | TXTBIT_WORDWRAP | TXTBIT_DISABLEDRAG) & mask;
    return S_OK;
}

static void schedule_paginate(void)
{
    SetTimer( view, TIMER_PAGINATE, 120, NULL );
}

static HRESULT TXCALL host_TxNotify( struct text_host *iface, DWORD code, void *data )
{
    if (iface->print_dc) return S_OK;
    switch (code)
    {
    case EN_CHANGE:
        if (!frozen)
        {
            set_modified( TRUE );
            schedule_paginate();
            SetTimer( view, TIMER_STATUS, 400, NULL );
        }
        break;
    case EN_SELCHANGE:
        if (!frozen)
        {
            ribbon_refresh();
            status_update();
        }
        break;
    case EN_REQUESTRESIZE:
    {
        REQRESIZE *req = data;
        int height = req->rc.bottom - req->rc.top;

        if (height != text_height)
        {
            text_height = height;
            schedule_paginate();
        }
        break;
    }
    }
    return S_OK;
}

static HIMC TXCALL host_TxImmGetContext( struct text_host *iface ) { return iface->print_dc ? NULL : ImmGetContext( view ); }
static void TXCALL host_TxImmReleaseContext( struct text_host *iface, HIMC himc ) { if (!iface->print_dc) ImmReleaseContext( view, himc ); }

static HRESULT TXCALL host_TxGetSelectionBarWidth( struct text_host *iface, LONG *width )
{
    *width = 0;
    return S_OK;
}

static const struct text_host_vtbl host_vtbl =
{
    host_QueryInterface, host_AddRef, host_Release, host_TxGetDC, host_TxReleaseDC, host_TxShowScrollBar,
    host_TxEnableScrollBar, host_TxSetScrollRange, host_TxSetScrollPos, host_TxInvalidateRect, host_TxViewChange,
    host_TxCreateCaret, host_TxShowCaret, host_TxSetCaretPos, host_TxSetTimer, host_TxKillTimer,
    host_TxScrollWindowEx, host_TxSetCapture, host_TxSetFocus, host_TxSetCursor, host_TxScreenToClient,
    host_TxClientToScreen, host_TxActivate, host_TxDeactivate, host_TxGetClientRect, host_TxGetViewInset,
    host_TxGetCharFormat, host_TxGetParaFormat, host_TxGetSysColor, host_TxGetBackStyle, host_TxGetMaxLength,
    host_TxGetScrollBars, host_TxGetPasswordChar, host_TxGetAcceleratorPos, host_TxGetExtent,
    host_OnTxCharFormatChange, host_OnTxParaFormatChange, host_TxGetPropertyBits, host_TxNotify,
    host_TxImmGetContext, host_TxImmReleaseContext, host_TxGetSelectionBarWidth,
};

static struct text_host host = { &host_vtbl };

static BOOL create_services(void)
{
    HRESULT (WINAPI *create)( IUnknown *, struct text_host *, IUnknown ** );
    HMODULE module = LoadLibraryW( L"riched20.dll" );

    if (!module) return FALSE;
    create = (void *)GetProcAddress( module, "CreateTextServices" );
    if (!create || FAILED(create( NULL, &host, &services_unk ))) return FALSE;
    if (FAILED(IUnknown_QueryInterface( services_unk, &iid_text_services, (void **)&services ))) return FALSE;
    services->vtbl->Release( services );
    SetRect( &format_rect, 0, 0, content_w(), TEXT_TALL );
    services->vtbl->OnTxInPlaceActivate( services, &format_rect );
    ed( EM_SETEVENTMASK, 0, ENM_CHANGE | ENM_SELCHANGE | ENM_REQUESTRESIZE );
    ed( EM_EXLIMITTEXT, 0, 0x7ffffffe );
    ed( EM_SETUNDOLIMIT, 1000, 0 );
    ed( EM_SETEDITSTYLE, SES_EMULATESYSEDIT, 0 );
    ed( EM_AUTOURLDETECT, FALSE, 0 );
    return TRUE;
}

/**********************************************************************
 *          Pages
 */

static LRESULT tsend( text_services *ts, UINT msg, WPARAM wparam, LPARAM lparam )
{
    LRESULT result = 0;

    ts->vtbl->TxSendMessage( ts, msg, wparam, lparam, &result );
    return result;
}

struct band_list
{
    struct band *bands;
    int count, size;
};

static void add_band( struct band_list *list, LONG top )
{
    if (list->count == list->size)
    {
        int size = max( 16, list->size * 2 );
        struct band *b = realloc( list->bands, size * sizeof(*b) );

        if (!b) return;
        list->bands = b;
        list->size = size;
    }
    if (list->count) list->bands[list->count - 1].bottom = top;
    list->bands[list->count].top = top;
    list->bands[list->count].bottom = top;
    list->count++;
}

/* the blocks of a riched20 that cannot name them: its lines, one after
 * the other, those that go back up (the cells of a row) taken with the rest */
static int guess_blocks( text_services *ts, LONG total, struct arctic_layout_block **out )
{
    int lines = tsend( ts, EM_GETLINECOUNT, 0, 0 ), count = 0, last_height = 18;
    struct arctic_layout_block *blocks = calloc( max( lines, 1 ), sizeof(*blocks) );
    LONG *tops = calloc( max( lines, 1 ), sizeof(LONG) );

    if (!blocks || !tops)
    {
        free( blocks );
        free( tops );
        return 0;
    }
    for (int i = 0; i < lines; i++)
    {
        POINTL pt = { 0, 0 };

        tsend( ts, EM_POSFROMCHAR, (WPARAM)&pt, tsend( ts, EM_LINEINDEX, i, 0 ) );
        tops[i] = pt.y;
    }
    for (int i = 0; i < lines; i++)
    {
        LONG top = tops[i], bottom;

        if (i + 1 < lines && tops[i + 1] > top) last_height = (bottom = tops[i + 1]) - top;
        else if (i + 1 == lines && total > top) bottom = total;
        else bottom = top + last_height;
        if (count && top < blocks[count - 1].bottom)
        {
            blocks[count - 1].bottom = max( blocks[count - 1].bottom, bottom );
            continue;
        }
        blocks[count].top = top;
        blocks[count].bottom = bottom;
        count++;
    }
    free( tops );
    *out = blocks;
    return count;
}

/* the column cut into pieces of a page's height, between its blocks */
static void paginate( text_services *ts, LONG height, LONG total, struct band_list *list )
{
    struct arctic_layout_block *blocks = NULL;
    LONG start = 0, last_bottom = 0;
    int count;

    list->count = 0;
    if ((count = tsend( ts, EM_ARCTIC_GETLAYOUT, 0, 0 )) > 0 && (blocks = calloc( count, sizeof(*blocks) )))
        count = tsend( ts, EM_ARCTIC_GETLAYOUT, count, (LPARAM)blocks );
    else count = guess_blocks( ts, total, &blocks );

    add_band( list, 0 );
    for (int i = 0; i < count; i++)
    {
        const struct arctic_layout_block *b = &blocks[i];

        if (b->top > start && ((b->flags & ALB_PAGEBREAK) || b->bottom - start > height))
        {
            LONG top = b->top;

            /* a heading stays with what follows it: it goes over too */
            if (!(b->flags & ALB_PAGEBREAK) && i > 0 && (blocks[i - 1].flags & ALB_KEEPNEXT) &&
                blocks[i - 1].top > start)
                top = blocks[i - 1].top;
            start = top;
            add_band( list, start );
        }
        last_bottom = max( last_bottom, b->bottom );
    }
    if (list->count) list->bands[list->count - 1].bottom = max( last_bottom, total );
    free( blocks );
}

void view_repaginate(void)
{
    struct band_list list = { bands, 0, band_size };
    int old_count = band_count;

    KillTimer( view, TIMER_PAGINATE );
    if (!services) return;
    if (web_layout)
    {
        add_band( &list, 0 );
        list.bands[0].bottom = TEXT_TALL;
    }
    else paginate( services, content_h(), text_height, &list );
    bands = list.bands;
    band_count = list.count;
    band_size = list.size;

    /* the pages side by side, as many as the window holds */
    {
        RECT client;

        GetClientRect( view, &client );
        columns = 1;
        if (!web_layout)
        {
            int fit = (client.right - gap()) / (page_w() + gap());
            columns = max( 1, min( fit, band_count ) );
            doc_width = columns * page_w() + (columns + 1) * gap();
            doc_height = (band_count + columns - 1) / columns * (page_h() + gap()) + gap();
        }
        else
        {
            doc_width = client.right;
            doc_height = max( text_height + 2 * px( 24 ), client.bottom );
        }
    }
    {
        SCROLLINFO info = { sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS };
        RECT client;

        GetClientRect( view, &client );
        scroll_y = max( 0, min( scroll_y, doc_height - client.bottom ) );
        scroll_x = max( 0, min( scroll_x, doc_width - client.right ) );
        info.nMax = doc_height - 1;
        info.nPage = client.bottom;
        info.nPos = scroll_y;
        SetScrollInfo( view, SB_VERT, &info, TRUE );
        info.nMax = doc_width - 1;
        info.nPage = client.right;
        info.nPos = scroll_x;
        SetScrollInfo( view, SB_HORZ, &info, TRUE );
    }
    place_caret();
    InvalidateRect( view, NULL, FALSE );
    if (old_count != band_count) status_update();
}

int view_page_count(void) { return max( band_count, 1 ); }

int view_current_page(void)
{
    return band_of( caret_pos.y ) + 1;
}

static void set_format_width(void)
{
    int width = content_w();

    if (format_rect.right == width) return;
    SetRect( &format_rect, 0, 0, width, TEXT_TALL );
    services->vtbl->OnTxInPlaceActivate( services, &format_rect );
}

void view_set_page( const struct page_setup *setup )
{
    page = *setup;
    if (!services) return;
    set_format_width();
    ed( EM_REQUESTRESIZE, 0, 0 );
    view_repaginate();
}

void view_set_zoom( int percent )
{
    percent = max( 10, min( percent, 500 ) );
    if (percent == zoom) return;
    /* the text under the middle of the window stays there */
    {
        RECT client;
        POINTL mid;
        POINT back;

        GetClientRect( view, &client );
        to_text( client.right / 2, client.bottom / 2, &mid );
        mid.x = MulDiv( mid.x, percent, zoom );
        mid.y = MulDiv( mid.y, percent, zoom );
        zoom = percent;
        ed( EM_SETZOOM, zoom, 100 );
        set_format_width();
        ed( EM_REQUESTRESIZE, 0, 0 );
        view_repaginate();
        to_window( mid.x, mid.y, &back );
        scroll_y += back.y - client.bottom / 2;
        view_repaginate();
    }
    status_update();
    ribbon_refresh();
}

int view_zoom(void) { return zoom; }

int view_fit_zoom( BOOL whole_page )
{
    RECT client;
    int w, h;

    GetClientRect( view, &client );
    w = MulDiv( client.right - 2 * gap(), 100 * 1440, dpi * page.width );
    if (!whole_page) return w;
    h = MulDiv( client.bottom - 2 * gap(), 100 * 1440, dpi * page.height );
    return min( w, h );
}

void view_set_web( BOOL web )
{
    if (web == web_layout) return;
    web_layout = web;
    scroll_x = scroll_y = 0;
    set_format_width();
    ed( EM_REQUESTRESIZE, 0, 0 );
    view_repaginate();
    ribbon_refresh();
    status_update();
}

void view_freeze( BOOL freeze )
{
    if (freeze) frozen++;
    else if (frozen && !--frozen)
    {
        ed( EM_REQUESTRESIZE, 0, 0 );
        view_repaginate();
        ribbon_refresh();
        status_update();
    }
}

static void scroll_to( int x, int y )
{
    RECT client;
    int old_x = scroll_x, old_y = scroll_y;

    GetClientRect( view, &client );
    scroll_y = max( 0, min( y, doc_height - client.bottom ) );
    scroll_x = max( 0, min( x, doc_width - client.right ) );
    if (scroll_x == old_x && scroll_y == old_y) return;
    SetScrollPos( view, SB_VERT, scroll_y, TRUE );
    SetScrollPos( view, SB_HORZ, scroll_x, TRUE );
    ScrollWindowEx( view, old_x - scroll_x, old_y - scroll_y, NULL, NULL, NULL, NULL, SW_INVALIDATE );
    place_caret();
}

void view_scroll_caret(void)
{
    RECT client;
    POINT pt;
    int margin = px( 24 );

    if (!view) return;
    GetClientRect( view, &client );
    to_window( caret_pos.x, caret_pos.y, &pt );
    if (pt.y < margin) scroll_to( scroll_x, scroll_y + pt.y - margin );
    else if (pt.y + caret_height > client.bottom - margin)
        scroll_to( scroll_x, scroll_y + pt.y + caret_height - client.bottom + margin );
    to_window( caret_pos.x, caret_pos.y, &pt );
    if (pt.x < margin) scroll_to( scroll_x + pt.x - margin, scroll_y );
    else if (pt.x > client.right - margin) scroll_to( scroll_x + pt.x - client.right + margin, scroll_y );
}

void view_focus(void) { if (view) SetFocus( view ); }

void view_theme_changed(void)
{
    InvalidateRect( view, NULL, FALSE );
    InvalidateRect( status_bar, NULL, FALSE );
}

/**********************************************************************
 *          Text in and out
 */

struct stream_data
{
    const char *data;
    size_t len, pos;
    struct buffer out;
};

static DWORD CALLBACK read_cb( DWORD_PTR cookie, BYTE *buffer, LONG size, LONG *done )
{
    struct stream_data *s = (struct stream_data *)cookie;
    size_t n = min( (size_t)size, s->len - s->pos );

    memcpy( buffer, s->data + s->pos, n );
    s->pos += n;
    *done = (LONG)n;
    return 0;
}

static DWORD CALLBACK write_cb( DWORD_PTR cookie, BYTE *buffer, LONG size, LONG *done )
{
    struct stream_data *s = (struct stream_data *)cookie;

    buf_add( &s->out, buffer, size );
    *done = size;
    return 0;
}

BOOL stream_in( const char *data, size_t len, UINT format )
{
    struct stream_data s = { data, len, 0 };
    EDITSTREAM es = { (DWORD_PTR)&s, 0, read_cb };

    view_freeze( TRUE );
    ed( EM_STREAMIN, format, (LPARAM)&es );
    view_freeze( FALSE );
    return !es.dwError;
}

char *stream_out( size_t *len, UINT format )
{
    struct stream_data s = { 0 };
    EDITSTREAM es = { (DWORD_PTR)&s, 0, write_cb };

    ed( EM_STREAMOUT, format, (LPARAM)&es );
    buf_add( &s.out, "\0\0", 2 );
    *len = s.out.len - 2;
    return s.out.data;
}

void view_reset_text(void)
{
    static const char empty[] = "{\\rtf1\\ansi }";

    frozen++;
    stream_in( empty, sizeof(empty) - 1, SF_RTF );
    apply_normal_defaults();
    ed( EM_EMPTYUNDOBUFFER, 0, 0 );
    ed( EM_SETSEL, 0, 0 );
    frozen--;
    scroll_x = scroll_y = 0;
    ed( EM_REQUESTRESIZE, 0, 0 );
    view_repaginate();
    ribbon_refresh();
    status_update();
}

/**********************************************************************
 *          The mouse
 */

static LONG para_bound( LONG cp, BOOL forward )
{
    WCHAR buf[256];
    LONG length = ed( WM_GETTEXTLENGTH, 0, 0 );

    for (;;)
    {
        TEXTRANGEW range;
        LONG n;

        if (forward)
        {
            if (cp >= length) return length;
            range.chrg.cpMin = cp;
            range.chrg.cpMax = min( cp + 255, length );
        }
        else
        {
            if (cp <= 0) return 0;
            range.chrg.cpMin = max( cp - 255, 0 );
            range.chrg.cpMax = cp;
        }
        range.lpstrText = buf;
        n = ed( EM_GETTEXTRANGE, 0, (LPARAM)&range );
        if (forward)
        {
            for (LONG i = 0; i < n; i++) if (buf[i] == '\r') return cp + i + 1;
            cp += n;
            if (!n) return cp;
        }
        else
        {
            for (LONG i = n - 1; i >= 0; i--) if (buf[i] == '\r') return range.chrg.cpMin + i + 1;
            cp = range.chrg.cpMin;
        }
    }
}

static LONG char_at( int x, int y )
{
    POINTL pt;

    to_text( x, y, &pt );
    return ed( EM_CHARFROMPOS, 0, (LPARAM)&pt );
}

static void select_range( LONG anchor, LONG active )
{
    ed( EM_SETSEL, anchor, active );
}

static void mouse_down( int x, int y, WPARAM keys )
{
    LONG cp = char_at( x, y );
    DWORD now = GetMessageTime();
    CHARRANGE sel;

    if (now - click_time <= GetDoubleClickTime() && abs( x - click_point.x ) <= GetSystemMetrics( SM_CXDOUBLECLK ) &&
        abs( y - click_point.y ) <= GetSystemMetrics( SM_CYDOUBLECLK ))
        click_count = click_count % 3 + 1;
    else click_count = 1;
    click_time = now;
    click_point.x = x;
    click_point.y = y;
    SetFocus( view );
    ed( EM_EXGETSEL, 0, (LPARAM)&sel );

    if (click_count == 2)
    {
        anchor_start = ed( EM_FINDWORDBREAK, WB_MOVEWORDLEFT, cp + 1 );
        anchor_end = ed( EM_FINDWORDBREAK, WB_MOVEWORDRIGHT, anchor_start );
        if (anchor_start > cp) anchor_start = ed( EM_FINDWORDBREAK, WB_MOVEWORDLEFT, cp );
        select_range( anchor_start, anchor_end );
    }
    else if (click_count == 3 || (keys & MK_CONTROL && click_count == 1 && FALSE))
    {
        anchor_start = para_bound( cp, FALSE );
        anchor_end = para_bound( cp, TRUE );
        select_range( anchor_start, anchor_end );
    }
    else if (keys & MK_SHIFT)
    {
        LONG anchor = anchor_start;

        if (anchor < 0 || (anchor != sel.cpMin && anchor != sel.cpMax)) anchor = sel.cpMin;
        anchor_start = anchor_end = anchor;
        select_range( anchor, cp );
    }
    else
    {
        anchor_start = anchor_end = cp;
        select_range( cp, cp );
    }
    selecting = TRUE;
    SetCapture( view );
}

static void mouse_drag( int x, int y )
{
    LONG cp = char_at( x, y );

    if (click_count == 2)
    {
        if (cp < anchor_start) select_range( anchor_end, ed( EM_FINDWORDBREAK, WB_MOVEWORDLEFT, cp + 1 ) );
        else if (cp > anchor_end) select_range( anchor_start, ed( EM_FINDWORDBREAK, WB_MOVEWORDRIGHT, cp ) );
        else select_range( anchor_start, anchor_end );
    }
    else if (click_count == 3)
    {
        if (cp < anchor_start) select_range( anchor_end, para_bound( cp, FALSE ) );
        else select_range( anchor_start, max( anchor_end, para_bound( cp, TRUE ) ) );
    }
    else select_range( anchor_start, cp );
}

static void context_menu( int x, int y )
{
    static const int ids[] = { IDM_CUT, IDM_COPY, IDM_PASTE, 0, IDM_FONTDIALOG, IDM_PARADIALOG, 0, IDM_LINK };
    static const char *const icons[] = { "cut", "copy", "paste", NULL, NULL, NULL, NULL, "link" };
    RECT anchor;
    POINT pt = { x, y };
    int id;

    ClientToScreen( view, &pt );
    SetRect( &anchor, pt.x, pt.y, pt.x, pt.y );
    if ((id = menu_popup( frame, &anchor, ids, icons, ARRAY_SIZE(ids), -1 ))) command( id );
}

/**********************************************************************
 *          The window
 */

static void paint( HDC hdc, const RECT *update )
{
    RECT client, r;

    GetClientRect( view, &client );
    if (!surface_size( &surf, client.right, client.bottom )) return;
    fill( &surf, update, th.canvas );

    for (int i = 0; i < max( band_count, 1 ); i++)
    {
        RECT text, clip;
        POINT o;
        LONG top = band_count ? bands[i].top : 0;
        LONG bottom = band_count && i < band_count - 1 ? bands[i].bottom : top + (web_layout ? TEXT_TALL / 2 : content_h());
        RECT update_text;
        RECTL bounds = { format_rect.left, format_rect.top, format_rect.right, format_rect.bottom };

        page_rect( i, &r );
        if (r.top > update->bottom) break;
        if (r.bottom < update->top - px( 12 ) || r.right < update->left || r.left > update->right) continue;

        if (!web_layout)
        {
            shadow_rect( &surf, &r, px( 6 ), th.dark ? 120 : 26 );
            fill( &surf, &r, RGB( 255, 255, 255 ) );
        }
        else fill( &surf, &r, RGB( 255, 255, 255 ) );

        text_origin( i, &o );
        SetRect( &text, r.left, o.y, r.right, o.y + (web_layout ? r.bottom - o.y : min( bottom - top, content_h() )) );
        if (!web_layout && i < band_count - 1) text.bottom = o.y + (bottom - top);
        if (!IntersectRect( &clip, &text, update )) continue;

        SaveDC( surf.dc );
        IntersectClipRect( surf.dc, clip.left, clip.top, clip.right, clip.bottom );
        SetViewportOrgEx( surf.dc, o.x, o.y - top, NULL );
        SetRect( &update_text, clip.left - o.x, clip.top - o.y + top, clip.right - o.x, clip.bottom - o.y + top );
        {
            HRESULT hr = services->vtbl->TxDraw( services, DVASPECT_CONTENT, 0, NULL, NULL, surf.dc, NULL, &bounds, NULL,
                                                 &update_text, NULL, 0, 0 );
            trace( "draw page %d hr %08lx origin %ld,%ld update %ld,%ld-%ld,%ld bounds %ld x %ld len %ld caret %ld,%ld\n",
                   i, hr, o.x, o.y - top, update_text.left, update_text.top, update_text.right, update_text.bottom,
                   bounds.right, bounds.bottom, (long)ed( WM_GETTEXTLENGTH, 0, 0 ), caret_pos.x, caret_pos.y );
        }
        RestoreDC( surf.dc, -1 );
    }
    GdiFlush();
    BitBlt( hdc, update->left, update->top, update->right - update->left, update->bottom - update->top,
            surf.dc, update->left, update->top, SRCCOPY );
}

static void scroll_bar( int bar, WPARAM wparam )
{
    SCROLLINFO info = { sizeof(info), SIF_ALL };
    RECT client;
    int pos, line = px( 40 );

    GetClientRect( view, &client );
    GetScrollInfo( view, bar, &info );
    pos = info.nPos;
    switch (LOWORD( wparam ))
    {
    case SB_LINEUP: pos -= line; break;
    case SB_LINEDOWN: pos += line; break;
    case SB_PAGEUP: pos -= info.nPage; break;
    case SB_PAGEDOWN: pos += info.nPage; break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION: pos = info.nTrackPos; break;
    case SB_TOP: pos = 0; break;
    case SB_BOTTOM: pos = info.nMax; break;
    default: return;
    }
    if (bar == SB_VERT) scroll_to( scroll_x, pos );
    else scroll_to( pos, scroll_y );
}

/* Page Up and Down: the window moves a screen, the caret goes along */
static void page_key( BOOL down, BOOL shift )
{
    RECT client;
    POINT pt;
    LONG cp;
    CHARRANGE sel;

    GetClientRect( view, &client );
    to_window( caret_pos.x, caret_pos.y, &pt );
    scroll_to( scroll_x, scroll_y + (down ? 1 : -1) * (client.bottom - px( 40 )) );
    cp = char_at( pt.x, pt.y + caret_height / 2 );
    ed( EM_EXGETSEL, 0, (LPARAM)&sel );
    if (shift)
    {
        LONG anchor = sel.cpMin == sel.cpMax ? sel.cpMin : (anchor_start >= 0 ? anchor_start : sel.cpMin);
        select_range( anchor, cp );
    }
    else select_range( cp, cp );
}

static LRESULT CALLBACK view_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint( hwnd, &ps );

        if (services) paint( hdc, &ps.rcPaint );
        EndPaint( hwnd, &ps );
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_SIZE:
        if (services)
        {
            if (web_layout) set_format_width();
            view_repaginate();
        }
        return 0;

    case WM_VSCROLL:
        scroll_bar( SB_VERT, wparam );
        return 0;
    case WM_HSCROLL:
        scroll_bar( SB_HORZ, wparam );
        return 0;

    case WM_MOUSEWHEEL:
        if (GET_KEYSTATE_WPARAM( wparam ) & MK_CONTROL)
        {
            int z = view_zoom();
            view_set_zoom( GET_WHEEL_DELTA_WPARAM( wparam ) > 0 ? z + 10 : z - 10 );
        }
        else scroll_to( scroll_x, scroll_y - GET_WHEEL_DELTA_WPARAM( wparam ) * px( 40 ) * 3 / WHEEL_DELTA );
        return 0;
    case WM_MOUSEHWHEEL:
        scroll_to( scroll_x + GET_WHEEL_DELTA_WPARAM( wparam ) * px( 40 ) / WHEEL_DELTA, scroll_y );
        return 0;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        mouse_down( GET_X( lparam ), GET_Y( lparam ), wparam );
        return 0;
    case WM_MOUSEMOVE:
        if (selecting && GetCapture() == hwnd)
        {
            RECT client;
            int y = GET_Y( lparam );

            mouse_drag( GET_X( lparam ), y );
            GetClientRect( hwnd, &client );
            if (y < 0 || y >= client.bottom) SetTimer( hwnd, TIMER_DRAGSCROLL, 30, NULL );
            else KillTimer( hwnd, TIMER_DRAGSCROLL );
        }
        return 0;
    case WM_LBUTTONUP:
        if (selecting)
        {
            selecting = FALSE;
            KillTimer( hwnd, TIMER_DRAGSCROLL );
            ReleaseCapture();
            format_painter_mouse_up();
        }
        return 0;
    case WM_CAPTURECHANGED:
        selecting = FALSE;
        KillTimer( hwnd, TIMER_DRAGSCROLL );
        return 0;
    case WM_RBUTTONDOWN:
    {
        CHARRANGE sel;
        LONG cp = char_at( GET_X( lparam ), GET_Y( lparam ) );

        SetFocus( hwnd );
        ed( EM_EXGETSEL, 0, (LPARAM)&sel );
        if (cp < sel.cpMin || cp > sel.cpMax || sel.cpMin == sel.cpMax) select_range( cp, cp );
        return 0;
    }
    case WM_RBUTTONUP:
        context_menu( GET_X( lparam ), GET_Y( lparam ) );
        return 0;
    case WM_CONTEXTMENU:
        if (lparam == -1)
        {
            POINT pt;
            to_window( caret_pos.x, caret_pos.y + caret_height, &pt );
            context_menu( pt.x, pt.y );
        }
        return 0;

    case WM_SETCURSOR:
        if (LOWORD( lparam ) == HTCLIENT)
        {
            POINT pt;
            RECT r;
            BOOL on_page = FALSE;

            GetCursorPos( &pt );
            ScreenToClient( hwnd, &pt );
            for (int i = 0; i < max( band_count, 1 ) && !on_page; i++)
            {
                page_rect( i, &r );
                on_page = PtInRect( &r, pt );
            }
            SetCursor( LoadCursorW( NULL, on_page ? (LPCWSTR)IDC_IBEAM : (LPCWSTR)IDC_ARROW ) );
            return TRUE;
        }
        break;

    case WM_TIMER:
        if (wparam == TIMER_PAGINATE)
        {
            view_repaginate();
            return 0;
        }
        if (wparam == TIMER_STATUS)
        {
            KillTimer( hwnd, TIMER_STATUS );
            status_update();
            return 0;
        }
        if (wparam == TIMER_DRAGSCROLL)
        {
            POINT pt;
            RECT client;

            GetCursorPos( &pt );
            ScreenToClient( hwnd, &pt );
            GetClientRect( hwnd, &client );
            if (pt.y < 0) scroll_to( scroll_x, scroll_y + pt.y );
            else if (pt.y >= client.bottom) scroll_to( scroll_x, scroll_y + pt.y - client.bottom );
            mouse_drag( pt.x, pt.y );
            return 0;
        }
        ed( WM_TIMER, wparam, lparam );
        return 0;

    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        ed( msg, wparam, lparam );
        if (msg == WM_SETFOCUS) place_caret();
        return 0;

    case WM_KEYDOWN:
        if (wparam == VK_PRIOR || wparam == VK_NEXT)
        {
            page_key( wparam == VK_NEXT, GetKeyState( VK_SHIFT ) < 0 );
            view_scroll_caret();
            return 0;
        }
        if (wparam == VK_ESCAPE) return 0;
        anchor_start = -1;
        ed( msg, wparam, lparam );
        view_scroll_caret();
        return 0;
    case WM_CHAR:
        if (wparam == 0x1b) return 0;
        ed( msg, wparam, lparam );
        view_scroll_caret();
        return 0;
    case WM_KEYUP:
    case WM_IME_STARTCOMPOSITION:
    case WM_IME_COMPOSITION:
    case WM_IME_ENDCOMPOSITION:
    case WM_IME_CHAR:
    case WM_IME_REQUEST:
    case WM_IME_SETCONTEXT:
    case WM_IME_NOTIFY:
    {
        LRESULT result = 0;
        if (services && services->vtbl->TxSendMessage( services, msg, wparam, lparam, &result ) == S_FALSE)
            return DefWindowProcW( hwnd, msg, wparam, lparam );
        view_scroll_caret();
        return result;
    }
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTTAB | DLGC_WANTARROWS;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

HWND view_create( HWND parent )
{
    WNDCLASSW cls = { CS_DBLCLKS, view_proc, 0, 0, instance, NULL, NULL, NULL, NULL, L"ArcticWordPadView" };
    HDC hdc;

    RegisterClassW( &cls );
    view = CreateWindowExW( 0, cls.lpszClassName, NULL, WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | WS_CLIPSIBLINGS,
                            0, 0, 100, 100, parent, NULL, instance, NULL );
    hdc = GetDC( view );
    ReleaseDC( view, hdc );
    if (!create_services())
    {
        MessageBoxW( parent, L"riched20.dll", string( IDS_APP_NAME ), MB_ICONERROR );
        return NULL;
    }
    anchor_start = -1;
    return view;
}

/**********************************************************************
 *          Words
 */

int word_count(void)
{
    LONG length = ed( WM_GETTEXTLENGTH, 0, 0 ), count = 0;
    BOOL in_word = FALSE;
    WCHAR *text;
    GETTEXTEX get = { 0 };

    if (!length || !(text = malloc( (length + 1) * sizeof(WCHAR) ))) return 0;
    get.cb = (length + 1) * sizeof(WCHAR);
    get.flags = GT_DEFAULT;
    get.codepage = 1200;
    length = ed( EM_GETTEXTEX, (WPARAM)&get, (LPARAM)text );
    for (LONG i = 0; i < length; i++)
    {
        WCHAR c = text[i];
        BOOL space = c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == 0xa0 || c == 0xb || c == 0xc ||
                     c == 0xfffc || (c >= 0x2000 && c <= 0x200b);

        if (!space && !in_word) count++;
        in_word = !space;
    }
    free( text );
    return count;
}

/**********************************************************************
 *          The status bar: the page, the words, the views and the zoom
 */

enum { SZ_NONE, SZ_PAGE, SZ_WORDS, SZ_PRINT, SZ_WEB, SZ_OUT, SZ_SLIDER, SZ_IN, SZ_PERCENT };
static RECT status_zones[SZ_PERCENT + 1];
static int status_hot, status_words = -1;
static BOOL status_dragging;

static int slider_to_zoom( int x )
{
    RECT *r = &status_zones[SZ_SLIDER];
    int mid = (r->left + r->right) / 2, half = (r->right - r->left) / 2;

    /* the middle is 100 %, the left end 10 %, the right end 500 % */
    if (x <= mid) return 10 + MulDiv( max( x - r->left, 0 ), 90, half );
    return 100 + MulDiv( min( x - mid, half ), 400, half );
}

static int zoom_to_slider( int z )
{
    RECT *r = &status_zones[SZ_SLIDER];
    int mid = (r->left + r->right) / 2, half = (r->right - r->left) / 2;

    if (z <= 100) return r->left + MulDiv( z - 10, half, 90 );
    return mid + MulDiv( z - 100, half, 400 );
}

void status_update(void)
{
    if (!status_bar) return;
    status_words = word_count();
    InvalidateRect( status_bar, NULL, FALSE );
}

static void status_paint( HWND hwnd, HDC hdc )
{
    RECT client, r;
    WCHAR text[96];
    HFONT font = get_font( F_SMALL );
    int x;

    GetClientRect( hwnd, &client );
    if (!surface_size( &status_surf, client.right, client.bottom )) return;
    fill( &status_surf, &client, th.chrome );

    /* the page and the words */
    x = px( 16 );
    if (!web_layout)
    {
        swprintf( text, ARRAY_SIZE(text), string( IDS_PAGE_OF ), view_current_page(), view_page_count() );
        SetRect( &r, x, 0, x + text_width( font, text, -1 ) + px( 8 ), client.bottom );
        status_zones[SZ_PAGE] = r;
        draw_text( &status_surf, text, -1, &r, font, th.dim, DT_LEFT );
        x = r.right + px( 16 );
    }
    if (status_words < 0) status_words = word_count();
    swprintf( text, ARRAY_SIZE(text), string( status_words == 1 ? IDS_WORD_ONE : IDS_WORDS ), status_words );
    SetRect( &r, x, 0, x + text_width( font, text, -1 ) + px( 8 ), client.bottom );
    status_zones[SZ_WORDS] = r;
    draw_text( &status_surf, text, -1, &r, font, th.dim, DT_LEFT );

    /* from the right: the percentage, +, the slider, -, the views */
    x = client.right - px( 16 );
    swprintf( text, ARRAY_SIZE(text), L"%d%%", zoom );
    SetRect( &r, x - px( 40 ), 0, x, client.bottom );
    status_zones[SZ_PERCENT] = r;
    draw_text( &status_surf, text, -1, &r, font, th.dim, DT_RIGHT );
    x = r.left - px( 4 );
    SetRect( &r, x - px( 20 ), (client.bottom - px( 20 )) / 2, x, (client.bottom + px( 20 )) / 2 );
    status_zones[SZ_IN] = r;
    if (status_hot == SZ_IN) fill_round( &status_surf, &r, (float)px( 4 ), th.hover, 255 );
    draw_glyph( &status_surf, G_PLUS, r.left + px( 4 ), r.top + px( 4 ), px( 12 ), th.dim );
    x = r.left - px( 4 );
    SetRect( &r, x - px( 100 ), 0, x, client.bottom );
    status_zones[SZ_SLIDER] = r;
    {
        RECT track = { r.left, client.bottom / 2, r.right, client.bottom / 2 + 1 };
        RECT mid = { (r.left + r.right) / 2, client.bottom / 2 - px( 4 ), (r.left + r.right) / 2 + 1, client.bottom / 2 + px( 4 ) };
        int tx = zoom_to_slider( zoom );
        RECT thumb = { tx - px( 4 ), client.bottom / 2 - px( 8 ), tx + px( 4 ), client.bottom / 2 + px( 8 ) };

        fill( &status_surf, &track, th.dim );
        fill( &status_surf, &mid, th.dim );
        fill_round( &status_surf, &thumb, (float)px( 2 ), status_hot == SZ_SLIDER ? th.text : th.dim, 255 );
    }
    x = r.left - px( 4 );
    SetRect( &r, x - px( 20 ), (client.bottom - px( 20 )) / 2, x, (client.bottom + px( 20 )) / 2 );
    status_zones[SZ_OUT] = r;
    if (status_hot == SZ_OUT) fill_round( &status_surf, &r, (float)px( 4 ), th.hover, 255 );
    draw_glyph( &status_surf, G_MINUS, r.left + px( 4 ), r.top + px( 4 ), px( 12 ), th.dim );

    x = r.left - px( 16 );
    for (int z = SZ_WEB; z >= SZ_PRINT; z--)
    {
        BOOL on = (z == SZ_WEB) == !!web_layout;

        SetRect( &r, x - px( 26 ), (client.bottom - px( 22 )) / 2, x, (client.bottom + px( 22 )) / 2 );
        status_zones[z] = r;
        if (on) fill_round( &status_surf, &r, (float)px( 4 ), th.checked, 255 );
        else if (status_hot == z) fill_round( &status_surf, &r, (float)px( 4 ), th.hover, 255 );
        draw_icon( &status_surf, z == SZ_WEB ? "weblayout" : "printlayout", r.left + px( 5 ), r.top + px( 3 ), px( 16 ), IS_NORMAL );
        x = r.left - px( 2 );
    }
    GdiFlush();
    BitBlt( hdc, 0, 0, client.right, client.bottom, status_surf.dc, 0, 0, SRCCOPY );
}

static int status_hit( POINT pt )
{
    for (int z = SZ_PAGE; z <= SZ_PERCENT; z++)
        if (PtInRect( &status_zones[z], pt )) return z;
    return SZ_NONE;
}

static LRESULT CALLBACK status_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    POINT pt = { GET_X( lparam ), GET_Y( lparam ) };

    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint( hwnd, &ps );
        status_paint( hwnd, hdc );
        EndPaint( hwnd, &ps );
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE:
    {
        int hot = status_hit( pt );
        TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, hwnd };

        if (status_dragging)
        {
            view_set_zoom( slider_to_zoom( pt.x ) );
            return 0;
        }
        if (hot != status_hot)
        {
            status_hot = hot;
            InvalidateRect( hwnd, NULL, FALSE );
        }
        TrackMouseEvent( &track );
        return 0;
    }
    case WM_MOUSELEAVE:
        status_hot = SZ_NONE;
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    case WM_LBUTTONDOWN:
        switch (status_hit( pt ))
        {
        case SZ_OUT: view_set_zoom( (zoom - 1) / 10 * 10 ); break;
        case SZ_IN: view_set_zoom( zoom / 10 * 10 + 10 ); break;
        case SZ_SLIDER:
            status_dragging = TRUE;
            SetCapture( hwnd );
            view_set_zoom( slider_to_zoom( pt.x ) );
            break;
        case SZ_PRINT: command( IDM_VIEW_PRINT ); break;
        case SZ_WEB: command( IDM_VIEW_WEB ); break;
        case SZ_PERCENT: command( IDM_ZOOM ); break;
        case SZ_WORDS: command( IDM_WORDCOUNT ); break;
        }
        return 0;
    case WM_LBUTTONUP:
        if (status_dragging)
        {
            status_dragging = FALSE;
            ReleaseCapture();
        }
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

HWND status_create( HWND parent )
{
    WNDCLASSW cls = { 0, status_proc, 0, 0, instance, NULL, NULL, NULL, NULL, L"ArcticWordPadStatus" };

    cls.hCursor = LoadCursorW( NULL, (LPCWSTR)IDC_ARROW );
    RegisterClassW( &cls );
    status_bar = CreateWindowExW( 0, cls.lpszClassName, NULL, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 100, 24,
                                  parent, NULL, instance, NULL );
    return status_bar;
}

/**********************************************************************
 *          Printing: the text again, laid out for the printer
 */

/* the pages first to last (from 1; 0: to the end) on a printer's DC, each
 * between StartPage and EndPage; returns how many pages there were */
int view_print( HDC dc, int first, int last, BOOL (*abort_proc)(void) )
{
    struct text_host print_host = { &host_vtbl, dc };
    struct band_list list = { 0 };
    text_services *ts = NULL;
    IUnknown *unk = NULL;
    HRESULT (WINAPI *create)( IUnknown *, struct text_host *, IUnknown ** );
    int dpi_x = GetDeviceCaps( dc, LOGPIXELSX ), dpi_y = GetDeviceCaps( dc, LOGPIXELSY );
    int off_x = GetDeviceCaps( dc, PHYSICALOFFSETX ), off_y = GetDeviceCaps( dc, PHYSICALOFFSETY );
    LONG height = MulDiv( page.height - page.top - page.bottom, dpi_y, 1440 );
    REQRESIZE dummy;
    char *rtf;
    size_t len;
    int printed = 0;

    (void)dummy;
    create = (void *)GetProcAddress( GetModuleHandleW( L"riched20.dll" ), "CreateTextServices" );
    if (!create || FAILED(create( NULL, &print_host, &unk ))) return 0;
    if (FAILED(IUnknown_QueryInterface( unk, &iid_text_services, (void **)&ts )))
    {
        IUnknown_Release( unk );
        return 0;
    }
    ts->vtbl->Release( ts );
    SetRect( &print_host.format, 0, 0, MulDiv( page.width - page.left - page.right, dpi_x, 1440 ), TEXT_TALL );
    ts->vtbl->OnTxInPlaceActivate( ts, &print_host.format );
    tsend( ts, EM_EXLIMITTEXT, 0, 0x7ffffffe );
    if ((rtf = stream_out( &len, SF_RTF )))
    {
        struct stream_data s = { rtf, len, 0 };
        EDITSTREAM es = { (DWORD_PTR)&s, 0, read_cb };

        tsend( ts, EM_STREAMIN, SF_RTF, (LPARAM)&es );
        free( rtf );
    }
    paginate( ts, height, 0, &list );
    if (!last || last > list.count) last = list.count;
    for (int i = max( first, 1 ) - 1; i < last; i++)
    {
        RECTL bounds = { 0, 0, print_host.format.right, TEXT_TALL };
        LONG top = list.bands[i].top;
        LONG bottom = i < list.count - 1 ? list.bands[i].bottom : top + height;
        RECT update = { -MulDiv( page.left, dpi_x, 1440 ), top, print_host.format.right + MulDiv( page.right, dpi_x, 1440 ), bottom };
        int x = MulDiv( page.left, dpi_x, 1440 ) - off_x, y = MulDiv( page.top, dpi_y, 1440 ) - off_y;

        if (abort_proc && abort_proc()) break;
        if (StartPage( dc ) <= 0) break;
        SaveDC( dc );
        IntersectClipRect( dc, x - MulDiv( page.left, dpi_x, 1440 ), y, x + print_host.format.right + MulDiv( page.right, dpi_x, 1440 ),
                           y + (bottom - top) );
        SetViewportOrgEx( dc, x, y - top, NULL );
        ts->vtbl->TxDraw( ts, DVASPECT_CONTENT, 0, NULL, NULL, dc, NULL, &bounds, NULL, &update, NULL, 0, 0 );
        RestoreDC( dc, -1 );
        if (EndPage( dc ) <= 0) break;
        printed++;
    }
    free( list.bands );
    IUnknown_Release( unk );
    return printed;
}
