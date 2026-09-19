#ifndef ARCTIC_SPLASH_H
#define ARCTIC_SPLASH_H

/* Draws the boot logo centred on a black screen of the given DRM card.
 * Returns the card fd (kept open to hold the picture on screen, and
 * inheritable across exec on purpose), or -1. */
int splash_show(const char *card, const char *logo_path);

#endif
