/* Renders the stop screen (host/init/stop.c) into a PPM file on the build
 * machine, so its look can be checked without booting anything.
 *
 * usage: bsod-preview <atlas.font> <out.ppm> <width> <height> <code> [what] [percent] */
#define STOP_PREVIEW
#define ARCTIC_SCREEN_H /* replaces the DRM framebuffer with plain memory */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

struct screen {
    uint32_t width, height, pitch;
    uint8_t *pixels;
};

static void screen_fill(struct screen *s, int x, int y, int w, int h, uint32_t xrgb)
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

#include "../../host/init/stop.c"

int main(int argc, char **argv)
{
    struct screen s;
    struct font font;
    struct layout l;
    FILE *out;

    if (argc < 6) {
        fprintf(stderr, "usage: %s atlas out.ppm width height code [what] [percent]\n", argv[0]);
        return 2;
    }
    s.width = (uint32_t)atoi(argv[3]);
    s.height = (uint32_t)atoi(argv[4]);
    s.pitch = s.width * 4;
    s.pixels = calloc(s.height, s.pitch);
    if (!s.pixels || load_font(argv[1], &font)) {
        fprintf(stderr, "cannot load %s\n", argv[1]);
        return 1;
    }
    stop_draw(&s, &font, argv[5], argc > 6 && argv[6][0] ? argv[6] : NULL, &l);
    stop_draw_progress(&s, &font, &l, argc > 7 ? atoi(argv[7]) : 0);

    if (!(out = fopen(argv[2], "wb")))
        return 1;
    fprintf(out, "P6\n%u %u\n255\n", s.width, s.height);
    for (uint32_t y = 0; y < s.height; y++)
        for (uint32_t x = 0; x < s.width; x++) {
            uint32_t p = ((uint32_t *)(s.pixels + (size_t)y * s.pitch))[x];
            fputc((p >> 16) & 0xff, out);
            fputc((p >> 8) & 0xff, out);
            fputc(p & 0xff, out);
        }
    fclose(out);
    return 0;
}
