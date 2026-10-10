/*
 * System of the Control Panel (systemcpl.dll)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __SYSTEMCPL_H
#define __SYSTEMCPL_H

#include "cpanel.h"

/* Windows' own (systemcpl.dll.mui), by their numbers there */
#define IDS_SYSTEM                 1
#define IDS_SYSTEM_TIP             2
#define IDS_RATING_UNAVAILABLE     5
#define IDS_WORKGROUP_LABEL        7
#define IDS_WORKGROUP_DEFAULT      8
#define IDS_PEN_TOUCH              26
#define IDS_PROCESSOR_SPEED        29
#define IDS_PROCESSORS_SPEED       30
#define IDS_MEMORY_AVAILABLE       45
#define IDS_NO_PEN_TOUCH           50
#define IDS_PEN                    51
#define IDS_PEN_TOUCH_POINTS       52
#define IDS_PEN_SINGLE_TOUCH       53
#define IDS_TOUCH_POINTS           54
#define IDS_SINGLE_TOUCH           55
#define IDS_TYPE_32_X86            58
#define IDS_TYPE_32_X64            59
#define IDS_TYPE_64_X64            60
#define IDS_TYPE_32_ARM            61
#define IDS_TYPE_64_ARM            62
#define IDS_DEVICE_MANAGER         100
#define IDS_REMOTE_SETTINGS        101
#define IDS_SYSTEM_PROTECTION      102
#define IDS_ADVANCED_SETTINGS      103
#define IDS_WINDOWS_UPDATE         104
#define IDS_ACTION_CENTER          105
#define IDS_TITLE                  1537
#define IDS_EDITION                1538
#define IDS_SYSTEM_GROUP           1542
#define IDS_NAME_GROUP             1545
#define IDS_CHANGE_SETTINGS        1546
#define IDS_COMPUTER_NAME          1547
#define IDS_FULL_NAME              1549
#define IDS_DESCRIPTION            1551
#define IDS_ACTIVATION             1555
#define IDS_PRODUCT_ID             1557
#define IDS_UNAVAILABLE            1558
#define IDS_PROCESSOR              1562
#define IDS_MEMORY                 1564
#define IDS_SYSTEM_TYPE            1571

/* Windows 7's, which Windows 10 has no more, and Arctic's own */
#define IDS_RATING                 2000
#define IDS_COPYRIGHT              2001
#define IDS_BUILD                  2002
#define IDS_ACTIVATION_NOT_NEEDED  2003
#define IDS_PERFORMANCE_TOOLS      2004
#define IDS_GHZ                    2005

#define IDB_LOGO_100               100
#define IDB_LOGO_125               101
#define IDB_LOGO_150               102
#define IDB_LOGO_200               103

#define IDI_SYSTEM                 1

void system_build( struct view *view );

#endif
