/*
 * Arctic desktop composition engine: display ownership (DRM/KMS)
 *
 * First step of the compositor: take DRM master, set up a dumb framebuffer
 * on the first connected output and paint the desktop colour. Works on any
 * KMS driver, simpledrm included, so it needs no GPU driver.
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

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "wine/debug.h"

#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(dwm);

struct output
{
    int      fd;
    uint32_t conn_id, crtc_id, fb_id;
    uint32_t width, height, pitch;
    size_t   size;
    uint8_t *pixels;
    struct drm_mode_modeinfo mode;
};

static struct output output = { .fd = -1 };

static void *zalloc( uint32_t count, size_t size )
{
    return calloc( count ? count : 1, size );
}

/* first connected connector, its preferred mode and a CRTC that can drive it */
static BOOL pick_output( struct output *out )
{
    struct drm_mode_card_res res = {0};
    uint32_t *conns, *crtcs, *encs, *fbs;
    BOOL found = FALSE;

    if (ioctl( out->fd, DRM_IOCTL_MODE_GETRESOURCES, &res )) return FALSE;
    conns = zalloc( res.count_connectors, 4 );
    crtcs = zalloc( res.count_crtcs, 4 );
    encs = zalloc( res.count_encoders, 4 );
    fbs = zalloc( res.count_fbs, 4 );
    res.connector_id_ptr = (uintptr_t)conns;
    res.crtc_id_ptr = (uintptr_t)crtcs;
    res.encoder_id_ptr = (uintptr_t)encs;
    res.fb_id_ptr = (uintptr_t)fbs;
    if (ioctl( out->fd, DRM_IOCTL_MODE_GETRESOURCES, &res )) goto done;

    for (uint32_t i = 0; i < res.count_connectors && !found; i++)
    {
        struct drm_mode_get_connector conn = { .connector_id = conns[i] };
        struct drm_mode_modeinfo *modes;
        uint32_t *cencs;

        if (ioctl( out->fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn ) || conn.connection != 1 || !conn.count_modes)
            continue;
        modes = zalloc( conn.count_modes, sizeof(*modes) );
        cencs = zalloc( conn.count_encoders, 4 );
        conn.modes_ptr = (uintptr_t)modes;
        conn.encoders_ptr = (uintptr_t)cencs;
        conn.count_props = 0;
        conn.props_ptr = 0;
        conn.prop_values_ptr = 0;
        if (!ioctl( out->fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn ) && conn.count_modes)
        {
            uint32_t enc_id = conn.encoder_id ? conn.encoder_id : (conn.count_encoders ? cencs[0] : 0);
            struct drm_mode_get_encoder enc = { .encoder_id = enc_id };
            uint32_t m = 0;

            for (uint32_t k = 0; k < conn.count_modes; k++)
                if (modes[k].type & DRM_MODE_TYPE_PREFERRED) { m = k; break; }
            if (enc_id && !ioctl( out->fd, DRM_IOCTL_MODE_GETENCODER, &enc ))
            {
                uint32_t crtc = enc.crtc_id;
                for (uint32_t k = 0; !crtc && k < res.count_crtcs; k++)
                    if (enc.possible_crtcs & (1u << k)) crtc = crtcs[k];
                if (crtc)
                {
                    out->conn_id = conn.connector_id;
                    out->crtc_id = crtc;
                    out->mode = modes[m];
                    found = TRUE;
                }
            }
        }
        free( modes );
        free( cencs );
    }
done:
    free( conns );
    free( crtcs );
    free( encs );
    free( fbs );
    return found;
}

static BOOL create_framebuffer( struct output *out )
{
    struct drm_mode_create_dumb create = { .width = out->mode.hdisplay, .height = out->mode.vdisplay, .bpp = 32 };
    struct drm_mode_fb_cmd fb = {0};
    struct drm_mode_map_dumb map = {0};
    void *pixels;

    if (ioctl( out->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create )) return FALSE;
    fb.width = create.width;
    fb.height = create.height;
    fb.pitch = create.pitch;
    fb.bpp = 32;
    fb.depth = 24;
    fb.handle = create.handle;
    if (ioctl( out->fd, DRM_IOCTL_MODE_ADDFB, &fb )) return FALSE;
    map.handle = create.handle;
    if (ioctl( out->fd, DRM_IOCTL_MODE_MAP_DUMB, &map )) return FALSE;
    pixels = mmap( NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, out->fd, (off_t)map.offset );
    if (pixels == MAP_FAILED) return FALSE;

    out->width = create.width;
    out->height = create.height;
    out->pitch = create.pitch;
    out->size = create.size;
    out->pixels = pixels;
    out->fb_id = fb.fb_id;
    return TRUE;
}

static BOOL show_framebuffer( struct output *out )
{
    struct drm_mode_crtc set =
    {
        .set_connectors_ptr = (uintptr_t)&out->conn_id,
        .count_connectors = 1,
        .crtc_id = out->crtc_id,
        .fb_id = out->fb_id,
        .mode_valid = 1,
        .mode = out->mode,
    };
    return !ioctl( out->fd, DRM_IOCTL_MODE_SETCRTC, &set );
}

static void fill( struct output *out, uint32_t xrgb )
{
    for (uint32_t y = 0; y < out->height; y++)
    {
        uint32_t *row = (uint32_t *)(out->pixels + (size_t)y * out->pitch);
        for (uint32_t x = 0; x < out->width; x++) row[x] = xrgb;
    }
}

static NTSTATUS dwm_start( void *args )
{
    const struct dwm_start_params *params = args;
    /* COLORREF is 0x00BBGGRR, the framebuffer wants 0x00RRGGBB */
    uint32_t color = ((params->background & 0xff) << 16) | (params->background & 0xff00) |
                     ((params->background >> 16) & 0xff);
    char card[32];

    for (int i = 0; i < 8; i++)
    {
        snprintf( card, sizeof(card), "/dev/dri/card%d", i );
        if ((output.fd = open( card, O_RDWR | O_CLOEXEC )) < 0) continue;
        if (pick_output( &output ) && create_framebuffer( &output ))
        {
            fill( &output, color );
            if (show_framebuffer( &output ))
            {
                MESSAGE( "dwm: desktop on %s, %ux%u\n", card, output.width, output.height );
                return STATUS_SUCCESS;
            }
            ERR( "%s: cannot set the mode (DRM master held elsewhere?)\n", card );
        }
        close( output.fd );
        output.fd = -1;
    }
    return STATUS_DEVICE_NOT_CONNECTED;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    dwm_start,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_funcs) == unix_funcs_count );
