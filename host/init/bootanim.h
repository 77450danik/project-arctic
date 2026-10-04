#ifndef ARCTIC_BOOTANIM_H
#define ARCTIC_BOOTANIM_H

#include <sys/types.h>

/* The boot screen, as Windows 11 draws it: Arctic's logo and Windows 11's
 * spinner below, from the first moment of the initrd until the desktop. (Not
 * the PC maker's logo from the firmware's BGRT, which Windows keeps: Arctic
 * shows its own.) A process of its own draws it, 60 frames a second, and
 * draws it again at once on the card a GPU driver brings when it takes the
 * screen from the firmware framebuffer.
 *
 * It takes commands, one line each, on the pipe bootanim_start returns:
 *   status <text>          a line under the spinner ("" for none)
 *   update <line>\t<line>  Windows' "working on updates" screen: no logo,
 *                          the spinner higher, two lines under it
 *   boot                   back to the logo and the spinner
 *   release                give up the display (DRM master) for dwm.exe,
 *                          the spinner turning on until dwm's own picture
 *                          is on the screen
 * It ends when the pipe closes; a stop screen kills it first. */

/* Starts it; *pid is the process. Returns the command pipe, or -1. */
int bootanim_start(const char *spinner, const char *logo, const char *font, pid_t *pid);

void bootanim_send(int ctl, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Ends it at once and waits until the display is free (for a stop screen) */
void bootanim_kill(int *ctl, pid_t *pid);

#endif
