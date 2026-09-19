#include "screen.h"

#include <drm/drm.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static void *zalloc(uint32_t count, size_t size)
{
    return calloc(count ? count : 1, size);
}

/* Finds a connected connector, its preferred mode and a CRTC that can drive it. */
static int pick_output(struct screen *s)
{
    struct drm_mode_card_res res = {0};
    uint32_t *conns = NULL, *crtcs = NULL, *encs = NULL, *fbs = NULL;
    int found = 0;

    if (ioctl(s->fd, DRM_IOCTL_MODE_GETRESOURCES, &res))
        return -1;
    conns = zalloc(res.count_connectors, 4);
    crtcs = zalloc(res.count_crtcs, 4);
    encs = zalloc(res.count_encoders, 4);
    fbs = zalloc(res.count_fbs, 4);
    res.connector_id_ptr = (uintptr_t)conns;
    res.crtc_id_ptr = (uintptr_t)crtcs;
    res.encoder_id_ptr = (uintptr_t)encs;
    res.fb_id_ptr = (uintptr_t)fbs;
    if (ioctl(s->fd, DRM_IOCTL_MODE_GETRESOURCES, &res))
        goto out;

    for (uint32_t i = 0; i < res.count_connectors && !found; i++) {
        struct drm_mode_get_connector conn = {.connector_id = conns[i]};
        struct drm_mode_modeinfo *modes;
        uint32_t *cencs;

        if (ioctl(s->fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) || conn.connection != 1 || !conn.count_modes)
            continue;
        modes = zalloc(conn.count_modes, sizeof(*modes));
        cencs = zalloc(conn.count_encoders, 4);
        conn.modes_ptr = (uintptr_t)modes;
        conn.encoders_ptr = (uintptr_t)cencs;
        conn.count_props = 0;
        conn.props_ptr = 0;
        conn.prop_values_ptr = 0;
        if (!ioctl(s->fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) && conn.count_modes) {
            uint32_t enc_id = conn.encoder_id ? conn.encoder_id : (conn.count_encoders ? cencs[0] : 0);
            struct drm_mode_get_encoder enc = {.encoder_id = enc_id};
            uint32_t m = 0;

            for (uint32_t k = 0; k < conn.count_modes; k++)
                if (modes[k].type & DRM_MODE_TYPE_PREFERRED) {
                    m = k;
                    break;
                }
            if (enc_id && !ioctl(s->fd, DRM_IOCTL_MODE_GETENCODER, &enc)) {
                uint32_t crtc = enc.crtc_id;
                for (uint32_t k = 0; !crtc && k < res.count_crtcs; k++)
                    if (enc.possible_crtcs & (1u << k))
                        crtc = crtcs[k];
                if (crtc) {
                    s->conn_id = conn.connector_id;
                    s->crtc_id = crtc;
                    s->mode = modes[m];
                    found = 1;
                }
            }
        }
        free(modes);
        free(cencs);
    }
out:
    free(conns);
    free(crtcs);
    free(encs);
    free(fbs);
    return found ? 0 : -1;
}

int screen_init(struct screen *s, int fd)
{
    memset(s, 0, sizeof(*s));
    s->fd = fd;
    if (fd < 0 || pick_output(s))
        return -1;

    struct drm_mode_create_dumb cd = {.width = s->mode.hdisplay, .height = s->mode.vdisplay, .bpp = 32};
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd))
        return -1;
    struct drm_mode_fb_cmd fb = {
        .width = cd.width, .height = cd.height, .pitch = cd.pitch, .bpp = 32, .depth = 24, .handle = cd.handle,
    };
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb))
        return -1;
    struct drm_mode_map_dumb md = {.handle = cd.handle};
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md))
        return -1;
    void *map = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)md.offset);
    if (map == MAP_FAILED)
        return -1;

    s->width = cd.width;
    s->height = cd.height;
    s->pitch = cd.pitch;
    s->size = cd.size;
    s->pixels = map;
    s->fb_id = fb.fb_id;
    return 0;
}

int screen_open(struct screen *s, int fd)
{
    char card[32];

    if (fd >= 0 && !screen_init(s, fd))
        return 0;
    for (int i = 0; i < 8; i++) {
        snprintf(card, sizeof(card), "/dev/dri/card%d", i);
        int cfd = open(card, O_RDWR);
        if (cfd < 0)
            continue;
        if (!screen_init(s, cfd))
            return 0;
        close(cfd);
    }
    return -1;
}

void screen_fill(struct screen *s, int x, int y, int w, int h, uint32_t xrgb)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)s->width) w = (int)s->width - x;
    if (y + h > (int)s->height) h = (int)s->height - y;
    for (int row = 0; row < h; row++) {
        uint32_t *p = (uint32_t *)(s->pixels + (size_t)(y + row) * s->pitch) + x;
        for (int col = 0; col < w; col++)
            p[col] = xrgb;
    }
}

int screen_show(struct screen *s)
{
    struct drm_mode_crtc set = {
        .set_connectors_ptr = (uintptr_t)&s->conn_id,
        .count_connectors = 1,
        .crtc_id = s->crtc_id,
        .fb_id = s->fb_id,
        .mode_valid = 1,
        .mode = s->mode,
    };
    return ioctl(s->fd, DRM_IOCTL_MODE_SETCRTC, &set);
}

int screen_flush(struct screen *s)
{
    struct drm_mode_fb_dirty_cmd dirty = {.fb_id = s->fb_id};

    return ioctl(s->fd, DRM_IOCTL_MODE_DIRTYFB, &dirty);
}
