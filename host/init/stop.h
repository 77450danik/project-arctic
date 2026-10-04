#ifndef ARCTIC_STOP_H
#define ARCTIC_STOP_H

#include <stdint.h>

/* Windows 10 style stop screen, drawn straight into a KMS framebuffer, so it
 * works on the firmware framebuffer with no GPU driver loaded.
 *
 * code:    stop code, e.g. "CRITICAL_PROCESS_DIED"
 * what:    the component that failed, or NULL
 * drm_fd:  a card fd that may already hold DRM master, or -1
 *
 * Development builds stay on the screen and return; release builds restart
 * the machine once the progress reaches 100%, as Windows does. */
void stop_screen(int drm_fd, const char *font_path, const char *code, const char *what, int dev_mode);

/* White text centred across the screen, for the boot screen (bootanim.c):
 * big is the stop screen's main size, else its small one; f scales them as
 * for a 3840x2160 screen; bg is what the glyph edges blend into */
struct screen;
int boot_font_load(const char *path);
double boot_line_height(int big, double f);
void boot_text(struct screen *s, int big, const char *text, double baseline, double f, uint32_t bg);

#endif
