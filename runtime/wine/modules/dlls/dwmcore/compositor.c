/*
 * Arctic desktop composition engine: the compositor
 *
 * Serves window buffers over the Wayland wire protocol on
 * $XDG_RUNTIME_DIR/arctic-0 and composes them on the CPU into the KMS
 * framebuffer of every monitor. This step speaks the standard protocols
 * Wine's own Wayland driver needs (wl_compositor, wl_shm, wl_subcompositor,
 * xdg_wm_base, wp_viewporter, wl_output, xdg_output), plus arctic_shell_v1,
 * through which a client says which Win32 window a surface shows, and
 * arctic_display_v1, through which it reads the EDID of a monitor and
 * changes modes. Where those windows are, whether they are visible and how
 * they are stacked comes from wineserver (patch 0001, read by the PE side),
 * never from the client. Surfaces without a window keep a simple cascade.
 *
 * Buffers in shared memory are copied at commit and released at once, so
 * clients never wait on the compositor. GPU buffers (zwp_linux_dmabuf_v1,
 * dmabuf.c) are kept: a window that fills a monitor with one, as a game does
 * full screen, is flipped to by the monitor directly, with variable refresh
 * when the monitor has it; otherwise it is read when it is composed.
 *
 * A frame is made when something changed and the monitor has shown the last
 * one (the page flip finished), so each monitor is paced by its own refresh,
 * or, with variable refresh, by the game.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-server.h>

#include "xdg-shell-server-protocol.h"
#include "viewporter-server-protocol.h"
#include "xdg-output-unstable-v1-server-protocol.h"
#include "linux-dmabuf-v1-server-protocol.h"
#include "arctic-shell-v1-server-protocol.h"
#include "arctic-display-v1-server-protocol.h"

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "winbase.h"
#include "winuser.h"
#include "wine/debug.h"

#include "compositor.h"

WINE_DEFAULT_DEBUG_CHANNEL(dwm);

#define SOCKET_NAME "arctic-0"

static struct wl_display      *display;
static struct wl_event_source *frame_timer;
struct wl_list                 all_surfaces;
static struct wl_list          toplevels;
static struct wl_list          outputs;      /* struct output */
static struct wl_list          dying_outputs;  /* unplugged, until their clients let go */
static struct wl_list          parked_outputs; /* of a card that went away, until the next one shows */
static bool                    parking;      /* the card is being replaced */
static uint32_t                parked_expire;  /* when the parked outputs go, 0 if not yet known */
uint32_t                       background;   /* XRGB */
static uint32_t               *shadow;       /* frame composed in RAM, then copied out */
static RECT                    screen;       /* the virtual screen every monitor is placed in */
static bool                    dirty = true;
static int                     cascade;
struct dwm_window             *windows;      /* from wineserver, topmost first */
uint32_t                       window_count;
int                            cursor_x, cursor_y;
bool                           cursor_hidden;  /* the window under it hides it */
static uint32_t                pending_events;  /* DWM_EVENT_*, for the PE side */
static struct wl_list          window_states;  /* struct window_state */
static bool                    windows_known;  /* the first list came: windows that show later animate */
static struct arctic_dwm_entry *attributes;  /* from dwmapi */
static uint32_t                attribute_count;
static bool                    animating;    /* frames keep coming until every animation ends */

/* Aero Snap: where a window dragged to a screen edge would go */
#define SNAP_GROW_MS 200
#define SNAP_FADE_MS 150
static uint32_t                snap_hwnd;    /* the window it is shown for, 0 once it goes */
static struct frect            snap_from, snap_to;  /* it grows from the pointer to where the window would go */
static uint64_t                snap_start;   /* µs, 0: not shown */
static bool                    snap_leaving; /* it fades away */
static bool                    vrr_allowed = true;  /* the Windows setting */
static uint32_t                vrr_off[DWM_MAX_OUTPUTS];  /* connectors it is turned off for */
static uint32_t                vrr_off_count;
static bool                    hw_cursor;    /* the card has a cursor plane for directly shown windows */
static uint64_t                last_callbacks;  /* µs, when clients were last told a frame was shown */

/* something changed: every monitor gets a new frame */
static void damage(void);

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void resource_destroy( struct wl_client *client, struct wl_resource *resource )
{
    wl_resource_destroy( resource );
}

static struct surface *get_surface( struct wl_resource *resource )
{
    return resource ? wl_resource_get_user_data( resource ) : NULL;
}

/**********************************************************************
 *          Composition
 */

/* src is premultiplied */
static uint32_t over( uint32_t src, uint32_t dst )
{
    uint32_t a = src >> 24, out = 0;
    for (int shift = 0; shift <= 16; shift += 8)
    {
        uint32_t s = (src >> shift) & 0xff, d = (dst >> shift) & 0xff;
        uint32_t c = s + d * (255 - a) / 255;
        out |= (c > 255 ? 255 : c) << shift;
    }
    return out;
}

/* a GDI pixel over a backdrop: without alpha of its own, it counts by its
 * brightness, so that black shows the backdrop and white text stays white */
static uint32_t keyed( uint32_t p )
{
    uint32_t a = p >> 24, r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, b = p & 0xff;

    if (!a) a = max( r, max( g, b ) );
    return (a << 24) | (min( r, a ) << 16) | (min( g, a ) << 8) | min( b, a );
}

/* the part of the buffer shown (viewport source) scaled to the viewport destination */
static void blit( const struct surface *s, int x0, int y0, enum layer_mode mode )
{
    int sx0 = 0, sy0 = 0, sw = s->width, sh = s->height, dw, dh;

    if (s->src_width > 0 && s->src_height > 0)
    {
        sx0 = s->src_x;
        sy0 = s->src_y;
        sw = s->src_width < s->width - sx0 ? s->src_width : s->width - sx0;
        sh = s->src_height < s->height - sy0 ? s->src_height : s->height - sy0;
        if (sw <= 0 || sh <= 0) return;
    }
    dw = s->dst_width ? s->dst_width : sw;
    dh = s->dst_height ? s->dst_height : sh;

    for (int y = 0; y < dh; y++)
    {
        int py = y0 + y;
        const uint32_t *src;
        uint32_t *dst;

        if (py < screen.top || py >= screen.bottom) continue;
        src = s->pixels + (size_t)(sy0 + (dh == sh ? y : sh * y / dh)) * s->width + sx0;
        dst = shadow + (size_t)(py - screen.top) * (screen.right - screen.left) - screen.left;
        for (int x = 0; x < dw; x++)
        {
            int px = x0 + x;
            uint32_t p;

            if (px < screen.left || px >= screen.right) continue;
            p = src[dw == sw ? x : sw * x / dw];
            if (mode == LAYER_KEYED && !s->alpha) p = keyed( p );
            if (mode == LAYER_OPAQUE || (p >> 24) == 0xff) dst[px] = p | 0xff000000;
            else if (p >> 24) dst[px] = over( p, dst[px] );
        }
    }
}

/* a GPU buffer is read by the CPU only when it is composed */
static void read_dmabuf( struct surface *s )
{
    s->stale = false;
    if (!s->pixels && !(s->pixels = malloc( (size_t)s->width * s->height * 4 ))) return;
    if (!dmabuf_read( s->dmabuf, s->pixels )) memset( s->pixels, 0, (size_t)s->width * s->height * 4 );
}

static void draw_tree( struct surface *s, int x, int y, bool backdrop )
{
    struct surface *child;

    if (s->dmabuf && s->stale) read_dmabuf( s );
    if (s->pixels) blit( s, x, y, backdrop ? LAYER_KEYED : s->alpha ? LAYER_PREMULTIPLIED : LAYER_OPAQUE );
    wl_list_for_each( child, &s->children, child_link )
    {
        if (child->hwnd) continue; /* a window of its own: placed by wineserver, not by its parent */
        draw_tree( child, x + child->sub_x, y + child->sub_y, backdrop );
    }
}

/* the standard arrow, until cursor shapes come from win32u */
static const char arrow[20][13] =
{
    "B           ",
    "BB          ",
    "BWB         ",
    "BWWB        ",
    "BWWWB       ",
    "BWWWWB      ",
    "BWWWWWB     ",
    "BWWWWWWB    ",
    "BWWWWWWWB   ",
    "BWWWWWWWWB  ",
    "BWWWWWWWWWB ",
    "BWWWWWWBBBBB",
    "BWWWBWWB    ",
    "BWWB BWWB   ",
    "BWB  BWWB   ",
    "BB    BWWB  ",
    "B     BWWB  ",
    "       BWWB ",
    "       BWWB ",
    "        BB  ",
};

/* its outline, in the pixels of the 100 % arrow above: at a larger scale
 * the arrow is drawn from it, smooth, with a border as wide as the scale,
 * as Windows 10 picks the larger pointer */
static const struct { float x, y; } arrow_outline[] =
{
    { 0, 0 }, { 12, 12 }, { 8, 12 }, { 11, 18.6f }, { 9.6f, 20 }, { 8.2f, 20 }, { 4.8f, 12.6f }, { 0, 17.4f },
};

struct cursor_image cursor_image;
static float cursor_scale;

static float monitor_scale( const struct dwm_window *w );
static const struct dwm_window *taskbar_window(void);

static bool in_arrow( float x, float y, float *border )
{
    bool inside = false;
    float d = 1e9f;

    for (size_t i = 0, j = ARRAY_SIZE(arrow_outline) - 1; i < ARRAY_SIZE(arrow_outline); j = i++)
    {
        float ax = arrow_outline[j].x, ay = arrow_outline[j].y, bx = arrow_outline[i].x, by = arrow_outline[i].y;
        float dx = bx - ax, dy = by - ay, t, ex, ey;

        if ((ay > y) != (by > y) && x < ax + (y - ay) * dx / dy) inside = !inside;
        t = ((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy);
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        ex = ax + t * dx - x;
        ey = ay + t * dy - y;
        d = min( d, ex * ex + ey * ey );
    }
    *border = d;
    return inside;
}

static void make_cursor_image( float scale )
{
    struct cursor_image *img = &cursor_image;

    memset( img->pixels, 0, sizeof(img->pixels) );
    if (scale <= 1.0f)
    {
        img->width = 12;
        img->height = ARRAY_SIZE(arrow);
        for (int y = 0; y < img->height; y++)
            for (int x = 0; arrow[y][x]; x++)
                if (arrow[y][x] != ' ') img->pixels[y * img->width + x] = arrow[y][x] == 'B' ? 0xff000000 : 0xffffffff;
    }
    else
    {
        img->width = min( CURSOR_MAX_W, (int)(12 * scale + 0.999f) + 1 );
        img->height = min( CURSOR_MAX_H, (int)(20 * scale + 0.999f) + 1 );
        for (int y = 0; y < img->height; y++)
            for (int x = 0; x < img->width; x++)
            {
                unsigned int total = 0, white = 0, a, g;

                /* 4 x 4 samples a pixel: black within one arrow pixel of the outline, white inside */
                for (int sy = 0; sy < 4; sy++)
                    for (int sx = 0; sx < 4; sx++)
                    {
                        float border;

                        if (!in_arrow( (x + (sx + 0.5f) / 4) / scale, (y + (sy + 0.5f) / 4) / scale, &border )) continue;
                        total++;
                        if (border >= 1.0f) white++;
                    }
                a = total * 255 / 16;
                g = white * 255 / 16;
                img->pixels[y * img->width + x] = (a << 24) | (g << 16) | (g << 8) | g;
            }
    }
    img->serial++;
    hw_cursor = kms_set_cursor_image( img->pixels, img->width, img->height );
}

/* the scale of the monitor the pointer is on: the topmost window there knows it */
void update_cursor_image(void)
{
    float scale = 0;

    for (uint32_t i = 0; i < window_count && !scale; i++)
    {
        const struct dwm_window *w = &windows[i];

        if (!(w->style & WS_VISIBLE) || !w->raw_dpi) continue;
        if (cursor_x >= w->left && cursor_x < w->right && cursor_y >= w->top && cursor_y < w->bottom)
            scale = monitor_scale( w );
    }
    if (!scale) scale = monitor_scale( taskbar_window() );
    scale = max( 1.0f, min( scale, 5.0f ) );
    if (scale == cursor_scale && cursor_image.width) return;
    cursor_scale = scale;
    make_cursor_image( scale );
}

static void set_cursor_image(void)
{
    cursor_scale = 0;
    update_cursor_image();
}

static void draw_cursor(void)
{
    const struct cursor_image *img = &cursor_image;

    if (cursor_hidden) return;
    update_cursor_image();
    for (int y = 0; y < img->height; y++)
    {
        int py = cursor_y + y;

        if (py < screen.top || py >= screen.bottom) continue;
        for (int x = 0; x < img->width; x++)
        {
            int px = cursor_x + x;
            uint32_t src = img->pixels[y * img->width + x], *dst, a;

            if (px < screen.left || px >= screen.right || !src) continue;
            dst = &shadow[(size_t)(py - screen.top) * (screen.right - screen.left) + px - screen.left];
            if ((a = src >> 24) == 0xff) *dst = src;
            else
            {
                uint32_t r = ((*dst >> 16) & 0xff) * (255 - a) / 255, g = ((*dst >> 8) & 0xff) * (255 - a) / 255,
                         b = (*dst & 0xff) * (255 - a) / 255;
                *dst = src + ((r << 16) | (g << 8) | b);
            }
        }
    }
}

/* frames are composed as often as the fastest monitor shows them */
static uint32_t screen_refresh_hz(void)
{
    uint32_t hz = 0;

    for (int i = 0; i < KMS_MAX_OUTPUTS; i++)
    {
        struct kms_output *o = &kms.outputs[i];
        uint32_t refresh;

        if (!o->connector_id || !o->enabled) continue;
        refresh = (o->modes[o->mode].refresh + 500) / 1000;
        if (refresh > hz) hz = refresh;
    }
    return hz ? hz : 60;
}

/* the rectangle every monitor stands in, and the buffer frames are composed in */
static bool update_screen(void)
{
    RECT rect = { INT_MAX, INT_MAX, INT_MIN, INT_MIN };
    uint32_t *buffer;

    for (int i = 0; i < KMS_MAX_OUTPUTS; i++)
    {
        struct kms_output *o = &kms.outputs[i];

        if (!o->connector_id || !o->enabled) continue;
        rect.left = min( rect.left, o->x );
        rect.top = min( rect.top, o->y );
        rect.right = max( rect.right, o->x + (int32_t)o->fbs[0].width );
        rect.bottom = max( rect.bottom, o->y + (int32_t)o->fbs[0].height );
    }
    if (rect.left > rect.right) return false;
    if (!memcmp( &rect, &screen, sizeof(rect) )) return true;

    if (!(buffer = realloc( shadow, (size_t)(rect.right - rect.left) * (rect.bottom - rect.top) * 4 )))
    {
        ERR( "no memory for a %dx%d screen\n", (int)(rect.right - rect.left), (int)(rect.bottom - rect.top) );
        return false;
    }
    shadow = buffer;
    screen = rect;
    damage();
    return true;
}

static struct surface *surface_for_hwnd( uint32_t hwnd )
{
    struct surface *s;

    wl_list_for_each( s, &all_surfaces, all_link )
        if (s->hwnd == hwnd) return s;
    return NULL;
}

struct surface *surface_for_window( uint32_t hwnd )
{
    return surface_for_hwnd( hwnd );
}

static void damage(void)
{
    struct output *output;

    dirty = true;
    wl_list_for_each( output, &outputs, link ) output->needs_frame = true;
}

static bool has_content( const struct surface *s )
{
    return s->pixels || s->dmabuf;
}

/* a window a monitor shows directly is not composed: no other monitor shows it */
static bool shown_directly( uint32_t hwnd )
{
    struct output *output;

    wl_list_for_each( output, &outputs, link ) if (output->direct_hwnd == hwnd) return true;
    return false;
}

bool window_is_shown_directly( uint32_t hwnd )
{
    return shown_directly( hwnd );
}

/* A window: its own surface, then what other surfaces show in it. Those
 * come from another process than the window's (a browser's GPU process
 * draws into the browser's window), so they cannot be subsurfaces of it;
 * dwm places them where wineserver has the window, as DWM does on Windows. */
static void for_each_window_surface( const struct dwm_window *w,
                                     void (*callback)( struct surface *s, int x, int y, void *ctx ), void *ctx )
{
    struct surface *s;

    wl_list_for_each( s, &all_surfaces, all_link )
        if (s->hwnd == w->hwnd && s->xdg_toplevel) callback( s, w->left, w->top, ctx );
    wl_list_for_each( s, &all_surfaces, all_link )
        if (s->hwnd == w->hwnd && s->subsurface) callback( s, w->left, w->top, ctx );
    /* what another process draws (Vulkan, Direct3D) has no role, and covers
     * the client area */
    wl_list_for_each( s, &all_surfaces, all_link )
        if (s->hwnd == w->hwnd && !s->xdg_toplevel && !s->subsurface) callback( s, w->client_left, w->client_top, ctx );
}

void scene_surfaces( const struct window_draw *draw,
                     void (*callback)( struct surface *s, int x, int y, void *ctx ), void *ctx )
{
    for_each_window_surface( draw->w, callback, ctx );
}

/**********************************************************************
 *          Window effects
 *
 * What DWM of Windows 11 does to windows: rounded corners (here half the
 * Windows 11 radius), a blurred backdrop for windows that ask for one (the
 * taskbar's acrylic), live thumbnails, and animations when a window opens,
 * closes, is minimized, restored or maximized, and when a menu or a flyout
 * shows, on the Windows 11 motion curves. They are drawn on the GPU; the
 * composition on the CPU keeps the backdrop's tint and the thumbnails.
 */

#define CORNER_RADIUS 4

/* the unix side has no user32 */
static void set_rect( RECT *rect, int left, int top, int right, int bottom )
{
    rect->left = left;
    rect->top = top;
    rect->right = right;
    rect->bottom = bottom;
}

static bool rect_equal( const RECT *a, const RECT *b )
{
    return a->left == b->left && a->top == b->top && a->right == b->right && a->bottom == b->bottom;
}

/* y of the cubic Bezier through (0,0), (x1,y1), (x2,y2), (1,1) at x */
static float bezier( float x1, float y1, float x2, float y2, float x )
{
    float lo = 0, hi = 1, t = x, u;

    for (int i = 0; i < 24; i++)
    {
        float bx;

        u = 1 - t;
        bx = 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t;
        if (bx < x) lo = t;
        else hi = t;
        t = (lo + hi) / 2;
    }
    u = 1 - t;
    return 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t;
}

/* things coming in slow down, things going out speed up */
static float decelerate( float x ) { return bezier( 0.1f, 0.9f, 0.2f, 1.0f, x ); }
static float accelerate( float x ) { return bezier( 0.7f, 0.0f, 1.0f, 0.5f, x ); }

struct window_state *window_state( uint32_t hwnd )
{
    struct window_state *ws;

    wl_list_for_each( ws, &window_states, link ) if (ws->hwnd == hwnd) return ws;
    return NULL;
}

/* what dwmapi was told about the window */
static void apply_attributes( struct window_state *ws )
{
    ws->has_attr = false;
    for (uint32_t i = 0; i < attribute_count; i++)
    {
        if (attributes[i].type != ARCTIC_DWM_WINDOW || attributes[i].hwnd != ws->hwnd) continue;
        ws->attr = attributes[i].u.window;
        ws->has_attr = true;
        return;
    }
}

static struct window_state *get_window_state( uint32_t hwnd )
{
    struct window_state *ws = window_state( hwnd );

    if (ws || !(ws = calloc( 1, sizeof(*ws) ))) return ws;
    ws->hwnd = hwnd;
    wl_list_insert( &window_states, &ws->link );
    apply_attributes( ws );
    return ws;
}

static void free_window_state( struct window_state *ws )
{
    gl_image_free( &ws->snapshot );
    gl_image_free( &ws->scratch );
    wl_list_remove( &ws->link );
    free( ws );
}

static bool has_caption( const struct dwm_window *w )
{
    return (w->style & WS_CAPTION) == WS_CAPTION;
}

static bool monitor_rect( int x, int y, RECT *rect )
{
    for (int i = 0; i < KMS_MAX_OUTPUTS; i++)
    {
        const struct kms_output *o = &kms.outputs[i];
        const struct kms_mode *mode;

        if (!o->connector_id || !o->enabled) continue;
        mode = &o->modes[o->mode];
        if (x < o->x || y < o->y || x >= o->x + mode->info.hdisplay || y >= o->y + mode->info.vdisplay) continue;
        set_rect(rect, o->x, o->y, o->x + mode->info.hdisplay, o->y + mode->info.vdisplay );
        return true;
    }
    return false;
}

/* a game or a video full screen, or the desktop itself */
static bool covers_monitor( const struct dwm_window *w )
{
    RECT rect;

    return monitor_rect( w->left, w->top, &rect ) && w->left <= rect.left && w->top <= rect.top &&
           w->right >= rect.right && w->bottom >= rect.bottom;
}

/* nine tenths of the monitor or more, both ways */
static bool nearly_covers_monitor( const struct dwm_window *w )
{
    RECT rect;

    if (!monitor_rect( (w->left + w->right) / 2, (w->top + w->bottom) / 2, &rect )) return false;
    return (w->right - w->left) * 10 >= (rect.right - rect.left) * 9 &&
           (w->bottom - w->top) * 10 >= (rect.bottom - rect.top) * 9;
}

static bool is_cloaked( const struct window_state *ws )
{
    return ws && ws->has_attr && ws->attr.cloaked;
}

static bool window_shown( const struct dwm_window *w, const struct window_state *ws )
{
    return (w->style & WS_VISIBLE) && !(w->style & WS_MINIMIZE) && !(w->style & WS_CHILD) && !is_cloaked( ws );
}

/* the rectangles of the window list are in the pixels of the monitor; what
 * a program gives in its own coordinates (thumbnails, its taskbar button)
 * grows by its scale, and what DWM draws of its own by the monitor's */
static float window_scale( const struct dwm_window *w )
{
    return w && w->dpi && w->raw_dpi ? (float)w->raw_dpi / w->dpi : 1.0f;
}

static float monitor_scale( const struct dwm_window *w )
{
    return w && w->raw_dpi ? w->raw_dpi / 96.0f : 1.0f;
}

/* Windows 11 rounds a window with a frame unless it is maximized or full
 * screen, or the program said not to (DWMWA_WINDOW_CORNER_PREFERENCE) */
static float corner_radius( const struct dwm_window *w, const struct window_state *ws )
{
    UINT32 corner = ws && ws->has_attr ? ws->attr.corner : 0;
    float radius = CORNER_RADIUS * monitor_scale( w );

    if (w->style & (WS_CHILD | WS_MAXIMIZE | WS_MINIMIZE)) return 0;
    if (corner == 1 /* DWMWCP_DONOTROUND */ || covers_monitor( w )) return 0;
    if (corner == 3 /* DWMWCP_ROUNDSMALL */) return radius / 2;
    if (corner == 2 /* DWMWCP_ROUND */) return radius;
    return has_caption( w ) || (w->style & WS_THICKFRAME) ? radius : 0;
}

/* Windows 11 puts a soft shadow under windows with a frame and under menus,
 * and outlines them with a thin line */
static bool window_decorated( const struct dwm_window *w, float radius )
{
    if (w->style & (WS_CHILD | WS_MAXIMIZE | WS_MINIMIZE)) return false;
    return radius > 0 || w->class_atom == 0x8000;
}

/* The backdrop a window asked for: SetWindowCompositionAttribute's accent
 * (the taskbar's acrylic) or DwmEnableBlurBehindWindow. tint is ARGB. */
static bool window_backdrop( const struct window_state *ws, bool *blur, uint32_t *tint )
{
    uint32_t c;

    if (!ws || !ws->has_attr) return false;
    c = ws->attr.accent_color;  /* 0xAABBGGRR */
    c = (c & 0xff00ff00) | ((c >> 16) & 0xff) | ((c & 0xff) << 16);
    switch (ws->attr.accent)
    {
    case ARCTIC_ACCENT_ENABLE_ACRYLICBLURBEHIND:
    case ARCTIC_ACCENT_ENABLE_BLURBEHIND:
        *blur = true;
        *tint = c;
        return true;
    case ARCTIC_ACCENT_ENABLE_TRANSPARENTGRADIENT:
        *blur = false;
        *tint = c;
        return true;
    case ARCTIC_ACCENT_ENABLE_GRADIENT:
        *blur = false;
        *tint = c | 0xff000000;
        return true;
    }
    if (!ws->attr.blur_behind) return false;
    *blur = true;
    *tint = 0;
    return true;
}

static enum layer_mode layer_mode( const struct surface *s, const struct window_draw *draw )
{
    if (s->alpha) return LAYER_PREMULTIPLIED;
    return draw && draw->backdrop ? LAYER_KEYED : LAYER_OPAQUE;
}

enum transition
{
    TRANSITION_NONE,
    TRANSITION_WINDOW,                       /* zoom and fade */
    TRANSITION_POPUP,                        /* fade, a little slide down */
    TRANSITION_SLIDE,                        /* up from below */
    TRANSITION_FADE,
};

static enum transition window_transition( const struct dwm_window *w, const struct window_state *ws )
{
    if ((w->style & WS_CHILD) || w->band) return TRANSITION_NONE;
    if (ws->has_attr)
    {
        if (ws->attr.transitions_off) return TRANSITION_NONE;
        switch (ws->attr.transition)
        {
        case ARCTIC_TRANSITION_NONE: return TRANSITION_NONE;
        case ARCTIC_TRANSITION_FADE: return TRANSITION_FADE;
        case ARCTIC_TRANSITION_SLIDE_UP: return TRANSITION_SLIDE;
        }
    }
    /* layered windows (the toasts) animate themselves */
    if ((w->ex_style & WS_EX_LAYERED) || covers_monitor( w )) return TRANSITION_NONE;
    if (has_caption( w )) return TRANSITION_WINDOW;
    /* menus and flyouts, not the desktop in the work area */
    if ((w->style & WS_POPUP) && !nearly_covers_monitor( w )) return TRANSITION_POPUP;
    return TRANSITION_NONE;
}

static struct frect frect_from( int left, int top, int right, int bottom )
{
    struct frect r = { left, top, right, bottom };
    return r;
}

static struct frect window_frect( const struct dwm_window *w )
{
    return frect_from( w->left, w->top, w->right, w->bottom );
}

static struct frect zoomed( struct frect r, float scale )
{
    float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    struct frect z = { cx + (r.left - cx) * scale, cy + (r.top - cy) * scale,
                       cx + (r.right - cx) * scale, cy + (r.bottom - cy) * scale };
    return z;
}

static struct frect moved( struct frect r, float dx, float dy )
{
    struct frect m = { r.left + dx, r.top + dy, r.right + dx, r.bottom + dy };
    return m;
}

/* where a minimized window goes: its taskbar button, which the taskbar
 * names (DWMWA_ARCTIC_MINIMIZE_RECT), else the middle of the bottom edge */
/* the taskbar: the window in the band over program windows */
static const struct dwm_window *taskbar_window(void)
{
    for (uint32_t i = 0; i < window_count; i++) if (windows[i].band) return &windows[i];
    return NULL;
}

static struct frect minimize_target( const struct window_state *ws, const struct dwm_window *w )
{
    float s = monitor_scale( w );
    RECT rect;

    if (ws->has_attr && ws->attr.minimize_right > ws->attr.minimize_left &&
        ws->attr.minimize_bottom > ws->attr.minimize_top)
    {
        /* the button's rectangle is in the taskbar's coordinates */
        float ts = window_scale( taskbar_window() );
        return frect_from( ws->attr.minimize_left * ts, ws->attr.minimize_top * ts, ws->attr.minimize_right * ts,
                           ws->attr.minimize_bottom * ts );
    }
    if (!monitor_rect( (w->left + w->right) / 2, (w->top + w->bottom) / 2, &rect ) &&
        !monitor_rect( 0, 0, &rect ))
        set_rect(&rect, 0, 0, 1024, 768 );
    return frect_from( (rect.left + rect.right) / 2 - 24 * s, rect.bottom - 40 * s,
                       (rect.left + rect.right) / 2 + 24 * s, rect.bottom );
}

static void animate( struct window_state *ws, enum animation anim, struct frect from, struct frect to,
                     float alpha_from, float alpha_to, uint32_t ms, bool ghost )
{
    if (!gl_active()) return;  /* the CPU would not keep up */
    TRACE( "%08x: animation %d for %u ms%s\n", ws->hwnd, anim, ms, ghost ? ", from its last frame" : "" );
    ws->anim = anim;
    ws->anim_from = from;
    ws->anim_to = to;
    ws->alpha_from = alpha_from;
    ws->alpha_to = alpha_to;
    ws->anim_duration = ms * 1000;
    ws->ghost = ghost;
    ws->anim_created = kms_now();
    /* a window coming in waits for its first frame; one going out starts now */
    ws->anim_start = ghost ? ws->anim_created : 0;
    animating = true;
}

static void animate_in( struct window_state *ws, const struct dwm_window *w )
{
    struct frect r = window_frect( w );

    switch (window_transition( w, ws ))
    {
    case TRANSITION_NONE: break;
    case TRANSITION_WINDOW: animate( ws, ANIM_OPEN, zoomed( r, 0.94f ), r, 0, 1, 250, false ); break;
    case TRANSITION_POPUP: animate( ws, ANIM_POPUP_IN, moved( r, 0, -8 * monitor_scale( w ) ), r, 0, 1, 167, false ); break;
    case TRANSITION_SLIDE: animate( ws, ANIM_SLIDE_IN, moved( r, 0, 48 * monitor_scale( w ) ), r, 0, 1, 250, false ); break;
    case TRANSITION_FADE: animate( ws, ANIM_FADE_IN, r, r, 0, 1, 150, false ); break;
    }
}

/* a window that went is drawn from what it showed last, if that was kept */
static void animate_out( struct window_state *ws, const struct dwm_window *w )
{
    struct frect r = window_frect( w );
    enum transition transition = window_transition( w, ws );

    if (transition == TRANSITION_NONE) return;
    if (!ws->snapshot_valid || !rect_equal(&ws->snapshot_rect, &(RECT){ w->left, w->top, w->right, w->bottom } ))
        gl_snapshot( ws, w );
    if (!ws->snapshot_valid)
    {
        TRACE( "%08x went without a last frame\n", ws->hwnd );
        return;
    }
    r = frect_from( ws->snapshot_rect.left, ws->snapshot_rect.top, ws->snapshot_rect.right, ws->snapshot_rect.bottom );
    switch (transition)
    {
    case TRANSITION_NONE: break;
    case TRANSITION_WINDOW: animate( ws, ANIM_CLOSE, r, zoomed( r, 0.94f ), 1, 0, 167, true ); break;
    case TRANSITION_POPUP: animate( ws, ANIM_POPUP_OUT, r, r, 1, 0, 100, true ); break;
    case TRANSITION_SLIDE: animate( ws, ANIM_SLIDE_OUT, r, moved( r, 0, 48 ), 1, 0, 167, true ); break;
    case TRANSITION_FADE: animate( ws, ANIM_FADE_OUT, r, r, 1, 0, 100, true ); break;
    }
}

/* What changed between two lists of windows: a window that shows, hides,
 * is minimized, restored, maximized or goes away starts its animation. */
static void update_window_states(void)
{
    static uint32_t serial;
    struct window_state *ws, *next;

    serial++;
    for (uint32_t i = 0; i < window_count; i++)
    {
        const struct dwm_window *w = &windows[i];
        bool shown;

        if (w->style & WS_CHILD) continue;
        if (!(ws = get_window_state( w->hwnd ))) continue;
        ws->list_serial = serial;
        shown = window_shown( w, ws );

        if (!ws->listed)
        {
            if (windows_known && shown) animate_in( ws, w );
        }
        else if (!ws->shown && shown)
        {
            if ((ws->last.style & (WS_MINIMIZE | WS_VISIBLE)) == (WS_MINIMIZE | WS_VISIBLE) &&
                window_transition( w, ws ) == TRANSITION_WINDOW)
            {
                struct frect r = window_frect( w );
                animate( ws, ANIM_RESTORE, minimize_target( ws, w ), r, 0, 1, 250, false );
            }
            else animate_in( ws, w );
        }
        else if (ws->shown && !shown)
        {
            if ((w->style & (WS_MINIMIZE | WS_VISIBLE)) == (WS_MINIMIZE | WS_VISIBLE) &&
                window_transition( &ws->last, ws ) == TRANSITION_WINDOW)
            {
                if (!ws->snapshot_valid || !rect_equal(&ws->snapshot_rect, &(RECT){ ws->last.left, ws->last.top,
                                                                                  ws->last.right, ws->last.bottom } ))
                    gl_snapshot( ws, &ws->last );
                if (ws->snapshot_valid)
                {
                    struct frect r = frect_from( ws->snapshot_rect.left, ws->snapshot_rect.top,
                                                 ws->snapshot_rect.right, ws->snapshot_rect.bottom );
                    animate( ws, ANIM_MINIMIZE, r, minimize_target( ws, &ws->last ), 1, 0, 250, true );
                }
            }
            else animate_out( ws, &ws->last );
        }
        else if (ws->shown && shown && window_transition( w, ws ) == TRANSITION_WINDOW)
        {
            /* Maximized or restored: WS_MAXIMIZE and the new rectangle may
             * come in two lists, the style first */
            bool moved = ws->last.left != w->left || ws->last.top != w->top ||
                         ws->last.right != w->right || ws->last.bottom != w->bottom;
            uint64_t now = kms_now();

            if ((ws->last.style ^ w->style) & WS_MAXIMIZE)
            {
                ws->maximize_time = now;
                ws->maximize_from = window_frect( &ws->last );
                /* what it shows before it redraws at the new size is stretched meanwhile */
                if (!ws->snapshot_valid || !rect_equal( &ws->snapshot_rect, &(RECT){ ws->last.left, ws->last.top,
                                                                                   ws->last.right, ws->last.bottom } ))
                    gl_snapshot( ws, &ws->last );
            }
            if (moved && ws->maximize_time && now - ws->maximize_time < 300000)
            {
                animate( ws, ANIM_MORPH, ws->maximize_from, window_frect( w ), 1, 1, 200, false );
                ws->anim_start = now;
                ws->maximize_time = 0;
            }
        }
        ws->last = *w;
        ws->shown = shown;
        ws->listed = true;
    }

    /* windows that are gone */
    wl_list_for_each_safe( ws, next, &window_states, link )
    {
        if (ws->list_serial == serial || !ws->listed) continue;
        if (ws->shown) animate_out( ws, &ws->last );
        ws->listed = ws->shown = false;
        if (!ws->anim) free_window_state( ws );
    }
    windows_known = true;
}

/* animations that ended; true while some still run */
static bool update_animations( uint64_t now )
{
    struct window_state *ws, *next;
    bool running = false;

    wl_list_for_each_safe( ws, next, &window_states, link )
    {
        if (!ws->anim) continue;
        if (!ws->anim_start)
        {
            /* a window that never drew its first frame is not waited for */
            if (now - ws->anim_created < 400000)
            {
                running = true;
                continue;
            }
            TRACE( "%08x never drew a frame to animate\n", ws->hwnd );
            ws->anim = ANIM_NONE;
        }
        else if (now - ws->anim_start >= ws->anim_duration) ws->anim = ANIM_NONE;
        else running = true;

        if (ws->anim) continue;
        ws->ghost = false;
        if (!ws->listed) free_window_state( ws );
    }

    if (snap_start)
    {
        uint64_t length = (snap_leaving ? SNAP_FADE_MS : SNAP_GROW_MS) * 1000;

        if (now - snap_start < length) running = true;
        else if (snap_leaving) snap_start = snap_hwnd = 0;
    }
    return running;
}

static bool has_content( const struct surface *s );

/* the window's own surface holds a frame of its current size */
static bool window_has_content( const struct dwm_window *w, bool full_size )
{
    struct surface *s;

    wl_list_for_each( s, &all_surfaces, all_link )
    {
        if (s->hwnd != w->hwnd || !has_content( s ) || s->subsurface) continue;
        if (!full_size || !s->xdg_toplevel) return true;
        return s->width == w->right - w->left && s->height == w->bottom - w->top;
    }
    return false;
}

static float lerp( float a, float b, float t )
{
    return a + (b - a) * t;
}

/* what maps one rectangle onto another */
static struct xform xform_between( struct frect base, struct frect shown )
{
    float bw = max( base.right - base.left, 1.0f ), bh = max( base.bottom - base.top, 1.0f );
    struct xform x;

    x.sx = (shown.right - shown.left) / bw;
    x.sy = (shown.bottom - shown.top) / bh;
    x.tx = shown.left - base.left * x.sx;
    x.ty = shown.top - base.top * x.sy;
    return x;
}

static void set_draw_geometry( struct window_draw *draw, struct frect base, struct frect shown )
{
    draw->shown = shown;
    draw->xform = xform_between( base, shown );
    draw->clip.rect = shown;
}

/* the window stands on a monitor, not where Wine parks minimized ones */
static bool on_screen( const struct dwm_window *w )
{
    RECT rect;
    return monitor_rect( (w->left + w->right) / 2, (w->top + w->bottom) / 2, &rect );
}

/* the snap preview now: it grows from the pointer, and fades once the drag
 * leaves the edge or ends */
static bool snap_frame( uint64_t now, struct frect *rect, float *alpha )
{
    float t, e;

    if (!snap_start) return false;
    t = min( 1.0f, (float)(now - snap_start) / ((snap_leaving ? SNAP_FADE_MS : SNAP_GROW_MS) * 1000) );
    if (snap_leaving)
    {
        *rect = snap_to;
        *alpha = 1 - accelerate( t );
        return t < 1;
    }
    e = decelerate( t );
    rect->left = lerp( snap_from.left, snap_to.left, e );
    rect->top = lerp( snap_from.top, snap_to.top, e );
    rect->right = lerp( snap_from.right, snap_to.right, e );
    rect->bottom = lerp( snap_from.bottom, snap_to.bottom, e );
    *alpha = min( 1.0f, t * 3 );
    return true;
}

static void update_snap( const struct dwm_set_windows_params *params )
{
    uint64_t now = kms_now();
    struct frect current;
    float alpha;

    if (!gl_active()) return;
    if (params->snap_window)
    {
        struct frect to = frect_from( params->snap_left, params->snap_top, params->snap_right, params->snap_bottom );

        if (snap_start && !snap_leaving && snap_hwnd == params->snap_window && !memcmp( &to, &snap_to, sizeof(to) ))
            return;
        /* from where it stands, or from the pointer */
        if (snap_start && !snap_leaving && snap_frame( now, &current, &alpha )) snap_from = current;
        else snap_from = frect_from( cursor_x - 8, cursor_y - 8, cursor_x + 8, cursor_y + 8 );
        snap_to = to;
        snap_hwnd = params->snap_window;
        snap_leaving = false;
        snap_start = now;
        animating = true;
    }
    else if (snap_start && !snap_leaving)
    {
        snap_leaving = true;
        snap_start = now;
        animating = true;
    }
}

/* the snap preview, drawn just under the window being dragged */
static bool preview_draw( struct window_draw *draw, uint64_t now )
{
    struct frect rect;
    float alpha;

    if (!snap_frame( now, &rect, &alpha )) return false;
    memset( draw, 0, sizeof(*draw) );
    draw->preview = true;
    draw->opacity = alpha;
    draw->backdrop = draw->blur = true;
    draw->tint = 0x40ffffff;  /* the light acrylic of Windows */
    draw->clip.radius = CORNER_RADIUS;
    set_draw_geometry( draw, rect, rect );
    return true;
}

/* how far an animation got: the window rectangle and opacity it has now */
static bool animation_frame( struct window_state *ws, uint64_t now, struct frect *rect, float *alpha )
{
    float t, e;

    if (!ws->anim || !ws->anim_start) return false;
    t = min( 1.0f, (float)(now - ws->anim_start) / ws->anim_duration );
    switch (ws->anim)
    {
    case ANIM_CLOSE:
    case ANIM_POPUP_OUT:
    case ANIM_SLIDE_OUT:
    case ANIM_FADE_OUT:
        e = accelerate( t );
        break;
    default:
        e = decelerate( t );
        break;
    }
    rect->left = lerp( ws->anim_from.left, ws->anim_to.left, e );
    rect->top = lerp( ws->anim_from.top, ws->anim_to.top, e );
    rect->right = lerp( ws->anim_from.right, ws->anim_to.right, e );
    rect->bottom = lerp( ws->anim_from.bottom, ws->anim_to.bottom, e );
    /* a minimized window fades out as it reaches its button */
    if (ws->anim == ANIM_MINIMIZE) *alpha = 1 - t * t;
    else if (ws->anim == ANIM_RESTORE) *alpha = min( 1.0f, t * 3 );
    else *alpha = lerp( ws->alpha_from, ws->alpha_to, e );
    TRACE( "%08x at %.2f: %.0f,%.0f-%.0f,%.0f opacity %.2f\n", ws->hwnd, t, rect->left, rect->top, rect->right,
           rect->bottom, *alpha );
    return true;
}

static bool fill_draw( struct window_draw *draw, const struct dwm_window *w, struct window_state *ws, uint64_t now )
{
    struct frect base = window_frect( w ), shown = base;
    float alpha = 1;

    memset( draw, 0, sizeof(*draw) );
    draw->w = w;
    draw->ws = ws;
    draw->opacity = 1;
    draw->mode = layer_mode;
    draw->clip.radius = corner_radius( w, ws );
    draw->decorated = window_decorated( w, draw->clip.radius );
    draw->backdrop = window_backdrop( ws, &draw->blur, &draw->tint );

    if (ws && ws->anim && !ws->ghost)
    {
        bool resized = ws->anim == ANIM_RESTORE || ws->anim == ANIM_MORPH;

        /* it grows into the window's rectangle as the window has it now */
        if (resized) ws->anim_to = base;
        if (!ws->anim_start)
        {
            /* it shows once it has drawn itself, where it stands */
            if (!window_has_content( w, false ) || !on_screen( w )) return false;
            ws->anim_start = now;
        }
        if (animation_frame( ws, now, &shown, &alpha ))
        {
            draw->as_image = true;
            draw->opacity = alpha;
            /* As Windows does: what the window showed before is stretched from
             * one rectangle to the other, and the window as it is now fades
             * in over it at the end, once it has drawn itself at its size.
             * A window that redraws slowly (Explorer) does not stutter. */
            if (resized && ws->snapshot_valid)
            {
                float t = min( 1.0f, (float)(now - ws->anim_start) / ws->anim_duration );
                struct frect live = base;

                draw->from_snapshot = true;
                base = frect_from( ws->snapshot_rect.left, ws->snapshot_rect.top, ws->snapshot_rect.right,
                                   ws->snapshot_rect.bottom );
                if (window_has_content( w, true ))
                {
                    draw->live_opacity = t > 0.5f ? (t - 0.5f) * 2 : 0;
                    draw->live_xform = xform_between( live, shown );
                }
            }
        }
    }
    set_draw_geometry( draw, base, shown );
    return true;
}

uint32_t scene_windows( struct window_draw *draws, uint32_t max )
{
    uint64_t now = kms_now();
    struct window_state *ws;
    uint32_t count = 0;
    bool previewed = false;

    /* Win32 windows, bottom to top, where wineserver has them */
    for (uint32_t i = window_count; i-- && count < max;)
    {
        const struct dwm_window *w = &windows[i];

        if (!previewed && snap_start && w->hwnd == snap_hwnd && count < max - 1)
        {
            previewed = true;
            if (preview_draw( &draws[count], now )) count++;
        }
        ws = window_state( w->hwnd );
        if (!(w->style & WS_VISIBLE) || (w->style & WS_MINIMIZE) || is_cloaked( ws )) continue;
        if (shown_directly( w->hwnd ) || (ws && ws->ghost)) continue;
        if (fill_draw( &draws[count], w, ws, now )) count++;
    }

    if (!previewed && snap_start && count < max && preview_draw( &draws[count], now )) count++;

    /* windows that went, over the rest while they go */
    wl_list_for_each( ws, &window_states, link )
    {
        struct window_draw *draw;
        struct frect shown;
        float alpha;

        if (count >= max) break;
        if (!ws->ghost || !ws->snapshot_valid || !animation_frame( ws, now, &shown, &alpha )) continue;
        draw = &draws[count++];
        memset( draw, 0, sizeof(*draw) );
        draw->w = &ws->last;
        draw->ws = ws;
        draw->mode = layer_mode;
        draw->opacity = alpha;
        draw->from_snapshot = true;
        draw->as_image = true;
        draw->clip.radius = corner_radius( &ws->last, ws );
        draw->decorated = window_decorated( &ws->last, draw->clip.radius );
        draw->backdrop = window_backdrop( ws, &draw->blur, &draw->tint );
        set_draw_geometry( draw, frect_from( ws->snapshot_rect.left, ws->snapshot_rect.top,
                                             ws->snapshot_rect.right, ws->snapshot_rect.bottom ), shown );
    }
    return count;
}

/* a window as it is, still: for its snapshot */
void scene_static_draw( struct window_draw *draw, const struct dwm_window *w, struct window_state *ws )
{
    struct frect r = window_frect( w );

    memset( draw, 0, sizeof(*draw) );
    draw->w = w;
    draw->ws = ws;
    draw->opacity = 1;
    draw->mode = layer_mode;
    draw->clip.radius = corner_radius( w, ws );
    draw->decorated = window_decorated( w, draw->clip.radius );
    draw->backdrop = window_backdrop( ws, &draw->blur, &draw->tint );
    set_draw_geometry( draw, r, r );
}

/* surfaces no Win32 window claims, in their cascade */
void scene_unowned_surfaces( void (*callback)( struct surface *s, int x, int y, void *ctx ), void *ctx )
{
    struct surface *s;

    wl_list_for_each( s, &toplevels, stack_link ) if (!s->hwnd) callback( s, s->x, s->y, ctx );
}

static const struct dwm_window *listed_window( uint32_t hwnd )
{
    for (uint32_t i = 0; i < window_count; i++) if (windows[i].hwnd == hwnd) return &windows[i];
    return NULL;
}

/* DwmRegisterThumbnail: live pictures of other windows drawn in this one */
uint32_t scene_thumbnails( const struct window_draw *draw, struct thumbnail_draw *thumbs, uint32_t max )
{
    uint32_t count = 0;

    if (draw->from_snapshot) return 0;
    for (uint32_t i = 0; i < attribute_count && count < max; i++)
    {
        const struct arctic_dwm_entry *entry = &attributes[i];
        const struct arctic_dwm_thumbnail *thumb = &entry->u.thumbnail;
        const struct dwm_window *source;
        struct thumbnail_draw *t = &thumbs[count];
        struct frect dest;
        int width, height;

        if (entry->type != ARCTIC_DWM_THUMBNAIL || entry->hwnd != draw->w->hwnd) continue;
        if (!thumb->visible || !thumb->opacity) continue;
        if (thumb->dest_right <= thumb->dest_left || thumb->dest_bottom <= thumb->dest_top) continue;
        if (!(source = listed_window( thumb->source ))) continue;

        memset( t, 0, sizeof(*t) );
        t->source = source;
        t->source_state = window_state( source->hwnd );
        width = source->right - source->left;
        height = source->bottom - source->top;
        if (!window_shown( source, t->source_state ))
        {
            /* minimized or hidden: what it showed last */
            if (!t->source_state || !t->source_state->snapshot_valid) continue;
            t->from_snapshot = true;
            width = t->source_state->snapshot_rect.right - t->source_state->snapshot_rect.left;
            height = t->source_state->snapshot_rect.bottom - t->source_state->snapshot_rect.top;
        }
        if (thumb->source_right > thumb->source_left && thumb->source_bottom > thumb->source_top)
        {
            float ss = window_scale( source );
            set_rect(&t->source_rect, thumb->source_left * ss, thumb->source_top * ss, thumb->source_right * ss,
                     thumb->source_bottom * ss );
        }
        else if (thumb->client_only && !t->from_snapshot)
            set_rect(&t->source_rect, source->client_left - source->left, source->client_top - source->top,
                     source->client_right - source->left, source->client_bottom - source->top );
        else
            set_rect(&t->source_rect, 0, 0, width, height );

        /* the destination is in the client coordinates of the window it is drawn in */
        dest.left = draw->w->client_left + thumb->dest_left * window_scale( draw->w );
        dest.top = draw->w->client_top + thumb->dest_top * window_scale( draw->w );
        dest.right = draw->w->client_left + thumb->dest_right * window_scale( draw->w );
        dest.bottom = draw->w->client_top + thumb->dest_bottom * window_scale( draw->w );
        t->dest.left = dest.left * draw->xform.sx + draw->xform.tx;
        t->dest.top = dest.top * draw->xform.sy + draw->xform.ty;
        t->dest.right = dest.right * draw->xform.sx + draw->xform.tx;
        t->dest.bottom = dest.bottom * draw->xform.sy + draw->xform.ty;
        t->opacity = thumb->opacity / 255.0f * draw->opacity;
        count++;
    }
    return count;
}

/* on the CPU: the backdrop is its tint, without the blur */
static void tint_rect( const RECT *rect, uint32_t tint )
{
    uint32_t a = tint >> 24;
    uint32_t premul;

    if (!a) return;
    premul = (a << 24) | ((((tint >> 16) & 0xff) * a / 255) << 16) | ((((tint >> 8) & 0xff) * a / 255) << 8) |
             ((tint & 0xff) * a / 255);
    for (int y = max( rect->top, screen.top ); y < min( rect->bottom, screen.bottom ); y++)
    {
        uint32_t *row = shadow + (size_t)(y - screen.top) * (screen.right - screen.left) - screen.left;
        for (int x = max( rect->left, screen.left ); x < min( rect->right, screen.right ); x++)
            row[x] = a == 0xff ? premul : over( premul, row[x] );
    }
}

/* on the CPU: a thumbnail is the source's own surface, scaled by the nearest pixel */
static void cpu_thumbnail( const struct thumbnail_draw *t )
{
    const struct surface *s = surface_for_hwnd( t->source->hwnd );
    int dw = t->dest.right - t->dest.left, dh = t->dest.bottom - t->dest.top;
    int sw = t->source_rect.right - t->source_rect.left, sh = t->source_rect.bottom - t->source_rect.top;

    if (t->from_snapshot || !s || !s->pixels || !s->xdg_toplevel || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    for (int y = 0; y < dh; y++)
    {
        int py = (int)t->dest.top + y, sy = t->source_rect.top + sh * y / dh;
        uint32_t *row;

        if (py < screen.top || py >= screen.bottom || sy < 0 || sy >= s->height) continue;
        row = shadow + (size_t)(py - screen.top) * (screen.right - screen.left) - screen.left;
        for (int x = 0; x < dw; x++)
        {
            int px = (int)t->dest.left + x, sx = t->source_rect.left + sw * x / dw;
            if (px < screen.left || px >= screen.right || sx < 0 || sx >= s->width) continue;
            row[px] = s->pixels[(size_t)sy * s->width + sx] | 0xff000000;
        }
    }
}

struct cpu_draw_ctx
{
    bool backdrop;
};

static void cpu_draw_surface( struct surface *s, int x, int y, void *ctx )
{
    const struct cpu_draw_ctx *c = ctx;
    draw_tree( s, x, y, c->backdrop );
}

/* the virtual screen, in RAM */
static void compose(void)
{
    size_t count = (size_t)(screen.right - screen.left) * (screen.bottom - screen.top);
    struct surface *s;

    if (!shadow) return;
    for (size_t i = 0; i < count; i++) shadow[i] = background;

    /* Win32 windows, bottom to top, where wineserver has them */
    for (uint32_t i = window_count; i--;)
    {
        const struct dwm_window *w = &windows[i];
        struct window_state *ws = window_state( w->hwnd );
        struct thumbnail_draw thumbs[16];
        struct window_draw draw;
        struct cpu_draw_ctx ctx;
        uint32_t n;

        if (!(w->style & WS_VISIBLE) || (w->style & WS_MINIMIZE) || is_cloaked( ws )) continue;
        if (shown_directly( w->hwnd )) continue;
        fill_draw( &draw, w, NULL, 0 );
        draw.ws = ws;
        ctx.backdrop = window_backdrop( ws, &draw.blur, &draw.tint );
        if (ctx.backdrop) tint_rect( &(RECT){ w->left, w->top, w->right, w->bottom }, draw.tint );
        for_each_window_surface( w, cpu_draw_surface, &ctx );
        n = scene_thumbnails( &draw, thumbs, ARRAY_SIZE(thumbs) );
        for (uint32_t k = 0; k < n; k++) cpu_thumbnail( &thumbs[k] );
    }
    wl_list_for_each( s, &toplevels, stack_link )
        if (!s->hwnd) draw_tree( s, s->x, s->y, false );
    draw_cursor();
    dirty = false;
}

/* the part of the virtual screen the monitor stands on */
static void copy_shadow( const struct kms_output *o, struct kms_fb *fb )
{
    uint32_t width = screen.right - screen.left, height = screen.bottom - screen.top;
    uint32_t rows = min( fb->height, height - (o->y - screen.top) );

    if (!shadow) return;
    for (uint32_t y = 0; y < rows; y++)
        memcpy( (uint8_t *)fb->pixels + (size_t)y * fb->pitch,
                shadow + (size_t)(o->y - screen.top + y) * width + (o->x - screen.left),
                min( fb->width, width ) * 4 );
}

static void send_frame_callbacks(void)
{
    struct wl_resource *cb, *tmp;
    struct surface *s;
    uint32_t time = now_ms();

    wl_list_for_each( s, &all_surfaces, all_link )
    {
        wl_resource_for_each_safe( cb, tmp, &s->frames )
        {
            wl_callback_send_done( cb, time );
            wl_resource_destroy( cb );
        }
    }
    last_callbacks = kms_now();
}

static bool cursor_on( const struct kms_output *o )
{
    const struct kms_mode *mode = &o->modes[o->mode];

    return !cursor_hidden && cursor_x >= o->x && cursor_y >= o->y && cursor_x < o->x + (int)mode->info.hdisplay &&
           cursor_y < o->y + (int)mode->info.vdisplay;
}

/* The buffer of a window that fills the monitor, with nothing over it: a game
 * or a video full screen. The client surface it draws in lies over the
 * window's own (the GDI part) and must cover all of it. */
static struct surface *fullscreen_content( const struct kms_output *o, uint32_t *hwnd )
{
    const struct kms_mode *mode = &o->modes[o->mode];
    RECT rect = { o->x, o->y, o->x + mode->info.hdisplay, o->y + mode->info.vdisplay };
    struct surface *s, *top, *child;

    wl_list_for_each( s, &toplevels, stack_link )
        if (!s->hwnd && s->x < rect.right && s->y < rect.bottom &&
            s->x + s->width > rect.left && s->y + s->height > rect.top) return NULL;

    for (uint32_t i = 0; i < window_count; i++)
    {
        const struct dwm_window *w = &windows[i];
        int x, y, width, height;

        if (!(w->style & WS_VISIBLE) || (w->style & WS_MINIMIZE)) continue;
        if (w->right <= rect.left || w->left >= rect.right || w->bottom <= rect.top || w->top >= rect.bottom) continue;
        /* the topmost window on the monitor is the one that fills it */
        if (w->left != rect.left || w->top != rect.top || w->right != rect.right || w->bottom != rect.bottom)
            return NULL;
        if (!(s = surface_for_hwnd( w->hwnd ))) return NULL;
        top = s;
        wl_list_for_each( child, &s->children, child_link )
            if (!child->hwnd && has_content( child )) top = child;
        if (!top->dmabuf || top->src_x || top->src_y) return NULL;
        wl_list_for_each( child, &top->children, child_link )
            if (!child->hwnd && has_content( child )) return NULL;
        x = w->left + (top == s ? 0 : top->sub_x);
        y = w->top + (top == s ? 0 : top->sub_y);
        width = top->dst_width ? top->dst_width : top->src_width > 0 ? top->src_width : top->width;
        height = top->dst_height ? top->dst_height : top->src_height > 0 ? top->src_height : top->height;
        if (x != rect.left || y != rect.top || width != rect.right - rect.left || height != rect.bottom - rect.top)
            return NULL;
        *hwnd = w->hwnd;
        return top;
    }
    return NULL;
}

static uint32_t source_width( const struct surface *s )
{
    return s->src_width > 0 ? min( s->src_width, s->width ) : s->width;
}

static uint32_t source_height( const struct surface *s )
{
    return s->src_height > 0 ? min( s->src_height, s->height ) : s->height;
}

/* Whether the monitor can show the buffer as it is, the way Windows flips a
 * full screen game's swap chain to the display without DWM composing it. */
static bool can_show_directly( struct kms_output *o, struct surface *content )
{
    struct dmabuf *buffer = content->dmabuf;
    uint32_t fb;

    if (!hw_cursor || !o->plane || !(fb = dmabuf_fb( buffer ))) return false;
    if (buffer->scanout_tested != o->crtc_id)
    {
        buffer->scanout_tested = o->crtc_id;
        buffer->scanout_ok = kms_can_scanout( o, fb, source_width( content ), source_height( content ) );
        if (!buffer->scanout_ok) TRACE( "%s cannot scan out a %ux%u buffer\n", o->name, buffer->width, buffer->height );
    }
    /* the pointer cannot be drawn into the game's buffer: it goes to the cursor plane */
    return buffer->scanout_ok && kms_show_cursor( o, cursor_on( o ), cursor_x - o->x, cursor_y - o->y );
}

/* the display settings of the Control Panel turned it off for this monitor */
static bool vrr_turned_off( const struct kms_output *o )
{
    for (uint32_t i = 0; i < vrr_off_count; i++) if (vrr_off[i] == o->connector_id) return true;
    return false;
}

static void present_directly( struct output *output, struct surface *content, bool vrr )
{
    struct kms_output *o = output->kms;
    struct dmabuf *buffer = content->dmabuf;

    /* the same buffer again (the pointer moved, another window changed): nothing new to show */
    if (o->front_buffer == buffer &&
        o->vrr_on == (vrr && o->vrr_capable && o->vrr_prop && !o->vrr_refused)) return;
    dmabuf_ref( buffer );
    if (kms_present( o, buffer->fb, source_width( content ), source_height( content ), vrr, buffer, false )) return;
    if (errno == EBUSY)
    {
        dmabuf_unref( buffer );
        output->needs_frame = true;  /* the card is still busy with the last one */
        return;
    }
    dmabuf_unref( buffer );
    /* the flip did not take it after all: it is composed from now on */
    buffer->scanout_ok = false;
    output->direct_hwnd = 0;
    damage();
}

static void present_composed( struct output *output, bool vrr )
{
    struct kms_output *o = output->kms;
    struct kms_fb *fb;

    kms_show_cursor( o, false, 0, 0 );
    if (!o->no_flip && (fb = kms_back_buffer( o )))
    {
        copy_shadow( o, fb );
        if (kms_present( o, fb->fb_id, fb->width, fb->height, vrr, NULL, true )) return;
    }
    if (!o->no_flip)
    {
        output->needs_frame = true;  /* busy: the next tick tries again */
        return;
    }
    /* a driver that cannot flip: the frame is drawn where it shows */
    copy_shadow( o, kms_front_buffer( o ) );
    kms_flush( o );
    output->not_before = kms_now() + kms_frame_time( o );
    send_frame_callbacks();
}

/* While a window fills the monitor with a GPU buffer (a game), the log says
 * every ten seconds how many frames a second it got, shown directly or
 * composed, and whether the refresh followed them */
static void count_frame( struct output *output, bool direct, bool vrr, uint64_t now )
{
    uint64_t elapsed;

    if (!direct && !vrr && !output->stat_direct && !output->stat_composed) return;
    if (!output->stat_since) output->stat_since = now;
    if (direct) output->stat_direct++;
    else output->stat_composed++;
    output->stat_vrr |= vrr;
    if ((elapsed = now - output->stat_since) < 10000000) return;
    MESSAGE( "dwm: %s %u frames/s, %u%% shown directly, variable refresh %s\n", output->kms->name,
             (unsigned int)((output->stat_direct + output->stat_composed) * 1000000ull / elapsed),
             output->stat_direct * 100 / (output->stat_direct + output->stat_composed),
             output->stat_vrr ? "on" : "off" );
    output->stat_direct = output->stat_composed = 0;
    output->stat_vrr = false;
    output->stat_since = 0;
}

/* A frame for every monitor that needs one and has shown its last one. A
 * window that fills a monitor with a GPU buffer turns variable refresh on
 * there: the monitor then waits for the game's frames. */
static void repaint(void)
{
    struct { struct output *output; struct surface *content; bool vrr; } ready[KMS_MAX_OUTPUTS];
    uint64_t now = kms_now();
    struct output *output;
    int count = 0;
    bool composed = false, gpu = gl_active();

    animating = update_animations( now );
    wl_list_for_each( output, &outputs, link )
    {
        struct kms_output *o = output->kms;
        struct surface *content;
        uint32_t hwnd = 0;

        if (!o || !o->enabled || !output->needs_frame || o->queued_fb || now < output->not_before) continue;
        if (count == ARRAY_SIZE(ready)) break;
        content = fullscreen_content( o, &hwnd );
        ready[count].output = output;
        ready[count].vrr = content && vrr_allowed && !vrr_turned_off( o );
        ready[count].content = content && can_show_directly( o, content ) ? content : NULL;
        if (ready[count].content) output->direct_hwnd = hwnd;
        else
        {
            /* a window shown directly until now must be composed again */
            if (output->direct_hwnd) dirty = true;
            output->direct_hwnd = 0;
            composed = true;
        }
        output->needs_frame = false;
        count++;
    }
    if (composed && dirty && !gpu) compose();

    for (int i = 0; i < count; i++)
    {
        if (ready[i].content) present_directly( ready[i].output, ready[i].content, ready[i].vrr );
        else if (!gpu) present_composed( ready[i].output, ready[i].vrr );
        else if (!gl_render( ready[i].output, ready[i].vrr ))
        {
            /* busy, or the GPU gave up: then the CPU composes from now on */
            if (!gl_active()) dirty = true;
            ready[i].output->needs_frame = true;
        }
        count_frame( ready[i].output, !!ready[i].content, ready[i].vrr, now );
    }
}

void compositor_flip_done( struct kms_output *o, void *buffer )
{
    struct output *output = o->user;

    if (buffer && !gl_frame_released( buffer )) dmabuf_unref( buffer );
    if (output && o->fake_vblank) output->not_before = o->done_time + kms_frame_time( o );
    send_frame_callbacks();
}

void compositor_buffer_unused( void *buffer )
{
    if (!gl_frame_released( buffer )) dmabuf_unref( buffer );
}

static bool flips_pending(void)
{
    for (int i = 0; i < KMS_MAX_OUTPUTS; i++) if (kms.outputs[i].queued_fb) return true;
    return false;
}

/* frames a flip could not take yet, the pacing of flips that finish at once,
 * and frame callbacks of surfaces while nothing is flipped */
static int frame_tick( void *data )
{
    uint32_t hz = screen_refresh_hz();

    if (animating) damage();
    repaint();
    if (kms_now() - last_callbacks >= 1000000 / hz && !flips_pending()) send_frame_callbacks();
    wl_event_source_timer_update( frame_timer, max( 1, 1000 / hz ) );
    return 0;
}

static int kms_event( int fd, uint32_t mask, void *data )
{
    kms_dispatch();
    return 0;
}

/**********************************************************************
 *          Toplevels
 */

static void map_toplevel( struct surface *s )
{
    int step = 32 * (cascade++ % 12);

    s->x = screen.left + 48 + step;
    s->y = screen.top + 48 + step;
    if (s->x + s->width > screen.right) s->x = screen.left;
    if (s->y + s->height > screen.bottom) s->y = screen.top;
    s->mapped = true;
    wl_list_insert( toplevels.prev, &s->stack_link );
}

static void unmap_toplevel( struct surface *s )
{
    if (!s->mapped) return;
    wl_list_remove( &s->stack_link );
    wl_list_init( &s->stack_link );
    s->mapped = false;
}

static void send_configure( struct surface *s )
{
    struct wl_array states;
    uint32_t *state;

    wl_array_init( &states );
    if (s->maximized && (state = wl_array_add( &states, sizeof(*state) ))) *state = XDG_TOPLEVEL_STATE_MAXIMIZED;
    if (s->fullscreen && (state = wl_array_add( &states, sizeof(*state) ))) *state = XDG_TOPLEVEL_STATE_FULLSCREEN;
    xdg_toplevel_send_configure( s->xdg_toplevel, 0, 0, &states );
    wl_array_release( &states );
    xdg_surface_send_configure( s->xdg_surface, wl_display_next_serial( display ) );
    s->configured = true;
}

/**********************************************************************
 *          wl_surface
 */

static void pending_buffer_destroyed( struct wl_listener *listener, void *data )
{
    struct surface *s = wl_container_of( listener, s, pending_buffer_destroy );

    wl_list_remove( &listener->link );
    wl_list_init( &listener->link );
    s->pending_buffer = NULL;
}

static void surface_attach( struct wl_client *client, struct wl_resource *resource,
                            struct wl_resource *buffer, int32_t x, int32_t y )
{
    struct surface *s = get_surface( resource );

    wl_list_remove( &s->pending_buffer_destroy.link );
    wl_list_init( &s->pending_buffer_destroy.link );
    s->pending_buffer = buffer;
    s->pending_attach = true;
    if (buffer) wl_resource_add_destroy_listener( buffer, &s->pending_buffer_destroy );
}

static void surface_damage( struct wl_client *client, struct wl_resource *resource,
                            int32_t x, int32_t y, int32_t width, int32_t height )
{
}

static void callback_destroyed( struct wl_resource *callback )
{
    wl_list_remove( wl_resource_get_link( callback ) );
}

static void surface_frame( struct wl_client *client, struct wl_resource *resource, uint32_t id )
{
    struct surface *s = get_surface( resource );
    struct wl_resource *callback = wl_resource_create( client, &wl_callback_interface, 1, id );

    if (!callback)
    {
        wl_client_post_no_memory( client );
        return;
    }
    wl_resource_set_implementation( callback, NULL, NULL, callback_destroyed );
    wl_list_insert( s->pending_frames.prev, wl_resource_get_link( callback ) );
}

static void surface_set_region( struct wl_client *client, struct wl_resource *resource, struct wl_resource *region )
{
}

static void copy_buffer( struct surface *s, struct wl_resource *buffer )
{
    struct wl_shm_buffer *shm = wl_shm_buffer_get( buffer );
    const uint8_t *data;
    int width, height, stride;

    if (!shm)
    {
        FIXME( "only shm buffers are supported so far\n" );
        return;
    }
    width = wl_shm_buffer_get_width( shm );
    height = wl_shm_buffer_get_height( shm );
    stride = wl_shm_buffer_get_stride( shm );
    if (width != s->width || height != s->height || !s->pixels)
    {
        free( s->pixels );
        if (!(s->pixels = malloc( (size_t)width * height * 4 ))) width = height = 0;
        s->width = width;
        s->height = height;
    }
    wl_shm_buffer_begin_access( shm );
    data = wl_shm_buffer_get_data( shm );
    for (int y = 0; y < height; y++)
        memcpy( s->pixels + (size_t)y * width, data + (size_t)y * stride, (size_t)width * 4 );
    wl_shm_buffer_end_access( shm );
    s->alpha = wl_shm_buffer_get_format( shm ) == WL_SHM_FORMAT_ARGB8888;
}

/* A game's frame changes only the monitor that shows it directly: the others
 * are not composed again for it. */
static bool wake_direct_output( struct surface *s )
{
    struct output *output;

    while (s->parent && !s->hwnd) s = s->parent;
    if (!s->hwnd) return false;
    wl_list_for_each( output, &outputs, link )
    {
        if (output->direct_hwnd != s->hwnd) continue;
        output->needs_frame = true;
        return true;
    }
    return false;
}

/* Before a window's surface takes a frame of another size, or none: what it
 * showed is kept, for when it turns out that the window was closed or
 * minimized (wineserver's list may say so only a moment later). */
static void keep_last_frame( struct surface *s, struct dmabuf *buffer )
{
    struct wl_shm_buffer *shm = s->pending_buffer ? wl_shm_buffer_get( s->pending_buffer ) : NULL;
    struct window_state *ws;
    int width = 0, height = 0;

    if (!s->hwnd || !s->xdg_toplevel || !has_content( s )) return;
    if (!(ws = window_state( s->hwnd )) || !ws->shown) return;
    if (buffer)
    {
        width = buffer->width;
        height = buffer->height;
    }
    else if (shm)
    {
        width = wl_shm_buffer_get_width( shm );
        height = wl_shm_buffer_get_height( shm );
    }
    if (width == s->width && height == s->height) return;
    gl_snapshot( ws, &ws->last );
}

static void surface_commit( struct wl_client *client, struct wl_resource *resource )
{
    struct surface *s = get_surface( resource );

    if (s->pending_attach)
    {
        struct dmabuf *buffer = dmabuf_from_resource( s->pending_buffer );

        keep_last_frame( s, buffer );

        if (buffer != s->dmabuf)
        {
            if (buffer) dmabuf_ref( buffer );
            if (s->dmabuf) dmabuf_unref( s->dmabuf );
            s->dmabuf = buffer;
        }
        if (buffer)
        {
            if (buffer->width != s->width || buffer->height != s->height)
            {
                free( s->pixels );
                s->pixels = NULL;
            }
            s->width = buffer->width;
            s->height = buffer->height;
            s->alpha = buffer->alpha;
            s->stale = true;
            wl_list_remove( &s->pending_buffer_destroy.link );
            wl_list_init( &s->pending_buffer_destroy.link );
        }
        else if (s->pending_buffer)
        {
            s->texture_stale = true;
            copy_buffer( s, s->pending_buffer );
            wl_buffer_send_release( s->pending_buffer );
            wl_list_remove( &s->pending_buffer_destroy.link );
            wl_list_init( &s->pending_buffer_destroy.link );
        }
        else
        {
            free( s->pixels );
            s->pixels = NULL;
            s->width = s->height = 0;
        }
        s->pending_buffer = NULL;
        s->pending_attach = false;
    }
    wl_list_insert_list( s->frames.prev, &s->pending_frames );
    wl_list_init( &s->pending_frames );

    if (s->xdg_toplevel)
    {
        if (!s->configured) send_configure( s );
        else if (has_content( s ) && !s->mapped) map_toplevel( s );
        else if (!has_content( s )) unmap_toplevel( s );
    }
    if (!wake_direct_output( s )) damage();
}

static void surface_set_buffer_transform( struct wl_client *client, struct wl_resource *resource, int32_t transform )
{
}

static void surface_set_buffer_scale( struct wl_client *client, struct wl_resource *resource, int32_t scale )
{
}

static void surface_offset( struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y )
{
}

static const struct wl_surface_interface surface_impl =
{
    .destroy = resource_destroy,
    .attach = surface_attach,
    .damage = surface_damage,
    .frame = surface_frame,
    .set_opaque_region = surface_set_region,
    .set_input_region = surface_set_region,
    .commit = surface_commit,
    .set_buffer_transform = surface_set_buffer_transform,
    .set_buffer_scale = surface_set_buffer_scale,
    .damage_buffer = surface_damage,
    .offset = surface_offset,
};

static void surface_destroyed( struct wl_resource *resource )
{
    struct surface *s = get_surface( resource ), *child, *next;
    struct wl_resource *cb, *tmp;
    struct window_state *ws;

    /* a window closing: what it showed stays for its animation */
    if (s->hwnd && s->xdg_toplevel && has_content( s ) && (ws = window_state( s->hwnd )) && ws->shown)
        gl_snapshot( ws, &ws->last );
    gl_surface_gone( s );
    unmap_toplevel( s );
    wl_list_remove( &s->all_link );
    wl_list_remove( &s->pending_buffer_destroy.link );
    wl_resource_for_each_safe( cb, tmp, &s->pending_frames ) wl_resource_destroy( cb );
    wl_resource_for_each_safe( cb, tmp, &s->frames ) wl_resource_destroy( cb );
    wl_list_for_each_safe( child, next, &s->children, child_link )
    {
        wl_list_remove( &child->child_link );
        wl_list_init( &child->child_link );
        child->parent = NULL;
    }
    if (s->parent) wl_list_remove( &s->child_link );
    /* role objects outlive the surface in the protocol; they must not point at it */
    if (s->subsurface) wl_resource_set_user_data( s->subsurface, NULL );
    if (s->xdg_surface) wl_resource_set_user_data( s->xdg_surface, NULL );
    if (s->xdg_toplevel) wl_resource_set_user_data( s->xdg_toplevel, NULL );
    if (s->viewport) wl_resource_set_user_data( s->viewport, NULL );
    if (s->arctic_window) wl_resource_set_user_data( s->arctic_window, NULL );
    if (s->dmabuf) dmabuf_unref( s->dmabuf );
    free( s->pixels );
    free( s );
    damage();
}

/**********************************************************************
 *          wl_compositor, wl_region
 */

static void region_op( struct wl_client *client, struct wl_resource *resource,
                       int32_t x, int32_t y, int32_t width, int32_t height )
{
}

static const struct wl_region_interface region_impl =
{
    .destroy = resource_destroy,
    .add = region_op,
    .subtract = region_op,
};

static void compositor_create_surface( struct wl_client *client, struct wl_resource *resource, uint32_t id )
{
    struct surface *s = calloc( 1, sizeof(*s) );

    if (!s || !(s->resource = wl_resource_create( client, &wl_surface_interface,
                                                  wl_resource_get_version( resource ), id )))
    {
        free( s );
        wl_client_post_no_memory( client );
        return;
    }
    wl_list_init( &s->pending_frames );
    wl_list_init( &s->frames );
    wl_list_init( &s->children );
    wl_list_init( &s->child_link );
    wl_list_init( &s->stack_link );
    wl_list_init( &s->pending_buffer_destroy.link );
    s->pending_buffer_destroy.notify = pending_buffer_destroyed;
    wl_list_insert( &all_surfaces, &s->all_link );
    wl_resource_set_implementation( s->resource, &surface_impl, s, surface_destroyed );
}

static void compositor_create_region( struct wl_client *client, struct wl_resource *resource, uint32_t id )
{
    struct wl_resource *region = wl_resource_create( client, &wl_region_interface, 1, id );

    if (!region)
    {
        wl_client_post_no_memory( client );
        return;
    }
    wl_resource_set_implementation( region, &region_impl, NULL, NULL );
}

static const struct wl_compositor_interface compositor_impl =
{
    .create_surface = compositor_create_surface,
    .create_region = compositor_create_region,
};

static void bind_compositor( struct wl_client *client, void *data, uint32_t version, uint32_t id )
{
    struct wl_resource *resource = wl_resource_create( client, &wl_compositor_interface, version, id );
    if (resource) wl_resource_set_implementation( resource, &compositor_impl, NULL, NULL );
}

/**********************************************************************
 *          wl_subcompositor
 */

static void subsurface_set_position( struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y )
{
    struct surface *s = get_surface( resource );

    if (!s) return;
    s->sub_x = x;
    s->sub_y = y;
    damage();
}

static void subsurface_place( struct wl_client *client, struct wl_resource *resource, struct wl_resource *sibling )
{
}

static void subsurface_sync( struct wl_client *client, struct wl_resource *resource )
{
}

static const struct wl_subsurface_interface subsurface_impl =
{
    .destroy = resource_destroy,
    .set_position = subsurface_set_position,
    .place_above = subsurface_place,
    .place_below = subsurface_place,
    .set_sync = subsurface_sync,
    .set_desync = subsurface_sync,
};

static void subsurface_destroyed( struct wl_resource *resource )
{
    struct surface *s = get_surface( resource );

    if (!s) return;
    if (s->parent)
    {
        wl_list_remove( &s->child_link );
        wl_list_init( &s->child_link );
        s->parent = NULL;
    }
    s->subsurface = NULL;
    damage();
}

static void subcompositor_get_subsurface( struct wl_client *client, struct wl_resource *resource, uint32_t id,
                                          struct wl_resource *surface, struct wl_resource *parent )
{
    struct surface *s = get_surface( surface ), *p = get_surface( parent );
    struct wl_resource *sub = wl_resource_create( client, &wl_subsurface_interface, 1, id );

    if (!sub)
    {
        wl_client_post_no_memory( client );
        return;
    }
    wl_resource_set_implementation( sub, &subsurface_impl, s, subsurface_destroyed );
    s->subsurface = sub;
    s->parent = p;
    wl_list_insert( p->children.prev, &s->child_link );
    damage();
}

static const struct wl_subcompositor_interface subcompositor_impl =
{
    .destroy = resource_destroy,
    .get_subsurface = subcompositor_get_subsurface,
};

static void bind_subcompositor( struct wl_client *client, void *data, uint32_t version, uint32_t id )
{
    struct wl_resource *resource = wl_resource_create( client, &wl_subcompositor_interface, version, id );
    if (resource) wl_resource_set_implementation( resource, &subcompositor_impl, NULL, NULL );
}

/**********************************************************************
 *          wp_viewporter
 */

static void viewport_set_source( struct wl_client *client, struct wl_resource *resource,
                                 wl_fixed_t x, wl_fixed_t y, wl_fixed_t width, wl_fixed_t height )
{
    struct surface *s = get_surface( resource );

    if (!s) return;
    if (width == wl_fixed_from_int( -1 )) /* all -1: unset */
    {
        s->src_x = s->src_y = s->src_width = s->src_height = 0;
    }
    else
    {
        s->src_x = wl_fixed_to_int( x );
        s->src_y = wl_fixed_to_int( y );
        s->src_width = wl_fixed_to_int( width );
        s->src_height = wl_fixed_to_int( height );
    }
    damage();
}

static void viewport_set_destination( struct wl_client *client, struct wl_resource *resource,
                                      int32_t width, int32_t height )
{
    struct surface *s = get_surface( resource );

    if (!s) return;
    s->dst_width = width > 0 ? width : 0;
    s->dst_height = height > 0 ? height : 0;
    damage();
}

static const struct wp_viewport_interface viewport_impl =
{
    .destroy = resource_destroy,
    .set_source = viewport_set_source,
    .set_destination = viewport_set_destination,
};

static void viewport_destroyed( struct wl_resource *resource )
{
    struct surface *s = get_surface( resource );

    if (!s) return;
    s->viewport = NULL;
    s->src_x = s->src_y = s->src_width = s->src_height = 0;
    s->dst_width = s->dst_height = 0;
    damage();
}

static void viewporter_get_viewport( struct wl_client *client, struct wl_resource *resource, uint32_t id,
                                     struct wl_resource *surface )
{
    struct surface *s = get_surface( surface );
    struct wl_resource *viewport = wl_resource_create( client, &wp_viewport_interface, 1, id );

    if (!viewport)
    {
        wl_client_post_no_memory( client );
        return;
    }
    wl_resource_set_implementation( viewport, &viewport_impl, s, viewport_destroyed );
    s->viewport = viewport;
}

static const struct wp_viewporter_interface viewporter_impl =
{
    .destroy = resource_destroy,
    .get_viewport = viewporter_get_viewport,
};

static void bind_viewporter( struct wl_client *client, void *data, uint32_t version, uint32_t id )
{
    struct wl_resource *resource = wl_resource_create( client, &wp_viewporter_interface, version, id );
    if (resource) wl_resource_set_implementation( resource, &viewporter_impl, NULL, NULL );
}

/**********************************************************************
 *          xdg_wm_base
 */

static void positioner_size( struct wl_client *client, struct wl_resource *resource, int32_t w, int32_t h )
{
}

static void positioner_rect( struct wl_client *client, struct wl_resource *resource,
                             int32_t x, int32_t y, int32_t w, int32_t h )
{
}

static void positioner_uint( struct wl_client *client, struct wl_resource *resource, uint32_t value )
{
}

static void positioner_void( struct wl_client *client, struct wl_resource *resource )
{
}

static const struct xdg_positioner_interface positioner_impl =
{
    .destroy = resource_destroy,
    .set_size = positioner_size,
    .set_anchor_rect = positioner_rect,
    .set_anchor = positioner_uint,
    .set_gravity = positioner_uint,
    .set_constraint_adjustment = positioner_uint,
    .set_offset = positioner_size,
    .set_reactive = positioner_void,
    .set_parent_size = positioner_size,
    .set_parent_configure = positioner_uint,
};

static void popup_grab( struct wl_client *client, struct wl_resource *resource,
                        struct wl_resource *seat, uint32_t serial )
{
}

static void popup_reposition( struct wl_client *client, struct wl_resource *resource,
                              struct wl_resource *positioner, uint32_t token )
{
}

static const struct xdg_popup_interface popup_impl =
{
    .destroy = resource_destroy,
    .grab = popup_grab,
    .reposition = popup_reposition,
};

static void toplevel_set_parent( struct wl_client *client, struct wl_resource *resource, struct wl_resource *parent )
{
}

static void toplevel_set_string( struct wl_client *client, struct wl_resource *resource, const char *value )
{
}

static void toplevel_show_window_menu( struct wl_client *client, struct wl_resource *resource,
                                       struct wl_resource *seat, uint32_t serial, int32_t x, int32_t y )
{
}

static void toplevel_move( struct wl_client *client, struct wl_resource *resource,
                           struct wl_resource *seat, uint32_t serial )
{
}

static void toplevel_resize( struct wl_client *client, struct wl_resource *resource,
                             struct wl_resource *seat, uint32_t serial, uint32_t edges )
{
}

static void toplevel_size( struct wl_client *client, struct wl_resource *resource, int32_t w, int32_t h )
{
}

static void toplevel_void( struct wl_client *client, struct wl_resource *resource )
{
}

/* Win32 decides whether a window is maximized or fullscreen; the state is
 * only confirmed back, at the size the window already has (0x0) */
static void toplevel_set_state( struct wl_resource *resource, bool maximized, bool fullscreen )
{
    struct surface *s = get_surface( resource );

    if (!s) return;
    s->maximized = maximized;
    s->fullscreen = fullscreen;
    if (s->configured) send_configure( s );
}

static void toplevel_set_maximized( struct wl_client *client, struct wl_resource *resource )
{
    struct surface *s = get_surface( resource );
    if (s) toplevel_set_state( resource, true, s->fullscreen );
}

static void toplevel_unset_maximized( struct wl_client *client, struct wl_resource *resource )
{
    struct surface *s = get_surface( resource );
    if (s) toplevel_set_state( resource, false, s->fullscreen );
}

static void toplevel_set_fullscreen( struct wl_client *client, struct wl_resource *resource,
                                     struct wl_resource *output )
{
    struct surface *s = get_surface( resource );
    if (s) toplevel_set_state( resource, s->maximized, true );
}

static void toplevel_unset_fullscreen( struct wl_client *client, struct wl_resource *resource )
{
    struct surface *s = get_surface( resource );
    if (s) toplevel_set_state( resource, s->maximized, false );
}

static const struct xdg_toplevel_interface toplevel_impl =
{
    .destroy = resource_destroy,
    .set_parent = toplevel_set_parent,
    .set_title = toplevel_set_string,
    .set_app_id = toplevel_set_string,
    .show_window_menu = toplevel_show_window_menu,
    .move = toplevel_move,
    .resize = toplevel_resize,
    .set_max_size = toplevel_size,
    .set_min_size = toplevel_size,
    .set_maximized = toplevel_set_maximized,
    .unset_maximized = toplevel_unset_maximized,
    .set_fullscreen = toplevel_set_fullscreen,
    .unset_fullscreen = toplevel_unset_fullscreen,
    .set_minimized = toplevel_void,
};

static void toplevel_destroyed( struct wl_resource *resource )
{
    struct surface *s = get_surface( resource );

    if (!s) return;
    unmap_toplevel( s );
    s->xdg_toplevel = NULL;
    s->configured = false;
    damage();
}

static void xdg_surface_get_toplevel( struct wl_client *client, struct wl_resource *resource, uint32_t id )
{
    struct surface *s = get_surface( resource );
    struct wl_resource *toplevel = wl_resource_create( client, &xdg_toplevel_interface,
                                                       wl_resource_get_version( resource ), id );

    if (!toplevel)
    {
        wl_client_post_no_memory( client );
        return;
    }
    wl_resource_set_implementation( toplevel, &toplevel_impl, s, toplevel_destroyed );
    if (!s) return;
    s->xdg_toplevel = toplevel;
    s->configured = false;
    s->maximized = s->fullscreen = false;
}

static void xdg_surface_get_popup( struct wl_client *client, struct wl_resource *resource, uint32_t id,
                                   struct wl_resource *parent, struct wl_resource *positioner )
{
    struct wl_resource *popup = wl_resource_create( client, &xdg_popup_interface,
                                                    wl_resource_get_version( resource ), id );

    if (!popup)
    {
        wl_client_post_no_memory( client );
        return;
    }
    wl_resource_set_implementation( popup, &popup_impl, NULL, NULL );
    xdg_popup_send_popup_done( popup ); /* Wine's driver uses subsurfaces for menus instead */
}

static void xdg_surface_set_window_geometry( struct wl_client *client, struct wl_resource *resource,
                                             int32_t x, int32_t y, int32_t width, int32_t height )
{
}

static void xdg_surface_ack_configure( struct wl_client *client, struct wl_resource *resource, uint32_t serial )
{
}

static const struct xdg_surface_interface xdg_surface_impl =
{
    .destroy = resource_destroy,
    .get_toplevel = xdg_surface_get_toplevel,
    .get_popup = xdg_surface_get_popup,
    .set_window_geometry = xdg_surface_set_window_geometry,
    .ack_configure = xdg_surface_ack_configure,
};

static void xdg_surface_destroyed( struct wl_resource *resource )
{
    struct surface *s = get_surface( resource );
    if (s) s->xdg_surface = NULL;
}

static void wm_base_create_positioner( struct wl_client *client, struct wl_resource *resource, uint32_t id )
{
    struct wl_resource *positioner = wl_resource_create( client, &xdg_positioner_interface,
                                                         wl_resource_get_version( resource ), id );

    if (!positioner)
    {
        wl_client_post_no_memory( client );
        return;
    }
    wl_resource_set_implementation( positioner, &positioner_impl, NULL, NULL );
}

static void wm_base_get_xdg_surface( struct wl_client *client, struct wl_resource *resource, uint32_t id,
                                     struct wl_resource *surface )
{
    struct surface *s = get_surface( surface );
    struct wl_resource *xdg = wl_resource_create( client, &xdg_surface_interface,
                                                  wl_resource_get_version( resource ), id );

    if (!xdg)
    {
        wl_client_post_no_memory( client );
        return;
    }
    wl_resource_set_implementation( xdg, &xdg_surface_impl, s, xdg_surface_destroyed );
    s->xdg_surface = xdg;
}

static void wm_base_pong( struct wl_client *client, struct wl_resource *resource, uint32_t serial )
{
}

static const struct xdg_wm_base_interface wm_base_impl =
{
    .destroy = resource_destroy,
    .create_positioner = wm_base_create_positioner,
    .get_xdg_surface = wm_base_get_xdg_surface,
    .pong = wm_base_pong,
};

static void bind_wm_base( struct wl_client *client, void *data, uint32_t version, uint32_t id )
{
    struct wl_resource *resource = wl_resource_create( client, &xdg_wm_base_interface, version, id );
    if (resource) wl_resource_set_implementation( resource, &wm_base_impl, NULL, NULL );
}

/**********************************************************************
 *          arctic_shell_v1
 */

static const struct arctic_window_v1_interface arctic_window_impl =
{
    .destroy = resource_destroy,
};

static void arctic_window_destroyed( struct wl_resource *resource )
{
    struct surface *s = get_surface( resource );

    if (!s) return;
    s->hwnd = 0;
    s->arctic_window = NULL;
    damage();
}

static void shell_get_window( struct wl_client *client, struct wl_resource *resource, uint32_t id,
                              struct wl_resource *surface, uint32_t hwnd )
{
    struct surface *s = get_surface( surface );
    struct wl_resource *window = wl_resource_create( client, &arctic_window_v1_interface, 1, id );

    if (!window)
    {
        wl_client_post_no_memory( client );
        return;
    }
    wl_resource_set_implementation( window, &arctic_window_impl, s, arctic_window_destroyed );
    s->hwnd = hwnd;
    s->arctic_window = window;
    damage();
}

static const struct arctic_shell_v1_interface shell_impl =
{
    .destroy = resource_destroy,
    .get_window = shell_get_window,
};

static void bind_shell( struct wl_client *client, void *data, uint32_t version, uint32_t id )
{
    struct wl_resource *resource = wl_resource_create( client, &arctic_shell_v1_interface, version, id );
    if (resource) wl_resource_set_implementation( resource, &shell_impl, NULL, NULL );
}

/**********************************************************************
 *          Monitors: wl_output, xdg_output and arctic_display_v1
 */

static struct output *output_from_resource( struct wl_resource *resource )
{
    return resource ? wl_resource_get_user_data( resource ) : NULL;
}

static void output_send_state( struct output *output, struct wl_resource *only )
{
    struct kms_output *o = output->kms;
    struct wl_resource *resource;
    const struct kms_mode *mode;

    if (!o || !o->enabled) return;
    mode = &o->modes[o->mode];

    wl_resource_for_each( resource, &output->resources )
    {
        if (only && resource != only) continue;
        /* a monitor that does not say its size is taken for 96 dpi */
        wl_output_send_geometry( resource, o->x, o->y,
                                 o->mm_width ? o->mm_width : mode->info.hdisplay * 254 / 960,
                                 o->mm_height ? o->mm_height : mode->info.vdisplay * 254 / 960,
                                 WL_OUTPUT_SUBPIXEL_UNKNOWN, "Arctic", o->name, WL_OUTPUT_TRANSFORM_NORMAL );
        for (uint32_t i = 0; i < o->mode_count; i++)
            wl_output_send_mode( resource, (i == o->mode ? WL_OUTPUT_MODE_CURRENT : 0) |
                                 (i == o->preferred ? WL_OUTPUT_MODE_PREFERRED : 0),
                                 o->modes[i].info.hdisplay, o->modes[i].info.vdisplay, o->modes[i].refresh );
        if (wl_resource_get_version( resource ) >= WL_OUTPUT_SCALE_SINCE_VERSION)
            wl_output_send_scale( resource, 1 );
    }

    wl_resource_for_each( resource, &output->xdg_resources )
    {
        zxdg_output_v1_send_logical_position( resource, o->x, o->y );
        zxdg_output_v1_send_logical_size( resource, mode->info.hdisplay, mode->info.vdisplay );
        if (wl_resource_get_version( resource ) < ZXDG_OUTPUT_V1_DONE_SINCE_VERSION)
            zxdg_output_v1_send_done( resource );
    }

    wl_resource_for_each( resource, &output->resources )
    {
        if (only && resource != only) continue;
        if (wl_resource_get_version( resource ) >= WL_OUTPUT_DONE_SINCE_VERSION)
            wl_output_send_done( resource );
    }
}

static void output_resource_destroyed( struct wl_resource *resource )
{
    wl_list_remove( wl_resource_get_link( resource ) );
}

static const struct wl_output_interface output_impl =
{
    .release = resource_destroy,
};

static void bind_output( struct wl_client *client, void *data, uint32_t version, uint32_t id )
{
    struct wl_resource *resource = wl_resource_create( client, &wl_output_interface, version, id );
    struct output *output = data;

    if (!resource) return;
    wl_resource_set_implementation( resource, &output_impl, output, output_resource_destroyed );
    wl_list_insert( &output->resources, wl_resource_get_link( resource ) );
    output_send_state( output, resource );
}

/* the clients see a monitor once it shows something */
static void update_globals(void)
{
    for (int i = 0; i < KMS_MAX_OUTPUTS; i++)
    {
        struct kms_output *o = &kms.outputs[i];
        struct output *output = o->user;

        if (!o->connector_id) continue;
        if (o->enabled && !output)
        {
            if (!(output = calloc( 1, sizeof(*output) ))) continue;
            output->kms = o;
            wl_list_init( &output->resources );
            wl_list_init( &output->xdg_resources );
            wl_list_insert( outputs.prev, &output->link );
            o->user = output;
            output->global = wl_global_create( display, &wl_output_interface, 2, output, bind_output );
        }
        else if (!o->enabled && output)
        {
            compositor_output_removed( o );
        }
    }
}

/* a monitor that was unplugged or turned off: it stops being a global, and
 * what a client still holds of it goes quiet until the client lets it go */
void compositor_output_removed( struct kms_output *o )
{
    struct output *output = o->user;

    if (!output) return;
    gl_output_gone( output );
    o->user = NULL;
    output->kms = NULL;
    /* while the card is replaced the clients keep seeing the monitors they
     * had: a desktop with no monitor at all is not something Windows has */
    if (parking)
    {
        wl_list_remove( &output->link );
        wl_list_insert( parked_outputs.prev, &output->link );
        damage();
        return;
    }
    if (output->global) wl_global_remove( output->global );
    output->expire = now_ms() + 3000;
    wl_list_remove( &output->link );
    wl_list_insert( dying_outputs.prev, &output->link );
    damage();
}

static void reap_outputs(void)
{
    struct wl_resource *resource, *next_resource;
    struct output *output, *next;

    if (parked_expire && (int32_t)(now_ms() - parked_expire) >= 0)
    {
        parked_expire = 0;
        wl_list_for_each_safe( output, next, &parked_outputs, link )
        {
            if (output->global) wl_global_remove( output->global );
            output->expire = now_ms() + 3000;
            wl_list_remove( &output->link );
            wl_list_insert( dying_outputs.prev, &output->link );
        }
    }

    wl_list_for_each_safe( output, next, &dying_outputs, link )
    {
        if ((int32_t)(now_ms() - output->expire) < 0) continue;
        wl_resource_for_each_safe( resource, next_resource, &output->resources )
        {
            wl_resource_set_user_data( resource, NULL );
            wl_list_remove( wl_resource_get_link( resource ) );
            wl_list_init( wl_resource_get_link( resource ) );
        }
        wl_resource_for_each_safe( resource, next_resource, &output->xdg_resources )
        {
            wl_resource_set_user_data( resource, NULL );
            wl_list_remove( wl_resource_get_link( resource ) );
            wl_list_init( wl_resource_get_link( resource ) );
        }
        if (output->global) wl_global_destroy( output->global );
        wl_list_remove( &output->link );
        free( output );
    }
}

/**********************************************************************
 *          xdg_output
 */

static void xdg_output_destroyed( struct wl_resource *resource )
{
    wl_list_remove( wl_resource_get_link( resource ) );
}

static const struct zxdg_output_v1_interface xdg_output_impl =
{
    .destroy = resource_destroy,
};

static void xdg_output_manager_get( struct wl_client *client, struct wl_resource *manager,
                                    uint32_t id, struct wl_resource *output_resource )
{
    struct output *output = output_from_resource( output_resource );
    struct wl_resource *resource;
    uint32_t version = wl_resource_get_version( manager );

    if (!(resource = wl_resource_create( client, &zxdg_output_v1_interface, version, id ))) return;
    wl_resource_set_implementation( resource, &xdg_output_impl, output, xdg_output_destroyed );
    if (!output || !output->kms)
    {
        wl_list_init( wl_resource_get_link( resource ) );
        return;
    }
    wl_list_insert( &output->xdg_resources, wl_resource_get_link( resource ) );
    if (version >= ZXDG_OUTPUT_V1_NAME_SINCE_VERSION)
    {
        zxdg_output_v1_send_name( resource, output->kms->name );
        zxdg_output_v1_send_description( resource, output->kms->name );
    }
    output_send_state( output, NULL );
}

static const struct zxdg_output_manager_v1_interface xdg_output_manager_impl =
{
    .destroy = resource_destroy,
    .get_xdg_output = xdg_output_manager_get,
};

static void bind_xdg_output_manager( struct wl_client *client, void *data, uint32_t version, uint32_t id )
{
    struct wl_resource *resource = wl_resource_create( client, &zxdg_output_manager_v1_interface, version, id );
    if (resource) wl_resource_set_implementation( resource, &xdg_output_manager_impl, NULL, NULL );
}

/**********************************************************************
 *          arctic_display_v1
 */

/* Modes and positions for every monitor at once, the way
 * ChangeDisplaySettingsEx changes them. */
static bool apply_configuration( const struct dwm_output_config *configs, uint32_t count, bool test, bool persist )
{
    struct output *output;

    if (!kms_apply( configs, count, test )) return false;
    if (test) return true;

    update_globals();
    update_screen();
    wl_list_for_each( output, &outputs, link ) output_send_state( output, NULL );
    /* the monitors of the new card are up: those of the old one go a moment
     * later, once the clients have taken the new ones in, the way a monitor
     * unplugged next to another goes */
    if (!wl_list_empty( &parked_outputs ) && !wl_list_empty( &outputs ) && !parked_expire)
        parked_expire = now_ms() + 1000;
    if (persist) pending_events |= DWM_EVENT_SAVE;
    damage();
    return true;
}

struct configuration
{
    struct dwm_output_config configs[KMS_MAX_OUTPUTS];
    uint32_t                 count;
    bool                     invalid;
};

static void configuration_set_mode( struct wl_client *client, struct wl_resource *resource,
                                    struct wl_resource *output_resource, int32_t x, int32_t y,
                                    int32_t width, int32_t height, int32_t refresh )
{
    struct configuration *config = wl_resource_get_user_data( resource );
    struct output *output = output_from_resource( output_resource );
    struct dwm_output_config *entry = NULL;

    if (!output || !output->kms || width <= 0 || height <= 0)
    {
        config->invalid = true;
        return;
    }
    for (uint32_t i = 0; i < config->count; i++)
        if (config->configs[i].id == output->kms->connector_id) entry = &config->configs[i];
    if (!entry && config->count < ARRAY_SIZE(config->configs)) entry = &config->configs[config->count++];
    if (!entry)
    {
        config->invalid = true;
        return;
    }
    entry->id = output->kms->connector_id;
    entry->enabled = 1;
    entry->x = x;
    entry->y = y;
    entry->width = width;
    entry->height = height;
    entry->refresh = refresh;
}

static void configuration_apply( struct wl_client *client, struct wl_resource *resource, uint32_t flags )
{
    struct configuration *config = wl_resource_get_user_data( resource );
    bool ok;

    ok = !config->invalid && config->count &&
         apply_configuration( config->configs, config->count, !!(flags & ARCTIC_CONFIGURATION_V1_APPLY_FLAGS_TEST),
                              !!(flags & ARCTIC_CONFIGURATION_V1_APPLY_FLAGS_PERSIST) );
    config->count = 0;
    config->invalid = false;
    if (ok) arctic_configuration_v1_send_succeeded( resource );
    else arctic_configuration_v1_send_failed( resource );
}

static void configuration_destroyed( struct wl_resource *resource )
{
    free( wl_resource_get_user_data( resource ) );
}

static const struct arctic_configuration_v1_interface configuration_impl =
{
    .destroy = resource_destroy,
    .set_mode = configuration_set_mode,
    .apply = configuration_apply,
};

static const struct arctic_output_v1_interface arctic_output_impl =
{
    .destroy = resource_destroy,
};

static void display_get_output( struct wl_client *client, struct wl_resource *display_resource,
                                uint32_t id, struct wl_resource *output_resource )
{
    struct output *output = output_from_resource( output_resource );
    struct wl_resource *resource;
    struct wl_array edid;

    if (!(resource = wl_resource_create( client, &arctic_output_v1_interface,
                                         wl_resource_get_version( display_resource ), id )))
        return;
    wl_resource_set_implementation( resource, &arctic_output_impl, NULL, NULL );
    wl_array_init( &edid );
    if (output && output->kms && output->kms->edid_len)
    {
        void *data = wl_array_add( &edid, output->kms->edid_len );
        if (data) memcpy( data, output->kms->edid, output->kms->edid_len );
    }
    arctic_output_v1_send_edid( resource, &edid );
    wl_array_release( &edid );
}

static void display_create_configuration( struct wl_client *client, struct wl_resource *display_resource,
                                          uint32_t id )
{
    struct configuration *config = calloc( 1, sizeof(*config) );
    struct wl_resource *resource;

    if (!config) return;
    if (!(resource = wl_resource_create( client, &arctic_configuration_v1_interface,
                                         wl_resource_get_version( display_resource ), id )))
    {
        free( config );
        return;
    }
    wl_resource_set_implementation( resource, &configuration_impl, config, configuration_destroyed );
}

static const struct arctic_display_v1_interface display_impl =
{
    .destroy = resource_destroy,
    .get_output = display_get_output,
    .create_configuration = display_create_configuration,
};

static void bind_display( struct wl_client *client, void *data, uint32_t version, uint32_t id )
{
    struct wl_resource *resource = wl_resource_create( client, &arctic_display_v1_interface, version, id );

    if (!resource) return;
    wl_resource_set_implementation( resource, &display_impl, NULL, NULL );
    arctic_display_v1_send_adapter( resource, kms.vendor, kms.device, kms.subsystem, kms.revision,
                                    kms.description );
}

/**********************************************************************
 *          Unix calls
 */

static struct wl_event_source *kms_source, *switch_timer;
static int switch_attempts;

/* The card that shows the desktop changed: a GPU driver took the screen over
 * from simpledrm after dwm started, the way Windows moves from the basic
 * display adapter to the card's own driver without a restart. A new card
 * needs a moment before udev lets us open it and its monitors are probed. */
static int switch_card( void *data )
{
    if (kms.fd >= 0) return 0;
    if (!kms_reopen())
    {
        if (++switch_attempts < 40) wl_event_source_timer_update( switch_timer, 250 );
        else ERR( "no display card to go on with\n" );
        return 0;
    }
    switch_attempts = 0;
    kms_source = wl_event_loop_add_fd( wl_display_get_event_loop( display ), kms.fd, WL_EVENT_READABLE,
                                       kms_event, NULL );
    set_cursor_image();
    gl_start();
    /* the Windows side gives the monitors of the new card their modes */
    update_globals();
    update_screen();
    pending_events |= DWM_EVENT_HOTPLUG;
    damage();
    return 0;
}

static int hotplug_event( int fd, uint32_t mask, void *data )
{
    switch (kms_hotplug_event( fd ))
    {
    case KMS_EVENT_NONE:
        return 0;
    case KMS_EVENT_CARD_GONE:
        if (kms_source) wl_event_source_remove( kms_source );
        kms_source = NULL;
        parking = true;
        kms_close();
        parking = false;
        /* the card that replaces it may be there already */
        /* fall through */
    case KMS_EVENT_CARD_ADDED:
        if (kms.fd < 0)
        {
            switch_attempts = 0;
            wl_event_source_timer_update( switch_timer, 250 );
        }
        return 0;
    case KMS_EVENT_MONITORS:
        break;
    }
    if (!kms_probe()) return 0;

    /* a monitor that went stops showing at once; a new one waits for the
     * Windows side to give it a mode (dwmcore.c) */
    update_globals();
    update_screen();
    pending_events |= DWM_EVENT_HOTPLUG;
    damage();
    return 0;
}

static NTSTATUS dwm_start( void *args )
{
    const struct dwm_start_params *params = args;
    struct wl_event_loop *loop;
    int hotplug;

    /* COLORREF is 0x00BBGGRR, the framebuffer wants 0x00RRGGBB */
    background = 0xff000000 | ((params->background & 0xff) << 16) | (params->background & 0xff00) |
                 ((params->background >> 16) & 0xff);

    if (kms_open()) return STATUS_DEVICE_NOT_CONNECTED;

    wl_list_init( &all_surfaces );
    wl_list_init( &toplevels );
    wl_list_init( &outputs );
    wl_list_init( &dying_outputs );
    wl_list_init( &parked_outputs );
    wl_list_init( &window_states );
    if (!(display = wl_display_create())) return STATUS_UNSUCCESSFUL;
    wl_display_init_shm( display );
    wl_global_create( display, &wl_compositor_interface, 4, NULL, bind_compositor );
    wl_global_create( display, &wl_subcompositor_interface, 1, NULL, bind_subcompositor );
    wl_global_create( display, &wp_viewporter_interface, 1, NULL, bind_viewporter );
    wl_global_create( display, &xdg_wm_base_interface, 2, NULL, bind_wm_base );
    wl_global_create( display, &zxdg_output_manager_v1_interface, 3, NULL, bind_xdg_output_manager );
    wl_global_create( display, &arctic_shell_v1_interface, 1, NULL, bind_shell );
    wl_global_create( display, &arctic_display_v1_interface, 1, NULL, bind_display );
    dmabuf_init( display );

    if (wl_display_add_socket( display, SOCKET_NAME ))
    {
        ERR( "cannot create the socket %s (XDG_RUNTIME_DIR=%s)\n", SOCKET_NAME, getenv( "XDG_RUNTIME_DIR" ) );
        return STATUS_UNSUCCESSFUL;
    }
    loop = wl_display_get_event_loop( display );
    frame_timer = wl_event_loop_add_timer( loop, frame_tick, NULL );
    wl_event_source_timer_update( frame_timer, 1 );
    kms_source = wl_event_loop_add_fd( loop, kms.fd, WL_EVENT_READABLE, kms_event, NULL );
    switch_timer = wl_event_loop_add_timer( loop, switch_card, NULL );
    set_cursor_image();
    gl_start();
    if ((hotplug = kms_hotplug_socket()) >= 0)
        wl_event_loop_add_fd( loop, hotplug, WL_EVENT_READABLE, hotplug_event, NULL );
    MESSAGE( "dwm: serving %s/%s\n", getenv( "XDG_RUNTIME_DIR" ), SOCKET_NAME );
    return STATUS_SUCCESS;
}

/* serves clients and composes (on the frame timer) for up to timeout_ms;
 * between calls the PE side asks wineserver about the windows */
static NTSTATUS dwm_dispatch( void *args )
{
    struct dwm_dispatch_params *params = args;

    wl_event_loop_dispatch( wl_display_get_event_loop( display ), (int)params->timeout_ms );
    repaint();
    wl_display_flush_clients( display );
    reap_outputs();
    params->events = pending_events;
    pending_events = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS dwm_set_windows( void *args )
{
    const struct dwm_set_windows_params *params = args;
    struct dwm_window *copy = NULL;

    if (params->count && !(copy = malloc( params->count * sizeof(*copy) ))) return STATUS_NO_MEMORY;
    if (copy) memcpy( copy, params->windows, params->count * sizeof(*copy) );
    free( windows );
    windows = copy;
    window_count = params->count;
    update_window_states();
    update_snap( params );
    damage();
    return STATUS_SUCCESS;
}

static NTSTATUS dwm_set_attributes( void *args )
{
    const struct dwm_set_attributes_params *params = args;
    struct arctic_dwm_entry *copy = NULL;
    struct window_state *ws;

    if (params->count && !(copy = malloc( params->count * sizeof(*copy) ))) return STATUS_NO_MEMORY;
    if (copy) memcpy( copy, params->entries, params->count * sizeof(*copy) );
    free( attributes );
    attributes = copy;
    attribute_count = params->count;
    wl_list_for_each( ws, &window_states, link ) apply_attributes( ws );
    /* a window cloaked or uncloaked comes or goes as if hidden or shown */
    if (windows_known) update_window_states();
    damage();
    return STATUS_SUCCESS;
}

static NTSTATUS dwm_set_cursor( void *args )
{
    const struct dwm_set_cursor_params *params = args;

    struct output *output;

    cursor_x = params->x;
    cursor_y = params->y;
    cursor_hidden = params->hidden;
    update_cursor_image();  /* another monitor, another scale */
    /* monitors showing a window directly move the cursor plane; the others compose */
    wl_list_for_each( output, &outputs, link )
    {
        struct kms_output *o = output->kms;

        if (!o || !o->enabled) continue;
        if (!output->direct_hwnd) output->needs_frame = true;
        else if (!kms_show_cursor( o, cursor_on( o ), cursor_x - o->x, cursor_y - o->y ))
        {
            hw_cursor = false;
            output->needs_frame = true;
        }
    }
    dirty = true;
    return STATUS_SUCCESS;
}

static NTSTATUS dwm_get_outputs( void *args )
{
    struct dwm_get_outputs_params *params = args;

    params->count = 0;
    for (int i = 0; i < KMS_MAX_OUTPUTS; i++)
    {
        struct kms_output *o = &kms.outputs[i];
        struct dwm_output *info = &params->outputs[params->count];

        if (!o->connector_id) continue;
        memset( info, 0, sizeof(*info) );
        info->id = o->connector_id;
        strcpy( info->name, o->name );
        info->internal = o->connector_type == 7 /* LVDS */ || o->connector_type == 14 /* eDP */ ||
                         o->connector_type == 16 /* DSI */;
        info->enabled = o->enabled;
        info->x = o->x;
        info->y = o->y;
        info->mode = o->mode;
        info->mode_count = min( o->mode_count, DWM_MAX_MODES );
        for (uint32_t k = 0; k < info->mode_count; k++)
        {
            info->modes[k].width = o->modes[k].info.hdisplay;
            info->modes[k].height = o->modes[k].info.vdisplay;
            info->modes[k].refresh = o->modes[k].refresh;
            info->modes[k].flags = k == o->preferred ? DWM_MODE_PREFERRED : 0;
        }
        info->vrr_capable = o->vrr_capable;
        info->edid_len = min( o->edid_len, DWM_MAX_EDID );
        memcpy( info->edid, o->edid, info->edid_len );
        params->count++;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS dwm_set_config( void *args )
{
    const struct dwm_set_config_params *params = args;

    return apply_configuration( params->configs, params->count, false, false ) ?
           STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

static NTSTATUS dwm_set_options( void *args )
{
    const struct dwm_set_options_params *params = args;

    if (vrr_allowed != !!params->vrr) MESSAGE( "dwm: variable refresh %s\n", params->vrr ? "allowed" : "not allowed" );
    vrr_allowed = !!params->vrr;
    vrr_off_count = min( params->vrr_off_count, ARRAY_SIZE(vrr_off) );
    memcpy( vrr_off, params->vrr_off, vrr_off_count * sizeof(*vrr_off) );
    damage();
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    dwm_start,
    dwm_dispatch,
    dwm_set_windows,
    dwm_set_cursor,
    dwm_get_outputs,
    dwm_set_config,
    dwm_set_options,
    dwm_set_attributes,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_funcs) == unix_funcs_count );
