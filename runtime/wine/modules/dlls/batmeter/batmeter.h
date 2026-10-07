/*
 * Battery icon of the notification area (batmeter.dll)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __BATMETER_H
#define __BATMETER_H

#include <stdarg.h>
#include <stdlib.h>
#include <wchar.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "wine/arctic_power.h"

/* the tip of the icon (batmeter.dll.mui of Windows 10) */
#define IDS_TIP_AC                 121
#define IDS_TIP_FULL_IN_HOURS      122
#define IDS_TIP_FULL_IN_MINUTES    123
#define IDS_TIP_CHARGING           124
#define IDS_TIP_AC_PERCENT         125
#define IDS_TIP_FULL               126
#define IDS_TIP_NO_BATTERY         127
#define IDS_TIP_AC_ONLY            128
#define IDS_TIP_HOURS_LEFT         150
#define IDS_TIP_MINUTES_LEFT       151
#define IDS_TIP_PERCENT_LEFT       154
#define IDS_TIP_UNKNOWN            155
#define IDS_TIP_SAVER_HOURS        180
#define IDS_TIP_SAVER_MINUTES      181
#define IDS_TIP_SAVER_PERCENT      182

/* the menu and the warnings (stobject.dll.mui) */
#define IDS_MENU_POWER_OPTIONS     200
#define IDS_MENU_MOBILITY_CENTER   201
#define IDS_LOW_TITLE              202
#define IDS_LOW_TEXT               203
#define IDS_RESERVE_TITLE          204
#define IDS_RESERVE_TEXT           205
#define IDS_BATTERY_METER          206

/* the flyout (BatteryFlyoutExperience in Windows.UI.PCShell.uk-UA.pri) */
#define IDS_ONE_MINUTE             300
#define IDS_ONE_HOUR               301
#define IDS_ONE_DAY                302
#define IDS_MINUTES                303
#define IDS_HOURS                  304
#define IDS_DAYS                   305
#define IDS_LEFT                   306
#define IDS_FULLY_CHARGED          307
#define IDS_PLUGGED_IN             308
#define IDS_UNTIL_FULL             309
#define IDS_SETTINGS_LINK          310
#define IDS_BEST_PERFORMANCE       311
#define IDS_BEST_BATTERY           312
#define IDS_MODE_DC                313
#define IDS_BETTER_PERFORMANCE     314
#define IDS_BETTER_BATTERY         315
#define IDS_BATTERY_SAVER          316
#define IDS_RECOMMENDED            317
#define IDS_MODE_AC                318
#define IDS_BATTERY_MISSING        319
#define IDS_BATTERY_PERCENT        320
#define IDS_BATTERY_FULL           321
#define IDS_BATTERY_IN_USE         322
#define IDS_PERCENT                323

/* the shell's colours in Windows 10, dark */
#define ACCENT_COLOR  RGB( 0x00, 0x78, 0xd7 )
#define LINK_COLOR    RGB( 0x99, 0xeb, 0xff )   /* SystemAccentColorLight3 */
#define ACRYLIC_TINT  0xcc1f1f1f                /* 0xAABBGGRR */

extern HINSTANCE batmeter_instance;

WCHAR *load_string( UINT id );
WCHAR *format_string( UINT id, ... );
void shell_look( HWND hwnd );
int px( int n );
HFONT shell_font( int height, int weight );

/* glyph.c: the battery glyphs of Segoe MDL2 Assets, filled */
enum battery_glyph { GLYPH_BATTERY, GLYPH_CHARGING, GLYPH_SAVER };
WCHAR battery_glyph( enum battery_glyph kind, UINT percent );
float *glyph_coverage( WCHAR code, int size );
void draw_glyph( HDC hdc, WCHAR code, int x, int y, int size, COLORREF color, BYTE alpha );
HICON make_glyph_icon( WCHAR code, int size );
void fill_alpha( HDC hdc, const RECT *rect, COLORREF color, BYTE alpha );
void fill_round_rect( HDC hdc, const RECT *rect, float radius, COLORREF color, BYTE alpha );

/* power.c: what the flyout and the icon show, and what they change */
struct battery_view
{
    struct arctic_power_status status;
    BOOL saver;                  /* battery saver is on */
    BOOL slider;                 /* the power slider is there: the Balanced scheme */
    int position;                /* of the slider: 0... (battery saver on battery), steps of 1 */
    int positions;               /* 3 on the mains, 4 on battery */
};
void battery_read( struct battery_view *view );
WCHAR battery_view_glyph( const struct battery_view *view );
void battery_tip( const struct battery_view *view, WCHAR *tip, size_t count );
void battery_state_label( const struct battery_view *view, WCHAR *label, size_t count );
const WCHAR *battery_mode_name( const struct battery_view *view, int position );
void battery_set_position( struct battery_view *view, int position );
void battery_set_saver( BOOL on );
void open_power_options(void);
void open_battery_panel(void);

/* flyout.c */
void flyout_toggle( HWND tray, UINT icon_id );
void flyout_hide(void);
BOOL flyout_visible(void);
void flyout_refresh(void);

/* tray.c */
void tray_start(void);
void tray_stop(void);
void tray_update(void);

#endif
