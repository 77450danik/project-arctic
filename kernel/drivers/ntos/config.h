/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* ntos: what the server's code asks of the system, as Wine's build found it on Linux (include/config.h of the build); IPX is not in the kernel any more */
#ifndef NTOS_CONFIG_H
#define NTOS_CONFIG_H
#define HAVE_ASM_TYPES_H 1
#define HAVE_EPOLL_CREATE 1
#define HAVE_EPOLL_PWAIT2 1
#define HAVE_FSTATFS 1
#define HAVE_IFADDRS_H 1
#define HAVE_LINUX_FILTER_H 1
#define HAVE_LINUX_MAJOR_H 1
#define HAVE_LINUX_MEMFD_H 1
#define HAVE_LINUX_NTSYNC_H 1
#define HAVE_LINUX_RTNETLINK_H 1
#define HAVE_LINUX_TYPES_H 1
#define HAVE_MEMFD_CREATE 1
#define HAVE_NETINET_IN_H 1
#define HAVE_NETINET_TCP_H 1
#define HAVE_NET_IF_H 1
#define HAVE_POSIX_FADVISE 1
#define HAVE_PROCESS_VM_READV 1
#define HAVE_PROCESS_VM_WRITEV 1
#define HAVE_SCHED_H 1
#define HAVE_SCHED_SETAFFINITY 1
#define HAVE_STRUCT_SOCKADDR_IN6_SIN6_SCOPE_ID 1
#define HAVE_SYS_EPOLL_H 1
#define HAVE_SYS_INOTIFY_H 1
#define HAVE_SYS_MOUNT_H 1
#define HAVE_SYS_PARAM_H 1
#define HAVE_SYS_PERSONALITY_H 1
#define HAVE_SYS_PTRACE_H 1
#define HAVE_SYS_QUEUE_H 1
#define HAVE_SYS_RESOURCE_H 1
#define HAVE_SYS_STATFS_H 1
#define HAVE_SYS_STATVFS_H 1
#define HAVE_SYS_SYSCALL_H 1
#define HAVE_SYS_UCONTEXT_H 1
#define HAVE_SYS_UIO_H 1
#define HAVE_SYS_USER_H 1
#define HAVE_SYS_VFS_H 1
#define HAVE_SYS_XATTR_H 1
#define HAVE_UTIME_H 1
#define PACKAGE_STRING "Wine 11.18"
#define PACKAGE_VERSION "11.18"
/* where Arctic installs Wine's data (share/nt) */
#define BINDIR "/usr/bin"
#define DATADIR "/usr/share"
#endif
