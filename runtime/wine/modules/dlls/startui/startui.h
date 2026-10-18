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
#include "prsht.h"

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
#define IDS_LOOK_BLUR_AMOUNT    181
#define IDS_LOOK_TEXT           182
#define IDS_TEXT_AUTO           183
#define IDS_TEXT_WHITE          184
#define IDS_TEXT_BLACK          185
#define IDS_START_TITLE         186
#define IDS_STYLE_STANDARD      187
#define IDS_STYLE_WIN10         188
#define IDS_STYLE_WIN11         189
#define IDS_STYLE_CLASSIC       190
#define IDS_CUSTOMIZE           191
#define IDS_INHERIT             192
#define IDS_PINNED              193
#define IDS_RECOMMENDED         194
#define IDS_MORE                195
#define IDS_SEARCH_WIN11        196
#define IDS_RECENTLY_ADDED      197
#define IDS_MOST_USED           198
#define IDS_EXPAND              199
#define IDS_POWER               200
#define IDS_NO_RECOMMENDED      201
#define IDS_JUST_NOW            202
#define IDS_MINUTES_AGO         203
#define IDS_HOURS_AGO           204
#define IDS_YESTERDAY           205
#define IDS_USER_ACCOUNTS       206
#define IDS_SIGN_OUT            207
#define IDS_LOCK                208
#define IDS_SHOW_APP_LIST       209
#define IDS_SHOW_RECENT_ADDED   210
#define IDS_SHOW_MOST_USED      211
#define IDS_SHOW_RECENT_ITEMS   212
#define IDS_FOLDERS_ON_START    213
#define IDS_LAYOUT              214
#define IDS_LAYOUT_PINS         215
#define IDS_LAYOUT_DEFAULT      216
#define IDS_LAYOUT_RECOMMENDED  217
#define IDS_UNPIN_TILE          218
#define IDS_SEARCH_RESULTS      219
#define IDS_FILE_EXPLORER_PLACE 220
#define IDS_VIDEOS              221
#define IDS_NETWORK             222
#define IDS_START_CAPTION       223
#define IDS_RESIZE              224
#define IDS_TILE_SMALL          225
#define IDS_TILE_MEDIUM         226
#define IDS_TILE_WIDE           227
#define IDS_TILE_LARGE          228
#define IDS_MORE_ACTIONS        229
#define IDS_PIN_START10         230
#define IDS_UNPIN_START10       231
#define IDS_RESULTS_FILES       232

extern HINSTANCE startui_instance;

WCHAR *load_string( UINT id );

/* taskbar.c: the look of the taskbar and of the Start menus */
#define LOOK_BLUR_DEFAULT  50

enum look_text { TEXT_AUTO, TEXT_WHITE, TEXT_BLACK, TEXT_COUNT };

struct look
{
    DWORD preset;
    DWORD effect;     /* ARCTIC_ACCENT_* */
    COLORREF color;
    DWORD opacity;    /* percent */
    DWORD blur;       /* 1-100, DWMWA_ARCTIC_BLUR */
    DWORD text;       /* enum look_text */
};

#define LOOK_EFFECT_COUNT 4
struct look_effect { DWORD effect; UINT name; };
extern const struct look_effect look_effects[LOOK_EFFECT_COUNT];

/* the styles of the Start menu, HKCU\Software\Arctic\StartMenu\Style */
enum start_style { STYLE_STANDARD, STYLE_CLASSIC, STYLE_WIN10, STYLE_WIN11, STYLE_COUNT };

COLORREF accent_color(void);
void look_load_taskbar( struct look *look );
BOOL look_is_light( const struct look *look );
BOOL look_apply( HWND hwnd, const struct look *look );
void start_look_defaults( int style, struct look *look );
BOOL start_look_inherits( int style );
void start_look_load_own( int style, struct look *look );
void start_look_load( int style, struct look *look );
void start_look_save( int style, BOOL inherit, const struct look *own );
void taskbar_page_look( struct look *look );

/* the controls of a look in a dialog, their ids from a first one */
enum
{
    LC_EFFECT_LABEL, LC_EFFECT, LC_COLOR_LABEL, LC_COLOR, LC_OPACITY_LABEL, LC_OPACITY, LC_OPACITY_VALUE,
    LC_BLUR_LABEL, LC_BLUR, LC_BLUR_VALUE, LC_TEXT_LABEL, LC_TEXT, LC_COUNT
};

HWND add_dialog_control( HWND dialog, const WCHAR *cls, const WCHAR *text, DWORD style, int x, int y, int width,
                         int height, int id );
int look_controls_create( HWND dialog, int y, int first );
void look_controls_show( HWND dialog, int first, const struct look *look, BOOL enabled );
BOOL look_controls_command( HWND dialog, int first, struct look *look, UINT msg, WPARAM wparam, LPARAM lparam );
BOOL look_controls_draw( int first, const struct look *look, WPARAM wparam, LPARAM lparam );

struct page_template
{
    DLGTEMPLATE header;
    WORD menu, cls, title;
    WORD point_size;
    WCHAR face[13];
};
extern const struct page_template page_template;

/* How a menu draws over its backdrop: white on black, which dwm.exe keys
 * by brightness, or, over a light look, dark on white, keyed by darkness */
struct palette
{
    BOOL light;
    COLORREF back;       /* what shows the backdrop */
    COLORREF text;
    COLORREF dim;        /* the second line, the group titles */
    COLORREF ink;        /* glyphs, lines, what is under the pointer */
    COLORREF field;      /* the search box */
    COLORREF field_text;
};

void palette_init( struct palette *pal, BOOL light );

/* glyph.c: sizes, fonts and the monochrome pictures of the right column */
enum glyph
{
    GLYPH_NONE = -1,
    GLYPH_PERSON, GLYPH_DOCUMENT, GLYPH_PICTURE, GLYPH_MUSIC, GLYPH_DOWNLOAD, GLYPH_RECENT, GLYPH_PC,
    GLYPH_CONTROL_PANEL, GLYPH_SETTINGS, GLYPH_UPDATE, GLYPH_RUN, GLYPH_POWER, GLYPH_CHEVRON_RIGHT,
    GLYPH_CHEVRON_LEFT, GLYPH_CHEVRON_DOWN, GLYPH_SEARCH, GLYPH_FOLDER, GLYPH_VIDEO, GLYPH_NETWORK,
    GLYPH_HAMBURGER, GLYPH_CHEVRON_UP, GLYPH_LIST, GLYPH_CHECK, GLYPH_PIN, GLYPH_UNPIN, GLYPH_RESIZE, GLYPH_MORE,
    GLYPH_SLEEP, GLYPH_SIGN_OUT, GLYPH_LOCK,
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
    HBITMAP icon_other;                    /* ... at another size */
    int other_size;
    FILETIME added;              /* when its shortcut was made */
    WCHAR aliases[160];          /* what search also finds it by: English names, the program's file */
    BOOL aliased;                /* ... looked up */
};

/* a file of the Recent folder, for Windows 11's Recommended */
struct recent_file
{
    WCHAR name[MAX_PATH];
    WCHAR path[MAX_PATH];        /* the shortcut in Recent */
    FILETIME time;
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
HBITMAP items_icon_sized( struct item *item, int size );
UINT items_apps( struct item ***list );
UINT items_recently_added( struct item ***list, UINT max );
UINT items_recent_files( struct recent_file *files, UINT max );
HBITMAP recent_file_icon( const struct recent_file *file, int size );

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

/* the Start menus of Windows 10 and 11: what "Personalization > Start" sets */
enum folder
{
    FOLDER_EXPLORER, FOLDER_SETTINGS, FOLDER_DOCUMENTS, FOLDER_DOWNLOADS, FOLDER_MUSIC, FOLDER_PICTURES,
    FOLDER_VIDEOS, FOLDER_NETWORK, FOLDER_USER, FOLDER_COUNT
};

enum layout11 { LAYOUT_MORE_PINS, LAYOUT_DEFAULT, LAYOUT_MORE_RECOMMENDED, LAYOUT_COUNT };

struct modern_settings
{
    BOOL app_list;           /* Windows 10: the list of programs beside the tiles */
    BOOL recently_added;
    BOOL most_used;
    BOOL recent_items;       /* Windows 11: Recommended shows recent files */
    DWORD layout;            /* Windows 11: enum layout11 */
    DWORD folders;           /* 1 << enum folder */
};

void modern_settings_load( int style, struct modern_settings *settings );
void modern_settings_defaults( int style, struct modern_settings *settings );
void modern_settings_save( int style, const struct modern_settings *settings );
void modern_customize_dialog( HWND owner, int style );

void WINAPI ArcticApplyShellLook( HWND hwnd );

/* common.c */
enum item_action { ACTION_NONE, ACTION_LAUNCHED, ACTION_CHANGED };

void paint_text( HDC hdc, const WCHAR *text, RECT *rect, HFONT font, COLORREF color, UINT format );
void frame_rect( HDC hdc, const RECT *rect, int width, COLORREF color );
HDC paint_begin( HWND hwnd, PAINTSTRUCT *ps, const struct palette *pal, HBITMAP *bitmap, HGDIOBJ *old );
void paint_end( HWND hwnd, PAINTSTRUCT *ps, HDC mem, HBITMAP bitmap, HGDIOBJ old );
void open_folder_id( int csidl );
void open_path( const WCHAR *path );
void open_downloads(void);
void open_run( HWND tray );
const WCHAR *folder_name( int folder );
enum glyph folder_glyph( int folder );
void folder_open( int folder );
BOOL update_ready(void);
void power_shut_down(void);
/* popup.c: the context menus of the Start menus */
enum popup_style { POPUP_SYSTEM = -1, POPUP_WIN10, POPUP_WIN11 };

struct popup_item
{
    UINT id;                     /* 0 with no submenu: a separator */
    const WCHAR *text;
    enum glyph glyph;            /* GLYPH_NONE: none */
    BOOL checked, disabled;
    const struct popup_item *sub;
    UINT sub_count;
};

UINT popup_track( HWND owner, POINT pt, UINT align, const struct popup_item *items, UINT count, int popup_style,
                  const struct look *look );

BOOL power_popup( HWND owner, POINT pt, UINT align, BOOL *in_popup, void (*hide)(void), int popup_style,
                  const struct look *look );
void user_popup( HWND owner, POINT pt, UINT align, BOOL *in_popup, void (*hide)(void), int popup_style,
                 const struct look *look );
enum item_action item_popup( HWND owner, struct item *item, POINT pt, BOOL removable, BOOL *in_popup,
                             void (*hide)(void), int popup_style, const struct look *look );
int start_style_load(void);
void start_style_save( int style );
void describe_age( const FILETIME *time, WCHAR *text, int size );

/* startpage.c: the page "Меню «Пуск»" of the taskbar's properties */
HPROPSHEETPAGE WINAPI ArcticStartMenuPage( HWND tray, void (WINAPI *classic_customize)( HWND ) );

/* menu.c: the standard menu, two columns as Windows 7 */
void menu_toggle( HWND tray, const RECT *button, BOOL click );
void menu_hide(void);
BOOL menu_visible(void);

/* menu10.c, menu11.c: the menus of Windows 10 and 11 */
void menu10_toggle( HWND tray, const RECT *button, BOOL click );
void menu10_hide(void);
BOOL menu10_visible(void);
void menu11_toggle( HWND tray, const RECT *button, BOOL click );
void menu11_hide(void);
BOOL menu11_visible(void);

/* avatar.c: the user's picture over the top edge */
void avatar_show( HWND owner, int center_x, int center_y );
void avatar_hide(void);
HWND avatar_window(void);
void avatar_draw( HDC hdc, int x, int y, int size, const struct palette *pal );

#endif
