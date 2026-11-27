// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * win32k: USER's side of processes and threads, and the table of handlers.
 *
 * The handlers come over from Wine's server (window.c, queue.c, hook.c,
 * class.c, winstation.c, clipboard.c, atom.c, user.c, region.c) stage by
 * stage (docs/win32k.md); a request without a handler here gets ENOSYS.
 */

#include "user.h"

void user_process_init(struct w32_process *process)
{
}

/* the last thread of the process ended, as wineserver's process_killed and
 * process_destroy did for USER */
void user_process_exit(struct w32_process *process)
{
	close_process_desktop(process);
	destroy_process_classes(process);
	free_process_user_handles(process);
	set_rawinput_process(process, 0);
	free(process->rawinput_devices);
	process->rawinput_devices = NULL;
	process->rawinput_device_count = 0;
	process->rawinput_mouse = NULL;
	process->rawinput_kbd = NULL;
}

/* the thread ended, as wineserver's cleanup_thread did for USER */
void user_thread_exit(struct w32_thread *thread)
{
	cleanup_clipboard_thread(thread);
	destroy_thread_windows(thread);
	free_msg_queue(thread);
	release_thread_desktop(thread, 1);
}

#define H(name) [REQ_##name] = { w32_req_##name, sizeof(struct name##_request), sizeof(struct name##_reply) },
const struct w32_handler_entry win32k_handlers[REQ_NB_REQUESTS] = {
	W32_HANDLERS
};
#undef H

static int __init win32k_init(void)
{
	int ret;

	if ((ret = win32k_session_init()))
		return ret;
	return win32k_atoms_init();
}
device_initcall(win32k_init);
