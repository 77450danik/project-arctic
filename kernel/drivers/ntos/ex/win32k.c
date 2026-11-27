// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * ntos: what the object manager tells win32k (Windows' process, thread and
 * window station callouts), as wineserver's server/win32k.c told it through
 * the system call win32k_callout; in the same kernel it is a call.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <uapi/linux/win32k.h>
#include <linux/ntos.h>
#include "ntos.h"

/* a callout; its NTSTATUS. ntos: win32k is in the same kernel, a call */
static unsigned int callout( struct win32k_callout *co )
{
    if (win32k_kernel_callout( co ))
    {
        nt_fprintf( stderr, "win32k callout %u failed\n", co->code );
        co->status = STATUS_INTERNAL_ERROR;
    }
    return co->status;
}

/* processes and threads */

void win32k_process_init( struct process *process, process_id_t parent_id, unsigned short machine )
{
    struct win32k_callout co = { .code = W32_CALLOUT_PROCESS_INIT, .process = process->id,
                                 .parent = parent_id, .value = machine };
    callout( &co );
}

void win32k_process_gone( struct process *process )
{
    struct win32k_callout co = { .code = W32_CALLOUT_PROCESS_GONE, .process = process->id };
    callout( &co );
}

void win32k_process_system( struct process *process )
{
    struct win32k_callout co = { .code = W32_CALLOUT_PROCESS_SYSTEM, .process = process->id };
    callout( &co );
}

void win32k_thread_init( struct thread *thread )
{
    struct win32k_callout co = { .code = W32_CALLOUT_THREAD_INIT, .thread = thread->id,
                                 .process = thread->process->id };
    callout( &co );
}

void win32k_thread_task( struct thread *thread )
{
    struct win32k_callout co = { .code = W32_CALLOUT_THREAD_TASK, .thread = thread->id,
                                 .value = thread->unix_tid };
    callout( &co );
}

void win32k_thread_gone( struct thread *thread )
{
    struct win32k_callout co = { .code = W32_CALLOUT_THREAD_GONE, .thread = thread->id };
    callout( &co );
}

/* window stations and desktops */

void win32k_winstation_create( struct winstation *winstation )
{
    struct win32k_callout co = { .code = W32_CALLOUT_WINSTATION_CREATE, .object = winstation->id,
                                 .flags = winstation->flags };
    callout( &co );
}

void win32k_winstation_delete( struct winstation *winstation )
{
    struct win32k_callout co = { .code = W32_CALLOUT_WINSTATION_DELETE, .object = winstation->id };
    callout( &co );
}

unsigned int win32k_winstation_flags( struct winstation *winstation, int set, unsigned int flags )
{
    struct win32k_callout co = { .code = W32_CALLOUT_WINSTATION_FLAGS, .object = winstation->id,
                                 .set = set, .flags = flags };
    callout( &co );
    return co.value;
}

unsigned int win32k_desktop_create( struct desktop *desktop, unsigned int flags, int existed )
{
    struct win32k_callout co = { .code = W32_CALLOUT_DESKTOP_CREATE, .object = desktop->id,
                                 .parent = desktop->winstation->id, .flags = flags,
                                 .thread = nt_current ? nt_current->id : 0, .set = existed };
    return callout( &co );
}

void win32k_desktop_delete( struct desktop *desktop )
{
    struct win32k_callout co = { .code = W32_CALLOUT_DESKTOP_DELETE, .object = desktop->id };
    callout( &co );
}

unsigned int win32k_desktop_flags( struct desktop *desktop, int set, unsigned int flags )
{
    struct win32k_callout co = { .code = W32_CALLOUT_DESKTOP_FLAGS, .object = desktop->id,
                                 .set = set, .flags = flags };
    callout( &co );
    return co.value;
}

struct obj_locator win32k_desktop_locator( struct desktop *desktop )
{
    struct win32k_callout co = { .code = W32_CALLOUT_DESKTOP_LOCATOR, .object = desktop->id };
    struct obj_locator locator = {0};

    if (!callout( &co ))
    {
        locator.id = co.locator_id;
        locator.offset = co.locator_offset;
    }
    return locator;
}

/* the id of the input desktop of a window station; 0 and an error if there is none */
unsigned int win32k_input_desktop( struct winstation *winstation )
{
    struct win32k_callout co = { .code = W32_CALLOUT_INPUT_DESKTOP, .parent = winstation->id };

    if (callout( &co )) set_error( co.status );
    return co.status ? 0 : co.object;
}

unsigned int win32k_set_input_desktop( struct winstation *winstation, struct desktop *desktop )
{
    struct win32k_callout co = { .code = W32_CALLOUT_SET_INPUT_DESKTOP, .parent = winstation->id,
                                 .object = desktop->id };
    return callout( &co );
}

void win32k_process_winstation( struct process *process, struct winstation *winstation, unsigned int access )
{
    struct win32k_callout co = { .code = W32_CALLOUT_PROCESS_WINSTATION, .process = process->id,
                                 .object = winstation ? winstation->id : 0, .value = access };
    callout( &co );
}

void win32k_process_desktop( struct process *process, struct desktop *desktop, unsigned int access )
{
    struct win32k_callout co = { .code = W32_CALLOUT_PROCESS_DESKTOP, .process = process->id,
                                 .object = desktop ? desktop->id : 0, .value = access };
    callout( &co );
}

/* the desktop of a thread: chosen (set_thread_desktop) or its default one;
 * the locator of its shared memory */
unsigned int win32k_thread_desktop( struct thread *thread, struct desktop *desktop, unsigned int access,
                                    int chosen, struct obj_locator *locator )
{
    struct win32k_callout co = { .code = W32_CALLOUT_THREAD_DESKTOP, .thread = thread->id,
                                 .object = desktop->id, .value = access, .set = chosen };

    if (!callout( &co ) && locator)
    {
        locator->id = co.locator_id;
        locator->offset = co.locator_offset;
    }
    return co.status;
}

void win32k_thread_desktop_release( struct thread *thread, int close )
{
    struct win32k_callout co = { .code = W32_CALLOUT_THREAD_DESKTOP_RELEASE, .thread = thread->id,
                                 .set = close };
    callout( &co );
}

void win32k_post_message( user_handle_t win, unsigned int message, lparam_t wparam, lparam_t lparam )
{
    struct win32k_callout co = { .code = W32_CALLOUT_POST_MESSAGE, .object = win, .flags = message,
                                 .value = wparam, .lparam = lparam };
    callout( &co );
}

