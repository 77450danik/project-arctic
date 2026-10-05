/*
 * Snipping Tool
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __SNIPPINGTOOL_H
#define __SNIPPINGTOOL_H

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include "resource.h"

/* BGRA, top row first, alpha not premultiplied: 255 where the screen was */
struct image
{
    LONG    ref;
    int     width, height;
    UINT32 *bits;
    BOOL    owned;  /* FALSE: the bits are a DIB section's */
};

struct image *image_create( int width, int height );
struct image *image_addref( struct image *image );
void image_release( struct image *image );
struct image *image_crop( const struct image *src, const RECT *rect );
void image_copy_rect( struct image *dst, const struct image *src, const RECT *rect );
void image_fill( struct image *dst, const RECT *rect, UINT32 color );
void image_blend_rect( struct image *dst, const RECT *rect, UINT32 color );
void image_rect( const struct image *image, RECT *rect );

#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))

struct pointf { float x, y; };

/* ink.c: lines with round ends, smoothed, in an image's pixels */
struct stroke
{
    LONG           ref;
    UINT32         color;
    float          radius;
    BOOL           highlighter;  /* multiplies, as a marker on paper */
    int            count, capacity;
    struct pointf *points;
};

struct stroke *stroke_create( UINT32 color, float radius, BOOL highlighter );
struct stroke *stroke_addref( struct stroke *stroke );
void stroke_release( struct stroke *stroke );
BOOL stroke_add( struct stroke *stroke, float x, float y );
struct stroke *stroke_moved( const struct stroke *stroke, float dx, float dy );
void stroke_bounds( const struct stroke *stroke, int from, RECT *rect );
BOOL stroke_hit( const struct stroke *stroke, float x, float y, float radius );
void stroke_render( struct image *dst, const struct stroke *stroke, const RECT *clip );

void ink_polyline( struct image *dst, const struct pointf *points, int count, BOOL closed, float radius,
                   UINT32 color, const RECT *clip );
void ink_round_rect( struct image *dst, const RECT *rect, float radius, UINT32 color, const RECT *clip );
void ink_scale( const struct image *src, struct image *dst, float scale, const RECT *dst_rect );

enum glyph
{
    GLYPH_RECTANGLE, GLYPH_WINDOW, GLYPH_FULL_SCREEN, GLYPH_FREEFORM, GLYPH_CLOSE,
    GLYPH_NEW, GLYPH_PEN, GLYPH_HIGHLIGHTER, GLYPH_ERASER, GLYPH_CROP,
    GLYPH_UNDO, GLYPH_REDO, GLYPH_COPY, GLYPH_SAVE, GLYPH_FOLDER,
};

void ink_glyph( struct image *dst, enum glyph glyph, const RECT *rect, int size, UINT32 color, const RECT *clip );

/* capture.c */
struct image *capture_screen( RECT *screen );
int list_windows( RECT *rects, int max );
BOOL clipboard_set_image( HWND owner, const struct image *image );
HRESULT save_image( const WCHAR *path, const struct image *image );
struct image *load_image( const WCHAR *path );
BOOL screenshots_folder( WCHAR *path, DWORD size );
BOOL autosave_path( WCHAR *path, DWORD size );

/* overlay.c: Win+Shift+S; NULL when the user changed their mind */
struct image *snip_screen(void);

/* editor.c */
HWND editor_open( struct image *image, const WCHAR *path );

/* main.c */
extern HINSTANCE instance;

struct theme
{
    BOOL   dark;
    UINT32 back, surface, text, subtext, border, hover, pressed, accent, on_accent;
};

void get_theme( struct theme *theme );
const WCHAR *string( UINT id );
HFONT ui_font( UINT dpi, int height, int weight );
DWORD setting( const WCHAR *name, DWORD def );
void set_setting( const WCHAR *name, DWORD value );
void editor_opened(void);
void editor_closed(void);

static inline COLORREF to_colorref( UINT32 color )
{
    return RGB( (color >> 16) & 0xff, (color >> 8) & 0xff, color & 0xff );
}

#endif
