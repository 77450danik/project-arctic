/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * ntos: the C library the server's code calls, over the kernel (crt.c).
 *
 * A descriptor here is ntos's own: a slot of a table of struct file that
 * belongs to the executive, not to the process that made the call, as the
 * server's descriptors were the server's. Errors go to errno as in the C
 * library (the executive lock makes it one at a time). Paths without a
 * leading / are taken from the directory nt_fchdir set, as the server's from
 * its working directory. FILE is a buffer over a descriptor; stderr goes to
 * the kernel's log.
 */
#ifndef NTOS_CRT_H
#define NTOS_CRT_H

/* the C library's errno: one per call of the executive (main.c, its call
 * context), so that calls side by side do not overwrite each other's */
#define errno nt_errno
#define nt_errno (*nt_errno_location())
extern int *nt_errno_location(void);

/* what errno and the C library have that the kernel names otherwise */
#ifndef ENOTSUP
#define ENOTSUP EOPNOTSUPP
#endif

typedef long nt_ssize_t;

struct nt_stat
{
    unsigned long long st_dev, st_ino, st_rdev;
    unsigned int st_mode, st_nlink, st_uid, st_gid;
    long long st_size, st_blocks;
    long st_blksize;
    struct timespec64 st_atim, st_mtim, st_ctim;
};
#define st_atime st_atim.tv_sec
#define st_mtime st_mtim.tv_sec
#define st_ctime st_ctim.tv_sec

/* the kernel has timespec64 only, of the same fields */
#define timespec timespec64

/* st_dev and st_rdev are as stat() gives them (new_encode_dev): glibc's
 * major and minor apply */
#define major(dev) ((unsigned int)((((unsigned long long)(dev) >> 32) & 0xfffff000) | (((dev) >> 8) & 0xfff)))
#define minor(dev) ((unsigned int)((((unsigned long long)(dev) >> 12) & 0xffffff00) | ((dev) & 0xff)))

#define statfs nt_statfs
struct nt_statfs
{
    long f_type, f_bsize;
    unsigned long long f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    long f_namelen, f_frsize, f_flags;
};

struct nt_pollfd
{
    int fd;
    short events, revents;
};
#define pollfd nt_pollfd

/* ------------------------------------------------------------- files */

int nt_open(const char *path, int flags, ...);
int nt_close(int fd);
nt_ssize_t nt_read(int fd, void *buf, size_t count);
nt_ssize_t nt_write(int fd, const void *buf, size_t count);
nt_ssize_t nt_pread(int fd, void *buf, size_t count, long long offset);
nt_ssize_t nt_pwrite(int fd, const void *buf, size_t count, long long offset);
int nt_fstat(int fd, struct nt_stat *st);
int nt_stat(const char *path, struct nt_stat *st);
int nt_lstat(const char *path, struct nt_stat *st);
int nt_fstatfs(int fd, struct nt_statfs *st);
int nt_fcntl(int fd, int cmd, ...);
int nt_ioctl(int fd, unsigned long request, ...);
int nt_poll(struct nt_pollfd *fds, unsigned int count, int timeout);
int nt_dup(int fd);
int nt_fchdir(int fd);
int nt_unlink(const char *path);
int nt_rename(const char *from, const char *to);
int nt_mkdir(const char *path, unsigned int mode);
int nt_rmdir(const char *path);
int nt_fsync(int fd);
int nt_ftruncate(int fd, long long length);
int nt_fchmod(int fd, unsigned int mode);
nt_ssize_t nt_readlink(const char *path, char *buf, size_t size);
int nt_pipe(int fds[2]);
/* a terminal's settings: TCGETS and TCSETS on its file */
struct termios;
#define TCSANOW 0
int nt_tcgetattr(int fd, struct termios *tios);
int nt_tcsetattr(int fd, int when, const struct termios *tios);

/* the struct file behind a descriptor, and a descriptor for one (crt.c) */
struct file;
struct file *nt_file_of(int fd);
int nt_fd_for_file(struct file *file);	/* takes the reference */
int nt_close_all(int *on_disk);		/* the executive stops: every file let go */

/* directories */
typedef struct nt_dir ntos_DIR;
struct nt_dirent
{
    unsigned long long d_ino;
    unsigned char d_type;
    char d_name[256];
};
#define dirent nt_dirent
#define DIR ntos_DIR
ntos_DIR *nt_fdopendir(int fd);
struct nt_dirent *nt_readdir(ntos_DIR *dir);
void nt_rewinddir(ntos_DIR *dir);
int nt_closedir(ntos_DIR *dir);

/* extended attributes (where Wine keeps the DOS attributes), links, advice */
nt_ssize_t nt_fgetxattr(int fd, const char *name, void *value, size_t size);
int nt_fsetxattr(int fd, const char *name, const void *value, size_t size, int flags);
int nt_fremovexattr(int fd, const char *name);
int nt_link(const char *from, const char *to);
int nt_posix_fadvise(int fd, long long offset, long long len, int advice);

/* ------------------------------------------------------------- sockets */

/* netlink's groups as user space binds them (uapi rtnetlink.h keeps them
 * from the kernel's own code) */
#define RTMGRP_IPV4_IFADDR 0x10

typedef int socklen_t;
typedef __be32 in_addr_t;

/* glibc's tests of IPv6 addresses */
#define IN6_IS_ADDR_UNSPECIFIED(a) ipv6_addr_any((const struct in6_addr *)(a))
#define IN6_IS_ADDR_V4MAPPED(a) ipv6_addr_v4mapped((const struct in6_addr *)(a))
#define IN6_IS_ADDR_V4COMPAT(a) \
    (!((const __be32 *)(a))[0] && !((const __be32 *)(a))[1] && !((const __be32 *)(a))[2] && \
     ntohl(((const __be32 *)(a))[3]) > 1)

/* the interfaces of the calling process's network namespace, as getifaddrs
 * gives them (crt.c, from the kernel's net devices and their addresses) */
struct ifaddrs
{
    struct ifaddrs *ifa_next;
    char *ifa_name;
    unsigned int ifa_flags;
    struct sockaddr *ifa_addr;
    struct sockaddr *ifa_netmask;
    struct sockaddr *ifa_broadaddr;
    void *ifa_data;
};
int nt_getifaddrs(struct ifaddrs **list);
void nt_freeifaddrs(struct ifaddrs *list);
unsigned int nt_if_nametoindex(const char *name);
#define getifaddrs nt_getifaddrs
#define freeifaddrs nt_freeifaddrs
#define if_nametoindex nt_if_nametoindex

int nt_socket(int domain, int type, int protocol);
int nt_socketpair(int domain, int type, int protocol, int fds[2]);
int nt_bind(int fd, const void *addr, int len);
int nt_listen(int fd, int backlog);
int nt_accept(int fd, void *addr, int *len);
int nt_accept4(int fd, void *addr, int *len, int flags);
int nt_connect(int fd, const void *addr, int len);
int nt_getsockopt(int fd, int level, int name, void *value, int *len);
int nt_setsockopt(int fd, int level, int name, const void *value, int len);
int nt_getsockname(int fd, void *addr, int *len);
int nt_getpeername(int fd, void *addr, int *len);
int nt_shutdown(int fd, int how);
nt_ssize_t nt_recv(int fd, void *buf, size_t len, int flags);
nt_ssize_t nt_send(int fd, const void *buf, size_t len, int flags);

/* memory shared with the clients (mm/mapping.c): a memfd is a shmem file the
 * clients map; the executive sees its pages through vmap, held while mapped.
 * Only MAP_SHARED of such a file is served; a private or executable test
 * mapping says MAP_FAILED, as a directory mounted noexec would. */
#define MAP_FAILED ((void *)-1)
int nt_memfd_create(const char *name, unsigned int flags);
void *nt_mmap(void *addr, size_t length, int prot, int flags, int fd, long long offset);
int nt_munmap(void *addr, size_t length);

/* what the server's main loop did (io/fd.c): a watch on the file behind fd
 * for events (fd -1: none) for poll user index user; the users that woke,
 * one at a time, with what they have (level-triggered: again while it
 * lasts); the kernel's timer for the next timeout, in ns from now (-1: none) */
void nt_poll_watch(int user, int fd, int events);
int nt_poll_ready(int *user, int *events);
void nt_timeout_arm(long long ns);
void nt_timeout_arm_before(long long ns);
void ntos_poll_dispatch(void);	/* io/fd.c, under the executive lock */
void ntos_note_hold( int req, const void *fn, u64 ns, u64 cpu_ns );	/* main.c: a long hold of the lock, and whose */
u64 ntos_cpu_ns( void );	/* main.c: the task's time on a CPU so far */
extern bool ntos_verify_registry;	/* main.c, ntos.verify_registry=1: cm/registry.c checks its snapshots */

/* time: CLOCK_REALTIME in Windows' 100 ns from 1601; UTC minus local time
 * in seconds at a time of the Unix epoch, summer time included (the zone of
 * TZ that arctic-init gave, else /etc/localtime) */
long long nt_realtime_ticks(void);
long nt_timezone_bias(long long unix_time);

/* ntsync (patch 0008, drivers/misc/ntsync.c): an object made on the device
 * dev_fd, a descriptor of ntos for it, as NTSYNC_IOC_CREATE_* gave one; an
 * object's EVENT_SET, EVENT_PULSE, EVENT_RESET, MUTEX_KILL on a __u32 */
int nt_ntsync_create(int dev_fd, unsigned int cmd, const void *args);
int nt_ntsync_ioctl(int fd, unsigned int cmd, void *arg);

/* inotify: an instance of the kernel's own (patch 0008, fs/notify/inotify) */
int nt_inotify_init(void);
int nt_inotify_add_watch(int fd, const char *path, unsigned int mask);
int nt_inotify_rm_watch(int fd, int wd);
/* the path of a descriptor of ntos (what /proc/self/fd/N led to), with extra
 * bytes free at its end; nt_free it */
char *nt_fd_path(int fd, int extra);

/* signals of the server's own process: there is none (SIGIO was dnotify's) */
typedef unsigned long nt_sigset_t;
#define sigset_t nt_sigset_t
static inline int nt_sigemptyset(nt_sigset_t *set) { *set = 0; return 0; }
static inline int nt_sigaddset(nt_sigset_t *set, int sig) { *set |= 1ul << (sig - 1); return 0; }
static inline int nt_sigprocmask(int how, const nt_sigset_t *set, nt_sigset_t *old) { return 0; }

/* ------------------------------------------------------------- tasks and time */

int nt_getpid(void);
unsigned int nt_getuid(void);
long long nt_time(long long *t);
int nt_kill(int pid, int sig);
int nt_tgkill(int tgid, int tid, int sig);
/* a process's memory as /proc/<pid>/status gives it, in bytes */
struct nt_vm_counters {
	unsigned long long virtual_size, peak_virtual_size;
	unsigned long long working_set_size, peak_working_set_size;
	unsigned long long pagefile_usage;
};
int nt_process_vm_counters(int pid, struct nt_vm_counters *c);
/* another process's memory, as process_vm_readv and process_vm_writev:
 * bytes copied, or -1 with errno */
long nt_process_vm_read(int pid, unsigned long long addr, void *buf, size_t size);
long nt_process_vm_write(int pid, unsigned long long addr, const void *buf, size_t size);
/* a thread's debug register n (0-7), as PTRACE_PEEKUSER and PTRACE_POKEUSER
 * on u_debugreg (patch 0008): 0, or -1 with errno */
int nt_debugreg_get(int tid, int n, unsigned long *value);
int nt_debugreg_set(int tid, int n, unsigned long value);
long nt_sysconf(int name);
#define _SC_PAGESIZE 30
#define _SC_PAGE_SIZE _SC_PAGESIZE
#define _SC_NPROCESSORS_ONLN 84
int nt_setpriority(int which, int who, int prio);
#define PRIO_PROCESS 0
/* a task's CPUs, a bit each (Windows' affinity mask): 0, or -1 with errno */
int nt_set_task_affinity(int tid, unsigned long long mask);
unsigned long long nt_get_task_affinity(int tid);

/* ------------------------------------------------------------- memory and strings */

void *nt_malloc(size_t size);
void *nt_calloc(size_t count, size_t size);
void *nt_realloc(void *ptr, size_t size);
void nt_free(void *ptr);
char *nt_strdup(const char *str);
const char *nt_strerror(int err);
unsigned long nt_strtoul(const char *str, char **end, int base);
int nt_atoi(const char *str);
int nt_asprintf(char **out, const char *format, ...) __printf(2, 3);
/* the server's environment: what arctic-init gave nt_boot (NT_REGISTRY_SAVE_PERIOD …) */
char *nt_getenv(const char *name);
/* an absolute path, as realpath: into resolved (PATH_MAX) or allocated */
char *nt_realpath(const char *path, char *resolved);

/* files written beside the executive (the registry's periodic save): data,
 * which the writer frees, goes to a temp file beside name (relative to the
 * directory of dir_fd; a symbolic link is followed to its target), is
 * flushed and renamed over it, in order; 0 if it could not be queued */
/* turns what was queued into what is written, in the saver (outside the lock) */
typedef int (*nt_save_format_fn)(const void *raw, size_t size, void **text, size_t *text_size);
int nt_saver_queue(int dir_fd, const char *name, void *data, size_t size, nt_save_format_fn format);
int nt_saver_busy(void);
void nt_saver_wait(void);

/* ------------------------------------------------------------- stdio */

typedef struct nt_file_stream ntos_FILE;
#define FILE ntos_FILE
extern ntos_FILE *nt_stderr;
#define stderr nt_stderr
#define stdout nt_stderr

ntos_FILE *nt_fopen(const char *path, const char *mode);
ntos_FILE *nt_fdopen(int fd, const char *mode);
int nt_fclose(ntos_FILE *f);
int nt_fprintf(ntos_FILE *f, const char *format, ...) __printf(2, 3);
int nt_vfprintf(ntos_FILE *f, const char *format, va_list args);
int nt_fputs(const char *str, ntos_FILE *f);
int nt_fputc(int c, ntos_FILE *f);
size_t nt_fwrite(const void *ptr, size_t size, size_t count, ntos_FILE *f);
char *nt_fgets(char *buf, int size, ntos_FILE *f);
int nt_fflush(ntos_FILE *f);
int nt_fileno(ntos_FILE *f);
/* a FILE that writes to memory; take gives the bytes (nt_free them) and closes it */
ntos_FILE *nt_open_memstream(void);
int nt_memstream_take(ntos_FILE *f, void **data, size_t *size);
void nt_perror(const char *str);
#define EOF (-1)

#endif
