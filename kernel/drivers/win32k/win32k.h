/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * win32k: USER of the NT side in the kernel, as in Windows (docs/win32k.md).
 *
 * The handlers of Wine's server for windows, queues, hooks, classes, atoms,
 * desktops and the clipboard run here, in the thread of the program that
 * calls them, under one lock for the session as win32k's UserLock. The
 * requests and replies are those of the Wine server protocol (protocol.h).
 */
#ifndef WIN32K_PRIVATE_H
#define WIN32K_PRIVATE_H

#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <uapi/linux/win32k.h>
#include "protocol.h"
#include "wine_list.h"
#include "object.h"

struct desktop;
struct winstation;
struct msg_queue;
struct rawinput_device;

/* W32PROCESS: found by the thread group (main.c), as Win32Process in EPROCESS.
 * Besides its own, the fields USER had in wineserver's struct process. */
struct w32_process
{
	struct object obj;		/* referenced as wineserver references a process */
	process_id_t id;		/* the NT process id */
	process_id_t parent_id;		/* the NT id of the process that made it */
	unsigned short machine;		/* IMAGE_FILE_MACHINE_* of the process */
	unsigned int is_system:1;	/* a system process (wineserver says so) */
	unsigned int exited:1;		/* USER let go of what it left */
	int suspend;			/* suspended for a debugger (wineserver says so) */
	unsigned int threads;		/* its W32THREADs that have not ended */
	unsigned int tasks;		/* its threads that have a W32THREAD on their task */
	struct list thread_list;	/* its W32THREADs that have not ended */
	struct list classes;		/* its window classes (class.c), local ones first */
	struct winstation *winstation;	/* its window station (USER body), referenced */
	unsigned int winstation_access;	/* granted to its handle, as wineserver checked it */
	struct desktop *desktop;	/* the desktop its new threads get, referenced */
	unsigned int desktop_access;
	struct rawinput_device *rawinput_devices;	/* registered raw input devices */
	unsigned int rawinput_device_count;
	const struct rawinput_device *rawinput_mouse;	/* raw input mouse device, if any */
	const struct rawinput_device *rawinput_kbd;	/* raw input keyboard device, if any */
	struct list rawinput_entry;	/* in the list of processes with raw input */
};

/* W32THREAD: found by the task (main.c), as Win32Thread in KTHREAD, with the
 * fields USER had in wineserver's struct thread */
struct w32_thread
{
	struct object obj;		/* referenced as wineserver references a thread */
	struct w32_process *process;	/* holds a reference */
	thread_id_t id;			/* the NT thread id */
	struct pid *pid;		/* its task, as wineserver said (NULL: not said) */
	unsigned int exited:1;		/* USER let go of what it left */
	unsigned int on_task:1;		/* hangs on its task_struct */
	struct list proc_entry;		/* in its process's thread_list */
	struct list desktop_entry;	/* in its desktop's threads */
	struct msg_queue *queue;	/* its message queue */
	struct desktop *desktop;	/* its desktop (USER body), referenced */
	unsigned int desktop_access;	/* granted to its handle, as wineserver checked it */
	int desktop_users;		/* objects using the thread's desktop */
};

/* the call being served: what the handlers of the Wine server call current,
 * and the request's data */
struct w32_call
{
	struct w32_thread *thread;
	const void *req_data;		/* the variable part of the request */
	u32 req_data_size;
	void *reply_data;		/* ... of the reply */
	u32 reply_data_max, reply_data_size;
	u32 error;			/* NTSTATUS, as set_error() sets it */
	int fd;				/* struct win32k_call's fd: the caller's file, or -1 */
};

extern struct mutex win32k_lock;	/* the UserLock */

/* a handler of a request: DECL_HANDLER (compat.h) makes one of each */
typedef void (*w32_handler)(const void *req, void *reply);

struct w32_handler_entry
{
	w32_handler handler;
	u32 req_size, reply_size;	/* of the fixed parts */
};

/* REQ_* of protocol.h → handler; NULL: not a request of USER */
extern const struct w32_handler_entry win32k_handlers[REQ_NB_REQUESTS];

/* the W32THREAD of a task that said win32k_hello, or NULL (main.c); for the
 * calling task itself, no lock needed */
struct w32_thread *w32_task_thread(struct task_struct *tsk);

/* a thread or a process by its NT id, referenced; NULL and STATUS_INVALID_CID
 * if there is none (main.c), as wineserver's get_thread_from_id */
struct w32_thread *get_thread_from_id(thread_id_t id);
struct w32_process *get_process_from_id(process_id_t id);

/* USER's side of a process and a thread starting and ending (user.c) */
void user_process_init(struct w32_process *process);
void user_process_exit(struct w32_process *process);
void user_thread_exit(struct w32_thread *thread);

/* the callouts of wineserver about window stations and desktops (winstation.c) */
void winstation_callout(struct win32k_callout *co, struct w32_thread *thread,
			struct w32_process *process);
void winstation_reset(void);

#endif
