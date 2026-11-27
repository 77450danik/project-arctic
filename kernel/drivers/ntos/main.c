// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * ntos: the executive of NT in the kernel: its system services, the threads
 * and processes that call them, and what wineserver's request loop and main
 * loop did (docs/ntos.md)
 *
 * A call is one request of the Wine server protocol (nt_call): its fixed
 * part and its data are copied in, the handler runs in the calling thread
 * under the executive lock, with the time and the current thread set as the
 * server's call_req_handler set them, and the reply goes back the same way.
 * The lock makes the executive what wineserver was, one request at a time
 * (finer locks: docs/ntos.md, "Замки").
 *
 * A task says what it is once (nt_hello): a process without a parent in NT
 * is made as one that connected to wineserver's master socket was; the first
 * thread of a process new_process made finds it by its end of the socket pair
 * the parent gave; a thread new_thread made by its id. Each task is then
 * found by its pid. Only tasks of the first user and pid namespaces may say
 * it: a container of WSL (its root is NT's user) never reaches NT.
 *
 * What the server's main loop did runs in the executive's worker: a watch of
 * the kernel's on each file the server polled marks it ready, a timer is set
 * for the next timeout, and the worker takes the lock and calls the turn of
 * the loop (ntos_poll_dispatch in io/fd.c).
 *
 * Based on Wine's server/request.c and server/main.c,
 * Copyright (C) 1998 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <linux/anon_inodes.h>
#include <linux/cred.h>
#include <linux/file.h>
#include <linux/hrtimer.h>
#include <linux/namei.h>
#include <linux/pid_namespace.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>
#include <linux/user_namespace.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>
#include <linux/lsm_hooks.h>
#include <uapi/linux/lsm.h>
#include <net/af_unix.h>
#include <linux/ntos.h>
#include "ntos.h"

/* the kernel's current again (Wine's headers have fields of that name; this
 * file touches none of them) */
#define current get_current()

/* net/unix/af_unix.h: the other end of a connected unix socket, referenced */
struct sock *unix_peer_get(struct sock *sk);

/* ------------------------------------------------------------- what the server's code takes from request.c and main.c */

int debug_level;
int server_dir_fd = -1, config_dir_fd = -1;
char *server_dir;
timeout_t server_start_time;
timeout_t master_socket_timeout = TIMEOUT_INFINITE;	/* the executive does not go away */
int win32k_active = 1;					/* USER is win32k's */

static DEFINE_MUTEX(ntos_lock);				/* the executive lock */

/*
 * How the executive's lock is used (docs/ntos.md, «Замки»): how often a call
 * found it taken and how long it waited, and which requests hold it longest.
 * What the split into finer locks is chosen by; nt_admin NT_ADMIN_LOCK_STATS
 * gives it as text, arctic-init writes it to host.log at shutdown.
 */
static struct
{
	atomic64_t taken, contended, wait_ns, max_wait_ns;
	atomic64_t worker_ns, worker_max_ns;	/* the worker: timers, ready descriptors, the registry's save */
	atomic64_t held_ns[REQ_NB_REQUESTS], calls[REQ_NB_REQUESTS];
} lock_stats;

static void ntos_lock_core( void )
{
	if (!mutex_trylock( &ntos_lock ))
	{
		u64 t0 = ktime_get_ns(), dt;
		s64 max;

		mutex_lock( &ntos_lock );
		dt = ktime_get_ns() - t0;
		atomic64_inc( &lock_stats.contended );
		atomic64_add( dt, &lock_stats.wait_ns );
		max = atomic64_read( &lock_stats.max_wait_ns );
		while ((s64)dt > max && !atomic64_try_cmpxchg( &lock_stats.max_wait_ns, &max, dt ))
			;
	}
	atomic64_inc( &lock_stats.taken );
	ntos_ctx()->held |= 1u << NTOS_DOMAIN_CORE;
}

static void ntos_unlock_core( void )
{
	ntos_ctx()->held &= ~(1u << NTOS_DOMAIN_CORE);
	mutex_unlock( &ntos_lock );
}


/*
 * The longest single holds and who held: a request by its number, the
 * worker's timeout or descriptor callback by its address (req -1). A total
 * says what to split, these say what stops every call for a long moment.
 * Kept under the executive lock.
 */
/* ntos.verify_registry=1 on the kernel's command line (VM tests) */
bool ntos_verify_registry;
module_param_named( verify_registry, ntos_verify_registry, bool, 0444 );

#define NTOS_SLOWEST 5
static struct { u64 ns, cpu_ns; int req; const void *fn; } slowest[NTOS_SLOWEST];

/* how long the task has run on a CPU: a hold much longer than that was
 * spent off the CPU (preempted, or a vCPU the host did not run) */
u64 ntos_cpu_ns( void )
{
	return READ_ONCE( current->se.sum_exec_runtime );
}

void ntos_note_hold( int req, const void *fn, u64 ns, u64 cpu_ns )
{
	int i;

	for (i = 0; i < NTOS_SLOWEST; i++)
		if (!slowest[i].ns || (slowest[i].req == req && slowest[i].fn == fn)) break;
	if (i == NTOS_SLOWEST) i = NTOS_SLOWEST - 1;	/* the shortest of them makes room */
	if (ns <= slowest[i].ns) return;
	slowest[i].ns = ns;
	slowest[i].cpu_ns = cpu_ns;
	slowest[i].req = req;
	slowest[i].fn = fn;
	for (; i > 0 && slowest[i - 1].ns < ns; i--)
		swap( slowest[i], slowest[i - 1] );
}

/* the statistics as text: totals, then the requests by the time they held it */
static size_t lock_stats_text( char *buf, size_t size )
{
	static const char *const names[] = {
#define NTOS_REQ_NAME(name) #name,
#include "req_names.h"
#undef NTOS_REQ_NAME
	};
	int order[16], n = 0, i, j;
	size_t pos;

	pos = scnprintf( buf, size, "taken %lld contended %lld wait %lld us max %lld us; worker held %lld us max %lld us; longest",
			 atomic64_read( &lock_stats.taken ), atomic64_read( &lock_stats.contended ),
			 atomic64_read( &lock_stats.wait_ns ) / 1000, atomic64_read( &lock_stats.max_wait_ns ) / 1000,
			 atomic64_read( &lock_stats.worker_ns ) / 1000, atomic64_read( &lock_stats.worker_max_ns ) / 1000 );
	for (i = 0; i < NTOS_SLOWEST && slowest[i].ns; i++)
	{
		/* the hold, and how much of it on a CPU */
		if (slowest[i].req >= 0)
			pos += scnprintf( buf + pos, size - pos, " %s %llu/%llu us",
					  slowest[i].req < ARRAY_SIZE(names) ? names[slowest[i].req] : "?",
					  slowest[i].ns / 1000, slowest[i].cpu_ns / 1000 );
		else
			pos += scnprintf( buf + pos, size - pos, " %ps %llu/%llu us", slowest[i].fn,
					  slowest[i].ns / 1000, slowest[i].cpu_ns / 1000 );
	}
	pos += scnprintf( buf + pos, size - pos, "\n" );
	/* the 16 requests that held it longest in all */
	for (i = 0; i < REQ_NB_REQUESTS; i++)
	{
		s64 held = atomic64_read( &lock_stats.held_ns[i] );

		if (!held) continue;
		for (j = n; j > 0 && atomic64_read( &lock_stats.held_ns[order[j - 1]] ) < held; j--)
			if (j < ARRAY_SIZE(order)) order[j] = order[j - 1];
		if (j < ARRAY_SIZE(order)) order[j] = i;
		if (n < ARRAY_SIZE(order)) n++;
	}
	for (i = 0; i < n; i++)
		pos += scnprintf( buf + pos, size - pos, "  %-28s %10lld calls %10lld us\n",
				  order[i] < ARRAY_SIZE(names) ? names[order[i]] : "?",
				  atomic64_read( &lock_stats.calls[order[i]] ),
				  atomic64_read( &lock_stats.held_ns[order[i]] ) / 1000 );
	return pos;
}
static bool running;
static bool fatal;					/* fatal_error while starting: it does not start */
static const struct cred *ntos_cred;

const struct cred *nt_creds(void)
{
	return ntos_cred ? ntos_cred : current_cred();
}

/*
 * The room for a call context's pointer in every task (thread.h, ntos_ctx):
 * an LSM with no hooks that asks for a pointer in each task's security blob.
 * LSM_ORDER_LAST is always on, as integrity is, whatever lsm= says; a field
 * of task_struct would rebuild the whole kernel for every change of sched.h.
 */
unsigned int ntos_task_off;
static bool ntos_lsm_ready;

static struct lsm_blob_sizes ntos_blobs __ro_after_init = {
	.lbs_task = sizeof(struct ntos_ctx *),
};

static const struct lsm_id ntos_lsmid = {
	.name = "ntos",
	.id = LSM_ID_UNDEF,
};

static int __init ntos_lsm_init( void )
{
	ntos_task_off = ntos_blobs.lbs_task;	/* the offset, once the blobs are laid out */
	ntos_lsm_ready = true;
	return 0;
}

DEFINE_LSM(ntos) = {
	.id = &ntos_lsmid,
	.order = LSM_ORDER_LAST,
	.blobs = &ntos_blobs,
	.init = ntos_lsm_init,
};

/* each way into the executive: a call context of its own on the task (thread.h,
 * struct ntos_ctx), the one it was in given back on the way out (a callout of
 * win32k can come back in) */
struct ntos_in
{
	struct ntos_ctx ctx;
	void *outer;
};

static void ntos_enter( struct ntos_in *in )
{
	in->ctx.thread = NULL;
	in->ctx.error = 0;
	in->ctx.held = 0;
	in->ctx.io_out = 0;
	in->ctx.err_no = 0;
	in->outer = *ntos_ctx_slot( current );
	*ntos_ctx_slot( current ) = &in->ctx;
}

static void ntos_leave( struct ntos_in *in )
{
	*ntos_ctx_slot( current ) = in->outer;
}

/* the C library's errno of this call (crt.h); outside any call (the registry's
 * writer), one of the executive's own */
int *nt_errno_location( void )
{
	static int outside;
	struct ntos_ctx *ctx = ntos_lsm_ready ? *ntos_ctx_slot( current ) : NULL;

	return ctx ? &ctx->err_no : &outside;
}

/*
 * A call lets the dispatcher's lock go while it does I/O of its own that
 * touches nothing shared: what it reads belongs to an object nobody else can
 * see yet (an unnamed section being made), through a descriptor it holds.
 * Windows reads an image's headers for MmCreateSection without a global
 * lock; under wineserver's one lock a cold DLL read from the disk (1 ms) held
 * up every other call meanwhile. 1 if it let it go (then ntos_io_end).
 */
int ntos_io_begin( void )
{
	struct ntos_ctx *ctx = ntos_ctx();

	if (!(ctx->held & (1u << NTOS_DOMAIN_CORE)) || ctx->io_out) return 0;
	ctx->io_out = 1;
	ntos_unlock_core();
	ctx->out_from = ktime_get_ns();
	ctx->out_cpu_from = ntos_cpu_ns();
	return 1;
}

void ntos_io_end( void )
{
	struct ntos_ctx *ctx = ntos_ctx();

	if (!ctx->io_out) return;
	ctx->io_out = 0;
	ntos_lock_core();
	ctx->out_ns += ktime_get_ns() - ctx->out_from;	/* not held: not in the request's hold */
	ctx->out_cpu_ns += ntos_cpu_ns() - ctx->out_cpu_from;
	set_current_time();	/* time went on meanwhile */
}

/* ------------------------------------------------------------- objects destroyed by their domain's worker */

int ntos_domain_held( unsigned int domain )
{
	struct ntos_ctx *ctx = ntos_lsm_ready ? *ntos_ctx_slot( current ) : NULL;

	return ctx && (ctx->held & (1u << domain));
}

static struct llist_head deferred[NTOS_DOMAIN_COUNT];
static void reap_work( struct work_struct *work );
static DECLARE_WORK(reap_core, reap_work);
static struct workqueue_struct *worker;

/* as ObpDeferObjectDeletion: the last reference went where the object's
 * domain is not locked; its worker destroys it under that lock */
void ntos_defer_destroy( struct object *obj )
{
	llist_add( &obj->deferred, &deferred[obj->ops->domain] );
	queue_work( worker ? worker : system_unbound_wq, &reap_core );
}

static const struct cred *enter_worker(void);

static void reap_work( struct work_struct *work )
{
	const struct cred *old = enter_worker();
	struct llist_node *nodes;
	struct object *obj, *next;
	struct ntos_in in;

	ntos_enter( &in );
	ntos_lock_core();
	nt_current = NULL;
	nodes = llist_del_all( &deferred[NTOS_DOMAIN_CORE] );
	llist_for_each_entry_safe( obj, next, nodes, deferred )
		destroy_object( obj );
	nt_current = NULL;
	ntos_unlock_core();
	ntos_leave( &in );
	revert_creds( old );
}

/* complain about a protocol error and terminate the client connection */
void fatal_protocol_error( struct thread *thread, const char *err, ... )
{
	va_list args;

	va_start( args, err );
	nt_fprintf( stderr, "Protocol error:%04x: ", thread->id );
	nt_vfprintf( stderr, err, args );
	va_end( args );
	thread->exit_code = 1;
	kill_thread( thread, 1 );
}

/* the server died on these; the executive cannot: it says so, and the call fails */
void fatal_error( const char *err, ... )
{
	va_list args;

	va_start( args, err );
	nt_fprintf( stderr, "fatal: " );
	nt_vfprintf( stderr, err, args );
	va_end( args );
	set_error( STATUS_INTERNAL_ERROR );
	fatal = true;
}

/* allocate the reply data */
void *set_reply_data_size( data_size_t size )
{
	assert( size <= get_reply_max_size() );
	if (size && !(nt_current->reply_data = mem_alloc( size ))) size = 0;
	nt_current->reply_size = size;
	return nt_current->reply_data;
}

static const struct object_attributes empty_attributes;

/* return object attributes from the nt_current request */
bool get_req_object_attributes( struct object_params *params )
{
	const struct object_attributes *attr = get_req_data();
	data_size_t size = get_req_data_size();

	params->root = NULL;
	params->sd   = NULL;

	if (!size)
	{
		params->name.len = 0;
		params->attr     = 0;
		params->objattr  = &empty_attributes;
		return true;
	}
	if ((size < sizeof(*attr)) || (size - sizeof(*attr) < attr->sd_len) ||
	    (size - sizeof(*attr) - attr->sd_len < attr->name_len))
	{
		set_error( STATUS_ACCESS_VIOLATION );
		return false;
	}
	if (attr->sd_len && !sd_is_valid( (const struct security_descriptor *)(attr + 1), attr->sd_len ))
	{
		set_error( STATUS_INVALID_SECURITY_DESCR );
		return false;
	}
	if ((attr->name_len & (sizeof(WCHAR) - 1)) || attr->name_len >= 65534)
	{
		set_error( STATUS_OBJECT_NAME_INVALID );
		return false;
	}
	if (attr->rootdir && attr->name_len)
	{
		if (!(params->root = get_handle_obj( nt_current->process, attr->rootdir, 0, NULL ))) return false;
	}
	if (attr->sd_len) params->sd = (const struct security_descriptor *)(attr + 1);

	params->name.len = attr->name_len;
	params->name.str = (const WCHAR *)(attr + 1) + attr->sd_len / sizeof(WCHAR);
	params->attr     = attr->attributes;
	params->objattr  = attr;
	return true;
}

/* return a pointer to the request data following an object attributes structure */
const void *get_req_data_after_objattr( const struct object_params *params, data_size_t *len )
{
	const struct object_attributes *attr = params->objattr;
	data_size_t size = (sizeof(*attr) + (attr->sd_len & ~1) + (attr->name_len & ~1) + 3) & ~3;

	if (attr == &empty_attributes || size >= get_req_data_size())
	{
		*len = 0;
		return NULL;
	}
	*len = get_req_data_size() - size;
	return (const char *)get_req_data() + size;
}

/* the server read requests from a pipe and wrote replies; nt_call carries them */
void read_request( struct thread *thread )
{
}

void write_reply( struct thread *thread )
{
}

/* return a monotonic time counter, CLOCK_BOOTTIME as the server's */
timeout_t monotonic_counter(void)
{
	return ktime_get_boottime_ns() / 100;
}

void close_master_socket( timeout_t timeout )
{
}

void fastcall_thread_gone( struct thread *thread )
{
}

void fastcall_process_gone( struct process *process )
{
}

/* Arctic's fast calls were a shortcut to wineserver; a system call is shorter */
DECL_HANDLER(register_fastcall)
{
	set_error( STATUS_NOT_SUPPORTED );
}

/* ------------------------------------------------------------- descriptors between the clients and the executive */

/* the descriptors a process is given (send_client_fd): it takes them in
 * order with nt_receive_fd, as it read them from its socket */
struct client_fd
{
	struct list_head entry;
	struct file *file;
	obj_handle_t handle;
};

struct process_fds
{
	struct process *process;	/* not referenced: the entry goes with the process */
	struct list_head fds;
};

static DEFINE_XARRAY(process_fds);	/* NT process id -> struct process_fds */

static struct process_fds *get_process_fds( struct process *process, bool create )
{
	struct process_fds *pf = xa_load( &process_fds, process->id );

	if (pf || !create) return pf;
	if (!(pf = kzalloc( sizeof(*pf), GFP_KERNEL ))) return NULL;
	pf->process = process;
	INIT_LIST_HEAD( &pf->fds );
	if (xa_err( xa_store( &process_fds, process->id, pf, GFP_KERNEL ) ))
	{
		kfree( pf );
		return NULL;
	}
	return pf;
}

/* the process went (fastcall_process_gone is called from process_killed too
 * late): what it was not given goes */
static void drop_process_fds( process_id_t id )
{
	struct process_fds *pf = xa_erase( &process_fds, id );
	struct client_fd *cfd, *next;

	if (!pf) return;
	list_for_each_entry_safe( cfd, next, &pf->fds, entry )
	{
		fput( cfd->file );
		kfree( cfd );
	}
	kfree( pf );
}

/* send an fd to a client */
int send_client_fd( struct process *process, int fd, obj_handle_t handle )
{
	struct process_fds *pf = get_process_fds( process, true );
	struct file *file = nt_file_of( fd );
	struct client_fd *cfd;

	if (!pf || !file || !(cfd = kmalloc( sizeof(*cfd), GFP_KERNEL )))
	{
		nt_fprintf( stderr, "Protocol error: process %04x: could not give fd %d\n", process->id, fd );
		return -1;
	}
	cfd->file = get_file( file );
	cfd->handle = handle;
	list_add_tail( &cfd->entry, &pf->fds );
	return 0;
}

/* the server looked at its socket again when an fd it waited for was not
 * there yet; nt_send_fd puts it in place before the request */
int nt_receive_client_fd( struct process *process )
{
	return -1;
}

/* ------------------------------------------------------------- tasks and their threads */

static DEFINE_XARRAY(tasks);	/* task pid (first namespace) -> struct thread, referenced */

static struct thread *task_thread( struct task_struct *tsk )
{
	return xa_load( &tasks, task_pid_nr( tsk ) );
}

static bool from_nt_namespaces(void)
{
	return current_user_ns() == &init_user_ns && task_active_pid_ns( get_current() ) == &init_pid_ns;
}

static int bind_task( struct thread *thread )
{
	if (xa_err( xa_store( &tasks, task_pid_nr( get_current() ), thread, GFP_KERNEL ) )) return -ENOMEM;
	grab_object( thread );
	return 0;
}

/* the executive's credentials while it runs on its own (the worker, the saver) */
static const struct cred *enter_worker(void)
{
	return override_creds( nt_creds() );
}

/* do_exit of every task (patch 0008): a thread of NT ends as its request
 * pipe closing ended it in wineserver */
void ntos_thread_exit( struct task_struct *tsk )
{
	struct thread *thread;
	struct ntos_in in;

	if (!running || !(thread = xa_load( &tasks, task_pid_nr( tsk ) ))) return;
	ntos_enter( &in );
	ntos_lock_core();
	if (xa_load( &tasks, task_pid_nr( tsk ) ) == thread)
	{
		xa_erase( &tasks, task_pid_nr( tsk ) );
		set_current_time();
		nt_current = NULL;
		if (thread->state != TERMINATED) kill_thread( thread, 0 );
		release_object( thread );	/* the task's */
	}
	ntos_unlock_core();
	ntos_leave( &in );
}

/* a process without a parent in NT: as wineserver's master socket made one */
static long hello_connect( struct nt_hello *hello )
{
	struct socket *pair[2];
	struct process *process;
	struct thread *thread;
	struct file *client;
	int server_fd, client_fd, ret;

	if ((ret = sock_create( AF_UNIX, SOCK_STREAM, 0, &pair[0] ))) return ret;
	if ((ret = sock_create( AF_UNIX, SOCK_STREAM, 0, &pair[1] )))
	{
		sock_release( pair[0] );
		return ret;
	}
	if ((ret = pair[0]->ops->socketpair( pair[0], pair[1] )))
	{
		sock_release( pair[0] );
		sock_release( pair[1] );
		return ret;
	}
	client = sock_alloc_file( pair[1], 0, NULL );
	if (IS_ERR( client ))
	{
		sock_release( pair[0] );
		return PTR_ERR( client );
	}
	{
		struct file *server = sock_alloc_file( pair[0], O_NONBLOCK, NULL );

		if (IS_ERR( server ))
		{
			fput( client );
			return PTR_ERR( server );
		}
		if ((server_fd = nt_fd_for_file( server )) == -1)
		{
			fput( client );
			return -ENOMEM;
		}
	}
	if (!(process = create_process( server_fd, NULL, 0, NULL, NULL, NULL, 0, NULL )))
	{
		fput( client );
		return -ENOMEM;
	}
	if (!(thread = create_thread( -1, process, NULL )))
	{
		release_object( process );
		fput( client );
		return -ENOMEM;
	}
	add_process_thread( process, thread );
	release_object( process );
	/* the reference create_thread gave stays the thread's: kill_thread
	 * drops it, as after master_socket_poll_event in wineserver */
	if ((ret = bind_task( thread )))
	{
		kill_thread( thread, 0 );
		fput( client );
		return ret;
	}
	if ((client_fd = get_unused_fd_flags( 0 )) < 0)
	{
		fput( client );
		return client_fd;
	}
	fd_install( client_fd, client );
	hello->fd = client_fd;
	return 0;
}

/* the first thread of a process new_process made: the process whose socket
 * is the peer of the one the child was given */
static long hello_process( struct nt_hello *hello )
{
	struct file *file = fget( hello->fd );
	struct socket *sock;
	struct sock *peer;
	struct process *process;
	struct thread *thread;
	long ret = -ESRCH;

	if (!file) return -EBADF;
	if (!(sock = sock_from_file( file )) || !sock->sk || !(peer = unix_peer_get( sock->sk )))
	{
		fput( file );
		return -EINVAL;
	}
	if ((process = ntos_process_of_socket( peer )) && (thread = get_process_first_thread( process )) &&
	    !task_thread( get_current() ))
		ret = bind_task( thread );
	sock_put( peer );
	fput( file );
	return ret;
}

/* a thread new_thread made in the caller's process */
static long hello_thread( struct nt_hello *hello )
{
	struct thread *thread, *sibling = NULL;
	struct task_struct *task;
	long ret;

	if (task_thread( get_current() )) return -EEXIST;
	/* another task of the caller's thread group says which process it is */
	rcu_read_lock();
	for_each_thread( get_current(), task )
		if ((sibling = xa_load( &tasks, task_pid_nr( task ) ))) break;
	rcu_read_unlock();
	if (!sibling) return -ESRCH;
	if (!(thread = get_thread_from_id( hello->thread_id ))) return -ESRCH;
	if (thread->process != sibling->process || thread->state == TERMINATED)
		ret = -EPERM;
	else
		ret = bind_task( thread );
	release_object( thread );
	return ret;
}

static long do_nt_hello( struct nt_hello __user *arg )
{
	struct nt_hello hello;
	long ret;

	if (copy_from_user( &hello, arg, sizeof(hello) )) return -EFAULT;
	if (!running) return -ENOENT;	/* arctic.ntos=0: wineserver serves */
	if (hello.protocol != SERVER_PROTOCOL_VERSION) return -EPROTO;
	if (!from_nt_namespaces()) return -EPERM;

	ntos_lock_core();
	set_current_time();
	nt_current = NULL;
	switch (hello.kind)
	{
	case NT_HELLO_CONNECT: ret = task_thread( get_current() ) ? -EEXIST : hello_connect( &hello ); break;
	case NT_HELLO_PROCESS: ret = hello_process( &hello ); break;
	case NT_HELLO_THREAD:  ret = hello_thread( &hello ); break;
	default:               ret = -EINVAL; break;
	}
	nt_current = NULL;
	ntos_unlock_core();
	if (!ret && copy_to_user( arg, &hello, sizeof(hello) )) ret = -EFAULT;
	return ret;
}

/* ------------------------------------------------------------- requests */

static void dispatch_pending( void );

/* the call served with only an error, as wineserver's reply to a request it
 * could not take (its memory): the thread goes on, its call fails */
static long nt_call_failed( struct nt_call __user *arg, struct nt_call *args, unsigned int status )
{
	union generic_reply reply;

	memset( &reply, 0, sizeof(reply) );
	reply.reply_header.error = status;
	args->status = status;
	args->reply_data_size = 0;
	if (copy_to_user( u64_to_user_ptr( args->reply ), &reply, sizeof(reply) ) ||
	    copy_to_user( arg, args, sizeof(*args) ))
		return -EFAULT;
	return 0;
}

/*
 * Before a request takes the lock: what it will look up on the disk is
 * looked up now, so that a cold directory or inode is read from the disk
 * while the others run (the open under the lock then finds them in the
 * dcache). Only a lookup: nothing is opened, nothing changes. create_file
 * gives the Unix name after its object attributes, as
 * get_req_data_after_objattr finds it.
 */
static void prefetch_for( enum request req, const void *data, data_size_t size )
{
	const struct object_attributes *attr = data;
	const char *name;
	data_size_t len, skip;
	struct path path;
	char *cname;

	if (req != REQ_create_file || size < sizeof(*attr) || attr->rootdir) return;
	skip = sizeof(*attr) + attr->sd_len + attr->name_len;
	if (skip >= size) return;
	name = (const char *)data + skip;
	len = size - skip;
	if (!len || name[0] != '/' || len >= PATH_MAX) return;
	if (!(cname = kmemdup_nul( name, len, GFP_KERNEL ))) return;
	if (!kern_path( cname, LOOKUP_FOLLOW, &path )) path_put( &path );
	else
	{
		/* a file to be made: its directory, at least */
		char *slash = strrchr( cname, '/' );

		if (slash && slash != cname)
		{
			*slash = 0;
			if (!kern_path( cname, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &path )) path_put( &path );
		}
	}
	kfree( cname );
}

static long do_nt_call( struct nt_call __user *arg )
{
	struct thread *thread = task_thread( get_current() );
	union generic_reply reply;
	struct nt_call args;
	struct iovec iov[8];
	enum request req;
	void *req_data = NULL, *reply_data;
	data_size_t size, done = 0, reply_size = 0;
	union generic_request request;
	unsigned int i;
	long ret = 0;
	u64 held_from, cpu_from;

	if (!thread || !running) return -ESRCH;	/* nt_hello first; none after NT_ADMIN_SHUTDOWN */
	if (copy_from_user( &args, arg, sizeof(args) )) return -EFAULT;
	if (args.data_count > ARRAY_SIZE(iov)) return -EINVAL;
	if (args.data_count && copy_from_user( iov, u64_to_user_ptr( args.data ), args.data_count * sizeof(*iov) ))
		return -EFAULT;

	/* the request and its data come in before the lock is taken: a page of
	 * the caller's that faults in (from the disk, or swap) holds up no one */
	if (copy_from_user( &request, u64_to_user_ptr( args.request ), sizeof(request) )) return -EFAULT;
	req = request.request_header.req;
	size = request.request_header.request_size;
	if (req >= REQ_NB_REQUESTS || !ntos_handlers[req])
		return -EINVAL;	/* USER's go to win32k */
	if (size)
	{
		/* any size, as wineserver took: a pipe's message of Chromium's IPC is
		 * often more than 4 MB, and a limit here killed the writing thread */
		if (!(req_data = kvmalloc( size, GFP_KERNEL | __GFP_NOWARN )))
			return nt_call_failed( arg, &args, STATUS_NO_MEMORY );
		for (i = 0; i < args.data_count && done < size; i++)
		{
			data_size_t part = min_t( data_size_t, iov[i].iov_len, size - done );

			if (copy_from_user( (char *)req_data + done, iov[i].iov_base, part ))
			{
				kvfree( req_data );
				return -EFAULT;
			}
			done += part;
		}
		if (done != size)
		{
			kvfree( req_data );
			return -EINVAL;
		}
	}
	prefetch_for( req, req_data, size );

	ntos_lock_core();
	/* what became ready before this request is served first, as wineserver's
	 * loop handled the descriptors epoll gave before the next request: a
	 * socket's FD_WRITE is there for the WSAEnumNetworkEvents that follows */
	dispatch_pending();
	grab_object( thread );
	thread->req = request;
	/* who the caller is, from the kernel: not what it says */
	if (req == REQ_init_first_thread)
	{
		thread->req.init_first_thread_request.unix_pid = task_tgid_nr( get_current() );
		thread->req.init_first_thread_request.unix_tid = task_pid_nr( get_current() );
	}
	else if (req == REQ_init_thread)
		thread->req.init_thread_request.unix_tid = task_pid_nr( get_current() );

	/* as call_req_handler */
	ntos_ctx()->out_ns = ntos_ctx()->out_cpu_ns = 0;
	cpu_from = ntos_cpu_ns();
	held_from = ktime_get_ns();
	thread->req_data = req_data;
	set_current_time();
	nt_current = thread;
	nt_current->reply_size = 0;
	clear_error();
	memset( &reply, 0, sizeof(reply) );
	if (thread->state == TERMINATED)
		set_error( STATUS_THREAD_IS_TERMINATING );
	else
		ntos_handlers[req]( &thread->req, &reply );
	thread->req_data = NULL;
	if (nt_current)
	{
		reply.reply_header.error = nt_current->error;
		reply.reply_header.reply_size = nt_current->reply_size;
		reply_size = nt_current->reply_size;
	}
	else ret = -ESRCH;	/* it killed itself */
	reply_data = thread->reply_data;
	thread->reply_data = NULL;
	nt_current = NULL;
	held_from = ktime_get_ns() - held_from - ntos_ctx()->out_ns;	/* without its own I/O outside the lock */
	atomic64_add( held_from, &lock_stats.held_ns[req] );
	atomic64_inc( &lock_stats.calls[req] );
	ntos_note_hold( req, NULL, held_from, ntos_cpu_ns() - cpu_from - ntos_ctx()->out_cpu_ns );
	ntos_unlock_core();

	if (!ret)
	{
		args.status = reply.reply_header.error;
		args.reply_data_size = reply_size;
		if (copy_to_user( u64_to_user_ptr( args.reply ), &reply, sizeof(reply) ) ||
		    (reply_size && copy_to_user( u64_to_user_ptr( args.reply_data ), reply_data, reply_size )) ||
		    copy_to_user( arg, &args, sizeof(args) ))
			ret = -EFAULT;
	}
	nt_free( reply_data );
	kvfree( req_data );
	ntos_lock_core();
	release_object( thread );
	ntos_unlock_core();
	return ret;

out:
	nt_current = NULL;
	release_object( thread );
	ntos_unlock_core();
	kvfree( req_data );
	return ret;
}

/* a descriptor for the executive, before the request that names it (as
 * wine_server_send_fd sent it on the process's socket) */
static long do_nt_send_fd( int fd, u32 tid )
{
	struct thread *thread = task_thread( get_current() ), *target;
	struct file *file;
	int server_fd;
	long ret = 0;

	if (!thread) return -ESRCH;
	if (!(file = fget( fd ))) return -EBADF;
	ntos_lock_core();
	nt_current = NULL;
	if ((server_fd = nt_fd_for_file( file )) == -1)
	{
		ret = -EMFILE;
		goto out;
	}
	if (tid)
	{
		if (!(target = get_thread_from_id( tid ))) goto bad;
	}
	else target = (struct thread *)grab_object( get_process_first_thread( thread->process ) );
	if (!target || target->process != thread->process || target->state == TERMINATED)
	{
		if (target) release_object( target );
		goto bad;
	}
	thread_add_inflight_fd( target, fd, server_fd );
	release_object( target );
	goto out;
bad:
	/* as receive_fd of wineserver: a descriptor for no thread of the
	 * process is dropped, the sender is not told */
	nt_close( server_fd );
out:
	ntos_unlock_core();
	return ret;
}

/* the next descriptor the executive gave the caller's process, in its table */
static long do_nt_receive_fd( u32 __user *handle )
{
	struct thread *thread = task_thread( get_current() );
	struct process_fds *pf;
	struct client_fd *cfd = NULL;
	int fd;

	if (!thread) return -ESRCH;
	ntos_lock_core();
	if ((pf = get_process_fds( thread->process, false )) && !list_empty( &pf->fds ))
	{
		cfd = list_first_entry( &pf->fds, struct client_fd, entry );
		list_del( &cfd->entry );
	}
	ntos_unlock_core();
	if (!cfd) return -EAGAIN;
	if (put_user( cfd->handle, handle ) || (fd = get_unused_fd_flags( O_CLOEXEC )) < 0)
	{
		fput( cfd->file );
		kfree( cfd );
		return -EFAULT;
	}
	fd_install( fd, cfd->file );
	kfree( cfd );
	return fd;
}

/* ------------------------------------------------------------- the main loop: watches, the timer, the worker */

struct watch
{
	struct file *file;
	int user;			/* its index in watches: the poll user of io/fd.c */
	int fd, events, nheads;
	wait_queue_entry_t wait[2];
	wait_queue_head_t *heads[2];
	poll_table pt;
	bool queued;
	struct list_head ready;
};

static struct watch **watches;		/* by poll user index of io/fd.c */
static int watches_size;
/* the watch of each descriptor, for nt_poll_forget: a closed handle looked
 * through every watch there ever was (thousands with a game, Steam and a
 * browser running), 18 us of the executive's lock per CloseHandle */
static DEFINE_XARRAY(watch_of_fd);
static bool fd_watched_twice;		/* two watches on one descriptor: forget searches */
static LIST_HEAD(ready_list);		/* woken, to be given to their users */
static LIST_HEAD(again_list);		/* given this turn: again next turn if still ready */
static DEFINE_SPINLOCK(ready_lock);
static struct hrtimer timer;
static void dispatch_work( struct work_struct *work );
static DECLARE_WORK(dispatch, dispatch_work);

static void mark_ready( struct watch *w )
{
	unsigned long flags;

	spin_lock_irqsave( &ready_lock, flags );
	if (!w->queued)
	{
		w->queued = true;
		list_add_tail( &w->ready, &ready_list );
	}
	spin_unlock_irqrestore( &ready_lock, flags );
	if (worker) queue_work( worker, &dispatch );
}

static int watch_wake( wait_queue_entry_t *wait, unsigned int mode, int sync, void *key )
{
	struct watch *w = wait->private;
	__poll_t events = key_to_poll( key );

	if (!events || (events & (w->events | EPOLLERR | EPOLLHUP))) mark_ready( w );
	return 0;
}

static void watch_queue_proc( struct file *file, wait_queue_head_t *head, poll_table *pt )
{
	struct watch *w = container_of( pt, struct watch, pt );
	int i = w->nheads;

	if (i >= ARRAY_SIZE(w->heads)) return;
	init_waitqueue_func_entry( &w->wait[i], watch_wake );
	w->wait[i].private = w;
	w->heads[i] = head;
	add_wait_queue( head, &w->wait[i] );
	w->nheads++;
}

static void watch_detach( struct watch *w )
{
	unsigned long flags;
	int i;

	for (i = 0; i < w->nheads; i++) remove_wait_queue( w->heads[i], &w->wait[i] );
	w->nheads = 0;
	spin_lock_irqsave( &ready_lock, flags );
	if (w->queued) list_del( &w->ready );
	w->queued = false;
	spin_unlock_irqrestore( &ready_lock, flags );
	if (w->file) fput( w->file );
	if (w->fd >= 0) xa_cmpxchg( &watch_of_fd, w->fd, w, NULL, GFP_KERNEL );
	w->file = NULL;
	w->fd = -1;
}

void nt_poll_watch( int user, int fd, int events )
{
	struct file *file = fd >= 0 ? nt_file_of( fd ) : NULL;
	struct watch *w;
	__poll_t now;

	if (user >= watches_size)
	{
		int size = max( user + 1, watches_size ? watches_size * 2 : 64 );
		struct watch **grown = kvrealloc( watches, size * sizeof(*grown), GFP_KERNEL );

		if (!grown) return;
		memset( grown + watches_size, 0, (size - watches_size) * sizeof(*grown) );
		watches = grown;
		watches_size = size;
	}
	if (!(w = watches[user]))
	{
		if (!file) return;
		if (!(w = watches[user] = kzalloc( sizeof(*w), GFP_KERNEL ))) return;
		w->user = user;
		w->fd = -1;
		INIT_LIST_HEAD( &w->ready );
	}
	if (w->file && w->file != file) watch_detach( w );
	w->events = events;
	if (!file) return;
	if (!w->file)
	{
		void *old;

		w->file = get_file( file );
		w->fd = fd;
		old = xa_store( &watch_of_fd, fd, w, GFP_KERNEL );
		if (old && old != w && !xa_is_err( old )) WRITE_ONCE( fd_watched_twice, true );
		init_poll_funcptr( &w->pt, watch_queue_proc );
		now = vfs_poll( file, &w->pt );
	}
	else now = vfs_poll( file, NULL );
	/* what is ready now is given at once, as epoll_ctl makes it */
	if (now & (events | EPOLLERR | EPOLLHUP)) mark_ready( w );
}

/* a descriptor closed: the watch on it, if any, ends */
void nt_poll_forget( int fd )
{
	struct watch *w;
	int i;

	if (fd < 0) return;
	if (unlikely( READ_ONCE( fd_watched_twice ) ))
	{
		for (i = 0; i < watches_size; i++)
			if (watches[i] && watches[i]->fd == fd) watch_detach( watches[i] );
		return;
	}
	if ((w = xa_load( &watch_of_fd, fd )) && w->fd == fd) watch_detach( w );
}

int nt_poll_ready( int *user, int *events )
{
	unsigned long flags;
	struct watch *w;

	for (;;)
	{
		__poll_t mask;

		spin_lock_irqsave( &ready_lock, flags );
		if (list_empty( &ready_list ))
		{
			spin_unlock_irqrestore( &ready_lock, flags );
			return 0;
		}
		w = list_first_entry( &ready_list, struct watch, ready );
		list_move_tail( &w->ready, &again_list );
		spin_unlock_irqrestore( &ready_lock, flags );
		if (!w->file) continue;
		mask = vfs_poll( w->file, NULL ) & (w->events | EPOLLERR | EPOLLHUP);
		if (!mask) continue;
		*user = w->user;
		*events = mask;
		return 1;
	}
}

/* after a turn: what is still ready and wanted is given again (epoll was
 * level-triggered), on the next turn */
static void requeue_ready(void)
{
	struct watch *w, *next;
	unsigned long flags;
	bool again = false;

	spin_lock_irqsave( &ready_lock, flags );
	list_for_each_entry_safe( w, next, &again_list, ready )
	{
		list_del( &w->ready );
		w->queued = false;
		if (w->file && (vfs_poll( w->file, NULL ) & (w->events | EPOLLERR | EPOLLHUP)))
		{
			w->queued = true;
			list_add_tail( &w->ready, &ready_list );
			again = true;
		}
	}
	spin_unlock_irqrestore( &ready_lock, flags );
	if (again && worker) queue_work( worker, &dispatch );
}

/* under the executive lock, in a request: the ready watches given to their
 * users now rather than by the worker later; nothing to do costs one look */
static bool timer_due;	/* the timer fired, its timeouts not run yet */

static void dispatch_pending( void )
{
	const struct cred *old;

	if (list_empty_careful( &ready_list ) && !READ_ONCE( timer_due )) return;
	WRITE_ONCE( timer_due, false );
	old = enter_worker();	/* as the worker: the executive's credentials, not the caller's */
	nt_current = NULL;
	ntos_poll_dispatch();
	nt_current = NULL;
	revert_creds( old );
	requeue_ready();
}

static enum hrtimer_restart timer_fn( struct hrtimer *t )
{
	WRITE_ONCE( timer_due, true );
	if (worker) queue_work( worker, &dispatch );
	return HRTIMER_NORESTART;
}

void nt_timeout_arm( long long ns )
{
	if (ns < 0)
	{
		hrtimer_try_to_cancel( &timer );
		return;
	}
	hrtimer_start( &timer, ns_to_ktime( ns ), HRTIMER_MODE_REL );
}

void nt_timeout_arm_before( long long ns )
{
	if (ns < 0) ns = 0;
	if (hrtimer_active( &timer ) && ktime_to_ns( hrtimer_get_remaining( &timer ) ) <= ns) return;
	hrtimer_start( &timer, ns_to_ktime( ns ), HRTIMER_MODE_REL );
}

static void dispatch_work( struct work_struct *work )
{
	const struct cred *old = enter_worker();
	struct ntos_in in;

	u64 t0;
	s64 dt, max;

	ntos_enter( &in );
	ntos_lock_core();
	t0 = ktime_get_ns();
	nt_current = NULL;
	WRITE_ONCE( timer_due, false );
	if (running) ntos_poll_dispatch();	/* not after NT_ADMIN_SHUTDOWN */
	nt_current = NULL;
	dt = ktime_get_ns() - t0;
	atomic64_add( dt, &lock_stats.worker_ns );
	max = atomic64_read( &lock_stats.worker_max_ns );
	while (dt > max && !atomic64_try_cmpxchg( &lock_stats.worker_max_ns, &max, dt ))
		;
	ntos_unlock_core();
	ntos_leave( &in );
	requeue_ready();
	revert_creds( old );
}

/* ------------------------------------------------------------- boot */

static char *boot_environ;

static long boot( const struct nt_boot *b )
{
	char *dir, **env = NULL;
	unsigned int count = 0, i;
	struct path path;
	struct cred *cred;
	long ret;

	if (b->environ_size > 65536) return -EINVAL;
	if (IS_ERR( dir = strndup_user( u64_to_user_ptr( b->config_dir ), PATH_MAX ) )) return PTR_ERR( dir );
	if (b->environ_size)
	{
		if (IS_ERR( boot_environ = memdup_user( u64_to_user_ptr( b->environ ), b->environ_size ) ))
		{
			kfree( dir );
			return PTR_ERR( boot_environ );
		}
		boot_environ[b->environ_size - 1] = 0;
		for (i = 0; i < b->environ_size; i += strlen( boot_environ + i ) + 1)
			if (boot_environ[i]) count++;
		if (!(env = kcalloc( count + 1, sizeof(*env), GFP_KERNEL )))
		{
			kfree( dir );
			return -ENOMEM;
		}
		count = 0;
		for (i = 0; i < b->environ_size; i += strlen( boot_environ + i ) + 1)
			if (boot_environ[i]) env[count++] = boot_environ + i;
	}
	nt_crt_set_environ( env, count );

	/* NT's user: what wineserver ran as (arctic-init's nt) */
	if (!(cred = prepare_creds()))
	{
		kfree( dir );
		return -ENOMEM;
	}
	cred->uid = cred->euid = cred->suid = cred->fsuid = KUIDT_INIT( b->uid );
	cred->gid = cred->egid = cred->sgid = cred->fsgid = KGIDT_INIT( b->gid );
	cap_clear( cred->cap_inheritable );
	cap_clear( cred->cap_permitted );
	cap_clear( cred->cap_effective );
	cap_clear( cred->cap_ambient );
	ntos_cred = cred;

	if ((ret = kern_path( dir, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &path )))
	{
		kfree( dir );
		return ret;
	}
	nt_crt_init( &path );
	path_put( &path );

	ntos_lock_core();
	{
		const struct cred *old = enter_worker();

		nt_current = NULL;
		if ((config_dir_fd = nt_open( dir, O_RDONLY | O_DIRECTORY )) == -1)
			ret = -nt_errno;
		else
		{
			/* as main(): the server's directory is its config directory here */
			server_dir_fd = config_dir_fd;
			server_dir = dir;
			dir = NULL;
			sock_init();
			set_current_time();
			server_start_time = nt_current_time;
			init_memory();
			{
				struct nt_fd *intl = load_intl_file();

				if (intl) init_directories( intl );
			}
			if (!fatal)
			{
				init_threading();
				init_registry();
			}
			/* freezable: asleep with the programs during hibernation, as
			 * wineserver was, its work of the moment finished first */
			if (!fatal && (worker = alloc_ordered_workqueue( "ntos", WQ_FREEZABLE | WQ_MEM_RECLAIM | WQ_HIGHPRI )))
			{
				win32k_ntos_register();
				running = true;
				ntos_poll_dispatch();
			}
			else
			{
				/* arctic-init starts wineserver instead */
				pr_err( "ntos: the executive could not start\n" );
				ret = -EIO;
			}
		}
		revert_creds( old );
	}
	ntos_unlock_core();
	kfree( dir );
	if (!ret) pr_info( "ntos: the executive is in the kernel, wineserver is not needed\n" );
	return ret;
}

static long do_nt_boot( const struct nt_boot __user *arg )
{
	struct nt_boot b;

	if (!capable( CAP_SYS_ADMIN )) return -EPERM;
	if (!ntos_lsm_ready) return -ENOSYS;	/* no room for call contexts: wineserver serves */
	if (copy_from_user( &b, arg, sizeof(b) )) return -EFAULT;
	if (b.protocol != SERVER_PROTOCOL_VERSION) return -EPROTO;
	if (running) return -EBUSY;
	return boot( &b );
}

static long do_nt_admin( u32 code, u64 value, void __user *buf, u32 size )
{
	long ret = 0;

	if (!capable( CAP_SYS_ADMIN )) return -EPERM;
	switch (code)
	{
	case NT_ADMIN_RUNNING:
		return running;
	case NT_ADMIN_FLUSH_REGISTRY:
		if (!running) return -ENOENT;
		ntos_lock_core();
		{
			const struct cred *old = enter_worker();

			nt_current = NULL;
			flush_registry();
			revert_creds( old );
		}
		ntos_unlock_core();
		return 0;
	case NT_ADMIN_LOCK_STATS:
	{
		char *text;
		size_t len;

		size = min_t( u32, size, 8192 );
		if (!(text = kvmalloc( size, GFP_KERNEL ))) return -ENOMEM;
		len = lock_stats_text( text, size );
		if (copy_to_user( buf, text, len )) ret = -EFAULT;
		else ret = len;
		kvfree( text );
		return ret;
	}
	case NT_ADMIN_SHUTDOWN:
	{
		int closed, on_disk, i;

		if (!running) return -ENOENT;
		ntos_lock_core();
		{
			const struct cred *old = enter_worker();

			nt_current = NULL;
			flush_registry();
			revert_creds( old );
		}
		/* nothing of NT runs any more: no call, timer or watch is served */
		running = false;
		hrtimer_cancel( &timer );
		for (i = 0; i < watches_size; i++)
			if (watches[i]) watch_detach( watches[i] );
		closed = nt_close_all( &on_disk );
		ntos_unlock_core();
		if (worker) flush_workqueue( worker );
		pr_info( "ntos: stopped, %d files let go (%d on disks)\n", closed, on_disk );
		return 0;
	}
	case NT_ADMIN_PROCESS_TREE:
	{
		char *reply;
		size_t len;

		if (!running) return -ENOENT;
		size = min_t( u32, size, 16384 );
		if (!(reply = kvmalloc( size, GFP_KERNEL ))) return -ENOMEM;
		ntos_lock_core();
		nt_current = NULL;
		len = ntos_process_tree( (int)value, reply, size );
		ntos_unlock_core();
		if (copy_to_user( buf, reply, len )) ret = -EFAULT;
		else ret = len;
		kvfree( reply );
		return ret;
	}
	}
	return -EINVAL;
}

/* ------------------------------------------------------------- the system services */

SYSCALL_DEFINE1(nt_hello, struct nt_hello __user *, arg)
{
	struct ntos_in in;
	long ret;

	ntos_enter( &in );
	ret = do_nt_hello( arg );
	ntos_leave( &in );
	return ret;
}

SYSCALL_DEFINE1(nt_call, struct nt_call __user *, arg)
{
	struct ntos_in in;
	long ret;

	ntos_enter( &in );
	ret = do_nt_call( arg );
	ntos_leave( &in );
	return ret;
}

SYSCALL_DEFINE2(nt_send_fd, int, fd, u32, tid)
{
	struct ntos_in in;
	long ret;

	ntos_enter( &in );
	ret = do_nt_send_fd( fd, tid );
	ntos_leave( &in );
	return ret;
}

SYSCALL_DEFINE1(nt_receive_fd, u32 __user *, handle)
{
	struct ntos_in in;
	long ret;

	ntos_enter( &in );
	ret = do_nt_receive_fd( handle );
	ntos_leave( &in );
	return ret;
}

SYSCALL_DEFINE1(nt_boot, const struct nt_boot __user *, arg)
{
	struct ntos_in in;
	long ret;

	ntos_enter( &in );
	ret = do_nt_boot( arg );
	ntos_leave( &in );
	return ret;
}

SYSCALL_DEFINE4(nt_admin, u32, code, u64, value, void __user *, buf, u32, size)
{
	struct ntos_in in;
	long ret;

	ntos_enter( &in );
	ret = do_nt_admin( code, value, buf, size );
	ntos_leave( &in );
	return ret;
}

static int __init ntos_init( void )
{
	hrtimer_setup( &timer, timer_fn, CLOCK_BOOTTIME, HRTIMER_MODE_REL );
	return 0;
}
subsys_initcall( ntos_init );
