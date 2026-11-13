/*
 * Paint: the picture on the screen and what the mouse does to it
 *
 * The picture sits in the middle of the grey round it, zoomed. What is not
 * part of it yet floats over it as a layer: a shape still being shaped, the
 * text being typed, a selection being moved. Each goes into the picture when
 * something else begins (canvas_commit), as in Paint.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <math.h>
#include "paint.h"

#define MARGIN px( 24 )
#define HANDLE_R px( 5 )
#define MAXPOLY 256

BOOL show_grid;

static HWND canvas;
static struct surface surf;
static double zoom = 1.0, ox, oy;
static int scroll_x, scroll_y;
static BOOL in_layout;
static POINT cursor_img;
static BOOL cursor_in;

enum drag
{
    D_NONE, D_DRAW, D_SHAPE_NEW, D_SHAPE_MOVE, D_SHAPE_HANDLE, D_CURVE_BEND, D_SEL_NEW, D_SEL_FREE, D_SEL_MOVE,
    D_SEL_HANDLE, D_CANVAS, D_TEXT_NEW, D_TEXT_MOVE, D_PAN
};
static enum drag drag;
static int drag_handle, drag_button;
static POINT drag_start, drag_last, drag_screen;
static double last_fx, last_fy;
static RECT drag_rect;
static int drag_scroll_x, drag_scroll_y;
static unsigned int seed = 12345;
static BOOL space_down;

/* a shape still being shaped */
static struct
{
    BOOL active;
    enum shape_kind kind;
    POINT pts[MAXPOLY];
    int count;
    RECT box;               /* image pixels, right and bottom past it */
    int stage;              /* the curve: 0 its line, 1 and 2 its bends; the polygon: 1 adding points */
    BOOL right;             /* drawn with the right button: the colours swapped */
    struct image *layer;
    POINT layer_at;
} shape;

/* the selection: a rectangle, a mask in it for a free form, its pixels
 * once they are lifted from the picture */
static struct
{
    BOOL active, floating;
    RECT r;
    BYTE *mask;             /* r's size, 1: selected; NULL: all of r */
    struct image *orig;     /* the lifted pixels, see-through where not selected */
    struct image *img;      /* orig at r's size */
    POINT *free_pts;
    int free_count, free_cap;
} sel;

/* the text being typed */
static struct
{
    BOOL active;
    RECT r;                 /* where it goes; at least the box drawn */
    int min_w, min_h;
    WCHAR *buf;
    int len, cap, caret;
    struct image *layer;
    BOOL caret_on;
    HFONT font;
    int line_h;
} text;

static void invalidate(void) { if (canvas) InvalidateRect( canvas, NULL, FALSE ); }

static void pic_changed(void)
{
    blit_forget( pic ? pic->bits : NULL );
    invalidate();
}

/**********************************************************************
 *          Where the picture is
 */

static void layout(void)
{
    RECT rc;
    SCROLLINFO si = { sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS };
    double iw, ih;
    int cw, ch;

    if (!canvas || !pic || in_layout) return;
    in_layout = TRUE;
    for (int pass = 0; pass < 2; pass++)
    {
        GetClientRect( canvas, &rc );
        iw = pic->width * zoom;
        ih = pic->height * zoom;
        cw = (int)ceil( iw ) + 2 * MARGIN + px( 8 );
        ch = (int)ceil( ih ) + 2 * MARGIN + px( 8 );
        scroll_x = max( 0, min( scroll_x, cw - rc.right ) );
        scroll_y = max( 0, min( scroll_y, ch - rc.bottom ) );
        si.nMin = 0;
        si.nMax = cw - 1;
        si.nPage = rc.right;
        si.nPos = scroll_x;
        SetScrollInfo( canvas, SB_HORZ, &si, TRUE );
        si.nMax = ch - 1;
        si.nPage = rc.bottom;
        si.nPos = scroll_y;
        SetScrollInfo( canvas, SB_VERT, &si, TRUE );
    }
    GetClientRect( canvas, &rc );
    ox = cw <= rc.right ? floor( (rc.right - iw) / 2 ) : MARGIN - scroll_x;
    oy = ch <= rc.bottom ? floor( (rc.bottom - ih) / 2 ) : MARGIN - scroll_y;
    in_layout = FALSE;
}

static void to_image( POINT pt, double *fx, double *fy )
{
    *fx = (pt.x - ox) / zoom;
    *fy = (pt.y - oy) / zoom;
}

static POINT image_pt( POINT pt )
{
    double fx, fy;
    POINT p;

    to_image( pt, &fx, &fy );
    p.x = (int)floor( fx );
    p.y = (int)floor( fy );
    return p;
}

static POINT screen_pt( double x, double y )
{
    POINT p = { (LONG)floor( ox + x * zoom + 0.5 ), (LONG)floor( oy + y * zoom + 0.5 ) };
    return p;
}

static RECT screen_rect( const RECT *r )
{
    POINT a = screen_pt( r->left, r->top ), b = screen_pt( r->right, r->bottom );
    RECT s = { a.x, a.y, b.x, b.y };
    return s;
}

double canvas_zoom(void) { return zoom; }

void canvas_set_zoom( double z, const POINT *around )
{
    RECT rc;
    POINT at;
    double fx, fy;

    z = max( 0.01, min( 8.0, z ) );
    if (!canvas || !pic) { zoom = z; return; }
    GetClientRect( canvas, &rc );
    if (around) at = *around;
    else { at.x = rc.right / 2; at.y = rc.bottom / 2; }
    to_image( at, &fx, &fy );
    zoom = z;
    /* the point under the mouse stays where it is */
    scroll_x = (int)floor( MARGIN - (at.x - fx * zoom) );
    scroll_y = (int)floor( MARGIN - (at.y - fy * zoom) );
    layout();
    shape.layer_at.x = -1;
    invalidate();
    status_refresh();
    ribbon_refresh();
}

void canvas_fit(void)
{
    RECT rc;
    double z;

    if (!canvas || !pic) return;
    GetClientRect( canvas, &rc );
    /* as layout() counts it, so that no scroll bar comes */
    z = min( (rc.right - 2.0 * MARGIN - px( 10 )) / pic->width, (rc.bottom - 2.0 * MARGIN - px( 10 )) / pic->height );
    z = max( 0.01, min( 1.0, z ) );
    zoom = z;
    scroll_x = scroll_y = 0;
    layout();
    invalidate();
    status_refresh();
}

void canvas_picture_changed(void)
{
    blit_forget( NULL );
    layout();
    invalidate();
    status_refresh();
    ribbon_refresh();
}

/**********************************************************************
 *          Layers
 */

static struct image *image_resampled( const struct image *src, int w, int h )
{
    struct image *out;

    if (w <= 0 || h <= 0 || !(out = image_create( w, h, 0 ))) return NULL;
    for (int y = 0; y < h; y++)
    {
        double sy0 = (double)y * src->height / h, sy1 = (double)(y + 1) * src->height / h;
        for (int x = 0; x < w; x++)
        {
            double sx0 = (double)x * src->width / w, sx1 = (double)(x + 1) * src->width / w;
            /* the mean of what it covers when smaller, the nearest when bigger */
            if (sx1 - sx0 <= 1.0 && sy1 - sy0 <= 1.0)
            {
                int ix = min( src->width - 1, (int)sx0 ), iy = min( src->height - 1, (int)sy0 );
                out->bits[(size_t)y * w + x] = src->bits[(size_t)iy * src->width + ix];
            }
            else
            {
                unsigned int sum[4] = { 0 }, n = 0;
                for (int j = (int)sy0; j < (int)ceil( sy1 ) && j < src->height; j++)
                    for (int i = (int)sx0; i < (int)ceil( sx1 ) && i < src->width; i++)
                    {
                        UINT32 p = src->bits[(size_t)j * src->width + i];
                        for (int k = 0; k < 4; k++) sum[k] += (p >> (8 * k)) & 0xff;
                        n++;
                    }
                if (n)
                    out->bits[(size_t)y * w + x] = (sum[0] / n) | ((sum[1] / n) << 8) | ((sum[2] / n) << 16) | ((sum[3] / n) << 24);
            }
        }
    }
    return out;
}

/* a layer onto the picture: see-through pixels leave it as it is */
static void stamp( struct image *dst, const struct image *src, int x0, int y0 )
{
    for (int y = 0; y < src->height; y++)
    {
        int dy = y0 + y;
        if (dy < 0 || dy >= dst->height) continue;
        for (int x = 0; x < src->width; x++)
        {
            int dx = x0 + x;
            UINT32 p = src->bits[(size_t)y * src->width + x], a = p >> 24, *d;

            if (dx < 0 || dx >= dst->width || !a) continue;
            d = dst->bits + (size_t)dy * dst->width + dx;
            if (a == 255) *d = p;
            else
            {
                /* over what is there, which may be see-through itself */
                UINT32 da = *d >> 24, rest = da * (255 - a) / 255, oa = a + rest, out = oa << 24;
                for (int k = 0; k < 24; k += 8)
                    out |= ((((p >> k) & 0xff) * a + ((*d >> k) & 0xff) * rest) / oa) << k;
                *d = out;
            }
        }
    }
}

/**********************************************************************
 *          Shapes
 */

static void shape_colors( COLORREF *outline, COLORREF *fillc )
{
    *outline = shape.right ? tools.color2 : tools.color1;
    *fillc = shape.right ? tools.color1 : tools.color2;
}

static void shape_box_from_points(void)
{
    if (shape.kind == SH_LINE || shape.kind == SH_CURVE || shape.kind == SH_POLYGON)
    {
        RECT b = { shape.pts[0].x, shape.pts[0].y, shape.pts[0].x + 1, shape.pts[0].y + 1 };
        for (int i = 1; i < shape.count; i++)
        {
            b.left = min( b.left, shape.pts[i].x );
            b.top = min( b.top, shape.pts[i].y );
            b.right = max( b.right, shape.pts[i].x + 1 );
            b.bottom = max( b.bottom, shape.pts[i].y + 1 );
        }
        shape.box = b;
    }
}

static void shape_draw_into( struct image *im, int dx, int dy )
{
    POINT pts[MAXPOLY];
    RECT box = shape.box;
    COLORREF oc, fc;
    int n = shape.count;

    shape_colors( &oc, &fc );
    for (int i = 0; i < n; i++)
    {
        pts[i].x = shape.pts[i].x - dx;
        pts[i].y = shape.pts[i].y - dy;
    }
    OffsetRect( &box, -dx, -dy );
    if (shape.kind == SH_POLYGON && shape.stage == 1)
    {
        /* still open: its sides as lines */
        for (int i = 0; i + 1 < n; i++)
            op_line( im, pts[i].x, pts[i].y, pts[i + 1].x, pts[i + 1].y, oc, tools.size[TOOL_SHAPE], FALSE );
        return;
    }
    op_shape( im, shape.kind, pts, n, &box, oc, fc, tools.size[TOOL_SHAPE],
              tools.outline != ST_NONE, tools.fill != ST_NONE );
}

static void shape_update_layer(void)
{
    int margin = tools.size[TOOL_SHAPE] + 2;
    RECT b;

    image_free( shape.layer );
    shape.layer = NULL;
    if (!shape.active) return;
    shape_box_from_points();
    b = shape.box;
    InflateRect( &b, margin, margin );
    if (!(shape.layer = image_create( b.right - b.left, b.bottom - b.top, 0 ))) return;
    shape.layer_at.x = b.left;
    shape.layer_at.y = b.top;
    shape_draw_into( shape.layer, b.left, b.top );
}

static void shape_commit(void)
{
    if (!shape.active) return;
    if (shape.kind == SH_POLYGON && shape.count >= 3) shape.stage = 2;
    history_push();
    shape_draw_into( pic, 0, 0 );
    shape.active = FALSE;
    image_free( shape.layer );
    shape.layer = NULL;
    pic_changed();
}

/* the handles of a box: corners and middles, from the top left round */
static POINT handle_pos( const RECT *s, int i )
{
    POINT p;
    int cx = (s->left + s->right) / 2, cy = (s->top + s->bottom) / 2;

    switch (i)
    {
    case 0: p.x = s->left; p.y = s->top; break;
    case 1: p.x = cx; p.y = s->top; break;
    case 2: p.x = s->right; p.y = s->top; break;
    case 3: p.x = s->right; p.y = cy; break;
    case 4: p.x = s->right; p.y = s->bottom; break;
    case 5: p.x = cx; p.y = s->bottom; break;
    case 6: p.x = s->left; p.y = s->bottom; break;
    default: p.x = s->left; p.y = cy; break;
    }
    return p;
}

static int hit_handles( const RECT *s, POINT pt )
{
    for (int i = 0; i < 8; i++)
    {
        POINT h = handle_pos( s, i );
        if (abs( pt.x - h.x ) <= HANDLE_R + 1 && abs( pt.y - h.y ) <= HANDLE_R + 1) return i;
    }
    return -1;
}

/* the line's ends are its handles */
static int hit_line_ends( POINT pt )
{
    for (int i = 0; i < 2; i++)
    {
        POINT h = screen_pt( shape.pts[i].x + 0.5, shape.pts[i].y + 0.5 );
        if (abs( pt.x - h.x ) <= HANDLE_R + 2 && abs( pt.y - h.y ) <= HANDLE_R + 2) return i;
    }
    return -1;
}

static void apply_handle( RECT *r, const RECT *from, int handle, int dx, int dy )
{
    *r = *from;
    if (handle == 0 || handle == 6 || handle == 7) r->left += dx;
    if (handle == 2 || handle == 3 || handle == 4) r->right += dx;
    if (handle == 0 || handle == 1 || handle == 2) r->top += dy;
    if (handle == 4 || handle == 5 || handle == 6) r->bottom += dy;
    if (r->right - r->left < 1)
    {
        if (handle == 0 || handle == 6 || handle == 7) r->left = r->right - 1;
        else r->right = r->left + 1;
    }
    if (r->bottom - r->top < 1)
    {
        if (handle == 0 || handle == 1 || handle == 2) r->top = r->bottom - 1;
        else r->bottom = r->top + 1;
    }
}

/**********************************************************************
 *          The selection
 */

static void sel_free(void)
{
    free( sel.mask );
    image_free( sel.orig );
    image_free( sel.img );
    free( sel.free_pts );
    memset( &sel, 0, sizeof(sel) );
}

BOOL canvas_has_selection(void) { return sel.active || text.active; }

static BOOL sel_selected( int x, int y )   /* in r's own pixels */
{
    return !sel.mask || sel.mask[(size_t)y * (sel.r.right - sel.r.left) + x];
}

/* the pixels go from the picture into the selection; what is left behind
 * is see-through, as in Windows 11's Paint */
static void sel_lift(void)
{
    int w = sel.r.right - sel.r.left, h = sel.r.bottom - sel.r.top;
    UINT32 key = to_pix( tools.color2 );

    if (!sel.active || sel.floating) return;
    history_push();
    if (!(sel.orig = image_create( w, h, 0 ))) return;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            int ix = sel.r.left + x, iy = sel.r.top + y;
            UINT32 p;

            if (ix < 0 || iy < 0 || ix >= pic->width || iy >= pic->height || !sel_selected( x, y )) continue;
            p = pic->bits[(size_t)iy * pic->width + ix];
            if (!(tools.transparent_select && p == key)) sel.orig->bits[(size_t)y * w + x] = p;
            pic->bits[(size_t)iy * pic->width + ix] = 0;
        }
    sel.img = image_copy( sel.orig );
    sel.floating = TRUE;
    pic_changed();
}

static void sel_commit(void)
{
    if (!sel.active) return;
    if (sel.floating && sel.img)
    {
        stamp( pic, sel.img, sel.r.left, sel.r.top );
        pic_changed();
    }
    sel_free();
    invalidate();
}

static void sel_rescale(void)
{
    int w = sel.r.right - sel.r.left, h = sel.r.bottom - sel.r.top;

    if (!sel.orig) return;
    image_free( sel.img );
    sel.img = (w == sel.orig->width && h == sel.orig->height) ? image_copy( sel.orig ) : image_resampled( sel.orig, w, h );
}

static void sel_set_rect( int l, int t, int r, int b )
{
    sel_free();
    l = max( 0, l ); t = max( 0, t );
    r = min( pic->width, r ); b = min( pic->height, b );
    if (r - l < 1 || b - t < 1) return;
    SetRect( &sel.r, l, t, r, b );
    sel.active = TRUE;
}

/* a free form becomes its bounding box and a mask of what is inside */
static void sel_from_free(void)
{
    RECT b = { INT_MAX, INT_MAX, INT_MIN, INT_MIN };
    POINT *pts = sel.free_pts;
    int n = sel.free_count, w, h;
    BYTE *mask;

    sel.free_pts = NULL;
    sel.free_count = sel.free_cap = 0;
    if (n < 3)
    {
        free( pts );
        sel_free();
        return;
    }
    for (int i = 0; i < n; i++)
    {
        pts[i].x = max( 0, min( pic->width - 1, pts[i].x ) );
        pts[i].y = max( 0, min( pic->height - 1, pts[i].y ) );
        b.left = min( b.left, pts[i].x );
        b.top = min( b.top, pts[i].y );
        b.right = max( b.right, pts[i].x + 1 );
        b.bottom = max( b.bottom, pts[i].y + 1 );
    }
    w = b.right - b.left;
    h = b.bottom - b.top;
    if (w < 2 || h < 2 || !(mask = calloc( (size_t)w * h, 1 )))
    {
        free( pts );
        sel_free();
        return;
    }
    for (int y = 0; y < h; y++)
    {
        double sy = b.top + y + 0.5;
        for (int i = 0; i < n; i++)
        {
            POINT a = pts[i], c = pts[(i + 1) % n];
            if ((a.y + 0.5 <= sy && c.y + 0.5 > sy) || (c.y + 0.5 <= sy && a.y + 0.5 > sy))
            {
                double xs = a.x + 0.5 + (sy - a.y - 0.5) * (c.x - a.x) / (double)(c.y - a.y) - b.left;
                /* even-odd: everything right of a crossing flips */
                for (int x = max( 0, (int)ceil( xs - 0.5 ) ); x < w; x++) mask[(size_t)y * w + x] ^= 1;
            }
        }
    }
    free( pts );
    sel_free();
    sel.r = b;
    sel.mask = mask;
    sel.active = TRUE;
}

void canvas_select_all(void)
{
    canvas_commit();
    set_tool( TOOL_SELECT );
    sel_set_rect( 0, 0, pic->width, pic->height );
    invalidate();
    status_refresh();
}

void canvas_invert_selection(void)
{
    BYTE *mask;
    RECT old;
    BYTE *old_mask;
    int ow;

    if (!sel.active) { canvas_select_all(); return; }
    if (sel.floating) sel_commit();
    if (!sel.active) return;
    old = sel.r;
    old_mask = sel.mask;
    sel.mask = NULL;
    ow = old.right - old.left;
    if (!(mask = malloc( (size_t)pic->width * pic->height ))) return;
    for (int y = 0; y < pic->height; y++)
        for (int x = 0; x < pic->width; x++)
        {
            BOOL in = x >= old.left && x < old.right && y >= old.top && y < old.bottom &&
                      (!old_mask || old_mask[(size_t)(y - old.top) * ow + (x - old.left)]);
            mask[(size_t)y * pic->width + x] = !in;
        }
    free( old_mask );
    sel_free();
    SetRect( &sel.r, 0, 0, pic->width, pic->height );
    sel.mask = mask;
    sel.active = TRUE;
    invalidate();
}

void canvas_delete_selection(void)
{
    if (text.active) return;
    if (!sel.active) return;
    if (!sel.floating) sel_lift();   /* see-through behind it */
    sel_free();
    pic_changed();
    status_refresh();
}

static struct image *selection_pixels(void)
{
    int w = sel.r.right - sel.r.left, h = sel.r.bottom - sel.r.top;
    struct image *out;

    if (sel.floating) return sel.img ? image_copy( sel.img ) : NULL;
    if (!(out = image_create( w, h, 0 ))) return NULL;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            int ix = sel.r.left + x, iy = sel.r.top + y;
            if (ix < 0 || iy < 0 || ix >= pic->width || iy >= pic->height || !sel_selected( x, y )) continue;
            out->bits[(size_t)y * w + x] = pic->bits[(size_t)iy * pic->width + ix];
        }
    return out;
}

void canvas_copy( BOOL cut )
{
    struct image *im;

    if (!sel.active) return;
    if (!(im = selection_pixels())) return;
    clipboard_put( frame, im );
    image_free( im );
    if (cut) canvas_delete_selection();
}

void canvas_paste( struct image *image )
{
    RECT rc;
    POINT at;

    if (!image) return;
    canvas_commit();
    /* a picture bigger than the canvas makes the canvas bigger */
    if (image->width > pic->width || image->height > pic->height)
    {
        struct image *grown = image_create( max( image->width, pic->width ), max( image->height, pic->height ), 0 );
        if (grown)
        {
            history_push();
            for (int y = 0; y < pic->height; y++)
                memcpy( grown->bits + (size_t)y * grown->width, pic->bits + (size_t)y * pic->width, pic->width * 4 );
            set_picture( grown );
        }
    }
    /* at the top left of what is seen of the picture */
    GetClientRect( canvas, &rc );
    at = image_pt( (POINT){ 0, 0 } );
    at.x = max( 0, at.x );
    at.y = max( 0, at.y );
    history_push();
    set_tool( TOOL_SELECT );
    sel_free();
    SetRect( &sel.r, at.x, at.y, at.x + image->width, at.y + image->height );
    sel.active = sel.floating = TRUE;
    sel.orig = image;
    sel.img = image_copy( image );
    invalidate();
    status_refresh();
    ribbon_refresh();
}

void canvas_crop(void)
{
    struct image *im;

    if (!sel.active) return;
    if (!(im = selection_pixels())) return;
    if (!sel.floating) history_push();
    sel_free();
    set_picture( im );
    canvas_fit();
}

static struct image *transformed( const struct image *src, int how )
{
    BOOL swap = how == IDM_ROTATE_RIGHT || how == IDM_ROTATE_LEFT;
    struct image *out = image_create( swap ? src->height : src->width, swap ? src->width : src->height, 0 );
    int w = src->width, h = src->height;

    if (!out) return NULL;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            UINT32 p = src->bits[(size_t)y * w + x];
            int nx, ny;
            switch (how)
            {
            case IDM_ROTATE_RIGHT: nx = h - 1 - y; ny = x; break;
            case IDM_ROTATE_LEFT: nx = y; ny = w - 1 - x; break;
            case IDM_ROTATE_180: nx = w - 1 - x; ny = h - 1 - y; break;
            case IDM_FLIP_V: nx = x; ny = h - 1 - y; break;
            default: nx = w - 1 - x; ny = y; break;
            }
            out->bits[(size_t)ny * out->width + nx] = p;
        }
    return out;
}

void canvas_rotate( int how )
{
    struct image *im;

    if (text.active) return;
    if (shape.active) shape_commit();
    if (sel.active)
    {
        int cx, cy;
        sel_lift();
        if (!sel.img || !(im = transformed( sel.img, how ))) return;
        cx = (sel.r.left + sel.r.right) / 2;
        cy = (sel.r.top + sel.r.bottom) / 2;
        image_free( sel.orig );
        image_free( sel.img );
        sel.orig = im;
        sel.img = image_copy( im );
        free( sel.mask );
        sel.mask = NULL;
        SetRect( &sel.r, cx - im->width / 2, cy - im->height / 2, cx - im->width / 2 + im->width,
                 cy - im->height / 2 + im->height );
        invalidate();
        return;
    }
    if (!(im = transformed( pic, how ))) return;
    history_push();
    set_picture( im );
}

/* resize and skew: the selection, or the whole picture */
void canvas_resize_skew( int w, int h, int skew_h, int skew_v )
{
    struct image *src, *scaled, *out;
    double th_ = tan( skew_h * 3.14159265 / 180 ), tv = tan( skew_v * 3.14159265 / 180 );
    int ow, oh;
    BOOL on_sel;

    if (text.active || w < 1 || h < 1) return;
    if (shape.active) shape_commit();
    on_sel = sel.active;
    if (on_sel) sel_lift();
    src = on_sel ? sel.img : pic;
    if (!src || !(scaled = image_resampled( src, w, h ))) return;
    if (!skew_h && !skew_v) out = scaled;
    else
    {
        /* each row slides across, each column up or down */
        int extra_w = (int)ceil( fabs( th_ ) * h ), extra_h = (int)ceil( fabs( tv ) * w );
        UINT32 back = 0;
        ow = w + extra_w;
        oh = h + extra_h;
        if (!(out = image_create( ow, oh, back ))) { image_free( scaled ); return; }
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                int nx = x + (int)floor( th_ >= 0 ? (h - 1 - y) * th_ : -y * th_ );
                int ny = y + (int)floor( tv >= 0 ? x * tv : (x - w + 1) * tv );
                if (nx >= 0 && ny >= 0 && nx < ow && ny < oh)
                    out->bits[(size_t)ny * ow + nx] = scaled->bits[(size_t)y * w + x];
            }
        image_free( scaled );
    }
    if (on_sel)
    {
        image_free( sel.orig );
        image_free( sel.img );
        sel.orig = out;
        sel.img = image_copy( out );
        free( sel.mask );
        sel.mask = NULL;
        sel.r.right = sel.r.left + out->width;
        sel.r.bottom = sel.r.top + out->height;
        invalidate();
    }
    else
    {
        history_push();
        set_picture( out );
    }
    status_refresh();
}

void canvas_selection_size( int *w, int *h )
{
    if (sel.active)
    {
        *w = sel.r.right - sel.r.left;
        *h = sel.r.bottom - sel.r.top;
    }
    else
    {
        *w = pic->width;
        *h = pic->height;
    }
}

/**********************************************************************
 *          Text
 */

static void text_font(void)
{
    LOGFONTW lf = { 0 };

    if (text.font) DeleteObject( text.font );
    lf.lfHeight = -MulDiv( text_style.size, 96, 72 );
    lf.lfWeight = text_style.bold ? FW_BOLD : FW_NORMAL;
    lf.lfItalic = text_style.italic;
    lf.lfUnderline = text_style.underline;
    lf.lfStrikeOut = text_style.strike;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = ANTIALIASED_QUALITY;
    lstrcpynW( lf.lfFaceName, text_style.face, LF_FACESIZE );
    text.font = CreateFontIndirectW( &lf );
}

/* the lines of the text and how wide each is */
static int text_lines( HDC hdc, int *starts, int *lens, int max_lines, int *widest )
{
    int n = 0, i = 0;

    *widest = 0;
    while (n < max_lines)
    {
        int j = i;
        SIZE size = { 0 };
        while (j < text.len && text.buf[j] != '\n') j++;
        starts[n] = i;
        lens[n] = j - i;
        GetTextExtentPoint32W( hdc, text.buf + i, j - i, &size );
        *widest = max( *widest, size.cx );
        n++;
        if (j >= text.len) break;
        i = j + 1;
    }
    return n;
}

#define TEXT_PAD 3

static void text_render(void)
{
    struct surface s = { 0 };
    HDC hdc;
    HGDIOBJ old;
    int starts[512], lens[512], n, widest, w, h;
    TEXTMETRICW tm;

    image_free( text.layer );
    text.layer = NULL;
    if (!text.active) return;
    if (!text.font) text_font();
    hdc = CreateCompatibleDC( NULL );
    old = SelectObject( hdc, text.font );
    GetTextMetricsW( hdc, &tm );
    text.line_h = tm.tmHeight;
    n = text_lines( hdc, starts, lens, ARRAY_SIZE(starts), &widest );
    SelectObject( hdc, old );
    DeleteDC( hdc );
    /* the box grows with what is typed */
    w = max( text.min_w, widest + 2 * TEXT_PAD + 2 );
    h = max( text.min_h, n * text.line_h + 2 * TEXT_PAD );
    text.r.right = text.r.left + w;
    text.r.bottom = text.r.top + h;
    if (!surface_size( &s, w, h )) return;
    /* the letters white on black: how much of each pixel they cover, which
     * is their alpha over whatever is below, see-through or not */
    {
        RECT all = { 0, 0, s.width, s.height };
        fill( &s, &all, RGB( 0, 0, 0 ) );
    }
    old = SelectObject( s.dc, text.font );
    SetTextColor( s.dc, RGB( 255, 255, 255 ) );
    SetBkMode( s.dc, TRANSPARENT );
    for (int i = 0; i < n; i++)
        TextOutW( s.dc, TEXT_PAD, TEXT_PAD + i * text.line_h, text.buf + starts[i], lens[i] );
    SelectObject( s.dc, old );
    GdiFlush();
    if ((text.layer = image_create( w, h, text_style.opaque ? to_pix( tools.color2 ) : 0 )))
    {
        UINT32 ink = to_pix( tools.color1 ) & 0xffffff;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                UINT32 c = s.bits[(size_t)y * s.width + x], *d = text.layer->bits + (size_t)y * w + x;
                UINT32 a = (((c >> 16) & 0xff) + ((c >> 8) & 0xff) + (c & 0xff)) / 3;
                if (!a) continue;
                if (!text_style.opaque) *d = (a << 24) | ink;
                else
                {
                    UINT32 out = 0xff000000;
                    for (int k = 0; k < 24; k += 8)
                        out |= ((((ink >> k) & 0xff) * a + ((*d >> k) & 0xff) * (255 - a)) / 255) << k;
                    *d = out;
                }
            }
    }
    surface_free( &s );
}

static void caret_place( int *x, int *y )
{
    HDC hdc = CreateCompatibleDC( NULL );
    HGDIOBJ old = SelectObject( hdc, text.font );
    int line = 0, start = 0;
    SIZE size = { 0 };

    for (int i = 0; i < text.caret; i++)
        if (text.buf[i] == '\n') { line++; start = i + 1; }
    GetTextExtentPoint32W( hdc, text.buf + start, text.caret - start, &size );
    SelectObject( hdc, old );
    DeleteDC( hdc );
    *x = text.r.left + TEXT_PAD + size.cx;
    *y = text.r.top + TEXT_PAD + line * text.line_h;
}

static int caret_from_point( int ix, int iy )
{
    HDC hdc = CreateCompatibleDC( NULL );
    HGDIOBJ old = SelectObject( hdc, text.font );
    int line = max( 0, (iy - text.r.top - TEXT_PAD) / max( text.line_h, 1 ) ), i = 0, best;

    for (int l = 0; l < line; l++)
    {
        while (i < text.len && text.buf[i] != '\n') i++;
        if (i < text.len) i++;
    }
    best = i;
    for (int j = i; ; j++)
    {
        SIZE size = { 0 };
        GetTextExtentPoint32W( hdc, text.buf + i, j - i, &size );
        if (text.r.left + TEXT_PAD + size.cx > ix + 2) break;
        best = j;
        if (j >= text.len || text.buf[j] == '\n') break;
    }
    SelectObject( hdc, old );
    DeleteDC( hdc );
    return best;
}

static void text_insert( const WCHAR *s, int n )
{
    if (text.len + n + 1 > text.cap)
    {
        int cap = max( text.cap * 2, text.len + n + 64 );
        WCHAR *grown = realloc( text.buf, cap * sizeof(WCHAR) );
        if (!grown) return;
        text.buf = grown;
        text.cap = cap;
    }
    memmove( text.buf + text.caret + n, text.buf + text.caret, (text.len - text.caret) * sizeof(WCHAR) );
    memcpy( text.buf + text.caret, s, n * sizeof(WCHAR) );
    text.len += n;
    text.caret += n;
    text.buf[text.len] = 0;
    text_render();
    invalidate();
}

static void text_start( int x, int y, int w, int h )
{
    text.active = TRUE;
    text.r.left = x;
    text.r.top = y;
    text.min_w = max( w, 24 );
    text.min_h = max( h, 8 );
    text.len = text.caret = 0;
    text.caret_on = TRUE;
    text_font();
    text_render();
    SetTimer( canvas, 1, GetCaretBlinkTime(), NULL );
    textbar_show( TRUE );
    invalidate();
}

static void text_end( BOOL keep )
{
    if (!text.active) return;
    if (keep && text.len && text.layer)
    {
        history_push();
        stamp( pic, text.layer, text.r.left, text.r.top );
        pic_changed();
    }
    text.active = FALSE;
    image_free( text.layer );
    text.layer = NULL;
    KillTimer( canvas, 1 );
    textbar_show( FALSE );
    invalidate();
}

BOOL canvas_text_active(void) { return text.active; }

void canvas_text_paste(void)
{
    HANDLE data;

    if (!text.active || !OpenClipboard( canvas )) return;
    if ((data = GetClipboardData( CF_UNICODETEXT )))
    {
        const WCHAR *s = GlobalLock( data );
        if (s)
        {
            for (; *s; s++)
            {
                WCHAR c = *s == '\r' ? 0 : *s;
                if (c) text_insert( &c, 1 );
            }
            GlobalUnlock( data );
        }
    }
    CloseClipboard();
}

static void text_key( WPARAM key )
{
    switch (key)
    {
    case VK_LEFT: if (text.caret > 0) text.caret--; break;
    case VK_RIGHT: if (text.caret < text.len) text.caret++; break;
    case VK_HOME: while (text.caret > 0 && text.buf[text.caret - 1] != '\n') text.caret--; break;
    case VK_END: while (text.caret < text.len && text.buf[text.caret] != '\n') text.caret++; break;
    case VK_UP:
    case VK_DOWN:
    {
        int x, y;
        caret_place( &x, &y );
        text.caret = caret_from_point( x, y + (key == VK_UP ? -text.line_h : text.line_h) + 1 );
        break;
    }
    case VK_DELETE:
        if (text.caret < text.len)
        {
            memmove( text.buf + text.caret, text.buf + text.caret + 1, (text.len - text.caret) * sizeof(WCHAR) );
            text.len--;
            text_render();
        }
        break;
    default:
        return;
    }
    text.caret_on = TRUE;
    invalidate();
}

static void text_char( WCHAR c )
{
    if (c == 8)
    {
        if (text.caret > 0)
        {
            memmove( text.buf + text.caret - 1, text.buf + text.caret, (text.len - text.caret + 1) * sizeof(WCHAR) );
            text.caret--;
            text.len--;
            text_render();
        }
    }
    else if (c == '\r') text_insert( L"\n", 1 );
    else if (c == 27) text_end( FALSE );
    else if (c >= 32 || c == '\t') text_insert( &c, 1 );
    text.caret_on = TRUE;
    invalidate();
}

/**********************************************************************
 *          What floats goes in
 */

void canvas_commit(void)
{
    if (drag && GetCapture() == canvas) ReleaseCapture();
    drag = D_NONE;
    shape_commit();
    text_end( TRUE );
    sel_commit();
    status_refresh();
    ribbon_refresh();
}

void canvas_cancel(void)
{
    if (drag && GetCapture() == canvas) ReleaseCapture();
    drag = D_NONE;
    shape.active = FALSE;
    image_free( shape.layer );
    shape.layer = NULL;
    text_end( FALSE );
    sel_free();
    invalidate();
}

void canvas_style_changed(void)
{
    if (shape.active) shape_update_layer();
    if (text.active)
    {
        text_font();
        text_render();
    }
    invalidate();
}

void canvas_get_status( POINT *cursor, BOOL *in, SIZE *selection )
{
    *cursor = cursor_img;
    *in = cursor_in;
    selection->cx = selection->cy = 0;
    if (sel.active)
    {
        selection->cx = sel.r.right - sel.r.left;
        selection->cy = sel.r.bottom - sel.r.top;
    }
    else if (drag == D_SEL_NEW || drag == D_SHAPE_NEW || drag == D_TEXT_NEW)
    {
        selection->cx = abs( drag_last.x - drag_start.x ) + 1;
        selection->cy = abs( drag_last.y - drag_start.y ) + 1;
    }
    else if (shape.active)
    {
        selection->cx = shape.box.right - shape.box.left;
        selection->cy = shape.box.bottom - shape.box.top;
    }
}

/**********************************************************************
 *          Painting
 */

static void dashed_rect( const RECT *r )
{
    COLORREF a = RGB( 0x1f, 0x1f, 0x1f ), b = RGB( 0xff, 0xff, 0xff );

    GdiFlush();
    for (int x = r->left; x < r->right; x++)
        for (int k = 0; k < 2; k++)
        {
            int y = k ? r->bottom - 1 : r->top;
            if (x < 0 || y < 0 || x >= surf.width || y >= surf.height) continue;
            surf.bits[(size_t)y * surf.width + x] = ((x / 4) & 1) ? 0xff000000 | b : 0xff000000 | (GetRValue( a ) << 16) | (GetGValue( a ) << 8) | GetBValue( a );
        }
    for (int y = r->top; y < r->bottom; y++)
        for (int k = 0; k < 2; k++)
        {
            int x = k ? r->right - 1 : r->left;
            if (x < 0 || y < 0 || x >= surf.width || y >= surf.height) continue;
            surf.bits[(size_t)y * surf.width + x] = ((y / 4) & 1) ? 0xff000000 | b : 0xff000000 | (GetRValue( a ) << 16) | (GetGValue( a ) << 8) | GetBValue( a );
        }
}

static void draw_handle( POINT p )
{
    fill_circle( &surf, p.x + 0.5f, p.y + 0.5f, (float)HANDLE_R, RGB( 255, 255, 255 ), 255 );
    ring( &surf, p.x + 0.5f, p.y + 0.5f, (float)HANDLE_R, 1.0f, RGB( 0x5d, 0x5d, 0x5d ), 255 );
}

static void draw_box_handles( const RECT *r )
{
    for (int i = 0; i < 8; i++) draw_handle( handle_pos( r, i ) );
}

/* the canvas's own handles: its right side, its bottom and their corner */
static RECT canvas_handle_rect( int i )
{
    RECT img = screen_rect( &(RECT){ 0, 0, pic->width, pic->height } ), r;
    int s = px( 7 ), g = px( 3 );
    POINT c;

    if (i == 0) { c.x = img.right + g + s / 2; c.y = (img.top + img.bottom) / 2; }
    else if (i == 1) { c.x = (img.left + img.right) / 2; c.y = img.bottom + g + s / 2; }
    else { c.x = img.right + g + s / 2; c.y = img.bottom + g + s / 2; }
    SetRect( &r, c.x - s / 2, c.y - s / 2, c.x - s / 2 + s, c.y - s / 2 + s );
    return r;
}

static int hit_canvas_handle( POINT pt )
{
    for (int i = 0; i < 3; i++)
    {
        RECT r = canvas_handle_rect( i );
        InflateRect( &r, px( 3 ), px( 3 ) );
        if (PtInRect( &r, pt )) return i;
    }
    return -1;
}

static void paint( HDC hdc, const RECT *update )
{
    RECT rc, img;

    GetClientRect( canvas, &rc );
    if (!pic || !surface_size( &surf, rc.right, rc.bottom )) return;
    fill( &surf, &rc, th.workspace );
    img = screen_rect( &(RECT){ 0, 0, pic->width, pic->height } );
    if (drag == D_CANVAS)
    {
        /* the new size as an outline while it is dragged */
        RECT r = { 0, 0, drag_rect.right, drag_rect.bottom };
        shadow_rect( &surf, &img, px( 10 ), th.dark ? 120 : 40 );
        blit_image( &surf, &rc, pic->bits, pic->width, pic->height, ox, oy, zoom, 0 );
        dashed_rect( &(RECT){ screen_rect( &r ).left, screen_rect( &r ).top, screen_rect( &r ).right + 1, screen_rect( &r ).bottom + 1 } );
    }
    else
    {
        shadow_rect( &surf, &img, px( 10 ), th.dark ? 120 : 40 );
        blit_image( &surf, &rc, pic->bits, pic->width, pic->height, ox, oy, zoom, 0 );
    }

    /* the grid between the pixels, when they are big enough to have one */
    if (show_grid && zoom >= 3)
    {
        RECT vis;
        GdiFlush();
        IntersectRect( &vis, &rc, &img );
        for (int x = 0; x <= pic->width; x++)
        {
            int sx = (int)floor( ox + x * zoom );
            if (sx < vis.left || sx >= vis.right) continue;
            for (int y = vis.top; y < vis.bottom; y++) surf.bits[(size_t)y * surf.width + sx] = 0xffc0c0c0;
        }
        for (int y = 0; y <= pic->height; y++)
        {
            int sy = (int)floor( oy + y * zoom );
            if (sy < vis.top || sy >= vis.bottom) continue;
            for (int x = vis.left; x < vis.right; x++) surf.bits[(size_t)sy * surf.width + x] = 0xffc0c0c0;
        }
    }

    if (sel.active && sel.floating && sel.img)
        blit_image( &surf, &rc, sel.img->bits, sel.img->width, sel.img->height, ox + sel.r.left * zoom,
                    oy + sel.r.top * zoom, zoom, BLIT_LAYER );
    if (shape.active && shape.layer)
        blit_image( &surf, &rc, shape.layer->bits, shape.layer->width, shape.layer->height,
                    ox + shape.layer_at.x * zoom, oy + shape.layer_at.y * zoom, zoom, BLIT_LAYER );
    if (text.active && text.layer)
        blit_image( &surf, &rc, text.layer->bits, text.layer->width, text.layer->height, ox + text.r.left * zoom,
                    oy + text.r.top * zoom, zoom, BLIT_LAYER );

    /* the canvas's handles */
    if (drag != D_CANVAS && !sel.active && !shape.active && !text.active)
        for (int i = 0; i < 3; i++)
        {
            RECT h = canvas_handle_rect( i );
            fill( &surf, &h, RGB( 0xff, 0xff, 0xff ) );
            outline_round( &surf, &h, 0, RGB( 0x6e, 0x6e, 0x6e ), 255 );
        }

    if (sel.active)
    {
        RECT s = screen_rect( &sel.r );
        if (sel.mask && !sel.floating)
        {
            /* the free form's edge: where selected meets not selected */
            int w = sel.r.right - sel.r.left, h = sel.r.bottom - sel.r.top;
            GdiFlush();
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                {
                    BOOL in = sel.mask[(size_t)y * w + x], edge;
                    if (!in) continue;
                    edge = x == 0 || y == 0 || x == w - 1 || y == h - 1 || !sel.mask[(size_t)y * w + x - 1] ||
                           !sel.mask[(size_t)y * w + x + 1] || !sel.mask[(size_t)(y - 1) * w + x] ||
                           !sel.mask[(size_t)(y + 1) * w + x];
                    if (edge)
                    {
                        POINT p = screen_pt( sel.r.left + x, sel.r.top + y );
                        RECT d = { p.x, p.y, p.x + max( 1, (int)zoom ), p.y + max( 1, (int)zoom ) };
                        fill( &surf, &d, ((x + y) / 3) & 1 ? RGB( 255, 255, 255 ) : RGB( 0x1f, 0x1f, 0x1f ) );
                    }
                }
        }
        else dashed_rect( &(RECT){ s.left - 1, s.top - 1, s.right + 1, s.bottom + 1 } );
        if (drag != D_SEL_NEW) draw_box_handles( &(RECT){ s.left - 1, s.top - 1, s.right, s.bottom } );
    }
    if (drag == D_SEL_NEW)
    {
        RECT r = { min( drag_start.x, drag_last.x ), min( drag_start.y, drag_last.y ),
                   max( drag_start.x, drag_last.x ) + 1, max( drag_start.y, drag_last.y ) + 1 };
        RECT s = screen_rect( &r );
        dashed_rect( &s );
    }
    if (drag == D_SEL_FREE && sel.free_count > 1)
    {
        HPEN pen = CreatePen( PS_SOLID, 1, RGB( 0x1f, 0x1f, 0x1f ) );
        HGDIOBJ old = SelectObject( surf.dc, pen );
        POINT p = screen_pt( sel.free_pts[0].x + 0.5, sel.free_pts[0].y + 0.5 );
        MoveToEx( surf.dc, p.x, p.y, NULL );
        for (int i = 1; i < sel.free_count; i++)
        {
            p = screen_pt( sel.free_pts[i].x + 0.5, sel.free_pts[i].y + 0.5 );
            LineTo( surf.dc, p.x, p.y );
        }
        SelectObject( surf.dc, old );
        DeleteObject( pen );
        GdiFlush();
    }
    if (shape.active && drag != D_SHAPE_NEW)
    {
        if (shape.kind == SH_LINE || shape.kind == SH_CURVE)
        {
            for (int i = 0; i < 2; i++) draw_handle( screen_pt( shape.pts[i].x + 0.5, shape.pts[i].y + 0.5 ) );
        }
        else if (shape.kind != SH_POLYGON || shape.stage != 1)
        {
            RECT s = screen_rect( &shape.box );
            dashed_rect( &(RECT){ s.left - 1, s.top - 1, s.right + 1, s.bottom + 1 } );
            draw_box_handles( &(RECT){ s.left - 1, s.top - 1, s.right, s.bottom } );
        }
    }
    if (text.active)
    {
        RECT s = screen_rect( &text.r );
        dashed_rect( &(RECT){ s.left - 1, s.top - 1, s.right + 1, s.bottom + 1 } );
        if (text.caret_on && GetFocus() == canvas)
        {
            int cx, cy;
            POINT a, b;
            RECT c;
            caret_place( &cx, &cy );
            a = screen_pt( cx, cy );
            b = screen_pt( cx, cy + text.line_h );
            SetRect( &c, a.x, a.y, a.x + max( 1, px( 1 ) ), b.y );
            fill( &surf, &c, text_style.opaque ? from_pix( ~to_pix( tools.color2 ) ) : RGB( 0, 0, 0 ) );
        }
    }
    /* the eraser's square under the mouse */
    if (tools.tool == TOOL_ERASER && cursor_in && drag != D_PAN)
    {
        int s = tools.size[TOOL_ERASER];
        RECT r = { cursor_img.x - (s - 1) / 2, cursor_img.y - (s - 1) / 2, cursor_img.x - (s - 1) / 2 + s, cursor_img.y - (s - 1) / 2 + s };
        RECT sr = screen_rect( &r );
        outline_round( &surf, &sr, 0, RGB( 0, 0, 0 ), 255 );
        InflateRect( &sr, -1, -1 );
        outline_round( &surf, &sr, 0, RGB( 255, 255, 255 ), 255 );
    }
    GdiFlush();
    BitBlt( hdc, update->left, update->top, update->right - update->left, update->bottom - update->top, surf.dc,
            update->left, update->top, SRCCOPY );
}

/**********************************************************************
 *          The mouse
 */

static int hit_sel_handle( POINT pt )
{
    RECT s;
    if (!sel.active) return -1;
    s = screen_rect( &sel.r );
    s.left--; s.top--;
    return hit_handles( &s, pt );
}

static BOOL in_sel( POINT pt )
{
    RECT s;
    if (!sel.active) return FALSE;
    s = screen_rect( &sel.r );
    return PtInRect( &s, pt );
}

static int hit_shape_handle( POINT pt )
{
    RECT s;
    if (!shape.active || (shape.kind == SH_POLYGON && shape.stage == 1)) return -1;
    if (shape.kind == SH_LINE || shape.kind == SH_CURVE) return hit_line_ends( pt );
    s = screen_rect( &shape.box );
    s.left--; s.top--;
    return hit_handles( &s, pt );
}

static BOOL in_shape( POINT pt )
{
    RECT s;
    if (!shape.active) return FALSE;
    s = screen_rect( &shape.box );
    InflateRect( &s, px( 2 ), px( 2 ) );
    return PtInRect( &s, pt );
}

static void pick_color( POINT ip, BOOL right )
{
    COLORREF c;
    if (ip.x < 0 || ip.y < 0 || ip.x >= pic->width || ip.y >= pic->height) return;
    c = from_pix( pic->bits[(size_t)ip.y * pic->width + ip.x] );
    if (right) tools.color2 = c;
    else tools.color1 = c;
    set_tool( tools.before_picker );
    ribbon_refresh();
}

static const double zoom_steps[] = { 0.125, 0.25, 0.5, 0.75, 1, 2, 3, 4, 5, 6, 7, 8 };

void canvas_zoom_step( int dir, const POINT *around )
{
    double z = zoom;
    if (dir > 0)
    {
        for (int i = 0; i < ARRAY_SIZE(zoom_steps); i++)
            if (zoom_steps[i] > zoom + 1e-6) { z = zoom_steps[i]; break; }
    }
    else
    {
        for (int i = ARRAY_SIZE(zoom_steps) - 1; i >= 0; i--)
            if (zoom_steps[i] < zoom - 1e-6) { z = zoom_steps[i]; break; }
    }
    canvas_set_zoom( z, around );
}

static void mouse_down( POINT pt, BOOL right, BOOL dbl )
{
    POINT ip = image_pt( pt );
    double fx, fy;
    int h;

    SetFocus( canvas );
    to_image( pt, &fx, &fy );
    drag_button = right;
    drag_start = drag_last = ip;
    drag_screen = pt;
    last_fx = fx;
    last_fy = fy;

    if (space_down || GetKeyState( VK_MBUTTON ) < 0)
    {
        drag = D_PAN;
        drag_scroll_x = scroll_x;
        drag_scroll_y = scroll_y;
        SetCapture( canvas );
        return;
    }

    /* the canvas's own handles come first */
    if (!sel.active && !shape.active && !text.active && !right && (h = hit_canvas_handle( pt )) >= 0)
    {
        drag = D_CANVAS;
        drag_handle = h;
        SetRect( &drag_rect, 0, 0, pic->width, pic->height );
        SetCapture( canvas );
        return;
    }

    /* the curve's bends and the polygon's corners, after its first line */
    if (shape.active && shape.kind == SH_CURVE && shape.stage >= 1)
    {
        if (shape.count < 4) shape.count++;
        shape.pts[shape.count - 1] = ip;
        if (shape.stage == 1 && shape.count == 3) shape.pts[3] = ip, shape.count = 3;
        drag = D_CURVE_BEND;
        shape_update_layer();
        invalidate();
        SetCapture( canvas );
        return;
    }
    if (shape.active && shape.kind == SH_POLYGON && shape.stage == 1)
    {
        POINT first = screen_pt( shape.pts[0].x + 0.5, shape.pts[0].y + 0.5 );
        if (dbl || (abs( pt.x - first.x ) <= HANDLE_R + 2 && abs( pt.y - first.y ) <= HANDLE_R + 2 && shape.count >= 3))
        {
            shape.stage = 2;
            shape_commit();
            return;
        }
        if (shape.count < MAXPOLY) shape.pts[shape.count++] = ip;
        drag = D_SHAPE_HANDLE;
        drag_handle = 100 + shape.count - 1;   /* the corner just made follows the mouse */
        shape_update_layer();
        invalidate();
        SetCapture( canvas );
        return;
    }

    if (shape.active)
    {
        if ((h = hit_shape_handle( pt )) >= 0)
        {
            drag = D_SHAPE_HANDLE;
            drag_handle = h;
            drag_rect = shape.box;
            SetCapture( canvas );
            return;
        }
        if (in_shape( pt ) && shape.kind != SH_LINE && shape.kind != SH_CURVE)
        {
            drag = D_SHAPE_MOVE;
            drag_rect = shape.box;
            SetCapture( canvas );
            return;
        }
        shape_commit();
    }
    if (text.active)
    {
        RECT s = screen_rect( &text.r ), inner = s;
        InflateRect( &s, px( 4 ), px( 4 ) );
        InflateRect( &inner, -px( 4 ), -px( 4 ) );
        if (PtInRect( &inner, pt ))
        {
            text.caret = caret_from_point( ip.x, ip.y );
            text.caret_on = TRUE;
            invalidate();
            return;
        }
        if (PtInRect( &s, pt ))
        {
            drag = D_TEXT_MOVE;
            drag_rect = text.r;
            SetCapture( canvas );
            return;
        }
        text_end( TRUE );
        return;
    }
    if (sel.active)
    {
        if ((h = hit_sel_handle( pt )) >= 0)
        {
            sel_lift();
            drag = D_SEL_HANDLE;
            drag_handle = h;
            drag_rect = sel.r;
            SetCapture( canvas );
            return;
        }
        if (in_sel( pt ) && tools.tool == TOOL_SELECT)
        {
            sel_lift();
            /* Ctrl: a copy is left where it was */
            if (GetKeyState( VK_CONTROL ) < 0 && sel.img) stamp( pic, sel.img, sel.r.left, sel.r.top ), pic_changed();
            drag = D_SEL_MOVE;
            drag_rect = sel.r;
            SetCapture( canvas );
            return;
        }
        sel_commit();
        if (tools.tool == TOOL_SELECT && !right) { /* a new selection begins below */ }
        else { status_refresh(); return; }
    }

    switch (tools.tool)
    {
    case TOOL_SELECT:
        if (right) return;
        if (tools.select == SEL_FREE)
        {
            sel_free();
            sel.free_cap = 256;
            sel.free_pts = malloc( sel.free_cap * sizeof(POINT) );
            sel.free_count = 0;
            if (sel.free_pts) sel.free_pts[sel.free_count++] = ip;
            drag = D_SEL_FREE;
        }
        else drag = D_SEL_NEW;
        break;
    case TOOL_PENCIL:
        history_push();
        op_line( pic, ip.x, ip.y, ip.x, ip.y, right ? tools.color2 : tools.color1, tools.size[TOOL_PENCIL], FALSE );
        pic_changed();
        drag = D_DRAW;
        break;
    case TOOL_BRUSH:
        history_push();
        op_brush_segment( pic, tools.brush, fx, fy, fx, fy, right ? tools.color2 : tools.color1, tools.size[TOOL_BRUSH], &seed );
        pic_changed();
        drag = D_DRAW;
        if (tools.brush == BR_AIRBRUSH) SetTimer( canvas, 2, 30, NULL );
        break;
    case TOOL_ERASER:
        history_push();
        op_erase( pic, ip.x, ip.y, ip.x, ip.y, tools.size[TOOL_ERASER] );
        pic_changed();
        drag = D_DRAW;
        break;
    case TOOL_FILL:
        if (ip.x < 0 || ip.y < 0 || ip.x >= pic->width || ip.y >= pic->height) return;
        history_push();
        op_flood_fill( pic, ip.x, ip.y, right ? tools.color2 : tools.color1 );
        pic_changed();
        return;
    case TOOL_PICKER:
        pick_color( ip, right );
        return;
    case TOOL_MAGNIFIER:
        canvas_zoom_step( right ? -1 : 1, &pt );
        return;
    case TOOL_TEXT:
        if (right) return;
        drag = D_TEXT_NEW;
        break;
    case TOOL_SHAPE:
        memset( shape.pts, 0, sizeof(shape.pts) );
        shape.active = TRUE;
        shape.kind = tools.shape;
        shape.right = right;
        shape.pts[0] = shape.pts[1] = ip;
        shape.count = 2;
        shape.stage = 0;
        SetRect( &shape.box, ip.x, ip.y, ip.x + 1, ip.y + 1 );
        shape_update_layer();
        drag = D_SHAPE_NEW;
        break;
    default:
        return;
    }
    SetCapture( canvas );
    invalidate();
}

static void box_from_drag( RECT *r, BOOL square )
{
    int dx = drag_last.x - drag_start.x, dy = drag_last.y - drag_start.y;

    if (square)
    {
        int m = max( abs( dx ), abs( dy ) );
        dx = dx < 0 ? -m : m;
        dy = dy < 0 ? -m : m;
    }
    r->left = min( drag_start.x, drag_start.x + dx );
    r->top = min( drag_start.y, drag_start.y + dy );
    r->right = max( drag_start.x, drag_start.x + dx ) + 1;
    r->bottom = max( drag_start.y, drag_start.y + dy ) + 1;
}

static void mouse_move( POINT pt, WPARAM keys )
{
    POINT ip = image_pt( pt );
    double fx, fy;
    BOOL shift = (keys & MK_SHIFT) != 0;

    to_image( pt, &fx, &fy );
    cursor_img = ip;
    cursor_in = ip.x >= 0 && ip.y >= 0 && ip.x < pic->width && ip.y < pic->height;
    if (!drag)
    {
        if (tools.tool == TOOL_ERASER) invalidate();
        status_refresh();
        return;
    }
    switch (drag)
    {
    case D_PAN:
        scroll_x = drag_scroll_x - (pt.x - drag_screen.x);
        scroll_y = drag_scroll_y - (pt.y - drag_screen.y);
        layout();
        break;
    case D_DRAW:
        if (tools.tool == TOOL_PENCIL)
            op_line( pic, drag_last.x, drag_last.y, ip.x, ip.y, drag_button ? tools.color2 : tools.color1,
                     tools.size[TOOL_PENCIL], FALSE );
        else if (tools.tool == TOOL_ERASER)
            op_erase( pic, drag_last.x, drag_last.y, ip.x, ip.y, tools.size[TOOL_ERASER] );
        else
            op_brush_segment( pic, tools.brush, last_fx, last_fy, fx, fy, drag_button ? tools.color2 : tools.color1,
                              tools.size[TOOL_BRUSH], &seed );
        pic_changed();
        break;
    case D_SEL_NEW:
    case D_TEXT_NEW:
        break;
    case D_SEL_FREE:
        if (sel.free_pts && (ip.x != drag_last.x || ip.y != drag_last.y))
        {
            if (sel.free_count == sel.free_cap)
            {
                POINT *grown = realloc( sel.free_pts, sel.free_cap * 2 * sizeof(POINT) );
                if (!grown) break;
                sel.free_pts = grown;
                sel.free_cap *= 2;
            }
            sel.free_pts[sel.free_count++] = ip;
        }
        break;
    case D_SEL_MOVE:
        OffsetRect( &sel.r, 0, 0 );
        sel.r = drag_rect;
        OffsetRect( &sel.r, ip.x - drag_start.x, ip.y - drag_start.y );
        break;
    case D_SEL_HANDLE:
        apply_handle( &sel.r, &drag_rect, drag_handle, ip.x - drag_start.x, ip.y - drag_start.y );
        sel_rescale();
        break;
    case D_TEXT_MOVE:
        text.r = drag_rect;
        OffsetRect( &text.r, ip.x - drag_start.x, ip.y - drag_start.y );
        text_render();
        break;
    case D_CANVAS:
    {
        int w = pic->width, h = pic->height;
        double ix = fx, iy = fy;
        if (drag_handle == 0 || drag_handle == 2) w = max( 1, (int)floor( ix + 0.5 ) );
        if (drag_handle == 1 || drag_handle == 2) h = max( 1, (int)floor( iy + 0.5 ) );
        SetRect( &drag_rect, 0, 0, w, h );
        break;
    }
    case D_SHAPE_NEW:
        if (shape.kind == SH_LINE || shape.kind == SH_CURVE || shape.kind == SH_POLYGON)
        {
            POINT e = ip;
            if (shift)   /* a line at a multiple of 45 degrees */
            {
                int dx = ip.x - drag_start.x, dy = ip.y - drag_start.y;
                if (abs( dx ) > 2 * abs( dy )) e.y = drag_start.y;
                else if (abs( dy ) > 2 * abs( dx )) e.x = drag_start.x;
                else
                {
                    int m = max( abs( dx ), abs( dy ) );
                    e.x = drag_start.x + (dx < 0 ? -m : m);
                    e.y = drag_start.y + (dy < 0 ? -m : m);
                }
            }
            shape.pts[1] = e;
        }
        else
        {
            drag_last = ip;
            box_from_drag( &shape.box, shift );
        }
        shape_update_layer();
        break;
    case D_SHAPE_MOVE:
    {
        int dx = ip.x - drag_start.x, dy = ip.y - drag_start.y;
        shape.box = drag_rect;
        OffsetRect( &shape.box, dx, dy );
        shape_update_layer();
        break;
    }
    case D_SHAPE_HANDLE:
        if (drag_handle >= 100) shape.pts[drag_handle - 100] = ip;
        else if (shape.kind == SH_LINE || shape.kind == SH_CURVE) shape.pts[drag_handle] = ip;
        else apply_handle( &shape.box, &drag_rect, drag_handle, ip.x - drag_start.x, ip.y - drag_start.y );
        shape_update_layer();
        break;
    case D_CURVE_BEND:
        shape.pts[shape.count - 1] = ip;
        if (shape.stage == 1) shape.pts[3] = ip, shape.count = 3;
        shape_update_layer();
        break;
    default:
        break;
    }
    drag_last = ip;
    last_fx = fx;
    last_fy = fy;
    invalidate();
    status_refresh();
}

static void mouse_up( POINT pt )
{
    enum drag was = drag;
    RECT r;

    drag = D_NONE;
    KillTimer( canvas, 2 );
    if (GetCapture() == canvas) ReleaseCapture();
    switch (was)
    {
    case D_SEL_NEW:
        box_from_drag( &r, GetKeyState( VK_SHIFT ) < 0 );
        if (r.right - r.left > 1 || r.bottom - r.top > 1) sel_set_rect( r.left, r.top, r.right, r.bottom );
        break;
    case D_SEL_FREE:
        sel_from_free();
        break;
    case D_TEXT_NEW:
        box_from_drag( &r, FALSE );
        if (r.right - r.left < 8) r.right = r.left + 160;
        if (r.bottom - r.top < 8) r.bottom = r.top + MulDiv( text_style.size, 96, 72 ) * 3 / 2 + 2 * TEXT_PAD;
        text_start( r.left, r.top, r.right - r.left, r.bottom - r.top );
        break;
    case D_CANVAS:
    {
        struct image *im = image_create( drag_rect.right, drag_rect.bottom, 0 );   /* the new part see-through */
        if (im && (drag_rect.right != pic->width || drag_rect.bottom != pic->height))
        {
            for (int y = 0; y < min( im->height, pic->height ); y++)
                memcpy( im->bits + (size_t)y * im->width, pic->bits + (size_t)y * pic->width, min( im->width, pic->width ) * 4 );
            history_push();
            set_picture( im );
        }
        else image_free( im );
        break;
    }
    case D_SHAPE_NEW:
        if (shape.kind == SH_CURVE)
        {
            if (shape.pts[0].x == shape.pts[1].x && shape.pts[0].y == shape.pts[1].y) { canvas_cancel(); break; }
            shape.stage = 1;
            shape.pts[2] = shape.pts[0];
            shape.pts[3] = shape.pts[1];
        }
        else if (shape.kind == SH_POLYGON)
        {
            shape.stage = 1;
        }
        else if (shape.kind != SH_LINE && (shape.box.right - shape.box.left < 2 && shape.box.bottom - shape.box.top < 2))
        {
            canvas_cancel();
        }
        shape_update_layer();
        break;
    case D_CURVE_BEND:
        if (shape.stage == 1) shape.stage = 2;
        else
        {
            shape_commit();
        }
        break;
    default:
        break;
    }
    invalidate();
    status_refresh();
    ribbon_refresh();
}

static BOOL set_cursor( POINT pt )
{
    UINT_PTR id = (UINT_PTR)IDC_CROSS;
    int h;

    if (space_down || drag == D_PAN) id = (UINT_PTR)IDC_SIZEALL;
    else if (!sel.active && !shape.active && !text.active && (h = hit_canvas_handle( pt )) >= 0)
        id = h == 0 ? (UINT_PTR)IDC_SIZEWE : h == 1 ? (UINT_PTR)IDC_SIZENS : (UINT_PTR)IDC_SIZENWSE;
    else if ((h = hit_shape_handle( pt )) >= 0 || (h = hit_sel_handle( pt )) >= 0)
    {
        if (shape.active && (shape.kind == SH_LINE || shape.kind == SH_CURVE)) id = (UINT_PTR)IDC_SIZEALL;
        else id = (h == 0 || h == 4) ? (UINT_PTR)IDC_SIZENWSE : (h == 2 || h == 6) ? (UINT_PTR)IDC_SIZENESW : (h == 1 || h == 5) ? (UINT_PTR)IDC_SIZENS : (UINT_PTR)IDC_SIZEWE;
    }
    else if ((shape.active && in_shape( pt ) && shape.kind != SH_LINE && shape.kind != SH_CURVE) ||
             (sel.active && in_sel( pt ) && tools.tool == TOOL_SELECT))
        id = (UINT_PTR)IDC_SIZEALL;
    else if (text.active)
    {
        RECT s = screen_rect( &text.r ), inner = s;
        InflateRect( &s, px( 4 ), px( 4 ) );
        InflateRect( &inner, -px( 4 ), -px( 4 ) );
        id = PtInRect( &inner, pt ) ? (UINT_PTR)IDC_IBEAM : PtInRect( &s, pt ) ? (UINT_PTR)IDC_SIZEALL : (UINT_PTR)IDC_ARROW;
    }
    else if (tools.tool == TOOL_TEXT) id = (UINT_PTR)IDC_IBEAM;
    else if (tools.tool == TOOL_ERASER) id = (UINT_PTR)IDC_ARROW;
    else if (tools.tool == TOOL_MAGNIFIER) id = (UINT_PTR)IDC_HAND;
    SetCursor( LoadCursorW( NULL, (LPCWSTR)id ) );
    return TRUE;
}

/**********************************************************************
 *          The window
 */

static void scroll( int bar, int code )
{
    SCROLLINFO si = { sizeof(si), SIF_ALL };
    int *pos = bar == SB_HORZ ? &scroll_x : &scroll_y;

    GetScrollInfo( canvas, bar, &si );
    switch (code)
    {
    case SB_LINEUP: *pos -= px( 40 ); break;
    case SB_LINEDOWN: *pos += px( 40 ); break;
    case SB_PAGEUP: *pos -= si.nPage; break;
    case SB_PAGEDOWN: *pos += si.nPage; break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION: *pos = si.nTrackPos; break;
    case SB_TOP: *pos = 0; break;
    case SB_BOTTOM: *pos = si.nMax; break;
    }
    layout();
    invalidate();
}

static LRESULT CALLBACK canvas_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    POINT pt = { GET_X( lparam ), GET_Y( lparam ) };

    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint( hwnd, &ps );
        paint( hdc, &ps.rcPaint );
        EndPaint( hwnd, &ps );
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        layout();
        invalidate();
        return 0;
    case WM_HSCROLL:
        scroll( SB_HORZ, LOWORD( wparam ) );
        return 0;
    case WM_VSCROLL:
        scroll( SB_VERT, LOWORD( wparam ) );
        return 0;
    case WM_MOUSEWHEEL:
    {
        int delta = (short)HIWORD( wparam );
        if (LOWORD( wparam ) & MK_CONTROL)
        {
            ScreenToClient( hwnd, &pt );
            canvas_zoom_step( delta > 0 ? 1 : -1, &pt );
        }
        else
        {
            if (LOWORD( wparam ) & MK_SHIFT) scroll_x -= delta * px( 60 ) / WHEEL_DELTA;
            else scroll_y -= delta * px( 60 ) / WHEEL_DELTA;
            layout();
            invalidate();
        }
        return 0;
    }
    case WM_MOUSEHWHEEL:
        scroll_x += (short)HIWORD( wparam ) * px( 60 ) / WHEEL_DELTA;
        layout();
        invalidate();
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        if (drag) return 0;
        mouse_down( pt, msg == WM_RBUTTONDOWN, FALSE );
        return 0;
    case WM_LBUTTONDBLCLK:
        if (shape.active && shape.kind == SH_POLYGON && shape.stage == 1) mouse_down( pt, FALSE, TRUE );
        else if (!drag) mouse_down( pt, FALSE, FALSE );
        return 0;
    case WM_MBUTTONDOWN:
        drag = D_PAN;
        drag_screen = pt;
        drag_scroll_x = scroll_x;
        drag_scroll_y = scroll_y;
        SetCapture( hwnd );
        return 0;
    case WM_MOUSEMOVE:
    {
        TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, hwnd };
        TrackMouseEvent( &track );
        mouse_move( pt, wparam );
        return 0;
    }
    case WM_MOUSELEAVE:
        cursor_in = FALSE;
        if (tools.tool == TOOL_ERASER) invalidate();
        status_refresh();
        return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
        if (drag) mouse_up( pt );
        return 0;
    case WM_CAPTURECHANGED:
        if (drag && (HWND)lparam != hwnd)
        {
            POINT p;
            GetCursorPos( &p );
            ScreenToClient( hwnd, &p );
            mouse_up( p );
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD( lparam ) == HTCLIENT)
        {
            GetCursorPos( &pt );
            ScreenToClient( hwnd, &pt );
            return set_cursor( pt );
        }
        break;
    case WM_TIMER:
        if (wparam == 1 && text.active)
        {
            text.caret_on = !text.caret_on;
            invalidate();
        }
        else if (wparam == 2 && drag == D_DRAW && tools.tool == TOOL_BRUSH)
        {
            op_brush_segment( pic, tools.brush, last_fx, last_fy, last_fx, last_fy,
                              drag_button ? tools.color2 : tools.color1, tools.size[TOOL_BRUSH], &seed );
            pic_changed();
        }
        return 0;
    case WM_KEYDOWN:
        if (text.active)
        {
            text_key( wparam );
            return 0;
        }
        if (wparam == VK_SPACE && !space_down)
        {
            space_down = TRUE;
            GetCursorPos( &pt );
            ScreenToClient( hwnd, &pt );
            set_cursor( pt );
            return 0;
        }
        if (wparam == VK_DELETE)
        {
            if (shape.active) canvas_cancel();
            else canvas_delete_selection();
            return 0;
        }
        if (wparam == VK_ESCAPE)
        {
            if (shape.active && shape.kind == SH_POLYGON && shape.stage == 1 && shape.count >= 3) shape_commit();
            else if (sel.active && !sel.floating) { sel_free(); invalidate(); }
            else canvas_commit();
            return 0;
        }
        if (wparam == VK_RETURN && (shape.active || sel.active))
        {
            canvas_commit();
            return 0;
        }
        if ((sel.active || shape.active) && (wparam == VK_LEFT || wparam == VK_RIGHT || wparam == VK_UP || wparam == VK_DOWN))
        {
            int dx = wparam == VK_LEFT ? -1 : wparam == VK_RIGHT ? 1 : 0, dy = wparam == VK_UP ? -1 : wparam == VK_DOWN ? 1 : 0;
            if (shape.active)
            {
                OffsetRect( &shape.box, dx, dy );
                for (int i = 0; i < shape.count; i++) { shape.pts[i].x += dx; shape.pts[i].y += dy; }
                shape_update_layer();
            }
            else
            {
                sel_lift();
                OffsetRect( &sel.r, dx, dy );
            }
            invalidate();
            return 0;
        }
        break;
    case WM_KEYUP:
        if (wparam == VK_SPACE) space_down = FALSE;
        break;
    case WM_CHAR:
        if (text.active) text_char( (WCHAR)wparam );
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        if (text.active) invalidate();
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

HWND canvas_create( HWND parent )
{
    WNDCLASSW cls = { CS_DBLCLKS, canvas_proc, 0, 0, instance, NULL, NULL, NULL, NULL, L"ArcticPaintCanvas" };

    RegisterClassW( &cls );
    canvas = CreateWindowExW( 0, cls.lpszClassName, NULL, WS_CHILD | WS_VISIBLE | WS_HSCROLL | WS_VSCROLL | WS_CLIPSIBLINGS,
                              0, 0, 10, 10, parent, NULL, instance, NULL );
    return canvas;
}

void canvas_refresh(void)
{
    invalidate();
}

void canvas_theme_changed(void)
{
    invalidate();
}
