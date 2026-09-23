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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <drm/drm_mode.h>

#define KMS_MAX_OUTPUTS 8

struct kms_mode
{
    struct drm_mode_modeinfo info;
    uint32_t                 refresh;      /* mHz, from the timings */
};

/* a dumb buffer the CRTC scans out */
struct kms_fb
{
    uint32_t  fb_id, handle;
    uint32_t  width, height, pitch;        /* pitch in bytes */
    uint32_t *pixels;                      /* XRGB8888 */
    size_t    size;
};

/* A connected monitor. Slots stay where they are while it is connected,
 * so the compositor can keep pointers to them. */
struct kms_output
{
    uint32_t         connector_id;         /* 0: free slot */
    uint32_t         connector_type;
    char             name[32];             /* HDMI-A-1, as Linux names connectors */
    uint32_t         mm_width, mm_height;
    uint8_t         *edid;
    uint32_t         edid_len;
    struct kms_mode *modes;                /* one per size and whole Hz */
    uint32_t         mode_count;
    uint32_t         preferred;            /* index into modes */
    uint32_t         possible_crtcs;       /* bit per CRTC of the card */

    /* what it shows */
    bool             enabled;
    uint32_t         crtc_id;              /* when off: the one firmware used, 0 if none */
    uint32_t         mode;                 /* index into modes */
    int32_t          x, y;                 /* in the virtual screen */
    struct kms_fb    fb;

    void            *user;                 /* the compositor's */
};

struct kms_card
{
    int               fd;
    char              name[32];            /* card node, for the log */
    char              driver[32];
    char              description[128];    /* the adapter, as Windows names it */
    uint16_t          vendor, device;      /* PCI ids, 0 if not on PCI */
    uint32_t          subsystem;
    uint8_t           revision;
    uint32_t          crtcs[32];
    uint32_t          crtc_count;
    struct kms_output outputs[KMS_MAX_OUTPUTS];
};

extern struct kms_card kms;

struct dwm_output_config;

int  kms_open(void);                  /* 0 on success */
bool kms_probe(void);                 /* rereads the connectors; true if monitors came or went */
bool kms_apply( const struct dwm_output_config *configs, uint32_t count, bool test );
void kms_flush( struct kms_output *output );  /* after drawing: shadow-buffered drivers copy only on this */
int  kms_hotplug_socket(void);        /* kernel uevents, -1 if none */
bool kms_hotplug_event( int fd );     /* reads one; true if it was a display hotplug */
struct kms_mode *kms_find_mode( struct kms_output *output, uint32_t width, uint32_t height, uint32_t refresh );

/* called by kms_probe for a monitor that was unplugged, before its slot is freed */
void compositor_output_removed( struct kms_output *output );

#endif
