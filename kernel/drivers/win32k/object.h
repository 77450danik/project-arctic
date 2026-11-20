/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * win32k: objects as wineserver counts them (server/object.h), without NT
 * handles or names: those stay with wineserver's own objects.
 */
#ifndef WIN32K_OBJECT_H
#define WIN32K_OBJECT_H

#include <linux/types.h>

struct object;

struct object_ops
{
	size_t size;
	const void *type;			/* kept for the ported tables; unused */
	void (*dump)(struct object *obj, int verbose);	/* unused */
	void (*destroy)(struct object *obj);
};

struct object
{
	unsigned int refcount;
	const struct object_ops *ops;
};

static const char no_type __maybe_unused;	/* the ported tables point .type at it */

#endif
