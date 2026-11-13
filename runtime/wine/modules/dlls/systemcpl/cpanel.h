/*
 * A page of the Control Panel, as Windows 7 draws them inside Explorer
 *
 * Shared by the modules whose Control Panel items are pages (systemcpl.dll,
 * netcenter.dll: PARENTSRC): the shell folder of the item and its pages
 * (cpfolder.c) and the shell view that lays a page out and draws it with the
 * Control Panel's look of Windows 7 (cpanel.c). A module gives its CLSID, its
 * pages and its instance.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __ARCTIC_CPANEL_H
#define __ARCTIC_CPANEL_H

#include <stdarg.h>
#include <stdlib.h>
#include <wchar.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "winreg.h"
#include "objbase.h"
#include "shlobj.h"

struct view;

typedef void (*link_proc)( struct view *view, UINT_PTR param );
typedef void (*paint_proc)( struct view *view, HDC hdc, const RECT *rect, UINT_PTR param );

/* a page of the module: the item's own (0) and the ones it browses into */
struct cp_page
{
    UINT name;                        /* string id: the address bar, the window's title */
    const WCHAR *canonical;           /* its parsing name, as Windows' pageXxx */
    void (*build)( struct view *view );
    BOOL (*command)( struct view *view, UINT id, UINT code, HWND control );
    void (*timer)( struct view *view );
};

/* what the module gives */
extern HINSTANCE cp_instance;
extern const CLSID cp_clsid;
extern const struct cp_page cp_pages[];
extern const UINT cp_page_count;
extern const WCHAR cp_class_name[];   /* a word for its window classes */

WCHAR *load_string( UINT id );
WCHAR *format_string( UINT id, ... );
int px( int n );

/* the shell folder (cpfolder.c) */
HRESULT cp_folder_create( UINT page, DWORD param, REFIID riid, void **out );
HRESULT cp_class_object( REFCLSID clsid, REFIID riid, void **out );
HRESULT cp_can_unload(void);
/* the item in a new Explorer window, on one of its pages */
BOOL cp_open( UINT page, DWORD param );

/* a page's contents, laid out top to bottom in the content pane */
enum
{
    STYLE_TITLE,      /* 16 px, the Control Panel's title blue */
    STYLE_BODY,       /* 12 px black */
    STYLE_BOLD,
    STYLE_GROUP,      /* a group header with a line after it */
    STYLE_SMALL,
    STYLE_GRAY,
    STYLE_BIG,
    STYLE_COUNT
};

/* the programs' mode (desk.cpl, Personalization) as the system colours
 * have it: a page is dark where windows are */
static inline BOOL cp_dark(void)
{
    COLORREF c = GetSysColor( COLOR_WINDOW );
    return GetRValue( c ) + GetGValue( c ) + GetBValue( c ) < 3 * 128;
}

#define COLOR_TITLE    (cp_dark() ? RGB( 0x99, 0xc9, 0xff ) : RGB( 0x00, 0x33, 0x99 ))  /* the main instruction's blue */
#define COLOR_GROUP    (cp_dark() ? RGB( 0xd8, 0xe2, 0xec ) : RGB( 0x1e, 0x39, 0x5b ))  /* a section's header (Windows 7) */
#define COLOR_TEXT     (cp_dark() ? RGB( 0xff, 0xff, 0xff ) : RGB( 0x00, 0x00, 0x00 ))
#define COLOR_GRAY     (cp_dark() ? RGB( 0xa0, 0xa0, 0xa0 ) : RGB( 0x6d, 0x6d, 0x6d ))
#define COLOR_LINK     (cp_dark() ? RGB( 0x99, 0xd6, 0xff ) : RGB( 0x00, 0x66, 0xcc ))
#define COLOR_LINK_HOT (cp_dark() ? RGB( 0xc4, 0xe8, 0xff ) : RGB( 0x33, 0x99, 0xff ))
#define COLOR_NAV_LINK (cp_dark() ? RGB( 0xd8, 0xe2, 0xec ) : RGB( 0x1e, 0x39, 0x5b ))  /* the task pane's links (Windows 7) */
#define COLOR_LINE     (cp_dark() ? RGB( 0x3c, 0x3c, 0x3c ) : RGB( 0xd5, 0xdf, 0xe5 ))
/* the page under it all */
#define COLOR_PAGE     (cp_dark() ? RGB( 0x19, 0x19, 0x19 ) : RGB( 0xff, 0xff, 0xff ))
/* Windows 11's Settings (view_heading): the page, a card on it, a card under
 * the pointer, a card's edge */
#define COLOR_PAGE11      (cp_dark() ? RGB( 0x20, 0x20, 0x20 ) : RGB( 0xf3, 0xf3, 0xf3 ))
#define COLOR_CARD        (cp_dark() ? RGB( 0x2b, 0x2b, 0x2b ) : RGB( 0xfb, 0xfb, 0xfb ))
#define COLOR_CARD_HOT    (cp_dark() ? RGB( 0x32, 0x32, 0x32 ) : RGB( 0xf6, 0xf6, 0xf6 ))
#define COLOR_CARD_BORDER (cp_dark() ? RGB( 0x1d, 0x1d, 0x1d ) : RGB( 0xe5, 0xe5, 0xe5 ))

void view_clear( struct view *view );
void view_text( struct view *view, int style, const WCHAR *text );
void view_text_at( struct view *view, int style, const WCHAR *text, int indent );
void view_link( struct view *view, const WCHAR *text, link_proc proc, UINT_PTR param, int indent );
/* a link with an icon before it (the shield), on the right edge of the page when right is set */
void view_icon_link( struct view *view, HICON icon, const WCHAR *text, link_proc proc, UINT_PTR param, int indent,
                     BOOL right );
void view_space( struct view *view, int height );
void view_group( struct view *view, const WCHAR *text );
HWND view_control( struct view *view, const WCHAR *cls, const WCHAR *text, DWORD style, int width, int height,
                   int indent, UINT id );
HWND view_control_beside( struct view *view, const WCHAR *cls, const WCHAR *text, DWORD style, int x, int width,
                          int height, UINT id );
void view_paint_area( struct view *view, int height, paint_proc proc, UINT_PTR param );
/* a link inside the last paint area, at x, y from its top left */
void view_link_in( struct view *view, HICON icon, const WCHAR *text, link_proc proc, UINT_PTR param, int x, int y );
void view_row_begin( struct view *view );
void view_row_end( struct view *view );
/* a label and its value side by side: Windows' rowlayout of the System page */
void view_pair( struct view *view, const WCHAR *label, const WCHAR *value, int indent, int label_width );
/* Windows 11's Settings: the page's title, large, on its grey page; a heading
 * over a few cards; a card with a large icon (the module's own, by its id:
 * the icon theme gives its picture), its name and a line under it, a control
 * at its right; a card that goes somewhere, with a chevron; a card with a
 * drawing of the page's own, and controls inside it at x, y; a switch */
void view_heading( struct view *view, const WCHAR *text );
void view_section( struct view *view, const WCHAR *text );
HICON view_icon( UINT id );
HWND view_card( struct view *view, HICON icon, const WCHAR *title, const WCHAR *text, const WCHAR *cls,
                const WCHAR *control_text, DWORD style, int width, int height, UINT id );
void view_card_link( struct view *view, HICON icon, const WCHAR *title, const WCHAR *text, link_proc proc, UINT_PTR param );
void view_card_area( struct view *view, int height, paint_proc proc, UINT_PTR param );
HWND view_card_control( struct view *view, const WCHAR *cls, const WCHAR *text, DWORD style, int x, int y, int width,
                        int height, UINT id );
/* one more control at the right of the last card, left of those it has */
HWND view_card_extra( struct view *view, const WCHAR *cls, const WCHAR *text, DWORD style, int width, int height,
                      UINT id );
/* a card whose controls (view_card_control, x and y unused) stand left to
 * right, gap apart, in as many rows as the width needs: pictures to choose */
void view_card_flow( struct view *view, int gap );
/* a brush of a colour, kept: for the module's own controls on a card */
HBRUSH view_brush( COLORREF color );
/* the accent colour, as the taskbar and Start have it (dwm's colorization) */
COLORREF view_accent(void);

/* A picture to choose, on a card: a bitmap (not the tile's: a cache keeps
 * it), an icon, a colour, with a drawing of the module's over it and a line
 * under it; the one chosen ringed in the accent's colour. It says BN_CLICKED,
 * Space and Enter as well. TM_SETBITMAP sets the bitmap from another thread
 * (wparam). */
typedef void (*tile_paint_proc)( HDC hdc, const RECT *rect, UINT_PTR param );
#define TM_SETBITMAP (WM_USER + 40)
HWND view_tile( struct view *view, int width, int height, const WCHAR *label, UINT id );
void tile_set_bitmap( HWND tile, HBITMAP bitmap );
void tile_set_icon( HWND tile, HICON icon, int size );
void tile_set_color( HWND tile, COLORREF color );
void tile_set_paint( HWND tile, tile_paint_proc proc, UINT_PTR param );
void tile_select( HWND tile, BOOL selected );
UINT_PTR tile_param( HWND tile );
/* "Увімкнуто" / "Вимкнуто" (IDS_CP_ON 0xf003, IDS_CP_OFF 0xf004) and the switch */
HWND view_card_toggle( struct view *view, HICON icon, const WCHAR *title, const WCHAR *text, BOOL on, UINT id );
/* in the page's command for a WM_NOTIFY: what it carries */
const NMHDR *view_notify( struct view *view );
/* a window made elsewhere, as one of the page's controls */
HWND view_embed( struct view *view, HWND hwnd, int width, int height, int indent );
/* A property page another module makes (desk.cpl, startui.dll, explorer):
 * on the page as in its own dialog, with all it does, its "Застосувати"
 * under it. Leaving the page without applying cancels it, as its dialog's
 * Cancel did. The module's string table has IDS_CP_APPLY (0xf002). */
HWND view_sheet( struct view *view, HPROPSHEETPAGE page );
#define IDS_CP_APPLY 0xf002
void view_nav_link( struct view *view, const WCHAR *text, link_proc proc, UINT_PTR param );
void view_nav_icon_link( struct view *view, HICON icon, const WCHAR *text, link_proc proc, UINT_PTR param );
void view_nav_see_also( struct view *view, const WCHAR *text, link_proc proc, UINT_PTR param );
/* the first link of every task pane */
void view_nav_home( struct view *view );
void view_layout( struct view *view );
void view_rebuild( struct view *view );
void view_navigate( struct view *view, UINT page, DWORD param );
void view_browse_control_panel( struct view *view );
/* another item of the Control Panel, or a page of it: "::{CLSID}", "::{CLSID}\pageName" */
void view_browse_item( struct view *view, const WCHAR *item );
void view_back( struct view *view );
void view_set_timer( struct view *view, UINT ms );
HWND view_window( struct view *view );
HFONT view_font( int style );
int view_content_width( struct view *view );

/* what a page keeps between its builds */
struct page_state
{
    UINT page;
    DWORD param;             /* which one: a connection, a device... */
    void *data;              /* the page's own, free()d with the view */
};
struct page_state *view_state( struct view *view );

/* helpers */
HICON cp_shield_icon(void);
void cp_run( const WCHAR *file, const WCHAR *args );     /* ShellExecute, shown */

#endif
