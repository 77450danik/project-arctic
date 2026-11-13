/*
 * Photos, as Windows 11 has it
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __ARCTIC_PHOTOS_H
#define __ARCTIC_PHOTOS_H

#define COBJMACROS
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <windows.h>
#include <ole2.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <dwmapi.h>

#include "resource.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#define GET_X(lp) ((int)(short)LOWORD(lp))
#define GET_Y(lp) ((int)(short)HIWORD(lp))

extern HINSTANCE instance;
extern HWND frame;
extern UINT dpi;

static inline int px( int n ) { return MulDiv( n, dpi, 96 ); }

const WCHAR *tr( const WCHAR *en, const WCHAR *uk );
DWORD setting( const WCHAR *name, DWORD def );
void set_setting( const WCHAR *name, DWORD value );

/**********************************************************************
 *          draw.c (the same as Paint's)
 */

struct theme
{
    BOOL dark;
    COLORREF chrome, bar, bar_line;
    COLORREF text, dim, faint;
    COLORREF hover, pressed, checked, checked_line;
    COLORREF accent, on_accent;
    COLORREF field, field_line;
    COLORREF workspace;
    COLORREF popup, popup_line;
};

extern struct theme th;
void theme_load(void);

struct surface
{
    HDC      dc;
    HBITMAP  bitmap;
    HGDIOBJ  old;
    UINT32  *bits;
    int      width, height;
};

BOOL surface_size( struct surface *s, int width, int height );
void surface_free( struct surface *s );
void fill( struct surface *s, const RECT *rect, COLORREF color );
void fill_round( struct surface *s, const RECT *rect, float radius, COLORREF color, BYTE alpha );
void outline_round( struct surface *s, const RECT *rect, float radius, COLORREF color, BYTE alpha );
void fill_circle( struct surface *s, float cx, float cy, float r, COLORREF color, BYTE alpha );
void ring( struct surface *s, float cx, float cy, float r, float width, COLORREF color, BYTE alpha );
void shadow_rect( struct surface *s, const RECT *rect, int spread, BYTE alpha );
void color_wheel( struct surface *s, float cx, float cy, float r );

#define BLIT_SMOOTH 1
#define BLIT_LAYER  2
void blit_image( struct surface *s, const RECT *clip, const UINT32 *bits, int width, int height,
                 double ox, double oy, double scale, UINT flags );
void blit_forget( const UINT32 *bits );

enum icon_state { IS_NORMAL, IS_DISABLED, IS_WHITE, IS_ACCENT };
BOOL icon_exists( const char *name );
void draw_icon( struct surface *s, const char *name, int x, int y, int size, enum icon_state state );
HBITMAP icon_bitmap( const char *name, int size );

enum glyph
{
    G_DOWN, G_UP, G_LEFT, G_RIGHT, G_CHECK, G_MIN, G_MAX, G_RESTORE, G_CLOSE, G_PLUS, G_MINUS, G_MORE,
    G_DOWN_SMALL,
};
void draw_glyph( struct surface *s, enum glyph glyph, int x, int y, int size, COLORREF color );

enum font_kind { F_UI, F_UI_SEMI, F_SMALL, F_TITLE, F_COUNT };
HFONT get_font( enum font_kind kind );
void fonts_reset(void);
const WCHAR *ui_face(void);
void draw_text( struct surface *s, const WCHAR *text, int len, const RECT *rect, HFONT font, COLORREF color, UINT format );
int text_width( HFONT font, const WCHAR *text, int len );
COLORREF mix( COLORREF a, COLORREF b, int alpha );

struct popup
{
    HWND hwnd;
    struct surface surf;
    int width, height;
    int hot, result;
    BOOL done;
    void (*paint)( struct popup *p );
    int (*hit)( struct popup *p, POINT pt );
    void *data;
};
int run_popup( HWND owner, const RECT *anchor, struct popup *p );

struct menu_item
{
    int id;
    const WCHAR *text;
    const char *icon;
    const WCHAR *keys;
    UINT flags;
    struct menu_item *sub;
    int sub_count;
};
int menu_run( HWND owner, const RECT *anchor, const struct menu_item *items, int count );

/**********************************************************************
 *          loader.c: pictures, their folder, their small copies
 */

struct picture
{
    int width, height;          /* as shown, after the camera's turn */
    UINT32 *bits;               /* straight alpha, top row first */
    int frames;                 /* an animation's, 1 for a still */
    UINT32 **frame_bits;        /* each frame composed; [0] is bits */
    UINT *delays;               /* milliseconds */
    GUID format;
    int orientation;            /* EXIF 1..8 as the file has it */
    WCHAR taken[64];            /* the date the camera gives, as text */
    WCHAR camera[128];
};

struct picture *picture_load( const WCHAR *path );
void picture_free( struct picture *p );
BOOL picture_save_rotated( const WCHAR *path, const struct picture *p );

/* the pictures of a folder in the order File Explorer shows them */
struct folder
{
    WCHAR dir[MAX_PATH];
    WCHAR **names;
    int count;
};
void folder_load( struct folder *f, const WCHAR *path, int *index );
void folder_free( struct folder *f );
BOOL is_picture_name( const WCHAR *name );

/* small copies for the strip, made by a thread of their own */
#define WM_APP_THUMB (WM_APP + 1)
void thumbs_start( HWND notify );
void thumbs_reset( const struct folder *f );
void thumbs_want( int index );
BOOL thumb_get( int index, UINT32 **bits, int *width, int *height );   /* NULL bits: not yet */

#endif
