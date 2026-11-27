// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * Server-side /proc support for Solaris
 *
 * Copyright (C) 2007 Alexandre Julliard
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

#ifdef USE_PROCFS

/* procfs doesn't support large files */
# undef _FILE_OFFSET_BITS
# define _FILE_OFFSET_BITS 32

static int open_proc_as( struct process *process, int flags )
{
    char buffer[32];
    int fd;

    if (process->unix_pid == -1)
    {
        set_error( STATUS_ACCESS_DENIED );
        return -1;
    }

    snprintf( buffer, sizeof(buffer), "/proc/%u/as", process->unix_pid );
    if ((fd = nt_open( buffer, flags )) == -1)
    {
        if (errno == ENOENT)  /* probably got killed */
        {
            process->unix_pid = -1;
            set_error( STATUS_ACCESS_DENIED );
        }
        else file_set_error();
    }
    return fd;
}

static int open_proc_lwpctl( struct thread *thread )
{
    char buffer[48];
    int fd;

    if (thread->unix_pid == -1) return -1;

    snprintf( buffer, sizeof(buffer), "/proc/%u/lwp/%u/lwpctl", thread->unix_pid, thread->unix_tid );
    if ((fd = nt_open( buffer, O_WRONLY )) == -1)
    {
        if (errno == ENOENT)  /* probably got killed */
            thread->unix_pid = thread->unix_tid = -1;
        else
            file_set_error();
    }
    return fd;
}

/* handle a SIGCHLD signal */
void sigchld_callback(void)
{
    assert( 0 );  /* should only be called when using ptrace */
}

/* initialize the process tracing mechanism */
void init_tracing_mechanism(void)
{
    /* no initialization needed */
}

/* initialize the per-process tracing mechanism */
void init_process_tracing( struct process *process )
{
    /* setup is done on-demand */
}

/* terminate the per-process tracing mechanism */
void finish_process_tracing( struct process *process )
{
}

/* send a Unix signal to a specific thread */
int send_thread_signal( struct thread *thread, int sig )
{
    int fd = open_proc_lwpctl( thread );
    long kill[2];
    ssize_t ret;

    if (fd == -1) return 0;

    kill[0] = PCKILL;
    kill[1] = sig;
    ret = nt_write( fd, kill, sizeof(kill) );
    nt_close( fd );
    return (ret == sizeof(kill));
}

/* read data from a process memory space */
int read_process_memory( struct process *process, client_ptr_t ptr, size_t size, char *dest )
{
    ssize_t ret;
    int fd;

    if ((off_t)ptr != ptr)
    {
        set_error( STATUS_ACCESS_DENIED );
        return 0;
    }

    if ((fd = open_proc_as( process, O_RDONLY )) == -1) return 0;

    ret = nt_pread( fd, dest, size, (off_t)ptr );
    nt_close( fd );
    if (ret == size) return 1;

    if (ret == -1) file_set_error();
    else set_error( STATUS_ACCESS_VIOLATION );
    return 0;
}

/* write data to a process memory space */
int write_process_memory( struct process *process, client_ptr_t ptr, size_t size, const char *src,
                          data_size_t *written )
{
    ssize_t ret;
    int fd;

    if ((off_t)ptr != ptr)
    {
        set_error( STATUS_ACCESS_DENIED );
        return 0;
    }

    if ((fd = open_proc_as( process, O_WRONLY )) == -1) return 0;

    ret = nt_pwrite( fd, src, size, (off_t)ptr );
    nt_close( fd );
    if (ret == size && written) *written = size;
    if (ret == size) return 1;

    if (ret == -1) file_set_error();
    else set_error( STATUS_ACCESS_VIOLATION );
    return 0;
}

/* initialize registers in new thread if necessary */
void init_thread_context( struct thread *thread )
{
}

/* retrieve the thread registers */
void get_thread_context( struct thread *thread, struct context_data *context, unsigned int flags )
{
    /* FIXME: get debug registers */
}

/* set the thread registers */
void set_thread_context( struct thread *thread, const struct context_data *context, unsigned int flags )
{
    /* FIXME: set debug registers */
}

#endif /* USE_PROCFS */
