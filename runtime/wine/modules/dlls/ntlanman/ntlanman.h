/*
 * Microsoft Windows Network, the network provider (ntlanman.dll)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __NTLANMAN_H
#define __NTLANMAN_H

#include <stdarg.h>
#include <stdlib.h>
#include "windef.h"
#include "winbase.h"
#include "winsock2.h"
#include "ws2tcpip.h"
#include "winuser.h"
#include "winreg.h"
#include "winnetwk.h"
#include "npapi.h"

#define IDS_PROVIDER            1
#define IDS_CONNECT_TO          2
#define IDS_ENTER_PASSWORD      3
#define IDS_MAP_TITLE           4
#define IDS_NO_FOLDER           5
#define IDS_CANNOT_CONNECT      6
#define IDS_BROWSE_TITLE        7
#define IDS_DISCONNECT_NONE     8
#define IDS_NOT_FOUND           9
#define IDS_NETWORK_DRIVE       10
#define IDS_VERB_MAP            20
#define IDS_VERB_DISCONNECT     21
#define IDS_FORGET_ASK          30
#define IDS_FORGET_NONE         31
#define IDS_FORGET_TITLE        32

#define IDD_MAP_DRIVE           100
#define IDD_DISCONNECT          101

#define IDC_DRIVE               1001
#define IDC_FOLDER              1002
#define IDC_BROWSE              1003
#define IDC_RECONNECT           1004
#define IDC_OTHER_USER          1005
#define IDC_DRIVES              1006
#define IDC_BANNER              1007

extern HINSTANCE ntlanman_instance;

/* a computer of the network, as the neighbours' answers describe it */
struct computer
{
    WCHAR name[64];
    WCHAR comment[128];
    char  address[46];
};

struct share
{
    WCHAR name[84];
    WCHAR remark[260];
    DWORD type;             /* STYPE_* of lmshare.h */
};

struct logon
{
    WCHAR user[260];        /* "" is nobody: a null session */
    WCHAR domain[260];
    WCHAR password[260];
};

/* discover.c */
extern UINT discover_computers( struct computer **list, BOOL fresh );
extern BOOL discovery_enabled(void);
extern BOOL discovered_address( const WCHAR *server, char *address, UINT size );

/* smb2.c */
extern DWORD smb_enum_shares( const WCHAR *server, const char *address, const struct logon *logon,
                              struct share **shares, UINT *count );

/* main.c: the host's service of the network folders (arctic-smb) */
extern DWORD host_command( const char *command, const WCHAR *a, const WCHAR *b, const WCHAR *c, const WCHAR *d,
                           char *reply, UINT size );
extern BOOL host_resolve( const WCHAR *server, char *address, UINT size );
extern void host_tell_address( const WCHAR *server, const char *address );
extern BOOL host_get_logon( const WCHAR *server, struct logon *logon );
extern void host_set_logon( const WCHAR *server, const struct logon *logon );
extern DWORD list_shares( const WCHAR *server, struct share **shares, UINT *count );
extern DWORD connect_resource( HWND owner, const WCHAR *remote, const WCHAR *local, const WCHAR *user,
                               const WCHAR *password, DWORD flags );
extern BOOL split_unc( const WCHAR *path, WCHAR *server, UINT server_count, WCHAR *share, UINT share_count,
                       const WCHAR **rest );
extern BOOL drive_remote( WCHAR letter, WCHAR *remote, UINT count );
extern DWORD drive_define( WCHAR letter, const WCHAR *server, const WCHAR *share );
extern DWORD drive_remove( WCHAR letter );

/* dialogs.c */
extern BOOL ask_logon( HWND owner, const WCHAR *server, struct logon *logon, BOOL wrong, BOOL *save );
extern void save_logon( const WCHAR *server, const struct logon *logon );
extern BOOL saved_logon( const WCHAR *server, struct logon *logon );
extern void push_saved_logons(void);
extern void forget_profile( WCHAR letter );

#endif
