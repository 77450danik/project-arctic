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
void draw_speaker( HDC hdc, int x, int y, int size, float level, BOOL mute, BYTE alpha, BYTE bars_alpha );
void draw_chevron( HDC hdc, int cx, int cy, BOOL up, BYTE alpha );
HICON make_speaker_icon( int size, float level, BOOL mute );
void fill_alpha( HDC hdc, const RECT *rect, COLORREF color, BYTE alpha );
void fill_round_rect( HDC hdc, const RECT *rect, float radius, COLORREF color, BYTE alpha );

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
