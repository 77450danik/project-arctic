/*
 * Arctic desktop composition engine: GPU buffers of the clients
 *
 * zwp_linux_dmabuf_v1, through which Vulkan (DXVK, vkd3d-proton) and OpenGL
 * hand over what they rendered without a copy. Only linear XRGB8888 and
 * ARGB8888 are offered for now: the CPU composition can read those, and the
 * monitor can scan them out as they are when one fills it. The main device
 * is the card dwm drives, so a program that renders on another GPU (the
 * discrete one of a laptop) gets its frames copied over by its own driver.
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

#include <drm/drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include <wayland-server.h>

#include "linux-dmabuf-v1-server-protocol.h"

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "wine/debug.h"

#include "dwmcore_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(dwm);

static const uint32_t formats[] = { DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888 };

/* the format table of the feedback: (format, 0, modifier) for each */
struct format_entry
{
    uint32_t format;
    uint32_t pad;
    uint64_t modifier;
};

static int    format_table = -1;
static size_t format_table_size;

/**********************************************************************
 *          Buffers
 */

static void buffer_free( struct dmabuf *buffer )
{
    gl_dmabuf_gone( buffer );
    if (buffer->fb && buffer->fb_generation == kms.generation) kms_remove_dmabuf( buffer->fb );
    if (buffer->map) munmap( buffer->map, buffer->map_size );
    close( buffer->fd );
    free( buffer );
}

void dmabuf_ref( struct dmabuf *buffer )
{
    buffer->refs++;
}

void dmabuf_unref( struct dmabuf *buffer )
{
    if (--buffer->refs) return;
    if (buffer->resource) wl_buffer_send_release( buffer->resource );
    else buffer_free( buffer );
}

static void buffer_destroy( struct wl_client *client, struct wl_resource *resource )
{
    wl_resource_destroy( resource );
}

static const struct wl_buffer_interface buffer_impl =
{
    .destroy = buffer_destroy,
};

static void buffer_destroyed( struct wl_resource *resource )
{
    struct dmabuf *buffer = wl_resource_get_user_data( resource );

    buffer->resource = NULL;
    if (!buffer->refs) buffer_free( buffer );
}

struct dmabuf *dmabuf_from_resource( struct wl_resource *resource )
{
    if (!resource || !wl_resource_instance_of( resource, &wl_buffer_interface, &buffer_impl )) return NULL;
    return wl_resource_get_user_data( resource );
}

/* Copies the buffer for the CPU composition. The sync waits for the GPU to
 * finish drawing it (implicit fences), as a scanout would. */
bool dmabuf_read( struct dmabuf *buffer, uint32_t *pixels )
{
    struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };

    /* video memory is read by the GPU: the CPU often may not map it at all */
    if (!buffer->gpu_failed)
    {
        if (gpu_read_dmabuf( buffer, pixels )) return true;
        buffer->gpu_failed = true;
    }
    if (buffer->map_failed) return false;
    if (!buffer->map)
    {
        buffer->map_size = buffer->offset + (size_t)buffer->stride * buffer->height;
        buffer->map = mmap( NULL, buffer->map_size, PROT_READ, MAP_SHARED, buffer->fd, 0 );
        if (buffer->map == MAP_FAILED)
        {
            ERR( "cannot map a %ux%u buffer to compose it: %s\n", buffer->width, buffer->height, strerror( errno ) );
            buffer->map = NULL;
            buffer->map_failed = true;
            return false;
        }
    }
    ioctl( buffer->fd, DMA_BUF_IOCTL_SYNC, &sync );
    for (uint32_t y = 0; y < buffer->height; y++)
        memcpy( pixels + (size_t)y * buffer->width,
                (const uint8_t *)buffer->map + buffer->offset + (size_t)y * buffer->stride, (size_t)buffer->width * 4 );
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    ioctl( buffer->fd, DMA_BUF_IOCTL_SYNC, &sync );
    return true;
}

uint32_t dmabuf_fb( struct dmabuf *buffer )
{
    /* made on a card that is gone: this one tries afresh */
    if (buffer->fb_generation != kms.generation)
    {
        buffer->fb = 0;
        buffer->fb_failed = false;
        buffer->scanout_tested = 0;
        buffer->fb_generation = kms.generation;
    }
    if (!buffer->fb && !buffer->fb_failed)
    {
        buffer->fb = kms_add_dmabuf( buffer->fd, buffer->width, buffer->height, buffer->format, buffer->offset,
                                     buffer->stride, buffer->modifier );
        buffer->fb_failed = !buffer->fb;
    }
    return buffer->fb;
}

/**********************************************************************
 *          zwp_linux_buffer_params_v1
 */

struct params
{
    int      fds[4];
    uint32_t offsets[4], strides[4];
    uint64_t modifiers[4];
    bool     used;
};

static void params_destroy( struct wl_client *client, struct wl_resource *resource )
{
    wl_resource_destroy( resource );
}

static void params_destroyed( struct wl_resource *resource )
{
    struct params *params = wl_resource_get_user_data( resource );

    for (int i = 0; i < 4; i++) if (params->fds[i] >= 0) close( params->fds[i] );
    free( params );
}

static void params_add( struct wl_client *client, struct wl_resource *resource, int32_t fd, uint32_t plane,
                        uint32_t offset, uint32_t stride, uint32_t modifier_hi, uint32_t modifier_lo )
{
    struct params *params = wl_resource_get_user_data( resource );

    if (params->used)
    {
        wl_resource_post_error( resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED, "params already used" );
        close( fd );
        return;
    }
    if (plane >= 4)
    {
        wl_resource_post_error( resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX, "plane %u", plane );
        close( fd );
        return;
    }
    if (params->fds[plane] >= 0)
    {
        wl_resource_post_error( resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET, "plane %u set twice", plane );
        close( fd );
        return;
    }
    params->fds[plane] = fd;
    params->offsets[plane] = offset;
    params->strides[plane] = stride;
    params->modifiers[plane] = (uint64_t)modifier_hi << 32 | modifier_lo;
}

/* a buffer from the params, or NULL with *error set (-1: not one we take) */
static struct dmabuf *params_buffer( struct params *params, int32_t width, int32_t height, uint32_t format,
                                     uint32_t flags, int *error )
{
    struct dmabuf *buffer;
    off_t size;
    int i;

    *error = ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE;
    if (params->fds[0] < 0) return NULL;
    *error = ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_DIMENSIONS;
    if (width <= 0 || height <= 0 || params->strides[0] < (uint32_t)width * 4) return NULL;

    /* one plane of a format we offered, linear, drawn the right way up */
    *error = -1;
    for (i = 0; i < ARRAY_SIZE(formats); i++) if (formats[i] == format) break;
    if (i == ARRAY_SIZE(formats) || flags || params->fds[1] >= 0) return NULL;
    if (params->modifiers[0] != DRM_FORMAT_MOD_LINEAR && params->modifiers[0] != DRM_FORMAT_MOD_INVALID) return NULL;

    *error = ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS;
    size = lseek( params->fds[0], 0, SEEK_END );
    if (size >= 0 && params->offsets[0] + (uint64_t)params->strides[0] * height > (uint64_t)size) return NULL;

    if (!(buffer = calloc( 1, sizeof(*buffer) ))) return NULL;
    buffer->fd = params->fds[0];
    params->fds[0] = -1;
    buffer->width = width;
    buffer->height = height;
    buffer->format = format;
    buffer->offset = params->offsets[0];
    buffer->stride = params->strides[0];
    buffer->modifier = params->modifiers[0];
    buffer->alpha = format == DRM_FORMAT_ARGB8888;
    return buffer;
}

static struct dmabuf *create_buffer( struct wl_client *client, struct wl_resource *resource, uint32_t id,
                                     int32_t width, int32_t height, uint32_t format, uint32_t flags )
{
    struct params *params = wl_resource_get_user_data( resource );
    struct dmabuf *buffer;
    int error;

    if (params->used)
    {
        wl_resource_post_error( resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED, "params already used" );
        return NULL;
    }
    params->used = true;
    if (!(buffer = params_buffer( params, width, height, format, flags, &error )))
    {
        if (error >= 0) wl_resource_post_error( resource, error, "%dx%d buffer rejected", width, height );
        else
        {
            static unsigned int logged;
            if (logged++ < 8)
                ERR( "a %dx%d buffer of format %.4s, modifier %#llx is not taken\n", width, height,
                     (const char *)&format, (unsigned long long)params->modifiers[0] );
        }
        return NULL;
    }
    if (!(buffer->resource = wl_resource_create( client, &wl_buffer_interface, 1, id )))
    {
        buffer_free( buffer );
        wl_client_post_no_memory( client );
        return NULL;
    }
    wl_resource_set_implementation( buffer->resource, &buffer_impl, buffer, buffer_destroyed );
    return buffer;
}

static void params_create( struct wl_client *client, struct wl_resource *resource, int32_t width,
                           int32_t height, uint32_t format, uint32_t flags )
{
    struct dmabuf *buffer = create_buffer( client, resource, 0, width, height, format, flags );

    if (buffer) zwp_linux_buffer_params_v1_send_created( resource, buffer->resource );
    else zwp_linux_buffer_params_v1_send_failed( resource );
}

static void params_create_immed( struct wl_client *client, struct wl_resource *resource, uint32_t id,
                                 int32_t width, int32_t height, uint32_t format, uint32_t flags )
{
    if (!create_buffer( client, resource, id, width, height, format, flags ))
        wl_resource_post_error( resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_WL_BUFFER,
                                "a %dx%d buffer cannot be imported", width, height );
}

static const struct zwp_linux_buffer_params_v1_interface params_impl =
{
    .destroy = params_destroy,
    .add = params_add,
    .create = params_create,
    .create_immed = params_create_immed,
};

/**********************************************************************
 *          zwp_linux_dmabuf_v1 and its feedback
 */

static void resource_destroy( struct wl_client *client, struct wl_resource *resource )
{
    wl_resource_destroy( resource );
}

static const struct zwp_linux_dmabuf_feedback_v1_interface feedback_impl =
{
    .destroy = resource_destroy,
};

static void send_feedback( struct wl_client *client, struct wl_resource *dmabuf, uint32_t id )
{
    struct wl_resource *resource;
    struct wl_array device, indices;
    dev_t devnum = kms.devnum;
    uint16_t *index;

    if (!(resource = wl_resource_create( client, &zwp_linux_dmabuf_feedback_v1_interface,
                                         wl_resource_get_version( dmabuf ), id )))
    {
        wl_client_post_no_memory( client );
        return;
    }
    wl_resource_set_implementation( resource, &feedback_impl, NULL, NULL );

    wl_array_init( &device );
    wl_array_init( &indices );
    if ((index = wl_array_add( &device, sizeof(devnum) ))) memcpy( index, &devnum, sizeof(devnum) );
    for (uint16_t i = 0; i < ARRAY_SIZE(formats); i++)
        if ((index = wl_array_add( &indices, sizeof(*index) ))) *index = i;

    zwp_linux_dmabuf_feedback_v1_send_format_table( resource, format_table, format_table_size );
    zwp_linux_dmabuf_feedback_v1_send_main_device( resource, &device );
    zwp_linux_dmabuf_feedback_v1_send_tranche_target_device( resource, &device );
    zwp_linux_dmabuf_feedback_v1_send_tranche_flags( resource, 0 );
    zwp_linux_dmabuf_feedback_v1_send_tranche_formats( resource, &indices );
    zwp_linux_dmabuf_feedback_v1_send_tranche_done( resource );
    zwp_linux_dmabuf_feedback_v1_send_done( resource );
    wl_array_release( &device );
    wl_array_release( &indices );
}

static void dmabuf_create_params( struct wl_client *client, struct wl_resource *resource, uint32_t id )
{
    struct params *params = calloc( 1, sizeof(*params) );
    struct wl_resource *params_resource;

    if (!params || !(params_resource = wl_resource_create( client, &zwp_linux_buffer_params_v1_interface,
                                                           wl_resource_get_version( resource ), id )))
    {
        free( params );
        wl_client_post_no_memory( client );
        return;
    }
    for (int i = 0; i < 4; i++) params->fds[i] = -1;
    wl_resource_set_implementation( params_resource, &params_impl, params, params_destroyed );
}

static void dmabuf_get_default_feedback( struct wl_client *client, struct wl_resource *resource, uint32_t id )
{
    send_feedback( client, resource, id );
}

/* every surface gets what every other does, until scanout formats are offered */
static void dmabuf_get_surface_feedback( struct wl_client *client, struct wl_resource *resource, uint32_t id,
                                         struct wl_resource *surface )
{
    send_feedback( client, resource, id );
}

static const struct zwp_linux_dmabuf_v1_interface dmabuf_impl =
{
    .destroy = resource_destroy,
    .create_params = dmabuf_create_params,
    .get_default_feedback = dmabuf_get_default_feedback,
    .get_surface_feedback = dmabuf_get_surface_feedback,
};

static void bind_dmabuf( struct wl_client *client, void *data, uint32_t version, uint32_t id )
{
    struct wl_resource *resource = wl_resource_create( client, &zwp_linux_dmabuf_v1_interface, version, id );

    if (!resource)
    {
        wl_client_post_no_memory( client );
        return;
    }
    wl_resource_set_implementation( resource, &dmabuf_impl, NULL, NULL );
    /* before the feedback (version 4), formats are told on binding */
    if (version >= ZWP_LINUX_DMABUF_V1_GET_DEFAULT_FEEDBACK_SINCE_VERSION) return;
    for (int i = 0; i < ARRAY_SIZE(formats); i++)
    {
        if (version >= ZWP_LINUX_DMABUF_V1_MODIFIER_SINCE_VERSION)
            zwp_linux_dmabuf_v1_send_modifier( resource, formats[i], DRM_FORMAT_MOD_LINEAR >> 32,
                                               DRM_FORMAT_MOD_LINEAR & 0xffffffff );
        else
            zwp_linux_dmabuf_v1_send_format( resource, formats[i] );
    }
}

void dmabuf_init( struct wl_display *display )
{
    struct format_entry table[ARRAY_SIZE(formats)];

    for (int i = 0; i < ARRAY_SIZE(formats); i++)
    {
        table[i].format = formats[i];
        table[i].pad = 0;
        table[i].modifier = DRM_FORMAT_MOD_LINEAR;
    }
    /* the table is read by clients through a sealed memfd they map */
    format_table_size = sizeof(table);
    if ((format_table = memfd_create( "dwm-dmabuf-formats", MFD_CLOEXEC | MFD_ALLOW_SEALING )) < 0 ||
        write( format_table, table, sizeof(table) ) != sizeof(table) ||
        fcntl( format_table, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL ))
    {
        ERR( "no dmabuf format table: %s\n", strerror( errno ) );
        return;
    }
    wl_global_create( display, &zwp_linux_dmabuf_v1_interface, 4, NULL, bind_dmabuf );
}
