/*
 * Network icon of the notification area (pnidui.dll)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __PNIDUI_H
#define __PNIDUI_H

#define IDS_NOT_CONNECTED        100
#define IDS_CONNECTIONS_AVAILABLE 101
#define IDS_INTERNET_ACCESS      102
#define IDS_WIRELESS_GROUP       103
#define IDS_WIRED_GROUP          104
#define IDS_CONNECTED            105
#define IDS_CONNECTING           106
#define IDS_CABLE_UNPLUGGED      107
#define IDS_CONNECT              108
#define IDS_DISCONNECT           109
#define IDS_OPEN_CONNECTIONS     110
#define IDS_CONNECT_FAILED       111
#define IDS_KEY_MISMATCH         112
#define IDS_NETWORK              113
#define IDS_NO_NETWORKS          114

#define IDD_NETWORK_KEY          200
#define IDC_KEY                  201
#define IDC_HIDE_KEY             202

void tray_start(void);
void tray_stop(void);

extern HINSTANCE pnidui_instance;

#endif
