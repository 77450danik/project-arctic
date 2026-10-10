/*
 * Sharing of folders, as Explorer shows it (ntshrui.dll)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __NTSHRUI_H
#define __NTSHRUI_H

#include <stdarg.h>
#include <stdlib.h>
#include <wchar.h>
#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "winreg.h"
#include "wingdi.h"
#include "commctrl.h"
#include "prsht.h"

/* the dialogs of Windows (ntshrui.dll, aclui.dll, objsel.dll), their numbers and control ids kept */
#define IDD_SHARING_PAGE        1000
#define IDD_ADVANCED            1009
#define IDD_NEW_SHARE           1018
#define IDD_WIZARD_PEOPLE       3002
#define IDD_WIZARD_DONE         3005
#define IDD_PERMISSIONS         103
#define IDD_SELECT_OBJECTS      237
#define IDD_OBJECT_TYPES        238
#define IDD_LOCATIONS           240
/* Arctic's own */
#define IDD_CACHING             5000
#define IDD_WIZARD              5001

/* the Sharing page */
#define IDC_PAGE_ICON           1021
#define IDC_PAGE_NAME           1058
#define IDC_PAGE_STATE          4407
#define IDC_PAGE_PATH           4406
#define IDC_PAGE_SHARE          1059
#define IDC_PAGE_ADVANCED       1060
#define IDC_PAGE_PASSWORD       1061

/* Advanced Sharing, New Share */
#define IDC_SHARE_THIS          1020
#define IDC_NAME_LABEL          1003
#define IDC_NAME_EDIT           1001
#define IDC_NAME_COMBO          1015
#define IDC_NAME_ADD            1017
#define IDC_NAME_REMOVE         1012
#define IDC_LIMIT_LABEL         1016
#define IDC_LIMIT               1011
#define IDC_LIMIT_SPIN          1010
#define IDC_COMMENT_LABEL       1004
#define IDC_COMMENT             1002
#define IDC_PERMISSIONS         1008
#define IDC_CACHING             1019
#define IDC_APPLY               1024
#define IDC_LIMIT_MAXIMUM       1006
#define IDC_LIMIT_ALLOW         1007

/* Permissions (aclui.dll) */
#define IDC_ACL_LIST            1000
#define IDC_ACL_ADD             1001
#define IDC_ACL_REMOVE          1002
#define IDC_ACL_CHECKS          1004
#define IDC_ACL_ADVANCED        1005
#define IDC_ACL_FOR             1008
#define IDC_ACL_ALLOW           1009
#define IDC_ACL_DENY            1010
#define IDC_ACL_ADVANCED_TEXT   6026
#define IDC_ACL_CHECK_FIRST     7000    /* allow full, change, read; deny full, change, read */

/* Select Users or Groups (objsel.dll) */
#define IDC_OBJECT_TYPES        231
#define IDC_OBJECT_TYPES_BUTTON 230
#define IDC_OBJECT_PLACE        233
#define IDC_OBJECT_PLACE_BUTTON 232
#define IDC_OBJECT_NAMES_LABEL  126
#define IDC_OBJECT_NAMES        214
#define IDC_OBJECT_CHECK        235
#define IDC_OBJECT_ADVANCED     236
#define IDC_OBJECT_TYPE_LIST    239
#define IDC_OBJECT_PLACE_TREE   241

/* Offline Settings */
#define IDC_CACHE_MANUAL        100
#define IDC_CACHE_NONE          101
#define IDC_CACHE_AUTO          102
#define IDC_CACHE_FAST          103

/* the sharing wizard */
#define IDC_WIZARD_TITLE        100
#define IDC_WIZARD_BAND         101
#define IDC_WIZARD_PLACE        102
#define IDC_WIZARD_INTRO        4004
#define IDC_WIZARD_WHO          4005
#define IDC_WIZARD_ADD          4015
#define IDC_WIZARD_PEOPLE       4006
#define IDC_WIZARD_HELP         4016
#define IDC_DONE_TEXT           4101
#define IDC_DONE_LIST           4102
#define IDC_DONE_ICON           4103
#define IDC_DONE_SLEEP          4104
#define IDC_DONE_SHARES         4105

/* the strings of ntshrui.dll, their numbers kept */
#define IDS_GIVE_ACCESS         103
#define IDS_READ                112
#define IDS_CHANGE              113
#define IDS_FULL_CONTROL        114
#define IDS_SHARE_PERMISSIONS   117
#define IDS_BAD_CHARACTERS      128
#define IDS_SHARING             3200
#define IDS_SHARED              3202
#define IDS_NOT_SHARED          3203
#define IDS_NETWORK_ACCESS      3204
#define IDS_CHOOSE_PEOPLE       3320
#define IDS_TYPE_A_NAME         3322
#define IDS_COLUMN_NAME         3324
#define IDS_COLUMN_LEVEL        3325
#define IDS_NOT_FOUND           3328
#define IDS_SHARE_BUTTON        3331
#define IDS_FOLDER_SHARED       3402
#define IDS_LINKS               3410
#define IDS_SHOW_SHARES         3487
#define IDS_SLEEP_NOTE          3488
#define IDS_DONE                3599
#define IDS_LEVEL_READ          3740
#define IDS_LEVEL_WRITE         3742
#define IDS_LEVEL_OWNER         3743
#define IDS_MAIL_SUBJECT        3762
#define IDS_MAIL_BODY           3766
#define IDS_NO_PASSWORD         5102
#define IDS_PASSWORD            5103
#define IDS_REMOVE_ACCESS       5104
#define IDS_SPECIFIC_PEOPLE     5107
#define IDS_ROOT_NOT_SHARED     5134
#define IDS_CANNOT_SHARE         5137
/* aclui.dll, objsel.dll and Arctic's own */
#define IDS_EVERYONE            200
#define IDS_PERMISSIONS_FOR     201
#define IDS_PERMISSIONS_OF      202
#define IDS_PERMISSIONS         203
#define IDS_DENY_WARNING        204
#define IDS_SECURITY            205
#define IDS_OBJECT_TYPES        210
#define IDS_OBJECT_NOT_FOUND    211
#define IDS_NAME_NOT_FOUND      212
#define IDS_TYPE_USERS          213
#define IDS_TYPE_GROUPS         214
#define IDS_TYPE_BUILTIN        215
#define IDS_NAME_TAKEN          220
#define IDS_NAME_EMPTY          221
#define IDS_REMOVE_LEVEL        222

enum { RIGHT_NONE, RIGHT_READ, RIGHT_CHANGE, RIGHT_FULL };
enum { CACHE_MANUAL, CACHE_AUTO, CACHE_VDO, CACHE_NONE };

#define MAX_LIMIT 20    /* as many as Windows lets in at once */

/* a share, as its file holds it under C:\ProgramData\Arctic\Smb\shares
 * (host/init/arctic-smbd.c, docs/M5-network.md) */
struct share
{
    WCHAR name[81];
    WCHAR comment[257];
    int   allow, deny;      /* what Everyone may and may not */
    UINT  limit;
    int   caching;
};

extern HINSTANCE ntshrui_instance;

/* shares.c */
extern UINT shares_of_folder( const WCHAR *folder, struct share *list, UINT max );
extern BOOL share_owner( const WCHAR *name, WCHAR *folder, UINT count );
extern BOOL share_write( const WCHAR *folder, const struct share *share );
extern void share_delete( const WCHAR *name );
extern const WCHAR *share_bad_characters( const WCHAR *name );
extern BOOL password_protected(void);
extern void file_sharing_on(void);

/* page.c */
extern HPROPSHEETPAGE sharing_page_create( const WCHAR *folder );
extern BOOL advanced_sharing( HWND owner, const WCHAR *folder );
extern WCHAR *load_text( UINT id );
extern void message( HWND owner, UINT id, const WCHAR *arg, UINT flags );
extern void folder_title( const WCHAR *folder, WCHAR *title, UINT count );
extern void share_default( const WCHAR *folder, struct share *share );

/* perms.c */
extern BOOL edit_permissions( HWND owner, struct share *share );
extern BOOL pick_everyone( HWND owner );

/* wizard.c */
extern void sharing_wizard( HWND owner, const WCHAR *folder );
extern void stop_sharing( HWND owner, const WCHAR *folder );

#endif
