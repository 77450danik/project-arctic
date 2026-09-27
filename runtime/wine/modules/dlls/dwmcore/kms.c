/*
 * Arctic desktop composition engine: display ownership (DRM/KMS)
 *
 * Takes DRM master of the first card with a connected monitor (the one the
 * firmware showed its picture on, if more have one) and drives every
 * connected monitor of it. Frames composed on the CPU go to a pair of dumb
 * framebuffers per monitor, one on screen while the other is drawn, and are
 * shown by page flips; a client's GPU buffer that fills a monitor is flipped
 * to directly, with variable refresh if the monitor has it. Works on any KMS
 * driver, simpledrm included, so it needs no GPU driver: a driver that cannot
 * flip gets its frames drawn where they are shown, as before.
 *
 * Which mode each monitor shows and where it sits is decided on the Windows
 * side (dwmcore.c, or a program through ChangeDisplaySettingsEx); this file
 * reports the monitors with their modes and EDID and sets what it is told.
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
#include <drm/drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "wine/debug.h"

#include "dwmcore_private.h"
#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(dwm);

struct kms_card kms = { .fd = -1 };

/* DRM_MODE_CONNECTOR_*, named as Linux names them */
static const char *const connector_names[] =
{
    "Unknown", "VGA", "DVI-I", "DVI-D", "DVI-A", "Composite", "SVIDEO", "LVDS", "Component", "DIN",
    "DP", "HDMI-A", "HDMI-B", "TV", "eDP", "Virtual", "DSI", "DPI", "Writeback", "SPI", "USB",
};

static void *zalloc( uint32_t count, size_t size )
{
    return calloc( count ? count : 1, size );
}

/* exact refresh from the timings: 59.940 Hz stays 59940, not 60000 */
static uint32_t mode_refresh( const struct drm_mode_modeinfo *m )
{
    uint64_t num, den;

    if (!m->htotal || !m->vtotal) return m->vrefresh * 1000;
    num = (uint64_t)m->clock * 1000000;
    den = (uint64_t)m->htotal * m->vtotal;
    if (m->vscan > 1) den *= m->vscan;
    return (num + den / 2) / den;
}

static uint32_t whole_hz( uint32_t refresh )
{
    return (refresh + 500) / 1000;
}

uint64_t kms_now(void)
{
    struct timespec ts;

    clock_gettime( CLOCK_MONOTONIC, &ts );
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

uint32_t kms_frame_time( const struct kms_output *out )
{
    uint32_t refresh = out->mode_count ? out->modes[out->mode].refresh : 0;
    return refresh ? 1000000000u / refresh : 16667;
}

/* The ids (and values) of the named properties of a KMS object; 0 for those
 * it does not have. */
static void object_properties( uint32_t obj, uint32_t type, const char *const *names, uint32_t count,
                               uint32_t *ids, uint64_t *values )
{
    struct drm_mode_obj_get_properties get = { .obj_id = obj, .obj_type = type };
    uint32_t *props, allocated;
    uint64_t *vals;

    memset( ids, 0, count * sizeof(*ids) );
    if (ioctl( kms.fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &get ) || !get.count_props) return;
    allocated = get.count_props;
    props = zalloc( allocated, sizeof(*props) );
    vals = zalloc( allocated, sizeof(*vals) );
    get.props_ptr = (uintptr_t)props;
    get.prop_values_ptr = (uintptr_t)vals;
    if (props && vals && !ioctl( kms.fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &get ))
    {
        for (uint32_t i = 0; i < min( get.count_props, allocated ); i++)
        {
            struct drm_mode_get_property prop = { .prop_id = props[i] };

            if (ioctl( kms.fd, DRM_IOCTL_MODE_GETPROPERTY, &prop )) continue;
            for (uint32_t k = 0; k < count; k++)
            {
                if (strcmp( prop.name, names[k] )) continue;
                ids[k] = props[i];
                if (values) values[k] = vals[i];
            }
        }
    }
    free( props );
    free( vals );
}

enum { PLANE_FB_ID, PLANE_CRTC_ID, PLANE_SRC_X, PLANE_SRC_Y, PLANE_SRC_W, PLANE_SRC_H,
       PLANE_CRTC_X, PLANE_CRTC_Y, PLANE_CRTC_W, PLANE_CRTC_H, PLANE_TYPE, PLANE_PROP_COUNT };

/* the properties a flip sets, looked up once per plane */
static const uint32_t *plane_properties( uint32_t plane )
{
    static const char *const names[PLANE_PROP_COUNT] =
    {
        "FB_ID", "CRTC_ID", "SRC_X", "SRC_Y", "SRC_W", "SRC_H", "CRTC_X", "CRTC_Y", "CRTC_W", "CRTC_H", "type"
    };
    static struct { uint32_t plane, ids[PLANE_PROP_COUNT]; } cache[32];
    int slot = 0;

    for (int i = 0; i < ARRAY_SIZE(cache); i++)
    {
        if (cache[i].plane == plane) return cache[i].ids;
        if (!cache[i].plane && !slot) slot = i;
    }
    cache[slot].plane = plane;
    object_properties( plane, DRM_MODE_OBJECT_PLANE, names, PLANE_PROP_COUNT, cache[slot].ids, NULL );
    return cache[slot].ids;
}

/* the primary plane the CRTC shows its framebuffer on, after a modeset */
static uint32_t primary_plane( uint32_t crtc )
{
    struct drm_mode_get_plane_res res = {0};
    uint32_t *planes, found = 0;

    if (ioctl( kms.fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &res ) || !res.count_planes) return 0;
    if (!(planes = zalloc( res.count_planes, sizeof(*planes) ))) return 0;
    res.plane_id_ptr = (uintptr_t)planes;
    if (!ioctl( kms.fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &res ))
    {
        for (uint32_t i = 0; i < res.count_planes && !found; i++)
        {
            static const char *const type_name[] = { "type" };
            struct drm_mode_get_plane plane = { .plane_id = planes[i] };
            uint32_t id;
            uint64_t type;

            if (ioctl( kms.fd, DRM_IOCTL_MODE_GETPLANE, &plane ) || plane.crtc_id != crtc) continue;
            object_properties( planes[i], DRM_MODE_OBJECT_PLANE, type_name, 1, &id, &type );
            if (id && type == 1 /* primary */) found = planes[i];
        }
    }
    free( planes );
    return found;
}

/* Windows lists each size once per whole Hz, progressive only: of modes that
 * round to the same Hz, the monitor's preferred one or else the first stays */
static void read_modes( struct kms_output *out, const struct drm_mode_modeinfo *modes, uint32_t count )
{
    bool have_preferred = false;

    out->modes = zalloc( count, sizeof(*out->modes) );
    out->mode_count = out->preferred = 0;
    for (uint32_t i = 0; i < count && out->mode_count < DWM_MAX_MODES; i++)
    {
        const struct drm_mode_modeinfo *m = &modes[i];
        bool preferred = (m->type & DRM_MODE_TYPE_PREFERRED) && !have_preferred;
        uint32_t refresh = mode_refresh( m ), k;

        if (m->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN)) continue;
        if (!m->hdisplay || !m->vdisplay || !refresh) continue;
        for (k = 0; k < out->mode_count; k++)
            if (out->modes[k].info.hdisplay == m->hdisplay && out->modes[k].info.vdisplay == m->vdisplay &&
                whole_hz( out->modes[k].refresh ) == whole_hz( refresh )) break;
        if (k < out->mode_count && !preferred) continue;
        out->modes[k].info = *m;
        out->modes[k].refresh = refresh;
        if (k == out->mode_count) out->mode_count++;
        if (preferred)
        {
            out->preferred = k;
            have_preferred = true;
        }
    }
}

static void read_edid( uint64_t blob_id, struct kms_output *out )
{
    struct drm_mode_get_blob blob = { .blob_id = blob_id };
    void *data;

    if (!blob_id || ioctl( kms.fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob ) || !blob.length) return;
    if (!(data = malloc( blob.length ))) return;
    blob.data = (uintptr_t)data;
    if (ioctl( kms.fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob ) || blob.length > DWM_MAX_EDID)
    {
        free( data );
        return;
    }
    out->edid = data;
    out->edid_len = blob.length;
}

/* the EDID, and whether the monitor and the link take a variable refresh */
static void read_properties( const uint32_t *props, const uint64_t *values, uint32_t count, struct kms_output *out )
{
    for (uint32_t i = 0; i < count; i++)
    {
        struct drm_mode_get_property prop = { .prop_id = props[i] };

        if (ioctl( kms.fd, DRM_IOCTL_MODE_GETPROPERTY, &prop )) continue;
        if (!strcmp( prop.name, "EDID" )) read_edid( values[i], out );
        else if (!strcmp( prop.name, "vrr_capable" )) out->vrr_capable = values[i] != 0;
    }
}

/* A connected monitor as a new slot, not yet in kms.outputs. count_modes 0 on
 * the first call makes the kernel probe the connector again. */
static bool read_connector( uint32_t id, struct kms_output *out )
{
    struct drm_mode_get_connector conn;
    struct drm_mode_modeinfo *modes = NULL;
    uint32_t *encoders = NULL, *props = NULL;
    uint64_t *values = NULL;
    bool ok = false;

    for (int tries = 0; tries < 5 && !ok; tries++)
    {
        uint32_t count_modes, count_encoders, count_props;

        free( modes );
        free( encoders );
        free( props );
        free( values );
        memset( &conn, 0, sizeof(conn) );
        conn.connector_id = id;
        if (ioctl( kms.fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn )) return false;
        if (conn.connection != 1 /* DRM_MODE_CONNECTED */ || !conn.count_modes) return false;

        count_modes = conn.count_modes;
        count_encoders = conn.count_encoders;
        count_props = conn.count_props;
        modes = zalloc( count_modes, sizeof(*modes) );
        encoders = zalloc( count_encoders, sizeof(*encoders) );
        props = zalloc( count_props, sizeof(*props) );
        values = zalloc( count_props, sizeof(*values) );
        conn.modes_ptr = (uintptr_t)modes;
        conn.encoders_ptr = (uintptr_t)encoders;
        conn.props_ptr = (uintptr_t)props;
        conn.prop_values_ptr = (uintptr_t)values;
        if (ioctl( kms.fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn )) break;
        /* the lists grew in between: read them again */
        ok = conn.count_modes <= count_modes && conn.count_encoders <= count_encoders &&
             conn.count_props <= count_props;
    }

    if (ok)
    {
        memset( out, 0, sizeof(*out) );
        out->connector_id = id;
        out->connector_type = conn.connector_type;
        snprintf( out->name, sizeof(out->name), "%s-%u",
                  conn.connector_type < ARRAY_SIZE(connector_names) ? connector_names[conn.connector_type] : "Unknown",
                  conn.connector_type_id );
        out->mm_width = conn.mm_width;
        out->mm_height = conn.mm_height;
        read_modes( out, modes, conn.count_modes );
        read_properties( props, values, conn.count_props, out );

        for (uint32_t i = 0; i < conn.count_encoders; i++)
        {
            struct drm_mode_get_encoder enc = { .encoder_id = encoders[i] };

            if (ioctl( kms.fd, DRM_IOCTL_MODE_GETENCODER, &enc )) continue;
            out->possible_crtcs |= enc.possible_crtcs;
            /* the CRTC firmware lit it with, the first choice for it */
            if (enc.encoder_id == conn.encoder_id && enc.crtc_id) out->crtc_id = enc.crtc_id;
        }
        ok = out->mode_count && out->possible_crtcs;
        if (!ok)
        {
            free( out->modes );
            free( out->edid );
        }
    }
    free( modes );
    free( encoders );
    free( props );
    free( values );
    return ok;
}

static bool create_fb( uint32_t width, uint32_t height, struct kms_fb *fb )
{
    struct drm_mode_create_dumb create = { .width = width, .height = height, .bpp = 32 };
    struct drm_mode_destroy_dumb destroy = {0};
    struct drm_mode_fb_cmd cmd = {0};
    struct drm_mode_map_dumb map = {0};
    void *pixels;

    if (ioctl( kms.fd, DRM_IOCTL_MODE_CREATE_DUMB, &create )) return false;
    cmd.width = create.width;
    cmd.height = create.height;
    cmd.pitch = create.pitch;
    cmd.bpp = 32;
    cmd.depth = 24;
    cmd.handle = create.handle;
    map.handle = create.handle;
    if (ioctl( kms.fd, DRM_IOCTL_MODE_ADDFB, &cmd )) goto failed;
    if (ioctl( kms.fd, DRM_IOCTL_MODE_MAP_DUMB, &map ) ||
        (pixels = mmap( NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, kms.fd, (off_t)map.offset )) == MAP_FAILED)
    {
        ioctl( kms.fd, DRM_IOCTL_MODE_RMFB, &cmd.fb_id );
        goto failed;
    }
    memset( pixels, 0, create.size );
    fb->fb_id = cmd.fb_id;
    fb->handle = create.handle;
    fb->width = create.width;
    fb->height = create.height;
    fb->pitch = create.pitch;
    fb->pixels = pixels;
    fb->size = create.size;
    return true;

failed:
    destroy.handle = create.handle;
    ioctl( kms.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy );
    return false;
}

static void destroy_fb( struct kms_fb *fb )
{
    struct drm_mode_destroy_dumb destroy = { .handle = fb->handle };

    if (!fb->fb_id) return;
    munmap( fb->pixels, fb->size );
    ioctl( kms.fd, DRM_IOCTL_MODE_RMFB, &fb->fb_id );
    ioctl( kms.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy );
    memset( fb, 0, sizeof(*fb) );
}

/* connector 0 turns the CRTC off */
static bool set_crtc( uint32_t crtc, uint32_t connector, uint32_t fb, const struct drm_mode_modeinfo *mode )
{
    struct drm_mode_crtc set = { .crtc_id = crtc };

    if (connector)
    {
        set.set_connectors_ptr = (uintptr_t)&connector;
        set.count_connectors = 1;
        set.fb_id = fb;
        set.mode_valid = 1;
        set.mode = *mode;
    }
    return !ioctl( kms.fd, DRM_IOCTL_MODE_SETCRTC, &set );
}

static int crtc_index( uint32_t crtc )
{
    for (uint32_t i = 0; i < kms.crtc_count; i++) if (kms.crtcs[i] == crtc) return i;
    return -1;
}

/* the client buffers the monitor shows or is about to show go back */
static void drop_scanout( struct kms_output *out )
{
    if (out->queued_buffer) compositor_buffer_unused( out->queued_buffer );
    if (out->front_buffer) compositor_buffer_unused( out->front_buffer );
    out->queued_buffer = out->front_buffer = NULL;
    out->queued_fb = out->front_fb = 0;
    if (out->cursor_on) kms_show_cursor( out, false, 0, 0 );
}

static void free_output( struct kms_output *out )
{
    compositor_output_removed( out );
    if (out->enabled) set_crtc( out->crtc_id, 0, 0, NULL );
    drop_scanout( out );
    destroy_fb( &out->fbs[0] );
    destroy_fb( &out->fbs[1] );
    free( out->modes );
    free( out->edid );
    memset( out, 0, sizeof(*out) );
}

static bool same_monitor( const struct kms_output *a, const struct kms_output *b )
{
    return a->edid_len == b->edid_len && (!a->edid_len || !memcmp( a->edid, b->edid, a->edid_len )) &&
           a->mode_count == b->mode_count && !memcmp( a->modes, b->modes, a->mode_count * sizeof(*a->modes) );
}

bool kms_probe(void)
{
    struct drm_mode_card_res res = {0};
    uint32_t *conns = NULL, *crtcs = NULL;
    bool seen[KMS_MAX_OUTPUTS] = {0}, changed = false;

    if (ioctl( kms.fd, DRM_IOCTL_MODE_GETRESOURCES, &res )) return false;
    conns = zalloc( res.count_connectors, sizeof(*conns) );
    crtcs = zalloc( res.count_crtcs, sizeof(*crtcs) );
    res.connector_id_ptr = (uintptr_t)conns;
    res.crtc_id_ptr = (uintptr_t)crtcs;
    res.count_fbs = res.count_encoders = 0;
    if (ioctl( kms.fd, DRM_IOCTL_MODE_GETRESOURCES, &res )) goto done;

    kms.crtc_count = min( res.count_crtcs, ARRAY_SIZE(kms.crtcs) );
    memcpy( kms.crtcs, crtcs, kms.crtc_count * sizeof(*crtcs) );

    for (uint32_t i = 0; i < res.count_connectors; i++)
    {
        struct kms_output found;
        int slot, free_slot = -1;

        if (!read_connector( conns[i], &found )) continue;
        for (slot = 0; slot < KMS_MAX_OUTPUTS; slot++)
            if (kms.outputs[slot].connector_id == found.connector_id) break;
        if (slot < KMS_MAX_OUTPUTS && same_monitor( &kms.outputs[slot], &found ))
        {
            if (kms.outputs[slot].vrr_capable != found.vrr_capable)
                MESSAGE( "dwm: %s variable refresh %s\n", found.name, found.vrr_capable ? "supported" : "not supported" );
            kms.outputs[slot].vrr_capable = found.vrr_capable;
            seen[slot] = true;
            free( found.modes );
            free( found.edid );
            continue;
        }
        /* another monitor on the same connector, its unplug missed */
        if (slot < KMS_MAX_OUTPUTS) free_output( &kms.outputs[slot] );
        for (slot = 0; slot < KMS_MAX_OUTPUTS && free_slot < 0; slot++)
            if (!kms.outputs[slot].connector_id) free_slot = slot;
        if (free_slot < 0)
        {
            WARN( "%s: more than %u monitors\n", found.name, KMS_MAX_OUTPUTS );
            free( found.modes );
            free( found.edid );
            continue;
        }
        if (found.crtc_id && crtc_index( found.crtc_id ) < 0) found.crtc_id = 0;
        kms.outputs[free_slot] = found;
        seen[free_slot] = true;
        changed = true;
        MESSAGE( "dwm: monitor on %s, %u modes, native %ux%u@%u, EDID %u bytes%s\n", found.name, found.mode_count,
                 found.modes[found.preferred].info.hdisplay, found.modes[found.preferred].info.vdisplay,
                 whole_hz( found.modes[found.preferred].refresh ), found.edid_len,
                 found.vrr_capable ? ", variable refresh" : "" );
    }

    for (int slot = 0; slot < KMS_MAX_OUTPUTS; slot++)
    {
        if (!kms.outputs[slot].connector_id || seen[slot]) continue;
        MESSAGE( "dwm: monitor on %s unplugged\n", kms.outputs[slot].name );
        free_output( &kms.outputs[slot] );
        changed = true;
    }

done:
    free( conns );
    free( crtcs );
    return changed;
}

struct kms_mode *kms_find_mode( struct kms_output *out, uint32_t width, uint32_t height, uint32_t refresh )
{
    struct kms_mode *best = NULL;

    for (uint32_t i = 0; i < out->mode_count; i++)
    {
        struct kms_mode *m = &out->modes[i];

        if (m->info.hdisplay != width || m->info.vdisplay != height) continue;
        if (m->refresh == refresh) return m;
        /* a whole Hz is all a DEVMODE carries */
        if (whole_hz( m->refresh ) == whole_hz( refresh )) best = m;
        else if (!refresh && (!best || m->refresh > best->refresh)) best = m;
    }
    return best;
}

/**********************************************************************
 *          Frames
 */

static void flip_finished( struct kms_output *out )
{
    void *off_screen = out->front_buffer;
    uint64_t now = kms_now();

    /* a real vertical blank never comes twice within one refresh */
    if (!out->fake_vblank && out->done_time && now - out->done_time < kms_frame_time( out ) * 3 / 4)
    {
        MESSAGE( "dwm: %s flips without a vertical blank: frames are paced by a timer\n", out->name );
        out->fake_vblank = true;
    }
    out->done_time = now;
    out->front_fb = out->queued_fb;
    out->front_buffer = out->queued_buffer;
    out->queued_fb = 0;
    out->queued_buffer = NULL;
    compositor_flip_done( out, off_screen );
}

void kms_dispatch(void)
{
    char buffer[1024];
    ssize_t len;

    while ((len = read( kms.fd, buffer, sizeof(buffer) )) > 0)
    {
        for (char *p = buffer; p + sizeof(struct drm_event) <= buffer + len;)
        {
            const struct drm_event *event = (const struct drm_event *)p;
            const struct drm_event_vblank *vblank = (const struct drm_event_vblank *)p;
            uint32_t crtc;

            if (event->length < sizeof(*event) || p + event->length > buffer + len) break;
            p += event->length;
            if (event->type != DRM_EVENT_FLIP_COMPLETE || event->length < sizeof(*vblank)) continue;
            crtc = vblank->crtc_id ? vblank->crtc_id : (uint32_t)vblank->user_data;
            for (int i = 0; i < KMS_MAX_OUTPUTS; i++)
            {
                struct kms_output *out = &kms.outputs[i];

                if (!out->connector_id || !out->queued_fb || out->crtc_id != crtc) continue;
                flip_finished( out );
                break;
            }
        }
    }
}

/* before a modeset: the flips on their way land first */
static void wait_flips(void)
{
    uint64_t start = kms_now();

    for (;;)
    {
        struct pollfd pfd = { .fd = kms.fd, .events = POLLIN };
        bool pending = false;

        for (int i = 0; i < KMS_MAX_OUTPUTS; i++) pending |= kms.outputs[i].queued_fb != 0;
        if (!pending) return;
        if (kms_now() - start > 500000) break;
        if (poll( &pfd, 1, 50 ) > 0) kms_dispatch();
    }
    for (int i = 0; i < KMS_MAX_OUTPUTS; i++)
    {
        struct kms_output *out = &kms.outputs[i];

        if (!out->queued_fb) continue;
        WARN( "%s: a flip never finished\n", out->name );
        if (out->queued_buffer) compositor_buffer_unused( out->queued_buffer );
        out->queued_fb = 0;
        out->queued_buffer = NULL;
    }
}

/* after a modeset: the plane flips go to, and the CRTC's variable refresh */
static void read_crtc_state( struct kms_output *out )
{
    static const char *const vrr_name[] = { "VRR_ENABLED" };
    const uint32_t *prop;
    uint64_t value = 0;

    out->plane = kms.atomic ? primary_plane( out->crtc_id ) : 0;
    if (out->plane)
    {
        prop = plane_properties( out->plane );
        for (int i = 0; i < PLANE_TYPE; i++) if (!prop[i]) out->plane = 0;
    }
    object_properties( out->crtc_id, DRM_MODE_OBJECT_CRTC, vrr_name, 1, &out->vrr_prop, &value );
    out->vrr_on = out->vrr_prop && value;
    out->vrr_refused = false;
}

/* the whole of src_width x src_height of fb over the whole mode */
static bool flip_atomic( struct kms_output *out, uint32_t fb, uint32_t src_width, uint32_t src_height, bool vrr,
                         uint32_t flags )
{
    const struct drm_mode_modeinfo *mode = &out->modes[out->mode].info;
    const uint32_t *prop = plane_properties( out->plane );
    const uint64_t plane_values[PLANE_TYPE] =
    {
        fb, out->crtc_id, 0, 0, (uint64_t)src_width << 16, (uint64_t)src_height << 16, 0, 0, mode->hdisplay, mode->vdisplay
    };
    uint32_t objs[2] = { out->plane, out->crtc_id }, counts[2] = { PLANE_TYPE, 1 }, props[PLANE_TYPE + 1];
    uint64_t values[PLANE_TYPE + 1];
    struct drm_mode_atomic atomic =
    {
        .flags = flags,
        .count_objs = vrr != out->vrr_on ? 2 : 1,
        .objs_ptr = (uintptr_t)objs,
        .count_props_ptr = (uintptr_t)counts,
        .props_ptr = (uintptr_t)props,
        .prop_values_ptr = (uintptr_t)values,
        .user_data = out->crtc_id,
    };

    for (int i = 0; i < PLANE_TYPE; i++)
    {
        props[i] = prop[i];
        values[i] = plane_values[i];
    }
    props[PLANE_TYPE] = out->vrr_prop;
    values[PLANE_TYPE] = vrr;
    return !ioctl( kms.fd, DRM_IOCTL_MODE_ATOMIC, &atomic );
}

static bool flip_legacy( struct kms_output *out, uint32_t fb )
{
    struct drm_mode_crtc_page_flip flip =
    {
        .crtc_id = out->crtc_id, .fb_id = fb, .flags = DRM_MODE_PAGE_FLIP_EVENT, .user_data = out->crtc_id
    };

    if (!ioctl( kms.fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip )) return true;
    if (errno != EBUSY)
    {
        MESSAGE( "dwm: %s cannot flip (%s): frames are drawn where they show\n", out->name, strerror( errno ) );
        out->no_flip = true;
    }
    return false;
}

bool kms_present( struct kms_output *out, uint32_t fb, uint32_t src_width, uint32_t src_height,
                  bool vrr, void *buffer )
{
    uint32_t flags = DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK;
    bool vrr_before = out->vrr_on;

    if (!out->enabled || out->queued_fb || out->no_flip) return false;
    vrr = vrr && out->vrr_capable && out->vrr_prop && !out->vrr_refused;

    if (out->plane && flip_atomic( out, fb, src_width, src_height, vrr, flags ))
        out->vrr_on = vrr;
    else if (out->plane && errno == EBUSY)
        return false;
    else if (out->plane && vrr != out->vrr_on && flip_atomic( out, fb, src_width, src_height, out->vrr_on, flags ))
    {
        /* switching it would take a modeset, which blanks the screen: not in the middle of a game */
        MESSAGE( "dwm: %s cannot switch variable refresh without a modeset\n", out->name );
        out->vrr_refused = true;
    }
    else if (buffer)
        return false;  /* composed instead */
    else
    {
        if (out->plane)
        {
            WARN( "%s: atomic flip failed: %s\n", out->name, strerror( errno ) );
            out->plane = 0;
        }
        if (!flip_legacy( out, fb )) return false;
    }

    if (out->vrr_on != vrr_before)
        MESSAGE( "dwm: %s variable refresh %s\n", out->name, out->vrr_on ? "on" : "off" );
    out->queued_fb = fb;
    out->queued_buffer = buffer;
    out->submit_time = kms_now();
    return true;
}

/* the frame buffer to compose into: neither on screen nor about to be */
struct kms_fb *kms_back_buffer( struct kms_output *out )
{
    if (!out->fbs[1].fb_id && out->fbs[0].fb_id)
        create_fb( out->fbs[0].width, out->fbs[0].height, &out->fbs[1] );
    for (int i = 0; i < 2; i++)
        if (out->fbs[i].fb_id && out->fbs[i].fb_id != out->front_fb && out->fbs[i].fb_id != out->queued_fb)
            return &out->fbs[i];
    return NULL;
}

/* the frame buffer on screen, for a driver that cannot flip */
struct kms_fb *kms_front_buffer( struct kms_output *out )
{
    return out->fbs[1].fb_id && out->fbs[1].fb_id == out->front_fb ? &out->fbs[1] : &out->fbs[0];
}

bool kms_can_scanout( struct kms_output *out, uint32_t fb, uint32_t src_width, uint32_t src_height )
{
    return out->enabled && out->plane &&
           flip_atomic( out, fb, src_width, src_height, out->vrr_on, DRM_MODE_ATOMIC_TEST_ONLY );
}

/* client buffers imported as framebuffers, with the GEM handles they hold */
static struct { uint32_t fb, handle; } imported[64];

/* a GEM handle belongs to the buffer object, shared by every import of it */
static void close_handle( uint32_t handle )
{
    struct drm_gem_close gem = { .handle = handle };

    for (int i = 0; i < ARRAY_SIZE(imported); i++)
        if (imported[i].fb && imported[i].handle == handle) return;
    ioctl( kms.fd, DRM_IOCTL_GEM_CLOSE, &gem );
}

uint32_t kms_add_dmabuf( int fd, uint32_t width, uint32_t height, uint32_t format, uint32_t offset,
                         uint32_t stride, uint64_t modifier )
{
    struct drm_prime_handle prime = { .fd = fd };
    struct drm_mode_fb_cmd2 cmd = { .width = width, .height = height, .pixel_format = format };
    int slot;

    for (slot = 0; slot < ARRAY_SIZE(imported) && imported[slot].fb; slot++);
    if (slot == ARRAY_SIZE(imported)) return 0;
    if (ioctl( kms.fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime )) return 0;

    cmd.handles[0] = prime.handle;
    cmd.pitches[0] = stride;
    cmd.offsets[0] = offset;
    if (modifier != DRM_FORMAT_MOD_INVALID)
    {
        cmd.flags = DRM_MODE_FB_MODIFIERS;
        cmd.modifier[0] = modifier;
    }
    if (ioctl( kms.fd, DRM_IOCTL_MODE_ADDFB2, &cmd ))
    {
        cmd.fb_id = 0;
        /* a driver that knows no modifiers takes a linear buffer without one */
        if (modifier == DRM_FORMAT_MOD_LINEAR)
        {
            cmd.flags = 0;
            cmd.modifier[0] = 0;
            if (ioctl( kms.fd, DRM_IOCTL_MODE_ADDFB2, &cmd )) cmd.fb_id = 0;
        }
    }
    if (!cmd.fb_id)
    {
        close_handle( prime.handle );
        return 0;
    }
    imported[slot].fb = cmd.fb_id;
    imported[slot].handle = prime.handle;
    return cmd.fb_id;
}

void kms_remove_dmabuf( uint32_t fb )
{
    for (int i = 0; i < ARRAY_SIZE(imported); i++)
    {
        uint32_t handle = imported[i].handle;

        if (imported[i].fb != fb) continue;
        ioctl( kms.fd, DRM_IOCTL_MODE_RMFB, &fb );
        imported[i].fb = 0;
        close_handle( handle );
        return;
    }
}

/* the hardware cursor: one image for every CRTC */
static struct { uint32_t handle, width, height; } cursor;

bool kms_set_cursor_image( const uint32_t *argb, uint32_t width, uint32_t height )
{
    struct drm_get_cap cap_width = { .capability = DRM_CAP_CURSOR_WIDTH };
    struct drm_get_cap cap_height = { .capability = DRM_CAP_CURSOR_HEIGHT };
    struct drm_mode_create_dumb create = { .bpp = 32 };
    struct drm_mode_destroy_dumb destroy = {0};
    struct drm_mode_map_dumb map = {0};
    uint8_t *pixels;

    create.width = !ioctl( kms.fd, DRM_IOCTL_GET_CAP, &cap_width ) && cap_width.value ? cap_width.value : 64;
    create.height = !ioctl( kms.fd, DRM_IOCTL_GET_CAP, &cap_height ) && cap_height.value ? cap_height.value : 64;
    if (width > create.width || height > create.height) return false;
    if (ioctl( kms.fd, DRM_IOCTL_MODE_CREATE_DUMB, &create )) return false;
    map.handle = destroy.handle = create.handle;
    if (ioctl( kms.fd, DRM_IOCTL_MODE_MAP_DUMB, &map ) ||
        (pixels = mmap( NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, kms.fd, (off_t)map.offset )) == MAP_FAILED)
    {
        ioctl( kms.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy );
        return false;
    }
    memset( pixels, 0, create.size );
    for (uint32_t y = 0; y < height; y++)
        memcpy( pixels + (size_t)y * create.pitch, argb + (size_t)y * width, width * 4 );
    munmap( pixels, create.size );
    cursor.handle = create.handle;
    cursor.width = create.width;
    cursor.height = create.height;
    return true;
}

bool kms_show_cursor( struct kms_output *out, bool show, int32_t x, int32_t y )
{
    struct drm_mode_cursor set = { .crtc_id = out->crtc_id };

    if (!show)
    {
        if (!out->cursor_on) return true;
        set.flags = DRM_MODE_CURSOR_BO;  /* no buffer: hidden */
        ioctl( kms.fd, DRM_IOCTL_MODE_CURSOR, &set );
        out->cursor_on = false;
        return true;
    }
    if (!cursor.handle) return false;
    set.flags = DRM_MODE_CURSOR_MOVE;
    set.x = x;
    set.y = y;
    if (!out->cursor_on)
    {
        set.flags |= DRM_MODE_CURSOR_BO;
        set.handle = cursor.handle;
        set.width = cursor.width;
        set.height = cursor.height;
    }
    if (ioctl( kms.fd, DRM_IOCTL_MODE_CURSOR, &set )) return false;
    out->cursor_on = true;
    return true;
}

/**********************************************************************
 *          Modes
 */

/* Modes, positions and CRTCs for every monitor at once. If one modeset fails,
 * the ones already made are undone: the screen stays as it was. */
bool kms_apply( const struct dwm_output_config *configs, uint32_t count, bool test )
{
    struct
    {
        bool          enabled, modeset;
        uint32_t      mode, crtc;
        int32_t       x, y;
        struct kms_fb fb;
        bool          new_fb;
    } next[KMS_MAX_OUTPUTS];
    uint32_t used = 0;
    bool any = false;
    int i;

    for (i = 0; i < KMS_MAX_OUTPUTS; i++)
    {
        struct kms_output *out = &kms.outputs[i];

        memset( &next[i], 0, sizeof(next[i]) );
        next[i].enabled = out->enabled;
        next[i].mode = out->mode;
        next[i].crtc = out->crtc_id;
        next[i].x = out->x;
        next[i].y = out->y;
    }

    for (uint32_t c = 0; c < count; c++)
    {
        struct kms_mode *mode;

        for (i = 0; i < KMS_MAX_OUTPUTS; i++) if (kms.outputs[i].connector_id == configs[c].id) break;
        if (i == KMS_MAX_OUTPUTS)
        {
            WARN( "no monitor on connector %u\n", configs[c].id );
            return false;
        }
        if (!(next[i].enabled = configs[c].enabled)) continue;
        if (!(mode = kms_find_mode( &kms.outputs[i], configs[c].width, configs[c].height, configs[c].refresh )))
        {
            WARN( "%s has no mode %ux%u@%u\n", kms.outputs[i].name, configs[c].width, configs[c].height,
                  configs[c].refresh );
            return false;
        }
        next[i].mode = mode - kms.outputs[i].modes;
        next[i].x = configs[c].x;
        next[i].y = configs[c].y;
    }

    /* CRTCs: each monitor keeps its own if it can, then the rest get free ones */
    for (i = 0; i < KMS_MAX_OUTPUTS; i++)
    {
        int index;

        if (!kms.outputs[i].connector_id || !next[i].enabled) continue;
        any = true;
        index = crtc_index( next[i].crtc );
        if (index >= 0 && (kms.outputs[i].possible_crtcs & (1u << index)) && !(used & (1u << index)))
            used |= 1u << index;
        else
            next[i].crtc = 0;
    }
    for (i = 0; i < KMS_MAX_OUTPUTS; i++)
    {
        if (!kms.outputs[i].connector_id) continue;
        if (!next[i].enabled)
        {
            next[i].crtc = 0;
            continue;
        }
        for (uint32_t k = 0; !next[i].crtc && k < kms.crtc_count; k++)
        {
            if (!(kms.outputs[i].possible_crtcs & (1u << k)) || (used & (1u << k))) continue;
            used |= 1u << k;
            next[i].crtc = kms.crtcs[k];
        }
        if (!next[i].crtc)
        {
            WARN( "no CRTC left for %s\n", kms.outputs[i].name );
            return false;
        }
    }
    if (!any)
    {
        WARN( "every monitor would be off\n" );
        return false;
    }
    if (test) return true;
    wait_flips();

    /* a framebuffer of the new size where the mode changes; same size keeps its own */
    for (i = 0; i < KMS_MAX_OUTPUTS; i++)
    {
        struct kms_output *out = &kms.outputs[i];
        const struct drm_mode_modeinfo *m;

        if (!out->connector_id || !next[i].enabled) continue;
        next[i].modeset = !out->enabled || next[i].mode != out->mode || next[i].crtc != out->crtc_id;
        if (!next[i].modeset) continue;
        m = &out->modes[next[i].mode].info;
        if (out->enabled && out->fbs[0].width == m->hdisplay && out->fbs[0].height == m->vdisplay)
            next[i].fb = out->fbs[0];
        else if (create_fb( m->hdisplay, m->vdisplay, &next[i].fb ))
            next[i].new_fb = true;
        else
        {
            ERR( "%s: no framebuffer for %ux%u: %s\n", out->name, m->hdisplay, m->vdisplay, strerror( errno ) );
            goto undo;
        }
    }

    /* off first, so that their CRTCs are free for the others */
    for (i = 0; i < KMS_MAX_OUTPUTS; i++)
    {
        struct kms_output *out = &kms.outputs[i];
        if (out->connector_id && out->enabled && (!next[i].enabled || next[i].crtc != out->crtc_id))
            set_crtc( out->crtc_id, 0, 0, NULL );
    }
    for (i = 0; i < KMS_MAX_OUTPUTS; i++)
    {
        struct kms_output *out = &kms.outputs[i];

        if (!next[i].modeset) continue;
        if (!set_crtc( next[i].crtc, out->connector_id, next[i].fb.fb_id, &out->modes[next[i].mode].info ))
        {
            ERR( "%s: the monitor does not take %ux%u@%u: %s\n", out->name, out->modes[next[i].mode].info.hdisplay,
                 out->modes[next[i].mode].info.vdisplay, whole_hz( out->modes[next[i].mode].refresh ), strerror( errno ) );
            goto restore;
        }
    }

    for (i = 0; i < KMS_MAX_OUTPUTS; i++)
    {
        struct kms_output *out = &kms.outputs[i];

        if (!out->connector_id) continue;
        /* a modeset shows our first frame buffer, whatever was on screen */
        if (next[i].modeset || !next[i].enabled) drop_scanout( out );
        if ((next[i].modeset && next[i].new_fb) || !next[i].enabled)
        {
            destroy_fb( &out->fbs[0] );
            destroy_fb( &out->fbs[1] );
        }
        if (next[i].modeset)
        {
            out->fbs[0] = next[i].fb;
            out->front_fb = next[i].fb.fb_id;
        }
        if (next[i].enabled && (next[i].modeset || !out->enabled || next[i].x != out->x || next[i].y != out->y))
            MESSAGE( "dwm: %s %ux%u@%u at %d,%d\n", out->name, out->modes[next[i].mode].info.hdisplay,
                     out->modes[next[i].mode].info.vdisplay, whole_hz( out->modes[next[i].mode].refresh ),
                     next[i].x, next[i].y );
        else if (out->enabled && !next[i].enabled)
            MESSAGE( "dwm: %s off\n", out->name );
        out->enabled = next[i].enabled;
        out->mode = next[i].mode;
        out->crtc_id = next[i].crtc;
        out->x = next[i].x;
        out->y = next[i].y;
        if (next[i].modeset) read_crtc_state( out );
    }
    return true;

restore:
    for (i = 0; i < KMS_MAX_OUTPUTS; i++)
        if (next[i].crtc && !(kms.outputs[i].enabled && kms.outputs[i].crtc_id == next[i].crtc))
            set_crtc( next[i].crtc, 0, 0, NULL );
    for (i = 0; i < KMS_MAX_OUTPUTS; i++)
    {
        struct kms_output *out = &kms.outputs[i];
        if (out->connector_id && out->enabled)
            set_crtc( out->crtc_id, out->connector_id, out->front_fb, &out->modes[out->mode].info );
    }
undo:
    for (i = 0; i < KMS_MAX_OUTPUTS; i++) if (next[i].new_fb) destroy_fb( &next[i].fb );
    return false;
}

void kms_flush( struct kms_output *out )
{
    struct drm_mode_fb_dirty_cmd dirty = { .fb_id = out->front_fb };

    ioctl( kms.fd, DRM_IOCTL_MODE_DIRTYFB, &dirty );
}

static unsigned int read_sysfs_hex( const char *card, const char *file )
{
    char path[128], text[32] = "";
    FILE *f;

    snprintf( path, sizeof(path), "/sys/class/drm/%s/device/%s", card, file );
    if (!(f = fopen( path, "r" ))) return 0;
    fgets( text, sizeof(text), f );
    fclose( f );
    return strtoul( text, NULL, 16 );
}

/* The model of a PCI adapter from the PCI id database, the way Linux
 * distributions ship it: "Ellesmere [Radeon RX 580]" is a Radeon RX 580. */
static bool pci_ids_name( uint16_t vendor, uint16_t device, char *name, size_t size )
{
    char line[256], *open, *close;
    bool in_vendor = false;
    FILE *f;

    if (!(f = fopen( "/usr/share/hwdata/pci.ids", "r" ))) return false;
    while (fgets( line, sizeof(line), f ))
    {
        unsigned int id;

        if (line[0] == '#' || line[0] == '\n') continue;
        if (line[0] != '\t')
        {
            in_vendor = strtoul( line, NULL, 16 ) == vendor;
            continue;
        }
        if (!in_vendor || line[1] == '\t') continue;
        id = strtoul( line + 1, NULL, 16 );
        if (id != device) continue;
        if (!(open = strchr( line, ' ' ))) break;
        while (*open == ' ') open++;
        if ((close = strrchr( open, '\n' ))) *close = 0;
        /* the marketing name is the one in brackets, when there is one */
        if ((open = strchr( open, '[' )) && (close = strrchr( open, ']' )))
        {
            *close = 0;
            open++;
        }
        else open = strchr( line, ' ' ) + 1;
        snprintf( name, size, "%s", open );
        fclose( f );
        return true;
    }
    fclose( f );
    return false;
}

static void read_adapter(void)
{
    struct drm_version version = { .name_len = sizeof(kms.driver) - 1, .name = kms.driver };
    const char *card = strrchr( kms.name, '/' ) + 1;
    char model[96];

    memset( kms.driver, 0, sizeof(kms.driver) );
    ioctl( kms.fd, DRM_IOCTL_VERSION, &version );
    kms.vendor = read_sysfs_hex( card, "vendor" );
    kms.device = read_sysfs_hex( card, "device" );
    kms.subsystem = read_sysfs_hex( card, "subsystem_device" ) << 16 | read_sysfs_hex( card, "subsystem_vendor" );
    kms.revision = read_sysfs_hex( card, "revision" );

    if (kms.vendor && pci_ids_name( kms.vendor, kms.device, model, sizeof(model) ))
    {
        const char *vendor = kms.vendor == 0x1002 ? "AMD " : kms.vendor == 0x10de ? "NVIDIA " :
                             kms.vendor == 0x8086 ? "Intel " : "";
        snprintf( kms.description, sizeof(kms.description), "%s%s", vendor, model );
    }
    /* a card nothing knows a name for is the basic adapter, as in Windows;
     * the name of the Linux driver is not for the user to see */
    else strcpy( kms.description, "Microsoft Basic Display Adapter" );
}

/* A card with a monitor, the one firmware showed its picture on first: on a
 * PC with two, that is the one Windows would start on too. With report, says
 * why each one did not do. */
static bool try_cards( bool report )
{
    for (int pass = 0; pass < 2; pass++)
    {
        for (int i = 0; i < 8; i++)
        {
            char card[16];
            bool connected = false;

            snprintf( card, sizeof(card), "card%d", i );
            if ((read_sysfs_hex( card, "boot_vga" ) == 1) != (pass == 0)) continue;
            snprintf( kms.name, sizeof(kms.name), "/dev/dri/%s", card );
            if ((kms.fd = open( kms.name, O_RDWR | O_CLOEXEC | O_NONBLOCK )) < 0)
            {
                if (report && errno != ENOENT) ERR( "%s: %s\n", kms.name, strerror( errno ) );
                continue;
            }
            if (ioctl( kms.fd, DRM_IOCTL_SET_MASTER, 0 ))
            {
                if (report) ERR( "%s: DRM master is held elsewhere\n", kms.name );
            }
            else
            {
                kms_probe();
                for (int k = 0; k < KMS_MAX_OUTPUTS; k++) connected |= !!kms.outputs[k].connector_id;
                if (connected)
                {
                    struct drm_set_client_cap atomic = { .capability = DRM_CLIENT_CAP_ATOMIC, .value = 1 };
                    struct stat st;

                    kms.atomic = !ioctl( kms.fd, DRM_IOCTL_SET_CLIENT_CAP, &atomic );
                    kms.devnum = fstat( kms.fd, &st ) ? 0 : st.st_rdev;
                    read_adapter();
                    MESSAGE( "dwm: display %s (%s, %04x:%04x%s)\n", kms.name, kms.driver, kms.vendor, kms.device,
                             kms.atomic ? ", atomic" : "" );
                    return true;
                }
                if (report) ERR( "%s: no connected monitor\n", kms.name );
            }
            close( kms.fd );
            kms.fd = -1;
        }
    }
    return false;
}

/* a GPU driver may still be probing its outputs: up to 10 s of retries */
int kms_open(void)
{
    for (int attempt = 0; attempt < 40; attempt++)
    {
        if (try_cards( attempt == 39 )) return 0;
        usleep( 250000 );
    }
    return -1;
}

int kms_hotplug_socket(void)
{
    struct sockaddr_nl addr = { .nl_family = AF_NETLINK, .nl_groups = 1 /* the kernel's own events */ };
    int fd = socket( AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT );

    if (fd < 0) return -1;
    if (bind( fd, (struct sockaddr *)&addr, sizeof(addr) ))
    {
        ERR( "no display hotplug: %s\n", strerror( errno ) );
        close( fd );
        return -1;
    }
    return fd;
}

/* "change@/devices/.../drm/card1" SUBSYSTEM=drm ACTION=change DEVNAME=dri/card1.
 * A monitor plugged in or out comes with HOTPLUG=1, but not every driver
 * says so, and rereading the connectors of our own card costs little. A card
 * that comes or goes is a GPU driver taking the screen over from simpledrm. */
enum kms_event kms_hotplug_event( int fd )
{
    const char *card = kms.fd >= 0 ? kms.name + strlen( "/dev/" ) : "";
    bool drm = false, ours = false, is_card = false;
    char buffer[4096], action[16] = "";
    ssize_t len;

    if ((len = recv( fd, buffer, sizeof(buffer) - 1, 0 )) <= 0) return KMS_EVENT_NONE;
    buffer[len] = 0;
    for (char *p = buffer; p < buffer + len; p += strlen( p ) + 1)
    {
        if (!strcmp( p, "SUBSYSTEM=drm" )) drm = true;
        else if (!strncmp( p, "ACTION=", 7 )) snprintf( action, sizeof(action), "%s", p + 7 );
        else if (!strncmp( p, "DEVNAME=dri/card", 16 ))
        {
            is_card = true;
            ours = !strcmp( p + 8, card );
        }
    }
    if (!drm || !is_card) return KMS_EVENT_NONE;
    if (!strcmp( action, "change" )) return ours ? KMS_EVENT_MONITORS : KMS_EVENT_NONE;
    if (!strcmp( action, "remove" )) return ours ? KMS_EVENT_CARD_GONE : KMS_EVENT_NONE;
    if (!strcmp( action, "add" )) return KMS_EVENT_CARD_ADDED;
    return KMS_EVENT_NONE;
}

/* Our card is gone: its monitors are let go and nothing more is sent to it */
void kms_close(void)
{
    MESSAGE( "dwm: %s went away\n", kms.name );
    for (int i = 0; i < KMS_MAX_OUTPUTS; i++)
        if (kms.outputs[i].connector_id) free_output( &kms.outputs[i] );
    memset( imported, 0, sizeof(imported) );
    cursor.handle = 0;
    close( kms.fd );
    kms.fd = -1;
    kms.generation++;
}

bool kms_reopen(void)
{
    return try_cards( false );
}
