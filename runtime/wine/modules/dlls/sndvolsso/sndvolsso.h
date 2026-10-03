/*
 * Volume icon of the notification area (sndvolsso.dll)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __SNDVOLSSO_H
#define __SNDVOLSSO_H

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"

#define IDS_TIP_LEVEL           100
#define IDS_TIP_MUTED           101
#define IDS_NO_DEVICE           102
#define IDS_OPEN_SETTINGS       103
#define IDS_OPEN_MIXER          104
#define IDS_SOUNDS              105
#define IDS_SPEAKERS            106
#define IDS_HEADPHONES          107
#define IDS_DIGITAL_OUTPUT      108
#define IDS_DIGITAL_DISPLAY     109
#define IDS_MUTE                110
#define IDS_UNMUTE              111

#define MAX_ENDPOINTS 16

/* the shell's colours in Windows 10, dark */
#define ACCENT_COLOR  RGB(0x00, 0x78, 0xd7)
#define ACRYLIC_TINT  0xcc1f1f1f   /* 0xAABBGGRR */

struct endpoint
{
    WCHAR id[128];
    WCHAR name[256];             /* as the flyout shows it: "Динаміки (Realtek ALC892)" */
};

extern HINSTANCE sndvolsso_instance;

WCHAR *load_string( UINT id );

/* audio.c: the default output and its volume, through the Core Audio API */
BOOL audio_init(void);
void audio_shutdown(void);
BOOL audio_state( struct endpoint *def, float *level, BOOL *mute );
UINT audio_endpoints( struct endpoint *list, UINT max );
void audio_set_level( float level );
void audio_set_mute( BOOL mute );
void audio_set_default( const WCHAR *id );

/* glyph.c: the speaker of Segoe MDL2 Assets, drawn */
int px( int n );
HFONT shell_font( int height, int weight );
void blend_white( HDC hdc, int x, int y, int width, int height, const float *alpha );
void draw_speaker( HDC hdc, int x, int y, int size, float level, BOOL mute, BYTE alpha, BYTE bars_alpha );
void draw_chevron( HDC hdc, int cx, int cy, BOOL up, BYTE alpha );
HICON make_speaker_icon( int size, float level, BOOL mute );
void fill_alpha( HDC hdc, const RECT *rect, COLORREF color, BYTE alpha );
void fill_round_rect( HDC hdc, const RECT *rect, float radius, COLORREF color, BYTE alpha );

/* media.c: what programs play (SystemMediaTransportControls) */
#define MEDIA_CARD_WIDTH  px(360)
#define MEDIA_CARD_HEIGHT px(120)

enum media_action { MEDIA_PREVIOUS, MEDIA_PLAYPAUSE, MEDIA_NEXT };

struct media_info
{
    BOOL   present;
    UINT32 id;
    HWND   notify, window;
    BOOL   playing;
    UINT32 buttons;
    WCHAR  app[128], title[128], artist[128];
    HICON  icon;
};

BOOL media_current( struct media_info *info );
BOOL media_session( struct media_info *info );
BOOL media_press( const struct media_info *info, int action );
int  media_hit( const RECT *card, POINT pt, const struct media_info *info );
void media_paint( HDC hdc, const RECT *card, const struct media_info *info, int hot, BOOL pressed, BYTE opacity );

/* osd.c: the overlay the volume and media keys bring up */
void osd_show(void);
void osd_hide(void);

/* flyout.c */
void flyout_toggle( HWND tray, UINT icon_id );
void flyout_hide(void);
BOOL flyout_visible(void);
void flyout_refresh(void);

/* tray.c */
void tray_start(void);
void tray_stop(void);
void tray_update(void);

#endif
