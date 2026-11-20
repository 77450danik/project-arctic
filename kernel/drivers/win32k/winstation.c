// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * Server-side window stations and desktops handling
 *
 * Copyright (C) 2002, 2005 Alexandre Julliard
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

/*
 * win32k: the USER bodies of window stations and desktops.
 *
 * The objects themselves, their names, handles, security and the requests
 * that open, close, name and enumerate them, stay with wineserver, the object
 * manager (docs/win32k.md). It tells win32k what happens to them through
 * win32k_callout, as Windows' object manager calls win32k's desktop and
 * window station callouts; a body is found by the id wineserver gave its
 * object. Access to the desktop or window station of a thread or process
 * was checked by wineserver when it was set, and is kept here with it.
 */

#include <linux/xarray.h>
#include "user.h"  /* win32k: all that the server's headers gave */

static struct list winstation_list = LIST_INIT(winstation_list);

/* id of wineserver's object → body; each holds the reference of the object */
static DEFINE_XARRAY(winstations);
static DEFINE_XARRAY(desktops);

static void winstation_dump( struct object *obj, int verbose );
static void winstation_destroy( struct object *obj );
static void desktop_dump( struct object *obj, int verbose );
static void desktop_destroy( struct object *obj );

static const struct object_ops winstation_ops =
{
    .size         = sizeof(struct winstation),
    .type         = &no_type,
    .dump         = winstation_dump,
    .destroy      = winstation_destroy,
};

static const struct object_ops desktop_ops =
{
    .size          = sizeof(struct desktop),
    .type          = &no_type,
    .dump          = desktop_dump,
    .destroy       = desktop_destroy,
};

static void winstation_dump( struct object *obj, int verbose )
{
    struct winstation *winstation = (struct winstation *)obj;

    fprintf( stderr, "Winstation flags=%x clipboard=%p atoms=%p\n",
             winstation->flags, winstation->clipboard, winstation->atom_table );
}

static struct winstation *create_winstation( object_id_t id, unsigned int flags )
{
    struct winstation *winstation;

    if (!(winstation = alloc_object( &winstation_ops ))) return NULL;
    winstation->id = id;
    winstation->flags = flags;
    winstation->input_desktop = NULL;
    winstation->clipboard = NULL;
    winstation->atom_table = NULL;
    winstation->monitors = NULL;
    winstation->monitor_count = 0;
    winstation->monitor_serial = 1;
    list_init( &winstation->desktops );
    if (xa_err( xa_store( &winstations, id, winstation, GFP_KERNEL ) ))
    {
        set_error( STATUS_NO_MEMORY );
        release_object( winstation );
        return NULL;
    }
    wlist_add_tail( &winstation_list, &winstation->entry );
    return winstation;
}

static void winstation_destroy( struct object *obj )
{
    struct winstation *winstation = (struct winstation *)obj;

    if (winstation->entry.next) list_remove( &winstation->entry );
    if (winstation->clipboard) release_object( winstation->clipboard );
    if (winstation->atom_table) release_object( winstation->atom_table );
    free( winstation->monitors );
}

/* retrieve the process window station, checking the access wineserver granted */
struct winstation *get_process_winstation( struct w32_process *process, unsigned int access )
{
    if (!process->winstation)
    {
        set_error( STATUS_INVALID_HANDLE );
        return NULL;
    }
    if (access & ~process->winstation_access)
    {
        set_error( STATUS_ACCESS_DENIED );
        return NULL;
    }
    return (struct winstation *)grab_object( process->winstation );
}

/* retrieve the visible winstation */
struct winstation *get_visible_winstation(void)
{
    struct winstation *winstation;
    LIST_FOR_EACH_ENTRY( winstation, &winstation_list, struct winstation, entry )
        if (winstation->flags & WSF_VISIBLE) return winstation;
    return NULL;
}

/* retrieve the winstation current input desktop */
struct desktop *get_input_desktop( struct winstation *winstation )
{
    struct desktop *desktop;
    if (!(desktop = winstation->input_desktop)) return NULL;
    return (struct desktop *)grab_object( desktop );
}

/* changes the winstation current input desktop and update its input time */
int set_input_desktop( struct winstation *winstation, struct desktop *new_desktop )
{
    struct desktop *old_desktop = winstation->input_desktop;
    struct w32_thread *thread;

    if (!(winstation->flags & WSF_VISIBLE)) return 0;
    if (new_desktop) new_desktop->input_time = current_time_now();
    if (old_desktop == new_desktop) return 1;

    if (old_desktop)
    {
        /* disconnect every process of the old input desktop from rawinput */
        LIST_FOR_EACH_ENTRY( thread, &old_desktop->threads, struct w32_thread, desktop_entry )
            set_rawinput_process( thread->process, 0 );
    }

    if ((winstation->input_desktop = new_desktop))
    {
        /* connect every process of the new input desktop to rawinput */
        LIST_FOR_EACH_ENTRY( thread, &new_desktop->threads, struct w32_thread, desktop_entry )
            set_rawinput_process( thread->process, 1 );
    }

    return 1;
}

/* a desktop by the id of its object: wineserver checked the access of the
 * handle a request had and ntdll gave its id (docs/win32k.md) */
struct desktop *get_desktop_obj( struct w32_process *process, obj_handle_t id, unsigned int access )
{
    struct desktop *desktop = xa_load( &desktops, id );

    if (!desktop)
    {
        set_error( STATUS_INVALID_HANDLE );
        return NULL;
    }
    return (struct desktop *)grab_object( desktop );
}

static void desktop_dump( struct object *obj, int verbose )
{
    struct desktop *desktop = (struct desktop *)obj;

    fprintf( stderr, "Desktop flags=%x winstation=%p top_win=%p hooks=%p\n",
             desktop->shared->flags, desktop->winstation, desktop->top_window, desktop->global_hooks );
}

static struct desktop *create_desktop( object_id_t id, struct winstation *winstation,
                                       unsigned int flags, struct w32_thread *creator )
{
    struct desktop *desktop, *current_desktop;

    /* inherit DF_WINE_*_DESKTOP flags if none of them are specified */
    if (!(flags & (DF_WINE_ROOT_DESKTOP | DF_WINE_VIRTUAL_DESKTOP)) && creator &&
        (current_desktop = creator->desktop))
        flags |= current_desktop->shared->flags & (DF_WINE_VIRTUAL_DESKTOP | DF_WINE_ROOT_DESKTOP);

    if (!(desktop = alloc_object( &desktop_ops ))) return NULL;
    desktop->id = id;
    list_init( &desktop->threads );
    list_init( &desktop->hotkeys );
    list_init( &desktop->shell_hooks );
    list_init( &desktop->pointers );
    if (!(desktop->shared = alloc_shared_object( sizeof(*desktop->shared) )))
    {
        release_object( desktop );
        return NULL;
    }
    if (xa_err( xa_store( &desktops, id, desktop, GFP_KERNEL ) ))
    {
        set_error( STATUS_NO_MEMORY );
        release_object( desktop );
        return NULL;
    }

    desktop->winstation = (struct winstation *)grab_object( winstation );
    desktop->top_window = NULL;
    desktop->msg_window = NULL;
    desktop->shell_window = NULL;
    desktop->shell_listview = NULL;
    desktop->progman_window = NULL;
    desktop->taskman_window = NULL;
    desktop->global_hooks = NULL;
    desktop->close_timeout = NULL;
    desktop->foreground_input = NULL;
    desktop->foreground_pid = 0;
    desktop->foreground_allowed = 0;
    desktop->users = 0;
    desktop->clip_flags = 0;
    desktop->cursor_win = 0;
    desktop->snap_window = 0;
    memset( &desktop->snap_rect, 0, sizeof(desktop->snap_rect) );
    desktop->alt_pressed = 0;
    memset( &desktop->key_repeat, 0, sizeof(desktop->key_repeat) );
    wlist_add_tail( &winstation->desktops, &desktop->entry );

    SHARED_WRITE_BEGIN( desktop->shared, desktop_shm_t )
    {
        shared->flags = flags;
        shared->cursor.x = 0;
        shared->cursor.y = 0;
        shared->cursor.last_change = 0;
        shared->cursor.clip.left = 0;
        shared->cursor.clip.top = 0;
        shared->cursor.clip.right = 0;
        shared->cursor.clip.bottom = 0;
        memset( (void *)shared->keystate, 0, sizeof(shared->keystate) );
        shared->keystate_serial = 1;
        shared->monitor_serial = winstation->monitor_serial;
    }
    SHARED_WRITE_END;

    return desktop;
}

static void desktop_destroy( struct object *obj )
{
    struct desktop *desktop = (struct desktop *)obj;
    struct winstation *winstation = desktop->winstation;

    if (!winstation)  /* failed in create_desktop */
    {
        if (desktop->shared) free_shared_object( desktop->shared );
        return;
    }

    list_remove( &desktop->entry );

    if (desktop == winstation->input_desktop)
    {
        struct desktop *other, *found = NULL;
        LIST_FOR_EACH_ENTRY(other, &winstation->desktops, struct desktop, entry)
            if (!found || other->input_time > found->input_time) found = other;
        set_input_desktop( winstation, found );
    }

    free_hotkeys( desktop, 0 );
    free_shell_hooks( desktop );
    free_pointers( desktop );
    if (desktop->top_window) free_window_handle( desktop->top_window );
    if (desktop->msg_window) free_window_handle( desktop->msg_window );
    if (desktop->global_hooks) release_object( desktop->global_hooks );
    if (desktop->close_timeout) remove_timeout_user( desktop->close_timeout );
    if (desktop->key_repeat.timeout) remove_timeout_user( desktop->key_repeat.timeout );
    release_object( desktop->winstation );
    if (desktop->shared) free_shared_object( desktop->shared );
}

/* retrieve the thread desktop, checking the access wineserver granted */
struct desktop *get_thread_desktop( struct w32_thread *thread, unsigned int access )
{
    if (!thread->desktop)
    {
        set_error( STATUS_INVALID_HANDLE );
        return NULL;
    }
    if (access & ~thread->desktop_access)
    {
        set_error( STATUS_ACCESS_DENIED );
        return NULL;
    }
    return (struct desktop *)grab_object( thread->desktop );
}

static void close_desktop_timeout( void *private )
{
    struct desktop *desktop = private;

    desktop->close_timeout = NULL;
    /* win32k: the name stays with wineserver's object; WM_CLOSE makes the
     * owner quit, and its handles, the last ones, go with it */
    post_desktop_message( desktop, WM_CLOSE, 0, 0 );  /* and signal the owner to quit */
}

/* add a user of the desktop and cancel the close timeout */
static void add_desktop_thread( struct desktop *desktop, struct w32_thread *thread )
{
    wlist_add_tail( &desktop->threads, &thread->desktop_entry );
    add_desktop_hook_count( desktop, thread, 1 );

    if (!thread->process->is_system)
    {
        desktop->users++;
        if (desktop->close_timeout)
        {
            remove_timeout_user( desktop->close_timeout );
            desktop->close_timeout = NULL;
        }
    }

    /* if thread process is now connected to the input desktop, let it receive rawinput */
    if (desktop == desktop->winstation->input_desktop) set_rawinput_process( thread->process, 1 );
}

/* remove a user of the desktop and start the close timeout if necessary */
static void remove_desktop_user( struct desktop *desktop, struct w32_thread *thread )
{
    struct w32_process *process;

    assert( desktop->users > 0 );
    desktop->users--;

    /* if we have one remaining user, it has to be the manager of the desktop window */
    if ((process = get_top_window_owner( desktop )) && desktop->users == process->threads && !desktop->close_timeout)
        desktop->close_timeout = add_timeout_user( -TICKS_PER_SEC, close_desktop_timeout, desktop );
}

/* remove a thread from the list of threads attached to a desktop */
static void remove_desktop_thread( struct desktop *desktop, struct w32_thread *thread )
{
    add_desktop_hook_count( desktop, thread, -1 );
    list_remove( &thread->desktop_entry );

    if (!thread->process->is_system) remove_desktop_user( desktop, thread );

    if (desktop == desktop->winstation->input_desktop)
    {
        /* thread process might still be connected the input desktop through another thread, update the full list */
        LIST_FOR_EACH_ENTRY( thread, &desktop->threads, struct w32_thread, desktop_entry )
            set_rawinput_process( thread->process, 1 );
    }
}

/* set the thread default desktop */
static void set_thread_default_desktop( struct w32_thread *thread, struct desktop *desktop, unsigned int access )
{
    if (thread->desktop) return;  /* nothing to do */

    thread->desktop = (struct desktop *)grab_object( desktop );
    thread->desktop_access = access;
    add_desktop_thread( desktop, thread );
}

/* set the process default desktop */
void set_process_default_desktop( struct w32_process *process, struct desktop *desktop )
{
    struct w32_thread *thread;

    if (process->desktop == desktop) return;  /* nothing to do */

    if (process->desktop) release_object( process->desktop );
    process->desktop = desktop ? (struct desktop *)grab_object( desktop ) : NULL;
    if (!desktop) return;

    /* set desktop for threads that don't have one yet */
    LIST_FOR_EACH_ENTRY( thread, &process->thread_list, struct w32_thread, proc_entry )
        set_thread_default_desktop( thread, desktop, process->desktop_access );
}

/* release (and eventually close) the desktop of a given thread */
void release_thread_desktop( struct w32_thread *thread, int close )
{
    struct desktop *desktop;

    if (!(desktop = thread->desktop)) return;

    if (close) remove_desktop_thread( desktop, thread );
    else if (!thread->process->is_system) remove_desktop_user( desktop, thread );

    if (close)
    {
        thread->desktop = NULL;
        release_object( desktop );
    }
}

/* the process ended: its window station and default desktop go */
void close_process_desktop( struct w32_process *process )
{
    set_process_default_desktop( process, NULL );
    if (process->winstation) release_object( process->winstation );
    process->winstation = NULL;
}

/* set the process current window station monitors */
DECL_HANDLER(set_winstation_monitors)
{
    struct winstation *winstation;
    struct desktop *desktop;
    unsigned int size = get_req_data_size();

    if (!(winstation = get_process_winstation( w32_current->process, 0 ))) return;

    if (req->increment || winstation->monitor_count != size / sizeof(*winstation->monitors) ||
        !winstation->monitors || memcmp( winstation->monitors, get_req_data(), size ))
        winstation->monitor_serial++;

    free( winstation->monitors );
    winstation->monitors = NULL;
    winstation->monitor_count = 0;

    LIST_FOR_EACH_ENTRY(desktop, &winstation->desktops, struct desktop, entry)
    {
        SHARED_WRITE_BEGIN( desktop->shared, desktop_shm_t )
        {
            shared->monitor_serial = winstation->monitor_serial;
        }
        SHARED_WRITE_END;
    }

    if (size && (winstation->monitors = memdup( get_req_data(), size )))
        winstation->monitor_count = size / sizeof(*winstation->monitors);
    update_windows_monitor_dpi( winstation );

    reply->serial = winstation->monitor_serial;
    release_object( winstation );
}

/* ------------------------------------------------------------------------
 * win32k: the callouts of the object manager (win32k_callout, main.c), under
 * win32k_lock. They are the USER halves of wineserver's create_winstation,
 * create_desktop, set_input_desktop, open_input_desktop, set_thread_desktop,
 * get_thread_desktop, set_user_object_info, connect_process_winstation,
 * make_process_system, and of the objects going.
 */

static struct winstation *callout_winstation( object_id_t id )
{
    struct winstation *winstation = xa_load( &winstations, id );

    if (!winstation) set_error( STATUS_INVALID_HANDLE );
    return winstation;
}

static struct desktop *callout_desktop( object_id_t id )
{
    struct desktop *desktop = xa_load( &desktops, id );

    if (!desktop) set_error( STATUS_INVALID_HANDLE );
    return desktop;
}

static void set_locator( struct win32k_callout *co, desktop_shm_t *shared )
{
    struct obj_locator locator = get_shared_object_locator( shared );

    co->locator_id = locator.id;
    co->locator_offset = locator.offset;
}

/* a new wineserver registered: the objects of the last one are gone, and its
 * ids will be given again */
void winstation_reset(void)
{
    struct winstation *winstation;
    struct desktop *desktop;
    unsigned long id;

    xa_for_each( &desktops, id, desktop )
    {
        xa_erase( &desktops, id );
        release_object( desktop );
    }
    xa_for_each( &winstations, id, winstation )
    {
        xa_erase( &winstations, id );
        list_remove( &winstation->entry );
        winstation->entry.next = NULL;
        release_object( winstation );
    }
}

void winstation_callout( struct win32k_callout *co, struct w32_thread *thread, struct w32_process *process )
{
    struct winstation *winstation;
    struct desktop *desktop, *old_desktop;

    switch (co->code)
    {
    case W32_CALLOUT_WINSTATION_CREATE:
        if (xa_load( &winstations, co->object )) set_error( STATUS_OBJECT_NAME_COLLISION );
        else create_winstation( co->object, co->flags );
        break;

    case W32_CALLOUT_WINSTATION_DELETE:
        if (!(winstation = callout_winstation( co->object ))) break;
        xa_erase( &winstations, co->object );
        list_remove( &winstation->entry );
        winstation->entry.next = NULL;
        release_object( winstation );
        break;

    case W32_CALLOUT_WINSTATION_FLAGS:
        if (!(winstation = callout_winstation( co->object ))) break;
        co->value = winstation->flags;
        if (co->set) winstation->flags = co->flags;
        break;

    case W32_CALLOUT_DESKTOP_CREATE:
        if (!(winstation = callout_winstation( co->parent ))) break;
        if ((desktop = xa_load( &desktops, co->object )))
        {
            /* create_desktop of a name that exists: wineserver opened it */
            SHARED_WRITE_BEGIN( desktop->shared, desktop_shm_t )
            {
                shared->flags |= co->flags & (DF_WINE_VIRTUAL_DESKTOP | DF_WINE_ROOT_DESKTOP);
            }
            SHARED_WRITE_END;
        }
        else if (!(desktop = create_desktop( co->object, winstation, co->flags, thread ))) break;
        if (!winstation->input_desktop) set_input_desktop( winstation, desktop );
        break;

    case W32_CALLOUT_DESKTOP_DELETE:
        if (!(desktop = callout_desktop( co->object ))) break;
        xa_erase( &desktops, co->object );
        release_object( desktop );
        break;

    case W32_CALLOUT_DESKTOP_FLAGS:
        if (!(desktop = callout_desktop( co->object ))) break;
        co->value = desktop->shared->flags;
        if (co->set)
        {
            SHARED_WRITE_BEGIN( desktop->shared, desktop_shm_t )
            {
                shared->flags = co->flags;
            }
            SHARED_WRITE_END;
        }
        break;

    case W32_CALLOUT_DESKTOP_LOCATOR:
        if ((desktop = callout_desktop( co->object ))) set_locator( co, desktop->shared );
        break;

    case W32_CALLOUT_INPUT_DESKTOP:
        if (!(winstation = callout_winstation( co->parent ))) break;
        if (!(winstation->flags & WSF_VISIBLE)) set_error( STATUS_ILLEGAL_FUNCTION );
        else co->object = winstation->input_desktop ? winstation->input_desktop->id : 0;
        break;

    case W32_CALLOUT_SET_INPUT_DESKTOP:
        if (!(winstation = callout_winstation( co->parent ))) break;
        if (!(desktop = callout_desktop( co->object ))) break;
        if (!set_input_desktop( winstation, desktop )) set_error( STATUS_ILLEGAL_FUNCTION );
        break;

    case W32_CALLOUT_PROCESS_WINSTATION:
        winstation = NULL;
        if (co->object && !(winstation = callout_winstation( co->object ))) break;
        if (process->winstation) release_object( process->winstation );
        process->winstation = winstation ? (struct winstation *)grab_object( winstation ) : NULL;
        process->winstation_access = co->value;
        break;

    case W32_CALLOUT_PROCESS_DESKTOP:
        desktop = NULL;
        if (co->object && !(desktop = callout_desktop( co->object ))) break;
        if (desktop && desktop->winstation != process->winstation)
        {
            set_error( STATUS_ACCESS_DENIED );
            break;
        }
        process->desktop_access = co->value;
        set_process_default_desktop( process, desktop );
        break;

    case W32_CALLOUT_PROCESS_SYSTEM:
        if (process->is_system) break;
        LIST_FOR_EACH_ENTRY( thread, &process->thread_list, struct w32_thread, proc_entry )
            release_thread_desktop( thread, 0 );
        process->is_system = 1;
        break;

    case W32_CALLOUT_THREAD_DESKTOP:
        if (!(desktop = callout_desktop( co->object ))) break;
        if (!co->set)  /* the default desktop of a new thread */
        {
            set_thread_default_desktop( thread, desktop, co->value );
            set_locator( co, thread->desktop->shared );
            break;
        }
        /* set_thread_desktop: wineserver checked it is of the process's window station */
        old_desktop = thread->desktop;

        /* when changing desktop, we can't have any users on the current one */
        if (old_desktop != desktop && thread->desktop_users > 0)
        {
            set_error( STATUS_DEVICE_BUSY );
            break;
        }
        if (old_desktop != desktop)
        {
            if (old_desktop)
            {
                remove_desktop_thread( old_desktop, thread );
                thread->desktop = NULL;
            }
            thread->desktop = (struct desktop *)grab_object( desktop );
            add_desktop_thread( desktop, thread );
        }
        thread->desktop_access = co->value;
        set_locator( co, desktop->shared );

        if (!thread->process->desktop)
        {
            thread->process->desktop_access = co->value;
            set_process_default_desktop( thread->process, desktop );
        }

        if (old_desktop != desktop)
        {
            if (thread->queue) detach_thread_input( thread->queue, NULL, desktop );
            if (old_desktop) release_object( old_desktop );  /* the thread's reference */
        }
        break;

    case W32_CALLOUT_THREAD_DESKTOP_RELEASE:
        release_thread_desktop( thread, co->set );
        break;

    default:
        set_error( STATUS_NOT_IMPLEMENTED );
        break;
    }
}
