/*
 * Programs and Features of the Control Panel (programscpl.dll)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __PROGRAMSCPL_H
#define __PROGRAMSCPL_H

#include "cpanel.h"
#include "commctrl.h"

#define IDS_NAME            1
#define IDS_TIP             2
#define IDS_TITLE           10
#define IDS_UPDATES_TITLE   11
#define IDS_UNINSTALL       12
#define IDS_CHANGE          13
#define IDS_REPAIR          14
#define IDS_COL_NAME        15
#define IDS_COL_PUBLISHER   16
#define IDS_COL_DATE        17
#define IDS_COL_SIZE        18
#define IDS_COL_VERSION     19
#define IDS_UPDATES         20
#define IDS_INSTALL         21
#define IDS_INSTALL_TITLE   22
#define IDS_FILTER_INSTALLS 23
#define IDS_FILTER_PROGRAMS 24
#define IDS_FILTER_ALL      25
#define IDS_RUN_FAILED      26
#define IDS_TOTAL           27
#define IDS_NONE            28
#define IDS_PROGRAMS        29
#define IDS_PUBLISHER       40
#define IDS_VERSION         41
#define IDS_SUPPORT_LINK    42
#define IDS_HELP_LINK       43
#define IDS_UPDATE_LINK     44
#define IDS_SIZE            45
#define IDS_CONTACT         46
#define IDS_PHONE           47
#define IDS_README          48
#define IDS_COMMENTS        49
#define IDS_LOCATION        50
#define IDS_INSTALLED_ON    51
#define IDS_INSTALL_HINT    52
#define IDS_BROWSE_INSTALL  53

#define IDI_PROGRAMS        1

/* the cards' pictures: Windows 11's, the icon theme's when it has them */
#define ICON_PROGRAMS       101
#define ICON_INSTALL        102
#define ICON_UPDATES        103

enum { PAGE_PROGRAMS, PAGE_UPDATES, PAGE_COUNT };

/* a program of the Uninstall keys (programs.c), as Wine's appwiz.cpl read it,
 * with what Windows 7 shows of it besides */
struct app
{
    WCHAR *name, *publisher, *version, *contact, *help_link, *phone, *readme, *update_link, *about_link;
    WCHAR *comments, *location;
    WCHAR *uninstall, *modify, *repair;     /* command lines; NULL where it has none */
    WCHAR *icon;                            /* DisplayIcon without its index */
    int icon_index;
    DWORD size_kb;                          /* EstimatedSize */
    SYSTEMTIME installed;                   /* InstallDate, wYear 0 without one */
    BOOL update;                            /* an update of another program */
    HKEY root;                              /* where its key is */
    REGSAM view;
    WCHAR key[256];
};

struct apps
{
    struct app *list;
    UINT count;
};

void apps_load( struct apps *apps );
void apps_free( struct apps *apps );
/* runs its command; returns the process to wait for, NULL if it did not start */
HANDLE app_run( HWND owner, struct app *app, const WCHAR *command );

/* the pane under the list with what the chosen program says of itself (details.c) */
#define DETAILS_CLASS   L"ArcticProgramDetails"
#define DM_SETAPP       (WM_USER + 1)       /* lparam const struct app * or NULL */
void details_register(void);

void programs_build( struct view *view );
void updates_build( struct view *view );
BOOL programs_command( struct view *view, UINT id, UINT code, HWND control );

#endif
