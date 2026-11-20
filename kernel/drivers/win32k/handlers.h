/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * win32k: the requests of the Wine server protocol it serves, as they come
 * over from wineserver (docs/win32k.md). H(name) for each: the handler is
 * DECL_HANDLER(name) in the file it came with.
 */
#ifndef WIN32K_HANDLERS_H
#define WIN32K_HANDLERS_H

#define W32_HANDLERS \
	/* atom.c */ \
	H(add_atom) H(delete_atom) H(find_atom) H(get_atom_information) \
	H(add_user_atom) H(get_user_atom_name) \
	/* class.c */ \
	H(create_class) H(destroy_class) H(set_class_info) H(get_class_info) \
	/* handles.c (user.c of the server) */ \
	H(alloc_user_handle) H(free_user_handle) \
	/* window.c */ \
	H(create_window) H(set_window_fnid) H(set_parent) H(destroy_window) H(get_desktop_window) \
	H(set_window_owner) H(get_window_info) H(init_window_info) H(set_window_info) \
	H(get_window_parents) H(get_window_list) H(get_class_windows) \
	H(get_window_children_from_point) H(register_shell_hook_window) H(set_window_composited) \
	H(get_composition_list) H(set_snap_preview) H(set_window_band) H(get_window_tree) \
	H(set_window_pos) H(get_window_rectangles) H(get_window_text) H(set_window_text) \
	H(get_windows_offset) H(get_visible_region) H(get_window_region) H(set_window_region) \
	H(get_update_region) H(update_window_zorder) H(redraw_window) H(set_window_property) \
	H(remove_window_property) H(get_window_property) H(get_window_properties) \
	H(set_desktop_shell_windows) H(get_window_layered_info) H(set_window_layered_info) \
	/* queue.c */ \
	H(is_window_hung) H(get_msg_queue_handle) H(get_msg_queue) H(set_queue_fd) \
	H(set_queue_mask) H(get_queue_status) H(send_message) H(send_hardware_message) \
	H(post_quit_message) H(get_message) H(reply_message) H(accept_hardware_message) \
	H(get_message_reply) H(set_win_timer) H(kill_win_timer) H(register_hotkey) \
	H(unregister_hotkey) H(attach_thread_input) H(get_thread_input) H(get_key_state) \
	H(set_key_state) H(allow_set_foreground) H(set_foreground_window) H(set_focus_window) \
	H(set_active_window) H(set_capture_window) H(set_caret_window) H(set_caret_info) \
	H(set_user_input_time) H(set_cursor) H(get_cursor_history) H(get_rawinput_buffer) \
	H(update_rawinput_devices) H(set_keyboard_repeat) \
	/* hook.c */ \
	H(set_hook) H(remove_hook) H(start_hook_chain) H(finish_hook_chain) H(get_hook_info) \
	/* clipboard.c */ \
	H(open_clipboard) H(close_clipboard) H(set_clipboard_data) H(get_clipboard_data) \
	H(get_clipboard_formats) H(enum_clipboard_formats) H(empty_clipboard) \
	H(release_clipboard) H(get_clipboard_info) H(set_clipboard_viewer) \
	H(add_clipboard_listener) H(remove_clipboard_listener) \
	/* winstation.c: the rest of it stays with wineserver, which calls out */ \
	H(set_winstation_monitors)

/* each handler as the table calls it */
#define H(name) void w32_req_##name(const void *req, void *reply);
W32_HANDLERS
#undef H

#endif
