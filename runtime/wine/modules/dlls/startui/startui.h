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
#define IDS_NOTIFICATIONS       233
#define IDS_NO_NOTIFICATIONS    234
#define IDS_CLEAR_ALL           235
#define IDS_PERSONALIZE_TITLE   236
#define IDS_MODE                237
#define IDS_MODE_LIGHT          238   /* enum mode of personalize.c from here */
#define IDS_MODE_DARK           239
#define IDS_MODE_CUSTOM         240
#define IDS_MODE_WINDOWS        241
#define IDS_MODE_APPS           242
#define IDS_CAPTIONS            243
#define IDS_EFFECT_NONE         244
#define IDS_RESET               245
#define IDS_BUTTONS_TITLE       246
#define IDS_TASK_BUTTONS        247
#define IDS_TASKBAR_WIN10       248
#define IDS_TASKBAR_WIN11       249
#define IDS_ALIGNMENT           250
#define IDS_ALIGN_LEFT          251
#define IDS_ALIGN_CENTER        252
#define IDS_START_BUTTON        253
#define IDS_START_STANDARD      254
#define IDS_START_IMAGE         255
#define IDS_BROWSE              256
#define IDS_IMAGES              257
#define IDS_CLOSE_WINDOW        258
#define IDS_CLOSE_ALL_WINDOWS   259
#define IDS_CONTROLS_STYLE      260
#define IDS_WIDGETS_TITLE       261
#define IDS_WIDGET_QUERY        262
#define IDS_WIDGET_SHOW         263
#define IDS_WIDGET_BOTH         264
#define IDS_WIDGET_ICON         265
#define IDS_WIDGET_OFF          266
#define IDS_WIDGET_HOVER        267
#define IDS_WIDGET_REFRESH      268
#define IDS_WIDGET_LOCATION     269
#define IDS_WIDGET_CITY         270
#define IDS_WIDGET_NOT_FOUND    271
#define IDS_WIDGET_FORECAST     272
#define IDS_WIDGET_ASK          273
#define IDS_WIDGET_FEELS        274
#define IDS_WIDGET_HUMIDITY     275
#define IDS_WIDGET_WIND         276
#define IDS_WIDGET_WIND_MPH     277
#define IDS_WIDGET_NO_DATA      278
#define IDS_WX_SUNNY            279
#define IDS_WX_CLEAR            280
#define IDS_WX_MOSTLY_CLEAR     281
#define IDS_WX_PARTLY_CLOUDY    282
#define IDS_WX_CLOUDY           283
#define IDS_WX_FOG              284
#define IDS_WX_DRIZZLE          285
#define IDS_WX_RAIN             286
#define IDS_WX_FREEZING         287
#define IDS_WX_SNOW             288
#define IDS_WX_SHOWERS          289
#define IDS_WX_SNOW_SHOWERS     290
#define IDS_EX_FOLDER           300
#define IDS_EX_NEW              301
#define IDS_EX_SORT             302
#define IDS_EX_VIEW             303
#define IDS_EX_SORT_NAME        304
#define IDS_EX_SORT_DATE        305
#define IDS_EX_SORT_TYPE        306
#define IDS_EX_SORT_SIZE        307
#define IDS_EX_LARGE            308
#define IDS_EX_SMALL            309
#define IDS_EX_LIST             310
#define IDS_EX_DETAILS          311
#define IDS_EX_TILES            312
#define IDS_EX_SELECT_ALL       313
#define IDS_EX_SELECT_NONE      314
#define IDS_EX_INVERT           315
#define IDS_EX_PROPERTIES       316
#define IDS_EX_OPTIONS          317
#define IDS_EX_NEW_WINDOW       318
#define IDS_EX_SEARCH_IN        319
#define IDS_EX_COPY             320
#define IDS_EX_PASTE            321
#define IDS_EX_CUT              322
#define IDS_EX_COPY_PATH        323
#define IDS_EX_PASTE_LINK       324
#define IDS_EX_MOVE_TO          325
#define IDS_EX_COPY_TO          326
#define IDS_EX_DELETE           327
#define IDS_EX_RENAME           328
#define IDS_EX_NEW_FOLDER       329
#define IDS_EX_CLIPBOARD        330
#define IDS_EX_ORGANIZE         331
#define IDS_EX_OPEN             332
#define IDS_EX_SELECT           333
#define IDS_EX_SHARE_WITH       334
#define IDS_EX_LAYOUT           335
#define IDS_EX_CURRENT_VIEW     336
#define IDS_EX_HOME             337
#define IDS_EX_SHARE            338
#define IDS_EX_VIEW_TAB         339
#define IDS_EX_FILE             340
#define IDS_DW_TITLE            350
#define IDS_DW_CLOCK            351
#define IDS_DW_LANG_SYSTEM      352
#define IDS_DW_MONITOR          353
#define IDS_DW_CPU              354
#define IDS_DW_GPU              355
#define IDS_DW_RAM              356
#define IDS_DW_DISK             357
#define IDS_DW_BATTERY          358
#define IDS_DW_RESET            359
#define IDS_DW_OK               360
#define IDS_DW_CANCEL           361
#define IDS_DW_SETTINGS         362
#define IDS_DW_REMOVE           363
#define IDS_DW_FREE             364
#define IDS_DW_GB               365
#define IDS_DW_GHZ              366
#define IDS_DW_MHZ              367
#define IDS_DW_CHARGING         368
#define IDS_DW_PLUGGED          369
#define IDS_DW_REMAINING        370
#define IDS_DW_NO_BATTERY       371
#define IDS_DW_ON_BATTERY       372
#define IDS_ICONS               373
#define IDS_ICONS_STANDARD      374
#define IDS_PIN_TASKBAR         375
#define IDS_UNPIN_TASKBAR       376
#define IDS_EX_NAV_PANE         377
#define IDS_EX_SHOW             378
#define IDS_EX_PANES            379
#define IDS_EX_ADD_DEVICE       380
#define IDS_EX_BLUETOOTH_DEVICE 381
#define IDS_EX_PRINTER          382
#define IDS_WX_STORM            291

extern HINSTANCE startui_instance;

/* gadgets.c: the widgets of the desktop */
void gadgets_start(void);
/* personalize.c: the icon theme, HKCU\Software\Arctic\Icons (user32's Arctic patch) */
#define ICONS_CHANGED L"ArcticIcons"   /* WM_SETTINGCHANGE's lparam */
void WINAPI ArcticDesktopWidgetsDialog( HWND parent );

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

/* personalize.c: the light and the dark mode */
BOOL system_mode_light(void);
void personalize_sync(void);

/* widgets.c: the weather on the taskbar and what it opens */
void widgets_hide(void);

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
    GLYPH_SLEEP, GLYPH_SIGN_OUT, GLYPH_LOCK, GLYPH_BELL, GLYPH_BELL_OFF, GLYPH_CLOSE,
};

int px( int n );
HFONT shell_font( int height, int weight );
void draw_glyph( HDC hdc, enum glyph glyph, int x, int y, int size, COLORREF color, BYTE alpha );
void fill_alpha( HDC hdc, const RECT *rect, COLORREF color, BYTE alpha );
void fill_round_rect( HDC hdc, const RECT *rect, float radius, COLORREF color, BYTE alpha );
void blend_bits( HDC hdc, int x, int y, int width, int height, const UINT32 *pixels );
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

/* A list keeps its place in whole steps, at once; what the eye sees follows,
 * an offset in pixels that runs out */
struct smooth_scroll
{
    float offset;
    DWORD last;
};

void smooth_scroll_add( struct smooth_scroll *s, HWND hwnd, UINT timer, int pixels, int limit );
BOOL smooth_scroll_step( struct smooth_scroll *s, HWND hwnd, UINT timer );
void smooth_scroll_stop( struct smooth_scroll *s, HWND hwnd, UINT timer );
int smooth_scroll_offset( const struct smooth_scroll *s );

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
    HBITMAP icon;                /* instead of the glyph: a program's, premultiplied */
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

/* taskbtn.c: the taskbar's buttons */
BOOL taskbar_centered(void);
BOOL taskbar_style11(void);
void program_name( const WCHAR *path, WCHAR *name, int size );

/* pins.c: the taskbar's pins, and what is dropped to be pinned */
#define PIN_TASKBAR       1
#define PIN_START         2
#define PIN_START_BUTTON  3
const WCHAR *path_extension( const WCHAR *path );
BOOL pinned_folder( const WCHAR *name, WCHAR *path );
BOOL link_target( const WCHAR *link, WCHAR *target, int size );
BOOL link_make( const WCHAR *link, const WCHAR *target, const WCHAR *args, const WCHAR *icon );
void free_name( const WCHAR *folder, const WCHAR *base, const WCHAR *ext, WCHAR *path );
BOOL taskbar_pin_of( const WCHAR *program, WCHAR *link );
BOOL taskbar_pin_named( const WCHAR *path, const WCHAR *name );
BOOL WINAPI ArcticTaskbarPinFile( LPCWSTR path );
BOOL WINAPI ArcticTaskbarUnpinFile( LPCWSTR link );
BOOL WINAPI ArcticStartMenuVisible(void);
BOOL pins_drop_register( HWND hwnd, DWORD where, void (*dropped)( HWND hwnd ) );

/* items.c: what pinning takes from the Start menu's programs */
BOOL items_shortcut_of( const WCHAR *program, WCHAR *link );
BOOL items_pin_file( const WCHAR *path );
BOOL items_taskbar_pinned( struct item *item, WCHAR *link );
void items_pin_taskbar( struct item *item, BOOL pin );
void items_sync_classic(void);
extern const GUID wic_factory_clsid, wic_factory_iid, wic_format_pbgra;

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

/* clock11.c: what the clock opens, Windows 11's notification center over
 * its calendar */
void clock_toggle( HWND tray, const RECT *clock, BOOL click );
void clock_hide(void);
BOOL clock_visible(void);
void text_over_opaque( HDC hdc, const WCHAR *text, RECT *rect, HFONT font, COLORREF color, UINT format );

/* avatar.c: the user's picture over the top edge */
void avatar_show( HWND owner, int center_x, int center_y );
void avatar_hide(void);
HWND avatar_window(void);
void avatar_draw( HDC hdc, int x, int y, int size, const struct palette *pal );

#endif
