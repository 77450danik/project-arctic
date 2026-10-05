/*
 * Snipping Tool: screen snipping (Win+Shift+S)
 *
 * As in Windows 11: the screen stands still and dims, a bar at the top of
 * the monitor under the pointer offers rectangle, window, full screen and
 * freeform snips, and what is chosen lights up. Esc changes the mind. The
 * mode chosen is kept for the next time, full screen apart, which snips at
 * once.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <math.h>

#include "snippingtool.h"
#include "shellscalingapi.h"
#include "dwmapi.h"
#include "wine/arctic_dwm.h"
enum mode { MODE_RECTANGLE, MODE_WINDOW, MODE_FULL_SCREEN, MODE_FREEFORM, BUTTON_CLOSE, BUTTON_COUNT };

#define MAX_WINDOWS 256

struct overlay
{
    HWND           hwnd;
    RECT           screen;        /* the virtual screen */
    RECT           client;
    struct image  *shot, *dim;
    struct image   back;          /* the window's pixels, a DIB section */
    HDC            back_dc;
    HBITMAP        back_bitmap;
    HFONT          font;
    struct theme   theme;
    float          scale;

    enum mode      mode;
    BOOL           dragging;
    POINT          start;
    RECT           selection;     /* the rectangle being drawn */
    RECT           windows[MAX_WINDOWS];
    int            window_count;
    RECT           hover;         /* the window under the pointer */
    struct pointf *points;        /* the freeform outline */
    int            count, capacity;

    RECT           bar, buttons[BUTTON_COUNT], separator, hint;
    int            hot;

    struct image  *result;
    BOOL           done;
};

static const UINT button_names[BUTTON_COUNT] =
{
    IDS_MODE_RECTANGLE, IDS_MODE_WINDOW, IDS_MODE_FULL_SCREEN, IDS_MODE_FREEFORM, IDS_CLOSE
};

static const enum glyph button_glyphs[BUTTON_COUNT] =
{
    GLYPH_RECTANGLE, GLYPH_WINDOW, GLYPH_FULL_SCREEN, GLYPH_FREEFORM, GLYPH_CLOSE
};

static int px( const struct overlay *o, float value )
{
    return (int)floorf( value * o->scale + 0.5f );
}

/* the bar, centred at the top of the monitor under the pointer */
static void layout_bar( struct overlay *o )
{
    MONITORINFO info = { .cbSize = sizeof(info) };
    UINT dpi_x = 96, dpi_y = 96;
    HMONITOR monitor;
    POINT pt;
    int pad, width, height, x, y, size;

    GetCursorPos( &pt );
    monitor = MonitorFromPoint( pt, MONITOR_DEFAULTTOPRIMARY );
    GetMonitorInfoW( monitor, &info );
    GetDpiForMonitor( monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y );
    o->scale = dpi_x / 96.0f;
    OffsetRect( &info.rcMonitor, -o->screen.left, -o->screen.top );

    pad = px( o, 4 );
    size = px( o, 40 );
    height = size + 2 * pad;
    width = 2 * pad + 5 * size + px( o, 13 );
    x = (info.rcMonitor.left + info.rcMonitor.right - width) / 2;
    y = info.rcMonitor.top + px( o, 12 );
    SetRect( &o->bar, x, y, x + width, y + height );
    x += pad;
    for (int i = 0; i < BUTTON_CLOSE; i++, x += size) SetRect( &o->buttons[i], x, y + pad, x + size, y + pad + size );
    SetRect( &o->separator, x + px( o, 6 ), y + pad + px( o, 10 ), x + px( o, 6 ) + max( 1, px( o, 1 ) ),
             y + pad + size - px( o, 10 ) );
    x += px( o, 13 );
    SetRect( &o->buttons[BUTTON_CLOSE], x, y + pad, x + size, y + pad + size );
}

static int hit_button( const struct overlay *o, POINT pt )
{
    for (int i = 0; i < BUTTON_COUNT; i++) if (PtInRect( &o->buttons[i], pt )) return i;
    return -1;
}

/* the name of the button under the pointer, under the bar */
static void layout_hint( struct overlay *o )
{
    const WCHAR *text;
    SIZE size;
    int x;

    SetRectEmpty( &o->hint );
    if (o->hot < 0) return;
    text = string( button_names[o->hot] );
    GetTextExtentPoint32W( o->back_dc, text, wcslen( text ), &size );
    x = (o->buttons[o->hot].left + o->buttons[o->hot].right - size.cx) / 2 - px( o, 8 );
    SetRect( &o->hint, x, o->bar.bottom + px( o, 6 ), x + size.cx + px( o, 16 ),
             o->bar.bottom + px( o, 6 ) + size.cy + px( o, 10 ) );
}

static void frame_rect( struct image *dst, const RECT *rect, UINT32 color, const RECT *clip )
{
    RECT sides[4], r;

    SetRect( &sides[0], rect->left - 1, rect->top - 1, rect->right + 1, rect->top );
    SetRect( &sides[1], rect->left - 1, rect->bottom, rect->right + 1, rect->bottom + 1 );
    SetRect( &sides[2], rect->left - 1, rect->top, rect->left, rect->bottom );
    SetRect( &sides[3], rect->right, rect->top, rect->right + 1, rect->bottom );
    for (int i = 0; i < 4; i++) if (IntersectRect( &r, &sides[i], clip )) image_blend_rect( dst, &r, color );
}

static void draw_bar( struct overlay *o, const RECT *clip )
{
    const struct theme *t = &o->theme;
    float radius = 8 * o->scale;
    RECT r, inner;

    if (IntersectRect( &r, &o->bar, clip ))
    {
        ink_round_rect( &o->back, &o->bar, radius, t->border, clip );
        inner = o->bar;
        InflateRect( &inner, -1, -1 );
        ink_round_rect( &o->back, &inner, radius - 1, t->surface, clip );
        for (int i = 0; i < BUTTON_COUNT; i++)
        {
            BOOL chosen = i == (int)o->mode && i != MODE_FULL_SCREEN && i != BUTTON_CLOSE;
            RECT button = o->buttons[i];

            InflateRect( &button, -px( o, 2 ), -px( o, 2 ) );
            if (chosen || i == o->hot)
                ink_round_rect( &o->back, &button, 4 * o->scale, chosen ? t->pressed : t->hover, clip );
            ink_glyph( &o->back, button_glyphs[i], &o->buttons[i], px( o, 20 ), t->text, clip );
            if (chosen)  /* the accent under the chosen mode */
            {
                RECT mark = { (button.left + button.right) / 2 - px( o, 8 ), button.bottom - px( o, 3 ),
                              (button.left + button.right) / 2 + px( o, 8 ), button.bottom - px( o, 1 ) };
                ink_round_rect( &o->back, &mark, 1.5f * o->scale, t->accent, clip );
            }
        }
        image_blend_rect( &o->back, &o->separator, t->border );
    }

    if (!IsRectEmpty( &o->hint ) && IntersectRect( &r, &o->hint, clip ))
    {
        const WCHAR *text = string( button_names[o->hot] );

        ink_round_rect( &o->back, &o->hint, 4 * o->scale, t->border, clip );
        inner = o->hint;
        InflateRect( &inner, -1, -1 );
        ink_round_rect( &o->back, &inner, 4 * o->scale - 1, t->surface, clip );
        SetTextColor( o->back_dc, to_colorref( t->text ) );
        SetBkMode( o->back_dc, TRANSPARENT );
        DrawTextW( o->back_dc, text, -1, &o->hint, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX );
    }
}

static BOOL highlight( const struct overlay *o, RECT *rect )
{
    if (o->mode == MODE_RECTANGLE && o->dragging) *rect = o->selection;
    else if (o->mode == MODE_WINDOW) *rect = o->hover;
    else return FALSE;
    return !IsRectEmpty( rect );
}

/* area of the window drawn again into its pixels */
static void render( struct overlay *o, const RECT *area )
{
    RECT r, lit, in;

    if (!IntersectRect( &r, area, &o->client )) return;
    GdiFlush();
    image_copy_rect( &o->back, o->dim, &r );
    if (highlight( o, &lit ))
    {
        if (IntersectRect( &in, &lit, &r )) image_copy_rect( &o->back, o->shot, &in );
        frame_rect( &o->back, &lit, 0xd0ffffff, &r );
    }
    if (o->mode == MODE_FREEFORM && o->dragging && o->count)
        ink_polyline( &o->back, o->points, o->count, FALSE, max( 0.75f, o->scale ), 0xffffffff, &r );
    draw_bar( o, &r );
    InvalidateRect( o->hwnd, &r, FALSE );
}

/* what changes when a lit rectangle goes from old to new: the ring between
 * the two, not what stays lit inside both */
static void render_change( struct overlay *o, const RECT *old, const RECT *new )
{
    RECT all, same, part;

    UnionRect( &all, old, new );
    InflateRect( &all, 2, 2 );
    if (IsRectEmpty( old ) || IsRectEmpty( new ) || !IntersectRect( &same, old, new ) ||
        (InflateRect( &same, -2, -2 ), IsRectEmpty( &same )))
    {
        render( o, &all );
        return;
    }
    SetRect( &part, all.left, all.top, all.right, same.top );          render( o, &part );
    SetRect( &part, all.left, same.bottom, all.right, all.bottom );    render( o, &part );
    SetRect( &part, all.left, same.top, same.left, same.bottom );      render( o, &part );
    SetRect( &part, same.right, same.top, all.right, same.bottom );    render( o, &part );
}

static void set_hot( struct overlay *o, int hot )
{
    RECT area;

    if (o->dragging) hot = -1;
    if (hot == o->hot) return;
    UnionRect( &area, &o->bar, &o->hint );
    o->hot = hot;
    layout_hint( o );
    UnionRect( &area, &area, &o->hint );
    render( o, &area );
}

static void finish( struct overlay *o, struct image *result )
{
    o->result = result;
    o->done = TRUE;
}

static void snip_rect( struct overlay *o, const RECT *rect )
{
    RECT r;

    if (IntersectRect( &r, rect, &o->client )) finish( o, image_crop( o->shot, &r ) );
}

static int compare_floats( const void *a, const void *b )
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

/* what the outline encloses, the rest transparent */
static void snip_freeform( struct overlay *o )
{
    struct image *image;
    float *xs;
    RECT box;

    SetRect( &box, o->points[0].x, o->points[0].y, o->points[0].x + 1, o->points[0].y + 1 );
    for (int i = 1; i < o->count; i++)
    {
        RECT p = { (int)o->points[i].x, (int)o->points[i].y, (int)o->points[i].x + 1, (int)o->points[i].y + 1 };
        UnionRect( &box, &box, &p );
    }
    if (!IntersectRect( &box, &box, &o->client ) || box.right - box.left < 2 || box.bottom - box.top < 2) return;
    if (!(image = image_create( box.right - box.left, box.bottom - box.top ))) return;
    if (!(xs = malloc( o->count * sizeof(*xs) )))
    {
        image_release( image );
        return;
    }
    for (int y = box.top; y < box.bottom; y++)
    {
        float fy = y + 0.5f;
        int n = 0;

        for (int i = 0; i < o->count; i++)  /* where row y crosses the outline, closed */
        {
            const struct pointf *a = &o->points[i], *b = &o->points[(i + 1) % o->count];
            if ((a->y <= fy) != (b->y <= fy)) xs[n++] = a->x + (fy - a->y) * (b->x - a->x) / (b->y - a->y);
        }
        qsort( xs, n, sizeof(*xs), compare_floats );
        for (int k = 0; k + 1 < n; k += 2)
        {
            int from = max( box.left, (int)ceilf( xs[k] - 0.5f ) ), to = min( box.right, (int)ceilf( xs[k + 1] - 0.5f ) );
            if (from < to)
                memcpy( image->bits + (SIZE_T)(y - box.top) * image->width + (from - box.left),
                        o->shot->bits + (SIZE_T)y * o->shot->width + from, (to - from) * sizeof(UINT32) );
        }
    }
    free( xs );
    finish( o, image );
}

static void add_point( struct overlay *o, POINT pt )
{
    if (o->count && fabsf( o->points[o->count - 1].x - pt.x ) < 1 && fabsf( o->points[o->count - 1].y - pt.y ) < 1)
        return;
    if (o->count == o->capacity)
    {
        int capacity = max( 256, o->capacity * 2 );
        struct pointf *points = realloc( o->points, capacity * sizeof(*points) );
        if (!points) return;
        o->points = points;
        o->capacity = capacity;
    }
    o->points[o->count].x = pt.x + 0.5f;
    o->points[o->count].y = pt.y + 0.5f;
    o->count++;
}

static void update_hover( struct overlay *o, POINT pt )
{
    RECT old = o->hover;

    SetRectEmpty( &o->hover );
    if (!PtInRect( &o->bar, pt ))
        for (int i = 0; i < o->window_count; i++)
            if (PtInRect( &o->windows[i], pt ))
            {
                IntersectRect( &o->hover, &o->windows[i], &o->client );
                break;
            }
    if (!EqualRect( &old, &o->hover )) render_change( o, &old, &o->hover );
}

static void choose( struct overlay *o, int button )
{
    switch (button)
    {
    case BUTTON_CLOSE:
        finish( o, NULL );
        break;
    case MODE_FULL_SCREEN:
        finish( o, image_crop( o->shot, &o->client ) );
        break;
    default:
    {
        POINT pt;
        o->mode = button;
        set_setting( L"SnipMode", button );
        SetRectEmpty( &o->hover );
        if (o->mode == MODE_WINDOW && GetCursorPos( &pt ))
        {
            ScreenToClient( o->hwnd, &pt );
            update_hover( o, pt );
        }
        render( o, &o->client );
        break;
    }
    }
}

static LRESULT CALLBACK overlay_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct overlay *o = (struct overlay *)GetWindowLongPtrW( hwnd, GWLP_USERDATA );
    POINT pt = { GET_X_LPARAM( lparam ), GET_Y_LPARAM( lparam ) };

    switch (msg)
    {
    case WM_NCCREATE:
        SetWindowLongPtrW( hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lparam)->lpCreateParams );
        break;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint( hwnd, &ps );
        GdiFlush();
        BitBlt( hdc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
                ps.rcPaint.bottom - ps.rcPaint.top, o->back_dc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY );
        EndPaint( hwnd, &ps );
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_DPICHANGED:  /* the window is the whole screen whatever the DPI */
        return 0;

    case WM_SETCURSOR:
        GetCursorPos( &pt );
        ScreenToClient( hwnd, &pt );
        SetCursor( LoadCursorW( NULL, PtInRect( &o->bar, pt ) && !o->dragging ? (const WCHAR *)IDC_ARROW : (const WCHAR *)IDC_CROSS ) );
        return TRUE;

    case WM_MOUSEMOVE:
        set_hot( o, hit_button( o, pt ) );
        if (o->dragging && o->mode == MODE_RECTANGLE)
        {
            RECT old = o->selection;
            SetRect( &o->selection, min( o->start.x, pt.x ), min( o->start.y, pt.y ), max( o->start.x, pt.x ) + 1,
                     max( o->start.y, pt.y ) + 1 );
            IntersectRect( &o->selection, &o->selection, &o->client );
            render_change( o, &old, &o->selection );
        }
        else if (o->dragging && o->mode == MODE_FREEFORM)
        {
            int from = o->count;
            RECT area;
            add_point( o, pt );
            if (o->count == from) return 0;
            from = max( 0, from - 1 );
            SetRect( &area, min( o->points[from].x, pt.x ), min( o->points[from].y, pt.y ),
                     max( o->points[from].x, pt.x ) + 1, max( o->points[from].y, pt.y ) + 1 );
            InflateRect( &area, px( o, 3 ) + 2, px( o, 3 ) + 2 );
            render( o, &area );
        }
        else if (!o->dragging && o->mode == MODE_WINDOW) update_hover( o, pt );
        return 0;

    case WM_LBUTTONDOWN:
    {
        int button = hit_button( o, pt );
        if (button >= 0)
        {
            choose( o, button );
            return 0;
        }
        if (PtInRect( &o->bar, pt )) return 0;
        if (o->mode == MODE_WINDOW)
        {
            if (!IsRectEmpty( &o->hover )) snip_rect( o, &o->hover );
            return 0;
        }
        o->dragging = TRUE;
        o->start = pt;
        o->count = 0;
        SetRectEmpty( &o->selection );
        set_hot( o, -1 );
        if (o->mode == MODE_FREEFORM) add_point( o, pt );
        SetCapture( hwnd );
        return 0;
    }

    case WM_LBUTTONUP:
        if (!o->dragging) return 0;
        o->dragging = FALSE;
        ReleaseCapture();
        if (o->mode == MODE_RECTANGLE && o->selection.right - o->selection.left > 2 &&
            o->selection.bottom - o->selection.top > 2)
            snip_rect( o, &o->selection );
        else if (o->mode == MODE_FREEFORM && o->count >= 3) snip_freeform( o );
        if (!o->done)  /* a click, not a snip: back to the start */
        {
            o->count = 0;
            SetRectEmpty( &o->selection );
            render( o, &o->client );
        }
        return 0;

    case WM_RBUTTONUP:
        finish( o, NULL );
        return 0;

    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) finish( o, NULL );
        return 0;

    case WM_CAPTURECHANGED:
        if (o && o->dragging && (HWND)lparam != hwnd)
        {
            o->dragging = FALSE;
            o->count = 0;
            SetRectEmpty( &o->selection );
            render( o, &o->client );
        }
        return 0;

    case WM_CLOSE:
        finish( o, NULL );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

static BOOL create_back_buffer( struct overlay *o )
{
    BITMAPINFO info = { .bmiHeader = { .biSize = sizeof(info.bmiHeader), .biWidth = o->shot->width,
                                       .biHeight = -o->shot->height, .biPlanes = 1, .biBitCount = 32 } };
    void *bits;

    if (!(o->back_dc = CreateCompatibleDC( NULL ))) return FALSE;
    if (!(o->back_bitmap = CreateDIBSection( o->back_dc, &info, DIB_RGB_COLORS, &bits, NULL, 0 ))) return FALSE;
    SelectObject( o->back_dc, o->back_bitmap );
    o->back.ref = 1;
    o->back.width = o->shot->width;
    o->back.height = o->shot->height;
    o->back.bits = bits;
    o->back.owned = FALSE;
    return TRUE;
}

struct image *snip_screen(void)
{
    static const WCHAR class_name[] = L"ArcticScreenSnipping";
    WNDCLASSW class = { .lpfnWndProc = overlay_proc, .hInstance = instance, .lpszClassName = class_name };
    struct overlay o = { .hot = -1 };
    DWORD transition = ARCTIC_TRANSITION_NONE;
    BOOL quit = FALSE;
    MSG msg;

    if (!(o.shot = capture_screen( &o.screen ))) return NULL;
    SetRect( &o.client, 0, 0, o.shot->width, o.shot->height );
    o.window_count = list_windows( o.windows, MAX_WINDOWS );
    for (int i = 0; i < o.window_count; i++) OffsetRect( &o.windows[i], -o.screen.left, -o.screen.top );
    o.mode = setting( L"SnipMode", MODE_RECTANGLE );
    if (o.mode != MODE_WINDOW && o.mode != MODE_FREEFORM) o.mode = MODE_RECTANGLE;
    get_theme( &o.theme );

    /* the screen, dimmed: half as bright */
    if (!(o.dim = image_create( o.shot->width, o.shot->height )) || !create_back_buffer( &o )) goto done;
    for (SIZE_T i = 0; i < (SIZE_T)o.shot->width * o.shot->height; i++)
        o.dim->bits[i] = 0xff000000 | ((o.shot->bits[i] >> 1) & 0x7f7f7f);

    layout_bar( &o );
    o.font = ui_font( (UINT)(96 * o.scale), px( &o, 14 ), FW_NORMAL );
    SelectObject( o.back_dc, o.font );

    RegisterClassW( &class );
    o.hwnd = CreateWindowExW( WS_EX_TOPMOST | WS_EX_TOOLWINDOW, class_name, string( IDS_OVERLAY_TITLE ), WS_POPUP,
                              o.screen.left, o.screen.top, o.shot->width, o.shot->height, 0, 0, instance, &o );
    if (!o.hwnd) goto done;
    DwmSetWindowAttribute( o.hwnd, DWMWA_ARCTIC_TRANSITION, &transition, sizeof(transition) );
    if (o.mode == MODE_WINDOW)
    {
        POINT pt;
        GetCursorPos( &pt );
        pt.x -= o.screen.left;
        pt.y -= o.screen.top;
        update_hover( &o, pt );
    }
    render( &o, &o.client );
    ShowWindow( o.hwnd, SW_SHOW );
    SetForegroundWindow( o.hwnd );
    SetFocus( o.hwnd );
    UpdateWindow( o.hwnd );

    while (!o.done)
    {
        if (GetMessageW( &msg, 0, 0, 0 ) <= 0)
        {
            quit = TRUE;
            break;
        }
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }
    DestroyWindow( o.hwnd );
    if (quit) PostQuitMessage( 0 );

done:
    if (o.back_dc) DeleteDC( o.back_dc );
    if (o.back_bitmap) DeleteObject( o.back_bitmap );
    if (o.font) DeleteObject( o.font );
    image_release( o.dim );
    image_release( o.shot );
    free( o.points );
    return o.result;
}
