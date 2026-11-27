// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * ntos: the C library the server's code calls, over the kernel (crt.h).
 *
 * Each call does what glibc's does, over the same operation of the kernel:
 * open is filp_open from ntos's working directory, fcntl(F_SETLK) is
 * vfs_lock_file with one owner for all of ntos (as wineserver was one
 * process), a socket is the kernel's socket with KERNEL_SOCKPTR options, a
 * terminal's settings are its tty's termios. Errors go to errno as the C
 * library sets it. Everything here runs under the executive lock (main.c),
 * except the writer of saved files, which keeps to its own structures.
 *
 * Copyright 2026 Arctic contributors
 */

#include <linux/anon_inodes.h>
#include <linux/cred.h>
#include <linux/dcache.h>
#include <linux/fadvise.h>
#include <linux/file.h>
#include <linux/filelock.h>
#include <linux/fs.h>
#include <linux/fs_struct.h>
#include <linux/inetdevice.h>
#include <linux/inotify.h>
#include <linux/pipe_fs_i.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/namei.h>
#include <linux/net.h>
#include <linux/netdevice.h>
#include <linux/pagemap.h>
#include <linux/pid.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/socket.h>
#include <linux/statfs.h>
#include <linux/string.h>
#include <linux/tty.h>
#include <linux/uio.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include <linux/xattr.h>
#include <net/addrconf.h>
#include <net/sock.h>
#include <net/tcp.h>
#include <asm/termbits.h>
#include "crt.h"
#include <linux/ntos.h>
#include "ntos_kernel.h"

/* fs/internal.h: the *at calls of the kernel's own, given a struct filename
 * that stays the caller's */
int filename_unlinkat(int dfd, struct filename *name);
int filename_rmdir(int dfd, struct filename *name);
int filename_mkdirat(int dfd, struct filename *name, umode_t mode);
int filename_renameat2(int olddfd, struct filename *oldname, int newdfd, struct filename *newname, unsigned int flags);
int filename_linkat(int olddfd, struct filename *old, int newdfd, struct filename *new, int flags);

static int set_errno(long err)
{
	nt_errno = (int)-err;
	return -1;
}

/* ------------------------------------------------------------- the table of files */

/*
 * The executive's own table of descriptors, as a process's: read without a
 * lock (RCU: a call that let the executive's lock go for its I/O looks its
 * files up while others open and close theirs), changed under files_mutex.
 * A table that grows is copied; the old one goes once no reader can see it.
 */
struct file_table
{
	int size;
	struct file __rcu *slot[];
};

static struct file_table __rcu *files;
static DEFINE_MUTEX(files_mutex);
static int files_free_hint;	/* no free slot below it */

/* a free slot, made if there is none; under files_mutex */
static int alloc_slot(void)
{
	struct file_table *table = rcu_dereference_protected(files, lockdep_is_held(&files_mutex)), *grown;
	int i, size = table ? table->size : 0;

	for (i = files_free_hint; i < size; i++)
		if (!rcu_access_pointer(table->slot[i])) {
			files_free_hint = i + 1;
			return i;
		}
	size = size ? size * 2 : 256;
	if (!(grown = kvzalloc(struct_size(grown, slot, size), GFP_KERNEL)))
		return -1;
	grown->size = size;
	if (table) {
		for (i = 0; i < table->size; i++)
			RCU_INIT_POINTER(grown->slot[i], rcu_access_pointer(table->slot[i]));
	}
	i = table ? table->size : 0;
	rcu_assign_pointer(files, grown);
	if (table)
		kvfree_rcu_mightsleep(table);
	files_free_hint = i + 1;
	return i;
}

/* the slot's file, taken out of the table; under files_mutex */
static struct file *take_slot(int fd)
{
	struct file_table *table = rcu_dereference_protected(files, lockdep_is_held(&files_mutex));
	struct file *file;

	if (!table || fd < 0 || fd >= table->size)
		return NULL;
	file = rcu_dereference_protected(table->slot[fd], lockdep_is_held(&files_mutex));
	if (file) {
		RCU_INIT_POINTER(table->slot[fd], NULL);
		if (fd < files_free_hint)
			files_free_hint = fd;
	}
	return file;
}

/* the executive stops (nt_admin NT_ADMIN_SHUTDOWN): every file it holds is
 * let go, as wineserver's were when its process ended, so that C: can go
 * read-only; *on_disk: how many of them were on a disk's file system */
int nt_close_all(int *on_disk)
{
	struct file_table *table;
	int i, closed = 0;

	*on_disk = 0;
	mutex_lock(&files_mutex);
	table = rcu_dereference_protected(files, lockdep_is_held(&files_mutex));
	for (i = 0; table && i < table->size; i++) {
		struct file *file = take_slot(i);

		if (!file)
			continue;
		if (file->f_path.mnt && file->f_path.mnt->mnt_sb->s_bdev)
			(*on_disk)++;
		fput(file);
		closed++;
	}
	mutex_unlock(&files_mutex);
	return closed;
}

int nt_fd_for_file(struct file *file)
{
	struct file_table *table;
	int fd;

	mutex_lock(&files_mutex);
	if ((fd = alloc_slot()) >= 0) {
		table = rcu_dereference_protected(files, lockdep_is_held(&files_mutex));
		rcu_assign_pointer(table->slot[fd], file);
	}
	mutex_unlock(&files_mutex);
	if (fd < 0) {
		fput(file);
		return set_errno(-EMFILE);
	}
	return fd;
}

/* the file of a descriptor, borrowed: the caller keeps the descriptor open
 * (it holds the object that owns it, or the executive's lock) */
struct file *nt_file_of(int fd)
{
	struct file_table *table;
	struct file *file = NULL;

	if (fd < 0)
		return NULL;
	rcu_read_lock();
	table = rcu_dereference(files);
	if (table && fd < table->size)
		file = rcu_dereference(table->slot[fd]);
	rcu_read_unlock();
	return file;
}

static struct file *file_or_ebadf(int fd)
{
	struct file *file = nt_file_of(fd);

	if (!file)
		nt_errno = EBADF;
	return file;
}

int nt_close(int fd)
{
	struct file *file = file_or_ebadf(fd);

	if (!file)
		return -1;
	mutex_lock(&files_mutex);
	file = take_slot(fd);
	mutex_unlock(&files_mutex);
	if (!file)
		return set_errno(-EBADF);
	nt_poll_forget(fd);
	fput(file);
	return 0;
}

int nt_dup(int fd)
{
	struct file *file = file_or_ebadf(fd);

	if (!file)
		return -1;
	return nt_fd_for_file(get_file(file));
}

/* ------------------------------------------------------------- paths */

/* the working directory: the server's was its config directory (NTPREFIX) */
static struct path cwd;

void nt_crt_init(const struct path *dir)
{
	path_put(&cwd);
	cwd = *dir;
	path_get(&cwd);
}

int nt_fchdir(int fd)
{
	struct file *file = file_or_ebadf(fd);

	if (!file)
		return -1;
	if (!S_ISDIR(file_inode(file)->i_mode))
		return set_errno(-ENOTDIR);
	path_put(&cwd);
	cwd = file->f_path;
	path_get(&cwd);
	return 0;
}

/* an absolute path for one relative to the working directory, kmalloc'ed */
static char *absolute(const char *path)
{
	char *buf, *dir, *ret;

	if (path[0] == '/' || !cwd.dentry)
		return kstrdup(path, GFP_KERNEL);
	if (!(buf = kmalloc(PATH_MAX, GFP_KERNEL)))
		return NULL;
	dir = d_path(&cwd, buf, PATH_MAX);
	if (IS_ERR(dir))
		ret = NULL;
	else if (!strcmp(dir, "/"))
		ret = kasprintf(GFP_KERNEL, "/%s", path);
	else
		ret = kasprintf(GFP_KERNEL, "%s/%s", dir, path);
	kfree(buf);
	return ret;
}

static struct filename *name_of(const char *path)
{
	char *abs = absolute(path);
	struct filename *name;

	if (!abs)
		return ERR_PTR(-ENOMEM);
	name = getname_kernel(abs);
	kfree(abs);
	return name;
}

static int lookup(const char *path, unsigned int flags, struct path *out)
{
	char *abs = absolute(path);
	int ret;

	if (!abs)
		return -ENOMEM;
	ret = kern_path(abs, flags, out);
	kfree(abs);
	return ret;
}

int nt_open(const char *path, int flags, ...)
{
	umode_t mode = 0;
	struct file *file;
	char *abs;
	va_list args;

	if (flags & O_CREAT) {
		va_start(args, flags);
		mode = va_arg(args, int);
		va_end(args);
	}
	if (!(abs = absolute(path)))
		return set_errno(-ENOMEM);
	file = filp_open(abs, flags | O_LARGEFILE, mode);
	kfree(abs);
	if (IS_ERR(file))
		return set_errno(PTR_ERR(file));
	return nt_fd_for_file(file);
}

char *nt_realpath(const char *path, char *resolved)
{
	struct path p;
	char *buf, *s;
	int ret;

	if ((ret = lookup(path, LOOKUP_FOLLOW, &p))) {
		set_errno(ret);
		return NULL;
	}
	buf = resolved ? resolved : kmalloc(PATH_MAX, GFP_KERNEL);
	if (!buf) {
		path_put(&p);
		set_errno(-ENOMEM);
		return NULL;
	}
	s = d_path(&p, buf, PATH_MAX);
	path_put(&p);
	if (IS_ERR(s)) {
		if (!resolved)
			kfree(buf);
		set_errno(PTR_ERR(s));
		return NULL;
	}
	memmove(buf, s, strlen(s) + 1);
	return buf;
}

/* one name, or two, to a filename_* call; the names are put back after it */
static int at1(int (*fn)(int, struct filename *), const char *path)
{
	struct filename *name = name_of(path);
	int ret;

	if (IS_ERR(name))
		return set_errno(PTR_ERR(name));
	ret = fn(AT_FDCWD, name);
	putname(name);
	return ret ? set_errno(ret) : 0;
}

int nt_unlink(const char *path)
{
	return at1(filename_unlinkat, path);
}

int nt_rmdir(const char *path)
{
	return at1(filename_rmdir, path);
}

int nt_mkdir(const char *path, unsigned int mode)
{
	struct filename *name = name_of(path);
	int ret;

	if (IS_ERR(name))
		return set_errno(PTR_ERR(name));
	ret = filename_mkdirat(AT_FDCWD, name, mode);
	putname(name);
	return ret ? set_errno(ret) : 0;
}

static int at2(const char *from, const char *to, bool link)
{
	struct filename *a = name_of(from), *b;
	int ret;

	if (IS_ERR(a))
		return set_errno(PTR_ERR(a));
	b = name_of(to);
	if (IS_ERR(b)) {
		putname(a);
		return set_errno(PTR_ERR(b));
	}
	ret = link ? filename_linkat(AT_FDCWD, a, AT_FDCWD, b, 0) : filename_renameat2(AT_FDCWD, a, AT_FDCWD, b, 0);
	putname(a);
	putname(b);
	return ret ? set_errno(ret) : 0;
}

int nt_rename(const char *from, const char *to)
{
	return at2(from, to, false);
}

int nt_link(const char *from, const char *to)
{
	return at2(from, to, true);
}

nt_ssize_t nt_readlink(const char *path, char *buf, size_t size)
{
	DEFINE_DELAYED_CALL(done);
	const char *target;
	struct path p;
	size_t len;
	int ret;

	if ((ret = lookup(path, 0, &p)))
		return set_errno(ret);
	target = vfs_get_link(p.dentry, &done);
	if (IS_ERR(target)) {
		path_put(&p);
		return set_errno(PTR_ERR(target));
	}
	len = min(strlen(target), size);
	memcpy(buf, target, len);
	do_delayed_call(&done);
	path_put(&p);
	return len;
}

/* ------------------------------------------------------------- reading and writing */

/* as read(2) and write(2) answer for a file not open for it: kernel_read and
 * kernel_write take that for a caller's bug and warn */
static struct file *file_for(int fd, fmode_t mode, fmode_t can)
{
	struct file *file = file_or_ebadf(fd);

	if (!file)
		return NULL;
	if (!(file->f_mode & mode)) {
		nt_errno = EBADF;
		return NULL;
	}
	if (!(file->f_mode & can)) {
		nt_errno = EINVAL;
		return NULL;
	}
	return file;
}

nt_ssize_t nt_read(int fd, void *buf, size_t count)
{
	struct file *file = file_for(fd, FMODE_READ, FMODE_CAN_READ);
	loff_t pos;
	ssize_t ret;

	if (!file)
		return -1;
	/* inotify reads into user memory only (no read_iter): change.c's
	 * instance is read with the kernel's own reader (patch 0008) */
	if (!file->f_op->read_iter) {
		ret = inotify_kernel_read(file, buf, count);
		if (ret != -ENOTTY)
			return ret < 0 ? set_errno(ret) : ret;
	}
	pos = file->f_pos;
	ret = kernel_read(file, buf, count, (file->f_mode & FMODE_STREAM) ? NULL : &pos);
	if (ret < 0)
		return set_errno(ret);
	if (!(file->f_mode & FMODE_STREAM))
		file->f_pos = pos;
	return ret;
}

nt_ssize_t nt_write(int fd, const void *buf, size_t count)
{
	struct file *file = file_for(fd, FMODE_WRITE, FMODE_CAN_WRITE);
	loff_t pos;
	ssize_t ret;

	if (!file)
		return -1;
	pos = file->f_pos;
	ret = kernel_write(file, buf, count, (file->f_mode & FMODE_STREAM) ? NULL : &pos);
	if (ret < 0)
		return set_errno(ret);
	if (!(file->f_mode & FMODE_STREAM))
		file->f_pos = pos;
	return ret;
}

nt_ssize_t nt_pread(int fd, void *buf, size_t count, long long offset)
{
	struct file *file = file_for(fd, FMODE_READ, FMODE_CAN_READ);
	loff_t pos = offset;
	ssize_t ret;

	if (!file)
		return -1;
	ret = kernel_read(file, buf, count, &pos);
	return ret < 0 ? set_errno(ret) : ret;
}

nt_ssize_t nt_pwrite(int fd, const void *buf, size_t count, long long offset)
{
	struct file *file = file_for(fd, FMODE_WRITE, FMODE_CAN_WRITE);
	loff_t pos = offset;
	ssize_t ret;

	if (!file)
		return -1;
	ret = kernel_write(file, buf, count, &pos);
	return ret < 0 ? set_errno(ret) : ret;
}

int nt_fsync(int fd)
{
	struct file *file = file_or_ebadf(fd);
	int ret;

	if (!file)
		return -1;
	ret = vfs_fsync(file, 0);
	return ret ? set_errno(ret) : 0;
}

int nt_ftruncate(int fd, long long length)
{
	struct file *file = file_or_ebadf(fd);
	int ret;

	if (!file)
		return -1;
	if (length < 0)
		return set_errno(-EINVAL);
	ret = do_truncate(file_mnt_idmap(file), file->f_path.dentry, length, ATTR_MTIME | ATTR_CTIME, file);
	return ret ? set_errno(ret) : 0;
}

int nt_fchmod(int fd, unsigned int mode)
{
	struct file *file = file_or_ebadf(fd);
	struct inode *inode;
	struct iattr attr;
	int ret;

	if (!file)
		return -1;
	inode = file_inode(file);
	inode_lock(inode);
	attr.ia_mode = (mode & S_IALLUGO) | (inode->i_mode & ~S_IALLUGO);
	attr.ia_valid = ATTR_MODE | ATTR_CTIME;
	ret = notify_change(file_mnt_idmap(file), file->f_path.dentry, &attr, NULL);
	inode_unlock(inode);
	return ret ? set_errno(ret) : 0;
}

int nt_posix_fadvise(int fd, long long offset, long long len, int advice)
{
	struct file *file = nt_file_of(fd);

	if (!file)
		return EBADF;	/* posix_fadvise returns the error */
	return -vfs_fadvise(file, offset, len, advice);
}

/* ------------------------------------------------------------- attributes */

static void fill_stat(const struct kstat *k, struct nt_stat *st)
{
	memset(st, 0, sizeof(*st));
	st->st_dev = new_encode_dev(k->dev);
	st->st_ino = k->ino;
	st->st_rdev = new_encode_dev(k->rdev);
	st->st_mode = k->mode;
	st->st_nlink = k->nlink;
	st->st_uid = from_kuid_munged(current_user_ns(), k->uid);
	st->st_gid = from_kgid_munged(current_user_ns(), k->gid);
	st->st_size = k->size;
	st->st_blocks = k->blocks;
	st->st_blksize = k->blksize;
	st->st_atim = k->atime;
	st->st_mtim = k->mtime;
	st->st_ctim = k->ctime;
}

int nt_fstat(int fd, struct nt_stat *st)
{
	struct file *file = file_or_ebadf(fd);
	struct kstat k;
	int ret;

	if (!file)
		return -1;
	if ((ret = vfs_getattr(&file->f_path, &k, STATX_BASIC_STATS, AT_STATX_SYNC_AS_STAT)))
		return set_errno(ret);
	fill_stat(&k, st);
	return 0;
}

static int stat_path(const char *path, struct nt_stat *st, unsigned int flags)
{
	struct kstat k;
	struct path p;
	int ret;

	if ((ret = lookup(path, flags, &p)))
		return set_errno(ret);
	ret = vfs_getattr(&p, &k, STATX_BASIC_STATS, AT_STATX_SYNC_AS_STAT);
	path_put(&p);
	if (ret)
		return set_errno(ret);
	fill_stat(&k, st);
	return 0;
}

int nt_stat(const char *path, struct nt_stat *st)
{
	return stat_path(path, st, LOOKUP_FOLLOW);
}

int nt_lstat(const char *path, struct nt_stat *st)
{
	return stat_path(path, st, 0);
}

int nt_fstatfs(int fd, struct nt_statfs *st)
{
	struct file *file = file_or_ebadf(fd);
	struct kstatfs k;
	int ret;

	if (!file)
		return -1;
	if ((ret = vfs_statfs(&file->f_path, &k)))
		return set_errno(ret);
	memset(st, 0, sizeof(*st));
	st->f_type = k.f_type;
	st->f_bsize = k.f_bsize;
	st->f_blocks = k.f_blocks;
	st->f_bfree = k.f_bfree;
	st->f_bavail = k.f_bavail;
	st->f_files = k.f_files;
	st->f_ffree = k.f_ffree;
	st->f_namelen = k.f_namelen;
	st->f_frsize = k.f_frsize;
	st->f_flags = k.f_flags;
	return 0;
}

nt_ssize_t nt_fgetxattr(int fd, const char *name, void *value, size_t size)
{
	struct file *file = file_or_ebadf(fd);
	ssize_t ret;

	if (!file)
		return -1;
	ret = vfs_getxattr(file_mnt_idmap(file), file->f_path.dentry, name, value, size);
	return ret < 0 ? set_errno(ret) : ret;
}

int nt_fsetxattr(int fd, const char *name, const void *value, size_t size, int flags)
{
	struct file *file = file_or_ebadf(fd);
	int ret;

	if (!file)
		return -1;
	ret = vfs_setxattr(file_mnt_idmap(file), file->f_path.dentry, name, value, size, flags);
	return ret ? set_errno(ret) : 0;
}

int nt_fremovexattr(int fd, const char *name)
{
	struct file *file = file_or_ebadf(fd);
	int ret;

	if (!file)
		return -1;
	ret = vfs_removexattr(file_mnt_idmap(file), file->f_path.dentry, name);
	return ret ? set_errno(ret) : 0;
}

/* ------------------------------------------------------------- fcntl, locks, poll, terminals */

/* the owner of every lock ntos takes, as the server's process was */
static int lock_owner;

static int file_lock(struct file *file, int cmd, struct flock *fl)
{
	struct file_lock lock;
	int ret;

	locks_init_lock(&lock);
	lock.c.flc_flags = FL_POSIX;
	lock.c.flc_owner = (fl_owner_t)&lock_owner;
	lock.c.flc_pid = current->tgid;
	lock.c.flc_file = file;
	lock.c.flc_type = fl->l_type;
	lock.fl_start = fl->l_start;
	lock.fl_end = fl->l_len ? fl->l_start + fl->l_len - 1 : OFFSET_MAX;
	if (cmd == F_GETLK) {
		vfs_test_lock(file, &lock);
		fl->l_type = lock.c.flc_type;
		if (lock.c.flc_type != F_UNLCK) {
			fl->l_start = lock.fl_start;
			fl->l_len = lock.fl_end == OFFSET_MAX ? 0 : lock.fl_end - lock.fl_start + 1;
			fl->l_pid = lock.c.flc_pid;
		}
		ret = 0;
	} else {
		ret = vfs_lock_file(file, F_SETLK, &lock, NULL);
	}
	locks_release_private(&lock);
	return ret;
}

int nt_fcntl(int fd, int cmd, ...)
{
	struct file *file = file_or_ebadf(fd);
	unsigned long arg;
	va_list args;
	int ret;

	if (!file)
		return -1;
	va_start(args, cmd);
	arg = va_arg(args, unsigned long);
	va_end(args);
	switch (cmd) {
	case F_GETFD:
	case F_SETFD:
		return 0;	/* not inherited by anything: the table is ntos's */
	case F_GETFL:
		return file->f_flags;
	case F_SETFL:
		spin_lock(&file->f_lock);
		file->f_flags = (file->f_flags & ~(O_NONBLOCK | O_APPEND)) | (arg & (O_NONBLOCK | O_APPEND));
		spin_unlock(&file->f_lock);
		return 0;
	case F_SETLK:
	case F_GETLK:
		if ((ret = file_lock(file, cmd, (struct flock *)arg)))
			return set_errno(ret);
		return 0;
	default:
		/* F_NOTIFY and F_SETSIG were dnotify's: inotify serves change notification */
		return set_errno(-EINVAL);
	}
}

int nt_ioctl(int fd, unsigned long request, ...)
{
	/* every ioctl of the server takes a pointer of user space; those it
	 * makes are served by their own functions (ntsync: inproc_sync.c) */
	return set_errno(-ENOTTY);
}

int nt_poll(struct nt_pollfd *fds, unsigned int count, int timeout)
{
	unsigned int i, ready = 0;

	/* the server asks what is ready now (sock_check_pollhup gives 1 ms, for
	 * Solaris): a socket's state is known at once here, nothing waits */
	WARN_ON_ONCE(timeout > 1);
	for (i = 0; i < count; i++) {
		struct file *file = nt_file_of(fds[i].fd);

		fds[i].revents = 0;
		if (fds[i].fd < 0)
			continue;
		if (!file)
			fds[i].revents = POLLNVAL;
		else
			fds[i].revents = vfs_poll(file, NULL) & (fds[i].events | POLLERR | POLLHUP);
		if (fds[i].revents)
			ready++;
	}
	return ready;
}

int nt_tcgetattr(int fd, struct termios *tios)
{
	struct file *file = file_or_ebadf(fd);
	struct tty_struct *tty;

	if (!file)
		return -1;
	if (!(tty = tty_kref_get_file(file)))
		return set_errno(-ENOTTY);
	down_read(&tty->termios_rwsem);
	tios->c_iflag = tty->termios.c_iflag;
	tios->c_oflag = tty->termios.c_oflag;
	tios->c_cflag = tty->termios.c_cflag;
	tios->c_lflag = tty->termios.c_lflag;
	tios->c_line = tty->termios.c_line;
	memcpy(tios->c_cc, tty->termios.c_cc, sizeof(tios->c_cc));
	up_read(&tty->termios_rwsem);
	tty_kref_put(tty);
	return 0;
}

int nt_tcsetattr(int fd, int when, const struct termios *tios)
{
	struct file *file = file_or_ebadf(fd);
	struct tty_struct *tty;
	struct ktermios k;
	int ret;

	if (!file)
		return -1;
	if (!(tty = tty_kref_get_file(file)))
		return set_errno(-ENOTTY);
	k = tty->termios;
	k.c_iflag = tios->c_iflag;
	k.c_oflag = tios->c_oflag;
	k.c_cflag = tios->c_cflag;
	k.c_lflag = tios->c_lflag;
	k.c_line = tios->c_line;
	memcpy(k.c_cc, tios->c_cc, sizeof(tios->c_cc));
	ret = tty_set_termios(tty, &k);
	tty_kref_put(tty);
	return ret ? set_errno(ret) : 0;
}

int nt_pipe(int fds[2])
{
	struct file *f[2];
	int ret;

	if ((ret = create_pipe_files(f, 0)))
		return set_errno(ret);
	if ((fds[0] = nt_fd_for_file(f[0])) < 0) {
		fput(f[1]);
		return -1;
	}
	if ((fds[1] = nt_fd_for_file(f[1])) < 0) {
		nt_close(fds[0]);
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------- directories */

struct nt_dir
{
	struct dir_context ctx;
	struct file *file;
	struct nt_dirent *entries;
	unsigned int count, size, next;
};

static bool dir_actor(struct dir_context *ctx, const char *name, int len, loff_t pos, u64 ino, unsigned int type)
{
	struct nt_dir *dir = container_of(ctx, struct nt_dir, ctx);
	struct nt_dirent *grown;

	if (dir->count == dir->size) {
		unsigned int size = dir->size ? dir->size * 2 : 64;

		if (!(grown = kvrealloc(dir->entries, size * sizeof(*grown), GFP_KERNEL)))
			return false;
		dir->entries = grown;
		dir->size = size;
	}
	dir->entries[dir->count].d_ino = ino;
	dir->entries[dir->count].d_type = type;
	len = min(len, (int)sizeof(dir->entries[0].d_name) - 1);
	memcpy(dir->entries[dir->count].d_name, name, len);
	dir->entries[dir->count].d_name[len] = 0;
	dir->count++;
	return true;
}

/* the whole directory is read when opened and rewound, as one getdents would */
static int dir_fill(struct nt_dir *dir)
{
	int ret;

	dir->count = dir->next = 0;
	dir->file->f_pos = 0;
	dir->ctx.pos = 0;
	ret = iterate_dir(dir->file, &dir->ctx);
	return ret;
}

ntos_DIR *nt_fdopendir(int fd)
{
	struct file *file = file_or_ebadf(fd);
	struct nt_dir *dir;

	if (!file)
		return NULL;
	if (!(dir = kzalloc(sizeof(*dir), GFP_KERNEL))) {
		nt_errno = ENOMEM;
		return NULL;
	}
	*(filldir_t *)&dir->ctx.actor = dir_actor;
	dir->file = get_file(file);
	if (dir_fill(dir)) {
		fput(dir->file);
		kvfree(dir->entries);
		kfree(dir);
		nt_errno = ENOTDIR;
		return NULL;
	}
	/* fdopendir takes the descriptor over: closedir closes it */
	nt_close(fd);
	return dir;
}

struct nt_dirent *nt_readdir(ntos_DIR *dir)
{
	if (dir->next >= dir->count)
		return NULL;
	return &dir->entries[dir->next++];
}

void nt_rewinddir(ntos_DIR *dir)
{
	dir_fill(dir);
}

int nt_closedir(ntos_DIR *dir)
{
	fput(dir->file);
	kvfree(dir->entries);
	kfree(dir);
	return 0;
}

/* ------------------------------------------------------------- sockets */

static struct socket *socket_of(int fd)
{
	struct file *file = file_or_ebadf(fd);
	struct socket *sock;

	if (!file)
		return NULL;
	if (!(sock = sock_from_file(file)))
		nt_errno = ENOTSOCK;
	return sock;
}

static int socket_fd(struct socket *sock, int flags)
{
	struct file *file = sock_alloc_file(sock, flags & (O_NONBLOCK | O_CLOEXEC), NULL);

	if (IS_ERR(file))
		return set_errno(PTR_ERR(file));	/* sock_alloc_file released the socket */
	return nt_fd_for_file(file);
}

int nt_socket(int domain, int type, int protocol)
{
	struct socket *sock;
	int ret, flags = type & (SOCK_NONBLOCK | SOCK_CLOEXEC);

	/* as the server's: a socket of the calling process's network namespace
	 * and credentials, not one of the kernel's own */
	if ((ret = sock_create(domain, type & SOCK_TYPE_MASK, protocol, &sock)))
		return set_errno(ret);
	return socket_fd(sock, flags & SOCK_NONBLOCK ? O_NONBLOCK : 0);
}

int nt_socketpair(int domain, int type, int protocol, int fds[2])
{
	struct socket *a, *b;
	int ret, flags = (type & SOCK_NONBLOCK) ? O_NONBLOCK : 0;

	if ((ret = sock_create(domain, type & SOCK_TYPE_MASK, protocol, &a)))
		return set_errno(ret);
	if ((ret = sock_create(domain, type & SOCK_TYPE_MASK, protocol, &b))) {
		sock_release(a);
		return set_errno(ret);
	}
	if ((ret = a->ops->socketpair(a, b))) {
		sock_release(a);
		sock_release(b);
		return set_errno(ret);
	}
	if ((fds[0] = socket_fd(a, flags)) < 0) {
		sock_release(b);
		return -1;
	}
	if ((fds[1] = socket_fd(b, flags)) < 0) {
		nt_close(fds[0]);
		return -1;
	}
	return 0;
}

int nt_bind(int fd, const void *addr, int len)
{
	struct socket *sock = socket_of(fd);
	int ret;

	if (!sock)
		return -1;
	ret = kernel_bind(sock, (struct sockaddr_unsized *)addr, len);
	return ret ? set_errno(ret) : 0;
}

int nt_listen(int fd, int backlog)
{
	struct socket *sock = socket_of(fd);
	int ret;

	if (!sock)
		return -1;
	ret = kernel_listen(sock, backlog);
	return ret ? set_errno(ret) : 0;
}

int nt_accept4(int fd, void *addr, int *len, int flags)
{
	struct socket *sock = socket_of(fd), *newsock;
	struct file *file;
	int ret, newfd;

	if (!sock)
		return -1;
	file = nt_file_of(fd);
	if ((ret = kernel_accept(sock, &newsock, (file->f_flags & O_NONBLOCK) | (flags & SOCK_NONBLOCK ? O_NONBLOCK : 0))))
		return set_errno(ret);
	if ((newfd = socket_fd(newsock, flags & SOCK_NONBLOCK ? O_NONBLOCK : 0)) < 0)
		return -1;
	if (addr && len) {
		struct sockaddr_storage peer;

		ret = kernel_getpeername(newsock, (struct sockaddr *)&peer);
		if (ret >= 0) {
			memcpy(addr, &peer, min(*len, ret));
			*len = ret;
		}
	}
	return newfd;
}

int nt_accept(int fd, void *addr, int *len)
{
	return nt_accept4(fd, addr, len, 0);
}

int nt_connect(int fd, const void *addr, int len)
{
	struct socket *sock = socket_of(fd);
	int ret;

	if (!sock)
		return -1;
	ret = kernel_connect(sock, (struct sockaddr_unsized *)addr, len, nt_file_of(fd)->f_flags & O_NONBLOCK);
	return ret ? set_errno(ret) : 0;
}

int nt_getsockopt(int fd, int level, int name, void *value, int *len)
{
	struct socket *sock = socket_of(fd);
	int ret;

	if (!sock)
		return -1;
	/* A protocol's own options (TCP, IP, IPv6) are read into user memory
	 * only, unless it has getsockopt_iter: the two the server reads are
	 * taken from the socket here. SOL_SOCKET's take a kernel buffer. */
	if (level == IPPROTO_IPV6 && name == IPV6_V6ONLY && sock->sk) {
		if (*len < (int)sizeof(int))
			return set_errno(-EINVAL);
		*(int *)value = sock->sk->sk_ipv6only;
		*len = sizeof(int);
		return 0;
	}
	if (level == IPPROTO_TCP && name == TCP_INFO && sock->sk) {
		struct tcp_info info;

		if (sock->sk->sk_protocol != IPPROTO_TCP)
			return set_errno(-EOPNOTSUPP);
		tcp_get_info(sock->sk, &info);
		*len = min_t(int, *len, sizeof(info));
		memcpy(value, &info, *len);
		return 0;
	}
	if (level != SOL_SOCKET && !READ_ONCE(sock->ops)->getsockopt_iter)
		return set_errno(-EOPNOTSUPP);
	ret = do_sock_getsockopt(sock, false, level, name, KERNEL_SOCKPTR(value), KERNEL_SOCKPTR(len));
	return ret ? set_errno(ret) : 0;
}

int nt_setsockopt(int fd, int level, int name, const void *value, int len)
{
	struct socket *sock = socket_of(fd);
	int ret;

	if (!sock)
		return -1;
	ret = do_sock_setsockopt(sock, false, level, name, KERNEL_SOCKPTR((void *)value), len);
	return ret ? set_errno(ret) : 0;
}

int nt_getsockname(int fd, void *addr, int *len)
{
	struct socket *sock = socket_of(fd);
	struct sockaddr_storage local;
	int ret;

	if (!sock)
		return -1;
	if ((ret = kernel_getsockname(sock, (struct sockaddr *)&local)) < 0)
		return set_errno(ret);
	memcpy(addr, &local, min(*len, ret));
	*len = ret;
	return 0;
}

int nt_getpeername(int fd, void *addr, int *len)
{
	struct socket *sock = socket_of(fd);
	struct sockaddr_storage peer;
	int ret;

	if (!sock)
		return -1;
	if ((ret = kernel_getpeername(sock, (struct sockaddr *)&peer)) < 0)
		return set_errno(ret);
	memcpy(addr, &peer, min(*len, ret));
	*len = ret;
	return 0;
}

int nt_shutdown(int fd, int how)
{
	struct socket *sock = socket_of(fd);
	int ret;

	if (!sock)
		return -1;
	ret = kernel_sock_shutdown(sock, how);
	return ret ? set_errno(ret) : 0;
}

nt_ssize_t nt_recv(int fd, void *buf, size_t len, int flags)
{
	struct socket *sock = socket_of(fd);
	struct msghdr msg = { .msg_flags = flags };
	struct kvec vec = { buf, len };
	int ret;

	if (!sock)
		return -1;
	if (nt_file_of(fd)->f_flags & O_NONBLOCK)
		flags |= MSG_DONTWAIT;
	ret = kernel_recvmsg(sock, &msg, &vec, 1, len, flags);
	return ret < 0 ? set_errno(ret) : ret;
}

nt_ssize_t nt_send(int fd, const void *buf, size_t len, int flags)
{
	struct socket *sock = socket_of(fd);
	struct msghdr msg = { .msg_flags = flags | MSG_NOSIGNAL };
	struct kvec vec = { (void *)buf, len };
	int ret;

	if (!sock)
		return -1;
	if (nt_file_of(fd)->f_flags & O_NONBLOCK)
		msg.msg_flags |= MSG_DONTWAIT;
	ret = kernel_sendmsg(sock, &msg, &vec, 1, len);
	return ret < 0 ? set_errno(ret) : ret;
}

/* ------------------------------------------------------------- interfaces */

struct ifaddrs_entry
{
	struct ifaddrs ifa;
	char name[IFNAMSIZ];
	struct sockaddr_storage addr, mask, broad;
};

static struct ifaddrs_entry *ifaddrs_add(struct ifaddrs **head, struct ifaddrs ***tail, struct net_device *dev)
{
	struct ifaddrs_entry *e = kzalloc(sizeof(*e), GFP_ATOMIC);

	if (!e)
		return NULL;
	strscpy(e->name, dev->name, sizeof(e->name));
	e->ifa.ifa_name = e->name;
	e->ifa.ifa_flags = netif_get_flags(dev);
	**tail = &e->ifa;
	*tail = &e->ifa.ifa_next;
	return e;
}

/* as glibc's: an entry per IPv4 and per IPv6 address of each interface */
int nt_getifaddrs(struct ifaddrs **list)
{
	struct net *net = current->nsproxy ? current->nsproxy->net_ns : &init_net;
	struct ifaddrs *head = NULL, **tail = &head;
	struct net_device *dev;

	rcu_read_lock();
	for_each_netdev_rcu(net, dev) {
		struct in_device *in4 = __in_dev_get_rcu(dev);
		struct inet6_dev *in6 = __in6_dev_get(dev);
		const struct in_ifaddr *ifa;
		struct inet6_ifaddr *ifa6;

		if (in4) {
			in_dev_for_each_ifa_rcu(ifa, in4) {
				struct ifaddrs_entry *e = ifaddrs_add(&head, &tail, dev);
				struct sockaddr_in *a, *m, *b;

				if (!e)
					break;
				a = (struct sockaddr_in *)&e->addr;
				m = (struct sockaddr_in *)&e->mask;
				b = (struct sockaddr_in *)&e->broad;
				a->sin_family = m->sin_family = b->sin_family = AF_INET;
				a->sin_addr.s_addr = ifa->ifa_local;
				m->sin_addr.s_addr = ifa->ifa_mask;
				b->sin_addr.s_addr = ifa->ifa_broadcast;
				e->ifa.ifa_addr = (struct sockaddr *)a;
				e->ifa.ifa_netmask = (struct sockaddr *)m;
				e->ifa.ifa_broadaddr = (struct sockaddr *)b;
			}
		}
		if (in6) {
			read_lock_bh(&in6->lock);
			list_for_each_entry(ifa6, &in6->addr_list, if_list) {
				struct ifaddrs_entry *e = ifaddrs_add(&head, &tail, dev);
				struct sockaddr_in6 *a, *m;

				if (!e)
					break;
				a = (struct sockaddr_in6 *)&e->addr;
				m = (struct sockaddr_in6 *)&e->mask;
				a->sin6_family = m->sin6_family = AF_INET6;
				a->sin6_addr = ifa6->addr;
				if (ipv6_addr_type(&ifa6->addr) & IPV6_ADDR_LINKLOCAL)
					a->sin6_scope_id = dev->ifindex;
				ipv6_addr_prefix(&m->sin6_addr, &(struct in6_addr){ .s6_addr32 = { ~0u, ~0u, ~0u, ~0u } },
						 ifa6->prefix_len);
				e->ifa.ifa_addr = (struct sockaddr *)a;
				e->ifa.ifa_netmask = (struct sockaddr *)m;
			}
			read_unlock_bh(&in6->lock);
		}
	}
	rcu_read_unlock();
	*list = head;
	return 0;
}

void nt_freeifaddrs(struct ifaddrs *list)
{
	while (list) {
		struct ifaddrs *next = list->ifa_next;

		kfree(container_of(list, struct ifaddrs_entry, ifa));
		list = next;
	}
}

unsigned int nt_if_nametoindex(const char *name)
{
	struct net *net = current->nsproxy ? current->nsproxy->net_ns : &init_net;
	struct net_device *dev = dev_get_by_name(net, name);
	unsigned int index;

	if (!dev) {
		nt_errno = ENODEV;
		return 0;
	}
	index = dev->ifindex;
	dev_put(dev);
	return index;
}

/* ------------------------------------------------------------- tasks */

/* the processes of NT are in the first pid namespace, as the server's were */
static struct task_struct *task_of(int tid)
{
	struct task_struct *task;

	rcu_read_lock();
	task = pid_task(find_pid_ns(tid, &init_pid_ns), PIDTYPE_PID);
	if (task)
		get_task_struct(task);
	rcu_read_unlock();
	return task;
}

int nt_kill(int pid, int sig)
{
	struct pid *p;
	int ret;

	rcu_read_lock();
	p = get_pid(find_pid_ns(pid, &init_pid_ns));
	rcu_read_unlock();
	if (!p)
		return set_errno(-ESRCH);
	ret = kill_pid(p, sig, 1);
	put_pid(p);
	return ret ? set_errno(ret) : 0;
}

/* what /proc/<pid>/status says of a process's memory, from its mm (the
 * server parsed that text): 0, or -1 with errno */
int nt_process_vm_counters(int pid, struct nt_vm_counters *c)
{
	struct task_struct *task = task_of(pid);
	struct mm_struct *mm;
	unsigned long rss, total;

	if (!task)
		return set_errno(-ESRCH);
	mm = get_task_mm(task);
	put_task_struct(task);
	if (!mm)
		return set_errno(-ESRCH);
	rss = get_mm_rss(mm);
	total = READ_ONCE(mm->total_vm);
	c->virtual_size = (unsigned long long)total << PAGE_SHIFT;			/* VmSize */
	c->peak_virtual_size = (unsigned long long)max(mm->hiwater_vm, total) << PAGE_SHIFT;	/* VmPeak */
	c->working_set_size = (unsigned long long)rss << PAGE_SHIFT;			/* VmRSS */
	c->peak_working_set_size = (unsigned long long)max(mm->hiwater_rss, rss) << PAGE_SHIFT;	/* VmHWM */
	c->pagefile_usage = (unsigned long long)(get_mm_counter(mm, MM_ANONPAGES) +	/* RssAnon */
						 get_mm_counter(mm, MM_SWAPENTS)) << PAGE_SHIFT;	/* VmSwap */
	mmput(mm);
	return 0;
}

int nt_tgkill(int tgid, int tid, int sig)
{
	struct task_struct *task = task_of(tid);
	int ret;

	if (!task || task->tgid != tgid) {
		if (task)
			put_task_struct(task);
		return set_errno(-ESRCH);
	}
	ret = do_send_sig_info(sig, SEND_SIG_PRIV, task, PIDTYPE_PID);
	put_task_struct(task);
	return ret ? set_errno(ret) : 0;
}

static long process_vm(int pid, unsigned long long addr, void *buf, size_t size, unsigned int gup_flags)
{
	struct task_struct *task = task_of(pid);
	long ret;

	if (!task)
		return set_errno(-ESRCH);
	ret = access_process_vm(task, addr, buf, size, gup_flags);
	put_task_struct(task);
	if (!ret && size)
		return set_errno(-EFAULT);
	return ret;
}

long nt_process_vm_read(int pid, unsigned long long addr, void *buf, size_t size)
{
	return process_vm(pid, addr, buf, size, 0);
}

long nt_process_vm_write(int pid, unsigned long long addr, const void *buf, size_t size)
{
	return process_vm(pid, addr, (void *)buf, size, FOLL_WRITE);
}

int nt_debugreg_get(int tid, int n, unsigned long *value)
{
	struct task_struct *task = task_of(tid);

	if (!task)
		return set_errno(-ESRCH);
	*value = ptrace_get_debugreg(task, n);
	put_task_struct(task);
	return 0;
}

int nt_debugreg_set(int tid, int n, unsigned long value)
{
	struct task_struct *task = task_of(tid);
	int ret;

	if (!task)
		return set_errno(-ESRCH);
	ret = ptrace_set_debugreg(task, n, value);
	put_task_struct(task);
	return ret ? set_errno(ret) : 0;
}

int nt_set_task_affinity(int tid, unsigned long long mask)
{
	cpumask_var_t set;
	int ret, cpu;

	if (!alloc_cpumask_var(&set, GFP_KERNEL))
		return set_errno(-ENOMEM);
	cpumask_clear(set);
	for (cpu = 0; cpu < 64 && cpu < nr_cpu_ids; cpu++)
		if (mask & (1ull << cpu))
			cpumask_set_cpu(cpu, set);
	ret = sched_setaffinity(tid, set);
	free_cpumask_var(set);
	return ret ? set_errno(ret) : 0;
}

unsigned long long nt_get_task_affinity(int tid)
{
	unsigned long long mask = 0;
	cpumask_var_t set;
	int cpu;

	if (!alloc_cpumask_var(&set, GFP_KERNEL))
		return 0;
	if (!sched_getaffinity(tid, set))
		for_each_cpu(cpu, set)
			if (cpu < 64)
				mask |= 1ull << cpu;
	free_cpumask_var(set);
	return mask;
}

int nt_setpriority(int which, int who, int prio)
{
	struct task_struct *task = task_of(who);

	if (!task)
		return set_errno(-ESRCH);
	set_user_nice(task, clamp(prio, MIN_NICE, MAX_NICE));
	put_task_struct(task);
	return 0;
}

int nt_getpid(void)
{
	return task_tgid_nr(current);
}

unsigned int nt_getuid(void)
{
	return from_kuid_munged(&init_user_ns, current_uid());
}

long nt_sysconf(int name)
{
	switch (name) {
	case _SC_PAGESIZE:
		return PAGE_SIZE;
	case _SC_NPROCESSORS_ONLN:
		return num_online_cpus();
	}
	return set_errno(-EINVAL);
}

/* ------------------------------------------------------------- time */

long long nt_time(long long *t)
{
	long long now = ktime_get_real_seconds();

	if (t)
		*t = now;
	return now;
}

long long nt_realtime_ticks(void)
{
	/* 100 ns since 1601, as Windows counts */
	return ktime_get_real_ns() / 100 + 116444736000000000ll;
}

/* the zone's rules, TZif as glibc reads them: the transitions and their
 * offsets; the local time of an instant is UTC plus the offset of the last
 * transition before it */
struct tz_rules
{
	unsigned int count, types;
	s64 *times;
	u8 *index;
	s32 *offsets;
	s32 default_offset;
	char path[256];
	s64 checked;
};

static struct tz_rules tz;

static u32 be32(const u8 *p)
{
	return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

static void tz_load(const char *path)
{
	struct file *file;
	u8 *data = NULL;
	loff_t size, pos = 0;
	u32 timecnt, typecnt, charcnt, leapcnt, isstd, isut;
	const u8 *p, *end;
	unsigned int i;

	kfree(tz.times);
	kfree(tz.index);
	kfree(tz.offsets);
	tz.times = NULL;
	tz.index = NULL;
	tz.offsets = NULL;
	tz.count = tz.types = 0;
	tz.default_offset = 0;
	file = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(file))
		return;
	size = i_size_read(file_inode(file));
	if (size < 44 || size > (1 << 20) || !(data = kvmalloc(size, GFP_KERNEL)) ||
	    kernel_read(file, data, size, &pos) != size || memcmp(data, "TZif", 4))
		goto out;
	end = data + size;
	/* the version 1 block, then (version 2+) the one with 64-bit times */
	isut = be32(data + 20);
	isstd = be32(data + 24);
	leapcnt = be32(data + 28);
	timecnt = be32(data + 32);
	typecnt = be32(data + 36);
	charcnt = be32(data + 40);
	p = data + 44;
	if (data[4] >= '2') {
		p += timecnt * 5 + typecnt * 6 + charcnt + leapcnt * 8 + isstd + isut;
		if (p + 44 > end || memcmp(p, "TZif", 4))
			goto out;
		isut = be32(p + 20);
		isstd = be32(p + 24);
		leapcnt = be32(p + 28);
		timecnt = be32(p + 32);
		typecnt = be32(p + 36);
		charcnt = be32(p + 40);
		p += 44;
		if (p + timecnt * 9 + typecnt * 6 > end || !typecnt)
			goto out;
		tz.times = kmalloc_array(timecnt + 1, sizeof(*tz.times), GFP_KERNEL);
		tz.index = kmalloc(timecnt + 1, GFP_KERNEL);
		tz.offsets = kmalloc_array(typecnt, sizeof(*tz.offsets), GFP_KERNEL);
		if (!tz.times || !tz.index || !tz.offsets)
			goto out;
		for (i = 0; i < timecnt; i++, p += 8)
			tz.times[i] = (s64)(((u64)be32(p) << 32) | be32(p + 4));
		for (i = 0; i < timecnt; i++)
			tz.index[i] = min_t(u8, *p++, typecnt - 1);
		for (i = 0; i < typecnt; i++, p += 6)
			tz.offsets[i] = (s32)be32(p);
	} else {
		if (p + timecnt * 5 + typecnt * 6 > end || !typecnt)
			goto out;
		tz.times = kmalloc_array(timecnt + 1, sizeof(*tz.times), GFP_KERNEL);
		tz.index = kmalloc(timecnt + 1, GFP_KERNEL);
		tz.offsets = kmalloc_array(typecnt, sizeof(*tz.offsets), GFP_KERNEL);
		if (!tz.times || !tz.index || !tz.offsets)
			goto out;
		for (i = 0; i < timecnt; i++, p += 4)
			tz.times[i] = (s32)be32(p);
		for (i = 0; i < timecnt; i++)
			tz.index[i] = min_t(u8, *p++, typecnt - 1);
		for (i = 0; i < typecnt; i++, p += 6)
			tz.offsets[i] = (s32)be32(p);
	}
	tz.count = timecnt;
	tz.types = typecnt;
	tz.default_offset = tz.offsets[0];
out:
	kvfree(data);
	fput(file);
}

long nt_timezone_bias(long long unix_time)
{
	const char *zone = nt_getenv("TZ");
	char path[256];
	s32 offset;
	unsigned int lo, hi;

	/* TZ=:/usr/share/zoneinfo/Europe/Kyiv, TZ=Europe/Kyiv, else /etc/localtime */
	if (zone && *zone == ':')
		zone++;
	if (zone && *zone == '/')
		strscpy(path, zone, sizeof(path));
	else if (zone && *zone)
		snprintf(path, sizeof(path), "/usr/share/zoneinfo/%s", zone);
	else
		strscpy(path, "/etc/localtime", sizeof(path));
	/* the file again when its name changed, or after a minute */
	if (strcmp(path, tz.path) || unix_time - tz.checked > 60 || unix_time < tz.checked) {
		strscpy(tz.path, path, sizeof(tz.path));
		tz.checked = unix_time;
		tz_load(path);
	}
	if (!tz.types)
		return 0;
	offset = tz.default_offset;
	if (tz.count && unix_time >= tz.times[0]) {
		lo = 0;
		hi = tz.count;
		while (hi - lo > 1) {
			unsigned int mid = (lo + hi) / 2;

			if (tz.times[mid] <= unix_time)
				lo = mid;
			else
				hi = mid;
		}
		offset = tz.offsets[tz.index[lo]];
	}
	return -offset;	/* Windows' bias: UTC minus local */
}

/* ------------------------------------------------------------- memory and strings */

void *nt_malloc(size_t size)
{
	return kvmalloc(size ? size : 1, GFP_KERNEL);
}

void *nt_calloc(size_t count, size_t size)
{
	return kvcalloc(count ? count : 1, size ? size : 1, GFP_KERNEL);
}

void *nt_realloc(void *ptr, size_t size)
{
	return kvrealloc(ptr, size ? size : 1, GFP_KERNEL);
}

void nt_free(void *ptr)
{
	kvfree(ptr);
}

char *nt_strdup(const char *str)
{
	return kstrdup(str, GFP_KERNEL);
}

unsigned long nt_strtoul(const char *str, char **end, int base)
{
	return simple_strtoul(str, end, base);
}

int nt_atoi(const char *str)
{
	return (int)simple_strtol(str, NULL, 10);
}

int nt_asprintf(char **out, const char *format, ...)
{
	va_list args;

	va_start(args, format);
	*out = kvasprintf(GFP_KERNEL, format, args);
	va_end(args);
	return *out ? strlen(*out) : -1;
}

const char *nt_strerror(int err)
{
	static char buf[24];

	switch (err) {
	case EPERM: return "Operation not permitted";
	case ENOENT: return "No such file or directory";
	case ESRCH: return "No such process";
	case EINTR: return "Interrupted system call";
	case EIO: return "Input/output error";
	case EBADF: return "Bad file descriptor";
	case EAGAIN: return "Resource temporarily unavailable";
	case ENOMEM: return "Cannot allocate memory";
	case EACCES: return "Permission denied";
	case EEXIST: return "File exists";
	case ENOTDIR: return "Not a directory";
	case EISDIR: return "Is a directory";
	case EINVAL: return "Invalid argument";
	case EMFILE: return "Too many open files";
	case ENOSPC: return "No space left on device";
	case EROFS: return "Read-only file system";
	case EPIPE: return "Broken pipe";
	case ENOSYS: return "Function not implemented";
	case ENOTEMPTY: return "Directory not empty";
	case ENOTSOCK: return "Socket operation on non-socket";
	case EOPNOTSUPP: return "Operation not supported";
	case EADDRINUSE: return "Address already in use";
	case ECONNREFUSED: return "Connection refused";
	}
	snprintf(buf, sizeof(buf), "error %d", err);
	return buf;
}

/* the server's environment: what arctic-init gave nt_boot, "NAME=value" each */
static char **environ;
static unsigned int environ_count;

void nt_crt_set_environ(char **env, unsigned int count)
{
	environ = env;
	environ_count = count;
}

char *nt_getenv(const char *name)
{
	size_t len = strlen(name);
	unsigned int i;

	for (i = 0; i < environ_count; i++)
		if (!strncmp(environ[i], name, len) && environ[i][len] == '=')
			return environ[i] + len + 1;
	return NULL;
}

/* ------------------------------------------------------------- stdio */

struct nt_file_stream
{
	int fd;			/* -1: the kernel's log (stderr), -2: memory */
	bool writing;
	char *buf;
	size_t size, used, pos;	/* read: used bytes, pos the next */
	bool eof, error;
};

static struct nt_file_stream stderr_stream = { .fd = -1, .writing = true };
ntos_FILE *nt_stderr = &stderr_stream;

static char log_line[512];
static size_t log_used;

/* stderr: whole lines to the kernel's log */
static void log_write(const char *data, size_t len)
{
	while (len--) {
		char c = *data++;

		if (c == '\n' || log_used == sizeof(log_line) - 1) {
			log_line[log_used] = 0;
			pr_info("ntos: %s\n", log_line);
			log_used = 0;
			if (c == '\n')
				continue;
		}
		log_line[log_used++] = c;
	}
}

static int stream_flush(ntos_FILE *f)
{
	size_t done = 0;

	if (f->fd < 0 || !f->writing)
		return 0;
	while (done < f->used) {
		nt_ssize_t ret = nt_write(f->fd, f->buf + done, f->used - done);

		if (ret <= 0) {
			f->error = true;
			return EOF;
		}
		done += ret;
	}
	f->used = 0;
	return 0;
}

size_t nt_fwrite(const void *ptr, size_t size, size_t count, ntos_FILE *f)
{
	size_t len = size * count;
	const char *data = ptr;

	if (f->fd == -1) {
		log_write(data, len);
		return count;
	}
	while (len) {
		size_t part;

		if (f->used == f->size) {
			if (f->fd == -2) {
				size_t grown = f->size ? f->size * 2 : 65536;
				char *buf = kvrealloc(f->buf, grown, GFP_KERNEL);

				if (!buf) {
					f->error = true;
					return 0;
				}
				f->buf = buf;
				f->size = grown;
			} else if (stream_flush(f)) {
				return 0;
			}
		}
		part = min(len, f->size - f->used);
		memcpy(f->buf + f->used, data, part);
		f->used += part;
		data += part;
		len -= part;
	}
	return count;
}

int nt_vfprintf(ntos_FILE *f, const char *format, va_list args)
{
	char small[256], *text = small;
	va_list copy;
	int len;

	va_copy(copy, args);
	len = vsnprintf(small, sizeof(small), format, copy);
	va_end(copy);
	if (len >= (int)sizeof(small)) {
		if (!(text = kvmalloc(len + 1, GFP_KERNEL)))
			return -1;
		vsnprintf(text, len + 1, format, args);
	}
	nt_fwrite(text, 1, len, f);
	if (text != small)
		kvfree(text);
	return f->error ? -1 : len;
}

int nt_fprintf(ntos_FILE *f, const char *format, ...)
{
	va_list args;
	int ret;

	va_start(args, format);
	ret = nt_vfprintf(f, format, args);
	va_end(args);
	return ret;
}

int nt_fputs(const char *str, ntos_FILE *f)
{
	nt_fwrite(str, 1, strlen(str), f);
	return f->error ? EOF : 0;
}

int nt_fputc(int c, ntos_FILE *f)
{
	char ch = c;

	nt_fwrite(&ch, 1, 1, f);
	return f->error ? EOF : (unsigned char)c;
}

void nt_perror(const char *str)
{
	nt_fprintf(nt_stderr, "%s: %s\n", str, nt_strerror(nt_errno));
}

static ntos_FILE *stream_new(int fd, const char *mode)
{
	ntos_FILE *f = kzalloc(sizeof(*f), GFP_KERNEL);

	if (!f)
		return NULL;
	f->fd = fd;
	f->writing = mode[0] != 'r';
	f->size = 65536;
	if (!(f->buf = kvmalloc(f->size, GFP_KERNEL))) {
		kfree(f);
		return NULL;
	}
	return f;
}

ntos_FILE *nt_fdopen(int fd, const char *mode)
{
	if (!nt_file_of(fd)) {
		nt_errno = EBADF;
		return NULL;
	}
	return stream_new(fd, mode);
}

ntos_FILE *nt_fopen(const char *path, const char *mode)
{
	int flags = mode[0] == 'r' ? O_RDONLY : O_WRONLY | O_CREAT | O_TRUNC;
	ntos_FILE *f;
	int fd;

	if (strchr(mode, '+'))
		flags = (flags & ~O_ACCMODE) | O_RDWR;
	if ((fd = nt_open(path, flags, 0666)) == -1)
		return NULL;
	if (!(f = stream_new(fd, mode)))
		nt_close(fd);
	return f;
}

int nt_fflush(ntos_FILE *f)
{
	return stream_flush(f);
}

int nt_fclose(ntos_FILE *f)
{
	int ret = stream_flush(f);

	if (f->fd >= 0 && nt_close(f->fd))
		ret = EOF;
	kvfree(f->buf);
	kfree(f);
	return ret;
}

int nt_fileno(ntos_FILE *f)
{
	return f->fd;
}

char *nt_fgets(char *out, int size, ntos_FILE *f)
{
	int n = 0;

	if (size <= 0)
		return NULL;
	while (n < size - 1) {
		char c;

		if (f->pos == f->used) {
			nt_ssize_t ret;

			if (f->eof)
				break;
			ret = nt_read(f->fd, f->buf, f->size);
			if (ret <= 0) {
				f->eof = true;
				if (ret < 0)
					f->error = true;
				break;
			}
			f->used = ret;
			f->pos = 0;
		}
		c = f->buf[f->pos++];
		out[n++] = c;
		if (c == '\n')
			break;
	}
	if (!n)
		return NULL;
	out[n] = 0;
	return out;
}

ntos_FILE *nt_open_memstream(void)
{
	ntos_FILE *f = kzalloc(sizeof(*f), GFP_KERNEL);

	if (f) {
		f->fd = -2;
		f->writing = true;
	}
	return f;
}

int nt_memstream_take(ntos_FILE *f, void **data, size_t *size)
{
	bool ok = !f->error;

	*data = f->buf;
	*size = f->used;
	if (!ok)
		kvfree(f->buf);
	kfree(f);
	return ok;
}

/* ------------------------------------------------------------- memory shared with the clients */

int nt_memfd_create(const char *name, unsigned int flags)
{
	struct file *file = shmem_file_setup(name, 0, mk_vma_flags(VMA_NORESERVE_BIT));

	if (IS_ERR(file))
		return set_errno(PTR_ERR(file));
	file->f_mode |= FMODE_LSEEK | FMODE_PREAD | FMODE_PWRITE;
	file->f_flags |= O_RDWR | O_LARGEFILE;
	return nt_fd_for_file(file);
}

struct mapped
{
	struct list_head entry;
	void *addr;
	unsigned int count;
	struct page **pages;
};

static LIST_HEAD(mapped_list);

void *nt_mmap(void *addr, size_t length, int prot, int flags, int fd, long long offset)
{
	struct file *file = file_or_ebadf(fd);
	struct mapped *m;
	unsigned int i;

	if (!file)
		return MAP_FAILED;
	if (!(flags & MAP_SHARED) || (prot & PROT_EXEC) || !shmem_file(file) || (offset & ~PAGE_MASK)) {
		nt_errno = EACCES;
		return MAP_FAILED;
	}
	if (!(m = kzalloc(sizeof(*m), GFP_KERNEL)))
		goto nomem;
	m->count = DIV_ROUND_UP(length, PAGE_SIZE);
	if (!(m->pages = kvcalloc(m->count, sizeof(*m->pages), GFP_KERNEL)))
		goto nomem;
	for (i = 0; i < m->count; i++) {
		struct folio *folio;

		folio = shmem_read_folio(file->f_mapping, (offset >> PAGE_SHIFT) + i);
		if (IS_ERR(folio))
			goto fail;
		m->pages[i] = folio_file_page(folio, (offset >> PAGE_SHIFT) + i);
		get_page(m->pages[i]);
		folio_put(folio);
	}
	if (!(m->addr = vmap(m->pages, m->count, VM_MAP, PAGE_KERNEL)))
		goto fail;
	list_add(&m->entry, &mapped_list);
	return m->addr;
fail:
	while (i--)
		put_page(m->pages[i]);
nomem:
	if (m)
		kvfree(m->pages);
	kfree(m);
	nt_errno = ENOMEM;
	return MAP_FAILED;
}

int nt_munmap(void *addr, size_t length)
{
	struct mapped *m;
	unsigned int i;

	list_for_each_entry(m, &mapped_list, entry) {
		if (m->addr != addr)
			continue;
		list_del(&m->entry);
		vunmap(m->addr);
		for (i = 0; i < m->count; i++)
			put_page(m->pages[i]);
		kvfree(m->pages);
		kfree(m);
		return 0;
	}
	return set_errno(-EINVAL);
}

/* ------------------------------------------------------------- inotify (patch 0008) */

int nt_inotify_init(void)
{
	struct file *file = inotify_kernel_file();

	if (IS_ERR(file))
		return set_errno(PTR_ERR(file));
	return nt_fd_for_file(file);
}

int nt_inotify_add_watch(int fd, const char *path, unsigned int mask)
{
	struct file *file = file_or_ebadf(fd);
	struct path p;
	int ret;

	if (!file)
		return -1;
	if ((ret = lookup(path, (mask & IN_DONT_FOLLOW) ? 0 : LOOKUP_FOLLOW, &p)))
		return set_errno(ret);
	ret = inotify_kernel_add_watch(file, &p, mask);
	path_put(&p);
	return ret < 0 ? set_errno(ret) : ret;
}

/* the path of a descriptor of ntos, as readlink of /proc/self/fd/N gave the
 * server its own (the caller's /proc/self is another process here), with
 * extra bytes free at its end; NULL if it has none (deleted, unreachable) */
char *nt_fd_path(int fd, int extra)
{
	struct file *file = file_or_ebadf(fd);
	char *buf, *p, *ret;

	if (!file || !(buf = kmalloc(PATH_MAX, GFP_KERNEL)))
		return NULL;
	p = d_path(&file->f_path, buf, PATH_MAX);
	if (IS_ERR(p) || !(ret = nt_malloc(strlen(p) + 1 + extra))) {
		kfree(buf);
		return NULL;
	}
	strcpy(ret, p);
	kfree(buf);
	return ret;
}

int nt_inotify_rm_watch(int fd, int wd)
{
	struct file *file = file_or_ebadf(fd);
	int ret;

	if (!file)
		return -1;
	ret = inotify_kernel_rm_watch(file, wd);
	return ret ? set_errno(ret) : 0;
}

/* ------------------------------------------------------------- files written beside the executive */

struct save_job
{
	struct work_struct work;
	struct path dir;
	char *name;
	void *data;
	size_t size;
	nt_save_format_fn format;	/* data -> the file's text, here rather than under the lock */
};

static struct workqueue_struct *saver;
static atomic_t saving = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(saved);

/* the hive's own file: a symbolic link is followed (C:\Windows\System32\config) */
static int save_target(struct save_job *job, struct path *dir, char **name)
{
	struct path link;
	char *buf, *target, *slash, *dir_name, *hive;
	int ret;

	/* The link is absolute (/mnt/c/...). vfs_path_lookup() takes the
	 * directory it starts from for the root too, and so resolved it as
	 * /run/nt/mnt/c/...: never there, the hive was written over the link
	 * into /run/nt, which is memory, and every change to the registry since
	 * the executive came into the kernel was gone at the next start
	 * (2026-11-25). The hive's own path is looked up from the root. */
	if (!(buf = kmalloc(PATH_MAX, GFP_KERNEL)))
		return -ENOMEM;
	dir_name = d_path(&job->dir, buf, PATH_MAX);
	hive = IS_ERR(dir_name) ? NULL : kasprintf(GFP_KERNEL, "%s/%s", dir_name, job->name);
	kfree(buf);
	if (!hive)
		return -ENOMEM;
	ret = kern_path(hive, LOOKUP_FOLLOW, &link);
	kfree(hive);
	if (ret) {
		/* not there yet: written where it is named */
		*dir = job->dir;
		path_get(dir);
		*name = kstrdup(job->name, GFP_KERNEL);
		return *name ? 0 : -ENOMEM;
	}
	if (!(buf = kmalloc(PATH_MAX, GFP_KERNEL))) {
		path_put(&link);
		return -ENOMEM;
	}
	target = d_path(&link, buf, PATH_MAX);
	path_put(&link);
	if (IS_ERR(target) || !(slash = strrchr(target, '/'))) {
		kfree(buf);
		return -EINVAL;
	}
	*name = kstrdup(slash + 1, GFP_KERNEL);
	*slash = 0;
	ret = kern_path(*target ? target : "/", LOOKUP_FOLLOW | LOOKUP_DIRECTORY, dir);
	kfree(buf);
	if (ret)
		kfree(*name);
	return ret;
}

static void save_work(struct work_struct *work)
{
	struct save_job *job = container_of(work, struct save_job, work);
	const struct cred *old = override_creds(nt_creds());
	char *name = NULL, *tmp = NULL;
	struct path dir;
	struct file *file;
	loff_t pos = 0;
	int ret;

	if (job->format) {
		void *text;
		size_t size;

		if (!job->format(job->data, job->size, &text, &size)) {
			ret = -ENOMEM;
			goto out;
		}
		kvfree(job->data);
		job->data = text;
		job->size = size;
	}
	if ((ret = save_target(job, &dir, &name)))
		goto out;
	tmp = kasprintf(GFP_KERNEL, "reg%lx%04x.tmp", (long)task_pid_nr(current), (unsigned int)(atomic_read(&saving) & 0xffff));
	if (!tmp)
		goto put;
	file = file_open_root(&dir, tmp, O_CREAT | O_EXCL | O_WRONLY | O_LARGEFILE, 0666);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		goto put;
	}
	ret = kernel_write(file, job->data, job->size, &pos) == job->size ? 0 : -EIO;
	/* on the disk before the rename makes it the hive */
	if (!ret)
		ret = vfs_fsync(file, 0);
	fput(file);
	{
		char *dirbuf = kmalloc(PATH_MAX, GFP_KERNEL), *d = dirbuf ? d_path(&dir, dirbuf, PATH_MAX) : NULL;
		char *from = (d && !IS_ERR(d)) ? kasprintf(GFP_KERNEL, "%s/%s", d, tmp) : NULL;
		char *to = (d && !IS_ERR(d)) ? kasprintf(GFP_KERNEL, "%s/%s", d, name) : NULL;

		if (!ret && from && to) {
			struct filename *a = getname_kernel(from), *b = getname_kernel(to);

			ret = (IS_ERR(a) || IS_ERR(b)) ? -ENOMEM : filename_renameat2(AT_FDCWD, a, AT_FDCWD, b, 0);
			if (!IS_ERR(a))
				putname(a);
			if (!IS_ERR(b))
				putname(b);
		} else if (!ret) {
			ret = -ENOMEM;
		}
		if (ret && from) {
			struct filename *a = getname_kernel(from);

			if (!IS_ERR(a)) {
				filename_unlinkat(AT_FDCWD, a);
				putname(a);
			}
		}
		kfree(from);
		kfree(to);
		kfree(dirbuf);
	}
put:
	path_put(&dir);
out:
	if (ret)
		pr_warn("ntos: could not save the registry branch %s (%d)\n", job->name, ret);
	revert_creds(old);
	path_put(&job->dir);
	kfree(name);
	kfree(tmp);
	kfree(job->name);
	kvfree(job->data);
	kfree(job);
	if (atomic_dec_and_test(&saving))
		wake_up_all(&saved);
}

int nt_saver_queue(int dir_fd, const char *name, void *data, size_t size, nt_save_format_fn format)
{
	struct file *dir = nt_file_of(dir_fd);
	struct save_job *job;

	/* freezable: a save being written ends before hibernation takes its
	 * image (frozen half way, a machine that then lost power had no hive) */
	if (!dir || (!saver && !(saver = alloc_ordered_workqueue("ntos-save", WQ_FREEZABLE | WQ_MEM_RECLAIM))) ||
	    !(job = kzalloc(sizeof(*job), GFP_KERNEL))) {
		kvfree(data);
		return 0;
	}
	if (!(job->name = kstrdup(name, GFP_KERNEL))) {
		kfree(job);
		kvfree(data);
		return 0;
	}
	job->dir = dir->f_path;
	path_get(&job->dir);
	job->data = data;
	job->size = size;
	job->format = format;
	INIT_WORK(&job->work, save_work);
	atomic_inc(&saving);
	queue_work(saver, &job->work);
	return 1;
}

int nt_saver_busy(void)
{
	return atomic_read(&saving) != 0;
}

void nt_saver_wait(void)
{
	wait_event(saved, !atomic_read(&saving));
}

/* ------------------------------------------------------------- ntsync (patch 0008) */

int nt_ntsync_create(int dev_fd, unsigned int cmd, const void *args)
{
	struct file *dev = file_or_ebadf(dev_fd), *file;

	if (!dev)
		return -1;
	file = ntsync_kernel_create(dev, cmd, args);
	if (IS_ERR(file))
		return set_errno(PTR_ERR(file));
	return nt_fd_for_file(file);
}

int nt_ntsync_ioctl(int fd, unsigned int cmd, void *arg)
{
	struct file *file = file_or_ebadf(fd);
	int ret;

	if (!file)
		return -1;
	ret = ntsync_kernel_ioctl(file, cmd, arg);
	return ret ? set_errno(ret) : 0;
}
