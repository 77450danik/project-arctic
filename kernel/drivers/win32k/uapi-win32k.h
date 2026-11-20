/* SPDX-License-Identifier: LGPL-2.1-or-later WITH Linux-syscall-note */
/*
 * win32k: USER of the NT side in the kernel (drivers/win32k, docs/win32k.md).
 * Installed as include/uapi/linux/win32k.h.
 *
 * As in Windows, win32k is reached by system calls of its own, numbered from
 * 0x1000 (Windows' second service table, that of win32k.sys), not through a
 * device: the state of a process and of a thread hangs on the kernel's own
 * (task_struct, signal_struct), as Win32Process and Win32Thread do on
 * EPROCESS and KTHREAD, and goes when the thread ends.
 */
#ifndef __LINUX_WIN32K_H
#define __LINUX_WIN32K_H

#include <linux/types.h>

/* __NR_win32k_hello 0x1000 and __NR_win32k_call 0x1001: <asm/unistd.h> */

/* win32k_hello: the thread that calls, in NT terms; once per thread, before
 * its first win32k_call. ENOENT: win32k is not in use (wineserver did not
 * register: arctic.win32k=0), the requests of USER go to wineserver. */
struct win32k_hello {
	__u32 protocol;			/* SERVER_PROTOCOL_VERSION of ntdll */
	__u32 process_id;		/* NT ids, as wineserver gave them */
	__u32 thread_id;
	__u32 requests_size;		/* bytes at requests */
	__u64 requests;			/* out: a bit for each REQ_* that win32k serves */
};

/* win32k_call: one request of the Wine server protocol, of USER */
struct win32k_call {
	__u64 request;			/* the request, fixed part */
	__u64 request_data;		/* its variable part */
	__u32 request_size;
	__u32 request_data_size;
	__u64 reply;			/* the reply, fixed part */
	__u64 reply_data;		/* its variable part, room for reply_data_max bytes */
	__u32 reply_size;
	__u32 reply_data_max;
	__u32 reply_data_size;		/* out */
	__u32 status;			/* out: NTSTATUS */
	/* an open file of the caller that the request names by its NT handle, as
	 * ntdll found it, or -1: the driver's connection of set_queue_fd, the
	 * ntsync event of get_msg_queue_handle */
	__s32 fd;
	__u32 pad;
};

/*
 * win32k_callout (0x1003): what the object manager tells win32k, as Windows'
 * object manager and process manager call win32k's callouts (desktop and
 * window station opened and deleted, a thread's desktop, a process ending).
 * Window stations and desktops are NT objects of wineserver, with their names,
 * handles and security; their USER state (windows, hooks, input, clipboard,
 * atoms, monitors) is win32k's, keyed by the id wineserver gives the object.
 * Only the process that registered (wineserver) may call it.
 */
enum win32k_callout_code {
	W32_CALLOUT_REGISTER = 1,	/* value: SERVER_PROTOCOL_VERSION */
	W32_CALLOUT_THREAD_INIT,	/* thread, process: wineserver made it */
	W32_CALLOUT_THREAD_TASK,	/* thread, value: its unix tid (caller's pid namespace) */
	W32_CALLOUT_THREAD_GONE,	/* thread: its object went */
	W32_CALLOUT_PROCESS_INIT,	/* process, parent, value: machine */
	W32_CALLOUT_PROCESS_GONE,	/* process */
	W32_CALLOUT_PROCESS_SYSTEM,	/* process: made a system process */
	W32_CALLOUT_WINSTATION_CREATE,	/* object, flags */
	W32_CALLOUT_WINSTATION_DELETE,	/* object */
	W32_CALLOUT_WINSTATION_FLAGS,	/* object; set: flags → value: the old ones */
	W32_CALLOUT_DESKTOP_CREATE,	/* object, parent (winstation), flags, thread (creator), set: existed */
	W32_CALLOUT_DESKTOP_DELETE,	/* object */
	W32_CALLOUT_DESKTOP_FLAGS,	/* object; set: flags → value: the old ones */
	W32_CALLOUT_DESKTOP_LOCATOR,	/* object → locator */
	W32_CALLOUT_INPUT_DESKTOP,	/* parent (winstation) → object, or STATUS_ILLEGAL_FUNCTION */
	W32_CALLOUT_SET_INPUT_DESKTOP,	/* parent (winstation), object */
	W32_CALLOUT_PROCESS_WINSTATION,	/* process, object (0: none) */
	W32_CALLOUT_PROCESS_DESKTOP,	/* process, object (0: none): its default desktop */
	W32_CALLOUT_THREAD_DESKTOP,	/* thread, object, set: chosen by the thread (else a default
					 * one, if it has none) → locator */
	W32_CALLOUT_THREAD_DESKTOP_RELEASE,	/* thread, set: closed (else the thread stops
					 * being a user of it, as for a system process) */
	W32_CALLOUT_POST_MESSAGE,	/* object: window, flags: message, value: wparam,
					 * lparam (a socket's WSAAsyncSelect) */
};

struct win32k_callout {
	__u32 code;
	__u32 status;			/* out: NTSTATUS */
	__u32 thread;			/* NT ids */
	__u32 process;
	__u32 object;			/* of a window station or desktop: wineserver's id */
	__u32 parent;
	__u32 flags;
	__u32 set;
	__u64 value;			/* in/out, as the code says */
	__u64 lparam;
	__u64 locator_id;		/* out: obj_locator of the desktop's shared memory */
	__u64 locator_offset;
};

#endif
