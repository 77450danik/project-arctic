/* The boot screen (bootanim.h): the logo and Windows 11's spinner, drawn
 * by a process of its own from the initrd until the desktop comes. */
#define _GNU_SOURCE
#include "bootanim.h"

#include "screen.h"
#include "stop.h"

#include <drm/drm.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FRAME_NS 16666667 /* Windows 11's ring runs at 60 frames a second */
#define GOLDEN 0.382      /* where a boot logo's middle sits, from the top, in Windows */

struct image {
    uint32_t w, h;
    uint32_t *px; /* XRGB */
};

static struct {
    int devfd;                   /* /dev, which the initrd moves under the new root */
    const char *spinner_path;
    struct image logo;           /* Arctic's */
    struct screen s;
    int attached, simple;        /* on a card; that card is simpledrm (the firmware framebuffer) */
    int show_error;              /* why the picture could not go on screen, said once */
    uint32_t frames, dim;        /* the spinner: frame count, the side of a frame */
    uint8_t *alpha;              /* frames * dim * dim */
    int cx, cy;                  /* the spinner's middle */
    int update;                  /* the "working on updates" screen */
    char status[256], line1[256], line2[256];
} a = {.devfd = -1};

static void say(const char *fmt, ...)
{
    char buf[300];
    int n = snprintf(buf, sizeof(buf), "<5>bootanim: "), fd;
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf + n, sizeof(buf) - (size_t)n, fmt, ap);
    va_end(ap);
    /* through the /dev it opened: the initrd's own /dev moves into the new root */
    if ((fd = a.devfd >= 0 ? openat(a.devfd, "kmsg", O_WRONLY | O_CLOEXEC) : open("/dev/kmsg", O_WRONLY | O_CLOEXEC)) >= 0) {
        if (write(fd, buf, strlen(buf)) < 0)
            n = 0;
        close(fd);
    }
}

static void *read_all(const char *path, size_t *size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    size_t len = 0, cap = 0;
    uint8_t *data = NULL;
    ssize_t n = 1;

    if (fd < 0)
        return NULL;
    /* read to the end, whatever the file says its size is */
    while (n > 0) {
        if (len == cap) {
            uint8_t *grown = realloc(data, cap = cap ? cap * 2 : 1 << 16);
            if (!grown) {
                n = -1;
                break;
            }
            data = grown;
        }
        n = read(fd, data + len, cap - len);
        if (n > 0)
            len += (size_t)n;
    }
    close(fd);
    if (n < 0 || !len) {
        free(data);
        return NULL;
    }
    *size = len;
    return data;
}

/* Arctic's logo: "ARLG", width, height, XRGB pixels (ci/logo2raw.py) */
static int load_logo(const char *path)
{
    size_t size = 0;
    uint32_t *data = read_all(path, &size);

    if (!data || size < 12 || data[0] != 0x474c5241u || size < 12 + (size_t)data[1] * data[2] * 4) {
        free(data);
        return -1;
    }
    a.logo.w = data[1];
    a.logo.h = data[2];
    a.logo.px = data + 3;
    return 0;
}

/* The frames for a spinner about want pixels across, from the file
 * ci/mkspinner.py makes: "ARSP", frames, sizes; per size: side, then each
 * frame's side * side alpha bytes */
static void load_spinner(uint32_t want)
{
    uint32_t hdr[3], dim, best = 0, best_at = 0;
    int fd = open(a.spinner_path, O_RDONLY | O_CLOEXEC);
    off_t at = 12;

    if (fd < 0 || pread(fd, hdr, 12, 0) != 12 || hdr[0] != 0x50535241u || !hdr[1]) {
        if (fd >= 0)
            close(fd);
        return;
    }
    for (uint32_t i = 0; i < hdr[2] && pread(fd, &dim, 4, at) == 4; i++) {
        if (!best || abs((int)dim - (int)want) < abs((int)best - (int)want)) {
            best = dim;
            best_at = (uint32_t)at + 4;
        }
        at += 4 + (off_t)hdr[1] * dim * dim;
    }
    if (best && best != a.dim) {
        size_t len = (size_t)hdr[1] * best * best;
        uint8_t *alpha = malloc(len);
        if (alpha && pread(fd, alpha, len, best_at) == (ssize_t)len) {
            free(a.alpha);
            a.alpha = alpha;
            a.dim = best;
            a.frames = hdr[1];
        } else {
            free(alpha);
        }
    }
    close(fd);
}

static double text_scale(void)
{
    double wf = a.s.width / 3840.0, hf = a.s.height / 2160.0, f = wf < hf ? wf : hf;
    return f < 0.36 ? 0.36 : f;
}

static void dirty(int x, int y, int w, int h)
{
    struct drm_clip_rect clip = {
        .x1 = (unsigned short)(x < 0 ? 0 : x), .y1 = (unsigned short)(y < 0 ? 0 : y),
        .x2 = (unsigned short)(x + w > (int)a.s.width ? a.s.width : (unsigned)(x + w)),
        .y2 = (unsigned short)(y + h > (int)a.s.height ? a.s.height : (unsigned)(y + h)),
    };
    struct drm_mode_fb_dirty_cmd cmd = {.fb_id = a.s.fb_id, .num_clips = 1, .clips_ptr = (uintptr_t)&clip};

    ioctl(a.s.fd, DRM_IOCTL_MODE_DIRTYFB, &cmd); /* only for drivers that copy (simpledrm, virtual GPUs) */
}

static void draw_logo(void)
{
    uint32_t scale = 1;
    int x0, y0;

    if (!a.logo.px)
        return;
    /* it grows with the screen, as Windows' does */
    scale = a.s.height >= 2000 ? 3 : a.s.height >= 1400 ? 2 : 1;
    while (scale > 1 && (a.logo.w * scale > a.s.width || a.logo.h * scale > a.s.height))
        scale--;
    x0 = ((int)a.s.width - (int)(a.logo.w * scale)) / 2;
    y0 = (int)(a.s.height * GOLDEN) - (int)(a.logo.h * scale) / 2;
    for (uint32_t y = 0; y < a.logo.h * scale; y++) {
        int py = y0 + (int)y;
        if (py < 0 || py >= (int)a.s.height)
            continue;
        uint32_t *row = (uint32_t *)(a.s.pixels + (size_t)py * a.s.pitch);
        const uint32_t *src = a.logo.px + (size_t)(y / scale) * a.logo.w;
        for (uint32_t x = 0; x < a.logo.w * scale; x++) {
            int px = x0 + (int)x;
            if (px >= 0 && px < (int)a.s.width)
                row[px] = src[x / scale];
        }
    }
}

/* Everything but the spinner: the logo or nothing, and the lines under it */
static void draw_background(void)
{
    double f = text_scale();

    /* Windows 11's ring is about a twentieth of the screen's height across */
    load_spinner((uint32_t)(a.s.height * 0.045 + 0.5));
    a.cx = (int)a.s.width / 2;
    a.cy = (int)(a.s.height * (a.update ? 0.43 : 0.75));
    screen_fill(&a.s, 0, 0, (int)a.s.width, (int)a.s.height, 0);
    if (a.update) {
        double base = a.cy + a.dim * 0.5 + boot_line_height(1, f * 0.8) * 1.6;
        boot_text(&a.s, 1, a.line1, base, f * 0.8, 0);
        boot_text(&a.s, 1, a.line2, base + boot_line_height(1, f * 0.8) * 1.25, f * 0.8, 0);
    } else {
        draw_logo();
        if (a.status[0])
            boot_text(&a.s, 0, a.status, a.cy + a.dim * 0.5 + boot_line_height(0, f) * 2.2, f, 0);
    }
}

static void draw_spinner(uint32_t frame)
{
    int x0 = a.cx - (int)a.dim / 2, y0 = a.cy - (int)a.dim / 2;
    const uint8_t *src;

    if (!a.alpha)
        return;
    src = a.alpha + (size_t)(frame % a.frames) * a.dim * a.dim;
    for (uint32_t y = 0; y < a.dim; y++) {
        int py = y0 + (int)y;
        if (py < 0 || py >= (int)a.s.height)
            continue;
        uint32_t *row = (uint32_t *)(a.s.pixels + (size_t)py * a.s.pitch);
        for (uint32_t x = 0; x < a.dim; x++) {
            int px = x0 + (int)x;
            uint32_t v = src[y * a.dim + x];
            if (px >= 0 && px < (int)a.s.width)
                row[px] = v << 16 | v << 8 | v;
        }
    }
    dirty(x0, y0, (int)a.dim, (int)a.dim);
}

static int card_is_simple(int fd)
{
    char name[32] = "";
    struct drm_version v = {.name_len = sizeof(name) - 1, .name = name};

    return !ioctl(fd, DRM_IOCTL_VERSION, &v) && !strcmp(name, "simpledrm");
}

static void detach(void)
{
    if (!a.attached)
        return;
    munmap(a.s.pixels, a.s.size);
    close(a.s.fd);
    a.attached = 0;
}

/* The card to draw on: one a GPU driver brought, else the firmware's */
static void attach(void)
{
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < 8; i++) {
            char card[16];
            int fd;

            snprintf(card, sizeof(card), "dri/card%d", i);
            if ((fd = openat(a.devfd, card, O_RDWR | O_CLOEXEC)) < 0)
                continue;
            int simple = card_is_simple(fd);
            if ((!pass && simple) || screen_init(&a.s, fd)) {
                close(fd);
                continue;
            }
            a.simple = simple;
            a.attached = 1;
            draw_background();
            draw_spinner(0);
            /* Someone else still has the display (dwm.exe going away as the
             * machine shuts down): tried again on the next check */
            if (screen_show(&a.s)) {
                if (errno != a.show_error)
                    say("card%d: no picture yet (%s)", i, strerror(errno));
                a.show_error = errno;
                detach();
                return;
            }
            a.show_error = 0;
            say("card%d %ux%u%s", i, a.s.width, a.s.height, simple ? " (firmware framebuffer)" : "");
            return;
        }
}

/* Still on a card, and on the best one there is? */
static void check_card(void)
{
    struct drm_mode_card_res res = {0};

    if (a.attached && ioctl(a.s.fd, DRM_IOCTL_MODE_GETRESOURCES, &res)) {
        say("the card went away");
        detach();
    }
    if (a.attached && a.simple) {
        for (int i = 0; i < 8; i++) {
            char card[16];
            int fd;
            snprintf(card, sizeof(card), "dri/card%d", i);
            if ((fd = openat(a.devfd, card, O_RDWR | O_CLOEXEC)) < 0)
                continue;
            int other = !card_is_simple(fd);
            close(fd);
            if (other) { /* a GPU driver came; its card takes over at once */
                detach();
                break;
            }
        }
    }
    if (!a.attached)
        attach();
}

static void command(char *line)
{
    char *arg = strchr(line, ' ');

    if (arg)
        *arg++ = 0;
    else
        arg = line + strlen(line);
    if (!strcmp(line, "status")) {
        snprintf(a.status, sizeof(a.status), "%s", arg);
    } else if (!strcmp(line, "update")) {
        char *tab = strchr(arg, '\t');
        if (tab)
            *tab++ = 0;
        snprintf(a.line1, sizeof(a.line1), "%s", arg);
        snprintf(a.line2, sizeof(a.line2), "%s", tab ? tab : "");
        a.update = 1;
    } else if (!strcmp(line, "boot")) {
        a.update = 0;
        a.status[0] = 0;
    }
    if (a.attached) {
        draw_background();
        dirty(0, 0, (int)a.s.width, (int)a.s.height);
    }
}

static void run(int ctl)
{
    char buf[1024];
    size_t used = 0;
    struct timespec start, next;
    uint64_t tick = 0;
    int released = 0, taken = 0; /* given up the display; dwm.exe has it on the screen */

    clock_gettime(CLOCK_MONOTONIC, &start);
    next = start;
    for (;;) {
        ssize_t n = read(ctl, buf + used, sizeof(buf) - 1 - used);
        if (n == 0)
            break; /* the pipe closed: the desktop is there */
        if (n > 0) {
            used += (size_t)n;
            buf[used] = 0;
            for (char *nl; (nl = strchr(buf, '\n'));) {
                *nl = 0;
                if (!strcmp(buf, "release")) {
                    ioctl(a.s.fd, DRM_IOCTL_DROP_MASTER, 0);
                    released = 1;
                } else if (!released) {
                    command(buf);
                }
                used -= (size_t)(nl + 1 - buf);
                memmove(buf, nl + 1, used + 1);
            }
            if (used == sizeof(buf) - 1)
                used = 0;
        }
        if (!released && tick % 15 == 0)
            check_card();
        /* Given up, the spinner still turns in the picture on the screen
         * until dwm.exe puts a picture of its own there, as Windows' turns
         * until the desktop: drawing into a buffer on the screen needs no
         * DRM master. */
        if (released && !taken && a.attached && tick % 6 == 0) {
            struct drm_mode_crtc crtc = {.crtc_id = a.s.crtc_id};
            taken = ioctl(a.s.fd, DRM_IOCTL_MODE_GETCRTC, &crtc) || crtc.fb_id != a.s.fb_id;
        }
        if (a.attached && !taken) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            uint64_t ns = (uint64_t)(now.tv_sec - start.tv_sec) * 1000000000u + (uint64_t)now.tv_nsec -
                          (uint64_t)start.tv_nsec;
            draw_spinner((uint32_t)(ns / FRAME_NS));
        }
        tick++;
        next.tv_nsec += taken ? 100000000 : FRAME_NS;
        while (next.tv_nsec >= 1000000000) {
            next.tv_nsec -= 1000000000;
            next.tv_sec++;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    _exit(0);
}

int bootanim_start(const char *spinner, const char *logo, const char *font, pid_t *pid)
{
    int p[2];

    if (pipe2(p, O_CLOEXEC))
        return -1;
    if ((*pid = fork()) < 0) {
        close(p[0]);
        close(p[1]);
        return -1;
    }
    if (*pid) {
        close(p[0]);
        return p[1];
    }
    close(p[1]);
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    signal(SIGPIPE, SIG_IGN);
    a.devfd = open("/dev", O_PATH | O_DIRECTORY | O_CLOEXEC);
    a.spinner_path = spinner;
    if (load_logo(logo))
        say("no logo in %s", logo);
    boot_font_load(font);
    run(p[0]);
    return -1;
}

void bootanim_send(int ctl, const char *fmt, ...)
{
    char line[600];
    va_list ap;
    int n;

    if (ctl < 0)
        return;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof(line) - 2)
        n = (int)sizeof(line) - 2;
    line[n++] = '\n';
    if (write(ctl, line, (size_t)n) < 0)
        return;
}

void bootanim_kill(int *ctl, pid_t *pid)
{
    if (*pid > 0) {
        kill(*pid, SIGKILL);
        waitpid(*pid, NULL, 0);
    }
    if (*ctl >= 0)
        close(*ctl);
    *pid = 0;
    *ctl = -1;
}
