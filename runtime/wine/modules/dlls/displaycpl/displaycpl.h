/*
 * Display of the Control Panel (displaycpl.dll)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __DISPLAYCPL_H
#define __DISPLAYCPL_H

#include "cpanel.h"
#include "commctrl.h"

#define IDS_NAME                1
#define IDS_TIP                 2

/* the pages and the task pane */
#define IDS_DISPLAY             10
#define IDS_ADVANCED            11
#define IDS_PERSONALIZE         12
#define IDS_BACKGROUND          13
#define IDS_SCREENSAVER         14
#define IDS_TASKBAR             15
#define IDS_POWER               16
#define IDS_NO_PAGE             17
#define IDS_DESKTOP             18
#define IDS_START               19

/* Display */
#define IDS_IDENTIFY            30
#define IDS_DETECT              31
#define IDS_APPLY_LAYOUT        33
#define IDS_CANCEL_LAYOUT       34
#define IDS_BRIGHTNESS_GROUP    40
#define IDS_BRIGHTNESS          41
#define IDS_SCALE_GROUP         42
#define IDS_SCALE               43
#define IDS_RESOLUTION          44
#define IDS_ORIENTATION         45
#define IDS_SMALLER_MODES       46
#define IDS_ICON_SIZE           47
#define IDS_RECOMMENDED         48
#define IDS_PERCENT             49
#define IDS_RES_FORMAT          50
#define IDS_LANDSCAPE           51
#define IDS_PORTRAIT            52
#define IDS_LANDSCAPE_FLIPPED   53
#define IDS_PORTRAIT_FLIPPED    54
#define IDS_SCALING_ASPECT      55
#define IDS_SCALING_STRETCH     56
#define IDS_SCALING_CENTER      57
#define IDS_ICONS_SMALL         58
#define IDS_ICONS_MEDIUM        59
#define IDS_ICONS_LARGE         60
#define IDS_MULTI_GROUP         61
#define IDS_EXTEND              62
#define IDS_ONLY_ON             63
#define IDS_MAKE_MAIN           64
#define IDS_DETECT_OTHER        65
#define IDS_RELATED             66
#define IDS_ADVANCED_LINK       67
#define IDS_GRAPHICS_LINK       68
#define IDS_SCALE_HINT          69
#define IDS_RESOLUTION_HINT     70
#define IDS_SMALLER_HINT        72
#define IDS_MAIN_DISPLAY        75
#define IDS_BRIGHTNESS_HINT     76
#define IDS_MULTI_MODE          77
#define IDS_ADVANCED_HINT       78
#define IDS_GRAPHICS_HINT       79

/* Advanced display */
#define IDS_SELECT_DISPLAY      80
#define IDS_DISPLAY_N           81
#define IDS_INFO_GROUP          82
#define IDS_CONNECTED_TO        83
#define IDS_DESKTOP_RES         84
#define IDS_SIGNAL_RES          85
#define IDS_REFRESH_RATE        86
#define IDS_BIT_DEPTH           87
#define IDS_COLOR_FORMAT        88
#define IDS_COLOR_SPACE         89
#define IDS_BITS                90
#define IDS_HZ                  91
#define IDS_ADAPTER_PROPS       92
#define IDS_REFRESH_GROUP       93
#define IDS_VRR                 94
#define IDS_DEPTH_GROUP         95
#define IDS_RGB                 96
#define IDS_SDR                 97
#define IDS_REFRESH_HINT        98
#define IDS_VRR_HINT            99

/* the question after a change */
#define IDS_KEEP_TITLE          100
#define IDS_KEEP_TEXT           101
#define IDS_KEEP                102
#define IDS_REVERT              103
#define IDS_FAILED              104
#define IDS_RESTART             105

/* Personalization */
#define IDS_MODE_GROUP          120
#define IDS_MODE_LIGHT          121
#define IDS_MODE_DARK           122
#define IDS_MODE_CUSTOM         123
#define IDS_MODE_WINDOWS        124
#define IDS_MODE_APPS           125
#define IDS_LOOK_GROUP          126
#define IDS_CONTROLS_STYLE      127
#define IDS_CONTROLS_HINT       128
#define IDS_CAPTIONS            129
#define IDS_CAPTIONS_HINT       130
#define IDS_CUSTOMIZE           131
#define IDS_ICON_THEME          132
#define IDS_ICON_THEME_HINT     133
#define IDS_ICONS_STANDARD      134
#define IDS_BACKGROUND_HINT     135
#define IDS_DESKTOP_HINT        136
#define IDS_SCREENSAVER_HINT    137
#define IDS_TASKBAR_HINT        138
#define IDS_START_HINT          139
#define IDS_TASKBAR_SHORT       140

/* Background */
#define IDS_BG_KIND             150
#define IDS_BG_PICTURE          151
#define IDS_BG_COLOR            152
#define IDS_RECENT              153
#define IDS_BROWSE_TITLE        154
#define IDS_BROWSE              155
#define IDS_FIT_TITLE           156
#define IDS_FIT_FILL            157
#define IDS_FIT_FIT             158
#define IDS_FIT_STRETCH         159
#define IDS_FIT_TILE            160
#define IDS_FIT_CENTER          161
#define IDS_BACK_COLOR          162
#define IDS_CHOOSE              163
#define IDS_COLORS_GROUP        164
#define IDS_CUSTOM_COLOR        165
#define IDS_VIEW_COLORS         166
#define IDS_PICTURES_FILTER     167
#define IDS_BROWSE_DIALOG       168

/* Desktop */
#define IDS_DESKTOP_ICONS       180
#define IDS_ICON_LOOK           181
#define IDS_RECYCLE_FULL        182
#define IDS_RECYCLE_EMPTY       183
#define IDS_DEFAULT_ICONS       184
#define IDS_RESTORE             185
#define IDS_WIDGETS_GROUP       186
#define IDS_WIDGETS             187
#define IDS_WIDGETS_HINT        188
#define IDS_ICON_LOOK_HINT      189

/* Screen saver */
#define IDS_SAVER               200
#define IDS_SAVER_NONE          201
#define IDS_SAVER_SETTINGS      202
#define IDS_SAVER_OPTIONS       203
#define IDS_SAVER_PREVIEW       204
#define IDS_WAIT                205
#define IDS_WAIT_HINT           206
#define IDS_SECURE              207
#define IDS_SECURE_HINT         208
#define IDS_POWER_HINT          209
#define IDS_MINUTES             210
#define IDS_HOURS               211

#define IDD_KEEP                300
#define IDC_KEEP_TEXT           301
#define IDC_KEEP_TITLE          302

#define IDI_DISPLAY             1

/* the cards' icons: Windows 11's pictures, the icon theme's when it has them
 * (runtime/icons/We10X/displaycpl, tools/make-we10x-icons.py) */
#define ICON_DISPLAY            101
#define ICON_BRIGHTNESS         102
#define ICON_SCALE              103
#define ICON_RESOLUTION         104
#define ICON_ORIENTATION        105
#define ICON_SMALLER            106
#define ICON_MULTI              107
#define ICON_DETECT             108
#define ICON_ADVANCED           109
#define ICON_GRAPHICS           110
#define ICON_PERSONALIZE        111
#define ICON_BACKGROUND         112
#define ICON_SCREENSAVER        113
#define ICON_MODE               114
#define ICON_COLORS             115
#define ICON_ICON_THEME         116
#define ICON_DESKTOP            117
#define ICON_STYLE              118
#define ICON_CAPTIONS           119
#define ICON_WIDGETS            120
#define ICON_DESKTOP_ICONS      121
#define ICON_ICON_SIZE          122
#define ICON_TASKBAR            123
#define ICON_START              124
#define ICON_POWER              125
#define ICON_WAIT               126
#define ICON_SECURE             127
#define ICON_SETTINGS           128

/* the pages, in cp_pages */
enum { PAGE_DISPLAY, PAGE_ADVANCED, PAGE_PERSONALIZE, PAGE_BACKGROUND, PAGE_SCREENSAVER, PAGE_DESKTOP, PAGE_COUNT };

/* the items of the Control Panel the pages go to */
#define ITEM_TASKBAR  L"::{0DF44EAA-FF21-4412-828E-260A8728E7F1}"
#define ITEM_START    L"::{0DF44EAA-FF21-4412-828E-260A8728E7F1}\\pageStartMenu"
#define ITEM_POWER    L"::{025A5937-A6BE-4686-A844-36FE4BEC8B6D}"

/**********************************************************************
 *          The monitors (monitors.c)
 */

struct mode
{
    DWORD width, height, bpp, freq, orientation;
};

struct monitor
{
    WCHAR device[32];          /* \\.\DISPLAY1: the source */
    WCHAR adapter[128];        /* the card it hangs on */
    WCHAR name[64];            /* the monitor's own, from its EDID */
    WCHAR pnp[16];             /* its Plug and Play id: where its own settings are kept */
    UINT number;               /* as Windows numbers them, from 1 */
    BOOL attached, primary;
    DEVMODEW current;          /* the mode it shows, or last showed */
    POINT pos;                 /* where it stands on the desktop; dragged on the page */
    struct mode *modes;
    UINT mode_count;
    BOOL vrr_capable, vrr;     /* variable refresh: what dwm.exe found, what is chosen */
    DWORD scaling;             /* how smaller modes show (DISPLAYCONFIG_SCALING) */
};

struct displays
{
    struct monitor *list;
    UINT count;
};

/* DISPLAYCONFIG_SCALING: pixel for pixel in the middle, stretched, shape kept */
#define SCALING_CENTER  2
#define SCALING_STRETCH 3
#define SCALING_ASPECT  4

void displays_load( struct displays *d );
void displays_free( struct displays *d );
struct monitor *displays_find( struct displays *d, const WCHAR *device );
UINT displays_attached( const struct displays *d );

/* a mode of the monitor nearest to what is asked, ~0u for any */
const struct mode *monitor_best_mode( const struct monitor *m, DWORD width, DWORD height, DWORD bpp, DWORD freq,
                                      DWORD orientation );
void mode_to_devmode( const struct mode *mode, DEVMODEW *dm );

/* a change of the monitors, kept to be taken back (display_apply asks) */
struct snapshot
{
    UINT count;
    struct { WCHAR device[32]; BOOL attached, primary; DEVMODEW mode; } list[16];
};
void snapshot_take( struct snapshot *s, const struct displays *d );
LONG snapshot_restore( const struct snapshot *s );

/* what the page asks for: written, then made so all at once */
LONG monitor_set_mode( const struct monitor *m, const DEVMODEW *dm, BOOL primary, BOOL commit );
LONG displays_commit(void);
LONG displays_set_layout( struct displays *d );
LONG displays_set_primary( struct displays *d, UINT index );
LONG displays_extend( struct displays *d );
LONG displays_only( struct displays *d, UINT index );

/* the scale of a monitor ("the size of text, apps and other items") */
BOOL scale_get( const WCHAR *device, int *min_rel, int *cur_rel, int *max_rel );
BOOL scale_set( const WCHAR *device, int rel );
extern const int scale_steps[12];

void monitor_write_vrr( const struct monitor *m );
void monitor_write_scaling( const struct monitor *m );
DWORD desktop_icon_size(void);
void desktop_icon_size_set( DWORD size );

/* the change made: kept if the user says so within 15 seconds (confirm.c) */
BOOL keep_settings( HWND owner );
void show_failure( HWND owner, LONG result );
/* each monitor's number on it for a few seconds */
void identify_monitors( const struct displays *d );

/**********************************************************************
 *          The arrangement of the monitors (layout.c)
 */

#define LAYOUT_CLASS      L"ArcticDisplayLayout"
#define LAYOUT_SELECTED   1     /* WM_COMMAND codes to the page */
#define LAYOUT_MOVED      2
#define LM_SETDISPLAYS    (WM_USER + 1)   /* lparam struct displays *, wparam the selected one */
#define LM_GETSELECTED    (WM_USER + 2)   /* the index chosen */

void layout_register(void);

/**********************************************************************
 *          Pictures in small (picture.c)
 */

HBITMAP picture_get( const WCHAR *path, int width, int height );
HBITMAP picture_peek( const WCHAR *path, int width, int height );
BOOL picture_size( const WCHAR *path, UINT *width, UINT *height );
void picture_to_tile( HWND tile, const WCHAR *path, int width, int height, BOOL set );

/**********************************************************************
 *          The desktop's background (wallpaper.c)
 */

enum { FIT_FILL, FIT_FIT, FIT_STRETCH, FIT_TILE, FIT_CENTER, FIT_COUNT };
#define BACKGROUND_PICTURE  0
#define BACKGROUND_COLOR    1

BOOL wallpaper_current( WCHAR *path, UINT count );
int wallpaper_fit(void);
DWORD background_type(void);
UINT wallpaper_history( WCHAR paths[][MAX_PATH], UINT max );
UINT wallpaper_windows( WCHAR paths[][MAX_PATH], UINT max );
BOOL wallpaper_set( const WCHAR *path, int fit );
COLORREF desktop_color(void);
void desktop_color_set( COLORREF color, BOOL remove_picture );

/* the desktop in small: the background, a window in the modes, the taskbar */
#define OVERLAY_SYSTEM_LIGHT  1
#define OVERLAY_APPS_LIGHT    2
#define OVERLAY_WINDOW        4

struct preview
{
    int width, height;         /* the preview's */
    int screen_width;
    BOOL picture;
    WCHAR path[MAX_PATH];
    UINT image_width, image_height;
    int fit;
    COLORREF color;
    UINT_PTR overlay;
};

void desktop_overlay( HDC hdc, const RECT *rect, UINT_PTR param );
void preview_init( struct preview *p, int width, int height );
void preview_thumb_size( const struct preview *p, int *width, int *height );
void preview_paint( HDC hdc, const RECT *rect, UINT_PTR param );
int preview_width( int height );

/**********************************************************************
 *          The light and the dark mode, the looks (startui.dll's)
 */

#define LOOK_STYLE11      4
#define LOOK_WE10X        8
/* OVERLAY_SYSTEM_LIGHT, OVERLAY_APPS_LIGHT, LOOK_STYLE11, LOOK_WE10X */
DWORD modes_get(void);

/**********************************************************************
 *          The pages
 */

/* a screen saver: its name (its string 1) and its file */
struct saver
{
    WCHAR name[128];
    WCHAR path[MAX_PATH];
};

struct display_state
{
    struct displays displays;
    WCHAR selected[32];        /* the device chosen in the arrangement */
    BOOL moved;                /* dragged and not applied yet */
    /* Advanced display: what the card of the monitor says */
    struct
    {
        WCHAR name[128], connected[256];
        WCHAR label[6][64], value[6][64];
        UINT count;
    } info;
    /* Background, Personalization: the desktop in small */
    struct preview preview;
    /* Background: the pictures it lists, what its list of the kind shows */
    WCHAR pictures[12][MAX_PATH];
    UINT picture_count;
    DWORD kind;
    /* Screen saver: the ones there are, the preview's process */
    struct saver *savers;
    UINT saver_count;
    HANDLE saver_preview;
};

struct display_state *display_state( struct view *view );
void nav_display( struct view *view, UINT current );

void display_build( struct view *view );
BOOL display_command( struct view *view, UINT id, UINT code, HWND control );
void advanced_build( struct view *view );
BOOL advanced_command( struct view *view, UINT id, UINT code, HWND control );
void personalize_build( struct view *view );
BOOL personalize_command( struct view *view, UINT id, UINT code, HWND control );
void background_build( struct view *view );
BOOL background_command( struct view *view, UINT id, UINT code, HWND control );
void desktop_build( struct view *view );
BOOL desktop_command( struct view *view, UINT id, UINT code, HWND control );
void screensaver_build( struct view *view );
BOOL screensaver_command( struct view *view, UINT id, UINT code, HWND control );

/* a change asked for from a control's notification, made after it returns:
 * the page is laid out anew then */
void defer_command( struct view *view, UINT id );

/* helpers of the pages (page_display.c) */
void combo_add( HWND combo, const WCHAR *text, LPARAM data, BOOL current );
LPARAM combo_data( HWND combo );
HWND card_combo( struct view *view, UINT icon, UINT title, UINT hint, UINT id );
void go_page( struct view *view, UINT_PTR page );
void go_item( struct view *view, UINT_PTR item );

#endif
