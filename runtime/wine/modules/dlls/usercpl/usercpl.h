/*
 * User Accounts of the Control Panel (usercpl.dll)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __USERCPL_H
#define __USERCPL_H

#include "cpanel.h"

/* Windows' own (usercpl.dll.mui), by their numbers there */
#define IDS_USER_ACCOUNTS          1
#define IDS_USER_ACCOUNTS_TIP      2
#define IDS_PASSWORD_PROTECTED     37
#define IDS_LOCAL_ACCOUNT          47
#define IDS_ADMINISTRATOR          1101
#define IDS_TITLE                  1108
#define IDS_MANAGE_ACCOUNTS        1116
#define IDS_UAC_SETTINGS           1213

/* Arctic's own: what its User Account Control guards (docs/linux-gate.md) */
#define IDS_LINUX_GROUP            2000
#define IDS_LINUX_ABOUT            2001
#define IDS_LINUX_PASSWORD         2002
#define IDS_ONE_ACCOUNT            2003

#define IDI_USERS                  1

void users_build( struct view *view );

#endif
