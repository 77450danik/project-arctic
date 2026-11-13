/*
 * Arctic WordPad
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __ARCTIC_WORDPAD_H
#define __ARCTIC_WORDPAD_H

#define COBJMACROS
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <windows.h>
#include <ole2.h>
#include <commctrl.h>
#include <commdlg.h>
#include <richedit.h>
#include <shellapi.h>

#include "resource.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#define GET_X(lp) ((int)(short)LOWORD(lp))
#define GET_Y(lp) ((int)(short)HIWORD(lp))

/* riched20 of Arctic: the blocks a page may break between (Wine patch
 * "riched20: what a word processor needs"), in the text's own pixels from
 * its top. wParam: room in the array, lParam: the array or NULL; returns how
 * many there are. A riched20 without it returns 0. */
#define EM_ARCTIC_GETLAYOUT  (WM_USER + 0x1a0)
struct arctic_layout_block
{
    LONG  top, bottom;
    DWORD flags;
    LONG  cp;           /* the block's first character */
};
#define ALB_PAGEBREAK   0x1     /* a page begins with it */
#define ALB_KEEPNEXT    0x2     /* on the page of the next */
#define ALB_TABLE       0x4     /* a row of a table */

/**********************************************************************
 *          main.c
 */

extern HINSTANCE instance;
extern HWND frame;
extern UINT dpi;

static inline int px( int n ) { return MulDiv( n, dpi, 96 ); }

const WCHAR *string( UINT id );
DWORD setting( const WCHAR *name, DWORD def );
void set_setting( const WCHAR *name, DWORD value );
void command( int id );
void command_arg( int id, LPARAM arg );
BOOL command_enabled( int id );
BOOL command_checked( int id );
void set_modified( BOOL modified );
BOOL is_modified(void);
const WCHAR *document_name(void);
void layout_frame(void);
void open_file( const WCHAR *path );
BOOL save_file( const WCHAR *path, int format );
BOOL save_document( BOOL ask_name );
BOOL ask_save_changes(void);
void new_document(void);
void frame_title_changed(void);
const WCHAR *current_path(void);
void format_painter_mouse_up(void);

/* backstage.c: the File page */
void show_backstage( BOOL show, int page );
BOOL backstage_shown(void);
void backstage_place( int x, int y, int width, int height );
void backstage_theme_changed(void);

#define RECENT_MAX 12
int recent_files( WCHAR paths[RECENT_MAX][MAX_PATH] );
void recent_add( const WCHAR *path );

enum format { FMT_DOCX, FMT_RTF, FMT_TXT, FMT_ODT };

/**********************************************************************
 *          draw.c
 */

struct theme
{
    BOOL dark;
    COLORREF chrome;        /* the title bar, the tabs, round the pages */
    COLORREF card;          /* the ribbon */
    COLORREF card_line;
    COLORREF text, dim, faint;
    COLORREF hover, pressed, checked, checked_line;
    COLORREF accent, on_accent;
    COLORREF field, field_line;
    COLORREF canvas, page_line;
    COLORREF popup, popup_line;
    COLORREF sel_back;      /* selected text, as Word's grey */
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
void shadow_rect( struct surface *s, const RECT *rect, int spread, BYTE alpha );

enum icon_state { IS_NORMAL, IS_DISABLED, IS_WHITE };
BOOL icon_exists( const char *name );
void draw_icon( struct surface *s, const char *name, int x, int y, int size, enum icon_state state );

enum glyph
{
    G_DOWN, G_UP, G_LEFT, G_RIGHT, G_CHECK, G_LAUNCHER, G_MIN, G_MAX, G_RESTORE, G_CLOSE, G_PLUS, G_MINUS,
    G_SEARCH, G_MORE, G_DOWN_SMALL,
};
void draw_glyph( struct surface *s, enum glyph glyph, int x, int y, int size, COLORREF color );

enum font_kind
{
    F_UI, F_UI_SEMI, F_SMALL, F_TAB, F_TITLE, F_HEAD, F_SUBHEAD, F_LETTER, F_LETTER_BOLD, F_LETTER_ITALIC,
    F_LETTER_UNDER, F_LETTER_STRIKE, F_LETTER_SMALL, F_COUNT
};
HFONT get_font( enum font_kind kind );
void fonts_reset(void);
const WCHAR *ui_face(void);
void draw_text( struct surface *s, const WCHAR *text, int len, const RECT *rect, HFONT font, COLORREF color, UINT format );
int text_width( HFONT font, const WCHAR *text, int len );
COLORREF mix( COLORREF a, COLORREF b, int alpha );

/**********************************************************************
 *          view.c: the pages and the text on them
 */

struct page_setup
{
    int width, height;                  /* twips */
    int left, top, right, bottom;
};

extern struct page_setup page;
extern BOOL web_layout;

LRESULT ed( UINT msg, WPARAM wparam, LPARAM lparam );
HWND view_create( HWND parent );
HWND status_create( HWND parent );
void view_reset_text(void);
void view_set_page( const struct page_setup *setup );
void view_set_zoom( int percent );
int view_zoom(void);
int view_fit_zoom( BOOL whole_page );
void view_set_web( BOOL web );
void view_repaginate(void);
int view_page_count(void);
int view_current_page(void);
void view_focus(void);
void view_scroll_caret(void);
void view_theme_changed(void);
void status_update(void);
int word_count(void);
BOOL stream_in( const char *data, size_t len, UINT format );
char *stream_out( size_t *len, UINT format );
void view_freeze( BOOL freeze );

/* the printer: print.c */
int view_print( HDC dc, int first, int last, BOOL (*abort_proc)(void) );
void print_document( HWND owner, BOOL ask );
void page_setup_dialog( HWND owner );

/**********************************************************************
 *          ribbon.c
 */

HWND ribbon_create( HWND parent );
int ribbon_height(void);
void ribbon_refresh(void);
void ribbon_theme_changed(void);
void ribbon_set_tab( int tab );
void ribbon_font_changed( const WCHAR *face, int size_half_points );
COLORREF ribbon_color( int cmd );
void ribbon_set_color( int cmd, COLORREF color );

/* popups.c */
COLORREF pick_color( HWND owner, const RECT *anchor, COLORREF current, BOOL highlight, BOOL *automatic );
int pick_table( HWND owner, const RECT *anchor );                 /* MAKELONG( columns, rows ), 0 */
WCHAR pick_symbol( HWND owner, const RECT *anchor );
BOOL pick_font( HWND owner, const RECT *anchor, WCHAR *face, int len );
int menu_popup( HWND owner, const RECT *anchor, const int *ids, const char *const *icons, int count, int checked );

/* styles.c: the paragraph styles of the gallery, as DOCX names them */
struct style
{
    const WCHAR *docx_id;           /* styleId in styles.xml */
    UINT  name;                     /* the string */
    const WCHAR *face;              /* NULL: the document's */
    int   size;                     /* half points, 0: the document's */
    BOOL  bold, italic;
    COLORREF color;                 /* CLR_DEFAULT: automatic */
    int   space_before, space_after;   /* twips */
    int   line;                     /* 240ths of a line, 0: single */
    int   outline;                  /* heading level, 0: body text */
    int   left_indent;
};
#define STYLE_COUNT 9
extern const struct style styles[STYLE_COUNT];
extern WCHAR body_face[LF_FACESIZE], head_face[LF_FACESIZE];
void apply_style( int index );
int current_style(void);
void apply_normal_defaults(void);

/* dialogs.c */
void dialog_paragraph( HWND owner );
void dialog_font( HWND owner );
void dialog_link( HWND owner );
void dialog_datetime( HWND owner );
void dialog_find( HWND owner, BOOL replace );
BOOL is_find_dialog_message( MSG *msg );
void dialog_zoom( HWND owner );
void dialog_about( HWND owner );
void dialog_wordcount( HWND owner );

/* insert.c */
void insert_table( int columns, int rows );
void insert_picture( HWND owner );
void insert_page_break(void);
void insert_text( const WCHAR *text );
void change_case( int mode );

/**********************************************************************
 *          Files: docx.c, zip.c, xml.c, image.c, odt (docx.c)
 */

struct buffer
{
    char   *data;
    size_t  len, size;
};

void buf_add( struct buffer *b, const void *data, size_t len );
void buf_str( struct buffer *b, const char *s );
void buf_printf( struct buffer *b, const char *format, ... );
void buf_wstr_utf8( struct buffer *b, const WCHAR *s, int len );
void buf_free( struct buffer *b );

/* what of a DOCX this program does not show, kept for its saving */
struct docx_keep;
void docx_keep_free( struct docx_keep *keep );

char *docx_to_rtf( const BYTE *data, size_t size, size_t *rtf_len, struct page_setup *setup, struct docx_keep **keep );
BOOL rtf_to_docx( const char *rtf, size_t len, const struct page_setup *setup, const struct docx_keep *keep,
                  BYTE **out, size_t *out_len );
char *odt_to_rtf( const BYTE *data, size_t size, size_t *rtf_len );

struct zip;
struct zip *zip_open( const BYTE *data, size_t size );
void zip_close( struct zip *zip );
BYTE *zip_read( struct zip *zip, const char *name, size_t *len );
int zip_count( struct zip *zip );
const char *zip_name( struct zip *zip, int index );

struct zip_writer;
struct zip_writer *zipw_create(void);
void zipw_add( struct zip_writer *z, const char *name, const void *data, size_t len, BOOL compress );
BYTE *zipw_finish( struct zip_writer *z, size_t *len );

/* xml.c: a reader of what Word writes */
enum xml_token { XML_END, XML_OPEN, XML_CLOSE, XML_TEXT };
struct xml
{
    const char *p, *end;
    char  name[64];              /* of the element, without its prefix when it is w: */
    char  qname[64];             /* as written */
    const char *attrs, *attrs_end;
    BOOL  empty;                 /* <a/>: an XML_CLOSE follows */
    BOOL  pending_close;
    struct buffer text;          /* XML_TEXT, decoded */
    int   depth;
};
void xml_init( struct xml *x, const char *data, size_t len );
enum xml_token xml_next( struct xml *x );
BOOL xml_attr( struct xml *x, const char *name, char *value, int size );
int xml_attr_int( struct xml *x, const char *name, int def );
BOOL xml_attr_bool( struct xml *x, const char *name, BOOL def );
BOOL xml_enter( struct xml *x );
void xml_skip( struct xml *x );   /* past the end of the element just opened */
void xml_free( struct xml *x );
BOOL xml_is( struct xml *x, const char *qname );

BYTE *image_to_dib( const BYTE *data, size_t len, int max_w, int max_h, int *width, int *height, size_t *dib_len );
BYTE *image_from_metafile( const BYTE *emf, size_t len, BOOL enhanced, int mm, int width, int height, size_t *png_len );
BOOL metafile_size( const BYTE *bits, size_t len, int *width, int *height );
BOOL picture_rtf( struct buffer *out, const BYTE *data, size_t len, int goal_w, int goal_h );

void hex_bytes( struct buffer *b, const BYTE *data, size_t len );
void trace( const char *format, ... );
void link_register( const WCHAR *text, const char *url );

#endif
