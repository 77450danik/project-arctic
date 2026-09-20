/*
 * Arctic desktop composition engine: display ownership (DRM/KMS)
 *
 * Takes DRM master and sets up one dumb framebuffer on the first connected
 * output. Works on any KMS driver, simpledrm included, so it needs no GPU
 * driver: this is the CPU ("basic") composition path.
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
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
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

#include "dwmcore_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(dwm);

struct kms_output kms = { .fd = -1 };

static uint32_t conn_id, crtc_id, fb_id;
static struct drm_mode_modeinfo mode;

static void *zalloc( uint32_t count, size_t size )
{
    return calloc( count ? count : 1, size );
}

/* first connected connector, its preferred mode and a CRTC that can drive it */
static BOOL pick_output( int fd )
{
    struct drm_mode_card_res res = {0};
    uint32_t *conns, *crtcs, *encs, *fbs;
    BOOL found = FALSE;

    if (ioctl( fd, DRM_IOCTL_MODE_GETRESOURCES, &res )) return FALSE;
    conns = zalloc( res.count_connectors, 4 );
    crtcs = zalloc( res.count_crtcs, 4 );
    encs = zalloc( res.count_encoders, 4 );
    fbs = zalloc( res.count_fbs, 4 );
    res.connector_id_ptr = (uintptr_t)conns;
    res.crtc_id_ptr = (uintptr_t)crtcs;
    res.encoder_id_ptr = (uintptr_t)encs;
    res.fb_id_ptr = (uintptr_t)fbs;
    if (ioctl( fd, DRM_IOCTL_MODE_GETRESOURCES, &res )) goto done;

    for (uint32_t i = 0; i < res.count_connectors && !found; i++)
    {
        struct drm_mode_get_connector conn = { .connector_id = conns[i] };
        struct drm_mode_modeinfo *modes;
        uint32_t *cencs;

        if (ioctl( fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn ) || conn.connection != 1 || !conn.count_modes)
            continue;
        modes = zalloc( conn.count_modes, sizeof(*modes) );
        cencs = zalloc( conn.count_encoders, 4 );
        conn.modes_ptr = (uintptr_t)modes;
        conn.encoders_ptr = (uintptr_t)cencs;
        conn.count_props = 0;
        conn.props_ptr = 0;
        conn.prop_values_ptr = 0;
        if (!ioctl( fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn ) && conn.count_modes)
        {
            uint32_t enc_id = conn.encoder_id ? conn.encoder_id : (conn.count_encoders ? cencs[0] : 0);
            struct drm_mode_get_encoder enc = { .encoder_id = enc_id };
            uint32_t m = 0;

            for (uint32_t k = 0; k < conn.count_modes; k++)
                if (modes[k].type & DRM_MODE_TYPE_PREFERRED) { m = k; break; }
            if (enc_id && !ioctl( fd, DRM_IOCTL_MODE_GETENCODER, &enc ))
            {
                uint32_t crtc = enc.crtc_id;
                for (uint32_t k = 0; !crtc && k < res.count_crtcs; k++)
                    if (enc.possible_crtcs & (1u << k)) crtc = crtcs[k];
                if (crtc)
                {
                    conn_id = conn.connector_id;
                    crtc_id = crtc;
                    mode = modes[m];
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

static BOOL create_framebuffer( int fd )
{
    struct drm_mode_create_dumb create = { .width = mode.hdisplay, .height = mode.vdisplay, .bpp = 32 };
    struct drm_mode_fb_cmd fb = {0};
    struct drm_mode_map_dumb map = {0};
    void *pixels;

    if (ioctl( fd, DRM_IOCTL_MODE_CREATE_DUMB, &create )) return FALSE;
    fb.width = create.width;
    fb.height = create.height;
    fb.pitch = create.pitch;
    fb.bpp = 32;
    fb.depth = 24;
    fb.handle = create.handle;
    if (ioctl( fd, DRM_IOCTL_MODE_ADDFB, &fb )) return FALSE;
    map.handle = create.handle;
    if (ioctl( fd, DRM_IOCTL_MODE_MAP_DUMB, &map )) return FALSE;
    pixels = mmap( NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)map.offset );
    if (pixels == MAP_FAILED) return FALSE;

    memset( pixels, 0, create.size );
    kms.width = create.width;
    kms.height = create.height;
    kms.pitch = create.pitch;
    kms.pixels = pixels;
    kms.refresh_mhz = (mode.vrefresh ? mode.vrefresh : 60) * 1000;
    fb_id = fb.fb_id;
    return TRUE;
}

static BOOL show_framebuffer( int fd )
{
    struct drm_mode_crtc set =
    {
        .set_connectors_ptr = (uintptr_t)&conn_id,
        .count_connectors = 1,
        .crtc_id = crtc_id,
        .fb_id = fb_id,
        .mode_valid = 1,
        .mode = mode,
    };
    return !ioctl( fd, DRM_IOCTL_MODE_SETCRTC, &set );
}

/* one pass over the cards; with report, says why each one did not do */
static BOOL try_cards( BOOL report )
{
    for (int i = 0; i < 8; i++)
    {
        int fd;

        snprintf( kms.name, sizeof(kms.name), "/dev/dri/card%d", i );
        if ((fd = open( kms.name, O_RDWR | O_CLOEXEC )) < 0)
        {
            if (report && errno != ENOENT) ERR( "%s: %s\n", kms.name, strerror( errno ) );
            continue;
        }
        if (!pick_output( fd ))
        {
            if (report) ERR( "%s: no connected output\n", kms.name );
        }
        else if (!create_framebuffer( fd ))
        {
            if (report) ERR( "%s: cannot create a framebuffer: %s\n", kms.name, strerror( errno ) );
        }
        else if (!show_framebuffer( fd ))
        {
            if (report) ERR( "%s: cannot set the mode (DRM master held elsewhere?)\n", kms.name );
        }
        else
        {
            kms.fd = fd;
            MESSAGE( "dwm: display %s, %ux%u@%u\n", kms.name, kms.width, kms.height, kms.refresh_mhz / 1000 );
            return TRUE;
        }
        close( fd );
    }
    return FALSE;
}

/* a GPU driver may still be probing its outputs: up to 10 s of retries */
int kms_init(void)
{
    for (int attempt = 0; attempt < 40; attempt++)
    {
        if (try_cards( attempt == 39 )) return 0;
        usleep( 250000 );
    }
    return -1;
}

void kms_flush(void)
{
    struct drm_mode_fb_dirty_cmd dirty = { .fb_id = fb_id };

    ioctl( kms.fd, DRM_IOCTL_MODE_DIRTYFB, &dirty );
}
