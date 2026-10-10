/*
 * Sound of the Control Panel (mmsys.cpl)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __MMSYS_H
#define __MMSYS_H

#include <stdarg.h>
#include <stdlib.h>
#include <wchar.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "winreg.h"
#include "commctrl.h"

/* Windows' dialogs (mmsys.cpl.mui), their control ids kept */
#define IDD_PLAYBACK           110
#define IDD_RECORDING          111
#define IDD_SOUNDS             112
#define IDD_COMMUNICATIONS     113
#define IDD_SAVE_SCHEME        115
#define IDD_GENERAL            121
#define IDD_LEVELS             124
#define IDD_ADVANCED           126
#define IDD_LISTEN             128
#define IDD_LEVEL_ROW          141
#define IDD_BALANCE_ROW        144
#define IDD_BALANCE            145

#define IDC_DEVICES            1000
#define IDC_CONFIGURE          1001
#define IDC_SET_DEFAULT        1002
#define IDC_DEVICE_PROPERTIES  1003
#define IDC_SCHEMES            1100
#define IDC_SAVE_AS            1101
#define IDC_DELETE_SCHEME      1102
#define IDC_EVENTS             1103
#define IDC_SOUND_LABEL        1104
#define IDC_SOUND_FILE         1105
#define IDC_TEST               1106
#define IDC_BROWSE             1107
#define IDC_STARTUP_SOUND      1108
#define IDC_SCHEME_NAME        1110
#define IDC_COMM_ICON          1150
#define IDC_COMM_MUTE          1151
#define IDC_COMM_50            1152
#define IDC_COMM_80            1153
#define IDC_COMM_NOTHING       1154
#define IDC_GEN_NAME           1201
#define IDC_GEN_ICON           1200
#define IDC_GEN_CHANGE_ICON    1204
#define IDC_GEN_CONTROLLER_ICON 1202
#define IDC_GEN_CONTROLLER     1207
#define IDC_GEN_CONTROLLER_PROPS 1208
#define IDC_GEN_NO_CONTROLLER  1203
#define IDC_GEN_JACK           1206
#define IDC_GEN_USAGE          1209
#define IDC_LEVELS_LINE        1507
#define IDC_ROW_GROUP          1510
#define IDC_ROW_SLIDER         1511
#define IDC_ROW_VALUE          1516
#define IDC_ROW_MUTE           1512
#define IDC_ROW_BALANCE        1513
#define IDC_BAL_LABEL          1524
#define IDC_BAL_SLIDER         1525
#define IDC_BAL_VALUE          1526
#define IDC_ADV_FORMAT         1410
#define IDC_ADV_TEST           1300
#define IDC_ADV_EXCLUSIVE      1411
#define IDC_ADV_EXCLUSIVE_PRIORITY 1412
#define IDC_ADV_RESTORE        1416
#define IDC_LISTEN_CHECK       1601
#define IDC_LISTEN_DEVICE      1600
#define IDC_LISTEN_TEXT        1604

/* Windows' strings (mmsys.cpl.mui) by their numbers there */
#define IDS_SOUND              300
#define IDS_SOUND_TIP          301
#define IDS_TEST               308
#define IDS_STOP               309
#define IDS_GENERAL            311
#define IDS_LEVELS             314
#define IDS_ADVANCED           315
#define IDS_LISTEN             318
#define IDS_QUALITY_FIRST      320    /* ... 328 */
#define IDS_USE_DEVICE         330
#define IDS_DONT_USE_DEVICE    331
#define IDS_READY              335
#define IDS_ENABLE_DEVICE      340
#define IDS_NOT_PLUGGED        336
#define IDS_UNAVAILABLE        337
#define IDS_DISABLED_STATE     338
#define IDS_FORMAT_CHANNELS    345
#define IDS_FORMAT_PLAIN       346
#define IDS_CHANNEL_SHORT      370    /* Л, П, Ц... */
#define IDS_HEARD              415
#define IDS_NONE               420
#define IDS_MASTER             421
#define IDS_NO_DEVICES         422
#define IDS_CHANGED            423
#define IDS_BROWSE_TITLE       424
#define IDS_WAV_FILTER         425
#define IDS_CHANGE_SCHEME      430
#define IDS_SCHEME_EXISTS      431
#define IDS_DELETE_SCHEME_Q    435
#define IDS_ZERO_DB            441
#define IDS_DB                 442
#define IDS_FORMAT_ERROR       505
#define IDS_IN_USE             507
#define IDS_DEFAULT_DEVICE     590
#define IDS_DEFAULT_COMM       592
#define IDS_DEFAULT_MULTIMEDIA 593

/* Arctic's own */
#define IDS_LISTEN_JACK        2000
#define IDS_NO_JACK_INFO       2001
#define IDS_SPEAKERS           2010
#define IDS_HEADPHONES         2011
#define IDS_DIGITAL_OUTPUT     2012
#define IDS_DIGITAL_DISPLAY    2013
#define IDS_MICROPHONE         2014
#define IDS_LINE_IN            2015
#define IDS_SET_DEFAULT_COMM   2020
#define IDS_SET_DEFAULT        2021
#define IDS_WINDOWS_DEFAULT    2022
#define IDS_NO_SOUNDS          2023
#define IDS_APP_EVENTS         2024
#define IDS_DISABLE_DEVICE     2025
#define IDS_PROPERTIES         2026
#define IDS_PROPERTIES_TITLE   2027
#define IDS_WAV_FILES          2028
#define IDS_DEFAULT_PLAYBACK   2030

#define IDI_SOUND              3004
#define IDI_SPEAKERS           3010
#define IDI_HEADPHONES         3011
#define IDI_LINE               3012
#define IDI_DIGITAL            3013
#define IDI_MICROPHONE         3014
#define IDI_HEADSET            3015
#define IDI_DISPLAY            3017
#define IDI_SOUND_CARD         3018
#define IDI_COMMUNICATIONS     3052

extern HINSTANCE mmsys_instance;

WCHAR *load_string( UINT id );
int px( int n );

/* an endpoint of Core Audio (devices.c) */
struct device
{
    WCHAR id[128];
    WCHAR name[128];          /* "Динаміки", or what the user renamed it to */
    WCHAR adapter[128];       /* "Realtek ALC892" */
    UINT icon;
    DWORD state;              /* DEVICE_STATE_* */
    BOOL is_default, is_default_comm;
    BOOL capture;
};

UINT devices_list( BOOL capture, struct device *list, UINT max );
BOOL device_get( const WCHAR *id, struct device *dev );
void device_set_default( const struct device *dev, BOOL communications_only );
void device_set_name( const struct device *dev, const WCHAR *name );
void device_set_enabled( const struct device *dev, BOOL enabled );

/* the volume of an endpoint, its channels and mute (devices.c) */
BOOL volume_get( const WCHAR *id, float *level, BOOL *mute, UINT *channels );
void volume_set( const WCHAR *id, float level );
void volume_set_mute( const WCHAR *id, BOOL mute );
float volume_channel( const WCHAR *id, UINT channel );
void volume_set_channel( const WCHAR *id, UINT channel, float level );

/* the format of an endpoint (devices.c) */
struct format_choice
{
    UINT channels, bits, rate;
};
BOOL format_get_mix( const WCHAR *id, struct format_choice *fmt );
BOOL format_get_chosen( const WCHAR *id, struct format_choice *fmt );
void format_choose( const WCHAR *id, const struct format_choice *fmt );   /* NULL: the device's own */

/* "Слухати": the microphone played on an output (sndvolsso.dll plays it) */
BOOL listen_get( const WCHAR *id, WCHAR *target, UINT count );
void listen_set( const WCHAR *id, BOOL on, const WCHAR *target );
BOOL format_test( const WCHAR *id, const struct format_choice *fmt );

/* the sheets */
void show_device_properties( HWND owner, const struct device *dev );
void show_balance( HWND owner, const struct device *dev );
INT_PTR CALLBACK devices_proc( HWND dlg, UINT msg, WPARAM wp, LPARAM lp );
INT_PTR CALLBACK sounds_proc( HWND dlg, UINT msg, WPARAM wp, LPARAM lp );
INT_PTR CALLBACK communications_proc( HWND dlg, UINT msg, WPARAM wp, LPARAM lp );

#endif
