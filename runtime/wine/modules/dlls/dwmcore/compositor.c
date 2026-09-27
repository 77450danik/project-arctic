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

#include "dwmcore_private.h"
#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(dwm);

#define SOCKET_NAME "arctic-0"

struct surface
{
    struct wl_resource *resource;
    struct wl_list      all_link;            /* all_surfaces */

    /* pending state, applied on commit */
    struct wl_resource *pending_buffer;
    struct wl_listener  pending_buffer_destroy;
    bool                pending_attach;
    struct wl_list      pending_frames;      /* wl_callback resources */
    struct wl_list      frames;

    /* committed content */
    uint32_t           *pixels;
    int                 width, height;
    struct dmabuf      *dmabuf;              /* a GPU buffer, kept while it is the content */
    bool                stale;               /* pixels are older than the dmabuf */
    bool                alpha;               /* premultiplied ARGB */
    int                 src_x, src_y, src_width, src_height; /* wp_viewport source, width 0 if unset */
    int                 dst_width, dst_height; /* wp_viewport destination, 0 if unset */
    struct wl_resource *viewport;

    /* subsurface role */
    struct wl_resource *subsurface;
    struct surface     *parent;
    struct wl_list      child_link;
    int                 sub_x, sub_y;
    struct wl_list      children;            /* bottom to top */

    /* the Win32 window it shows, 0 if none (arctic_shell_v1) */
    uint32_t            hwnd;
    struct wl_resource *arctic_window;

    /* toplevel role */
    struct wl_resource *xdg_surface, *xdg_toplevel;
    bool                configured, mapped;
    bool                maximized, fullscreen;
    int                 x, y;
    struct wl_list      stack_link;          /* toplevels, bottom to top */
};

/* one monitor, as the clients see it */
struct output
{
    struct kms_output *kms;                  /* NULL once it is unplugged */
    struct wl_global  *global;
    struct wl_list     link;                 /* outputs, then dying_outputs */
    struct wl_list     resources;            /* wl_output */
    struct wl_list     xdg_resources;        /* zxdg_output_v1 */
    uint32_t           expire;               /* when an unplugged monitor is let go */
    bool               needs_frame;          /* something it shows changed */
    uint64_t           not_before;           /* µs: flips that finish at once are paced */
    uint32_t           direct_hwnd;          /* the window it shows directly, not composed */
};

static struct wl_display      *display;
static struct wl_event_source *frame_timer;
static struct wl_list          all_surfaces;
static struct wl_list          toplevels;
static struct wl_list          outputs;      /* struct output */
static struct wl_list          dying_outputs;  /* unplugged, until their clients let go */
static uint32_t                background;   /* XRGB */
static uint32_t               *shadow;       /* frame composed in RAM, then copied out */
static RECT                    screen;       /* the virtual screen every monitor is placed in */
static bool                    dirty = true;
static int                     cascade;
static struct dwm_window      *windows;      /* from wineserver, topmost first */
static uint32_t                window_count;
static int                     cursor_x, cursor_y;
static bool                    cursor_hidden;  /* the window under it hides it */
static uint32_t                pending_events;  /* DWM_EVENT_*, for the PE side */
static bool                    vrr_allowed = true;  /* the Windows setting */
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

/* the part of the buffer shown (viewport source) scaled to the viewport destination */
static void blit( const struct surface *s, int x0, int y0 )
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
            if (!s->alpha || (p >> 24) == 0xff) dst[px] = p | 0xff000000;
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

static void draw_tree( struct surface *s, int x, int y )
{
    struct surface *child;

    if (s->dmabuf && s->stale) read_dmabuf( s );
    if (s->pixels) blit( s, x, y );
    wl_list_for_each( child, &s->children, child_link )
    {
        if (child->hwnd) continue; /* a window of its own: placed by wineserver, not by its parent */
        draw_tree( child, x + child->sub_x, y + child->sub_y );
    }
}

/* the standard arrow, until cursor shapes come from win32u */
static const char arrow[][13] =
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

static void set_cursor_image(void)
{
    uint32_t argb[ARRAY_SIZE(arrow) * 12] = {0};

    for (int y = 0; y < (int)ARRAY_SIZE(arrow); y++)
        for (int x = 0; arrow[y][x]; x++)
            if (arrow[y][x] != ' ') argb[y * 12 + x] = arrow[y][x] == 'B' ? 0xff000000 : 0xffffffff;
    hw_cursor = kms_set_cursor_image( argb, 12, ARRAY_SIZE(arrow) );
}

static void draw_cursor(void)
{
    if (cursor_hidden) return;
    for (int y = 0; y < (int)ARRAY_SIZE(arrow); y++)
    {
        int py = cursor_y + y;

        if (py < screen.top || py >= screen.bottom) continue;
        for (int x = 0; arrow[y][x]; x++)
        {
            int px = cursor_x + x;

            if (px < screen.left || px >= screen.right || arrow[y][x] == ' ') continue;
            shadow[(size_t)(py - screen.top) * (screen.right - screen.left) + px - screen.left] =
                arrow[y][x] == 'B' ? 0xff000000 : 0xffffffff;
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

        if (!(w->style & WS_VISIBLE) || (w->style & WS_MINIMIZE)) continue;
        if (shown_directly( w->hwnd )) continue;
        if ((s = surface_for_hwnd( w->hwnd ))) draw_tree( s, w->left, w->top );
    }
    wl_list_for_each( s, &toplevels, stack_link )
        if (!s->hwnd) draw_tree( s, s->x, s->y );
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

static void present_directly( struct output *output, struct surface *content, bool vrr )
{
    struct kms_output *o = output->kms;
    struct dmabuf *buffer = content->dmabuf;

    /* the same buffer again (the pointer moved, another window changed): nothing new to show */
    if (o->front_buffer == buffer &&
        o->vrr_on == (vrr && o->vrr_capable && o->vrr_prop && !o->vrr_refused)) return;
    dmabuf_ref( buffer );
    if (kms_present( o, buffer->fb, source_width( content ), source_height( content ), vrr, buffer )) return;
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
        if (kms_present( o, fb->fb_id, fb->width, fb->height, vrr, NULL )) return;
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

/* A frame for every monitor that needs one and has shown its last one. A
 * window that fills a monitor with a GPU buffer turns variable refresh on
 * there: the monitor then waits for the game's frames. */
static void repaint(void)
{
    struct { struct output *output; struct surface *content; bool vrr; } ready[KMS_MAX_OUTPUTS];
    uint64_t now = kms_now();
    struct output *output;
    int count = 0;
    bool composed = false;

    wl_list_for_each( output, &outputs, link )
    {
        struct kms_output *o = output->kms;
        struct surface *content;
        uint32_t hwnd = 0;

        if (!o || !o->enabled || !output->needs_frame || o->queued_fb || now < output->not_before) continue;
        if (count == ARRAY_SIZE(ready)) break;
        content = fullscreen_content( o, &hwnd );
        ready[count].output = output;
        ready[count].vrr = content && vrr_allowed;
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
    if (composed && dirty) compose();

    for (int i = 0; i < count; i++)
    {
        if (ready[i].content) present_directly( ready[i].output, ready[i].content, ready[i].vrr );
        else present_composed( ready[i].output, ready[i].vrr );
    }
}

void compositor_flip_done( struct kms_output *o, void *buffer )
{
    struct output *output = o->user;

    if (buffer) dmabuf_unref( buffer );
    if (output && o->fake_vblank) output->not_before = o->done_time + kms_frame_time( o );
    send_frame_callbacks();
}

void compositor_buffer_unused( void *buffer )
{
    dmabuf_unref( buffer );
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

static void surface_commit( struct wl_client *client, struct wl_resource *resource )
{
    struct surface *s = get_surface( resource );

    if (s->pending_attach)
    {
        struct dmabuf *buffer = dmabuf_from_resource( s->pending_buffer );

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
    o->user = NULL;
    output->kms = NULL;
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

static int hotplug_event( int fd, uint32_t mask, void *data )
{
    if (!kms_hotplug_event( fd )) return 0;
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
    wl_event_loop_add_fd( loop, kms.fd, WL_EVENT_READABLE, kms_event, NULL );
    set_cursor_image();
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
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_funcs) == unix_funcs_count );
