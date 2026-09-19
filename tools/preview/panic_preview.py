"""Checks the drm_panic stop screen from kernel/patches without building a kernel.

Takes arctic_panic_blit() and draw_panic_screen_user() verbatim out of the
patched drm_panic.c, compiles them with stand-ins for the kernel pieces they
call, and renders a few screen sizes to PNG.

usage: panic_preview.py <patched drm_panic.c> <drm_panic_arctic.h> <zig> <out dir>
"""
import os
import re
import subprocess
import sys

from PIL import Image

patched, header, zig, out_dir = sys.argv[1:5]
source = open(patched, encoding="utf-8").read()


def function(name):
    start = source.index(f"static void {name}(")
    end = source.index("\n}\n", start) + 3
    return source[start:end]


harness = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define CONFIG_DRM_PANIC_BACKGROUND_COLOR 0x0078d7
#define CONFIG_DRM_PANIC_FOREGROUND_COLOR 0xffffff
struct drm_format_info { u32 format; };
struct drm_scanout_buffer { unsigned int width, height; const struct drm_format_info *format; u32 *px; };
struct drm_rect { int x1, y1, x2, y2; };
#define DRM_RECT_INIT(_x, _y, _w, _h) ((struct drm_rect){ .x1 = (_x), .y1 = (_y), .x2 = (_x) + (_w), .y2 = (_y) + (_h) })
struct font_desc { unsigned int width, height; };
struct drm_panic_line { u32 len; const char *txt; };
static struct drm_panic_line panic_msg[] = { { 13, "KERNEL PANIC!" }, { 0, "" }, { 0, "" }, { 0, "" }, { 0, 0 } };
static const size_t panic_msg_lines = ARRAY_SIZE(panic_msg);
static const struct font_desc font16 = { 8, 16 };
static const struct font_desc *get_default_font(int w, int h, void *a, void *b) { return &font16; }
static u32 drm_draw_color_from_xrgb8888(u32 c, u32 f) { return c; }
static void drm_panic_fill(struct drm_scanout_buffer *sb, struct drm_rect *r, u32 color)
{
    for (int y = r->y1; y < r->y2; y++)
        for (int x = r->x1; x < r->x2; x++)
            if (x >= 0 && y >= 0 && x < (int)sb->width && y < (int)sb->height) sb->px[y * sb->width + x] = color;
}
/* the reason text: a white bar per character, enough to check placement */
static void draw_txt_rectangle(struct drm_scanout_buffer *sb, const struct font_desc *font,
                               const struct drm_panic_line *msg, unsigned int lines, bool centered,
                               struct drm_rect *clip, u32 color)
{
    unsigned int n = msg->len < (clip->x2 - clip->x1) / font->width ? msg->len : (clip->x2 - clip->x1) / font->width;
    for (unsigned int i = 0; i < n; i++) {
        if (msg->txt[i] == ' ') continue;
        struct drm_rect r = DRM_RECT_INIT(clip->x1 + i * font->width + 1, clip->y1 + 3, font->width - 2, font->height - 5);
        drm_panic_fill(sb, &r, color);
    }
}
static int fallback_used;
static void draw_panic_screen_user_linux(struct drm_scanout_buffer *sb) { fallback_used = 1; }
#include "HEADER"
FUNCTIONS
int main(int argc, char **argv)
{
    struct drm_format_info fmt = { 0 };
    struct drm_scanout_buffer sb = { atoi(argv[1]), atoi(argv[2]), &fmt, 0 };
    sb.px = calloc(sb.width * sb.height, 4);
    panic_msg[panic_msg_lines - 1].txt = "Kernel panic - not syncing: Attempted to kill init! exitcode=0x0000000b";
    panic_msg[panic_msg_lines - 1].len = strlen(panic_msg[panic_msg_lines - 1].txt);
    draw_panic_screen_user(&sb);
    FILE *f = fopen(argv[3], "wb");
    fprintf(f, "P6\n%u %u\n255\n", sb.width, sb.height);
    for (unsigned int i = 0; i < sb.width * sb.height; i++) {
        fputc(sb.px[i] >> 16, f); fputc(sb.px[i] >> 8, f); fputc(sb.px[i], f);
    }
    fclose(f);
    printf("%ux%u fallback=%d\n", sb.width, sb.height, fallback_used);
    return 0;
}
'''
harness = harness.replace("HEADER", os.path.abspath(header).replace("\\", "/"))
harness = harness.replace("FUNCTIONS", function("arctic_panic_blit") + "\n" + function("draw_panic_screen_user"))
c_path = os.path.join(out_dir, "panic_harness.c")
exe = os.path.join(out_dir, "panic_harness.exe")
open(c_path, "w", encoding="utf-8").write(harness)
subprocess.run([zig, "cc", "-target", "x86_64-windows-gnu", "-O2", "-Wall", "-Wno-unused-parameter",
                "-o", exe, c_path], check=True)
for w, h in [(1024, 768), (1366, 768), (1920, 1080), (3840, 2160), (640, 480)]:
    ppm = os.path.join(out_dir, f"panic-{w}x{h}.ppm")
    print(subprocess.run([exe, str(w), str(h), ppm], check=True, capture_output=True, text=True).stdout.strip())
    Image.open(ppm).save(ppm[:-4] + ".png")
    os.remove(ppm)
