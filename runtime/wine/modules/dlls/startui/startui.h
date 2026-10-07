/*
 * The Start menu of Arctic (startui.dll)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __STARTUI_H
#define __STARTUI_H

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"

#define IDS_ALL_PROGRAMS        100
#define IDS_BACK                101
#define IDS_SEARCH_HINT         102
#define IDS_DOCUMENTS           103
#define IDS_PICTURES            104
#define IDS_MUSIC               105
#define IDS_DOWNLOADS           106
#define IDS_RECENT              107
#define IDS_THIS_PC             108
#define IDS_CONTROL_PANEL       109
#define IDS_SETTINGS            110
#define IDS_UPDATE              111
#define IDS_RUN                 112
#define IDS_SHUT_DOWN           113
#define IDS_RESTART             114
#define IDS_UPDATE_AND_RESTART  115
#define IDS_HIBERNATE           178
#define IDS_HIBERNATE_REFUSED   179
#define IDS_SLEEP               180
#define IDS_PROGRAMS            116
#define IDS_RESULTS_PROGRAMS    117
#define IDS_RESULTS_SETTINGS    118
#define IDS_NO_RESULTS          119
#define IDS_OPEN                120
#define IDS_PIN                 121
#define IDS_UNPIN               122
#define IDS_REMOVE_FROM_LIST    123
#define IDS_OPEN_LOCATION       124
#define IDS_ACCESSORIES         125
#define IDS_EXPLORER            126
#define IDS_NOTEPAD             127
#define IDS_CMD                 128
#define IDS_POWERSHELL          129
#define IDS_TASKMGR             130
#define IDS_SNIPPING            131
#define IDS_REGEDIT             132
#define IDS_CPL_DISPLAY         133
#define IDS_CPL_SOUND           134
#define IDS_CPL_NETWORK         135
#define IDS_CPL_PROGRAMS        136
#define IDS_CPL_REGION          137
#define IDS_CPL_MOUSE           138
#define IDS_CPL_DATETIME        139
#define IDS_CPL_SYSTEM          140
#define IDS_CPL_KEYBOARD        141
#define IDS_CPL_POWER           142
#define IDS_CUSTOMIZE_TITLE     143
#define IDS_CUSTOMIZE_ITEMS     144
#define IDS_SHOW_LINK           145
#define IDS_SHOW_MENU           146
#define IDS_SHOW_HIDDEN         147
#define IDS_RECENT_COUNT        148
#define IDS_LARGE_ICONS         149
#define IDS_DEFAULTS            150
#define IDS_OK                  151
#define IDS_CANCEL              152
#define IDS_USER_FOLDER         153
#define IDS_EMPTY               154
#define IDS_LOOK_TITLE          160
#define IDS_LOOK_INTRO          161
#define IDS_LOOK_PRESET         162
#define IDS_LOOK_EFFECT         163
#define IDS_LOOK_COLOR          164
#define IDS_LOOK_OPACITY        165
#define IDS_LOOK_NOTE           166
#define IDS_LOOK_WIN10          167
#define IDS_LOOK_WIN11          168
#define IDS_LOOK_ACCENT         169
#define IDS_LOOK_BLUR           170
#define IDS_LOOK_CLEAR          171
#define IDS_LOOK_SOLID          172
#define IDS_LOOK_CUSTOM         173
#define IDS_EFFECT_ACRYLIC      174
#define IDS_EFFECT_BLUR         175
#define IDS_EFFECT_CLEAR        176
#define IDS_EFFECT_SOLID        177

/* the shell's colours in Windows 10, dark */
#define ACRYLIC_TINT  0xcc202020   /* 0xAABBGGRR */

extern HINSTANCE startui_instance;

WCHAR *load_string( UINT id );

/* glyph.c: sizes, fonts and the monochrome pictures of the right column */
enum glyph
{
    GLYPH_PERSON, GLYPH_DOCUMENT, GLYPH_PICTURE, GLYPH_MUSIC, GLYPH_DOWNLOAD, GLYPH_RECENT, GLYPH_PC,
    GLYPH_CONTROL_PANEL, GLYPH_SETTINGS, GLYPH_UPDATE, GLYPH_RUN, GLYPH_POWER, GLYPH_CHEVRON_RIGHT,
    GLYPH_CHEVRON_LEFT, GLYPH_CHEVRON_DOWN, GLYPH_SEARCH, GLYPH_FOLDER,
};

int px( int n );
HFONT shell_font( int height, int weight );
void draw_glyph( HDC hdc, enum glyph glyph, int x, int y, int size, COLORREF color, BYTE alpha );
void fill_alpha( HDC hdc, const RECT *rect, COLORREF color, BYTE alpha );
void fill_round_rect( HDC hdc, const RECT *rect, float radius, COLORREF color, BYTE alpha );
void make_opaque( HDC hdc, const RECT *rect );
HBITMAP icon_bitmap( HICON icon, int size );
void draw_icon_bitmap( HDC hdc, HBITMAP bitmap, int x, int y, int size );

/* items.c: what the menu lists */
enum item_kind
{
    ITEM_PROGRAM,     /* a shortcut or a program: path, maybe arguments */
    ITEM_FOLDER,      /* a folder of All programs */
    ITEM_HEADER,      /* a group title of the search results */
};

struct item
{
    enum item_kind kind;
    WCHAR name[MAX_PATH];
    WCHAR path[MAX_PATH];        /* the .lnk, the program, or the folder */
    WCHAR path2[MAX_PATH];       /* a folder's twin in the other Programs (all users / the user) */
    WCHAR args[MAX_PATH];
    WCHAR icon_path[MAX_PATH];   /* where the icon comes from, when not the path */
    WCHAR key[MAX_PATH];         /* what pins and usage counts remember it by */
    struct item *children;       /* of a folder, once read */
    UINT child_count;
    BOOL expanded;
    BOOL builtin;
    int depth;                   /* in All programs */
    HBITMAP icon_large, icon_small;        /* the icon, premultiplied, once drawn */
};

void items_reload(void);
UINT items_pinned( struct item ***list );
UINT items_frequent( struct item ***list, UINT max );
UINT items_all_programs( struct item ***list );
UINT items_search( const WCHAR *text, struct item ***list );
UINT items_applets( struct item ***list );
void items_launch( struct item *item );
BOOL items_is_pinned( const struct item *item );
void items_pin( struct item *item, BOOL pin );
void items_forget( struct item *item );
void items_open_location( struct item *item );
HBITMAP items_icon( struct item *item, BOOL large );

/* settings.c: what the menu shows, HKCU\Software\Arctic\StartMenu, and the
 * dialog of "Customize..." in the taskbar's properties */
enum right_action
{
    R_USER, R_DOCUMENTS, R_PICTURES, R_MUSIC, R_DOWNLOADS, R_RECENT, R_PC, R_CONTROL_PANEL, R_SETTINGS, R_UPDATE,
    R_RUN, R_COUNT
};

#define SHOW_HIDDEN  0
#define SHOW_LINK    1
#define SHOW_MENU    2

struct start_settings
{
    UINT recent;                 /* the most used programs shown, 0-16 */
    BOOL large_icons;
    BYTE show[R_COUNT];          /* SHOW_* */
};

void settings_load( struct start_settings *settings );
void settings_defaults( struct start_settings *settings );
void settings_save( const struct start_settings *settings );
BOOL right_can_be_menu( int action );
int right_group( int action );
enum glyph right_glyph( int action );
const WCHAR *right_name( int action );
void customize_dialog( HWND owner );

void WINAPI ArcticApplyShellLook( HWND hwnd );

/* menu.c */
void menu_toggle( HWND tray, const RECT *button, BOOL click );
void menu_hide(void);
BOOL menu_visible(void);

/* avatar.c: the user's picture over the top edge */
void avatar_show( HWND owner, int center_x, int center_y );
void avatar_hide(void);
HWND avatar_window(void);

#endif
