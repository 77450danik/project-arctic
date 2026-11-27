/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * win32k: what the handlers that came from Wine's server expect of it.
 *
 * The handlers keep their own code (docs/win32k.md); what they called in the
 * server is given here in the kernel's terms. Everything runs under
 * win32k_lock, so the call being served is one: w32_call.
 */
#ifndef WIN32K_COMPAT_H
#define WIN32K_COMPAT_H

#include <linux/ktime.h>
#include <linux/minmax.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timekeeping.h>
#include <linux/vmalloc.h>
#include "win32k.h"
#include "handlers.h"
#include "unicode.h"
#include "windefs.h"
#include "wine_list.h"
#include "userdefs.h"

/* the call being served, set by win32k_call while it holds win32k_lock */
extern struct w32_call *w32_call;

/* the server's current thread: the W32THREAD that called; NULL in a timeout's
 * callback, as current is in the server's main loop */
#define w32_current (w32_call ? w32_call->thread : NULL)

static inline thread_id_t get_thread_id(const struct w32_thread *thread)
{
	return thread->id;
}

static inline process_id_t get_process_id(const struct w32_process *process)
{
	return process->id;
}

#define WriteRelease64(ptr, val) smp_store_release(ptr, val)

/* ------------------------------------------------------------- errors */

typedef s32 NTSTATUS;

/* no call is served in a timeout's callback (timer.c): errors go nowhere then */
static inline void set_error(NTSTATUS status)
{
	if (w32_call)
		w32_call->error = (u32)status;
}

static inline void clear_error(void)
{
	if (w32_call)
		w32_call->error = 0;
}

static inline NTSTATUS get_error(void)
{
	return w32_call ? (NTSTATUS)w32_call->error : 0;
}

/* timer.c: as the server's */
struct timeout_user;
typedef void (*timeout_callback)(void *private);
struct timeout_user *add_timeout_user(timeout_t when, timeout_callback func, void *private);
void remove_timeout_user(struct timeout_user *user);

/* ------------------------------------------------------------- memory */

/* The server's heap is malloc's: a block of any size. Here kvmalloc, small
 * blocks from the slab as before, big ones (a clipboard's bitmap) from
 * vmalloc, which kmalloc cannot give past 4 MB. All of it goes back with
 * kvfree (free), never kfree. */

/* as the server's: STATUS_NO_MEMORY when it fails */
static inline void *mem_alloc(size_t size)
{
	void *ptr = kvmalloc(size, GFP_KERNEL | __GFP_NOWARN);

	if (!ptr)
		set_error((NTSTATUS)0xC0000017 /* STATUS_NO_MEMORY */);
	return ptr;
}

/* as wineserver's: nothing to copy still gives a pointer (malloc(0)), here
 * ZERO_SIZE_PTR, which kvfree takes; NULL is a failure the server returns on
 * (an out-of-context WinEvent hook has no module and failed with no error) */
static inline void *memdup(const void *data, size_t size)
{
	void *ptr = kvmemdup(data, size, GFP_KERNEL | __GFP_NOWARN);

	if (!ptr)
		set_error((NTSTATUS)0xC0000017 /* STATUS_NO_MEMORY */);
	return ptr;
}

/* the C library's, which the server calls where it sets no error itself */
static inline void *malloc(size_t size)
{
	return kvmalloc(size, GFP_KERNEL | __GFP_NOWARN);
}

static inline void free(void *ptr)
{
	kvfree(ptr);
}

/* as realloc: NULL when it fails, the old block left as it was */
static inline void *w32_realloc(void *ptr, size_t size)
{
	return kvrealloc(ptr, size, GFP_KERNEL | __GFP_NOWARN);
}

/* ------------------------------------------------------------- the request's data */

static inline const void *get_req_data(void)
{
	return w32_call->req_data;
}

static inline data_size_t get_req_data_size(void)
{
	return w32_call->req_data_size;
}

static inline data_size_t get_reply_max_size(void)
{
	return w32_call->reply_data_max;
}

/* room for size bytes of reply data (the server allocates; here the buffer is the call's) */
static inline void *set_reply_data_size(data_size_t size)
{
	if (size > w32_call->reply_data_max) {
		set_error((NTSTATUS)0xC0000023 /* STATUS_BUFFER_TOO_SMALL */);
		return NULL;
	}
	w32_call->reply_data_size = size;
	return w32_call->reply_data;
}

static inline void set_reply_data(const void *data, data_size_t size)
{
	void *ptr = set_reply_data_size(min_t(data_size_t, size, w32_call->reply_data_max));

	if (ptr)
		memcpy(ptr, data, w32_call->reply_data_size);
}

/* the server hands over a buffer it allocated; here it is copied and freed */
static inline void set_reply_data_ptr(void *data, data_size_t size)
{
	set_reply_data(data, size);
	kvfree(data);
}

/* ------------------------------------------------------------- time, in 100 ns as Windows counts it */

typedef s64 timeout_t;
typedef s64 abstime_t;

#define TICKS_PER_SEC 10000000
#define TICKS_1601_TO_1970 ((u64)(369 * 365 + 89) * 86400 * TICKS_PER_SEC)

/* the server's monotonic_counter: CLOCK_BOOTTIME, the clock of GetTickCount
 * in ntdll too, so that the times of messages match it */
static inline timeout_t monotonic_time_now(void)
{
	return ktime_get_boottime_ns() / 100;
}

static inline unsigned int get_tick_count(void)
{
	return monotonic_time_now() / 10000;
}

static inline timeout_t current_time_now(void)
{
	return ktime_get_real_ns() / 100 + TICKS_1601_TO_1970;
}

/* the server reads them as variables, refreshed for each request; current_time
 * is a function of the kernel's own (fs.h): ported code calls current_time_now() */
#define monotonic_time monotonic_time_now()

/* an absolute time of a request (> 0) or one relative to the monotonic
 * clock (< 0), as a timeout of add_timeout_user (the server's file.h) */
static inline timeout_t abstime_to_timeout(abstime_t abstime)
{
	if (abstime > 0)
		return abstime;
	return -abstime < monotonic_time ? 0 : abstime + monotonic_time;
}

/* ------------------------------------------------------------- objects */

/* struct object and object_ops: object.h (win32k.h needs them too) */

static inline void *alloc_object(const struct object_ops *ops)
{
	struct object *obj = kzalloc(ops->size, GFP_KERNEL);

	if (!obj) {
		set_error((NTSTATUS)0xC0000017 /* STATUS_NO_MEMORY */);
		return NULL;
	}
	obj->refcount = 1;
	obj->ops = ops;
	return obj;
}

static inline struct object *grab_object(void *ptr)
{
	struct object *obj = ptr;

	obj->refcount++;
	return obj;
}

static inline void release_object(void *ptr)
{
	struct object *obj = ptr;

	if (--obj->refcount)
		return;
	if (obj->ops->destroy)
		obj->ops->destroy(obj);
	kfree(obj);
}

/* ------------------------------------------------------------- handlers */

/* as the server's DECL_HANDLER: the body takes its own request and reply; the
 * table calls the one signature (handlers.h), so no call goes through a
 * pointer of another type (kCFI) */
#define DECL_HANDLER(name) \
	static void req_##name(const struct name##_request *req, struct name##_reply *reply); \
	void w32_req_##name(const void *req, void *reply) \
	{ \
		req_##name(req, reply); \
	} \
	static void req_##name(const struct name##_request *req, struct name##_reply *reply)

static inline struct unicode_str get_req_unicode_str(void)
{
	struct unicode_str str = { get_req_data(), get_req_data_size() & ~1u };

	return str;
}

/* ------------------------------------------------------------- the session's shared memory (session.c) */

#define CONTAINING_RECORD(address, type, field) \
	((type *)((char *)(address) - offsetof(type, field)))

/* as the server writes a shared object: the sequence odd while it changes,
 * released before and after, so that a reader that saw it move reads again */
#define SHARED_WRITE_BEGIN(object_shm, type) \
	do { \
		type *shared = (type *)(object_shm); \
		shared_object_t *__obj = CONTAINING_RECORD(shared, shared_object_t, shm); \
		LONG64 __seq = READ_ONCE(__obj->seq) + 1, __end = __seq + 1; \
		WARN_ON_ONCE(!(__seq & 1)); \
		smp_store_release(&__obj->seq, __seq); \
		do

#define SHARED_WRITE_END \
		while (0); \
		smp_store_release(&__obj->seq, __end); \
	} while (0)

/* ------------------------------------------------------------- the rest */

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif
#define assert(x) WARN_ON_ONCE(!(x))
#define fprintf(stream, ...) pr_debug(__VA_ARGS__)
#define debug_level 0

#endif
