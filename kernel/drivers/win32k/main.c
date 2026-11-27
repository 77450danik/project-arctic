// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * win32k: its system calls, and the processes and threads that make them.
 *
 * W32PROCESS and W32THREAD are made when wineserver, the object manager, says
 * a process or thread was made (win32k_callout), as Windows' process manager
 * calls win32k's process and thread callouts; they are found by NT id. A
 * thread says who it is (win32k_hello) before its first call: its W32THREAD
 * is then found by its task (pid) and its process's W32PROCESS by the thread
 * group (tgid), as Win32Thread and Win32Process hang on KTHREAD and EPROCESS
 * in Windows. In tables of win32k's own, not fields of task_struct: the
 * kernel's structures stay as they are, so the rest of the kernel and its
 * modules are not built again for win32k (a lookup is a few nanoseconds).
 *
 * A call is one request of the Wine server protocol: its fixed part and its
 * data are copied in, the handler runs in the calling thread under
 * win32k_lock, as win32k's handlers run under UserLock, and the reply goes
 * back the same way.
 *
 * A thread ends once: in do_exit, which calls win32k_thread_exit (Windows'
 * thread callout), or when wineserver says its object went, whichever comes
 * first; USER frees what it left (windows, hooks, queue, desktop), and with
 * the last thread of the process what the process left.
 */

#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/xarray.h>
#include <linux/win32k.h>
#include <linux/ntos.h>
#include "user.h"

DEFINE_MUTEX(win32k_lock);
struct w32_call *w32_call;	/* the call being served, under win32k_lock */

/* NT id → W32THREAD / W32PROCESS that have not ended; each holds a reference */
static DEFINE_XARRAY(w32_threads);
static DEFINE_XARRAY(w32_processes);

/* task pid → its W32THREAD, thread group id → its W32PROCESS, from
 * win32k_hello to the task's do_exit; each holds a reference. Only the task
 * itself adds or removes its own entry, so it reads it without the lock. */
static DEFINE_XARRAY(w32_tasks);
static DEFINE_XARRAY(w32_groups);

struct w32_thread *w32_task_thread(struct task_struct *tsk)
{
	return xa_load(&w32_tasks, tsk->pid);
}

/* wineserver's thread group, once it registered (win32k_callout); or ntos,
 * the object manager in the kernel itself (docs/ntos.md) */
static struct pid *object_manager;
static bool ntos_object_manager;

static void w32_process_destroy(struct object *obj)
{
}

static void w32_thread_destroy(struct object *obj)
{
	struct w32_thread *thread = container_of(obj, struct w32_thread, obj);

	put_pid(thread->pid);
	release_object(thread->process);
}

static const struct object_ops w32_process_ops = {
	.size = sizeof(struct w32_process),
	.destroy = w32_process_destroy,
};

static const struct object_ops w32_thread_ops = {
	.size = sizeof(struct w32_thread),
	.destroy = w32_thread_destroy,
};

struct w32_thread *get_thread_from_id(thread_id_t id)
{
	struct w32_thread *thread = xa_load(&w32_threads, id);

	if (!thread) {
		set_error(STATUS_INVALID_CID);
		return NULL;
	}
	grab_object(thread);
	return thread;
}

struct w32_process *get_process_from_id(process_id_t id)
{
	struct w32_process *process = xa_load(&w32_processes, id);

	if (!process) {
		set_error(STATUS_INVALID_CID);
		return NULL;
	}
	grab_object(process);
	return process;
}

/* the W32PROCESS of an NT id, made if there is none */
static struct w32_process *w32_get_process(process_id_t id)
{
	struct w32_process *process = xa_load(&w32_processes, id);

	if (process)
		return process;
	if (!(process = kzalloc(sizeof(*process), GFP_KERNEL)))
		return NULL;
	process->obj.refcount = 1;	/* the table's */
	process->obj.ops = &w32_process_ops;
	process->id = id;
	list_init(&process->thread_list);
	list_init(&process->classes);
	list_init(&process->rawinput_entry);
	if (xa_err(xa_store(&w32_processes, id, process, GFP_KERNEL))) {
		kfree(process);
		return NULL;
	}
	user_process_init(process);
	return process;
}

static void w32_process_end(struct w32_process *process)
{
	if (process->exited)
		return;
	process->exited = 1;
	user_process_exit(process);
	if (xa_load(&w32_processes, process->id) == process) {
		xa_erase(&w32_processes, process->id);
		release_object(process);	/* the table's */
	}
}

static struct w32_thread *w32_new_thread(thread_id_t id, struct w32_process *process)
{
	struct w32_thread *thread;

	if (!(thread = kzalloc(sizeof(*thread), GFP_KERNEL)))
		return NULL;
	thread->obj.refcount = 1;		/* the table's */
	thread->obj.ops = &w32_thread_ops;
	thread->process = (struct w32_process *)grab_object(process);
	thread->id = id;
	list_init(&thread->desktop_entry);
	if (xa_err(xa_store(&w32_threads, id, thread, GFP_KERNEL))) {
		release_object(thread);
		return NULL;
	}
	wlist_add_tail(&process->thread_list, &thread->proc_entry);
	process->threads++;
	/* the process's default desktop, as wineserver's create_thread gives it */
	if (process->desktop) {
		struct win32k_callout co = { .code = W32_CALLOUT_THREAD_DESKTOP,
					     .object = process->desktop->id,
					     .value = process->desktop_access };
		winstation_callout(&co, thread, process);
	}
	return thread;
}

/* the thread ended: what USER kept for it goes (wineserver's thread cleanup) */
static void w32_thread_end(struct w32_thread *thread)
{
	struct w32_process *process = thread->process;

	if (thread->exited)
		return;
	thread->exited = 1;
	user_thread_exit(thread);
	list_remove(&thread->proc_entry);
	if (!--process->threads)
		w32_process_end(process);	/* the last one: as process_killed */
	if (xa_load(&w32_threads, thread->id) == thread) {
		xa_erase(&w32_threads, thread->id);
		release_object(thread);		/* the table's */
	}
}

/* under win32k_lock: serve as a call of thread, what the handlers see as
 * current, and where errors go */
static void w32_enter(struct w32_call *call, struct w32_thread *thread)
{
	memset(call, 0, sizeof(*call));
	call->thread = thread;
	call->fd = -1;
	w32_call = call;
}

static void w32_leave(void)
{
	w32_call = NULL;
}

/* do_exit of every task, under no lock of ours: for one with a W32THREAD,
 * Windows' thread callout */
void win32k_thread_exit(struct task_struct *tsk)
{
	struct w32_thread *thread;
	struct w32_process *process;
	struct w32_call call;

	if (!(thread = xa_load(&w32_tasks, tsk->pid)))
		return;
	mutex_lock(&win32k_lock);
	xa_erase(&w32_tasks, tsk->pid);
	thread->on_task = 0;
	w32_enter(&call, thread);
	w32_thread_end(thread);
	w32_leave();
	process = thread->process;
	if (!--process->tasks && xa_load(&w32_groups, tsk->tgid) == process) {
		xa_erase(&w32_groups, tsk->tgid);
		release_object(process);	/* the thread group's */
	}
	release_object(thread);			/* the task's */
	mutex_unlock(&win32k_lock);
}

/* the requests win32k serves, a bit each, for ntdll to send them here */
static long w32_put_requests(const struct win32k_hello *hello)
{
	u8 bits[DIV_ROUND_UP(REQ_NB_REQUESTS, 8)] = {0};
	int req;

	if (hello->requests_size < sizeof(bits))
		return -EINVAL;
	for (req = 0; req < REQ_NB_REQUESTS; req++)
		if (win32k_handlers[req].handler)
			bits[req / 8] |= 1 << (req % 8);
	if (copy_to_user(u64_to_user_ptr(hello->requests), bits, sizeof(bits)))
		return -EFAULT;
	return 0;
}

SYSCALL_DEFINE1(win32k_hello, const struct win32k_hello __user *, arg)
{
	struct win32k_hello hello;
	struct w32_process *process;
	struct w32_process *group;
	struct w32_thread *thread;
	long ret = 0;

	if (copy_from_user(&hello, arg, sizeof(hello)))
		return -EFAULT;
	if (hello.protocol != SERVER_PROTOCOL_VERSION)
		return -EPROTO;

	mutex_lock(&win32k_lock);
	if ((thread = w32_task_thread(current))) {
		if (thread->id != hello.thread_id)
			ret = -EINVAL;
		goto out;
	}
	if (!ntos_object_manager && (!object_manager || !pid_task(object_manager, PIDTYPE_TGID))) {
		ret = -ENOENT;	/* no wineserver uses win32k: USER stays with wineserver */
		goto out;
	}
	/* made when wineserver made the thread: it must be this task */
	if (!(thread = xa_load(&w32_threads, hello.thread_id))) {
		ret = -ESRCH;
		goto out;
	}
	if (thread->on_task || thread->exited || thread->pid != task_pid(current)) {
		ret = -EPERM;
		goto out;
	}
	process = thread->process;
	if (process->id != hello.process_id) {
		ret = -EINVAL;
		goto out;
	}
	group = xa_load(&w32_groups, current->tgid);
	if (group && group != process) {
		ret = -EINVAL;
		goto out;
	}
	if (xa_err(xa_store(&w32_tasks, current->pid, thread, GFP_KERNEL))) {
		ret = -ENOMEM;
		goto out;
	}
	if (!group && xa_err(xa_store(&w32_groups, current->tgid, process, GFP_KERNEL))) {
		xa_erase(&w32_tasks, current->pid);
		ret = -ENOMEM;
		goto out;
	}
	if (!group)
		grab_object(process);		/* the thread group's */
	grab_object(thread);			/* the task's */
	process->tasks++;
	thread->on_task = 1;
out:
	mutex_unlock(&win32k_lock);
	if (!ret)
		ret = w32_put_requests(&hello);
	return ret;
}

SYSCALL_DEFINE1(win32k_call, struct win32k_call __user *, arg)
{
	union { struct request_header header; struct request_max_size max; } req;
	union { struct reply_header header; struct request_max_size max; } reply;
	const struct w32_handler_entry *entry;
	struct win32k_call args;
	struct w32_call call;
	void *req_data = NULL, *reply_data = NULL;
	struct w32_thread *thread;
	long ret = 0;

	if (!(thread = w32_task_thread(current)))
		return -ESRCH;	/* win32k_hello first */
	if (copy_from_user(&args, arg, sizeof(args)))
		return -EFAULT;
	/* the data of any size, as wineserver took it: a screenshot on the
	 * clipboard is 8 MB at 1920x1080 */
	if (args.request_size > sizeof(req) || args.reply_size > sizeof(reply))
		return -EINVAL;
	memset(&req, 0, sizeof(req));
	memset(&reply, 0, sizeof(reply));
	if (copy_from_user(&req, u64_to_user_ptr(args.request), args.request_size))
		return -EFAULT;
	if (req.header.req < 0 || req.header.req >= REQ_NB_REQUESTS)
		return -EINVAL;
	entry = &win32k_handlers[req.header.req];
	if (!entry->handler)
		return -ENOSYS;	/* not a request of USER */

	if (args.request_data_size) {
		if (!(req_data = kvmalloc(args.request_data_size, GFP_KERNEL | __GFP_NOWARN)))
			goto no_memory;
		if (copy_from_user(req_data, u64_to_user_ptr(args.request_data), args.request_data_size)) {
			ret = -EFAULT;
			goto out;
		}
	}
	if (args.reply_data_max && !(reply_data = kvzalloc(args.reply_data_max, GFP_KERNEL | __GFP_NOWARN)))
		goto no_memory;

	mutex_lock(&win32k_lock);
	w32_enter(&call, thread);
	call.req_data = req_data;
	call.req_data_size = args.request_data_size;
	call.reply_data = reply_data;
	call.reply_data_max = args.reply_data_max;
	call.fd = args.fd;
	if (thread->exited)
		call.error = STATUS_THREAD_IS_TERMINATING;	/* wineserver said it went */
	else
		entry->handler(&req, &reply);
	w32_leave();
	mutex_unlock(&win32k_lock);

	reply.header.error = call.error;
	reply.header.reply_size = call.reply_data_size;
	args.status = call.error;
	args.reply_data_size = call.reply_data_size;
	if (copy_to_user(u64_to_user_ptr(args.reply), &reply, min_t(u32, args.reply_size, entry->reply_size)) ||
	    (call.reply_data_size &&
	     copy_to_user(u64_to_user_ptr(args.reply_data), reply_data, call.reply_data_size)) ||
	    copy_to_user(arg, &args, sizeof(args)))
		ret = -EFAULT;
out:
	kvfree(req_data);
	kvfree(reply_data);
	return ret;

no_memory:
	/* the call fails with an error, as wineserver answered: not the thread */
	reply.header.error = STATUS_NO_MEMORY;
	args.status = STATUS_NO_MEMORY;
	args.reply_data_size = 0;
	if (copy_to_user(u64_to_user_ptr(args.reply), &reply, min_t(u32, args.reply_size, entry->reply_size)) ||
	    copy_to_user(arg, &args, sizeof(args)))
		ret = -EFAULT;
	goto out;
}

/* what the object manager says: under win32k_lock */
static void w32_callout(struct win32k_callout *co)
{
	struct w32_process *process = NULL;
	struct w32_thread *thread = NULL;

	switch (co->code) {
	case W32_CALLOUT_PROCESS_INIT:
		/* made, or said again once its machine is known (new_process) */
		if (!(process = w32_get_process(co->process))) {
			set_error(STATUS_NO_MEMORY);
			return;
		}
		process->parent_id = co->parent;
		process->machine = co->value;
		return;

	case W32_CALLOUT_PROCESS_GONE:
		if ((process = xa_load(&w32_processes, co->process)))
			w32_process_end(process);
		return;

	case W32_CALLOUT_THREAD_INIT:
		if ((thread = xa_load(&w32_threads, co->thread)))
			w32_thread_end(thread);	/* an id given again: the old one went */
		if (!(process = w32_get_process(co->process)) ||
		    !(thread = w32_new_thread(co->thread, process)))
			set_error(STATUS_NO_MEMORY);
		return;

	case W32_CALLOUT_THREAD_TASK:
		if (!(thread = xa_load(&w32_threads, co->thread)) || thread->on_task) {
			set_error(STATUS_INVALID_CID);
			return;
		}
		put_pid(thread->pid);
		thread->pid = find_get_pid((pid_t)co->value);	/* in wineserver's pid namespace */
		return;

	case W32_CALLOUT_POST_MESSAGE:
		post_message(co->object, co->flags, co->value, co->lparam);
		return;

	case W32_CALLOUT_THREAD_GONE:
		if ((thread = xa_load(&w32_threads, co->thread))) {
			w32_call->thread = thread;
			w32_thread_end(thread);
		}
		return;

	case W32_CALLOUT_PROCESS_SYSTEM:
	case W32_CALLOUT_PROCESS_WINSTATION:
	case W32_CALLOUT_PROCESS_DESKTOP:
		/* a process that ended has nothing left to change */
		if (!(process = xa_load(&w32_processes, co->process)))
			return;
		break;

	case W32_CALLOUT_THREAD_DESKTOP:
	case W32_CALLOUT_THREAD_DESKTOP_RELEASE:
		if (!(thread = xa_load(&w32_threads, co->thread)))
			return;	/* it ended: nothing left to change */
		process = thread->process;
		break;

	case W32_CALLOUT_DESKTOP_CREATE:
		thread = co->thread ? xa_load(&w32_threads, co->thread) : NULL;	/* the creator */
		break;

	default:
		break;
	}
	w32_call->thread = thread;
	winstation_callout(co, thread, process);
}

/* a new wineserver took over: the threads and processes of the last one that
 * are not alive in a task end, and so do its window stations and desktops */
static void w32_reset(void)
{
	struct w32_process *process;
	struct w32_thread *thread;
	struct w32_call call;
	unsigned long id;

	w32_enter(&call, NULL);
	xa_for_each(&w32_threads, id, thread) {
		if (thread->on_task)
			continue;
		call.thread = thread;
		w32_thread_end(thread);
	}
	call.thread = NULL;
	xa_for_each(&w32_processes, id, process)
		if (!process->tasks)
			w32_process_end(process);
	winstation_reset();
	w32_leave();
}

SYSCALL_DEFINE1(win32k_callout, struct win32k_callout __user *, arg)
{
	struct win32k_callout co;
	struct w32_call call;
	long ret = 0;

	if (copy_from_user(&co, arg, sizeof(co)))
		return -EFAULT;

	mutex_lock(&win32k_lock);
	if (ntos_object_manager) {
		mutex_unlock(&win32k_lock);
		return -EPERM;	/* ntos is the object manager: no wineserver */
	}
	if (co.code == W32_CALLOUT_REGISTER) {
		if (co.value != SERVER_PROTOCOL_VERSION) {
			ret = -EPROTO;
		} else if (object_manager && pid_task(object_manager, PIDTYPE_TGID) &&
			   object_manager != task_tgid(current)) {
			ret = -EBUSY;	/* another wineserver is alive */
		} else {
			if (object_manager && object_manager != task_tgid(current))
				w32_reset();	/* what the last wineserver left */
			put_pid(object_manager);
			object_manager = get_pid(task_tgid(current));
			pr_info("win32k: USER served here, wineserver %d is the object manager\n",
				task_tgid_nr(current));
		}
		mutex_unlock(&win32k_lock);
		return ret;
	}
	if (!object_manager || object_manager != task_tgid(current)) {
		mutex_unlock(&win32k_lock);
		return -EPERM;
	}
	w32_enter(&call, NULL);
	w32_callout(&co);
	co.status = call.error;
	w32_leave();
	mutex_unlock(&win32k_lock);

	if (copy_to_user(arg, &co, sizeof(co)))
		return -EFAULT;
	return 0;
}

/* ntos (docs/ntos.md): the object manager is in the kernel; what the last
 * wineserver left, if one ran, goes */
void win32k_ntos_register(void)
{
	mutex_lock(&win32k_lock);
	if (object_manager)
		w32_reset();
	put_pid(object_manager);
	object_manager = NULL;
	ntos_object_manager = true;
	mutex_unlock(&win32k_lock);
	pr_info("win32k: USER served here, ntos is the object manager\n");
}

/* a callout of ntos, called under its executive lock (the order is the
 * executive's lock, then win32k_lock) */
long win32k_kernel_callout(struct win32k_callout *co)
{
	struct w32_call call;

	mutex_lock(&win32k_lock);
	w32_enter(&call, NULL);
	w32_callout(co);
	co->status = call.error;
	w32_leave();
	mutex_unlock(&win32k_lock);
	return 0;
}
