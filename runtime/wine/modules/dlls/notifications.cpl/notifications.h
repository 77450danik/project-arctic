/*
 * Notifications applet (notifications.cpl)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __NOTIFICATIONS_H
#define __NOTIFICATIONS_H

#ifndef RC_INVOKED
#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "winreg.h"
#include "commctrl.h"
#endif

/* Where the settings are, as Windows keeps them; the shell (explorer's
 * notification area, ReactOS patch 0060) reads them for every notification
 * and registers there each program that sent one. A sender's key is its
 * program's path with '/' for '\'. */
#define PUSH_KEY        L"Software\\Microsoft\\Windows\\CurrentVersion\\PushNotifications"
#define PUSH_TOASTS     L"ToastEnabled"
#define SETTINGS_KEY    L"Software\\Microsoft\\Windows\\CurrentVersion\\Notifications\\Settings"
#define SETTINGS_SOUND  L"NOC_GLOBAL_SETTING_ALLOW_NOTIFICATION_SOUND"
#define FOCUS_KEY       L"Software\\Arctic\\FocusAssist"
#define FOCUS_MODE      L"Mode"          /* 0 off, 1 priority only, 2 alarms only */
#define FOCUS_FULLSCREEN L"FullScreen"   /* the same, while a program covers the screen */

#define TOGGLE_CLASS    L"ArcticToggle"

#define IDI_NOTIFICATIONS 1

#define IDS_NAME          1
#define IDS_INFO          2
#define IDS_CAPTION       3
#define IDS_CLOSE         4
#define IDS_ON            5
#define IDS_OFF           6
#define IDS_ON_BANNERS_SOUNDS 7
#define IDS_ON_BANNERS    8
#define IDS_ON_SOUNDS     9
#define IDS_ON_NOTHING    10
#define IDS_RECENT        11
#define IDS_BY_NAME       12
#define IDS_COLUMN_SENDER 13
#define IDS_COLUMN_STATE  14
#define IDS_5_SECONDS     15
#define IDS_7_SECONDS     16
#define IDS_15_SECONDS    17
#define IDS_30_SECONDS    18
#define IDS_1_MINUTE      19
#define IDS_5_MINUTES     20
#define IDS_PRIORITY_ONLY 21
#define IDS_ALARMS_ONLY   22
#define IDS_SENDER_CAPTION 23

#define IDD_GENERAL       100
#define IDD_SENDERS       101
#define IDD_FOCUS         102
#define IDD_SENDER        103

#define IDC_TITLE         1000
#define IDC_TOASTS_LABEL  1001
#define IDC_TOASTS        1002
#define IDC_SOUNDS        1003
#define IDC_DURATION_LABEL 1004
#define IDC_DURATION      1005
#define IDC_DURATION_TEXT 1006

#define IDC_SORT          1100
#define IDC_LIST          1101
#define IDC_EMPTY         1102
#define IDC_OPTIONS       1103
#define IDC_SENDERS_TEXT  1104

#define IDC_F_OFF         1200
#define IDC_F_PRIORITY    1201
#define IDC_F_ALARMS      1202
#define IDC_F_LIST        1203
#define IDC_F_FULLSCREEN  1204
#define IDC_F_FSMODE      1205
#define IDC_F_LIST_LABEL  1206

#define IDC_S_ICON        1300
#define IDC_S_NAME        1301
#define IDC_S_ENABLED     1302
#define IDC_S_BANNER      1303
#define IDC_S_SOUND       1304
#define IDC_S_TOP         1305
#define IDC_S_HIGH        1306
#define IDC_S_NORMAL      1307
#define IDC_S_LABEL       1308

#ifndef RC_INVOKED

extern HINSTANCE instance;

struct sender
{
    WCHAR key[MAX_PATH];           /* the subkey of SETTINGS_KEY */
    WCHAR name[128];
    WCHAR icon[MAX_PATH];
    DWORD enabled, banner, sound, rank, priority;
    ULONGLONG last;                /* FILETIME of its last notification */
};

/* Windows' ranks: top, high, normal */
#define RANK_TOP    99
#define RANK_HIGH   1
#define RANK_NORMAL 0

DWORD reg_dword( const WCHAR *path, const WCHAR *name, DWORD fallback );
void set_reg_dword( const WCHAR *path, const WCHAR *name, DWORD value );
UINT load_senders( struct sender **list, BOOL by_name );
void save_sender( const struct sender *sender );
void sender_path( const struct sender *sender, WCHAR *path, DWORD count );
const WCHAR *load_string( UINT id );
const WCHAR *sender_state( const struct sender *sender );
BOOL edit_sender( HWND owner, struct sender *sender );
void register_toggle(void);

INT_PTR CALLBACK general_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp );
INT_PTR CALLBACK senders_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp );
INT_PTR CALLBACK focus_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp );

#endif

#endif
