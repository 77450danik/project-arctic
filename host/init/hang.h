/* When the NT world says it is stuck (a thread waited a minute, Ctrl+Alt+Del
 * pressed twice): where every one of its threads waits, written to
 * C:\Windows\Logs\Arctic\hang-*.txt while the session still hangs. */
#ifndef ARCTIC_HANG_H
#define ARCTIC_HANG_H

#include <sys/types.h>

extern void (*hang_log)(const char *fmt, ...);

/* the main loop: the lines nt.log got since the last call */
void hang_watch(int ntlog, const char *dir, uid_t nt_uid);

#endif
