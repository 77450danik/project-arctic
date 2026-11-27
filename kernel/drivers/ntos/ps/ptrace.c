// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * ntos: what wineserver did to other processes through signals and ptrace,
 * done by the kernel itself (docs/ntos.md)
 *
 * Its ptrace.c on Linux x86 came down to three things, kept here with their
 * outcomes: a signal to one thread (tgkill), another process's memory
 * (process_vm_readv and process_vm_writev, which honour the protection of
 * the pages; ptrace only when those were refused, which they are not for
 * the executive), and a thread's debug registers (PTRACE_PEEKUSER and
 * PTRACE_POKEUSER on u_debugreg, through the same functions of the kernel
 * those requests end in).
 *
 * Based on Wine's server/ptrace.c, Copyright (C) 1999 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "ntos.h"

/* initialize the process tracing mechanism */
void init_tracing_mechanism(void)
{
}

/* initialize the per-process tracing mechanism */
void init_process_tracing( struct process *process )
{
}

/* terminate the per-process tracing mechanism */
void finish_process_tracing( struct process *process )
{
}

/* no children of its own to reap */
void sigchld_callback(void)
{
}

/* send a Unix signal to a specific thread */
int send_thread_signal( struct thread *thread, int sig )
{
    int ret = -1;

    if (thread->unix_tid != -1) ret = nt_tgkill( thread->unix_pid, thread->unix_tid, sig );
    if (ret == -1 && errno == ESRCH) /* thread got killed */
    {
        thread->unix_pid = -1;
        thread->unix_tid = -1;
    }
    return (ret != -1);
}

/* return a thread of the process suitable for reaching its memory */
static struct thread *get_ptrace_thread( struct process *process )
{
    struct thread *thread;

    LIST_FOR_EACH_ENTRY( thread, &process->thread_list, struct thread, proc_entry )
    {
        if (thread->unix_pid != -1) return thread;
    }
    set_error( STATUS_PROCESS_IS_TERMINATING );  /* process is dead */
    return NULL;
}

/* read data from a process memory space */
int read_process_memory( struct process *process, client_ptr_t ptr, data_size_t size, char *dest )
{
    struct thread *thread = get_ptrace_thread( process );
    long len;

    if (!thread) return 0;

    if ((unsigned long)ptr != ptr)
    {
        set_error( STATUS_ACCESS_DENIED );
        return 0;
    }
    if (thread->unix_pid == -1 || !is_process_init_done( thread->process ))
    {
        set_error( STATUS_ACCESS_DENIED );
        return 0;
    }
    /* as process_vm_readv */
    len = nt_process_vm_read( thread->unix_pid, ptr, dest, size );
    if (len == size) return 1;
    set_error( STATUS_PARTIAL_COPY );
    return 0;
}

/* write data to a process memory space */
int write_process_memory( struct process *process, client_ptr_t ptr, data_size_t size, const char *src,
                          data_size_t *written )
{
    struct thread *thread = get_ptrace_thread( process );
    long len;

    if (!thread) return 0;

    if ((unsigned long)ptr != ptr)
    {
        set_error( STATUS_ACCESS_DENIED );
        return 0;
    }
    if (thread->unix_pid == -1 || !is_process_init_done( thread->process ))
    {
        set_error( STATUS_ACCESS_DENIED );
        return 0;
    }
    /* as process_vm_writev */
    len = nt_process_vm_write( thread->unix_pid, ptr, src, size );
    if (written) *written = max( len, 0l );
    if (len != size) set_error( STATUS_PARTIAL_COPY );
    return len == size;
}

/* initialize registers in new thread if necessary */
void init_thread_context( struct thread *thread )
{
    /* Linux doesn't clear all registers, but hopefully enough to avoid spurious breakpoints */
    thread->system_regs = 0;
}

/* retrieve the thread x86 debug registers */
void get_thread_context( struct thread *thread, struct context_data *context, unsigned int flags )
{
    unsigned long data[8];
    int i;

    /* all other regs are handled on the client side */
    assert( flags == SERVER_CTX_DEBUG_REGISTERS );

    if (!(thread->system_regs & SERVER_CTX_DEBUG_REGISTERS))
    {
        /* caller has initialized everything to 0 already, just return */
        context->flags |= SERVER_CTX_DEBUG_REGISTERS;
        return;
    }
    if (thread->unix_tid == -1 || !is_process_init_done( thread->process ))
    {
        set_error( STATUS_ACCESS_DENIED );
        return;
    }
    for (i = 0; i < 8; i++)
    {
        if (i == 4 || i == 5) continue;
        if (nt_debugreg_get( thread->unix_tid, i, &data[i] ) == -1)
        {
            file_set_error();
            return;
        }
    }
    switch (context->machine)
    {
    case IMAGE_FILE_MACHINE_I386:
        context->debug.i386_regs.dr0 = data[0];
        context->debug.i386_regs.dr1 = data[1];
        context->debug.i386_regs.dr2 = data[2];
        context->debug.i386_regs.dr3 = data[3];
        context->debug.i386_regs.dr6 = data[6];
        context->debug.i386_regs.dr7 = data[7];
        break;
    case IMAGE_FILE_MACHINE_AMD64:
        context->debug.x86_64_regs.dr0 = data[0];
        context->debug.x86_64_regs.dr1 = data[1];
        context->debug.x86_64_regs.dr2 = data[2];
        context->debug.x86_64_regs.dr3 = data[3];
        context->debug.x86_64_regs.dr6 = data[6];
        context->debug.x86_64_regs.dr7 = data[7];
        break;
    default:
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }
    context->flags |= SERVER_CTX_DEBUG_REGISTERS;
}

/* set the thread x86 debug registers, in the order PTRACE_POKEUSER took them */
void set_thread_context( struct thread *thread, const struct context_data *context, unsigned int flags )
{
    unsigned long dr[8];
    int tid = thread->unix_tid, i;

    /* all other regs are handled on the client side */
    assert( flags == SERVER_CTX_DEBUG_REGISTERS );

    if (tid == -1 || !is_process_init_done( thread->process ))
    {
        set_error( STATUS_ACCESS_DENIED );
        return;
    }
    switch (context->machine)
    {
    case IMAGE_FILE_MACHINE_I386:
        dr[0] = context->debug.i386_regs.dr0;
        dr[1] = context->debug.i386_regs.dr1;
        dr[2] = context->debug.i386_regs.dr2;
        dr[3] = context->debug.i386_regs.dr3;
        dr[6] = context->debug.i386_regs.dr6;
        dr[7] = context->debug.i386_regs.dr7;
        break;
    case IMAGE_FILE_MACHINE_AMD64:
        dr[0] = context->debug.x86_64_regs.dr0;
        dr[1] = context->debug.x86_64_regs.dr1;
        dr[2] = context->debug.x86_64_regs.dr2;
        dr[3] = context->debug.x86_64_regs.dr3;
        dr[6] = context->debug.x86_64_regs.dr6;
        dr[7] = context->debug.x86_64_regs.dr7;
        break;
    default:
        set_error( STATUS_INVALID_PARAMETER );
        return;
    }
    if (nt_debugreg_set( tid, 7, 0 ) == -1) goto error;
    for (i = 0; i < 7; i++)
    {
        if (i == 4 || i == 5) continue;
        if (nt_debugreg_set( tid, i, dr[i] ) == -1) goto error;
    }
    /* Linux 2.6.33+ needs enable bits set briefly to update value returned by PEEKUSER later */
    nt_debugreg_set( tid, 7, dr[7] | 0x55 );
    if (nt_debugreg_set( tid, 7, dr[7] ) == -1) goto error;
    thread->system_regs |= SERVER_CTX_DEBUG_REGISTERS;
    return;
 error:
    file_set_error();
}
