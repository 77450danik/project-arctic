/*
 * Power Options of the Control Panel (powercpl.dll)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __POWERCPL_H
#define __POWERCPL_H

#include <stdarg.h>
#include <stdlib.h>
#include <wchar.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "winreg.h"
#include "wine/arctic_power.h"

/* Windows 10's own (powercpl.dll.mui), by their numbers there */
#define IDS_POWER_OPTIONS          1
#define IDS_POWER_OPTIONS_TIP      2
#define IDS_BATTERY_COLUMN         3
#define IDS_AC_COLUMN              4
#define IDS_SAVE_CHANGES           5
#define IDS_CANCEL                 6
#define IDS_LESS_THAN_MINUTE       9
#define IDS_ONE_MIN                10
#define IDS_MINUTES                11
#define IDS_ONE_HOUR               12
#define IDS_HOURS                  13
#define IDS_HOURS_MINUTES          14
#define IDS_NEVER                  15
#define IDS_SAVE_FAILED            25
#define IDS_ACTIVATE_FAILED        26
#define IDS_CHANGE_UNAVAILABLE     30
#define IDS_PAGE_PLANS             50
#define IDS_PAGE_CREATE            51
#define IDS_PAGE_EDIT              52
#define IDS_PAGE_SYSTEM            54
#define IDS_PLANS_TITLE            100
#define IDS_PLANS_TEXT             101
#define IDS_TASK_BUTTONS           110
#define IDS_TASK_POWER_BUTTON      111
#define IDS_TASK_LID               112
#define IDS_TASK_CREATE            114
#define IDS_TASK_DISPLAY           120
#define IDS_TASK_SLEEP             121
#define IDS_SEE_MOBILITY           131
#define IDS_SEE_PERSONALIZATION    132
#define IDS_SEE_ACCOUNTS           133
#define IDS_PREFERRED_PLANS        150
#define IDS_BATTERY_METER_PLANS    151
#define IDS_SHOW_MORE_PLANS        152
#define IDS_HIDE_MORE_PLANS        153
#define IDS_CHANGE_PLAN            165
#define IDS_CHANGE_PLAN_OF         166
#define IDS_RECOMMENDED            170
#define IDS_EDIT_TITLE             200
#define IDS_EDIT_TEXT              201
#define IDS_TURN_OFF_DISPLAY       210
#define IDS_PUT_TO_SLEEP           211
#define IDS_ADVANCED_LINK          227
#define IDS_DELETE_PLAN_LINK       228
#define IDS_RESTORE_PLAN_LINK      229
#define IDS_DELETE_CONFIRM         222
#define IDS_DELETE_CONFIRM_TEXT    223
#define IDS_RESTORE_CONFIRM        225
#define IDS_RESTORE_CONFIRM_TEXT   226
#define IDS_CREATE                 230
#define IDS_SYSTEM_TITLE           300
#define IDS_SYSTEM_TEXT            301
#define IDS_BUTTONS_SLEEP_LID      310
#define IDS_BUTTONS_SLEEP          311
#define IDS_BUTTONS_POWER          312
#define IDS_BUTTONS_LID            313
#define IDS_WHEN_POWER_BUTTON      320
#define IDS_WHEN_SLEEP_BUTTON      321
#define IDS_WHEN_LID               322
#define IDS_SHUTDOWN_SETTINGS      350
#define IDS_FAST_STARTUP           351
#define IDS_FAST_STARTUP_TEXT      354
#define IDS_SHOW_SLEEP             356
#define IDS_SHOW_SLEEP_TEXT        357
#define IDS_SHOW_HIBERNATE         358
#define IDS_SHOW_HIBERNATE_TEXT    359
#define IDS_SHOW_LOCK              360
#define IDS_SHOW_LOCK_TEXT         361
#define IDS_CREATE_TITLE           400
#define IDS_CREATE_TEXT            401
#define IDS_PLAN_NAME              425
#define IDS_NAME_TAKEN             440
#define IDS_NAME_EMPTY             441
#define IDS_MY_PLAN                450
#define IDS_NEXT                   460

/* Control Panel and Windows 10 Settings texts not in powercpl.dll.mui */
#define IDS_CONTROL_PANEL_HOME     600
#define IDS_SEE_ALSO               601
#define IDS_SCREEN_BRIGHTNESS      602
#define IDS_ADJUST_BRIGHTNESS      603
#define IDS_TASK_BATTERY           604
#define IDS_TASK_GRAPHICS          605

/* the page of the batteries and graphics cards (Arctic's) */
#define IDS_BATTERY_TITLE          700
#define IDS_BATTERY_TEXT           701
#define IDS_NO_BATTERY             702
#define IDS_STATE_CHARGING         703
#define IDS_STATE_DISCHARGING      704
#define IDS_STATE_FULL             705
#define IDS_STATE_NOT_CHARGING     706
#define IDS_FROM_MAINS             707
#define IDS_FROM_BATTERY           708
#define IDS_DRAW_NOW               709
#define IDS_UNTIL_EMPTY            710
#define IDS_UNTIL_FULL             711
#define IDS_LAST_HOUR              712
#define IDS_HEALTH                 713
#define IDS_MANUFACTURER           714
#define IDS_MODEL                  715
#define IDS_CHEMISTRY              716
#define IDS_DESIGN_CAPACITY        717
#define IDS_FULL_CAPACITY          718
#define IDS_WEAR                   719
#define IDS_CYCLES                 720
#define IDS_VOLTAGE                721
#define IDS_GPUS                   722
#define IDS_GPU_POWER_SAVING       723
#define IDS_GPU_HIGH_PERFORMANCE   724
#define IDS_GPU_ONLY               725
#define IDS_GPU_ACTIVE             726
#define IDS_GPU_ASLEEP             727
#define IDS_GPU_DRAW               728
#define IDS_GPU_TEMPERATURE        729
#define IDS_GPU_LOAD               730
#define IDS_GPU_MEMORY             731
#define IDS_GPU_PROGRAMS           732
#define IDS_GPU_NO_PROGRAMS        733
#define IDS_UNKNOWN                734
#define IDS_WATTS                  735
#define IDS_MWH                    736
#define IDS_PROCESSOR              737
#define IDS_GRAPH_EMPTY            738
#define IDS_BATTERY_N              739
#define IDS_NOT_COUNTED            740
#define IDS_POWER_SOURCE           741

/* Graphics settings (Windows 10's Settings, Windows.UI.SettingsAppThreshold) */
#define IDS_GRAPHICS_TITLE         800
#define IDS_GRAPHICS_PREFERENCE    801
#define IDS_GRAPHICS_TEXT          802
#define IDS_CHOOSE_APP             803
#define IDS_CLASSIC_APP            804
#define IDS_STORE_APP              805
#define IDS_BROWSE                 806
#define IDS_OPTIONS                807
#define IDS_REMOVE                 808
#define IDS_LET_WINDOWS_DECIDE     809
#define IDS_POWER_SAVING           810
#define IDS_HIGH_PERFORMANCE       811
#define IDS_GRAPHICS_OPTIONS       812
#define IDS_GRAPHICS_QUESTION      813
#define IDS_SAVE                   814
#define IDS_GPU_LINE               815
#define IDS_ALREADY_ADDED          816
#define IDS_NO_APPS                817
#define IDS_HAGS                   818
#define IDS_HAGS_TEXT              819
#define IDS_VRR                    820
#define IDS_VRR_TEXT               821
#define IDS_RESTART_NEEDED         822
#define IDS_DEFAULT_GRAPHICS       823
#define IDS_PROGRAMS_FILTER        824
#define IDS_ONE_GPU                825

/* the Advanced settings dialog (powercfg.cpl.mui) */
#define IDS_ADV_SHEET              900
#define IDS_ADV_RESTORE_CONFIRM    901
#define IDS_ADV_RESTORE_TEXT       902
#define IDS_ADV_ACTIVE             903
#define IDS_ADV_ON_BATTERY         904
#define IDS_ADV_PLUGGED_IN         905
#define IDS_ADV_SETTING            906
#define IDS_ADV_ON_BATTERY_UNITS   907
#define IDS_ADV_PLUGGED_IN_UNITS   908
#define IDS_ADV_SETTING_UNITS      909
#define IDS_ADV_MINUTES            910
#define IDS_ADV_NEVER              911
#define IDS_ADV_SAVE_FAILED        912

#define IDD_ADVANCED               1000
#define IDC_ADV_ICON               1114
#define IDC_ADV_TEXT               1100
#define IDC_ADV_SCHEME             1104
#define IDC_ADV_TREE               1105
#define IDC_ADV_RESTORE            1113

#define IDD_GRAPHICS_OPTIONS       1200
#define IDC_GFX_QUESTION           1201
#define IDC_GFX_DECIDE             1202
#define IDC_GFX_SAVING             1203
#define IDC_GFX_SAVING_GPU         1204
#define IDC_GFX_FAST               1205
#define IDC_GFX_FAST_GPU           1206

#define IDI_POWER                  1

extern HINSTANCE powercpl_instance;

WCHAR *load_string( UINT id );
WCHAR *format_string( UINT id, ... );
int px( int n );

/* the pages, one shell folder each (folder.c) */
enum page
{
    PAGE_PLANS,       /* Вибір і настроювання плану живлення */
    PAGE_EDIT,        /* Змінення настройок плану */
    PAGE_CREATE,      /* Створення плану живлення */
    PAGE_SYSTEM,      /* Настройки системи */
    PAGE_BATTERY,     /* Акумулятор і відеокарти */
    PAGE_GRAPHICS,    /* Настройки графіки */
    PAGE_COUNT
};

HRESULT folder_create( enum page page, const GUID *scheme, BOOL creating, REFIID riid, void **out );
const WCHAR *page_name( enum page page );

/* an item of Power Options: a page (folder.c) */
void *page_item_create( enum page page, const GUID *scheme, BOOL creating );   /* an ITEMIDLIST, CoTaskMemFree */

/* view.c: the shell view of a page, with the Control Panel's look */
struct view;
typedef void (*page_build_proc)( struct view *view );

/* a page's contents, laid out top to bottom in the content pane */
enum
{
    STYLE_TITLE,      /* 16 px, the Control Panel's title blue */
    STYLE_BODY,       /* 12 px black */
    STYLE_BOLD,
    STYLE_GROUP,      /* a group header with a line after it */
    STYLE_SMALL,      /* the note under a check box */
    STYLE_GRAY,
    STYLE_BIG,        /* the battery page's charge */
};

typedef void (*link_proc)( struct view *view, UINT_PTR param );
typedef void (*paint_proc)( struct view *view, HDC hdc, const RECT *rect );

void view_clear( struct view *view );
void view_text( struct view *view, int style, const WCHAR *text );
void view_text_at( struct view *view, int style, const WCHAR *text, int indent );
void view_link( struct view *view, const WCHAR *text, link_proc proc, UINT_PTR param, int indent );
void view_space( struct view *view, int height );
void view_group( struct view *view, const WCHAR *text );
HWND view_control( struct view *view, const WCHAR *cls, const WCHAR *text, DWORD style, int width, int height,
                   int indent, UINT id );
HWND view_control_beside( struct view *view, const WCHAR *cls, const WCHAR *text, DWORD style, int x, int width,
                          int height, UINT id );
void view_paint_area( struct view *view, int height, paint_proc proc );
void view_row_begin( struct view *view );
void view_row_end( struct view *view );
void view_nav_link( struct view *view, const WCHAR *text, link_proc proc, UINT_PTR param );
void view_nav_see_also( struct view *view, const WCHAR *text, link_proc proc, UINT_PTR param );
void view_layout( struct view *view );
void view_rebuild( struct view *view );
void view_navigate( struct view *view, enum page page, const GUID *scheme, BOOL creating );
/* the page "control powercfg.cpl,,battery" asked for, once, for the window that opens */
#define START_PAGE_KEY L"Software\\Arctic\\PowerOptions"
void start_page_set( enum page page );
enum page start_page_take(void);
void view_back( struct view *view );
void view_set_timer( struct view *view, UINT ms );
HWND view_window( struct view *view );
HFONT view_font( int style );
int view_content_width( struct view *view );

/* what a page keeps between its builds */
struct page_state
{
    enum page page;
    GUID scheme;
    BOOL creating;           /* the edit page of a plan being made */
    BOOL more_plans;         /* the other plans are shown */
    BOOL elevated;           /* "Change settings that are currently unavailable" was clicked */
    void *data;              /* the page's own */
};
struct page_state *view_state( struct view *view );

/* the pages (page_*.c): build their contents, answer WM_COMMAND from their controls */
void plans_build( struct view *view );
BOOL plans_command( struct view *view, UINT id, UINT code, HWND control );
void edit_build( struct view *view );
BOOL edit_command( struct view *view, UINT id, UINT code, HWND control );
void create_build( struct view *view );
BOOL create_command( struct view *view, UINT id, UINT code, HWND control );
void system_build( struct view *view );
BOOL system_command( struct view *view, UINT id, UINT code, HWND control );
void battery_build( struct view *view );
BOOL battery_command( struct view *view, UINT id, UINT code, HWND control );
void battery_timer( struct view *view );
void graphics_build( struct view *view );
BOOL graphics_command( struct view *view, UINT id, UINT code, HWND control );

/* the task links every page's left pane has */
void nav_power_tasks( struct view *view );

/* schemes.c: the plans */
struct plan
{
    GUID guid;
    WCHAR name[128];
    WCHAR description[256];
    BOOL builtin;
    BOOL active;
};
UINT plans_list( struct plan *plans, UINT max );
BOOL plan_active( GUID *guid );
BOOL plan_name( const GUID *guid, WCHAR *name, DWORD count );
BOOL on_battery_machine(void);   /* the machine has a battery: two columns */
BOOL lid_present(void);
DWORD plan_value( const GUID *scheme, const GUID *sub, const GUID *setting, BOOL ac, DWORD fallback );
void plan_set_value( const GUID *scheme, const GUID *sub, const GUID *setting, BOOL ac, DWORD value );
void format_timeout( DWORD seconds, WCHAR *text, size_t count );
void open_advanced_settings( HWND owner, const GUID *scheme );
void open_control_panel( const WCHAR *args );

extern const GUID sub_video, sub_sleep, sub_buttons, sub_battery, sub_none;
extern const GUID set_video_idle, set_brightness, set_standby, set_hibernate_idle;
extern const GUID set_lid, set_pbutton, set_sbutton;
extern const GUID scheme_balanced, scheme_max, scheme_min;

#endif
