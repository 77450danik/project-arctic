/*
 * Arctic desktop composition engine: the compositor
 *
 * Serves window buffers over the Wayland wire protocol on
 * $XDG_RUNTIME_DIR/arctic-0 and composes them on the CPU into the KMS
 * framebuffer. This step speaks the standard protocols Wine's own Wayland
 * driver needs (wl_compositor, wl_shm, wl_subcompositor, xdg_wm_base,
 * wp_viewporter, wl_output), so the compositor can be tested before
 * winearctic.drv exists. Toplevels are cascaded for now; with
 * winearctic.drv their places come from wineserver (patch 0001).
 *
 * Buffers are copied at commit and released at once, so clients never wait
 * on the compositor; the copy is what the next frame is composed from.
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

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-server.h>

#include "xdg-shell-server-protocol.h"
#include "viewporter-server-protocol.h"

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
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
    bool                alpha;               /* premultiplied ARGB */
    int                 dst_width, dst_height; /* wp_viewport destination, 0 if unset */
    struct wl_resource *viewport;

    /* subsurface role */
    struct wl_resource *subsurface;
    struct surface     *parent;
    struct wl_list      child_link;
    int                 sub_x, sub_y;
    struct wl_list      children;            /* bottom to top */

    /* toplevel role */
    struct wl_resource *xdg_surface, *xdg_toplevel;
    bool                configured, mapped;
    int                 x, y;
    struct wl_list      stack_link;          /* toplevels, bottom to top */
};

static struct wl_display      *display;
static struct wl_event_source *frame_timer;
static struct wl_list          all_surfaces;
static struct wl_list          toplevels;
static uint32_t                background;   /* XRGB */
static uint32_t               *shadow;       /* frame composed in RAM, then copied out */
static bool                    dirty = true;
static int                     cascade;

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

static void blit( const struct surface *s, int x0, int y0 )
{
    int dw = s->dst_width ? s->dst_width : s->width;
    int dh = s->dst_height ? s->dst_height : s->height;

    for (int y = 0; y < dh; y++)
    {
        int py = y0 + y;
        const uint32_t *src;
        uint32_t *dst;

        if (py < 0 || py >= (int)kms.height) continue;
        src = s->pixels + (size_t)(dh == s->height ? y : s->height * y / dh) * s->width;
        dst = shadow + (size_t)py * kms.width;
        for (int x = 0; x < dw; x++)
        {
            int px = x0 + x;
            uint32_t p;

            if (px < 0 || px >= (int)kms.width) continue;
            p = src[dw == s->width ? x : s->width * x / dw];
            if (!s->alpha || (p >> 24) == 0xff) dst[px] = p | 0xff000000;
            else if (p >> 24) dst[px] = over( p, dst[px] );
        }
    }
}

static void draw_tree( const struct surface *s, int x, int y )
{
    const struct surface *child;

    if (s->pixels) blit( s, x, y );
    wl_list_for_each( child, &s->children, child_link )
        draw_tree( child, x + child->sub_x, y + child->sub_y );
}

static void compose(void)
{
    struct surface *s;
    size_t count = (size_t)kms.width * kms.height;

    for (size_t i = 0; i < count; i++) shadow[i] = background;
    wl_list_for_each( s, &toplevels, stack_link ) draw_tree( s, s->x, s->y );
    for (uint32_t y = 0; y < kms.height; y++)
        memcpy( (uint8_t *)kms.pixels + (size_t)y * kms.pitch, shadow + (size_t)y * kms.width, kms.width * 4 );
    kms_flush();
}

static int frame_tick( void *data )
{
    struct wl_resource *cb, *tmp;
    struct surface *s;
    uint32_t time = now_ms();

    if (dirty)
    {
        compose();
        dirty = false;
    }
    wl_list_for_each( s, &all_surfaces, all_link )
    {
        wl_resource_for_each_safe( cb, tmp, &s->frames )
        {
            wl_callback_send_done( cb, time );
            wl_resource_destroy( cb );
        }
    }
    wl_event_source_timer_update( frame_timer, 1000000 / (kms.refresh_mhz ? kms.refresh_mhz : 60000) );
    return 0;
}

/**********************************************************************
 *          Toplevels
 */

static void map_toplevel( struct surface *s )
{
    int step = 32 * (cascade++ % 12);

    s->x = 48 + step;
    s->y = 48 + step;
    if (s->x + s->width > (int)kms.width) s->x = 0;
    if (s->y + s->height > (int)kms.height) s->y = 0;
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

    wl_array_init( &states );
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

static void surface_commit( struct wl_client *client, struct wl_resource *resource )
{
    struct surface *s = get_surface( resource );

    if (s->pending_attach)
    {
        if (s->pending_buffer)
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
        else if (s->pixels && !s->mapped) map_toplevel( s );
        else if (!s->pixels) unmap_toplevel( s );
    }
    dirty = true;
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
    free( s->pixels );
    free( s );
    dirty = true;
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
    dirty = true;
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
    dirty = true;
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
    dirty = true;
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
}

static void viewport_set_destination( struct wl_client *client, struct wl_resource *resource,
                                      int32_t width, int32_t height )
{
    struct surface *s = get_surface( resource );

    if (!s) return;
    s->dst_width = width > 0 ? width : 0;
    s->dst_height = height > 0 ? height : 0;
    dirty = true;
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
    s->dst_width = s->dst_height = 0;
    dirty = true;
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

static void toplevel_set_fullscreen( struct wl_client *client, struct wl_resource *resource,
                                     struct wl_resource *output )
{
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
    .set_maximized = toplevel_void,
    .unset_maximized = toplevel_void,
    .set_fullscreen = toplevel_set_fullscreen,
    .unset_fullscreen = toplevel_void,
    .set_minimized = toplevel_void,
};

static void toplevel_destroyed( struct wl_resource *resource )
{
    struct surface *s = get_surface( resource );

    if (!s) return;
    unmap_toplevel( s );
    s->xdg_toplevel = NULL;
    s->configured = false;
    dirty = true;
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
 *          wl_output
 */

static const struct wl_output_interface output_impl =
{
    .release = resource_destroy,
};

static void bind_output( struct wl_client *client, void *data, uint32_t version, uint32_t id )
{
    struct wl_resource *resource = wl_resource_create( client, &wl_output_interface, version, id );

    if (!resource) return;
    wl_resource_set_implementation( resource, &output_impl, NULL, NULL );
    /* physical size as if 96 dpi, until EDID is read */
    wl_output_send_geometry( resource, 0, 0, kms.width * 254 / 960, kms.height * 254 / 960,
                             WL_OUTPUT_SUBPIXEL_UNKNOWN, "Arctic", "Display", WL_OUTPUT_TRANSFORM_NORMAL );
    wl_output_send_mode( resource, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
                         kms.width, kms.height, kms.refresh_mhz );
    if (version >= WL_OUTPUT_SCALE_SINCE_VERSION) wl_output_send_scale( resource, 1 );
    if (version >= WL_OUTPUT_DONE_SINCE_VERSION) wl_output_send_done( resource );
}

/**********************************************************************
 *          Unix calls
 */

static NTSTATUS dwm_start( void *args )
{
    const struct dwm_start_params *params = args;
    struct wl_event_loop *loop;

    /* COLORREF is 0x00BBGGRR, the framebuffer wants 0x00RRGGBB */
    background = 0xff000000 | ((params->background & 0xff) << 16) | (params->background & 0xff00) |
                 ((params->background >> 16) & 0xff);

    if (kms_init()) return STATUS_DEVICE_NOT_CONNECTED;
    if (!(shadow = malloc( (size_t)kms.width * kms.height * 4 ))) return STATUS_NO_MEMORY;

    wl_list_init( &all_surfaces );
    wl_list_init( &toplevels );
    if (!(display = wl_display_create())) return STATUS_UNSUCCESSFUL;
    wl_display_init_shm( display );
    wl_global_create( display, &wl_compositor_interface, 4, NULL, bind_compositor );
    wl_global_create( display, &wl_subcompositor_interface, 1, NULL, bind_subcompositor );
    wl_global_create( display, &wp_viewporter_interface, 1, NULL, bind_viewporter );
    wl_global_create( display, &xdg_wm_base_interface, 2, NULL, bind_wm_base );
    wl_global_create( display, &wl_output_interface, 2, NULL, bind_output );

    if (wl_display_add_socket( display, SOCKET_NAME ))
    {
        ERR( "cannot create the socket %s (XDG_RUNTIME_DIR=%s)\n", SOCKET_NAME, getenv( "XDG_RUNTIME_DIR" ) );
        return STATUS_UNSUCCESSFUL;
    }
    loop = wl_display_get_event_loop( display );
    frame_timer = wl_event_loop_add_timer( loop, frame_tick, NULL );
    wl_event_source_timer_update( frame_timer, 1 );
    MESSAGE( "dwm: serving %s/%s\n", getenv( "XDG_RUNTIME_DIR" ), SOCKET_NAME );
    return STATUS_SUCCESS;
}

/* never returns: the calling thread becomes the compositor's event loop */
static NTSTATUS dwm_run( void *args )
{
    wl_display_run( display );
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    dwm_start,
    dwm_run,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_funcs) == unix_funcs_count );
