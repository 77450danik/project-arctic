/*
 * Windows Logon Application
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINLOGON_H
#define __WINLOGON_H

#define IDS_COMMAND_PROMPT 1
#define IDS_TASK_MANAGER   2
#define IDS_RESTART        3
#define IDS_SHUT_DOWN      4
#define IDS_CANCEL         5

/* exit codes: how the session ended, for wininit.exe and arctic-init */
#define WINLOGON_EXIT_RESTART  3
#define WINLOGON_EXIT_SHUTDOWN 4
#define WINLOGON_EXIT_LOGOFF   0 /* the session starts again, the machine stays on */

#ifndef RC_INVOKED
void run( WCHAR *cmdline );
void run_userinit(void);
void end_session( int exit_code );
void show_security_options(void);
#endif

#endif
