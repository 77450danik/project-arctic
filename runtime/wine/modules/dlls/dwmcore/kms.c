/*
 * Arctic desktop composition engine: display ownership (DRM/KMS)
 *
 * Takes DRM master of the first card with a connected monitor and drives
 * every connected monitor of it, each from a dumb framebuffer of its own.
 * Works on any KMS driver, simpledrm included, so it needs no GPU driver:
 * this is the CPU ("basic") composition path.
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
#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
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

static void read_edid( const uint32_t *props, const uint64_t *values, uint32_t count, struct kms_output *out )
{
    for (uint32_t i = 0; i < count; i++)
    {
        struct drm_mode_get_property prop = { .prop_id = props[i] };
        struct drm_mode_get_blob blob = { .blob_id = values[i] };
        void *data;

        if (ioctl( kms.fd, DRM_IOCTL_MODE_GETPROPERTY, &prop ) || strcmp( prop.name, "EDID" )) continue;
        if (!values[i] || ioctl( kms.fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob ) || !blob.length) return;
        if (!(data = malloc( blob.length ))) return;
        blob.data = (uintptr_t)data;
        if (ioctl( kms.fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob ) || blob.length > DWM_MAX_EDID)
        {
            free( data );
            return;
        }
        out->edid = data;
        out->edid_len = blob.length;
        return;
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
        read_edid( props, values, conn.count_props, out );

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

static void free_output( struct kms_output *out )
{
    compositor_output_removed( out );
    if (out->enabled) set_crtc( out->crtc_id, 0, 0, NULL );
    destroy_fb( &out->fb );
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
        MESSAGE( "dwm: monitor on %s, %u modes, native %ux%u@%u, EDID %u bytes\n", found.name, found.mode_count,
                 found.modes[found.preferred].info.hdisplay, found.modes[found.preferred].info.vdisplay,
                 whole_hz( found.modes[found.preferred].refresh ), found.edid_len );
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

    /* a framebuffer of the new size where the mode changes; same size keeps its own */
    for (i = 0; i < KMS_MAX_OUTPUTS; i++)
    {
        struct kms_output *out = &kms.outputs[i];
        const struct drm_mode_modeinfo *m;

        if (!out->connector_id || !next[i].enabled) continue;
        next[i].modeset = !out->enabled || next[i].mode != out->mode || next[i].crtc != out->crtc_id;
        if (!next[i].modeset) continue;
        m = &out->modes[next[i].mode].info;
        if (out->enabled && out->fb.width == m->hdisplay && out->fb.height == m->vdisplay)
            next[i].fb = out->fb;
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
        if ((next[i].modeset && next[i].new_fb) || !next[i].enabled) destroy_fb( &out->fb );
        if (next[i].modeset) out->fb = next[i].fb;
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
            set_crtc( out->crtc_id, out->connector_id, out->fb.fb_id, &out->modes[out->mode].info );
    }
undo:
    for (i = 0; i < KMS_MAX_OUTPUTS; i++) if (next[i].new_fb) destroy_fb( &next[i].fb );
    return false;
}

void kms_flush( struct kms_output *out )
{
    struct drm_mode_fb_dirty_cmd dirty = { .fb_id = out->fb.fb_id };

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

    if (!kms.vendor)
        strcpy( kms.description, "Microsoft Basic Display Adapter" );
    else if (pci_ids_name( kms.vendor, kms.device, model, sizeof(model) ))
    {
        const char *vendor = kms.vendor == 0x1002 ? "AMD " : kms.vendor == 0x10de ? "NVIDIA " :
                             kms.vendor == 0x8086 ? "Intel " : "";
        snprintf( kms.description, sizeof(kms.description), "%s%s", vendor, model );
    }
    else snprintf( kms.description, sizeof(kms.description), "%s display adapter", kms.driver );
}

/* one pass over the cards; with report, says why each one did not do */
static bool try_cards( bool report )
{
    for (int i = 0; i < 8; i++)
    {
        bool connected = false;

        snprintf( kms.name, sizeof(kms.name), "/dev/dri/card%d", i );
        if ((kms.fd = open( kms.name, O_RDWR | O_CLOEXEC )) < 0)
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
                read_adapter();
                MESSAGE( "dwm: display %s (%s, %04x:%04x)\n", kms.name, kms.driver, kms.vendor, kms.device );
                return true;
            }
            if (report) ERR( "%s: no connected monitor\n", kms.name );
        }
        close( kms.fd );
        kms.fd = -1;
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
        WARN( "no display hotplug: %s\n", strerror( errno ) );
        close( fd );
        return -1;
    }
    return fd;
}

/* "change@/devices/.../drm/card1" SUBSYSTEM=drm HOTPLUG=1 DEVNAME=dri/card1 */
bool kms_hotplug_event( int fd )
{
    const char *card = kms.name + strlen( "/dev/" );
    bool drm = false, hotplug = false, ours = false;
    char buffer[4096];
    ssize_t len;

    if ((len = recv( fd, buffer, sizeof(buffer) - 1, 0 )) <= 0) return false;
    buffer[len] = 0;
    for (char *p = buffer; p < buffer + len; p += strlen( p ) + 1)
    {
        if (!strcmp( p, "SUBSYSTEM=drm" )) drm = true;
        else if (!strcmp( p, "HOTPLUG=1" )) hotplug = true;
        else if (!strncmp( p, "DEVNAME=", 8 ) && !strcmp( p + 8, card )) ours = true;
    }
    return drm && hotplug && ours;
}
