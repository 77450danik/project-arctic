/*
 * Arctic desktop composition engine: what the compositor and its GPU
 * renderer share
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_DWMCORE_COMPOSITOR_H
#define __WINE_DWMCORE_COMPOSITOR_H

#include <wayland-server.h>

#include "windef.h"
#include "winternl.h"
#include "dwmcore_private.h"
#include "unixlib.h"

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

    /* on the GPU: the pixels as a texture */
    uint32_t            texture;             /* 0 if none */
    uint32_t            texture_generation;  /* of the GL context it was made in */
    int                 texture_width, texture_height;
    bool                texture_stale;       /* the pixels changed since the upload */

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

struct gl_output;

/* one monitor, as the clients see it */
struct output
{
    struct kms_output *kms;                  /* NULL once it is unplugged */
    struct wl_global  *global;
    struct wl_list     link;                 /* outputs, then parked_outputs or dying_outputs */
    struct wl_list     resources;            /* wl_output */
    struct wl_list     xdg_resources;        /* zxdg_output_v1 */
    uint32_t           expire;               /* when an unplugged monitor is let go */
    bool               needs_frame;          /* something it shows changed */
    uint64_t           not_before;           /* µs: flips that finish at once are paced */
    uint32_t           direct_hwnd;          /* the window it shows directly, not composed */
    /* frames of a window that fills it, for the log: how a game really runs */
    uint32_t           stat_direct, stat_composed;
    bool               stat_vrr;
    uint64_t           stat_since;           /* µs */
    struct gl_output  *gl;                   /* its frames on the GPU */
};

/* a rectangle in screen coordinates, fractional while it moves */
struct frect
{
    float left, top, right, bottom;
};

/* p' = p * scale + offset: where a window is drawn while it moves or zooms */
struct xform
{
    float sx, sy, tx, ty;
};

/* what is drawn outside a rounded rectangle is cut off */
struct clip
{
    struct frect rect;
    float        radius;                     /* 0: square corners, and nothing cut */
};

enum layer_mode
{
    LAYER_OPAQUE,                            /* XRGB: alpha is ignored */
    LAYER_PREMULTIPLIED,                     /* ARGB */
    LAYER_KEYED,                             /* over a backdrop: GDI pixels with no alpha count by brightness */
};

/* a picture of a window: what it showed before it went, or what it shows
 * now, drawn once to be faded or zoomed as a whole */
struct window_image
{
    uint32_t texture, framebuffer;
    uint32_t generation;
    int      width, height;
    uint32_t *pixels;                        /* on the CPU: width x height */
};

enum animation
{
    ANIM_NONE,
    ANIM_OPEN,                               /* a window with a caption appears */
    ANIM_CLOSE,                              /* ... and goes */
    ANIM_MINIMIZE,                           /* to its taskbar button */
    ANIM_RESTORE,                            /* back from it */
    ANIM_MORPH,                              /* maximized or restored: from one rectangle to another */
    ANIM_POPUP_IN,                           /* a menu, a tooltip, a flyout */
    ANIM_POPUP_OUT,
    ANIM_SLIDE_IN,                           /* up from below, the Start menu */
    ANIM_SLIDE_OUT,
    ANIM_FADE_IN,
    ANIM_FADE_OUT,
};

/* what the compositor keeps of a Win32 window between lists */
struct window_state
{
    struct wl_list      link;
    uint32_t            hwnd;
    uint32_t            list_serial;         /* of the last list it was in */
    uint64_t            anim_created;        /* µs: an animation waits for the window's first frame */
    struct dwm_window   last;                /* as it was last listed */
    bool                listed;              /* in the list now */
    bool                shown;               /* visible and not minimized, last time */
    struct arctic_dwm_window attr;           /* from dwmapi */
    bool                has_attr;

    enum animation      anim;
    uint64_t            anim_start;          /* µs */
    uint32_t            anim_duration;       /* µs */
    struct frect        anim_from, anim_to;  /* the window rectangle, animated */
    float               alpha_from, alpha_to;
    bool                ghost;               /* drawn from the snapshot: the window itself is gone */

    struct window_image snapshot;            /* the last thing it showed, at its size */
    RECT                snapshot_rect;       /* where it showed it */
    bool                snapshot_valid;
    struct window_image scratch;             /* what it shows now, while it animates */
};

/* compositor.c */
extern struct wl_list       all_surfaces;
extern struct dwm_window   *windows;         /* from wineserver, topmost first */
extern uint32_t             window_count;
extern uint32_t             background;      /* XRGB */
extern int                  cursor_x, cursor_y;
extern bool                 cursor_hidden;
extern const char           arrow[20][13];

/* gl.c */
bool gl_start(void);                         /* a GL context on the card: composition on the GPU */
bool gl_active(void);
bool gl_render( struct output *output, bool vrr );  /* composes and queues a flip; false if it could not */
void gl_output_gone( struct output *output );
bool gl_frame_released( void *buffer );      /* true if the buffer was one of our frames */
void gl_surface_gone( struct surface *s );
void gl_dmabuf_gone( struct dmabuf *buffer );
void gl_image_free( struct window_image *image );
bool gl_snapshot( struct window_state *ws, const struct dwm_window *w );

/* compositor.c, for gl.c: how each window is drawn this frame */
struct window_draw
{
    const struct dwm_window *w;
    struct window_state     *ws;             /* NULL for a window without effects */
    struct xform             xform;          /* from where wineserver has it to where it is drawn */
    struct frect             shown;          /* the window rectangle as drawn */
    float                    opacity;
    struct clip              clip;
    bool                     from_snapshot;  /* the window is gone: its snapshot stands in */
    bool                     as_image;       /* drawn once into an image, then faded or zoomed */
    bool                     backdrop;       /* blurred or tinted behind it */
    bool                     blur;
    uint32_t                 tint;           /* 0xAARRGGBB, straight alpha */
    enum layer_mode          (*mode)( const struct surface *s, const struct window_draw *draw );
};

uint32_t scene_windows( struct window_draw *draws, uint32_t max );  /* bottom to top */
void     scene_static_draw( struct window_draw *draw, const struct dwm_window *w, struct window_state *ws );
void     scene_unowned_surfaces( void (*callback)( struct surface *s, int x, int y, void *ctx ), void *ctx );
void     scene_surfaces( const struct window_draw *draw,
                         void (*callback)( struct surface *s, int x, int y, void *ctx ), void *ctx );
struct surface *surface_for_window( uint32_t hwnd );
struct window_state *window_state( uint32_t hwnd );
bool     window_is_shown_directly( uint32_t hwnd );

/* thumbnails drawn in a window */
struct thumbnail_draw
{
    const struct dwm_window *source;
    struct window_state     *source_state;
    struct frect             dest;           /* screen coordinates */
    RECT                     source_rect;    /* of the source window, relative to its window rect */
    float                    opacity;
    bool                     from_snapshot;  /* minimized or hidden: what it showed last */
};
uint32_t scene_thumbnails( const struct window_draw *draw, struct thumbnail_draw *thumbs, uint32_t max );

#endif
