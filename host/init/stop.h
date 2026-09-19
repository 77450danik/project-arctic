#ifndef ARCTIC_STOP_H
#define ARCTIC_STOP_H

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

#endif
