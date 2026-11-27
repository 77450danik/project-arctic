// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * Server-side file descriptor management
 *
 * Copyright (C) 2000, 2003 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "ntos.h"  /* ntos: all that the server's headers and the C library gave */

#ifdef HAVE_LINUX_MAJOR_H
#endif
#ifdef HAVE_SYS_STATVFS_H
#endif
#ifdef HAVE_SYS_VFS_H
/* Work around a conflict with Solaris' system list defined in sys/list.h. */
#define list SYSLIST
#define list_next SYSLIST_NEXT
#define list_prev SYSLIST_PREV
#define list_head SYSLIST_HEAD
#define list_tail SYSLIST_TAIL
#define list_move_tail SYSLIST_MOVE_TAIL
#define list_remove SYSLIST_REMOVE
#undef list
#undef list_next
#undef list_prev
#undef list_head
#undef list_tail
#undef list_move_tail
#undef list_remove
#endif
#ifdef HAVE_SYS_PARAM_H
#endif
#ifdef HAVE_SYS_MOUNT_H
#endif
#ifdef HAVE_SYS_STATFS_H
#endif
#ifdef HAVE_SYS_SYSCTL_H
#endif
#ifdef HAVE_SYS_EVENT_H
#undef LIST_INIT
#undef LIST_ENTRY
#endif
#ifdef MAJOR_IN_MKDEV
#elif defined(MAJOR_IN_SYSMACROS)
#endif
#ifdef HAVE_SYS_SYSCALL_H
#endif
#ifdef HAVE_SYS_XATTR_H
#endif
#ifdef HAVE_SYS_EXTATTR_H
#undef XATTR_ADDITIONAL_OPTIONS
#endif

#if defined(HAVE_SYS_EPOLL_H) && defined(HAVE_EPOLL_CREATE)
# define USE_EPOLL
#endif /* HAVE_SYS_EPOLL_H && HAVE_EPOLL_CREATE */

#if defined(HAVE_PORT_H) && defined(HAVE_PORT_CREATE)
# define USE_EVENT_PORTS
#endif /* HAVE_PORT_H && HAVE_PORT_CREATE */

/* Because of the stupid Posix locking semantics, we need to keep
 * track of all file descriptors referencing a given file, and not
 * close a single one until all the locks are gone (sigh).
 */

/* file descriptor object */

/* closed_fd is used to keep track of the unix fd belonging to a closed fd object */
struct closed_fd
{
    struct list     entry;      /* entry in inode closed list */
    int             unix_fd;    /* the unix file descriptor */
    unsigned int    disp_flags; /* the disposition flags */
    char           *unix_name;  /* name to unlink on close, points to parent fd unix_name */
};

struct nt_fd
{
    struct object        obj;         /* object header */
    const struct fd_ops *fd_ops;      /* file descriptor operations */
    struct object       *sync;        /* sync object for wait/signal */
    struct nt_inode        *inode;       /* inode that this fd belongs to */
    struct list          inode_entry; /* entry in inode fd list */
    struct closed_fd    *closed;      /* structure to store the unix fd at destroy time */
    struct object       *user;        /* object using this file descriptor */
    struct list          locks;       /* list of locks on this fd */
    client_ptr_t         map_addr;    /* default mapping address for PE files */
    mem_size_t           map_size;    /* mapping size for PE files */
    unsigned int         access;      /* file access (FILE_READ_DATA etc.) */
    unsigned int         options;     /* file options (FILE_DELETE_ON_CLOSE, FILE_SYNCHRONOUS...) */
    unsigned int         sharing;     /* file sharing mode */
    char                *unix_name;   /* unix file name */
    WCHAR               *nt_name;     /* NT file name */
    data_size_t          nt_namelen;  /* length of NT file name */
    int                  unix_fd;     /* unix file descriptor */
    unsigned int         no_fd_status;/* status to return when unix_fd is -1 */
    unsigned int         cacheable :1;/* can the fd be cached on the client side? */
    unsigned int         fs_locks :1; /* can we use filesystem locks for this fd? */
    int                  poll_index;  /* index of fd in poll array */
    struct async_queue   read_q;      /* async readers of this fd */
    struct async_queue   write_q;     /* async writers of this fd */
    struct async_queue   wait_q;      /* other async waiters of this fd */
    struct nt_completion   *completion;  /* completion object attached to this fd */
    apc_param_t          comp_key;    /* completion key to set in completion events */
    unsigned int         comp_flags;  /* completion flags */
};

static void fd_dump( struct object *obj, int verbose );
static struct object *fd_get_sync( struct object *obj );
static void fd_destroy( struct object *obj );

static const struct object_ops fd_ops =
{
    .size     = sizeof(struct nt_fd),
    .type     = &no_type,
    .dump     = fd_dump,
    .get_sync = fd_get_sync,
    .destroy  = fd_destroy,
};

/* device object */

#define DEVICE_HASH_SIZE 7
#define INODE_HASH_SIZE 17

struct nt_device
{
    struct object       obj;        /* object header */
    struct list         entry;      /* entry in device hash list */
    dev_t               dev;        /* device number */
    int                 removable;  /* removable device? (or -1 if unknown) */
    struct list         inode_hash[INODE_HASH_SIZE];  /* inodes hash table */
};

static void device_dump( struct object *obj, int verbose );
static void nt_device_destroy( struct object *obj );

static const struct object_ops device_ops =
{
    .size    = sizeof(struct nt_device),
    .type    = &no_type,
    .dump    = device_dump,
    .destroy = nt_device_destroy,
};

/* inode object */

struct nt_inode
{
    struct object       obj;        /* object header */
    struct list         entry;      /* inode hash list entry */
    struct nt_device      *device;     /* device containing this inode */
    ino_t               ino;        /* inode number */
    struct list         open;       /* list of open file descriptors */
    struct list         locks;      /* list of file locks */
    struct list         closed;     /* list of file descriptors to close at destroy time */
};

static void inode_dump( struct object *obj, int verbose );
static void inode_destroy( struct object *obj );

static const struct object_ops inode_ops =
{
    .size    = sizeof(struct nt_inode),
    .type    = &no_type,
    .dump    = inode_dump,
    .destroy = inode_destroy,
};

/* file lock object */

struct nt_file_lock
{
    struct object       obj;         /* object header */
    struct object      *sync;        /* sync object for wait/signal */
    struct nt_fd          *fd;          /* fd owning this lock */
    struct list         fd_entry;    /* entry in list of locks on a given fd */
    struct list         inode_entry; /* entry in inode list of locks */
    int                 shared;      /* shared lock? */
    file_pos_t          start;       /* locked region is interval [start;end) */
    file_pos_t          end;
    struct process     *process;     /* process owning this lock */
    struct list         proc_entry;  /* entry in list of locks owned by the process */
};

static void file_lock_dump( struct object *obj, int verbose );
static struct object *file_lock_get_sync( struct object *obj );
static void file_lock_destroy( struct object *obj );

static const struct object_ops file_lock_ops =
{
    .size     = sizeof(struct nt_file_lock),
    .type     = &no_type,
    .dump     = file_lock_dump,
    .get_sync = file_lock_get_sync,
    .destroy  = file_lock_destroy,
};

#define OFF_T_MAX       (~((file_pos_t)1 << (8*sizeof(off_t)-1)))
#define FILE_POS_T_MAX  (~(file_pos_t)0)

static file_pos_t max_unix_offset = OFF_T_MAX;

#define DUMP_LONG_LONG(val) do { \
    if (sizeof(val) > sizeof(unsigned long) && (val) > ~0UL) \
        nt_fprintf( stderr, "%lx%08lx", (unsigned long)((unsigned long long)(val) >> 32), (unsigned long)(val) ); \
    else \
        nt_fprintf( stderr, "%lx", (unsigned long)(val) ); \
  } while (0)

/****************************************************************/
/* timeouts support */

struct timeout_user
{
    struct list           entry;      /* entry in sorted timeout list */
    abstime_t             when;       /* timeout expiry */
    timeout_callback      callback;   /* callback function */
    void                 *private;    /* callback private data */
};

static struct list abs_timeout_list = LIST_INIT(abs_timeout_list); /* sorted absolute timeouts list */
static struct list rel_timeout_list = LIST_INIT(rel_timeout_list); /* sorted relative timeouts list */
timeout_t nt_current_time;
timeout_t nt_monotonic_time;

struct _KUSER_SHARED_DATA *user_shared_data = NULL;
static const timeout_t user_shared_data_timeout = 16 * 10000;

static void atomic_store_ulong(volatile ULONG *ptr, ULONG value)
{
    /* on x86 there should be total store order guarantees, so volatile is
     * enough to ensure the stores aren't reordered by the compiler, and then
     * they will always be seen in-order from other CPUs. On other archs, we
     * need atomic intrinsics to guarantee that. */
#if defined(__i386__) || defined(__x86_64__)
    *ptr = value;
#else
    __atomic_store_n(ptr, value, __ATOMIC_SEQ_CST);
#endif
}

static void atomic_store_long(volatile LONG *ptr, LONG value)
{
#if defined(__i386__) || defined(__x86_64__)
    *ptr = value;
#else
    __atomic_store_n(ptr, value, __ATOMIC_SEQ_CST);
#endif
}

static void set_user_shared_data_time(void)
{
    timeout_t tick_count = nt_monotonic_time / 10000;
    static timeout_t last_timezone_update;
    timeout_t timezone_bias;

    if (nt_monotonic_time - last_timezone_update > TICKS_PER_SEC)
    {
        /* ntos: UTC minus local time now, summer time included, from the
         * zone's file (/etc/localtime) as localtime and mktime had it */
        timezone_bias = (timeout_t)nt_timezone_bias( nt_time( NULL ) ) * TICKS_PER_SEC;

        atomic_store_long(&user_shared_data->TimeZoneBias.High2Time, timezone_bias >> 32);
        atomic_store_ulong(&user_shared_data->TimeZoneBias.LowPart, timezone_bias);
        atomic_store_long(&user_shared_data->TimeZoneBias.High1Time, timezone_bias >> 32);

        last_timezone_update = nt_monotonic_time;
    }

    atomic_store_long(&user_shared_data->SystemTime.High2Time, nt_current_time >> 32);
    atomic_store_ulong(&user_shared_data->SystemTime.LowPart, nt_current_time);
    atomic_store_long(&user_shared_data->SystemTime.High1Time, nt_current_time >> 32);

    atomic_store_long(&user_shared_data->InterruptTime.High2Time, nt_monotonic_time >> 32);
    atomic_store_ulong(&user_shared_data->InterruptTime.LowPart, nt_monotonic_time);
    atomic_store_long(&user_shared_data->InterruptTime.High1Time, nt_monotonic_time >> 32);

    atomic_store_long(&user_shared_data->TickCount.High2Time, tick_count >> 32);
    atomic_store_ulong(&user_shared_data->TickCount.LowPart, tick_count);
    atomic_store_long(&user_shared_data->TickCount.High1Time, tick_count >> 32);
    atomic_store_ulong(&user_shared_data->TickCountLowDeprecated, tick_count);
}

void set_current_time(void)
{
    /* ntos: CLOCK_REALTIME and the monotonic counter, as gettimeofday gave */
    nt_current_time = nt_realtime_ticks();
    nt_monotonic_time = monotonic_counter();
    if (user_shared_data) set_user_shared_data_time();
}

/* add a timeout user */
struct timeout_user *add_timeout_user( timeout_t when, timeout_callback func, void *private )
{
    struct timeout_user *user;
    struct list *ptr;

    if (!(user = mem_alloc( sizeof(*user) ))) return NULL;
    user->when     = timeout_to_abstime( when );
    user->callback = func;
    user->private  = private;

    /* Now insert it in the linked list */

    if (user->when > 0)
    {
        LIST_FOR_EACH( ptr, &abs_timeout_list )
        {
            struct timeout_user *timeout = LIST_ENTRY( ptr, struct timeout_user, entry );
            if (timeout->when >= user->when) break;
        }
    }
    else
    {
        LIST_FOR_EACH( ptr, &rel_timeout_list )
        {
            struct timeout_user *timeout = LIST_ENTRY( ptr, struct timeout_user, entry );
            if (timeout->when <= user->when) break;
        }
    }
    wlist_add_before( ptr, &user->entry );
    /* ntos: the kernel's timer, if this one is due before it */
    nt_timeout_arm_before( user->when > 0 ? (user->when - nt_current_time) * 100 : (-user->when - nt_monotonic_time) * 100 );
    return user;
}

/* remove a timeout user */
void remove_timeout_user( struct timeout_user *user )
{
    wlist_remove( &user->entry );
    nt_free( user );
}

/* return a text description of a timeout for debugging purposes */
const char *get_timeout_str( timeout_t timeout )
{
    static char buffer[64];
    long secs, nsecs;

    if (!timeout) return "0";
    if (timeout == TIMEOUT_INFINITE) return "infinite";

    if (timeout < 0)  /* relative */
    {
        secs = -timeout / TICKS_PER_SEC;
        nsecs = -timeout % TICKS_PER_SEC;
        snprintf( buffer, sizeof(buffer), "+%ld.%07ld", secs, nsecs );
    }
    else  /* absolute */
    {
        secs = (timeout - nt_current_time) / TICKS_PER_SEC;
        nsecs = (timeout - nt_current_time) % TICKS_PER_SEC;
        if (nsecs < 0)
        {
            nsecs += TICKS_PER_SEC;
            secs--;
        }
        if (secs >= 0)
            snprintf( buffer, sizeof(buffer), "%x%08x (+%ld.%07ld)",
                      (unsigned int)(timeout >> 32), (unsigned int)timeout, secs, nsecs );
        else
            snprintf( buffer, sizeof(buffer), "%x%08x (-%ld.%07ld)",
                      (unsigned int)(timeout >> 32), (unsigned int)timeout,
                      -(secs + 1), TICKS_PER_SEC - nsecs );
    }
    return buffer;
}

/****************************************************************/
/* poll support */

static struct nt_fd **poll_users;              /* users array */
static struct pollfd *pollfd;               /* poll fd array */
static int nb_users;                        /* count of array entries actually in use */
static int active_users;                    /* nt_current number of active users */
static int allocated_users;                 /* count of allocated entries in the array */
static struct nt_fd **freelist;                /* list of free entries in the array */

static int get_next_timeout( struct timespec *ts );

static inline void fd_poll_event( struct nt_fd *fd, int event )
{
    if (fd->fd_ops->poll_event) fd->fd_ops->poll_event( fd, event );
    else default_poll_event( fd, event );
}

/* ntos: what epoll did, a watch of the kernel's on the file of each poll
 * user (crt.c): its wakeup marks the user ready and has the executive's
 * worker call ntos_poll_dispatch, which gives the events as the main loop
 * gave them, again while they last (epoll was level-triggered too) */
static inline void init_epoll(void) { }

static inline void set_fd_epoll_events( struct nt_fd *fd, int user, int events )
{
    if (events == -1) nt_poll_watch( user, -1, 0 );
    else nt_poll_watch( user, fd->unix_fd, events );
}

static inline void remove_epoll_user( struct nt_fd *fd, int user )
{
    nt_poll_watch( user, -1, 0 );
}

int has_poll_events(void)
{
    return 0;
}


/* add a user in the poll array and return its index, or -1 on failure */
static int add_poll_user( struct nt_fd *fd )
{
    int ret;
    if (freelist)
    {
        ret = freelist - poll_users;
        freelist = (struct nt_fd **)poll_users[ret];
    }
    else
    {
        if (nb_users == allocated_users)
        {
            struct nt_fd **newusers;
            struct pollfd *newpoll;
            int new_count = allocated_users ? (allocated_users + allocated_users / 2) : 16;
            if (!(newusers = nt_realloc( poll_users, new_count * sizeof(*poll_users) ))) return -1;
            if (!(newpoll = nt_realloc( pollfd, new_count * sizeof(*pollfd) )))
            {
                if (allocated_users)
                    poll_users = newusers;
                else
                    nt_free( newusers );
                return -1;
            }
            poll_users = newusers;
            pollfd = newpoll;
            if (!allocated_users) init_epoll();
            allocated_users = new_count;
        }
        ret = nb_users++;
    }
    pollfd[ret].fd = -1;
    pollfd[ret].events = 0;
    pollfd[ret].revents = 0;
    poll_users[ret] = fd;
    active_users++;
    return ret;
}

/* remove a user from the poll list */
static void remove_poll_user( struct nt_fd *fd, int user )
{
    assert( user >= 0 );
    assert( poll_users[user] == fd );

    remove_epoll_user( fd, user );
    pollfd[user].fd = -1;
    pollfd[user].events = 0;
    pollfd[user].revents = 0;
    poll_users[user] = (struct nt_fd *)freelist;
    freelist = &poll_users[user];
    active_users--;
}

/* process pending timeouts and return the time until the next timeout in milliseconds,
 * and full nanosecond precision in the timespec parameter if given */
static int get_next_timeout( struct timespec *ts )
{
    timeout_t ret = user_shared_data ? user_shared_data_timeout : -1;

    if (!wlist_empty( &abs_timeout_list ) || !wlist_empty( &rel_timeout_list ))
    {
        struct list expired_list, *ptr;

        /* first remove all expired timers from the list */

        wlist_init( &expired_list );
        while ((ptr = wlist_head( &abs_timeout_list )) != NULL)
        {
            struct timeout_user *timeout = LIST_ENTRY( ptr, struct timeout_user, entry );

            if (timeout->when <= nt_current_time)
            {
                wlist_remove( &timeout->entry );
                wlist_add_tail( &expired_list, &timeout->entry );
            }
            else break;
        }
        while ((ptr = wlist_head( &rel_timeout_list )) != NULL)
        {
            struct timeout_user *timeout = LIST_ENTRY( ptr, struct timeout_user, entry );

            if (-timeout->when <= nt_monotonic_time)
            {
                wlist_remove( &timeout->entry );
                wlist_add_tail( &expired_list, &timeout->entry );
            }
            else break;
        }

        /* now call the callback for all the removed timers */

        while ((ptr = wlist_head( &expired_list )) != NULL)
        {
            struct timeout_user *timeout = LIST_ENTRY( ptr, struct timeout_user, entry );
            const void *fn = timeout->callback;
            u64 c0 = ntos_cpu_ns(), t0 = ktime_get_ns();

            wlist_remove( &timeout->entry );
            timeout->callback( timeout->private );
            ntos_note_hold( -1, fn, ktime_get_ns() - t0, ntos_cpu_ns() - c0 );
            nt_free( timeout );
        }

        if ((ptr = wlist_head( &abs_timeout_list )) != NULL)
        {
            struct timeout_user *timeout = LIST_ENTRY( ptr, struct timeout_user, entry );
            timeout_t diff = timeout->when - nt_current_time;
            if (diff < 0) diff = 0;
            if (ret == -1 || diff < ret) ret = diff;
        }

        if ((ptr = wlist_head( &rel_timeout_list )) != NULL)
        {
            struct timeout_user *timeout = LIST_ENTRY( ptr, struct timeout_user, entry );
            timeout_t diff = -timeout->when - nt_monotonic_time;
            if (diff < 0) diff = 0;
            if (ret == -1 || diff < ret) ret = diff;
        }
    }

    /* infinite */
    if (ret == -1) return -1;

    if (ts)
    {
        ts->tv_sec = ret / TICKS_PER_SEC;
        ts->tv_nsec = (ret % TICKS_PER_SEC) * 100;
    }

    /* convert to milliseconds, ceil to avoid spinning with 0 timeout */
    ret = (ret + 9999) / 10000;
    if (ret > INT_MAX) ret = INT_MAX;
    return ret;
}

/* ntos: one turn of the server's main loop, under the executive lock, when
 * a watch woke or a timeout is due (crt.c): the time, the expired timeouts,
 * the events of the users that are ready; then the next timeout for the
 * kernel's timer */
void ntos_poll_dispatch(void)
{
    struct timespec ts;
    int user, events, timeout;

    set_current_time();
    if (!server_start_time) server_start_time = nt_current_time;
    get_next_timeout( NULL );
    while (nt_poll_ready( &user, &events ))
    {
        if (user < nb_users && poll_users[user] && pollfd[user].fd != -1 &&
            (events &= pollfd[user].events | POLLERR | POLLHUP))
        {
            /* whose callback, before it may free the descriptor */
            struct nt_fd *fd = poll_users[user];
            const void *fn = fd->fd_ops->poll_event ? (const void *)fd->fd_ops->poll_event
                                                    : (const void *)default_poll_event;
            u64 c0 = ntos_cpu_ns(), t0 = ktime_get_ns();

            fd_poll_event( fd, events );
            ntos_note_hold( -1, fn, ktime_get_ns() - t0, ntos_cpu_ns() - c0 );
        }
    }
    timeout = get_next_timeout( &ts );
    nt_timeout_arm( timeout == -1 ? -1 : (long long)ts.tv_sec * 1000000000ll + ts.tv_nsec );
}

/****************************************************************/
/* device functions */

static struct list device_hash[DEVICE_HASH_SIZE];

static int is_device_removable( dev_t dev, int unix_fd )
{
#if defined(linux) && defined(HAVE_FSTATFS)
    struct statfs stfs;

    /* check for floppy disk */
    if (major(dev) == FLOPPY_MAJOR) return 1;

    if (nt_fstatfs( unix_fd, &stfs ) == -1) return 0;
    return (stfs.f_type == 0x9660 ||    /* iso9660 */
            stfs.f_type == 0x9fa1 ||    /* supermount */
            stfs.f_type == 0x15013346); /* udf */
#elif defined(__FreeBSD__) || defined(__FreeBSD_kernel__) || defined(__DragonFly__) || defined(__APPLE__)
    struct statfs stfs;

    if (nt_fstatfs( unix_fd, &stfs ) == -1) return 0;
    return (!strcmp("cd9660", stfs.f_fstypename) || !strcmp("udf", stfs.f_fstypename));
#elif defined(__NetBSD__)
    struct statvfs stfs;

    if (fstatvfs( unix_fd, &stfs ) == -1) return 0;
    return (!strcmp("cd9660", stfs.f_fstypename) || !strcmp("udf", stfs.f_fstypename));
#elif defined(sun)
    struct dk_cinfo dkinf;
    if (nt_ioctl( unix_fd, DKIOCINFO, &dkinf ) == -1) return 0;
    return (dkinf.dki_ctype == DKC_CDROM ||
            dkinf.dki_ctype == DKC_NCRFLOPPY ||
            dkinf.dki_ctype == DKC_SMSFLOPPY ||
            dkinf.dki_ctype == DKC_INTEL82072 ||
            dkinf.dki_ctype == DKC_INTEL82077);
#else
    return 0;
#endif
}

/* retrieve the device object for a given fd, creating it if needed */
static struct nt_device *nt_get_device( dev_t dev, int unix_fd )
{
    struct nt_device *device;
    unsigned int i, hash = dev % DEVICE_HASH_SIZE;

    if (device_hash[hash].next)
    {
        LIST_FOR_EACH_ENTRY( device, &device_hash[hash], struct nt_device, entry )
            if (device->dev == dev) return (struct nt_device *)grab_object( device );
    }
    else wlist_init( &device_hash[hash] );

    /* not found, create it */

    if (unix_fd == -1) return NULL;
    if ((device = alloc_object( &device_ops )))
    {
        device->dev = dev;
        device->removable = is_device_removable( dev, unix_fd );
        for (i = 0; i < INODE_HASH_SIZE; i++) wlist_init( &device->inode_hash[i] );
        wlist_add_head( &device_hash[hash], &device->entry );
    }
    return device;
}

static void device_dump( struct object *obj, int verbose )
{
    struct nt_device *device = (struct nt_device *)obj;
    nt_fprintf( stderr, "Device dev=" );
    DUMP_LONG_LONG( device->dev );
    nt_fprintf( stderr, "\n" );
}

static void nt_device_destroy( struct object *obj )
{
    struct nt_device *device = (struct nt_device *)obj;
    unsigned int i;

    for (i = 0; i < INODE_HASH_SIZE; i++)
        assert( wlist_empty(&device->inode_hash[i]) );

    wlist_remove( &device->entry );  /* remove it from the hash table */
}

/****************************************************************/
/* inode functions */

static void unlink_closed_fd( struct nt_inode *inode, struct closed_fd *fd )
{
    /* make sure it is still the same file */
    struct nt_stat st;
    if (!nt_stat( fd->unix_name, &st ) && st.st_dev == inode->device->dev && st.st_ino == inode->ino)
    {
        if (S_ISDIR(st.st_mode)) nt_rmdir( fd->unix_name );
        else nt_unlink( fd->unix_name );
    }
}

/* close all pending file descriptors in the closed list */
static void inode_close_pending( struct nt_inode *inode, int keep_unlinks )
{
    struct list *ptr = wlist_head( &inode->closed );

    while (ptr)
    {
        struct closed_fd *fd = LIST_ENTRY( ptr, struct closed_fd, entry );
        struct list *next = wlist_next( &inode->closed, ptr );

        if (fd->unix_fd != -1)
        {
            nt_close( fd->unix_fd );
            fd->unix_fd = -1;
        }

        /* get rid of it unless there's an unlink pending on that file */
        if (!keep_unlinks || !(fd->disp_flags & FILE_DISPOSITION_DELETE))
        {
            wlist_remove( ptr );
            nt_free( fd->unix_name );
            nt_free( fd );
        }
        ptr = next;
    }
}

static void inode_dump( struct object *obj, int verbose )
{
    struct nt_inode *inode = (struct nt_inode *)obj;
    nt_fprintf( stderr, "Inode device=%p ino=", inode->device );
    DUMP_LONG_LONG( inode->ino );
    nt_fprintf( stderr, "\n" );
}

static void inode_destroy( struct object *obj )
{
    struct nt_inode *inode = (struct nt_inode *)obj;
    struct list *ptr;

    assert( wlist_empty(&inode->open) );
    assert( wlist_empty(&inode->locks) );

    wlist_remove( &inode->entry );

    while ((ptr = wlist_head( &inode->closed )))
    {
        struct closed_fd *fd = LIST_ENTRY( ptr, struct closed_fd, entry );
        wlist_remove( ptr );
        if (fd->unix_fd != -1) nt_close( fd->unix_fd );
        if (fd->disp_flags & FILE_DISPOSITION_DELETE)
            unlink_closed_fd( inode, fd );
        nt_free( fd->unix_name );
        nt_free( fd );
    }
    release_object( inode->device );
}

/* retrieve the inode object for a given fd, creating it if needed */
static struct nt_inode *get_inode( dev_t dev, ino_t ino, int unix_fd )
{
    struct nt_device *device;
    struct nt_inode *inode;
    unsigned int hash = ino % INODE_HASH_SIZE;

    if (!(device = nt_get_device( dev, unix_fd ))) return NULL;

    LIST_FOR_EACH_ENTRY( inode, &device->inode_hash[hash], struct nt_inode, entry )
    {
        if (inode->ino == ino)
        {
            release_object( device );
            return (struct nt_inode *)grab_object( inode );
        }
    }

    /* not found, create it */
    if ((inode = alloc_object( &inode_ops )))
    {
        inode->device = device;
        inode->ino    = ino;
        wlist_init( &inode->open );
        wlist_init( &inode->locks );
        wlist_init( &inode->closed );
        wlist_add_head( &device->inode_hash[hash], &inode->entry );
    }
    else release_object( device );

    return inode;
}

static int inode_has_pending_close( struct nt_inode *inode, const char *path )
{
    struct closed_fd *fd;

    LIST_FOR_EACH_ENTRY( fd, &inode->closed, struct closed_fd, entry )
        if (!strcmp( fd->unix_name, path )) return 1;
    return 0;
}

/* add fd to the inode list of file descriptors to close */
static void inode_add_closed_fd( struct nt_inode *inode, struct closed_fd *fd )
{
    if (!wlist_empty( &inode->locks ))
    {
        wlist_add_head( &inode->closed, &fd->entry );
    }
    else if ((fd->disp_flags & FILE_DISPOSITION_DELETE) &&
        (fd->disp_flags & FILE_DISPOSITION_POSIX_SEMANTICS))
    {
        /* close the fd and unlink it at once */
        if (fd->unix_fd != -1) nt_close( fd->unix_fd );
        unlink_closed_fd( inode, fd );
        nt_free( fd->unix_name );
        nt_free( fd );
    }
    else if ((fd->disp_flags & FILE_DISPOSITION_DELETE) && !inode_has_pending_close( inode, fd->unix_name ))
    {
        /* close the fd but keep the structure around for unlink */
        if (fd->unix_fd != -1) nt_close( fd->unix_fd );
        fd->unix_fd = -1;
        wlist_add_head( &inode->closed, &fd->entry );
    }
    else  /* no locks on this inode and no unlink, get rid of the fd */
    {
        if (fd->unix_fd != -1) nt_close( fd->unix_fd );
        nt_free( fd->unix_name );
        nt_free( fd );
    }
}

/****************************************************************/
/* file lock functions */

static void file_lock_dump( struct object *obj, int verbose )
{
    struct nt_file_lock *lock = (struct nt_file_lock *)obj;
    nt_fprintf( stderr, "Lock %s fd=%p proc=%p start=",
             lock->shared ? "shared" : "excl", lock->fd, lock->process );
    DUMP_LONG_LONG( lock->start );
    nt_fprintf( stderr, " end=" );
    DUMP_LONG_LONG( lock->end );
    nt_fprintf( stderr, "\n" );
}

static struct object *file_lock_get_sync( struct object *obj )
{
    struct nt_file_lock *lock = (struct nt_file_lock *)obj;
    assert( obj->ops == &file_lock_ops );
    return grab_object( lock->sync );
}

static void file_lock_destroy( struct object *obj )
{
    struct nt_file_lock *lock = (struct nt_file_lock *)obj;
    assert( obj->ops == &file_lock_ops );
    if (lock->sync) release_object( lock->sync );
}

/* set (or remove) a Unix lock if possible for the given range */
static int set_unix_lock( struct nt_fd *fd, file_pos_t start, file_pos_t end, int type )
{
    struct flock fl;

    if (!fd->fs_locks) return 1;  /* no fs locks possible for this fd */
    for (;;)
    {
        if (start == end) return 1;  /* can't set zero-byte lock */
        if (start > max_unix_offset) return 1;  /* ignore it */
        fl.l_type   = type;
        fl.l_whence = SEEK_SET;
        fl.l_start  = start;
        if (!end || end > max_unix_offset) fl.l_len = 0;
        else fl.l_len = end - start;
        if (nt_fcntl( fd->unix_fd, F_SETLK, &fl ) != -1) return 1;

        switch(errno)
        {
        case EACCES:
            /* check whether locks work at all on this file system */
            if (nt_fcntl( fd->unix_fd, F_GETLK, &fl ) != -1)
            {
                set_error( STATUS_FILE_LOCK_CONFLICT );
                return 0;
            }
            fallthrough;
        case EIO:
        case ENOLCK:
        case ENOTSUP:
            /* no locking on this fs, just ignore it */
            fd->fs_locks = 0;
            return 1;
        case EAGAIN:
            set_error( STATUS_FILE_LOCK_CONFLICT );
            return 0;
        case EBADF:
            /* this can happen if we try to set a write lock on a read-only file */
            /* try to at least grab a read lock */
            if (fl.l_type == F_WRLCK)
            {
                type = F_RDLCK;
                break; /* retry */
            }
            set_error( STATUS_ACCESS_DENIED );
            return 0;
#ifdef EOVERFLOW
        case EOVERFLOW:
#endif
        case EINVAL:
            /* this can happen if off_t is 64-bit but the kernel only supports 32-bit */
            /* in that case we shrink the limit and retry */
            if (max_unix_offset > INT_MAX)
            {
                max_unix_offset = INT_MAX;
                break;  /* retry */
            }
            fallthrough;
        default:
            file_set_error();
            return 0;
        }
    }
}

/* check if interval [start;end) overlaps the lock */
static inline int lock_overlaps( struct nt_file_lock *lock, file_pos_t start, file_pos_t end )
{
    if (lock->end && start >= lock->end) return 0;
    if (end && lock->start >= end) return 0;
    return 1;
}

/* remove Unix locks for all bytes in the specified area that are no longer locked */
static void remove_unix_locks( struct nt_fd *fd, file_pos_t start, file_pos_t end )
{
    struct hole
    {
        struct hole *next;
        struct hole *prev;
        file_pos_t   start;
        file_pos_t   end;
    } *first, *cur, *next, *buffer;

    struct nt_file_lock *lock;
    int count = 0;

    if (!fd->inode) return;
    if (!fd->fs_locks) return;
    if (start == end || start > max_unix_offset) return;
    if (!end || end > max_unix_offset) end = max_unix_offset + 1;

    /* count the number of locks overlapping the specified area */

    LIST_FOR_EACH_ENTRY( lock, &fd->inode->locks, struct nt_file_lock, inode_entry )
    {
        if (lock->start == lock->end) continue;
        if (lock_overlaps( lock, start, end )) count++;
    }

    if (!count)  /* no locks at all, we can unlock everything */
    {
        set_unix_lock( fd, start, end, F_UNLCK );
        return;
    }

    /* allocate space for the list of holes */
    /* max. number of holes is number of locks + 1 */

    if (!(buffer = nt_malloc( sizeof(*buffer) * (count+1) ))) return;
    first = buffer;
    first->next  = NULL;
    first->prev  = NULL;
    first->start = start;
    first->end   = end;
    next = first + 1;

    /* build a sorted list of unlocked holes in the specified area */

    LIST_FOR_EACH_ENTRY( lock, &fd->inode->locks, struct nt_file_lock, inode_entry )
    {
        if (lock->start == lock->end) continue;
        if (!lock_overlaps( lock, start, end )) continue;

        /* go through all the holes touched by this lock */
        for (cur = first; cur; cur = cur->next)
        {
            if (cur->end <= lock->start) continue; /* hole is before start of lock */
            if (lock->end && cur->start >= lock->end) break;  /* hole is after end of lock */

            /* now we know that lock is overlapping hole */

            if (cur->start >= lock->start)  /* lock starts before hole, shrink from start */
            {
                cur->start = lock->end;
                if (cur->start && cur->start < cur->end) break;  /* done with this lock */
                /* now hole is empty, remove it */
                if (cur->next) cur->next->prev = cur->prev;
                if (cur->prev) cur->prev->next = cur->next;
                else if (!(first = cur->next)) goto done;  /* no more holes at all */
            }
            else if (!lock->end || cur->end <= lock->end)  /* lock larger than hole, shrink from end */
            {
                cur->end = lock->start;
                assert( cur->start < cur->end );
            }
            else  /* lock is in the middle of hole, split hole in two */
            {
                next->prev = cur;
                next->next = cur->next;
                cur->next = next;
                next->start = lock->end;
                next->end = cur->end;
                cur->end = lock->start;
                assert( next->start < next->end );
                assert( cur->end < next->start );
                next++;
                break;  /* done with this lock */
            }
        }
    }

    /* clear Unix locks for all the holes */

    for (cur = first; cur; cur = cur->next)
        set_unix_lock( fd, cur->start, cur->end, F_UNLCK );

 done:
    nt_free( buffer );
}

/* create a new lock on a fd */
static struct nt_file_lock *add_lock( struct nt_fd *fd, int shared, file_pos_t start, file_pos_t end )
{
    struct nt_file_lock *lock;

    if (!(lock = alloc_object( &file_lock_ops ))) return NULL;
    lock->sync    = NULL;
    lock->shared  = shared;
    lock->start   = start;
    lock->end     = end;
    lock->fd      = fd;
    lock->process = nt_current->process;

    if (!(lock->sync = create_internal_sync( 1, 0 ))) goto error;
    /* now try to set a Unix lock */
    if (!set_unix_lock( lock->fd, lock->start, lock->end, lock->shared ? F_RDLCK : F_WRLCK )) goto error;
    wlist_add_tail( &fd->locks, &lock->fd_entry );
    wlist_add_tail( &fd->inode->locks, &lock->inode_entry );
    wlist_add_tail( &lock->process->locks, &lock->proc_entry );
    return lock;

error:
    release_object( lock );
    return NULL;
}

/* remove an existing lock */
static void remove_lock( struct nt_file_lock *lock, int remove_unix )
{
    struct nt_inode *inode = lock->fd->inode;

    wlist_remove( &lock->fd_entry );
    wlist_remove( &lock->inode_entry );
    wlist_remove( &lock->proc_entry );
    if (remove_unix) remove_unix_locks( lock->fd, lock->start, lock->end );
    if (wlist_empty( &inode->locks )) inode_close_pending( inode, 1 );
    lock->process = NULL;
    signal_sync( lock->sync );
    release_object( lock );
}

/* remove all locks owned by a given process */
void remove_process_locks( struct process *process )
{
    struct list *ptr;

    while ((ptr = wlist_head( &process->locks )))
    {
        struct nt_file_lock *lock = LIST_ENTRY( ptr, struct nt_file_lock, proc_entry );
        remove_lock( lock, 1 );  /* this removes it from the list */
    }
}

/* remove all locks on a given fd */
static void remove_fd_locks( struct nt_fd *fd )
{
    file_pos_t start = FILE_POS_T_MAX, end = 0;
    struct list *ptr;

    while ((ptr = wlist_head( &fd->locks )))
    {
        struct nt_file_lock *lock = LIST_ENTRY( ptr, struct nt_file_lock, fd_entry );
        if (lock->start < start) start = lock->start;
        if (!lock->end || lock->end > end) end = lock->end - 1;
        remove_lock( lock, 0 );
    }
    if (start < end) remove_unix_locks( fd, start, end + 1 );
}

/* add a lock on an fd */
/* returns handle to wait on */
obj_handle_t lock_fd( struct nt_fd *fd, file_pos_t start, file_pos_t count, int shared, int wait )
{
    struct nt_file_lock *lock;
    file_pos_t end = start + count;

    if (!fd->inode)  /* not a regular file */
    {
        set_error( STATUS_INVALID_DEVICE_REQUEST );
        return 0;
    }

    /* don't allow wrapping locks */
    if (end && end < start)
    {
        set_error( STATUS_INVALID_PARAMETER );
        return 0;
    }

    /* check if another lock on that file overlaps the area */
    LIST_FOR_EACH_ENTRY( lock, &fd->inode->locks, struct nt_file_lock, inode_entry )
    {
        if (!lock_overlaps( lock, start, end )) continue;
        if (shared && (lock->shared || lock->fd == fd)) continue;
        /* found one */
        if (!wait)
        {
            set_error( STATUS_FILE_LOCK_CONFLICT );
            return 0;
        }
        set_error( STATUS_PENDING );
        return alloc_handle( nt_current->process, lock, SYNCHRONIZE, 0 );
    }

    /* not found, add it */
    if (add_lock( fd, shared, start, end )) return 0;
    if (get_error() == STATUS_FILE_LOCK_CONFLICT)
    {
        /* Unix lock conflict -> tell client to wait and retry */
        if (wait) set_error( STATUS_PENDING );
    }
    return 0;
}

/* remove a lock on an fd */
void unlock_fd( struct nt_fd *fd, file_pos_t start, file_pos_t count )
{
    struct nt_file_lock *lock;
    file_pos_t end = start + count;

    /* find an existing lock with the exact same parameters */
    LIST_FOR_EACH_ENTRY( lock, &fd->locks, struct nt_file_lock, fd_entry )
    {
        if ((lock->start == start) && (lock->end == end))
        {
            remove_lock( lock, 1 );
            return;
        }
    }
    set_error( STATUS_RANGE_NOT_LOCKED );
}

/****************************************************************/
/* file descriptor functions */

static void fd_dump( struct object *obj, int verbose )
{
    struct nt_fd *fd = (struct nt_fd *)obj;
    nt_fprintf( stderr, "Fd unix_fd=%d user=%p options=%08x", fd->unix_fd, fd->user, fd->options );
    if (fd->inode) nt_fprintf( stderr, " inode=%p disp_flags=%x", fd->inode, fd->closed->disp_flags );
    nt_fprintf( stderr, "\n" );
}

static struct object *fd_get_sync( struct object *obj )
{
    struct nt_fd *fd = (struct nt_fd *)obj;
    return grab_object( fd->sync );
}

static void fd_destroy( struct object *obj )
{
    struct nt_fd *fd = (struct nt_fd *)obj;

    free_async_queue( &fd->read_q );
    free_async_queue( &fd->write_q );
    free_async_queue( &fd->wait_q );

    if (fd->map_addr) free_map_addr( fd->map_addr, fd->map_size );
    if (fd->completion) release_object( fd->completion );
    remove_fd_locks( fd );
    wlist_remove( &fd->inode_entry );
    if (fd->poll_index != -1) remove_poll_user( fd, fd->poll_index );
    nt_free( fd->nt_name );
    if (fd->inode)
    {
        inode_add_closed_fd( fd->inode, fd->closed );
        release_object( fd->inode );
    }
    else  /* no inode, close it right away */
    {
        if (fd->unix_fd != -1) nt_close( fd->unix_fd );
        nt_free( fd->unix_name );
    }
    if (fd->sync) release_object( fd->sync );
}

/* check if the desired access is possible without violating */
/* the sharing mode of other opens of the same file */
static unsigned int check_sharing( struct nt_fd *fd, unsigned int access, unsigned int sharing,
                                   unsigned int open_flags, unsigned int options )
{
    /* only a few access bits are meaningful wrt sharing */
    const unsigned int read_access = FILE_READ_DATA | FILE_EXECUTE;
    const unsigned int write_access = FILE_WRITE_DATA | FILE_APPEND_DATA;
    const unsigned int all_access = read_access | write_access | DELETE;

    unsigned int existing_sharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    unsigned int existing_access = 0;
    struct nt_fd *fd_ptr;

    fd->access = access;
    fd->sharing = sharing;

    LIST_FOR_EACH_ENTRY( fd_ptr, &fd->inode->open, struct nt_fd, inode_entry )
    {
        if (fd_ptr == fd) continue;
        /* if access mode is 0, sharing mode is ignored */
        if (fd_ptr->access & all_access) existing_sharing &= fd_ptr->sharing;
        existing_access |= fd_ptr->access;
    }

    if (((access & read_access) && !(existing_sharing & FILE_SHARE_READ)) ||
        ((access & write_access) && !(existing_sharing & FILE_SHARE_WRITE)) ||
        ((access & DELETE) && !(existing_sharing & FILE_SHARE_DELETE)))
        return STATUS_SHARING_VIOLATION;
    if (((existing_access & FILE_MAPPING_WRITE) && !(sharing & FILE_SHARE_WRITE)) ||
        ((existing_access & FILE_MAPPING_IMAGE) && (access & FILE_WRITE_DATA)))
        return STATUS_SHARING_VIOLATION;
    if ((existing_access & FILE_MAPPING_IMAGE) && (options & FILE_DELETE_ON_CLOSE))
        return STATUS_CANNOT_DELETE;
    if ((existing_access & FILE_MAPPING_ACCESS) && (open_flags & O_TRUNC))
        return STATUS_USER_MAPPED_FILE;
    if (!(access & all_access))
        return 0;  /* if access mode is 0, sharing mode is ignored (except for mappings) */
    if (((existing_access & read_access) && !(sharing & FILE_SHARE_READ)) ||
        ((existing_access & write_access) && !(sharing & FILE_SHARE_WRITE)) ||
        ((existing_access & DELETE) && !(sharing & FILE_SHARE_DELETE)))
        return STATUS_SHARING_VIOLATION;
    return 0;
}

/* set the events that select waits for on this fd */
void set_fd_events( struct nt_fd *fd, int events )
{
    int user = fd->poll_index;
    assert( poll_users[user] == fd );

    set_fd_epoll_events( fd, user, events );

    if (events == -1)  /* stop waiting on this fd completely */
    {
        pollfd[user].fd = -1;
        pollfd[user].events = POLLERR;
        pollfd[user].revents = 0;
    }
    else
    {
        pollfd[user].fd = fd->unix_fd;
        pollfd[user].events = events;
    }
}

/* get the events we want to nt_poll() for */
static int get_poll_events( struct nt_fd *fd )
{
    if (fd->fd_ops->get_poll_events) return fd->fd_ops->get_poll_events( fd );
    return default_fd_get_poll_events( fd );
}

/* prepare an fd for unmounting its corresponding device */
static inline void unmount_fd( struct nt_fd *fd )
{
    assert( fd->inode );

    async_wake_up( &fd->read_q, STATUS_VOLUME_DISMOUNTED );
    async_wake_up( &fd->write_q, STATUS_VOLUME_DISMOUNTED );

    if (fd->poll_index != -1) set_fd_events( fd, -1 );

    if (fd->unix_fd != -1) nt_close( fd->unix_fd );

    fd->unix_fd = -1;
    fd->no_fd_status = STATUS_VOLUME_DISMOUNTED;
    fd->closed->unix_fd = -1;
    fd->closed->disp_flags = 0;

    /* stop using Unix locks on this fd (existing locks have been removed by close) */
    fd->fs_locks = 0;
}

/* allocate an fd object, without setting the unix fd yet */
static struct nt_fd *alloc_fd_object(void)
{
    struct nt_fd *fd = alloc_object( &fd_ops );

    if (!fd) return NULL;

    fd->fd_ops     = NULL;
    fd->sync       = NULL;
    fd->user       = NULL;
    fd->inode      = NULL;
    fd->closed     = NULL;
    fd->map_addr   = 0;
    fd->map_size   = 0;
    fd->access     = 0;
    fd->options    = 0;
    fd->sharing    = 0;
    fd->unix_fd    = -1;
    fd->unix_name  = NULL;
    fd->nt_name    = NULL;
    fd->nt_namelen = 0;
    fd->cacheable  = 0;
    fd->fs_locks   = 1;
    fd->poll_index = -1;
    fd->completion = NULL;
    fd->comp_flags = 0;
    init_async_queue( &fd->read_q );
    init_async_queue( &fd->write_q );
    init_async_queue( &fd->wait_q );
    wlist_init( &fd->inode_entry );
    wlist_init( &fd->locks );

    if (!(fd->sync = create_internal_sync( 1, 1 ))) goto error;
    if ((fd->poll_index = add_poll_user( fd )) == -1) goto error;

    return fd;

error:
    release_object( fd );
    return NULL;
}

/* allocate a pseudo fd object, for objects that need to behave like files but don't have a unix fd */
struct nt_fd *alloc_pseudo_fd( const struct fd_ops *fd_user_ops, struct object *user, unsigned int options )
{
    struct nt_fd *fd = alloc_object( &fd_ops );

    if (!fd) return NULL;

    fd->fd_ops     = fd_user_ops;
    fd->sync       = NULL;
    fd->user       = user;
    fd->inode      = NULL;
    fd->closed     = NULL;
    fd->map_addr   = 0;
    fd->map_size   = 0;
    fd->access     = 0;
    fd->options    = options;
    fd->sharing    = 0;
    fd->unix_name  = NULL;
    fd->nt_name    = NULL;
    fd->nt_namelen = 0;
    fd->unix_fd    = -1;
    fd->cacheable  = 0;
    fd->fs_locks   = 0;
    fd->poll_index = -1;
    fd->completion = NULL;
    fd->comp_flags = 0;
    fd->no_fd_status = STATUS_BAD_DEVICE_TYPE;
    init_async_queue( &fd->read_q );
    init_async_queue( &fd->write_q );
    init_async_queue( &fd->wait_q );
    wlist_init( &fd->inode_entry );
    wlist_init( &fd->locks );

    if (!(fd->sync = create_internal_sync( 1, 1 )))
    {
        release_object( fd );
        return NULL;
    }
    return fd;
}

/* duplicate an fd object for a different user */
struct nt_fd *dup_fd_object( struct nt_fd *orig, unsigned int access, unsigned int sharing, unsigned int options )
{
    unsigned int err;
    struct nt_fd *fd = alloc_fd_object();

    if (!fd) return NULL;

    fd->options    = options;
    fd->cacheable  = orig->cacheable;

    if (orig->unix_name)
    {
        if (!(fd->unix_name = mem_alloc( strlen(orig->unix_name) + 1 ))) goto failed;
        strcpy( fd->unix_name, orig->unix_name );
    }
    if (orig->nt_namelen)
    {
        if (!(fd->nt_name = memdup( orig->nt_name, orig->nt_namelen ))) goto failed;
        fd->nt_namelen = orig->nt_namelen;
    }

    if (orig->inode)
    {
        struct closed_fd *closed = mem_alloc( sizeof(*closed) );
        if (!closed) goto failed;
        if ((fd->unix_fd = nt_dup( orig->unix_fd )) == -1)
        {
            file_set_error();
            nt_free( closed );
            goto failed;
        }
        closed->unix_fd = fd->unix_fd;
        closed->disp_flags = 0;
        closed->unix_name = fd->unix_name;
        fd->closed = closed;
        fd->inode = (struct nt_inode *)grab_object( orig->inode );
        wlist_add_head( &fd->inode->open, &fd->inode_entry );
        if ((err = check_sharing( fd, access, sharing, 0, options )))
        {
            set_error( err );
            goto failed;
        }
    }
    else if ((fd->unix_fd = nt_dup( orig->unix_fd )) == -1)
    {
        file_set_error();
        goto failed;
    }
    return fd;

failed:
    release_object( fd );
    return NULL;
}

/* find an existing fd object that can be reused for a mapping */
struct nt_fd *get_fd_object_for_mapping( struct nt_fd *fd, unsigned int access, unsigned int sharing )
{
    struct nt_fd *fd_ptr;

    if (!fd->inode) return NULL;

    LIST_FOR_EACH_ENTRY( fd_ptr, &fd->inode->open, struct nt_fd, inode_entry )
        if (fd_ptr->access == access && fd_ptr->sharing == sharing)
            return (struct nt_fd *)grab_object( fd_ptr );

    return NULL;
}

/* sets the user of an fd that previously had no user */
void set_fd_user( struct nt_fd *fd, const struct fd_ops *user_ops, struct object *user )
{
    assert( fd->fd_ops == NULL );
    fd->fd_ops = user_ops;
    fd->user   = user;
}

char *dup_fd_name( struct nt_fd *root, const char *name )
{
    char *ret;

    if (!root) return nt_strdup( name );
    if (!root->unix_name) return NULL;

    /* skip . prefix */
    if (name[0] == '.' && (!name[1] || name[1] == '/')) name++;

    if ((ret = nt_malloc( strlen(root->unix_name) + strlen(name) + 2 )))
    {
        strcpy( ret, root->unix_name );
        if (name[0] && name[0] != '/') strcat( ret, "/" );
        strcat( ret, name );
    }
    return ret;
}

static WCHAR *dup_nt_name( struct nt_fd *root, struct unicode_str name, data_size_t *len )
{
    WCHAR *ptr, *ret;

    if (!root)
    {
        *len = name.len;
        if (!name.len) return NULL;
        return memdup( name.str, name.len );
    }
    if (!root->nt_namelen) return NULL;

    /* skip . prefix */
    if (name.len && name.str[0] == '.' && (name.len == sizeof(WCHAR) || name.str[1] == '\\'))
    {
        name.str++;
        name.len -= sizeof(WCHAR);
    }
    if ((ret = nt_malloc( root->nt_namelen + name.len + sizeof(WCHAR) )))
    {
        ptr = mem_append( ret, root->nt_name, root->nt_namelen );
        if (name.len && name.str[0] != '\\' && ptr[-1] != '\\') *ptr++ = '\\';
        ptr = mem_append( ptr, name.str, name.len );
        *len = (ptr - ret) * sizeof(WCHAR);
    }
    return ret;
}

struct unicode_str get_nt_name( struct nt_fd *fd )
{
    struct unicode_str name = { .str = fd->nt_name, .len = fd->nt_namelen };
    return name;
}

/* nt_open() wrapper that returns a struct nt_fd with no fd user set */
struct nt_fd *open_fd( struct nt_fd *root, const char *name, struct unicode_str nt_name,
                    int flags, mode_t *mode, unsigned int access,
                    unsigned int sharing, unsigned int options )
{
    struct nt_stat st;
    struct closed_fd *closed_fd;
    struct nt_fd *fd;
    int root_fd = -1;
    int rw_mode;
    char *path;

    if (((options & FILE_DELETE_ON_CLOSE) && !(access & DELETE)) ||
        ((options & FILE_DIRECTORY_FILE) && (flags & O_TRUNC)))
    {
        set_error( STATUS_INVALID_PARAMETER );
        return NULL;
    }

    if (!(fd = alloc_fd_object())) return NULL;

    fd->options = options;
    if (!(closed_fd = mem_alloc( sizeof(*closed_fd) )))
    {
        release_object( fd );
        return NULL;
    }

    if (root)
    {
        if ((root_fd = get_unix_fd( root )) == -1) goto error;
        if (nt_fchdir( root_fd ) == -1)
        {
            file_set_error();
            root_fd = -1;
            goto error;
        }
    }

    /* create the directory if needed */
    if ((options & FILE_DIRECTORY_FILE) && (flags & O_CREAT))
    {
        if (nt_mkdir( name, *mode ) == -1)
        {
            if (errno != EEXIST || (flags & O_EXCL))
            {
                file_set_error();
                goto error;
            }
        }
        flags &= ~(O_CREAT | O_EXCL | O_TRUNC);
    }

    if ((access & FILE_UNIX_WRITE_ACCESS) && !(options & FILE_DIRECTORY_FILE))
    {
        if (access & FILE_UNIX_READ_ACCESS) rw_mode = O_RDWR;
        else rw_mode = O_WRONLY;
    }
    else rw_mode = O_RDONLY;

    if ((fd->unix_fd = nt_open( name, rw_mode | (flags & ~O_TRUNC), *mode )) == -1)
    {
        /* if we tried to open a directory for write access, retry read-only */
        if (errno == EISDIR)
        {
            if ((access & FILE_UNIX_WRITE_ACCESS) || (flags & O_CREAT))
                fd->unix_fd = nt_open( name, O_RDONLY | (flags & ~(O_TRUNC | O_CREAT | O_EXCL)), *mode );
        }

        if (fd->unix_fd == -1)
        {
            /* check for trailing slash on file path */
            if ((errno == ENOENT || (errno == ENOTDIR && !(options & FILE_DIRECTORY_FILE))) && name[strlen(name) - 1] == '/')
                set_error( STATUS_OBJECT_NAME_INVALID );
            else
                file_set_error();
            goto error;
        }
    }

    fd->nt_name = dup_nt_name( root, nt_name, &fd->nt_namelen );
    fd->unix_name = NULL;
    nt_fstat( fd->unix_fd, &st );
    *mode = st.st_mode;

    /* only bother with an inode for normal files and directories */
    if (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode))
    {
        unsigned int err;
        struct nt_inode *inode = get_inode( st.st_dev, st.st_ino, fd->unix_fd );

        if (!inode)
        {
            /* we can close the fd because there are no others open on the same file,
             * otherwise we wouldn't have failed to allocate a new inode
             */
            goto error;
        }

        if ((path = dup_fd_name( root, name )))
        {
            fd->unix_name = nt_realpath( path, NULL );
            nt_free( path );
        }

        closed_fd->unix_fd = fd->unix_fd;
        closed_fd->unix_name = fd->unix_name;
        closed_fd->disp_flags = 0;
        fd->inode = inode;
        fd->closed = closed_fd;
        fd->cacheable = !inode->device->removable;
        wlist_add_head( &inode->open, &fd->inode_entry );
        closed_fd = NULL;

        /* check directory options */
        if ((options & FILE_DIRECTORY_FILE) && !S_ISDIR(st.st_mode))
        {
            set_error( STATUS_NOT_A_DIRECTORY );
            goto error;
        }
        if ((options & FILE_NON_DIRECTORY_FILE) && S_ISDIR(st.st_mode))
        {
            set_error( STATUS_FILE_IS_A_DIRECTORY );
            goto error;
        }
        if ((err = check_sharing( fd, access, sharing, flags, options )))
        {
            set_error( err );
            goto error;
        }

        /* can't unlink files if we don't have permission to access */
        if ((options & FILE_DELETE_ON_CLOSE) && !(flags & O_CREAT) &&
            !(st.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH)))
        {
            set_error( STATUS_CANNOT_DELETE );
            goto error;
        }

        fd->closed->disp_flags = (options & FILE_DELETE_ON_CLOSE) ?
            FILE_DISPOSITION_DELETE : 0;
        if (flags & O_TRUNC)
        {
            if (S_ISDIR(st.st_mode))
            {
                set_error( STATUS_OBJECT_NAME_COLLISION );
                goto error;
            }
            nt_ftruncate( fd->unix_fd, 0 );
        }
    }
    else  /* special file */
    {
        if (options & FILE_DELETE_ON_CLOSE)  /* we can't unlink special files */
        {
            set_error( STATUS_INVALID_PARAMETER );
            goto error;
        }
        nt_free( closed_fd );
        fd->cacheable = 1;
    }

#ifdef HAVE_POSIX_FADVISE
    switch (options & (FILE_SEQUENTIAL_ONLY | FILE_RANDOM_ACCESS))
    {
    case FILE_SEQUENTIAL_ONLY:
        nt_posix_fadvise( fd->unix_fd, 0, 0, POSIX_FADV_SEQUENTIAL );
        break;
    case FILE_RANDOM_ACCESS:
        nt_posix_fadvise( fd->unix_fd, 0, 0, POSIX_FADV_RANDOM );
        break;
    }
#endif

    if (root_fd != -1) nt_fchdir( server_dir_fd ); /* go back to the server dir */
    return fd;

error:
    release_object( fd );
    nt_free( closed_fd );
    if (root_fd != -1) nt_fchdir( server_dir_fd ); /* go back to the server dir */
    return NULL;
}

/* create an fd for an anonymous file */
/* if the function fails the unix fd is closed */
struct nt_fd *create_anonymous_fd( const struct fd_ops *fd_user_ops, int unix_fd, struct object *user,
                                unsigned int options )
{
    struct nt_fd *fd = alloc_fd_object();

    if (fd)
    {
        set_fd_user( fd, fd_user_ops, user );
        fd->unix_fd = unix_fd;
        fd->options = options;
        return fd;
    }
    nt_close( unix_fd );
    return NULL;
}

/* retrieve the object that is using an fd */
void *get_fd_user( struct nt_fd *fd )
{
    return fd->user;
}

/* retrieve the opening options for the fd */
unsigned int get_fd_options( struct nt_fd *fd )
{
    return fd->options;
}

/* retrieve the completion flags for the fd */
unsigned int get_fd_comp_flags( struct nt_fd *fd )
{
    return fd->comp_flags;
}

/* check if fd is in overlapped mode */
int is_fd_overlapped( struct nt_fd *fd )
{
    return !(fd->options & (FILE_SYNCHRONOUS_IO_ALERT | FILE_SYNCHRONOUS_IO_NONALERT));
}

/* retrieve the unix fd for an object */
int get_unix_fd( struct nt_fd *fd )
{
    if (fd->unix_fd == -1) set_error( fd->no_fd_status );
    return fd->unix_fd;
}

/* retrieve the suggested mapping address for the fd */
client_ptr_t get_fd_map_address( struct nt_fd *fd )
{
    return fd->map_addr;
}

/* set the suggested mapping address for the fd */
void set_fd_map_address( struct nt_fd *fd, client_ptr_t addr, mem_size_t size )
{
    assert( !fd->map_addr );
    fd->map_addr = addr;
    fd->map_size = size;
}

/* check if two file descriptors point to the same file */
int is_same_file_fd( struct nt_fd *fd1, struct nt_fd *fd2 )
{
    return fd1->inode == fd2->inode;
}

/* allow the fd to be cached (can't be reset once set) */
void allow_fd_caching( struct nt_fd *fd )
{
    fd->cacheable = 1;
}

/* check if fd is on a removable device */
int is_fd_removable( struct nt_fd *fd )
{
    return (fd->inode && fd->inode->device->removable);
}

/* set or clear the fd signaled state */
void set_fd_signaled( struct nt_fd *fd, int signaled )
{
    if (fd->comp_flags & FILE_SKIP_SET_EVENT_ON_HANDLE) return;
    if (signaled) signal_sync( fd->sync );
    else reset_sync( fd->sync );
}

/* check if events are pending and if yes return which one(s) */
int check_fd_events( struct nt_fd *fd, int events )
{
    struct pollfd pfd;

    if (fd->unix_fd == -1) return POLLERR;
    if (fd->inode) return events;  /* regular files are always signaled */

    pfd.fd     = fd->unix_fd;
    pfd.events = events;
    if (nt_poll( &pfd, 1, 0 ) <= 0) return 0;
    return pfd.revents;
}

/* default get_sync() routine for objects that nt_poll() on an fd */
struct object *default_fd_get_sync( struct object *obj )
{
    struct nt_fd *fd = get_obj_fd( obj );
    struct object *sync = get_obj_sync( &fd->obj );
    release_object( fd );
    return sync;
}

/* default get_full_name() routine for objects with an fd */
WCHAR *default_fd_get_full_name( struct object *obj, data_size_t max, data_size_t *ret_len )
{
    struct nt_fd *fd = get_obj_fd( obj );
    WCHAR *ret = NULL;

    if (fd->nt_name)
    {
        *ret_len = fd->nt_namelen;
        ret = memdup( fd->nt_name, fd->nt_namelen );
    }
    release_object( fd );
    if (*ret_len > max) set_error( STATUS_BUFFER_OVERFLOW );
    return ret;
}

int default_fd_get_poll_events( struct nt_fd *fd )
{
    int events = 0;

    if (async_waiting( &fd->read_q )) events |= POLLIN;
    if (async_waiting( &fd->write_q )) events |= POLLOUT;
    return events;
}

/* default handler for nt_poll() events */
void default_poll_event( struct nt_fd *fd, int event )
{
    if (event & (POLLIN | POLLERR | POLLHUP)) async_wake_up( &fd->read_q, STATUS_ALERTED );
    if (event & (POLLOUT | POLLERR | POLLHUP)) async_wake_up( &fd->write_q, STATUS_ALERTED );

    /* if an error occurred, stop polling this fd to avoid busy-looping */
    if (event & (POLLERR | POLLHUP)) set_fd_events( fd, -1 );
    else if (!fd->inode) set_fd_events( fd, get_poll_events( fd ) );
}

void fd_queue_async( struct nt_fd *fd, struct async *async, int type )
{
    struct async_queue *queue;

    switch (type)
    {
    case ASYNC_TYPE_READ:
        queue = &fd->read_q;
        break;
    case ASYNC_TYPE_WRITE:
        queue = &fd->write_q;
        break;
    case ASYNC_TYPE_WAIT:
        queue = &fd->wait_q;
        break;
    default:
        queue = NULL;
        assert(0);
    }

    queue_async( queue, async );

    if (type != ASYNC_TYPE_WAIT)
    {
        if (!fd->inode)
            set_fd_events( fd, get_poll_events( fd ) );
        else  /* regular files are always ready for read and write */
            async_wake_up( queue, STATUS_ALERTED );
    }
}

void fd_async_wake_up( struct nt_fd *fd, int type, unsigned int status )
{
    switch (type)
    {
    case ASYNC_TYPE_READ:
        async_wake_up( &fd->read_q, status );
        break;
    case ASYNC_TYPE_WRITE:
        async_wake_up( &fd->write_q, status );
        break;
    case ASYNC_TYPE_WAIT:
        async_wake_up( &fd->wait_q, status );
        break;
    default:
        assert(0);
    }
}

void fd_cancel_async( struct nt_fd *fd, struct async *async )
{
    if (fd->fd_ops->cancel_async) fd->fd_ops->cancel_async( fd, async );
    else async_terminate( async, STATUS_CANCELLED );
}

void fd_reselect_async( struct nt_fd *fd, struct async_queue *queue )
{
    if (fd->fd_ops->reselect_async) fd->fd_ops->reselect_async( fd, queue );
    else default_fd_reselect_async( fd, queue );
}

void default_fd_queue_async( struct nt_fd *fd, struct async *async, int type, int count )
{
    fd_queue_async( fd, async, type );
    set_error( STATUS_PENDING );
}

/* default reselect_async() fd routine */
void default_fd_reselect_async( struct nt_fd *fd, struct async_queue *queue )
{
    if (queue == &fd->read_q || queue == &fd->write_q)
    {
        int poll_events = get_poll_events( fd );
        int events = check_fd_events( fd, poll_events );
        if (events) fd_poll_event( fd, events );
        else set_fd_events( fd, poll_events );
    }
}

static int is_dir_empty( int fd )
{
    DIR *dir;
    int empty;
    struct dirent *de;

    if ((fd = nt_dup( fd )) == -1)
        return -1;

    if (!(dir = nt_fdopendir( fd )))
    {
        nt_close( fd );
        return -1;
    }

    nt_rewinddir( dir );
    empty = 1;
    while (empty && (de = nt_readdir( dir )))
    {
        if (!strcmp( de->d_name, "." ) || !strcmp( de->d_name, ".." )) continue;
        empty = 0;
    }
    nt_closedir( dir );
    return empty;
}

static inline int is_valid_mounted_device( struct nt_stat *st )
{
#if defined(linux) || defined(__sun__)
    return S_ISBLK( st->st_mode );
#else
    /* disks are char devices on *BSD */
    return S_ISCHR( st->st_mode );
#endif
}

/* close all Unix file descriptors on a device to allow unmounting it */
static void unmount_device( struct nt_fd *device_fd )
{
    unsigned int i;
    struct nt_stat st;
    struct nt_device *device;
    struct nt_inode *inode;
    struct nt_fd *fd;
    int unix_fd = get_unix_fd( device_fd );

    if (unix_fd == -1) return;

    if (nt_fstat( unix_fd, &st ) == -1 || !is_valid_mounted_device( &st ))
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }

    if (!(device = nt_get_device( st.st_rdev, -1 ))) return;

    for (i = 0; i < INODE_HASH_SIZE; i++)
    {
        LIST_FOR_EACH_ENTRY( inode, &device->inode_hash[i], struct nt_inode, entry )
        {
            LIST_FOR_EACH_ENTRY( fd, &inode->open, struct nt_fd, inode_entry )
            {
                unmount_fd( fd );
            }
            inode_close_pending( inode, 0 );
        }
    }
    /* remove it from the hash table */
    wlist_remove( &device->entry );
    wlist_init( &device->entry );
    release_object( device );
}

#ifndef XATTR_USER_PREFIX
# define XATTR_USER_PREFIX "user."
#endif
#ifndef XATTR_USER_PREFIX_LEN
# define XATTR_USER_PREFIX_LEN (sizeof(XATTR_USER_PREFIX) - 1)
#endif

#define XATTR_REPARSE XATTR_USER_PREFIX "WINEREPARSE"

static int xattr_fset( int filedes, const char *name, const void *value, size_t size )
{
#ifdef HAVE_SYS_XATTR_H
# ifdef XATTR_ADDITIONAL_OPTIONS
    return nt_fsetxattr( filedes, name, value, size, 0, 0 );
# else
    return nt_fsetxattr( filedes, name, value, size, 0 );
# endif
#elif defined(HAVE_SYS_EXTATTR_H)
    return extattr_set_fd( filedes, EXTATTR_NAMESPACE_USER, &name[XATTR_USER_PREFIX_LEN],
                           value, size );
#else
    errno = ENOSYS;
    return -1;
#endif
}

static int xattr_fget( int filedes, const char *name, void *value, size_t size )
{
#ifdef HAVE_SYS_XATTR_H
# ifdef XATTR_ADDITIONAL_OPTIONS
    return nt_fgetxattr( filedes, name, value, size, 0, 0 );
# else
    return nt_fgetxattr( filedes, name, value, size );
# endif
#elif defined(HAVE_SYS_EXTATTR_H)
    return extattr_get_fd( filedes, EXTATTR_NAMESPACE_USER, &name[XATTR_USER_PREFIX_LEN],
                           value, size );
#else
    errno = ENOSYS;
    return -1;
#endif
}

static int xattr_fremove( int filedes, const char *name )
{
#ifdef HAVE_SYS_XATTR_H
# ifdef XATTR_ADDITIONAL_OPTIONS
    return nt_fremovexattr( filedes, name, 0 );
# else
    return nt_fremovexattr( filedes, name );
# endif
#elif defined(HAVE_SYS_EXTATTR_H)
    return extattr_delete_fd( filedes, EXTATTR_NAMESPACE_USER, &name[XATTR_USER_PREFIX_LEN] );
#else
    errno = ENOSYS;
    return -1;
#endif
}

static void set_reparse_point( struct nt_fd *fd, struct async *async )
{
    char *reparse_name;
    struct nt_stat st;
    size_t len;

    if (!fd->unix_name)
    {
        set_error( STATUS_OBJECT_TYPE_MISMATCH );
        return;
    }

    if (nt_fstat( fd->unix_fd, &st ) == -1)
    {
        file_set_error();
        return;
    }

    if (S_ISDIR(st.st_mode) && !is_dir_empty( fd->unix_fd ))
    {
        set_error( STATUS_DIRECTORY_NOT_EMPTY );
        return;
    }

    if (xattr_fset( fd->unix_fd, XATTR_REPARSE, get_req_data(), get_req_data_size() ) < 0)
    {
        file_set_error();
        return;
    }

    len = strlen( fd->unix_name );
    if (fd->unix_name[len - 1] != '?')
    {
        /* we are adding a reparse point where there previously was not one;
         * move the file out of the way so open attempts will fail */

        if (!(reparse_name = mem_alloc( len + 2 ))) return;
        memcpy( reparse_name, fd->unix_name, len );
        strcpy( reparse_name + len, "?" );

        if (nt_rename( fd->unix_name, reparse_name ))
        {
            nt_free( reparse_name );
            return;
        }
        nt_free( fd->unix_name );
        fd->closed->unix_name = fd->unix_name = reparse_name;
    }
}

static void get_reparse_point( struct nt_fd *fd, struct async *async )
{
    /* we can't just allocate get_reply_max_size() here;
     * Linux won't return any data if the size is too small */
    char *buffer;  /* ntos: 16 KB, from the heap: a kernel stack is 16 KB in all */
    int ret;

    if (!fd->unix_name)
    {
        set_error( STATUS_OBJECT_TYPE_MISMATCH );
        return;
    }

    if (!get_reply_max_size())
    {
        set_error( STATUS_INVALID_USER_BUFFER );
        return;
    }

    if (fd->unix_name[strlen( fd->unix_name ) - 1] != '?')
    {
        set_error( STATUS_NOT_A_REPARSE_POINT );
        return;
    }

    if (get_reply_max_size() < sizeof(REPARSE_GUID_DATA_BUFFER))
    {
        set_error( STATUS_BUFFER_TOO_SMALL );
        return;
    }

    if (!(buffer = mem_alloc( MAXIMUM_REPARSE_DATA_BUFFER_SIZE ))) return;
    ret = xattr_fget( fd->unix_fd, XATTR_REPARSE, buffer, MAXIMUM_REPARSE_DATA_BUFFER_SIZE );
    if (ret >= 0)
    {
        if (ret > get_reply_max_size())
        {
            set_error( STATUS_BUFFER_OVERFLOW );
            ret = get_reply_max_size();
        }
        set_reply_data( buffer, ret );
    }
    else file_set_error();
    nt_free( buffer );
}

static void delete_reparse_point( struct nt_fd *fd, struct async *async )
{
    const REPARSE_DATA_BUFFER *data = get_req_data();
    char *base_name;
    size_t len;

    if (!fd->unix_name)
    {
        set_error( STATUS_OBJECT_TYPE_MISMATCH );
        return;
    }

    if (!get_req_data_size())
    {
        set_error( STATUS_INVALID_BUFFER_SIZE );
        return;
    }

    len = strlen( fd->unix_name );
    if (fd->unix_name[len - 1] != '?')
    {
        set_error( STATUS_NOT_A_REPARSE_POINT );
        return;
    }

    if (get_req_data_size() != sizeof(REPARSE_DATA_BUFFER) || data->ReparseDataLength)
    {
        set_error( STATUS_IO_REPARSE_DATA_INVALID );
        return;
    }

    if (!data->ReparseTag)
    {
        set_error( STATUS_IO_REPARSE_TAG_INVALID );
        return;
    }

    if (!(base_name = mem_alloc( len )))
        return;
    memcpy( base_name, fd->unix_name, len - 1 );
    base_name[len - 1] = 0;

    if (nt_rename( fd->unix_name, base_name ) < 0)
    {
        file_set_error();
        nt_free( base_name );
        return;
    }

    nt_free( fd->unix_name );
    fd->closed->unix_name = fd->unix_name = base_name;

    xattr_fremove( fd->unix_fd, XATTR_REPARSE );
}

/* default get_file_info() routine */
void default_fd_get_file_info( struct nt_fd *fd, obj_handle_t handle, unsigned int info_class )
{
    switch (info_class)
    {
    case FileAccessInformation:
        {
            FILE_ACCESS_INFORMATION info;
            if (get_reply_max_size() < sizeof(info))
            {
                set_error( STATUS_INFO_LENGTH_MISMATCH );
                return;
            }
            info.AccessFlags = get_handle_access( nt_current->process, handle );
            set_reply_data( &info, sizeof(info) );
            break;
        }
    case FileModeInformation:
        {
            FILE_MODE_INFORMATION info;
            if (get_reply_max_size() < sizeof(info))
            {
                set_error( STATUS_INFO_LENGTH_MISMATCH );
                return;
            }
            info.Mode = fd->options & ( FILE_WRITE_THROUGH
                                      | FILE_SEQUENTIAL_ONLY
                                      | FILE_NO_INTERMEDIATE_BUFFERING
                                      | FILE_SYNCHRONOUS_IO_ALERT
                                      | FILE_SYNCHRONOUS_IO_NONALERT );
            set_reply_data( &info, sizeof(info) );
            break;
        }
    case FileIoCompletionNotificationInformation:
        {
            FILE_IO_COMPLETION_NOTIFICATION_INFORMATION info;
            if (get_reply_max_size() < sizeof(info))
            {
                set_error( STATUS_INFO_LENGTH_MISMATCH );
                return;
            }
            info.Flags = fd->comp_flags;
            set_reply_data( &info, sizeof(info) );
            break;
        }
    case WinFileUnixNameInformation:
        if (fd->unix_name)
        {
            data_size_t len = strlen( fd->unix_name );
            data_size_t data_len = offsetof( WINE_FILE_UNIX_NAME_INFORMATION, Name[len] );
            WINE_FILE_UNIX_NAME_INFORMATION *info;

            if (get_reply_max_size() < sizeof(*info))
            {
                set_error( STATUS_INFO_LENGTH_MISMATCH );
                return;
            }
            if (get_reply_max_size() < data_len)
            {
                set_error( STATUS_BUFFER_OVERFLOW );
                data_len = sizeof(*info);
            }
            if (!(info = set_reply_data_size( data_len ))) break;
            info->Length = len;
            memcpy( info->Name, fd->unix_name, data_len - sizeof(*info) );
        }
        else set_error( STATUS_OBJECT_TYPE_MISMATCH );
        break;
    default:
        set_error( STATUS_NOT_IMPLEMENTED );
    }
}

/* default nt_ioctl() routine */
void default_fd_ioctl( struct nt_fd *fd, ioctl_code_t code, struct async *async )
{
    switch(code)
    {
    case FSCTL_DISMOUNT_VOLUME:
        unmount_device( fd );
        break;

    case FSCTL_SET_REPARSE_POINT:
        set_reparse_point( fd, async );
        break;

    case FSCTL_GET_REPARSE_POINT:
        get_reparse_point( fd, async );
        break;

    case FSCTL_DELETE_REPARSE_POINT:
        delete_reparse_point( fd, async );
        break;

    default:
        set_error( STATUS_NOT_SUPPORTED );
    }
}

/* same as get_handle_obj but retrieve the struct nt_fd associated to the object */
static struct nt_fd *get_handle_fd_obj( struct process *process, obj_handle_t handle,
                                     unsigned int access )
{
    struct nt_fd *fd = NULL;
    struct object *obj;

    if ((obj = get_handle_obj( process, handle, access, NULL )))
    {
        fd = get_obj_fd( obj );
        release_object( obj );
    }
    return fd;
}

/* set disposition for the fd */
static void set_fd_disposition( struct nt_fd *fd, unsigned int flags )
{
    struct nt_stat st;

    if (!fd->inode)
    {
        set_error( STATUS_OBJECT_TYPE_MISMATCH );
        return;
    }

    if (fd->unix_fd == -1)
    {
        set_error( fd->no_fd_status );
        return;
    }

    if (flags & FILE_DISPOSITION_DELETE)
    {
        struct nt_fd *fd_ptr;

        LIST_FOR_EACH_ENTRY( fd_ptr, &fd->inode->open, struct nt_fd, inode_entry )
        {
            if (fd_ptr->access & FILE_MAPPING_ACCESS)
            {
                set_error( STATUS_CANNOT_DELETE );
                return;
            }
        }

        if (nt_fstat( fd->unix_fd, &st ) == -1)
        {
            file_set_error();
            return;
        }
        if (S_ISREG( st.st_mode ))  /* can't unlink files we don't have permission to write */
        {
            if (!(flags & FILE_DISPOSITION_IGNORE_READONLY_ATTRIBUTE) &&
                !(st.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH)))
            {
                set_error( STATUS_CANNOT_DELETE );
                return;
            }
        }
        else if (S_ISDIR( st.st_mode ))  /* can't remove non-empty directories */
        {
            switch (is_dir_empty( fd->unix_fd ))
            {
            case -1:
                file_set_error();
                return;
            case 0:
                set_error( STATUS_DIRECTORY_NOT_EMPTY );
                return;
            }
        }
        else  /* can't unlink special files */
        {
            set_error( STATUS_INVALID_PARAMETER );
            return;
        }
    }

    if (flags & FILE_DISPOSITION_ON_CLOSE)
        fd->options &= ~FILE_DELETE_ON_CLOSE;

    fd->closed->disp_flags =
        (flags & (FILE_DISPOSITION_DELETE | FILE_DISPOSITION_POSIX_SEMANTICS)) |
        ((fd->options & FILE_DELETE_ON_CLOSE) ? FILE_DISPOSITION_DELETE : 0);
}

/* Arctic: a rename to another case of the same name, on a nocase volume:
 * the file first goes to a name of its own beside it, the rename proper
 * then gives it the new case. 1 if it moved (fd->unix_name is that name). */
static int move_aside_for_case( struct nt_fd *fd )
{
    static unsigned int count;
    size_t len = strlen( fd->unix_name );
    char *aside;

    if (fd->unix_name[len - 1] == '?') return 0;  /* a reparse point keeps its marker name */
    if (!(aside = mem_alloc( len + 32 ))) return 0;
    snprintf( aside, len + 32, "%s.arctic-case-%x", fd->unix_name, ++count );
    if (nt_rename( fd->unix_name, aside ))
    {
        nt_free( aside );
        return 0;
    }
    nt_free( fd->unix_name );
    fd->closed->unix_name = fd->unix_name = aside;
    return 1;
}

/* set new name for the fd */
static void set_fd_name( struct nt_fd *fd, struct nt_fd *root, const char *nameptr, data_size_t len,
                         struct unicode_str nt_name, int create_link, unsigned int flags )
{
    struct nt_inode *inode;
    struct nt_stat st, st2;
    char *name;
    const unsigned int replace = flags & FILE_RENAME_REPLACE_IF_EXISTS;

    if (!fd->inode || !fd->unix_name)
    {
        set_error( STATUS_OBJECT_TYPE_MISMATCH );
        return;
    }
    if (fd->unix_fd == -1)
    {
        set_error( fd->no_fd_status );
        return;
    }

    if (!len || ((nameptr[0] == '/') ^ !root))
    {
        set_error( STATUS_OBJECT_PATH_SYNTAX_BAD );
        return;
    }
    if (!(name = mem_alloc( len + 2 ))) return;
    memcpy( name, nameptr, len );
    if (fd->unix_name[strlen( fd->unix_name ) - 1] == '?' && name[len - 1] != '?')
        name[len++] = '?';
    name[len] = 0;

    if (root)
    {
        char *combined_name = dup_fd_name( root, name );
        if (!combined_name)
        {
            set_error( STATUS_NO_MEMORY );
            goto failed;
        }
        nt_free( name );
        name = combined_name;
    }

    /* when creating a hard link, source cannot be a dir */
    if (create_link && !nt_fstat( fd->unix_fd, &st ) && S_ISDIR( st.st_mode ))
    {
        set_error( STATUS_FILE_IS_A_DIRECTORY );
        goto failed;
    }

    if (!nt_stat( name, &st ))
    {
        if (!nt_fstat( fd->unix_fd, &st2 ) && st.st_ino == st2.st_ino && st.st_dev == st2.st_dev)
        {
            if (create_link && !replace) set_error( STATUS_OBJECT_NAME_COLLISION );
            /* Arctic: on a nocase volume another case of the file's own name
             * finds the file itself; with one link it can be nothing else, and
             * Windows renames it to that case. The kernel takes a rename to an
             * alias of the same file for nothing to do: through a name of its
             * own in the same directory. */
            else if (!create_link && st2.st_nlink == 1 && strcmp( name, fd->unix_name ) &&
                     move_aside_for_case( fd ))
                goto do_rename;
            nt_free( name );
            return;
        }

        if (!replace)
        {
            set_error( STATUS_OBJECT_NAME_COLLISION );
            goto failed;
        }

        /* can't replace directories or special files */
        if (!S_ISREG( st.st_mode ))
        {
            set_error( STATUS_ACCESS_DENIED );
            goto failed;
        }

        /* read-only files cannot be replaced */
        if (!(st.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH)) &&
            !(flags & FILE_RENAME_IGNORE_READONLY_ATTRIBUTE))
        {
            set_error( STATUS_ACCESS_DENIED );
            goto failed;
        }

        /* can't replace an opened file */
        if ((inode = get_inode( st.st_dev, st.st_ino, -1 )))
        {
            int is_empty = wlist_empty( &inode->open );
            release_object( inode );
            if (!is_empty)
            {
                set_error( STATUS_ACCESS_DENIED );
                goto failed;
            }
        }

        /* link() expects that the target doesn't exist */
        /* nt_rename() cannot replace files with directories */
        if (create_link || S_ISDIR( st2.st_mode ))
        {
            if (nt_unlink( name ))
            {
                file_set_error();
                goto failed;
            }
        }
    }

    if (create_link)
    {
        if (nt_link( fd->unix_name, name ))
            file_set_error();
        nt_free( name );
        return;
    }

do_rename:
    if (nt_rename( fd->unix_name, name ))
    {
        char *aside = strstr( fd->unix_name, ".arctic-case-" );

        file_set_error();
        if (aside)  /* moved aside for a case rename: back to its own name */
        {
            char *own = nt_strdup( fd->unix_name );

            if (own)
            {
                own[aside - fd->unix_name] = 0;
                if (!nt_rename( fd->unix_name, own ))
                {
                    nt_free( fd->unix_name );
                    fd->closed->unix_name = fd->unix_name = own;
                }
                else nt_free( own );
            }
        }
        goto failed;
    }

    if (is_file_executable( fd->unix_name ) != is_file_executable( name ) && !nt_fstat( fd->unix_fd, &st ))
    {
        if (is_file_executable( name ))
            /* set executable bit where read bit is set */
            st.st_mode |= (st.st_mode & 0444) >> 2;
        else
            st.st_mode &= ~0111;
        nt_fchmod( fd->unix_fd, st.st_mode );
    }

    nt_free( fd->nt_name );
    fd->nt_name = dup_nt_name( root, nt_name, &fd->nt_namelen );
    nt_free( fd->unix_name );
    fd->closed->unix_name = fd->unix_name = nt_realpath( name, NULL );
    nt_free( name );
    if (!fd->unix_name)
        set_error( STATUS_NO_MEMORY );
    return;

failed:
    nt_free( name );
}

static void set_fd_eof( struct nt_fd *fd, file_pos_t eof )
{
    struct nt_stat st;

    if (!fd->inode)
    {
        set_error( STATUS_OBJECT_TYPE_MISMATCH );
        return;
    }

    if (fd->unix_fd == -1)
    {
        set_error( fd->no_fd_status );
        return;
    }
    if (nt_fstat( fd->unix_fd, &st) == -1)
    {
        file_set_error();
        return;
    }
    if (eof < st.st_size)
    {
        struct nt_fd *fd_ptr;
        LIST_FOR_EACH_ENTRY( fd_ptr, &fd->inode->open, struct nt_fd, inode_entry )
        {
            if (fd_ptr->access & FILE_MAPPING_ACCESS)
            {
                set_error( STATUS_USER_MAPPED_FILE );
                return;
            }
        }
        if (nt_ftruncate( fd->unix_fd, eof ) == -1) file_set_error();
    }
    else grow_file( fd->unix_fd, eof );
}

struct nt_completion *fd_get_completion( struct nt_fd *fd, apc_param_t *p_key )
{
    *p_key = fd->comp_key;
    return fd->completion ? (struct nt_completion *)grab_object( fd->completion ) : NULL;
}

void fd_copy_completion( struct nt_fd *src, struct nt_fd *dst )
{
    assert( !dst->completion );
    dst->completion = fd_get_completion( src, &dst->comp_key );
    dst->comp_flags = src->comp_flags;
}

/* flush a file buffers */
DECL_HANDLER(flush)
{
    struct nt_fd *fd = get_handle_fd_obj( nt_current->process, req->async.handle, 0 );
    struct async *async;

    if (!fd) return;

    if ((async = create_request_async( fd, &req->async, 0 )))
    {
        if (fd->fd_ops->flush) fd->fd_ops->flush( fd, async );
        else set_error( STATUS_OBJECT_TYPE_MISMATCH );
        reply->event = async_handoff( async, NULL, 1 );
        release_object( async );
    }
    release_object( fd );
}

/* query file info */
DECL_HANDLER(get_file_info)
{
    struct nt_fd *fd = get_handle_fd_obj( nt_current->process, req->handle, 0 );

    if (fd)
    {
        if (fd->fd_ops->get_file_info) fd->fd_ops->get_file_info( fd, req->handle, req->info_class );
        else set_error( STATUS_OBJECT_TYPE_MISMATCH );
        release_object( fd );
    }
}

/* query volume info */
DECL_HANDLER(get_volume_info)
{
    struct nt_fd *fd = get_handle_fd_obj( nt_current->process, req->handle, 0 );
    struct async *async;

    if (!fd) return;

    if ((async = create_request_async( fd, &req->async, 0 )))
    {
        if (fd->fd_ops->get_volume_info) fd->fd_ops->get_volume_info( fd, async, req->info_class );
        else set_error( STATUS_OBJECT_TYPE_MISMATCH );
        reply->wait = async_handoff( async, NULL, 1 );
        release_object( async );
    }
    release_object( fd );
}

/* open a file object */
DECL_HANDLER(open_file_object)
{
    struct object *obj, *result;
    struct object_params params = { .name = get_req_unicode_str(), .attr = req->attributes };
    struct nt_fd *fd;
    struct async *async;

    if (req->rootdir && !(params.root = get_handle_obj( nt_current->process, req->rootdir, 0, NULL ))) return;

    obj = open_named_object( &params );
    if (params.root) release_object( params.root );
    if (!obj) return;

    if (!obj->ops->open_file) set_error( STATUS_OBJECT_TYPE_MISMATCH );
    else if ((result = obj->ops->open_file( obj, req->access, req->sharing, req->options )))
    {
        struct async_data async_data = {.user = req->async_user};

        reply->handle = alloc_handle( nt_current->process, result, req->access, req->attributes );
        async_data.handle = reply->handle;

        if (reply->handle && (fd = get_obj_fd( result )))
        {
            if (fd->fd_ops->create && (async = create_request_async( fd, &async_data, 1 )))
            {
                fd->fd_ops->create( fd, async, req->access, req->sharing, req->options );
                reply->wait = async_handoff( async, NULL, 1 );
                release_object( async );
            }
            release_object( fd );
        }

        release_object( result );
    }
    release_object( obj );
}

/* get the Unix name from a file handle */
DECL_HANDLER(get_handle_unix_name)
{
    struct nt_fd *fd;

    if ((fd = get_handle_fd_obj( nt_current->process, req->handle, 0 )))
    {
        if (fd->unix_name)
        {
            data_size_t name_len = strlen( fd->unix_name );
            reply->name_len = name_len;
            if (name_len <= get_reply_max_size()) set_reply_data( fd->unix_name, name_len );
            else set_error( STATUS_BUFFER_OVERFLOW );
        }
        else set_error( STATUS_OBJECT_TYPE_MISMATCH );
        release_object( fd );
    }
}

/* get a Unix fd to access a file */
DECL_HANDLER(get_handle_fd)
{
    struct nt_fd *fd;

    if ((fd = get_handle_fd_obj( nt_current->process, req->handle, 0 )))
    {
        int unix_fd = get_unix_fd( fd );
        reply->cacheable = fd->cacheable;
        if (unix_fd != -1)
        {
            reply->type = fd->fd_ops->get_fd_type( fd );
            reply->options = fd->options;
            reply->access = get_handle_access( nt_current->process, req->handle );
            send_client_fd( nt_current->process, unix_fd, req->handle );
        }
        release_object( fd );
    }
}

/* perform a read on a file object */
DECL_HANDLER(read)
{
    struct nt_fd *fd = get_handle_fd_obj( nt_current->process, req->async.handle, FILE_READ_DATA );
    struct async *async;

    if (!fd) return;

    if ((async = create_request_async( fd, &req->async, 0 )))
    {
        if (fd->fd_ops->read) fd->fd_ops->read( fd, async, req->pos );
        else set_error( STATUS_OBJECT_TYPE_MISMATCH );
        reply->wait = async_handoff( async, NULL, 0 );
        reply->options = fd->options;
        release_object( async );
    }
    release_object( fd );
}

/* perform a write on a file object */
DECL_HANDLER(write)
{
    struct nt_fd *fd = get_handle_fd_obj( nt_current->process, req->async.handle, FILE_WRITE_DATA );
    struct async *async;

    if (!fd) return;

    if ((async = create_request_async( fd, &req->async, 0 )))
    {
        if (fd->fd_ops->write) fd->fd_ops->write( fd, async, req->pos );
        else set_error( STATUS_OBJECT_TYPE_MISMATCH );
        reply->wait = async_handoff( async, &reply->size, 0 );
        reply->options = fd->options;
        release_object( async );
    }
    release_object( fd );
}

/* perform an ioctl on a file */
DECL_HANDLER(ioctl)
{
    unsigned int access = (req->code >> 14) & (FILE_READ_DATA|FILE_WRITE_DATA);
    struct nt_fd *fd = get_handle_fd_obj( nt_current->process, req->async.handle, access );
    struct async *async;

    if (!fd) return;

    if ((async = create_request_async( fd, &req->async, 0 )))
    {
        if (fd->fd_ops->ioctl) fd->fd_ops->ioctl( fd, req->code, async );
        else set_error( STATUS_OBJECT_TYPE_MISMATCH );
        reply->wait = async_handoff( async, NULL, 0 );
        reply->options = fd->options;
        release_object( async );
    }
    release_object( fd );
}

/* create / reschedule an async I/O */
DECL_HANDLER(register_async)
{
    unsigned int access;
    struct async *async;
    struct nt_fd *fd;

    switch(req->type)
    {
    case ASYNC_TYPE_READ:
        access = FILE_READ_DATA;
        break;
    case ASYNC_TYPE_WRITE:
        access = FILE_WRITE_DATA;
        break;
    default:
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }

    if ((fd = get_handle_fd_obj( nt_current->process, req->async.handle, access )))
    {
        if (!fd->fd_ops->queue_async) set_error( STATUS_OBJECT_TYPE_MISMATCH );
        else if (get_unix_fd( fd ) != -1 && (async = create_async( fd, nt_current, &req->async, NULL )))
        {
            fd->fd_ops->queue_async( fd, async, req->type, req->count );
            release_object( async );
        }
        release_object( fd );
    }
}

/* attach completion object to a fd */
DECL_HANDLER(set_completion_info)
{
    struct nt_fd *fd = get_handle_fd_obj( nt_current->process, req->handle, 0 );

    if (fd)
    {
        if (is_fd_overlapped( fd ) && !fd->completion)
        {
            fd->completion = get_completion_obj( nt_current->process, req->chandle, IO_COMPLETION_MODIFY_STATE );
            fd->comp_key = req->ckey;
            set_fd_signaled( fd, 1 );
        }
        else set_error( STATUS_INVALID_PARAMETER );
        release_object( fd );
    }
}

/* push new completion msg into a completion queue attached to the fd */
DECL_HANDLER(add_fd_completion)
{
    struct nt_fd *fd = get_handle_fd_obj( nt_current->process, req->handle, 0 );
    if (fd)
    {
        if (fd->completion && (req->async || !(fd->comp_flags & FILE_SKIP_COMPLETION_PORT_ON_SUCCESS)))
            add_completion( fd->completion, fd->comp_key, req->cvalue, req->status, req->information );
        release_object( fd );
    }
}

/* set fd completion information */
DECL_HANDLER(set_fd_completion_mode)
{
    struct nt_fd *fd = get_handle_fd_obj( nt_current->process, req->handle, 0 );
    if (fd)
    {
        if (is_fd_overlapped( fd ))
        {
            if (req->flags & FILE_SKIP_SET_EVENT_ON_HANDLE)
                set_fd_signaled( fd, 0 );
            /* removing flags is not allowed */
            fd->comp_flags |= req->flags & ( FILE_SKIP_COMPLETION_PORT_ON_SUCCESS
                                           | FILE_SKIP_SET_EVENT_ON_HANDLE
                                           | FILE_SKIP_SET_USER_EVENT_ON_FAST_IO );
        }
        else
            set_error( STATUS_INVALID_PARAMETER );
        release_object( fd );
    }
}

/* set fd disposition information */
DECL_HANDLER(set_fd_disp_info)
{
    struct nt_fd *fd = get_handle_fd_obj( nt_current->process, req->handle, DELETE );
    if (fd)
    {
        set_fd_disposition( fd, req->flags );
        release_object( fd );
    }
}

/* set fd name information */
DECL_HANDLER(set_fd_name_info)
{
    struct nt_fd *fd, *root_fd = NULL;
    struct unicode_str nt_name;

    if (req->namelen > get_req_data_size())
    {
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }
    nt_name.str = get_req_data();
    nt_name.len = (req->namelen / sizeof(WCHAR)) * sizeof(WCHAR);

    if (req->rootdir)
    {
        struct dir *root;

        if (!(root = get_dir_obj( nt_current->process, req->rootdir, 0 ))) return;
        root_fd = get_obj_fd( (struct object *)root );
        release_object( root );
        if (!root_fd) return;
    }

    if ((fd = get_handle_fd_obj( nt_current->process, req->handle, 0 )))
    {
        set_fd_name( fd, root_fd, (const char *)get_req_data() + req->namelen,
                     get_req_data_size() - req->namelen, nt_name, req->link, req->flags );
        release_object( fd );
    }
    if (root_fd) release_object( root_fd );
}

/* set fd eof information */
DECL_HANDLER(set_fd_eof_info)
{
    struct nt_fd *fd = get_handle_fd_obj( nt_current->process, req->handle, 0 );
    if (fd)
    {
        set_fd_eof( fd, req->eof );
        release_object( fd );
    }
}
