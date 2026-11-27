// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * ntos: the server's USER side, as wineserver had it while win32k served USER
 *
 * wineserver kept its own queues, windows, hooks, classes, hot keys and
 * clipboard (queue.c, window.c, hook.c, class.c, clipboard.c, user.c) next to
 * win32k's, and with win32k on they stayed empty: win32k keeps all of them
 * and hears of a thread, process or desktop through the callouts
 * (ex/win32k.c). ntos has no copy of those files, so these are what the
 * server's functions did over empty lists. Without them each call bound to
 * win32k's function of the same name, on win32k's structures (a desktop of
 * ntos taken for one of win32k's: the oops in free_hotkeys, 2026-11-22).
 * Named apart from win32k's by shim/ntos_names.h.
 */

#include "ntos.h"  /* ntos: all that the server's headers and the C library gave */

/* hook.c: no global hooks on the server's desktop */
void add_desktop_hook_count( struct desktop *desktop, struct thread *thread, int count )
{
}

/* clipboard.c: the clipboard is win32k's */
void cleanup_clipboard_thread( struct thread *thread )
{
}

/* class.c: window classes are win32k's */
void destroy_process_classes( struct process *process )
{
}

/* window.c: windows, shell hooks and the desktop's own windows are win32k's */
void destroy_thread_windows( struct thread *thread )
{
}

void free_shell_hooks( struct desktop *desktop )
{
}

void free_window_handle( struct window *win )
{
}

struct process *get_top_window_owner( struct desktop *desktop )
{
    return NULL;  /* no desktop window on the server's side */
}

void post_desktop_message( struct desktop *desktop, unsigned int message, lparam_t wparam, lparam_t lparam )
{
}

void update_windows_monitor_dpi( struct winstation *winstation )
{
}

/* queue.c: message queues, hot keys, pointers and raw input are win32k's */
void detach_thread_input( struct msg_queue *queue_from, struct msg_queue *queue_to, struct desktop *desktop )
{
}

void free_hotkeys( struct desktop *desktop, user_handle_t window )
{
}

void free_msg_queue( struct thread *thread )
{
}

void free_pointers( struct desktop *desktop )
{
}

void post_message( user_handle_t win, unsigned int message, lparam_t wparam, lparam_t lparam )
{
}

void set_rawinput_process( struct process *process, int enable )
{
}

/* user.c: the handles of USER objects are win32k's */
void free_process_user_handles( struct process *process )
{
}
