/*
 * Paint, as Windows 11 has it
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __ARCTIC_PAINT_H
#define __ARCTIC_PAINT_H

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
#include <dwmapi.h>

#include "resource.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#define GET_X(lp) ((int)(short)LOWORD(lp))
#define GET_Y(lp) ((int)(short)HIWORD(lp))

/**********************************************************************
 *          main.c
 */

extern HINSTANCE instance;
extern HWND frame;
extern UINT dpi;

static inline int px( int n ) { return MulDiv( n, dpi, 96 ); }

/* the text in the language of the user: Ukrainian or English */
const WCHAR *tr( const WCHAR *en, const WCHAR *uk );
DWORD setting( const WCHAR *name, DWORD def );
void set_setting( const WCHAR *name, DWORD value );
void command( int id );
BOOL command_enabled( int id );
BOOL command_checked( int id );
void set_modified( BOOL modified );
void layout_frame(void);
void frame_title_changed(void);
void open_file( const WCHAR *path );

/**********************************************************************
 *          draw.c
 */

struct theme
{
    BOOL dark;
    COLORREF chrome;        /* the title bar */
    COLORREF bar;           /* the tools */
    COLORREF bar_line;
    COLORREF text, dim, faint;
    COLORREF hover, pressed, checked, checked_line;
    COLORREF accent, on_accent;
    COLORREF field, field_line;
    COLORREF workspace;     /* round the picture */
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

/* a picture's pixels into a rectangle of a surface: zoomed, smooth when it
 * gets smaller, as squares when it gets bigger */
#define BLIT_SMOOTH 1      /* smooth when it gets bigger too */
#define BLIT_LAYER  2      /* over what is drawn, not over a chequer */
void blit_image( struct surface *s, const RECT *clip, const UINT32 *bits, int width, int height,
                 double ox, double oy, double scale, UINT flags );
void blit_forget( const UINT32 *bits );     /* its pixels changed; NULL: all */

/* popups of our own (draw.c) */
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

/* a menu of the system with pictures: ids 0 are lines, -1 opens the
 * submenu given in subs */
struct menu_item
{
    int id;
    const WCHAR *text;
    const char *icon;
    const WCHAR *keys;
    UINT flags;             /* MFS_CHECKED, MFS_DISABLED, MFT_RADIOCHECK */
    struct menu_item *sub;
    int sub_count;
};
int menu_run( HWND owner, const RECT *anchor, const struct menu_item *items, int count );

/**********************************************************************
 *          image.c: the picture, its history and its files
 */

struct image
{
    int width, height;
    UINT32 *bits;           /* 0xAARRGGBB, top row first */
};

struct image *image_create( int width, int height, UINT32 fill_pixel );
struct image *image_copy( const struct image *src );
void image_free( struct image *image );
struct image *image_load( const WCHAR *path, GUID *format );
BOOL image_opaque( const struct image *image );
void image_flatten( struct image *image, UINT32 back );   /* see-through onto back */
BOOL image_save( const struct image *image, const WCHAR *path, const GUID *format );
BOOL clipboard_put( HWND owner, const struct image *image );
struct image *clipboard_get( HWND owner );
BOOL clipboard_has_image(void);
HBITMAP image_dib( const struct image *image, UINT32 **bits );

extern struct image *pic;           /* the picture */
void history_push(void);            /* before a change */
void history_clear(void);
BOOL can_undo(void);
BOOL can_redo(void);
void undo(void);
void redo(void);
void set_picture( struct image *image );

/**********************************************************************
 *          ribbon.c: the tools under the title bar, the status bar
 */

enum tool
{
    TOOL_SELECT, TOOL_PENCIL, TOOL_FILL, TOOL_TEXT, TOOL_ERASER, TOOL_PICKER, TOOL_MAGNIFIER,
    TOOL_BRUSH, TOOL_SHAPE, TOOL_COUNT
};
enum brush_kind
{
    BR_BRUSH, BR_CALLIGRAPHY, BR_PEN, BR_AIRBRUSH, BR_OIL, BR_CRAYON, BR_MARKER, BR_NATURAL, BR_WATERCOLOR,
    BR_COUNT
};
enum shape_kind
{
    SH_LINE, SH_CURVE, SH_OVAL, SH_RECT, SH_ROUNDRECT, SH_POLYGON, SH_TRIANGLE, SH_RTRIANGLE, SH_DIAMOND,
    SH_PENTAGON, SH_HEXAGON, SH_ARROW_RIGHT, SH_ARROW_LEFT, SH_ARROW_UP, SH_ARROW_DOWN, SH_STAR4, SH_STAR5,
    SH_STAR6, SH_CALLOUT_RECT, SH_CALLOUT_OVAL, SH_CALLOUT_CLOUD, SH_HEART, SH_LIGHTNING, SH_COUNT
};
enum select_kind { SEL_RECT, SEL_FREE };
enum style_kind { ST_NONE, ST_SOLID };

struct tools
{
    enum tool tool, before_picker;
    enum brush_kind brush;
    enum shape_kind shape;
    enum select_kind select;
    BOOL transparent_select;
    enum style_kind outline, fill;
    int size[TOOL_COUNT];          /* the thickness per tool, in pixels */
    COLORREF color1, color2;
    COLORREF custom[10];
    int custom_count;
};
extern struct tools tools;
extern const char *const brush_icons[BR_COUNT];
extern const char *const shape_icons[SH_COUNT];
const WCHAR *brush_name( int kind );
const WCHAR *shape_name( int kind );

HWND ribbon_create( HWND parent );
int ribbon_height(void);
void ribbon_refresh(void);
void ribbon_theme_changed(void);
void set_tool( enum tool tool );

HWND status_create( HWND parent );
void status_refresh(void);
void status_theme_changed(void);

HWND textbar_create( HWND parent );
void textbar_show( BOOL show );
BOOL textbar_shown(void);
void textbar_theme_changed(void);
extern struct text_style
{
    WCHAR face[LF_FACESIZE];
    int size;               /* points */
    BOOL bold, italic, underline, strike, opaque;
} text_style;

/**********************************************************************
 *          canvas.c: the picture on the screen and what the mouse does to it
 */

HWND canvas_create( HWND parent );
void canvas_refresh(void);
void canvas_theme_changed(void);
void canvas_set_zoom( double zoom, const POINT *around );
double canvas_zoom(void);
void canvas_fit(void);
void canvas_picture_changed(void);   /* a new size or a new picture */
BOOL canvas_has_selection(void);
void canvas_commit(void);            /* what floats goes into the picture */
void canvas_cancel(void);
void canvas_select_all(void);
void canvas_invert_selection(void);
void canvas_delete_selection(void);
void canvas_crop(void);
void canvas_copy( BOOL cut );
void canvas_paste( struct image *image );
void canvas_rotate( int how );       /* IDM_ROTATE_* */
void canvas_style_changed(void);     /* colours, size, the text's style */
void canvas_get_status( POINT *cursor, BOOL *in, SIZE *selection );
void canvas_resize_skew( int width, int height, int skew_h, int skew_v );
void canvas_selection_size( int *width, int *height );
BOOL canvas_text_active(void);
void canvas_text_paste(void);
void canvas_zoom_step( int dir, const POINT *around );
extern BOOL show_grid;

/* paintops.c: what draws into a picture */
void op_line( struct image *im, int x0, int y0, int x1, int y1, COLORREF color, int width, BOOL square );
void op_brush_segment( struct image *im, enum brush_kind kind, double x0, double y0, double x1, double y1,
                       COLORREF color, int size, unsigned int *seed );
void op_flood_fill( struct image *im, int x, int y, COLORREF color );
void op_shape( struct image *im, enum shape_kind kind, const POINT *pts, int count, const RECT *box,
               COLORREF outline_color, COLORREF fill_color, int width, BOOL outline, BOOL filled );
void op_erase( struct image *im, int x0, int y0, int x1, int y1, int size );
UINT32 to_pix( COLORREF c );
COLORREF from_pix( UINT32 p );

/* dialogs.c */
void dialog_resize( HWND owner );
void dialog_properties( HWND owner );

#endif
