/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * ntos: Wine's own headers in the kernel (docs/ntos.md). The server's code is
 * built against the same windef.h, winternl.h, ddk/wdm.h, ws2def.h as it is in
 * Wine, so that every structure and constant is the one ntdll sees. The C
 * library they take comes from shim/; the kernel's macros that clash are put
 * aside while they are read, and every kernel header ntos needs comes before
 * (ntos.h), so none is read with Wine's macros in force.
 */
#pragma once

/* what shim/ gives, first: its headers are guarded and add nothing later */
#include <linux/ctype.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/stdarg.h>
#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/time.h>
#include <linux/types.h>

/* the kernel's attribute macros of the same names as the attributes */
#pragma push_macro("__alloc_size__")
#undef __alloc_size__
#undef ARRAY_SIZE
#undef PAGE_ALIGN
#define NOMINMAX
/* Windows' page protections, not x86's page table bits */
#undef PAGE_NONE
#undef PAGE_READONLY
#undef PAGE_EXECUTE
#undef PAGE_SHARED
#undef PAGE_COPY
/* Wine's structures have fields of that name; ntos calls the thread it
 * serves nt_current and the kernel's task get_current() */
#undef current

#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#undef WIN32_NO_STATUS
#include "ntstatus.h"
#include "winbase.h"
#include "winioctl.h"
#undef DEVICE_TYPE
#include "ddk/wdm.h"
#include "winerror.h"

/* the sockets' headers with the WS_ prefix, as sock.c has them */
#define USE_WS_PREFIX
#include "winsock2.h"
#include "ws2tcpip.h"
#include "tcpmib.h"
#include "wsipx.h"
#include "af_irda.h"
#include "bthsdpdef.h"
#include "bluetoothapis.h"
#include "bthdef.h"
#include "ws2bth.h"
#include "wine/afd.h"
#include "wincon.h"
#include "wine/condrv.h"
#include "ddk/ntddser.h"
#include "ddk/ntifs.h"
#include "winuser.h"
#include "ntuser.h"

/* the kernel has a struct thread_info of its own: the protocol's is nt_thread_info */
#define thread_info nt_thread_info
#include "wine/server_protocol.h"
#undef thread_info

/* Wine's red-black tree beside the kernel's */
#define rb_next wrb_next
#define rb_prev wrb_prev
#include "wine/rbtree.h"

/* the kernel's again, after Wine's; Wine's attribute in the kernel's spelling */
#pragma pop_macro("__alloc_size__")
#undef __WINE_ALLOC_SIZE
#define __WINE_ALLOC_SIZE(...) __alloc_size__(__VA_ARGS__)
/* names the C library's free, which is nt_free here */
#undef __WINE_DEALLOC
#define __WINE_DEALLOC(...)
#undef ARRAY_SIZE
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]) + __must_be_array(arr))
