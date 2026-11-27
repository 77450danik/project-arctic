/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * ntos: everything a file of the server's code had from its headers
 * (docs/ntos.md). The order is the server's own: Wine's headers of Windows,
 * the server's protocol, then the server's headers, over the kernel's
 * constants and ntos's C library (crt.h).
 */
#ifndef NTOS_H
#define NTOS_H

/* the kernel's: what the server had from the C library's headers */
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/stat.h>
#include <linux/poll.h>
#include <linux/socket.h>
#include <linux/net.h>
#include <linux/in.h>
#include <linux/in6.h>
#include <linux/un.h>
#include <linux/tcp.h>
#include <linux/filter.h>
#include <linux/signal.h>
#include <linux/inotify.h>
#include <linux/sched.h>
#include <linux/major.h>
#include <linux/ntsync.h>
#include <linux/memfd.h>
#include <linux/uio.h>
#include <linux/fadvise.h>
#include <linux/mman.h>
#include <linux/rtnetlink.h>
#include <net/ipv6.h>
#include <linux/xattr.h>
#include <asm/termbits.h>

#include "config.h"

/* Wine's headers of Windows (shim/wine_headers.h) */
#include "wine_headers.h"

#include "crt.h"
#include "wine_list.h"

#undef assert
#define assert(x) WARN_ON_ONCE(!(x))

/* the server's own headers */
#include "object.h"
#include "file.h"
#include "handle.h"
#include "process.h"
#include "thread.h"
#include "request.h"
#include "security.h"
#include "unicode.h"
#include "user.h"
#include "ntos_kernel.h"

#endif
