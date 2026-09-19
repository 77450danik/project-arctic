#include "splash.h"

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/* Logo file: "ARLG", u32 width, u32 height, then width*height pixels as
 * little-endian XRGB8888 (bytes B, G, R, 0), produced by ci/logo2raw.py. */
#define LOGO_MAGIC 0x474c5241u

struct logo {
    uint32_t w, h;
    uint32_t *px;
};

static int read_full(int fd, void *buf, size_t len)
{
    char *p = buf;
    while (len) {
        ssize_t n = read(fd, p, len);
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int load_logo(const char *path, struct logo *logo)
{
    uint32_t hdr[3];
    int fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return -1;
    if (read_full(fd, hdr, sizeof(hdr)) || hdr[0] != LOGO_MAGIC || !hdr[1] || !hdr[2] ||
        hdr[1] > 4096 || hdr[2] > 4096)
        goto fail;
    logo->w = hdr[1];
    logo->h = hdr[2];
    logo->px = malloc((size_t)logo->w * logo->h * 4);
    if (!logo->px || read_full(fd, logo->px, (size_t)logo->w * logo->h * 4))
        goto fail;
    close(fd);
    return 0;
fail:
    free(logo->px);
    close(fd);
    return -1;
}

static void *zalloc(uint32_t count, size_t size)
{
    return calloc(count ? count : 1, size);
}

/* Finds a connected connector, its preferred mode and a CRTC that can drive it. */
static int pick_output(int fd, uint32_t *conn_out, uint32_t *crtc_out, struct drm_mode_modeinfo *mode_out)
{
    struct drm_mode_card_res res = {0};
    uint32_t *conns = NULL, *crtcs = NULL, *encs = NULL, *fbs = NULL;
    int found = 0;

    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res))
        return -1;
    conns = zalloc(res.count_connectors, 4);
    crtcs = zalloc(res.count_crtcs, 4);
    encs = zalloc(res.count_encoders, 4);
    fbs = zalloc(res.count_fbs, 4);
    res.connector_id_ptr = (uintptr_t)conns;
    res.crtc_id_ptr = (uintptr_t)crtcs;
    res.encoder_id_ptr = (uintptr_t)encs;
    res.fb_id_ptr = (uintptr_t)fbs;
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res))
        goto out;

    for (uint32_t i = 0; i < res.count_connectors && !found; i++) {
        struct drm_mode_get_connector conn = {.connector_id = conns[i]};
        struct drm_mode_modeinfo *modes;
        uint32_t *cencs;

        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) || conn.connection != 1 || !conn.count_modes)
            continue;
        modes = zalloc(conn.count_modes, sizeof(*modes));
        cencs = zalloc(conn.count_encoders, 4);
        conn.modes_ptr = (uintptr_t)modes;
        conn.encoders_ptr = (uintptr_t)cencs;
        conn.count_props = 0;
        conn.props_ptr = 0;
        conn.prop_values_ptr = 0;
        if (!ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) && conn.count_modes) {
            uint32_t enc_id = conn.encoder_id ? conn.encoder_id : (conn.count_encoders ? cencs[0] : 0);
            struct drm_mode_get_encoder enc = {.encoder_id = enc_id};
            uint32_t m = 0;

            for (uint32_t k = 0; k < conn.count_modes; k++)
                if (modes[k].type & DRM_MODE_TYPE_PREFERRED) {
                    m = k;
                    break;
                }
            if (enc_id && !ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc)) {
                uint32_t crtc = enc.crtc_id;
                for (uint32_t k = 0; !crtc && k < res.count_crtcs; k++)
                    if (enc.possible_crtcs & (1u << k))
                        crtc = crtcs[k];
                if (crtc) {
                    *conn_out = conn.connector_id;
                    *crtc_out = crtc;
                    *mode_out = modes[m];
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

int splash_show(const char *card, const char *logo_path)
{
    struct logo logo = {0};
    struct drm_mode_modeinfo mode;
    uint32_t conn_id, crtc_id;
    int fd;

    if (load_logo(logo_path, &logo))
        return -1;
    fd = open(card, O_RDWR); /* no O_CLOEXEC: the next init keeps the picture alive */
    if (fd < 0)
        goto fail_logo;
    if (pick_output(fd, &conn_id, &crtc_id, &mode))
        goto fail;

    struct drm_mode_create_dumb cd = {.width = mode.hdisplay, .height = mode.vdisplay, .bpp = 32};
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd))
        goto fail;
    struct drm_mode_fb_cmd fb = {
        .width = cd.width, .height = cd.height, .pitch = cd.pitch, .bpp = 32, .depth = 24, .handle = cd.handle,
    };
    if (ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb))
        goto fail;
    struct drm_mode_map_dumb md = {.handle = cd.handle};
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md))
        goto fail;
    uint8_t *map = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)md.offset);
    if (map == MAP_FAILED)
        goto fail;

    memset(map, 0, cd.size);
    uint32_t scale = cd.height >= 2000 ? 3 : cd.height >= 1400 ? 2 : 1;
    while (scale > 1 && (logo.w * scale > cd.width || logo.h * scale > cd.height))
        scale--;
    if (logo.w <= cd.width && logo.h <= cd.height) {
        uint32_t x0 = (cd.width - logo.w * scale) / 2, y0 = (cd.height - logo.h * scale) / 2;
        for (uint32_t y = 0; y < logo.h * scale; y++) {
            uint32_t *row = (uint32_t *)(map + (size_t)(y0 + y) * cd.pitch) + x0;
            const uint32_t *src = logo.px + (size_t)(y / scale) * logo.w;
            for (uint32_t x = 0; x < logo.w * scale; x++)
                row[x] = src[x / scale];
        }
    }
    munmap(map, cd.size);

    struct drm_mode_crtc set = {
        .set_connectors_ptr = (uintptr_t)&conn_id,
        .count_connectors = 1,
        .crtc_id = crtc_id,
        .fb_id = fb.fb_id,
        .mode_valid = 1,
        .mode = mode,
    };
    if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &set))
        goto fail;
    free(logo.px);
    return fd;

fail:
    close(fd);
fail_logo:
    free(logo.px);
    return -1;
}
