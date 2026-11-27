/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * ntos: names that win32k has too. Both came from wineserver and are built
 * into the same kernel: win32k's work on its USER bodies (struct w32_process,
 * struct w32_thread), ntos's on the objects of NT. Included first in every
 * file of ntos (Makefile), so that its own calls and definitions agree.
 */
#ifndef NTOS_NAMES_H
#define NTOS_NAMES_H

#define add_timeout_user		ntos_add_timeout_user
#define remove_timeout_user		ntos_remove_timeout_user
#define alloc_shared_object		ntos_alloc_shared_object
#define free_shared_object		ntos_free_shared_object
#define get_shared_object_locator	ntos_get_shared_object_locator
#define invalidate_shared_object	ntos_invalidate_shared_object
#define shared_session			ntos_shared_session
#define get_process_from_id		ntos_get_process_from_id
#define get_thread_from_id		ntos_get_thread_from_id
#define get_process_winstation		ntos_get_process_winstation
#define get_visible_winstation		ntos_get_visible_winstation
#define get_desktop_obj			ntos_get_desktop_obj
#define get_input_desktop		ntos_get_input_desktop
#define set_input_desktop		ntos_set_input_desktop
#define get_thread_desktop		ntos_get_thread_desktop
#define release_thread_desktop		ntos_release_thread_desktop
#define close_process_desktop		ntos_close_process_desktop
#define set_process_default_desktop	ntos_set_process_default_desktop
/* the server's USER side, which win32k has and ntos keeps empty (ex/user.c) */
#define add_desktop_hook_count		ntos_add_desktop_hook_count
#define cleanup_clipboard_thread	ntos_cleanup_clipboard_thread
#define destroy_process_classes		ntos_destroy_process_classes
#define destroy_thread_windows		ntos_destroy_thread_windows
#define detach_thread_input		ntos_detach_thread_input
#define free_hotkeys			ntos_free_hotkeys
#define free_msg_queue			ntos_free_msg_queue
#define free_pointers			ntos_free_pointers
#define free_process_user_handles	ntos_free_process_user_handles
#define free_shell_hooks		ntos_free_shell_hooks
#define free_window_handle		ntos_free_window_handle
#define get_top_window_owner		ntos_get_top_window_owner
#define post_desktop_message		ntos_post_desktop_message
#define post_message			ntos_post_message
#define set_rawinput_process		ntos_set_rawinput_process
#define update_windows_monitor_dpi	ntos_update_windows_monitor_dpi

#endif
