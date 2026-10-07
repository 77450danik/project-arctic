#ifndef ARCTIC_SCREEN_H
#define ARCTIC_SCREEN_H

#include <drm/drm_mode.h>
#include <stddef.h>
#include <stdint.h>

/* A dumb framebuffer on the first connected output of a DRM card. Works on
 * any KMS driver, including simpledrm on the firmware framebuffer, so it
 * needs no GPU driver at all. Pixels are XRGB8888. */
struct screen {
    int fd;
    uint32_t width, height, pitch;
    uint8_t *pixels;
    size_t size;
    uint32_t fb_id, crtc_id, conn_id;
    struct drm_mode_modeinfo mode;
    int kept_mode; /* mode is the one already on the CRTC, not the monitor's preferred */
};

/* Which step the last failed screen_init stopped at */
extern const char *screen_error;

/* Prepares a framebuffer on an already open card fd. */
int screen_init(struct screen *s, int fd);

/* Reuses fd when it still works (it may hold DRM master), otherwise tries
 * /dev/dri/card0..7. The fd is left inheritable on purpose. */
int screen_open(struct screen *s, int fd);

void screen_fill(struct screen *s, int x, int y, int w, int h, uint32_t xrgb);

/* Puts the framebuffer on screen. */
int screen_show(struct screen *s);

/* Tells the driver the framebuffer changed after it went on screen. Drivers
 * that scan out from a shadow copy (simpledrm, virtual GPUs) need this. */
int screen_flush(struct screen *s);

#endif
