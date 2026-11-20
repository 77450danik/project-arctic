// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * win32k: timeouts, as wineserver's add_timeout_user (server/fd.c).
 *
 * A timeout fires on an hrtimer; its callback runs in a work item under
 * win32k_lock, as the server runs it from its main loop, one thing at a time.
 * There is no call being served then: set_error() and the rest are no-ops.
 * remove_timeout_user, under the lock, either stops the timer before it
 * fires or marks a timeout whose work is already queued, which then frees it
 * without calling back.
 */

#include <linux/hrtimer.h>
#include <linux/workqueue.h>
#include "user.h"

struct timeout_user
{
	struct hrtimer timer;
	struct work_struct work;
	timeout_callback callback;
	void *private;
	bool removed;		/* under win32k_lock */
};

static void timeout_work(struct work_struct *work)
{
	struct timeout_user *user = container_of(work, struct timeout_user, work);

	mutex_lock(&win32k_lock);
	if (!user->removed)
		user->callback(user->private);
	mutex_unlock(&win32k_lock);
	kfree(user);
}

static enum hrtimer_restart timeout_fired(struct hrtimer *timer)
{
	struct timeout_user *user = container_of(timer, struct timeout_user, timer);

	queue_work(system_unbound_wq, &user->work);
	return HRTIMER_NORESTART;
}

/* when: negative, that long from now; else the time it is due (current_time) */
struct timeout_user *add_timeout_user(timeout_t when, timeout_callback func, void *private)
{
	struct timeout_user *user = kzalloc(sizeof(*user), GFP_KERNEL);
	s64 delay;

	if (!user) {
		set_error(STATUS_NO_MEMORY);
		return NULL;
	}
	delay = when < 0 ? -when : when - current_time_now();
	if (delay < 0)
		delay = 0;
	user->callback = func;
	user->private = private;
	INIT_WORK(&user->work, timeout_work);
	hrtimer_setup(&user->timer, timeout_fired, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	hrtimer_start(&user->timer, ns_to_ktime(delay * 100), HRTIMER_MODE_REL);
	return user;
}

void remove_timeout_user(struct timeout_user *user)
{
	if (hrtimer_try_to_cancel(&user->timer) == 1) {
		kfree(user);	/* stopped before it fired: no work is queued */
		return;
	}
	user->removed = true;	/* fired, or firing: its work frees it */
}
