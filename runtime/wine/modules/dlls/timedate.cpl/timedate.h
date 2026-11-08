/*
 * Date and Time of the Control Panel (timedate.cpl)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __TIMEDATE_H
#define __TIMEDATE_H

#include "cpanel.h"

#define IDI_TIMEDATE            1

#define IDS_NAME                1
#define IDS_TIP                 2
#define IDS_TITLE               3
#define IDS_TIME_GROUP          4
#define IDS_AUTO_TIME           5
#define IDS_AUTO_TIME_HINT      6
#define IDS_SERVER              7
#define IDS_SYNC_NOW            8
#define IDS_SYNCED              9
#define IDS_NEVER_SYNCED        10
#define IDS_SYNCING             11
#define IDS_SYNC_FAILED         12
#define IDS_SYNC_OFF            13
#define IDS_MANUAL_GROUP        14
#define IDS_CHANGE              15
#define IDS_MANUAL_HINT         16
#define IDS_ZONE_GROUP          17
#define IDS_AUTO_ZONE           18
#define IDS_AUTO_ZONE_HINT      19
#define IDS_DST_START           20
#define IDS_DST_END             21
#define IDS_NO_DST              22
#define IDS_FORWARD             23
#define IDS_BACK                24
#define IDS_NOTIFICATIONS       25
#define IDS_SYSTEM              26
#define IDS_ZONE_FAILED         27
#define IDS_TIME_SET            28
#define IDS_TIME_FAILED         29
#define IDS_ZONE_FINDING        30
#define IDS_SYNCED_NOW          31
#define IDS_HOUR                32
#define IDS_HOURS               33

#define IDC_AUTO_TIME           100
#define IDC_SERVER              101
#define IDC_SYNC                102
#define IDC_DATE                103
#define IDC_TIME                104
#define IDC_SET                 105
#define IDC_AUTO_ZONE           106
#define IDC_ZONE                107

void datetime_build( struct view *view );
BOOL datetime_command( struct view *view, UINT id, UINT code, HWND control );
void datetime_timer( struct view *view );

#endif
