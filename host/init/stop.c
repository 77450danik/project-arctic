#include "stop.h"

#include "screen.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLUE 0x0078d7u
#define WHITE 0xffffffu

static const char MESSAGE[] = "На вашому ПК виникла проблема, і його потрібно перезавантажити. "
                              "Ми лише збираємо деякі відомості про помилку, а потім перезавантажимо його.";
static const char PROGRESS[] = "Виконано %d%%";
static const char CODE_LABEL[] = "Код зупинки: ";
static const char WHAT_LABEL[] = "Що спричинило збій: ";

/* Glyph atlas written by ci/mkfont.py */
enum { SET_FACE, SET_MAIN, SET_SMALL, SET_BOOT, SET_UPDATE, SET_COUNT };

struct __attribute__((packed)) glyph {
    uint32_t cp;
    int16_t left, top;
    uint16_t w, h, adv;
    uint32_t off;
};

struct fontset {
    uint32_t line_height, ascent, count;
    const struct glyph *glyphs;
};

struct font {
    struct fontset set[SET_COUNT];
    const uint8_t *bitmaps;
    uint32_t bitmaps_size;
};

/* Where the progress line goes, filled in by stop_draw */
struct layout {
    double f, x, max_w, progress_base, main_h;
};

static int parse_font(const uint8_t *data, size_t size, struct font *font)
{
    size_t pos = 12;
    uint32_t sets;

    memset(font, 0, sizeof(*font));
    if (size < 16 || memcmp(data, "ARFN", 4))
        return -1;
    memcpy(&sets, data + 8, 4);
    for (uint32_t i = 0; i < sets; i++) {
        uint32_t hdr[4];
        if (pos + 16 > size)
            return -1;
        memcpy(hdr, data + pos, 16);
        pos += 16;
        if (pos + (size_t)hdr[3] * sizeof(struct glyph) > size)
            return -1;
        if (hdr[0] < SET_COUNT) {
            font->set[hdr[0]].line_height = hdr[1];
            font->set[hdr[0]].ascent = hdr[2];
            font->set[hdr[0]].count = hdr[3];
            font->set[hdr[0]].glyphs = (const struct glyph *)(data + pos);
        }
        pos += (size_t)hdr[3] * sizeof(struct glyph);
    }
    if (pos + 4 > size)
        return -1;
    memcpy(&font->bitmaps_size, data + pos, 4);
    font->bitmaps = data + pos + 4;
    return pos + 4 + font->bitmaps_size <= size ? 0 : -1;
}

static int load_font(const char *path, struct font *font)
{
    FILE *f = fopen(path, "rb");
    uint8_t *data;
    long size;

    if (!f)
        return -1;
    if (fseek(f, 0, SEEK_END) || (size = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET) ||
        !(data = malloc((size_t)size)) || fread(data, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return parse_font(data, (size_t)size, font); /* the atlas stays loaded for good */
}

static uint32_t utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t c = *p++;

    if (c >= 0xf0 && p[0] && p[1] && p[2]) {
        c = ((c & 0x07) << 18) | ((p[0] & 0x3fu) << 12) | ((p[1] & 0x3fu) << 6) | (p[2] & 0x3fu);
        p += 3;
    } else if (c >= 0xe0 && p[0] && p[1]) {
        c = ((c & 0x0f) << 12) | ((p[0] & 0x3fu) << 6) | (p[1] & 0x3fu);
        p += 2;
    } else if (c >= 0xc0 && p[0]) {
        c = ((c & 0x1f) << 6) | (p[0] & 0x3fu);
        p += 1;
    }
    *s = (const char *)p;
    return c;
}

static const struct glyph *find_glyph(const struct fontset *fs, uint32_t cp)
{
    uint32_t lo = 0, hi = fs->count;

    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (fs->glyphs[mid].cp == cp)
            return &fs->glyphs[mid];
        if (fs->glyphs[mid].cp < cp)
            lo = mid + 1;
        else
            hi = mid;
    }
    return cp == '?' ? NULL : find_glyph(fs, '?');
}

static int ifloor(double v)
{
    int i = (int)v;
    return v < i ? i - 1 : i;
}

static uint32_t text_bg = BLUE; /* what glyph edges blend into */

static uint32_t blend(uint32_t bg, uint32_t fg, unsigned a)
{
    uint32_t out = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        int b = (bg >> shift) & 0xff, f = (fg >> shift) & 0xff;
        out |= (uint32_t)(b + (f - b) * (int)a / 255) << shift;
    }
    return out;
}

/* f = screen pixels per atlas pixel. Shrinking averages n x n samples per
 * pixel so the text stays smooth at 1080p and below. */
static void draw_glyph(struct screen *s, const struct font *font, const struct glyph *g, double x, double baseline,
                       double f)
{
    if (!g->w || !g->h || g->off + (uint32_t)g->w * g->h > font->bitmaps_size)
        return;
    const uint8_t *src = font->bitmaps + g->off;
    double gx = x + g->left * f, gy = baseline + g->top * f;
    int x0 = ifloor(gx), y0 = ifloor(gy);
    int dw = (int)(g->w * f) + 2, dh = (int)(g->h * f) + 2;
    int n = f < 1.0 ? (int)(1.0 / f + 0.999) : 1;

    if (n > 4)
        n = 4;
    for (int dy = 0; dy < dh; dy++) {
        int py = y0 + dy;
        if (py < 0 || py >= (int)s->height)
            continue;
        uint32_t *row = (uint32_t *)(s->pixels + (size_t)py * s->pitch);
        for (int dx = 0; dx < dw; dx++) {
            int px = x0 + dx;
            unsigned sum = 0;
            if (px < 0 || px >= (int)s->width)
                continue;
            for (int sy = 0; sy < n; sy++)
                for (int sx = 0; sx < n; sx++) {
                    double u = (px + (sx + 0.5) / n - gx) / f, v = (py + (sy + 0.5) / n - gy) / f;
                    if (u >= 0 && v >= 0 && (int)u < g->w && (int)v < g->h)
                        sum += src[(int)v * g->w + (int)u];
                }
            if (sum)
                row[px] = blend(text_bg, WHITE, sum / (unsigned)(n * n));
        }
    }
}

static double draw_text(struct screen *s, const struct font *font, int set, const char *text, size_t len, double x,
                        double baseline, double f)
{
    const struct fontset *fs = &font->set[set];
    const char *p = text, *end = text + len;

    while (p < end && *p) {
        const struct glyph *g = find_glyph(fs, utf8_next(&p));
        if (!g)
            continue;
        draw_glyph(s, font, g, x, baseline, f);
        x += g->adv * f;
    }
    return x;
}

/* Greedy word wrap; returns the baseline below the last line */
static double draw_paragraph(struct screen *s, const struct font *font, int set, const char *text, double x,
                             double baseline, double max_w, double f)
{
    const struct fontset *fs = &font->set[set];
    double line_h = fs->line_height * f * 1.2;
    const char *p = text;

    while (*p) {
        const char *q = p, *space = NULL;
        double w = 0;

        while (*q) {
            const char *at = q;
            uint32_t cp = utf8_next(&q);
            const struct glyph *g = find_glyph(fs, cp);
            if (cp == ' ')
                space = at;
            w += g ? g->adv * f : 0;
            if (w > max_w && space) {
                q = space;
                break;
            }
        }
        draw_text(s, font, set, p, (size_t)(q - p), x, baseline, f);
        p = q;
        while (*p == ' ')
            p++;
        baseline += line_h;
    }
    return baseline;
}

/* Everything except the progress line */
static void stop_draw(struct screen *s, const struct font *font, const char *code, const char *what,
                      struct layout *l)
{
    char line[256];
    double wf = (double)s->width / 3840, hf = (double)s->height / 2160;

    l->f = wf < hf ? wf : hf;
    if (l->f < 0.36) /* small screens: keep the stop code readable, as Windows does */
        l->f = 0.36;
    l->x = s->width * 0.1;
    l->max_w = s->width * 0.75;
    l->main_h = font->set[SET_MAIN].line_height * l->f * 1.2;

    screen_fill(s, 0, 0, (int)s->width, (int)s->height, BLUE);
    draw_text(s, font, SET_FACE, ":(", 2, l->x, s->height * 0.30, l->f);
    double base = draw_paragraph(s, font, SET_MAIN, MESSAGE, l->x, s->height * 0.42, l->max_w, l->f);
    l->progress_base = base + l->main_h * 0.4;

    double small_base = l->progress_base + l->main_h * 1.8;
    snprintf(line, sizeof(line), "%s%s", CODE_LABEL, code);
    draw_text(s, font, SET_SMALL, line, strlen(line), l->x, small_base, l->f);
    if (what) {
        snprintf(line, sizeof(line), "%s%s", WHAT_LABEL, what);
        draw_text(s, font, SET_SMALL, line, strlen(line), l->x,
                  small_base + font->set[SET_SMALL].line_height * l->f * 1.4, l->f);
    }
}

static void stop_draw_progress(struct screen *s, const struct font *font, const struct layout *l, int percent)
{
    char line[64];

    screen_fill(s, (int)l->x, (int)(l->progress_base - l->main_h), (int)l->max_w, (int)(l->main_h * 1.3), BLUE);
    snprintf(line, sizeof(line), PROGRESS, percent);
    draw_text(s, font, SET_MAIN, line, strlen(line), l->x, l->progress_base, l->f);
}

#ifndef STOP_PREVIEW
#include <sys/reboot.h>
#include <unistd.h>

static double text_width(const struct fontset *fs, const char *text, double f)
{
    double w = 0;

    while (*text) {
        const struct glyph *g = find_glyph(fs, utf8_next(&text));
        w += g ? g->adv * f : 0;
    }
    return w;
}

/* The boot screen's text (bootanim.c), in the stop screen's font */
static struct font boot_font;
static int boot_font_loaded;

int boot_font_load(const char *path)
{
    if (!boot_font_loaded)
        boot_font_loaded = !load_font(path, &boot_font);
    return boot_font_loaded ? 0 : -1;
}

/* the boot screen's sets, in the regular face; an older atlas has only the
 * stop screen's light ones */
static int boot_set(int big)
{
    int set = big ? SET_UPDATE : SET_BOOT;

    if (!boot_font.set[set].line_height)
        set = big ? SET_MAIN : SET_SMALL;
    return set;
}

double boot_line_height(int big, double f)
{
    return boot_font_loaded ? boot_font.set[boot_set(big)].line_height * f : 0;
}

void boot_text(struct screen *s, int big, const char *text, double baseline, double f, uint32_t bg)
{
    int set = boot_set(big);

    if (!boot_font_loaded)
        return;
    text_bg = bg;
    draw_text(s, &boot_font, set, text, strlen(text), (s->width - text_width(&boot_font.set[set], text, f)) / 2,
              baseline, f);
    text_bg = BLUE;
}

void stop_screen(int drm_fd, const char *font_path, const char *code, const char *what, int dev_mode)
{
    static const int steps[] = {0, 10, 25, 40, 55, 70, 85, 100};
    struct screen s;
    struct font font;
    struct layout l;

    sync();
    if (!screen_open(&s, drm_fd) && !load_font(font_path, &font)) {
        stop_draw(&s, &font, code, what, &l);
        stop_draw_progress(&s, &font, &l, 0);
        screen_show(&s);
        for (size_t i = 1; i < sizeof(steps) / sizeof(*steps); i++) {
            usleep(700000);
            if (i == 1)
                sync();
            stop_draw_progress(&s, &font, &l, steps[i]);
            screen_flush(&s);
        }
    } else {
        sleep(5);
    }

    if (dev_mode)
        return;
    sleep(3);
    sync();
    reboot(RB_AUTOBOOT);
    for (;;)
        pause();
}
#endif
