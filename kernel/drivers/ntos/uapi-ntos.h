/* SPDX-License-Identifier: LGPL-2.1-or-later WITH Linux-syscall-note */
/*
 * ntos: the system services of NT's executive in the kernel (docs/ntos.md),
 * before win32k's (0x1000), as the service tables of Windows are
 *
 * ntdll speaks the Wine server protocol through them: a request and its data
 * in, the reply and its data out, in the calling thread.
 */
#ifndef _UAPI_LINUX_NTOS_H
#define _UAPI_LINUX_NTOS_H

#include <linux/types.h>

/* asm/unistd_64.h has them too, from the system call table */
#ifndef __NR_nt_hello
#define __NR_nt_hello		3840	/* 0x0f00 */
#define __NR_nt_call		3841
#define __NR_nt_send_fd		3842
#define __NR_nt_receive_fd	3843
#define __NR_nt_boot		3844
#define __NR_nt_admin		3845
#endif

/* nt_hello: a task says what it is to NT */
enum nt_hello_kind {
	NT_HELLO_CONNECT = 1,	/* a process without a parent in NT (wininit): what connecting to wineserver was */
	NT_HELLO_PROCESS,	/* the first thread of a process new_process made: fd is its end of the socket pair */
	NT_HELLO_THREAD,	/* a thread new_thread made: thread_id is the id it got */
};

struct nt_hello {
	__u32 protocol;		/* SERVER_PROTOCOL_VERSION */
	__u32 kind;		/* enum nt_hello_kind */
	__s32 fd;		/* in (NT_HELLO_PROCESS), out (NT_HELLO_CONNECT): the process's socket */
	__u32 thread_id;	/* in (NT_HELLO_THREAD) */
};

/* nt_call: one request of the protocol */
struct nt_call {
	__u64 request;		/* union generic_request (64 bytes) */
	__u64 data;		/* struct iovec[data_count]: the request's data, request_size bytes in all */
	__u32 data_count;
	__u32 status;		/* out: the reply's error */
	__u64 reply;		/* union generic_reply (64 bytes) */
	__u64 reply_data;	/* the reply's data, reply_size bytes at most */
	__u32 reply_data_size;	/* out */
	__u32 pad;
};

/* nt_boot: arctic-init starts the executive, once */
struct nt_boot {
	__u64 config_dir;	/* NTPREFIX: the registry's files, dosdevices */
	__u64 environ;		/* "NAME=value\0…\0": what wineserver had in its environment */
	__u32 environ_size;
	__u32 uid, gid;		/* the user NT runs as */
	__u32 protocol;		/* SERVER_PROTOCOL_VERSION */
};

/* nt_admin: what arctic-init and arctic-lxss ask of it (root only) */
enum nt_admin_code {
	NT_ADMIN_FLUSH_REGISTRY = 1,	/* the registry on the disk now, and wait for it */
	NT_ADMIN_PROCESS_TREE,		/* arg: a Unix pid; buf: its NT-level ancestors, a line each */
	NT_ADMIN_RUNNING,		/* 1 if the executive was started */
	NT_ADMIN_SHUTDOWN,		/* the registry on the disk, then the executive stops and lets
					 * every file go (the machine powers off next): wineserver ending */
	NT_ADMIN_LOCK_STATS,		/* buf: how the executive's lock was used, as text */
};

#endif
