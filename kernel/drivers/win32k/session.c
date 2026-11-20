// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * win32k: the session's shared memory of USER (from Wine's server/mapping.c)
 *
 * Copyright (C) 2023 Rémi Bernon for CodeWeavers (the session in the server)
 *
 * What programs read of USER without a call (the table of USER handles,
 * desktops, queues, input, classes, windows) lies in one memory every NT
 * process maps read-only, as Windows maps win32k's shared section
 * (gSharedInfo) into each process. Here it is a shmem file of the kernel:
 * win32k writes it through a kernel mapping of each block, a process maps
 * the file as a whole from the fd win32k_session gives, read-only.
 *
 * Objects are written as the server writes them (SHARED_WRITE_BEGIN): the
 * sequence is odd while a write is under way, so that a reader that saw it
 * change reads again. Unlike in the server, the allocator's own records are
 * kept in the kernel's memory, not in the shared one.
 */

#include <linux/file.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/shmem_fs.h>
#include <linux/syscalls.h>
#include <linux/xarray.h>
#include "user.h"

#define SESSION_MAX	(64u << 20)	/* reserved; pages come as objects need them */
#define SESSION_BLOCK_SIZE	(4u << 20)	/* mapped into the kernel a block at a time */
#define NB_BLOCKS	(SESSION_MAX / SESSION_BLOCK_SIZE)

struct free_object
{
	struct list entry;
	u32 offset;			/* of its shared_object_t in the session */
	u32 size;
};

static struct file *session_file;
static char *blocks[NB_BLOCKS];		/* kernel addresses of the blocks mapped so far */
static u32 block_used[NB_BLOCKS];
static unsigned int nb_blocks;
static struct list free_objects = LIST_INIT(free_objects);
static DEFINE_XARRAY(object_sizes);	/* offset → size of each object handed out */
static object_id_t last_object_id;

session_shm_t *shared_session;

/* the pages of block i, read in (zero) and kept, mapped into the kernel */
static bool map_block(unsigned int i)
{
	unsigned int count = SESSION_BLOCK_SIZE / PAGE_SIZE;
	struct page **pages;
	void *addr;

	if (!(pages = kvmalloc_array(count, sizeof(*pages), GFP_KERNEL)))
		return false;
	for (unsigned int n = 0; n < count; n++) {
		struct folio *folio = shmem_read_folio(session_file->f_mapping, (pgoff_t)i * count + n);

		if (IS_ERR(folio)) {
			while (n--)
				put_page(pages[n]);
			kvfree(pages);
			return false;
		}
		pages[n] = folio_page(folio, 0);
	}
	addr = vmap(pages, count, VM_MAP, PAGE_KERNEL);
	kvfree(pages);	/* vmap keeps its own; the references keep the pages */
	if (!addr)
		return false;
	blocks[i] = addr;
	return true;
}

int win32k_session_init(void)
{
	vma_flags_t flags = EMPTY_VMA_FLAGS;

	vma_flags_set(&flags, VMA_NORESERVE_BIT);	/* pages are charged as they are used */
	session_file = shmem_kernel_file_setup("win32k-session", SESSION_MAX, flags);
	if (IS_ERR(session_file))
		return PTR_ERR(session_file);
	if (!map_block(0))
		return -ENOMEM;
	nb_blocks = 1;
	shared_session = (session_shm_t *)blocks[0];
	block_used[0] = round_up(sizeof(*shared_session), 64);
	return 0;
}

static u32 object_offset(const volatile void *obj)
{
	for (unsigned int i = 0; i < nb_blocks; i++)
		if ((const char *)obj >= blocks[i] && (const char *)obj < blocks[i] + SESSION_BLOCK_SIZE)
			return i * SESSION_BLOCK_SIZE + (u32)((const char *)obj - blocks[i]);
	WARN_ON_ONCE(1);
	return 0;
}

static shared_object_t *object_at(u32 offset)
{
	return (shared_object_t *)(blocks[offset / SESSION_BLOCK_SIZE] + offset % SESSION_BLOCK_SIZE);
}

volatile void *alloc_shared_object(mem_size_t shm_size)
{
	u32 size = round_up(offsetof(shared_object_t, shm) + max_t(mem_size_t, shm_size, sizeof(object_shm_t)), 8);
	struct free_object *free;
	shared_object_t *obj = NULL;
	u32 offset = 0;

	LIST_FOR_EACH_ENTRY(free, &free_objects, struct free_object, entry) {
		if (free->size != size)
			continue;
		list_remove(&free->entry);
		offset = free->offset;
		kfree(free);
		obj = object_at(offset);
		break;
	}
	if (!obj) {
		unsigned int i;

		for (i = 0; i < nb_blocks; i++)
			if (block_used[i] + size <= SESSION_BLOCK_SIZE)
				break;
		if (i == nb_blocks) {
			if (i == NB_BLOCKS || size > SESSION_BLOCK_SIZE || !map_block(i)) {
				set_error(STATUS_NO_MEMORY);
				return NULL;
			}
			nb_blocks++;
		}
		offset = i * SESSION_BLOCK_SIZE + block_used[i];
		block_used[i] += size;
		obj = object_at(offset);
		if (xa_err(xa_store(&object_sizes, offset, xa_mk_value(size), GFP_KERNEL))) {
			block_used[i] -= size;
			set_error(STATUS_NO_MEMORY);
			return NULL;
		}
	}

	SHARED_WRITE_BEGIN(&obj->shm, object_shm_t)
	{
		memset((void *)shared, 0, size - offsetof(shared_object_t, shm));
		CONTAINING_RECORD(shared, shared_object_t, shm)->id = ++last_object_id;
	}
	SHARED_WRITE_END;
	return &obj->shm;
}

void free_shared_object(volatile void *object_shm)
{
	shared_object_t *obj = CONTAINING_RECORD(object_shm, shared_object_t, shm);
	u32 offset = object_offset(obj);
	struct free_object *free = kmalloc(sizeof(*free), GFP_KERNEL);

	SHARED_WRITE_BEGIN(&obj->shm, object_shm_t)
	{
		CONTAINING_RECORD(shared, shared_object_t, shm)->id = 0;
	}
	SHARED_WRITE_END;
	if (!free)
		return;	/* the object is lost to the allocator, not to the session */
	free->offset = offset;
	free->size = xa_to_value(xa_load(&object_sizes, offset));
	wlist_add_tail(&free_objects, &free->entry);
}

/* invalidate client caches for a shared object by giving it a new id */
void invalidate_shared_object(volatile void *object_shm)
{
	SHARED_WRITE_BEGIN(object_shm, object_shm_t)
	{
		CONTAINING_RECORD(shared, shared_object_t, shm)->id = ++last_object_id;
	}
	SHARED_WRITE_END;
}

struct obj_locator get_shared_object_locator(const volatile void *object_shm)
{
	const shared_object_t *obj = CONTAINING_RECORD(object_shm, shared_object_t, shm);
	struct obj_locator locator = { .offset = object_offset(obj), .id = obj->id };

	return locator;
}

/* win32k_session: the session's shared memory, read-only, for the process to map */
SYSCALL_DEFINE0(win32k_session)
{
	if (!w32_task_thread(current))
		return -ESRCH;
	return FD_ADD(O_CLOEXEC, dentry_open(&session_file->f_path, O_RDONLY | O_LARGEFILE, current_cred()));
}
