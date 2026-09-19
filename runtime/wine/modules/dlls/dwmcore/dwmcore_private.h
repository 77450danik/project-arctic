/*
 * Arctic desktop composition engine: unix-side internals
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_DWMCORE_PRIVATE_H
#define __WINE_DWMCORE_PRIVATE_H

#include <stddef.h>
#include <stdint.h>

/* The screen: one dumb framebuffer on the first connected output (kms.c) */
struct kms_output
{
    int       fd;
    uint32_t  width, height, pitch;   /* pitch in bytes */
    uint32_t  refresh_mhz;
    uint32_t *pixels;                 /* XRGB8888 */
    char      name[32];               /* card node, for the log */
};

extern struct kms_output kms;

int  kms_init(void);   /* 0 on success */
void kms_flush(void);  /* after drawing: shadow-buffered drivers copy only on this */

#endif
