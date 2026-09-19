#include "splash.h"

#include "screen.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
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

int splash_show(const char *card, const char *logo_path)
{
    struct logo logo = {0};
    struct screen s;
    int fd;

    if (load_logo(logo_path, &logo))
        return -1;
    fd = open(card, O_RDWR); /* no O_CLOEXEC: the next init keeps the picture alive */
    if (fd < 0 || screen_init(&s, fd)) {
        if (fd >= 0)
            close(fd);
        free(logo.px);
        return -1;
    }

    screen_fill(&s, 0, 0, (int)s.width, (int)s.height, 0);
    uint32_t scale = s.height >= 2000 ? 3 : s.height >= 1400 ? 2 : 1;
    while (scale > 1 && (logo.w * scale > s.width || logo.h * scale > s.height))
        scale--;
    if (logo.w <= s.width && logo.h <= s.height) {
        uint32_t x0 = (s.width - logo.w * scale) / 2, y0 = (s.height - logo.h * scale) / 2;
        for (uint32_t y = 0; y < logo.h * scale; y++) {
            uint32_t *row = (uint32_t *)(s.pixels + (size_t)(y0 + y) * s.pitch) + x0;
            const uint32_t *src = logo.px + (size_t)(y / scale) * logo.w;
            for (uint32_t x = 0; x < logo.w * scale; x++)
                row[x] = src[x / scale];
        }
    }
    munmap(s.pixels, s.size);
    free(logo.px);
    if (screen_show(&s)) {
        close(fd);
        return -1;
    }
    return fd;
}
