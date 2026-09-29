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

struct wl_display;
struct wl_resource;

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
    bool             vrr_capable;          /* the monitor and the link take a variable refresh */

    /* what it shows */
    bool             enabled;
    uint32_t         crtc_id;              /* when off: the one firmware used, 0 if none */
    uint32_t         mode;                 /* index into modes */
    int32_t          x, y;                 /* in the virtual screen */
    struct kms_fb    fbs[2];               /* composed frames: one on screen while the other is drawn */

    /* scanout, when the monitor is on */
    uint32_t         front_fb;             /* the framebuffer on screen */
    uint32_t         queued_fb;            /* a flip to it is pending, 0 if none */
    void            *front_buffer;         /* the client buffers they are, NULL for our own frames */
    void            *queued_buffer;
    bool             no_flip;              /* the driver cannot flip: frames are drawn where they show */
    bool             fake_vblank;          /* flips complete at once: frames are paced by a timer */
    uint32_t         short_flips;          /* flips in a row that came too soon for a vertical blank */
    uint64_t         submit_time;          /* µs, CLOCK_MONOTONIC */
    uint64_t         done_time;
    uint32_t         plane;                /* primary plane (atomic) */
    uint32_t         vrr_prop;             /* the CRTC's VRR_ENABLED, 0 if it has none */
    bool             vrr_on;               /* what the CRTC runs with now */
    bool             vrr_refused;          /* the driver would not switch it without a modeset */
    bool             cursor_on;            /* the hardware cursor is shown on this CRTC */

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
    uint64_t          devnum;              /* dev_t of the card node */
    bool              atomic;              /* atomic modesetting: page flips of any buffer, VRR */
    uint32_t          generation;          /* counts the cards dwm has driven: framebuffers of an
                                            * earlier one mean nothing to this one */
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
enum kms_event
{
    KMS_EVENT_NONE,
    KMS_EVENT_MONITORS,               /* monitors of our card may have come or gone */
    KMS_EVENT_CARD_GONE,              /* our card is no more */
    KMS_EVENT_CARD_ADDED,             /* a card came */
};
enum kms_event kms_hotplug_event( int fd );  /* reads one */
void kms_close(void);
bool kms_reopen(void);                /* the best card there is now, once */
struct kms_mode *kms_find_mode( struct kms_output *output, uint32_t width, uint32_t height, uint32_t refresh );

/* Frames. kms_present queues a flip to fb, showing src_width x src_height of
 * it over the whole mode, with variable refresh on or off; buffer is what the
 * compositor gets back once it is off screen. A composed frame falls back to
 * a legacy flip; a client's buffer is composed instead. It returns false if
 * the flip cannot be made; output->no_flip then says whether it never will. */
bool kms_present( struct kms_output *output, uint32_t fb, uint32_t src_width, uint32_t src_height,
                  bool vrr, void *buffer, bool composed );
struct kms_fb *kms_back_buffer( struct kms_output *output );   /* NULL if both are busy */
struct kms_fb *kms_front_buffer( struct kms_output *output );
bool kms_can_scanout( struct kms_output *output, uint32_t fb, uint32_t src_width, uint32_t src_height );
void kms_dispatch(void);              /* reads the card's events: finished flips */
uint64_t kms_now(void);               /* µs */
uint32_t kms_frame_time( const struct kms_output *output );  /* µs one refresh takes */

/* a client's buffer as a framebuffer, 0 if the card cannot take it */
uint32_t kms_add_dmabuf( int fd, uint32_t width, uint32_t height, uint32_t format, uint32_t offset,
                         uint32_t stride, uint64_t modifier );
void kms_remove_dmabuf( uint32_t fb );

/* the hardware cursor, for monitors that show a client buffer directly */
bool kms_set_cursor_image( const uint32_t *argb, uint32_t width, uint32_t height );
bool kms_show_cursor( struct kms_output *output, bool show, int32_t x, int32_t y );  /* false if it cannot */

/* called by kms_probe for a monitor that was unplugged, before its slot is freed */
void compositor_output_removed( struct kms_output *output );
/* called by kms_dispatch when a flip finished; buffer is what went off screen */
void compositor_flip_done( struct kms_output *output, void *buffer );
/* a buffer given to kms_present that will not be shown after all */
void compositor_buffer_unused( void *buffer );

/* Client GPU buffers (zwp_linux_dmabuf_v1). A buffer lives on while the
 * compositor uses it, after the client destroyed it. */
struct dmabuf
{
    struct wl_resource *resource;          /* the wl_buffer, NULL once destroyed */
    uint32_t            refs;              /* the compositor's uses: a surface, a monitor */
    int                 fd;
    uint32_t            width, height, format, offset, stride;
    uint64_t            modifier;
    bool                alpha;
    uint32_t            fb;                /* as a framebuffer, 0 until needed */
    uint32_t            fb_generation;     /* of the card it was made on */
    bool                fb_failed;         /* the card cannot take it */
    uint32_t            scanout_tested;    /* CRTC it was tried on, 0 if none */
    bool                scanout_ok;
    void               *map;               /* for composition on the CPU */
    size_t              map_size;
    bool                map_failed;
    bool                gpu_failed;        /* the GPU could not read it back */
    /* composed on the GPU: the buffer as a texture, without a copy */
    void               *image;             /* EGLImage */
    uint32_t            texture;
    uint32_t            texture_generation;  /* of the GL context */
    bool                texture_external;  /* GL_TEXTURE_EXTERNAL_OES */
    bool                texture_failed;
};

void dmabuf_init( struct wl_display *display );
struct dmabuf *dmabuf_from_resource( struct wl_resource *buffer );
void dmabuf_ref( struct dmabuf *buffer );
void dmabuf_unref( struct dmabuf *buffer );  /* the client gets its buffer back at the last one */
bool dmabuf_read( struct dmabuf *buffer, uint32_t *pixels );  /* width x height, packed */
uint32_t dmabuf_fb( struct dmabuf *buffer );  /* 0 if the card cannot scan it out */

/* gl.c */
bool gpu_read_dmabuf( struct dmabuf *buffer, uint32_t *pixels );
void gl_dmabuf_gone( struct dmabuf *buffer );

/* a framebuffer of a buffer object of ours (GBM), 0 if the card cannot take it */
uint32_t kms_add_fb( uint32_t width, uint32_t height, uint32_t format, uint32_t planes, const uint32_t *handles,
                     const uint32_t *pitches, const uint32_t *offsets, uint64_t modifier );
void kms_remove_fb( uint32_t fb );

#endif
