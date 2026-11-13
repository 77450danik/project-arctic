/*
 * The parts of the file dialog around the browser, as Windows 10 and 11
 * have them: the buttons, the address bar of folders, the search box
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_ITEMDLG_CHROME_H
#define __WINE_ITEMDLG_CHROME_H

#include "shlobj.h"

/* WM_COMMAND codes the parts send their parent */
#define CHROME_NAVIGATE     0x1001  /* the address bar: go to chrome_address_target() */
#define CHROME_REFRESH      0x1002  /* the address bar's refresh button */
#define CHROME_DROPDOWN     0x1003  /* a button's arrow, or a button that is all menu */

enum chrome_glyph
{
    GLYPH_BACK, GLYPH_FORWARD, GLYPH_UP, GLYPH_REFRESH, GLYPH_SEARCH, GLYPH_RIGHT, GLYPH_DOWN, GLYPH_MORE,
    GLYPH_DETAILS, GLYPH_LIST, GLYPH_ICONS, GLYPH_TILES, GLYPH_COUNT,
    GLYPH_NONE = -1
};

/* the colours and the shape of the look: Windows 11's when the visual style
 * has its size, else Windows 10's; dark when the windows are */
struct chrome_look
{
    BOOL dark, win11;
    UINT dpi;
    COLORREF top;       /* behind the rows over the browser */
    COLORREF band;      /* behind the names and the buttons at the bottom */
    COLORREF field;     /* the address bar and the search box */
    COLORREF border;    /* their edge */
    COLORREF focus;     /* their edge while typed in */
    COLORREF hover, down, hover_edge;
    COLORREF text, dim, line;
};

void chrome_get_look( struct chrome_look *look, HWND hwnd );
int chrome_scale( UINT dpi, int px );
HFONT chrome_font( UINT dpi );
void chrome_draw_glyph( HDC hdc, int glyph, int x, int y, int size, COLORREF color );
void chrome_fill_round( HDC hdc, const RECT *rect, int radius, COLORREF fill, COLORREF edge );
void chrome_register(void);

/* a row of flat buttons: glyphs, text or both */
#define CHROME_BUTTON_MENU      0x1     /* the button opens a menu: an arrow after the text */
#define CHROME_BUTTON_SPLIT     0x2     /* the button, and apart from it an arrow with a menu */

HWND chrome_create_buttons( HWND parent, UINT id );
void chrome_buttons_add( HWND hwnd, UINT id, int glyph, const WCHAR *text, const WCHAR *tip, UINT flags );
void chrome_buttons_enable( HWND hwnd, UINT id, BOOL enable );
void chrome_buttons_show( HWND hwnd, UINT id, BOOL show );
void chrome_buttons_set_glyph( HWND hwnd, UINT id, int glyph );
int chrome_buttons_width( HWND hwnd, int height );
BOOL chrome_buttons_rect( HWND hwnd, UINT id, RECT *rect );

/* the folders of where the dialog is, one click away each */
HWND chrome_create_address( HWND parent, UINT id );
void chrome_address_set( HWND hwnd, PCIDLIST_ABSOLUTE pidl );
LPITEMIDLIST chrome_address_target( HWND hwnd );
void chrome_address_edit( HWND hwnd );

/* the search in the folder */
HWND chrome_create_search( HWND parent, UINT id );
void chrome_search_set_cue( HWND hwnd, const WCHAR *folder );
void chrome_search_get( HWND hwnd, WCHAR *text, int count );
void chrome_search_clear( HWND hwnd );
void chrome_search_focus( HWND hwnd );

#endif /* __WINE_ITEMDLG_CHROME_H */
