/* arctic-lxss: the Linux distributions of WSL, in Arctic (docs/updates.md).
 *
 * The very disks WSL keeps on Windows 10 (ext4.vhdx) are attached with
 * qemu-nbd and mounted under /run/lxss/<name>, with every drive letter
 * under /mnt/<letter> as WSL has it, and under /<letter> as Git Bash names
 * it (Claude Code speaks Git Bash). wsl.exe, bash.exe and git.exe (Wine
 * builtins, runtime/wine/modules/programs/wsl) ask this service over TCP on
 * 127.0.0.1 to run a command there: the request, then stdin, stdout and
 * stderr as frames, then the exit code. Like LxssManager it runs as root,
 * and commands run as root in the distribution unless asked otherwise.
 *
 *   C:\ProgramData\Arctic\Lxss\distros   "name D:\WSL\name\ext4.vhdx", a line
 *                                        each, the default first
 *   C:\ProgramData\Arctic\Lxss\session   "port token", written at start
 *
 * On SIGTERM (arctic-init, before anything else when the system goes down)
 * the processes in the distributions end, the disks are unmounted and
 * qemu-nbd writes the VHDX out: a VHDX left half written is a lost distro. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sched.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "gate.h"

#define C_DRIVE "/mnt/c"
#define DOSDEVICES "/run/nt/dosdevices"
#define LXSS_DIR C_DRIVE "/ProgramData/Arctic/Lxss"
#define DISTROS LXSS_DIR "/distros"
#define SESSION LXSS_DIR "/session"
#define ROOTS "/run/lxss"
#define LOCK ROOTS "/.lock"
#define NT_USER "nt"
#define MAX_NBD 16
#define ATTACH_SECONDS 120 /* a VHDX log to replay on a slow disk takes a while */

/* the protocol, shared with programs/wsl/main.c */
#define MAGIC "ARCTLXS1"
#define TOKEN_LEN 32
enum { F_STDIN, F_STDIN_EOF, F_STDOUT, F_STDERR, F_EXIT, F_ERROR, F_SIGNAL, F_DENIED };
#define REQ_SHUTDOWN 1  /* wsl --shutdown */
#define REQ_GITBASH 2   /* the working folder as Git Bash names it: /d/... */
#define REQ_TERMINATE 4 /* wsl --terminate <name> */
#define REQ_GATE 8      /* the Panel's "status" / "password" / "require" */
#define REQ_HOST 16     /* hamam.exe: root on the host kernel, behind the consent window */

/* ---- the containers ----
 *
 * A command of wsl.exe runs in its distribution's container, as WSL runs it
 * on Windows, only without a VM: its own user, mount, PID, IPC, UTS and
 * cgroup namespaces on the host's kernel. Root in there is the Windows
 * user (nt) on the host and nothing more: no host process is in sight, no
 * device but the harmless ones, /sys read-only, no firmware, no module, no
 * disk but its own and the drives, and C:\ as a standard user of Windows
 * has it. Whatever is deleted in there, the kernel and the system stay.
 * The container lives from its first command to wsl --terminate, --shutdown
 * or the end of the system; every command joins it (setns), as WSL's do.
 * Root on the host itself is hamam.exe's (REQ_HOST), behind the consent
 * window (docs/linux-gate.md). */
#define CONTAINERS ROOTS "/.c"   /* a distribution's disk, staged id-mapped for its container */
#define SUBID_BASE 1000000u      /* the ids 1..65535 of every container, on the host */
#define SUBID_COUNT 65535u

#ifndef OPEN_TREE_CLONE
#define OPEN_TREE_CLONE 1
#endif
#ifndef OPEN_TREE_CLOEXEC
#define OPEN_TREE_CLOEXEC O_CLOEXEC
#endif
#ifndef MOVE_MOUNT_F_EMPTY_PATH
#define MOVE_MOUNT_F_EMPTY_PATH 0x00000004
#endif
#ifndef MOUNT_ATTR_RDONLY
#define MOUNT_ATTR_RDONLY 0x00000001
#endif
#ifndef MOUNT_ATTR_IDMAP
#define MOUNT_ATTR_IDMAP 0x00100000
#endif
#ifndef AT_RECURSIVE
#define AT_RECURSIVE 0x8000
#endif
#ifndef SYS_open_tree
#define SYS_open_tree 428
#endif
#ifndef SYS_move_mount
#define SYS_move_mount 429
#endif
#ifndef SYS_mount_setattr
#define SYS_mount_setattr 442
#endif
struct lxss_mount_attr {
    uint64_t attr_set, attr_clr, propagation, userns_fd;
};

static char token[TOKEN_LEN + 1];
static volatile sig_atomic_t stopping;
static void say(const char *fmt, ...);
static long read_number(const char *path);
static void write_text(const char *path, const char *text);
static int running(pid_t pid);
#ifndef SYS_close_range
#define SYS_close_range 436
#endif

/* ---- the Linux gate (docs/linux-gate.md) ----
 *
 * Who may skip the consent window: a table of (pid, start time) of already
 * agreed-to source processes, and whether the password has been typed once
 * this boot, in memory shared across every connection's forked child
 * (mapped before the accept loop's first fork). A pid is never enough by
 * itself: a new, unrelated process reusing an old pid must not inherit its
 * grant, hence the start time alongside it. */
#define MAX_GRANTS 256
#define GATE_LOCK GATE_RUN_DIR "/.lock"         /* the grants table and password_entered */
#define CONSENT_LOCK GATE_RUN_DIR "/.consent"   /* one consent.exe window at a time, as in Windows */
#define APPINFO_SESSION C_DRIVE "/ProgramData/Arctic/Appinfo/session"
#define APPINFO_WAIT_S 20                       /* the service comes up with the other services of Windows */
#define CONSENT_TIMEOUT_S 120
#define MAX_WRONG_ATTEMPTS 5
#define WRONG_PAUSE_S 30

struct grant {
    pid_t pid;
    long long start_time;
};

struct gate_shared {
    int password_entered;
    int ngrants;
    int next;
    struct grant grants[MAX_GRANTS];
};

static struct gate_shared *gate_shm;
static int gate_lock_fd = -1;

static void gate_shared_init(void)
{
    gate_shm = mmap(NULL, sizeof(*gate_shm), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (gate_shm == MAP_FAILED) {
        say("gate: shared memory: %s", strerror(errno));
        exit(1);
    }
    memset(gate_shm, 0, sizeof(*gate_shm));
    mkdir(GATE_RUN_DIR, 0700); /* already there when arctic-init bound it; ours anyway if not */
    gate_lock_fd = open(GATE_LOCK, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
}

static void gate_lock(void)
{
    if (gate_lock_fd >= 0)
        flock(gate_lock_fd, LOCK_EX);
}

static void gate_unlock(void)
{
    if (gate_lock_fd >= 0)
        flock(gate_lock_fd, LOCK_UN);
}

static int grant_lookup(pid_t pid, long long start_time)
{
    for (int i = 0; i < gate_shm->ngrants; i++)
        if (gate_shm->grants[i].pid == pid && gate_shm->grants[i].start_time == start_time)
            return 1;
    return 0;
}

static void grant_add(pid_t pid, long long start_time)
{
    if (grant_lookup(pid, start_time))
        return;
    gate_shm->grants[gate_shm->next] = (struct grant){pid, start_time};
    gate_shm->next = (gate_shm->next + 1) % MAX_GRANTS;
    if (gate_shm->ngrants < MAX_GRANTS)
        gate_shm->ngrants++;
}

static void say(const char *fmt, ...)
{
    char buf[1024];
    int n = snprintf(buf, sizeof(buf), "lxss: ");
    va_list ap;

    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof(buf) - (size_t)n - 1, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf) - 2)
        n = sizeof(buf) - 2;
    buf[n++] = '\n';
    write(2, buf, (size_t)n);
}

static int run(char *const argv[])
{
    int status;
    pid_t pid = fork();

    if (!pid) {
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0);
        execv(argv[0], argv);
        _exit(127);
    }
    if (pid < 0 || waitpid(pid, &status, 0) < 0)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int running(pid_t pid);

/* run(), for so many seconds at most: -2 when it took longer and was ended */
static int run_for(char *const argv[], int seconds)
{
    int status;
    pid_t pid = fork();

    if (!pid) {
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0);
        execv(argv[0], argv);
        _exit(127);
    }
    if (pid < 0)
        return -1;
    for (int i = 0; i < seconds * 10; i++) {
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid)
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (done < 0)
            return -1;
        usleep(100000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return -2;
}

/* the processes started with this argument, signalled when sig is not 0 */
static int signal_by_argument(const char *arg, int sig)
{
    char path[64], cmdline[4096];
    DIR *proc = opendir("/proc");
    int found = 0;

    for (struct dirent *de; proc && (de = readdir(proc));) {
        if (!isdigit((unsigned char)de->d_name[0]))
            continue;
        snprintf(path, sizeof(path), "/proc/%s/cmdline", de->d_name);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        ssize_t n = fd >= 0 ? read(fd, cmdline, sizeof(cmdline) - 1) : -1;
        if (fd >= 0)
            close(fd);
        if (n <= 0 || !running((pid_t)atoi(de->d_name)))
            continue;
        cmdline[n] = 0;
        for (char *p = cmdline; p < cmdline + n; p += strlen(p) + 1)
            if (!strcmp(p, arg)) {
                if (sig)
                    kill((pid_t)atoi(de->d_name), sig);
                found++;
                break;
            }
    }
    if (proc)
        closedir(proc);
    return found;
}

/* qemu-nbd --fork serves from a daemon that is not our child, and its pid
 * file comes only once the disk is attached: its own socket names it.
 * SIGTERM first, so that it closes the VHDX as it should. */
static void end_by_argument(const char *arg)
{
    for (int sig = SIGTERM; sig; sig = sig == SIGTERM ? SIGKILL : 0) {
        if (!signal_by_argument(arg, sig))
            return;
        /* 10 s to write it out; the main process reaps it, a zombie has ended */
        for (int i = 0; i < 100 && signal_by_argument(arg, 0); i++)
            usleep(100000);
    }
}

static void make_dirs(const char *path)
{
    char p[PATH_MAX];

    snprintf(p, sizeof(p), "%s", path);
    for (char *s = strchr(p + 1, '/'); s; s = strchr(s + 1, '/')) {
        *s = 0;
        mkdir(p, 0755);
        *s = '/';
    }
    mkdir(p, 0755);
}

static int is_mountpoint(const char *path)
{
    char parent[PATH_MAX];
    struct stat a, b;

    snprintf(parent, sizeof(parent), "%s/..", path);
    return !stat(path, &a) && !stat(parent, &b) && (a.st_dev != b.st_dev || a.st_ino == b.st_ino);
}

/* C:\x\y as the host sees it, through Wine's drive letters */
static int windows_to_host(const char *win, char *out, size_t len)
{
    char link[64], target[PATH_MAX];

    if (!isalpha((unsigned char)win[0]) || win[1] != ':')
        return -1;
    snprintf(link, sizeof(link), DOSDEVICES "/%c:", tolower((unsigned char)win[0]));
    if (!realpath(link, target))
        return -1;
    snprintf(out, len, "%s/%s", target, win[2] == '\\' || win[2] == '/' ? win + 3 : win + 2);
    for (char *p = out; *p; p++)
        if (*p == '\\')
            *p = '/';
    return 0;
}

/* The disk of a distribution: at its drive letter or, as letters can move
 * between starts, at the same path on another drive */
static int find_vhdx(const char *win, char *out, size_t len)
{
    char other[PATH_MAX];
    DIR *d;

    if (!windows_to_host(win, out, len) && !access(out, R_OK | W_OK))
        return 0;
    if (!(d = opendir(DOSDEVICES)))
        return -1;
    for (struct dirent *de; (de = readdir(d));) {
        if (strlen(de->d_name) != 2 || de->d_name[1] != ':' || tolower((unsigned char)de->d_name[0]) == tolower((unsigned char)win[0]))
            continue;
        snprintf(other, sizeof(other), "%c%s", de->d_name[0], win + 1);
        if (!windows_to_host(other, out, len) && !access(out, R_OK | W_OK)) {
            say("%s is on %c: now", win, toupper((unsigned char)de->d_name[0]));
            closedir(d);
            return 0;
        }
    }
    closedir(d);
    return -1;
}

/* a line of the distros file: the one named, or the first */
static int find_distro(const char *want, char *name, size_t name_len, char *vhdx, size_t vhdx_len)
{
    char line[PATH_MAX + 128];
    FILE *f = fopen(DISTROS, "re");
    int found = 0;

    while (f && !found && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *gap = line + strcspn(line, " \t"), *path = gap + strspn(gap, " \t");
        if (!*gap || !*path || line[0] == '#')
            continue;
        *gap = 0;
        if (!*want || !strcasecmp(want, line)) {
            snprintf(name, name_len, "%s", line);
            snprintf(vhdx, vhdx_len, "%s", path);
            found = 1;
        }
    }
    if (f)
        fclose(f);
    return found ? 0 : -1;
}

/* Every drive letter, bound as /mnt/x and /x. Done for each command: a
 * stick plugged in since is there too. */
static void bind_drives(const char *root)
{
    char link[PATH_MAX], target[PATH_MAX], dir[PATH_MAX];
    DIR *d = opendir(DOSDEVICES);

    for (struct dirent *de; d && (de = readdir(d));) {
        if (strlen(de->d_name) != 2 || de->d_name[1] != ':' || !isalpha((unsigned char)de->d_name[0]))
            continue;
        snprintf(link, sizeof(link), DOSDEVICES "/%s", de->d_name);
        struct stat st;
        if (!realpath(link, target) || stat(target, &st) || !S_ISDIR(st.st_mode))
            continue;
        char letter = (char)tolower((unsigned char)de->d_name[0]);
        for (int git_bash = 0; git_bash < 2; git_bash++) {
            snprintf(dir, sizeof(dir), git_bash ? "%s/%c" : "%s/mnt/%c", root, letter);
            make_dirs(dir);
            if (is_mountpoint(dir))
                continue;
            if (mount(target, dir, NULL, MS_BIND | MS_REC, NULL))
                say("%s -> %s: %s", target, dir, strerror(errno));
        }
    }
    if (d)
        closedir(d);
}

/* ---- the containers: what is in them ---- */

static int lxss_mount_setattr(int dfd, const char *path, unsigned int flags, uint64_t set, int userns_fd)
{
    struct lxss_mount_attr attr = {.attr_set = set, .userns_fd = (uint64_t)userns_fd};

    return (int)syscall(SYS_mount_setattr, dfd, path, flags, &attr, sizeof(attr));
}

/* a mount made read-only, whatever flags it was locked with */
static void read_only(const char *path)
{
    if (lxss_mount_setattr(AT_FDCWD, path, 0, MOUNT_ATTR_RDONLY, 0))
        say("container: %s read-only: %s", path, strerror(errno));
}

static void bind_at(const char *src, const char *dst, unsigned long flags)
{
    if (mount(src, dst, NULL, MS_BIND | flags, NULL))
        say("container: %s -> %s: %s", src, dst, strerror(errno));
}

/* the drive that holds Windows */
static int system_drive(const char *target)
{
    char path[PATH_MAX];

    snprintf(path, sizeof(path), "%s/Windows/System32", target);
    return !access(path, F_OK);
}

/* The drives, as WSL gives them: /mnt/x, and /x as Git Bash names them. The
 * container's root is the Windows user, and the drives are the user's as a
 * standard user of Windows has them: the system's drive read-only but its
 * Users folder, the other drives writable; the disks of the distributions
 * read-only whatever drive they are on (deleting the disk a distribution
 * runs from would lose it). Before pivot_root: the host's paths are still
 * there to bind from. */
static void container_drives(const char *root)
{
    char link[PATH_MAX], target[PATH_MAX], dir[PATH_MAX], sub[PATH_MAX];
    char drives[26][PATH_MAX];
    int count = 0;
    DIR *d = opendir(DOSDEVICES);

    for (struct dirent *de; d && (de = readdir(d));) {
        if (strlen(de->d_name) != 2 || de->d_name[1] != ':' || !isalpha((unsigned char)de->d_name[0]))
            continue;
        snprintf(link, sizeof(link), DOSDEVICES "/%s", de->d_name);
        struct stat st;
        if (!realpath(link, target) || stat(target, &st) || !S_ISDIR(st.st_mode))
            continue;
        char letter = (char)tolower((unsigned char)de->d_name[0]);
        int sys = system_drive(target);
        for (int git_bash = 0; git_bash < 2; git_bash++) {
            snprintf(dir, sizeof(dir), git_bash ? "%s/%c" : "%s/mnt/%c", root, letter);
            make_dirs(dir);
            bind_at(target, dir, MS_REC);
            if (sys) {
                read_only(dir);
                snprintf(link, sizeof(link), "%s/Users", target);
                snprintf(sub, sizeof(sub), "%s/Users", dir);
                if (!access(link, F_OK))
                    bind_at(link, sub, MS_REC);
            }
        }
        if (count < 26)
            snprintf(drives[count++], PATH_MAX, "%c%s", letter, target);
    }
    if (d)
        closedir(d);

    /* every distribution's disk, read-only in both views of its drive */
    char line[PATH_MAX + 128], host[PATH_MAX];
    FILE *f = fopen(DISTROS, "re");
    while (f && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *gap = line + strcspn(line, " \t"), *path = gap + strspn(gap, " \t");
        if (!*gap || !*path || line[0] == '#' || find_vhdx(path, host, sizeof(host)))
            continue;
        for (int i = 0; i < count; i++) {
            size_t len = strlen(drives[i] + 1);
            if (strncmp(host, drives[i] + 1, len) || host[len] != '/')
                continue;
            for (int git_bash = 0; git_bash < 2; git_bash++) {
                snprintf(dir, sizeof(dir), git_bash ? "%s/%c%s" : "%s/mnt/%c%s", root, drives[i][0], host + len);
                bind_at(dir, dir, 0);
                read_only(dir);
            }
        }
    }
    if (f)
        fclose(f);
}

/* /dev with the harmless devices only: no disk, no memory, no firmware */
static void container_dev(const char *root)
{
    static const char *const nodes[] = {"null", "zero", "full", "random", "urandom", "tty", "fuse", NULL};
    static const char *const links[][2] = {
        {"/proc/self/fd", "fd"}, {"/proc/self/fd/0", "stdin"}, {"/proc/self/fd/1", "stdout"},
        {"/proc/self/fd/2", "stderr"}, {"pts/ptmx", "ptmx"},
    };
    char dev[PATH_MAX], path[PATH_MAX], host[64];

    snprintf(dev, sizeof(dev), "%s/dev", root);
    make_dirs(dev);
    if (mount("tmpfs", dev, "tmpfs", MS_NOSUID | MS_NOEXEC, "mode=755,size=1m"))
        say("container: /dev: %s", strerror(errno));
    for (int i = 0; nodes[i]; i++) {
        snprintf(host, sizeof(host), "/dev/%s", nodes[i]);
        if (access(host, F_OK))
            continue;
        snprintf(path, sizeof(path), "%s/%s", dev, nodes[i]);
        int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0666);
        if (fd >= 0)
            close(fd);
        bind_at(host, path, 0);
    }
    snprintf(path, sizeof(path), "%s/pts", dev);
    mkdir(path, 0755);
    if (mount("devpts", path, "devpts", MS_NOSUID | MS_NOEXEC, "newinstance,ptmxmode=0666,mode=620"))
        say("container: devpts: %s", strerror(errno));
    snprintf(path, sizeof(path), "%s/shm", dev);
    mkdir(path, 01777);
    mount("tmpfs", path, "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777");
    for (size_t i = 0; i < sizeof(links) / sizeof(*links); i++) {
        snprintf(path, sizeof(path), "%s/%s", dev, links[i][1]);
        symlink(links[i][0], path);
    }
}

static volatile sig_atomic_t init_stop;

static void init_child(int sig)
{
    (void)sig;
}

static void init_term(int sig)
{
    (void)sig;
    init_stop = 1;
}

/* The container's PID 1: it sets the container up from the inside (it is
 * root of the container's namespaces, nothing more), says it is ready on fd
 * 3, then reaps what ends in there until it is told to stop. */
static void container_init(const char *stage, const char *hostname)
{
    char path[PATH_MAX];
    struct sigaction sa = {.sa_handler = init_child};

    prctl(PR_SET_NAME, "init", 0, 0, 0);
    /* not to be traced by root in the container: its memory came from
     * arctic-lxss (fork), and must not be any process's in there */
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);

    /* The staged disk came into this namespace as a copy, and a copy made
     * for a less privileged namespace is locked to its parent: pivot_root
     * will not take it (EINVAL). Bound onto itself here it is a mount of
     * this namespace's own, with the same id-mapping, and can be the root. */
    if (mount(stage, stage, NULL, MS_BIND, NULL)) {
        say("container: %s onto itself: %s", stage, strerror(errno));
        _exit(1);
    }

    snprintf(path, sizeof(path), "%s/proc", stage);
    make_dirs(path);
    if (mount("proc", path, "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL))
        say("container: /proc: %s", strerror(errno));
    /* /sys whole and read-only, its submounts too (efivarfs, debugfs...):
     * they are locked to it in a namespace like this one and cannot be left
     * out, and nothing in there is root of the host to write them anyway */
    snprintf(path, sizeof(path), "%s/sys", stage);
    make_dirs(path);
    bind_at("/sys", path, MS_REC);
    if (lxss_mount_setattr(AT_FDCWD, path, AT_RECURSIVE, MOUNT_ATTR_RDONLY, 0))
        say("container: /sys read-only: %s", strerror(errno));
    container_dev(stage);
    snprintf(path, sizeof(path), "%s/run", stage);
    make_dirs(path);
    mount("tmpfs", path, "tmpfs", MS_NOSUID | MS_NODEV, "mode=755");
    container_drives(stage);
    if (hostname[0])
        sethostname(hostname, strlen(hostname));

    /* the host's tree goes: nothing outside the container is left to reach */
    if (chdir(stage) || syscall(SYS_pivot_root, ".", ".") || umount2(".", MNT_DETACH) || chdir("/")) {
        say("container: pivot_root %s: %s", stage, strerror(errno));
        _exit(1);
    }
    write(3, "r", 1);
    close(3);

    sigaction(SIGCHLD, &sa, NULL);
    sa.sa_handler = init_term;
    sigaction(SIGTERM, &sa, NULL);
    while (!init_stop) {
        while (waitpid(-1, NULL, WNOHANG) > 0)
            ;
        pause();
    }
    kill(-1, SIGKILL); /* everything in the container, as WSL's terminate does */
    _exit(0);
}

/* the PID 1 of a distribution's running container, or 0 */
static pid_t container_running(const char *name)
{
    char path[PATH_MAX], ours[64] = "", theirs[64] = "";
    pid_t pid;

    snprintf(path, sizeof(path), ROOTS "/%s.init", name);
    pid = (pid_t)read_number(path);
    if (pid <= 0 || !running(pid))
        return 0;
    /* still that container's: a pid the host reused is in the host's user namespace */
    snprintf(path, sizeof(path), "/proc/%d/ns/user", (int)pid);
    if (readlink(path, theirs, sizeof(theirs) - 1) <= 0 || readlink("/proc/self/ns/user", ours, sizeof(ours) - 1) <= 0 ||
        !strcmp(ours, theirs))
        return 0;
    return pid;
}

static int write_map(pid_t pid, const char *which, unsigned int outside)
{
    char path[64], text[128];
    int fd, n;

    snprintf(path, sizeof(path), "/proc/%d/%s", (int)pid, which);
    n = snprintf(text, sizeof(text), "0 %u 1\n1 %u %u\n", outside, SUBID_BASE + 1, SUBID_COUNT);
    if ((fd = open(path, O_WRONLY | O_CLOEXEC)) < 0)
        return -1;
    int ok = write(fd, text, (size_t)n) == n;
    close(fd);
    return ok ? 0 : -1;
}

/* Starts a distribution's container on its mounted disk (attach): a user
 * namespace whose root is nt, the disk id-mapped into it (an ext4 written
 * as root by WSL is root's in there too), then the other namespaces and its
 * PID 1. The PID 1, or 0 with err set. */
static pid_t start_container(const char *name, char *err, size_t err_len)
{
    char root[PATH_MAX], stage[PATH_MAX], hostname[256] = "", byte;
    int up[2], down[2], ready[2];   /* the steps, back and forth; the init's "ready" on a pipe of its own */
    struct passwd *pw = getpwnam(NT_USER);
    uid_t nt_uid = pw ? pw->pw_uid : 1000;
    gid_t nt_gid = pw ? pw->pw_gid : 1000;
    pid_t c, init = 0;

    snprintf(root, sizeof(root), ROOTS "/%s", name);
    snprintf(stage, sizeof(stage), CONTAINERS "/%s", name);
    gethostname(hostname, sizeof(hostname) - 1);
    if (pipe2(up, O_CLOEXEC) || pipe2(down, O_CLOEXEC) || pipe2(ready, O_CLOEXEC)) {
        snprintf(err, err_len, "pipe: %s", strerror(errno));
        return 0;
    }
    if (!(c = fork())) {
        close(up[0]);
        close(down[1]);
        close(ready[0]);
        /* nothing of arctic-lxss goes in: its socket, the gate's shared memory */
        if (gate_shm)
            munmap(gate_shm, sizeof(*gate_shm));
        if (unshare(CLONE_NEWUSER) || write(up[1], "u", 1) != 1 || read(down[0], &byte, 1) != 1 || byte != 'm')
            _exit(1);
        if (setresgid(0, 0, 0) || setresuid(0, 0, 0) || setgroups(0, NULL))
            _exit(1);
        if (unshare(CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWIPC | CLONE_NEWUTS | CLONE_NEWCGROUP) ||
            mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) || write(up[1], "n", 1) != 1)
            _exit(1);
        pid_t i = fork();
        if (!i) {
            int null = open("/dev/null", O_RDWR | O_CLOEXEC);
            dup2(ready[1], 3); /* "ready" goes here */
            if (null >= 0) { /* stderr stays arctic-lxss's: what goes wrong in there is in the host's log */
                dup2(null, 0);
                dup2(null, 1);
            }
            syscall(SYS_close_range, 4, ~0U, 0);
            container_init(stage, hostname);
        }
        close(ready[1]);
        write(up[1], &i, sizeof(i));
        _exit(i > 0 ? 0 : 1);
    }
    close(up[1]);
    close(down[0]);
    close(ready[1]); /* the init's is the only one left: its end is a failed start */
    if (c < 0) {
        snprintf(err, err_len, "fork: %s", strerror(errno));
        goto done;
    }
    if (read(up[0], &byte, 1) != 1 || byte != 'u' || write_map(c, "uid_map", nt_uid) || write_map(c, "gid_map", nt_gid)) {
        snprintf(err, err_len, "%s: the container's user namespace: %s", name, strerror(errno));
        goto done;
    }
    /* the disk, id-mapped through the container's user namespace, staged
     * here for the container's copy of the mounts to take */
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/ns/user", (int)c);
    int userns = open(path, O_RDONLY | O_CLOEXEC);
    int tree = (int)syscall(SYS_open_tree, AT_FDCWD, root, OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC);
    make_dirs(stage);
    if (userns < 0 || tree < 0 || lxss_mount_setattr(tree, "", AT_EMPTY_PATH, MOUNT_ATTR_IDMAP, userns) ||
        syscall(SYS_move_mount, tree, "", AT_FDCWD, stage, MOVE_MOUNT_F_EMPTY_PATH)) {
        snprintf(err, err_len, "%s: the disk for the container: %s", name, strerror(errno));
        if (userns >= 0)
            close(userns);
        if (tree >= 0)
            close(tree);
        goto done;
    }
    close(userns);
    close(tree);
    if (write(down[1], "m", 1) != 1 || read(up[0], &byte, 1) != 1 || byte != 'n') {
        snprintf(err, err_len, "%s: the container did not start", name);
        umount2(stage, MNT_DETACH);
        goto done;
    }
    /* the container has its own copy: the host keeps none. The directory
     * stays until the container has left it (pivot_root): /run is one
     * tmpfs for every namespace, and removing a mount point detaches the
     * mount in all of them, the container's copy too */
    umount2(stage, MNT_DETACH);
    if (read(up[0], &init, sizeof(init)) != sizeof(init) || init <= 0 ||
        read(ready[0], &byte, 1) != 1 || byte != 'r') {
        rmdir(stage);
        snprintf(err, err_len, "%s: the container did not come up", name);
        if (init > 0)
            kill(init, SIGKILL);
        init = 0;
        goto done;
    }
    rmdir(stage); /* the container is on its own root now */
    snprintf(path, sizeof(path), ROOTS "/%s.init", name);
    char number[32];
    snprintf(number, sizeof(number), "%d", (int)init);
    write_text(path, number);
    say("%s: container up (init %d)", name, (int)init);

done:
    close(up[0]);
    close(down[1]);
    close(ready[0]);
    if (c > 0) {
        if (!init)
            kill(c, SIGKILL); /* it may be waiting for a step that will not come */
        waitpid(c, NULL, 0);
    }
    return init;
}

/* A command's child process, joining the distribution's container (its
 * namespaces, as root of it), then running the command in it */
static void exec_child(const char *root, const char *user, const char *cwd, int git_bash, int argc, char **argv,
                       int envc, char **env, const char *distro);

static void container_child(pid_t init, const char *user, const char *cwd, int git_bash, int argc, char **argv,
                            int envc, char **env, const char *distro)
{
    static const struct { const char *name; int type; } spaces[] = {
        {"user", CLONE_NEWUSER}, {"mnt", CLONE_NEWNS}, {"pid", CLONE_NEWPID},
        {"ipc", CLONE_NEWIPC},   {"uts", CLONE_NEWUTS}, {"cgroup", CLONE_NEWCGROUP},
    };
    int fds[6], status;
    char path[64];

    setsid(); /* one process group for the session's signals, the command's too */
    if (gate_shm)
        munmap(gate_shm, sizeof(*gate_shm));
    for (int i = 0; i < 6; i++) {
        snprintf(path, sizeof(path), "/proc/%d/ns/%s", (int)init, spaces[i].name);
        if ((fds[i] = open(path, O_RDONLY | O_CLOEXEC)) < 0) {
            fprintf(stderr, "wsl: the container of %s: %s\n", distro, strerror(errno));
            _exit(126);
        }
    }
    for (int i = 0; i < 6; i++)
        if (setns(fds[i], spaces[i].type)) {
            fprintf(stderr, "wsl: into the container of %s (%s): %s\n", distro, spaces[i].name, strerror(errno));
            _exit(126);
        }
    if (setresgid(0, 0, 0) || setresuid(0, 0, 0) || setgroups(0, NULL)) {
        fprintf(stderr, "wsl: root of the container of %s: %s\n", distro, strerror(errno));
        _exit(126);
    }
    pid_t g = fork(); /* the PID namespace is the children's */
    if (!g)
        exec_child(NULL, user, cwd, git_bash, argc, argv, envc, env, distro);
    if (g < 0)
        _exit(126);
    while (waitpid(g, &status, 0) < 0 && errno == EINTR)
        ;
    _exit(WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
}

static int free_nbd(void)
{
    char path[64];

    for (int i = 0; i < MAX_NBD; i++) {
        snprintf(path, sizeof(path), "/sys/block/nbd%d/pid", i);
        if (access(path, F_OK)) {
            snprintf(path, sizeof(path), "/sys/block/nbd%d", i);
            if (!access(path, F_OK))
                return i;
        }
    }
    return -1;
}

static long read_number(const char *path)
{
    char text[32] = "";
    int fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd >= 0) {
        if (read(fd, text, sizeof(text) - 1) < 0)
            text[0] = 0;
        close(fd);
    }
    return strtol(text, NULL, 10);
}

static void write_text(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

    if (fd >= 0) {
        write(fd, text, strlen(text));
        close(fd);
    }
}

/* ---- the log of a VHDX ----
 *
 * qemu's search of the log (block/vhdx-log.c, vhdx_log_search) never ends
 * when the active sequence wraps round the end of the circular log and ends
 * right before its own start: the scan starts there again and again, and
 * qemu-nbd spins for ever. Windows leaves such logs. So a log that is not
 * empty is replayed here first, as the VHDX specification has it: from the
 * tail the head entry names up to the head, then a header with an empty log
 * in the other slot. Everything is checked before anything is written. */

#define VHDX_SECTOR 4096
#define VHDX_MAX_LOG (64u << 20)

static uint64_t get64le(const unsigned char *p)
{
    uint64_t v = 0;

    for (int i = 7; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}

static uint32_t get32le(const unsigned char *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put64le(unsigned char *p, uint64_t v)
{
    for (int i = 0; i < 8; i++, v >>= 8)
        p[i] = (unsigned char)v;
}

/* CRC-32C, with the checksum field at zero_at counted as 0 */
static uint32_t crc32c(const unsigned char *buf, size_t len, size_t zero_at)
{
    static uint32_t table[256];
    uint32_t c = 0xffffffff;

    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t t = i;
            for (int k = 0; k < 8; k++)
                t = t & 1 ? 0x82f63b78 ^ (t >> 1) : t >> 1;
            table[i] = t;
        }
    for (size_t i = 0; i < len; i++)
        c = table[(c ^ (i >= zero_at && i < zero_at + 4 ? 0 : buf[i])) & 0xff] ^ (c >> 8);
    return ~c;
}

static int pread_all(int fd, void *buf, size_t len, off_t off)
{
    for (size_t done = 0; done < len;) {
        ssize_t n = pread(fd, (char *)buf + done, len - done, off + (off_t)done);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

static int pwrite_all(int fd, const void *buf, size_t len, off_t off)
{
    for (size_t done = 0; done < len;) {
        ssize_t n = pwrite(fd, (const char *)buf + done, len - done, off + (off_t)done);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

/* The entry at byte at of the log, read round the circular buffer, when it
 * is a whole and valid one of this log: its length, else 0 */
static uint32_t log_entry(const unsigned char *log, uint32_t log_len, uint32_t at, const unsigned char *guid,
                          unsigned char **entry)
{
    uint32_t len = get32le(log + at + 8);
    unsigned char *e;

    if (memcmp(log + at, "loge", 4) || !len || len > log_len || len % VHDX_SECTOR)
        return 0;
    if (!(e = malloc(len)))
        return 0;
    for (uint32_t i = 0; i < len; i += VHDX_SECTOR)
        memcpy(e + i, log + (at + i) % log_len, VHDX_SECTOR);
    if (memcmp(e + 32, guid, 16) || get32le(e + 4) != crc32c(e, len, 4) ||
        64 + 32 * (uint64_t)get32le(e + 24) > len) {
        free(e);
        return 0;
    }
    *entry = e;
    return len;
}

/* The writes of one entry: checked only, or done */
static int replay_entry(int fd, const unsigned char *e, uint32_t len, int apply)
{
    static const unsigned char zeros[65536];
    uint64_t seq = get64le(e + 16);
    uint32_t count = get32le(e + 24);
    uint32_t data_at = (64 + 32 * count + VHDX_SECTOR - 1) / VHDX_SECTOR * VHDX_SECTOR;
    unsigned char sector[VHDX_SECTOR];
    struct stat st;

    for (uint32_t i = 0; i < count; i++) {
        const unsigned char *d = e + 64 + 32 * i;
        uint64_t offset = get64le(d + 16);

        if (get64le(d + 24) != seq || offset % VHDX_SECTOR)
            return -1;
        if (!memcmp(d, "desc", 4)) {
            const unsigned char *data = e + data_at;
            if (data_at + VHDX_SECTOR > len || memcmp(data, "data", 4) ||
                ((uint64_t)get32le(data + 4) << 32 | get32le(data + 4092)) != seq)
                return -1;
            memcpy(sector, d + 8, 8);               /* the leading bytes */
            memcpy(sector + 8, data + 8, 4084);
            memcpy(sector + 4092, d + 4, 4);        /* the trailing bytes */
            if (apply && pwrite_all(fd, sector, VHDX_SECTOR, (off_t)offset))
                return -1;
            data_at += VHDX_SECTOR;
        } else if (!memcmp(d, "zero", 4)) {
            uint64_t length = get64le(d + 8);
            if (length % VHDX_SECTOR)
                return -1;
            for (uint64_t done = 0; apply && done < length; done += sizeof(zeros))
                if (pwrite_all(fd, zeros, (size_t)(length - done < sizeof(zeros) ? length - done : sizeof(zeros)),
                               (off_t)(offset + done)))
                    return -1;
        } else
            return -1;
    }
    /* the file is as long as the entry says, at least */
    uint64_t last = get64le(e + 56);
    if (apply && !fstat(fd, &st) && (uint64_t)st.st_size < last &&
        ftruncate(fd, (off_t)((last + (1u << 20) - 1) & ~(uint64_t)((1u << 20) - 1))))
        return -1;
    return 0;
}

/* Both slots get the next headers, naming no log: the other slot first, then
 * the current one. One is not enough. qemu-nbd writes both headers again at
 * its first write, each with the log it named when read, so a slot left with
 * the old log's name comes back as the current header. And with a log named
 * qemu refuses every block it has to add (-ENOTSUP, "the log be flushed after
 * every write") after the block is in its table in memory: the write fails,
 * later writes to that block go through, and none of them is on the disk
 * once it is detached. */
static int write_empty_log_headers(int fd, unsigned char *header, int cur)
{
    for (int i = 0; i < 2; i++) {
        put64le(header + 8, get64le(header + 8) + 1);
        memset(header + 48, 0, 16);
        memset(header + 4, 0, 4);
        uint32_t crc = crc32c(header, VHDX_SECTOR, 4);
        for (int k = 0; k < 4; k++)
            header[4 + k] = (unsigned char)(crc >> 8 * k);
        if (pwrite_all(fd, header, VHDX_SECTOR, 0x10000 * (i ? cur + 1 : 2 - cur)) || fsync(fd))
            return -1;
    }
    return 0;
}

/* 0 when the log is empty or has been replayed (or is qemu's to judge),
 * -1 when the disk is better left alone */
static int replay_vhdx_log(const char *path, const char *name, char *err, size_t err_len)
{
    static const unsigned char no_guid[16];
    unsigned char header[2][VHDX_SECTOR], *log = NULL, *head = NULL, *e;
    uint32_t log_len, at, len, starts[VHDX_MAX_LOG / VHDX_SECTOR / 2], count = 0, head_at = 0;
    uint64_t head_seq = 0;
    int fd, cur = -1, ret = -1;

    if ((fd = open(path, O_RDWR | O_CLOEXEC)) < 0)
        return 0;
    for (int i = 0; i < 2; i++) {
        if (pread_all(fd, header[i], VHDX_SECTOR, 0x10000 * (i + 1)) || memcmp(header[i], "head", 4) ||
            get32le(header[i] + 4) != crc32c(header[i], VHDX_SECTOR, 4))
            continue;
        if (cur < 0 || get64le(header[i] + 8) > get64le(header[cur] + 8))
            cur = i;
    }
    log_len = cur < 0 ? 0 : get32le(header[cur] + 68);
    if (cur < 0 || !memcmp(header[cur] + 48, no_guid, 16) || !log_len || log_len % VHDX_SECTOR ||
        log_len > VHDX_MAX_LOG || !(log = malloc(log_len)) ||
        pread_all(fd, log, log_len, (off_t)get64le(header[cur] + 72))) {
        ret = 0;
        goto done;
    }

    /* the head: the valid entry with the highest sequence number */
    for (at = 0; at < log_len; at += VHDX_SECTOR) {
        if (!log_entry(log, log_len, at, header[cur] + 48, &e))
            continue;
        if (!head || get64le(e + 16) > head_seq) {
            free(head);
            head = e;
            head_seq = get64le(e + 16);
            head_at = at;
        } else
            free(e);
    }
    if (!head) {
        /* a log named and nothing in it: the power went between the header
         * and the entry. qemu finds nothing to replay either, and keeps the
         * name, which is what must not stay */
        if (write_empty_log_headers(fd, header[cur], cur)) {
            snprintf(err, err_len, "%s: a header for its disk: %s", name, strerror(errno));
            goto done;
        }
        say("%s: the log of %s was named but empty", name, path);
        ret = 0;
        goto done;
    }

    /* the active sequence, from the tail the head names up to the head */
    at = get32le(head + 12);
    for (uint64_t seq = 0; at % VHDX_SECTOR == 0 && at < log_len && count < sizeof(starts) / sizeof(*starts);) {
        if (!(len = log_entry(log, log_len, at, header[cur] + 48, &e)))
            break;
        uint64_t s = get64le(e + 16);
        int fits = (!count || s == seq + 1) && !replay_entry(fd, e, len, 0);
        free(e);
        if (!fits)
            break;
        starts[count++] = at;
        seq = s;
        if (at == head_at && s == head_seq)
            goto replay;
        at = (at + len) % log_len;
    }
    snprintf(err, err_len, "%s: the log of its disk is damaged", name);
    goto done;

replay:
    for (uint32_t i = 0; i < count; i++) {
        len = log_entry(log, log_len, starts[i], header[cur] + 48, &e);
        int failed = !len || replay_entry(fd, e, len, 1);
        free(e);
        if (failed) {
            snprintf(err, err_len, "%s: replaying the log of its disk: %s", name, strerror(errno));
            goto done;
        }
    }
    if (fsync(fd)) {
        snprintf(err, err_len, "%s: replaying the log of its disk: %s", name, strerror(errno));
        goto done;
    }
    if (write_empty_log_headers(fd, header[cur], cur)) {
        snprintf(err, err_len, "%s: a header for its disk: %s", name, strerror(errno));
        goto done;
    }
    say("%s: replayed %u entries of the log of %s", name, count, path);
    ret = 0;

done:
    free(head);
    free(log);
    close(fd);
    return ret;
}

/* The VHDX on /dev/nbdN, its ext4 under /run/lxss/<name> with /proc, /sys,
 * /dev, the drives and the host's DNS servers */
static int attach(const char *name, const char *vhdx_win, char *err, size_t err_len)
{
    char root[PATH_MAX], vhdx[PATH_MAX], dev[32], arg_dev[48], arg_pid[PATH_MAX + 16], arg_sock[PATH_MAX + 16], path[PATH_MAX];
    int nbd;

    snprintf(root, sizeof(root), ROOTS "/%s", name);
    if (is_mountpoint(root))
        return 0;
    if (find_vhdx(vhdx_win, vhdx, sizeof(vhdx))) {
        snprintf(err, err_len, "%s: no such disk", vhdx_win);
        return -1;
    }
    /* one disk, one distribution: ext4 mounted twice is ext4 lost */
    DIR *d = opendir(ROOTS);
    for (struct dirent *de; d && (de = readdir(d));) {
        char used[PATH_MAX] = "";
        size_t len = strlen(de->d_name);
        if (len < 6 || strcmp(de->d_name + len - 5, ".vhdx"))
            continue;
        snprintf(path, sizeof(path), ROOTS "/%s", de->d_name);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        ssize_t n = fd >= 0 ? read(fd, used, sizeof(used) - 1) : -1;
        if (fd >= 0)
            close(fd);
        used[n > 0 ? n : 0] = 0;
        if (!strcmp(used, vhdx)) {
            snprintf(err, err_len, "%s is in use by %.*s", vhdx_win, (int)(len - 5), de->d_name);
            closedir(d);
            return -1;
        }
    }
    if (d)
        closedir(d);
    if (replay_vhdx_log(vhdx, name, err, err_len))
        return -1;
    if (access("/sys/module/nbd", F_OK)) {
        char *modprobe[] = {"/usr/bin/modprobe", "nbd", "nbds_max=16", "max_part=0", NULL};
        run(modprobe);
        /* what the builds in there use (losetup, ntfs-3g): modprobe in the
         * distribution finds no modules of this kernel */
        char *more[] = {"/usr/bin/modprobe", "-a", "loop", "fuse", NULL};
        run(more);
    }
    if ((nbd = free_nbd()) < 0) {
        snprintf(err, err_len, "no free NBD device");
        return -1;
    }
    snprintf(dev, sizeof(dev), "/dev/nbd%d", nbd);
    snprintf(arg_dev, sizeof(arg_dev), "--connect=%s", dev);
    snprintf(arg_pid, sizeof(arg_pid), "--pid-file=" ROOTS "/%s.pid", name);
    /* its own socket would go to /var/lock, on the host's read-only image */
    snprintf(arg_sock, sizeof(arg_sock), "--socket=" ROOTS "/%s.sock", name);
    char *qemu_nbd[] = {"/usr/bin/qemu-nbd", arg_dev, "--format=vhdx", "--cache=writeback", "--discard=unmap",
                        "--fork",            arg_pid, arg_sock,        vhdx,                NULL};
    /* it holds the lock of every distribution meanwhile: a qemu-nbd that
     * never gets the disk attached must not hang every wsl.exe after it */
    int attached = run_for(qemu_nbd, ATTACH_SECONDS);
    if (attached) {
        if (attached == -2) {
            end_by_argument(arg_sock);
            snprintf(err, err_len, "qemu-nbd did not attach %s in %d s", vhdx_win, ATTACH_SECONDS);
        } else
            snprintf(err, err_len, "qemu-nbd could not attach %s", vhdx_win);
        snprintf(path, sizeof(path), ROOTS "/%s.sock", name);
        unlink(path);
        return -1;
    }
    snprintf(path, sizeof(path), "/sys/block/nbd%d/size", nbd);
    for (int i = 0; i < 100 && read_number(path) <= 0; i++)
        usleep(100000);
    snprintf(path, sizeof(path), ROOTS "/%s.nbd", name);
    write_text(path, dev);
    snprintf(path, sizeof(path), ROOTS "/%s.vhdx", name);
    write_text(path, vhdx);

    make_dirs(root);
    if (mount(dev, root, "ext4", MS_NOATIME, NULL)) {
        snprintf(err, err_len, "%s: ext4: %s", vhdx_win, strerror(errno));
        char *detach[] = {"/usr/bin/qemu-nbd", "--disconnect", dev, NULL};
        run(detach);
        return -1;
    }
    /* Only the disk is mounted here, in the host's namespace. /proc, /sys,
     * /dev and the drives are each session's own: a container's, or a
     * hamam.exe command's in a mount namespace of its own (host_child), gone
     * with it. Nothing of the host stays bound under /run/lxss for an
     * rm -rf there to reach (12.11.2026: /dev and the firmware's boot
     * entries were erased through such binds). */

    /* WSL writes it for each start as well */
    char resolv[4096];
    int in = open("/etc/resolv.conf", O_RDONLY | O_CLOEXEC);
    ssize_t n = in >= 0 ? read(in, resolv, sizeof(resolv) - 1) : -1;
    if (in >= 0)
        close(in);
    if (n > 0) {
        resolv[n] = 0;
        snprintf(path, sizeof(path), "%s/etc/resolv.conf", root);
        unlink(path);
        write_text(path, resolv);
    }
    say("%s: %s on %s, mounted", name, vhdx_win, dev);
    return 0;
}

/* /proc/self/mountinfo escapes blanks as \040 */
static void unescape(char *s)
{
    char *out = s;

    for (; *s; s++) {
        if (s[0] == '\\' && s[1] >= '0' && s[1] <= '7' && s[2] && s[3]) {
            *out++ = (char)((s[1] - '0') * 64 + (s[2] - '0') * 8 + (s[3] - '0'));
            s += 3;
        } else
            *out++ = *s;
    }
    *out = 0;
}

static void kill_inside(const char *root, int sig)
{
    char link[64], where[PATH_MAX];
    size_t len = strlen(root);
    DIR *proc = opendir("/proc");

    for (struct dirent *de; proc && (de = readdir(proc));) {
        if (!isdigit((unsigned char)de->d_name[0]))
            continue;
        snprintf(link, sizeof(link), "/proc/%s/root", de->d_name);
        ssize_t n = readlink(link, where, sizeof(where) - 1);
        if (n <= 0)
            continue;
        where[n] = 0;
        if (!strncmp(where, root, len) && (!where[len] || where[len] == '/'))
            kill((pid_t)atoi(de->d_name), sig);
    }
    if (proc)
        closedir(proc);
}

static int anything_inside(const char *root)
{
    char link[64], where[PATH_MAX];
    size_t len = strlen(root);
    DIR *proc = opendir("/proc");
    int found = 0;

    for (struct dirent *de; proc && !found && (de = readdir(proc));) {
        if (!isdigit((unsigned char)de->d_name[0]))
            continue;
        snprintf(link, sizeof(link), "/proc/%s/root", de->d_name);
        ssize_t n = readlink(link, where, sizeof(where) - 1);
        if (n > 0) {
            where[n] = 0;
            found = !strncmp(where, root, len) && (!where[len] || where[len] == '/');
        }
    }
    if (proc)
        closedir(proc);
    return found;
}

/* a process that has not ended yet (a zombie has) */
static int running(pid_t pid)
{
    char path[64], stat[256] = "";
    int fd;

    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
        return 0;
    ssize_t n = read(fd, stat, sizeof(stat) - 1);
    close(fd);
    const char *state = n > 0 ? strrchr(stat, ')') : NULL;
    return state && state[1] && state[2] != 'Z';
}

/* The distribution's processes end, its mounts go, deepest first, and
 * qemu-nbd writes the VHDX out and exits */
static void detach(const char *name)
{
    char root[PATH_MAX], path[PATH_MAX], dev[32] = "", line[3 * PATH_MAX];
    char *mounts[512];
    int count = 0;

    snprintf(root, sizeof(root), ROOTS "/%s", name);
    /* the container: its PID 1 takes every process in it along, and its
     * copy of the mounts (the disk) goes with the last of them */
    pid_t init = container_running(name);
    if (init) {
        kill(init, SIGTERM);
        for (int i = 0; i < 30 && running(init); i++)
            usleep(100000);
        if (running(init))
            kill(init, SIGKILL);
        for (int i = 0; i < 30 && running(init); i++) {
            while (waitpid(-1, NULL, WNOHANG) > 0) /* ours: we are its subreaper */
                ;
            usleep(100000);
        }
    }
    snprintf(path, sizeof(path), ROOTS "/%s.init", name);
    unlink(path);
    snprintf(path, sizeof(path), CONTAINERS "/%s", name);
    if (is_mountpoint(path))
        umount2(path, MNT_DETACH);
    rmdir(path);
    /* hamam.exe's commands, chrooted there */
    kill_inside(root, SIGTERM);
    for (int i = 0; i < 30 && anything_inside(root); i++)
        usleep(100000);
    kill_inside(root, SIGKILL);
    for (int i = 0; i < 20 && anything_inside(root); i++)
        usleep(100000);

    FILE *f = fopen("/proc/self/mountinfo", "re");
    size_t len = strlen(root);
    while (f && count < 512 && fgets(line, sizeof(line), f)) {
        char *field = line, *at = NULL;
        for (int i = 0; i < 4 && field; i++)
            field = strchr(field, ' ') ? strchr(field, ' ') + 1 : NULL;
        if (!field || !(at = strchr(field, ' ')))
            continue;
        *at = 0;
        unescape(field);
        if (!strncmp(field, root, len) && (!field[len] || field[len] == '/'))
            mounts[count++] = strdup(field);
    }
    if (f)
        fclose(f);
    sync();
    for (int i = count - 1; i >= 0; i--) {
        if (umount2(mounts[i], 0) && umount2(mounts[i], MNT_DETACH))
            say("%s: unmount %s: %s", name, mounts[i], strerror(errno));
        free(mounts[i]);
    }

    snprintf(path, sizeof(path), ROOTS "/%s.nbd", name);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t n = read(fd, dev, sizeof(dev) - 1);
        dev[n > 0 ? n : 0] = 0;
        close(fd);
    }
    unlink(path);
    snprintf(path, sizeof(path), ROOTS "/%s.pid", name);
    pid_t server = (pid_t)read_number(path);
    if (dev[0]) {
        char *disconnect[] = {"/usr/bin/qemu-nbd", "--disconnect", dev, NULL};
        run(disconnect);
    }
    /* it flushes the VHDX on its way out */
    for (int i = 0; server > 0 && i < 300 && running(server); i++) {
        while (waitpid(-1, NULL, WNOHANG) > 0) /* it is ours: we are its subreaper */
            ;
        usleep(100000);
    }
    if (server > 0 && running(server))
        say("%s: qemu-nbd did not end", name);
    unlink(path);
    snprintf(path, sizeof(path), ROOTS "/%s.sock", name);
    unlink(path);
    snprintf(path, sizeof(path), ROOTS "/%s.vhdx", name);
    unlink(path);
    rmdir(root);
    say("%s: detached", name);
}

static void detach_all(void)
{
    DIR *d = opendir(ROOTS);
    char path[PATH_MAX];

    for (struct dirent *de; d && (de = readdir(d));) {
        struct stat st;
        snprintf(path, sizeof(path), ROOTS "/%s", de->d_name);
        if (de->d_name[0] != '.' && !lstat(path, &st) && S_ISDIR(st.st_mode))
            detach(de->d_name);
    }
    if (d)
        closedir(d);
}

static int lock_roots(void)
{
    int fd = open(LOCK, O_RDWR | O_CREAT | O_CLOEXEC, 0600);

    if (fd >= 0)
        flock(fd, LOCK_EX);
    return fd;
}

/* ---- one connection ---- */

static int read_all(int fd, void *buf, size_t len)
{
    for (size_t done = 0; done < len;) {
        ssize_t n = recv(fd, (char *)buf + done, len - done, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

static int send_all(int fd, const void *buf, size_t len)
{
    for (size_t done = 0; done < len;) {
        ssize_t n = send(fd, (const char *)buf + done, len - done, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

static int send_frame(int fd, int type, const void *data, uint32_t len)
{
    unsigned char head[5] = {(unsigned char)type, len & 0xff, (len >> 8) & 0xff, (len >> 16) & 0xff, len >> 24};

    return send_all(fd, head, sizeof(head)) || (len && send_all(fd, data, len)) ? -1 : 0;
}

static void send_error(int fd, const char *fmt, ...)
{
    char msg[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    send_frame(fd, F_ERROR, msg, (uint32_t)strlen(msg));
}

static uint32_t get32(const unsigned char *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static char *read_string(int fd)
{
    unsigned char len[4];
    char *s;

    if (read_all(fd, len, 4) || get32(len) > (1u << 20) || !(s = malloc(get32(len) + 1)))
        return NULL;
    if (read_all(fd, s, get32(len))) {
        free(s);
        return NULL;
    }
    s[get32(len)] = 0;
    return s;
}

/* ---- who is asking (docs/linux-gate.md, "Модель доступу") ---- */

#define NTREE_MAX 32

struct ntree_entry {
    pid_t unix_pid;
    unsigned int win32_id;
    long long start_time;
    char image[512];
};

/* The pid and its NT-level ancestors, from wineserver's own process tree
 * (runtime/wine/patches/0090-...): on Linux every Wine process ends up a
 * child of init, so /proc/<pid>/stat's ppid chain is never this. Closest
 * first; stops at the first ancestor that is gone. 0 when wineserver has
 * nothing for this pid (it always does for a real Wine process). */
static int query_ntree(pid_t unix_pid, struct ntree_entry *entries, int max_entries)
{
    static const char name[] = "arctic-wineserver-ntree";
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    struct timeval tv = {2, 0};
    char reply[16384];
    unsigned int want = (unsigned int)unix_pid;
    ssize_t total = 0, n;
    int fd, count = 0;
    char *line, *nl;

    if ((fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0)
        return 0;
    memcpy(addr.sun_path + 1, name, sizeof(name) - 1);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&addr, (socklen_t)(offsetof(struct sockaddr_un, sun_path) + sizeof(name))) ||
        send_all(fd, &want, sizeof(want))) {
        close(fd);
        return 0;
    }
    while (total < (ssize_t)sizeof(reply) - 1 &&
           (n = recv(fd, reply + total, sizeof(reply) - 1 - (size_t)total, 0)) > 0)
        total += n;
    close(fd);
    reply[total > 0 ? total : 0] = 0;

    for (line = reply; count < max_entries && (nl = strchr(line, '\n')); line = nl + 1) {
        int consumed = -1;

        *nl = 0;
        if (sscanf(line, "%d %u %lld %n", &entries[count].unix_pid, &entries[count].win32_id,
                   &entries[count].start_time, &consumed) >= 3 && consumed >= 0)
            snprintf(entries[count].image, sizeof(entries[count].image), "%s", line + consumed);
        else
            entries[count].image[0] = 0;
        count++;
    }
    return count;
}

static const char *image_basename(const char *path)
{
    const char *base = path;

    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\')
            base = p + 1;
    return base;
}

/* NT's executive, ntexec (it was wineserver before the names of 2026-11-16) */
static int is_server_image(const char *image)
{
    const char *base = image_basename(image);
    return !strcasecmp(base, "ntexec") || !strcasecmp(base, "wineserver");
}

/* never given a grant: what starts them (the shell, a service) must not
 * hand every program it runs the grant it builds up for itself */
static int is_system_image(const char *basename)
{
    static const char *const system[] = {
        "explorer.exe", "svchost.exe", "services.exe", "winlogon.exe", "wininit.exe", "csrss.exe",
        "rpcss.exe",    "windevice.exe", "ntexec", "wineserver", "arctic-init", NULL,
    };

    for (int i = 0; system[i]; i++)
        if (!strcasecmp(basename, system[i]))
            return 1;
    return 0;
}

/* the shells and consoles programs run commands through: each command of an
 * agent is a new one of them */
static int is_shell_image(const char *basename)
{
    static const char *const shells[] = {
        "cmd.exe", "pwsh.exe", "powershell.exe", "conhost.exe", "openconsole.exe", "sh.exe", NULL,
    };

    for (int i = 0; shells[i]; i++)
        if (!strcasecmp(basename, shells[i]))
            return 1;
    return 0;
}

/* The process-source: past wsl.exe/bash.exe themselves, and past the shells
 * a program runs its commands in (claude.exe -> pwsh.exe -> wsl.exe is
 * claude.exe: one question for the agent, not one for every command), the
 * first program. A shell whose parent is a system process is the program
 * (cmd.exe started from Explorer); a system process never is: then it is
 * wsl.exe/bash.exe's own window, the one it opened, so that a shortcut in
 * Start does not give Linux to everything Explorer starts. */
static void find_origin(const struct ntree_entry *chain, int n, struct ntree_entry *origin)
{
    int i = 0;

    while (i < n && (!strcasecmp(image_basename(chain[i].image), "wsl.exe") ||
                      !strcasecmp(image_basename(chain[i].image), "bash.exe") ||
                      !strcasecmp(image_basename(chain[i].image), "hamam.exe")))
        i++;
    while (i + 1 < n && is_shell_image(image_basename(chain[i].image)) &&
           !is_system_image(image_basename(chain[i + 1].image)))
        i++;
    *origin = (i < n && !is_system_image(image_basename(chain[i].image))) ? chain[i] : chain[0];
}


/* Every process that holds this socket: /proc/net/tcp names no owner, only
 * the inode, and /proc/<pid>/fd links one as "socket:[<inode>]". There is
 * more than one holder: a socket of a Wine program is made by wineserver and
 * passed on, and both of them have it open, so the program itself is not
 * always the first one found. */
#define MAX_HOLDERS 8

static int pids_by_socket_inode(unsigned long inode, pid_t *pids, int max_pids)
{
    char target[48], link[PATH_MAX], dest[PATH_MAX], fd_dir[64];
    DIR *proc = opendir("/proc");
    int count = 0;

    snprintf(target, sizeof(target), "socket:[%lu]", inode);
    for (struct dirent *de; proc && count < max_pids && (de = readdir(proc));) {
        DIR *fds;

        if (!isdigit((unsigned char)de->d_name[0]))
            continue;
        snprintf(fd_dir, sizeof(fd_dir), "/proc/%s/fd", de->d_name);
        fds = opendir(fd_dir);
        for (struct dirent *fe; fds && (fe = readdir(fds));) {
            ssize_t n;

            if (fe->d_name[0] == '.')
                continue;
            snprintf(link, sizeof(link), "%s/%s", fd_dir, fe->d_name);
            if ((n = readlink(link, dest, sizeof(dest) - 1)) > 0) {
                dest[n] = 0;
                if (!strcmp(dest, target)) {
                    pids[count++] = (pid_t)atoi(de->d_name);
                    break;
                }
            }
        }
        if (fds)
            closedir(fds);
    }
    if (proc)
        closedir(proc);
    return count;
}

static int peer_pids(int sock, pid_t *pids, int max_pids)
{
    struct sockaddr_in local = {0}, peer = {0};
    socklen_t len = sizeof(local);
    unsigned int local_port, peer_port;
    char line[512];
    FILE *f;
    int count = 0;

    if (getsockname(sock, (struct sockaddr *)&local, &len) || getpeername(sock, (struct sockaddr *)&peer, &len))
        return 0;
    local_port = ntohs(local.sin_port);
    peer_port = ntohs(peer.sin_port);
    if (!(f = fopen("/proc/net/tcp", "re")))
        return 0;
    if (!fgets(line, sizeof(line), f)) { /* the header line */
        fclose(f);
        return 0;
    }
    while (fgets(line, sizeof(line), f)) {
        unsigned int lport, rport, st;
        unsigned long inode;

        if (sscanf(line, " %*d: %*x:%x %*x:%x %x %*x:%*x %*x:%*x %*x %*d %*d %lu", &lport, &rport, &st, &inode) ==
                4 &&
            st == 1 /* TCP_ESTABLISHED */ && lport == peer_port && rport == local_port) {
            count = pids_by_socket_inode(inode, pids, max_pids);
            break;
        }
    }
    fclose(f);
    return count;
}

/* the one of them that is a program (not wineserver, which holds the sockets
 * it made for everybody), or the first that is anything at all */
static pid_t peer_pid(int sock)
{
    pid_t pids[MAX_HOLDERS];
    int count = peer_pids(sock, pids, MAX_HOLDERS);

    for (int i = 0; i < count; i++) {
        struct ntree_entry chain[NTREE_MAX];

        if (query_ntree(pids[i], chain, 1) && !is_server_image(chain[0].image))
            return pids[i];
    }
    return count ? pids[0] : -1;
}

/* field 22 of /proc/<pid>/stat (starttime, clock ticks since boot): only a
 * fallback for a peer wineserver's ntree socket had nothing for, since that
 * never happens for a real Wine process */
static long long process_start_time_fallback(pid_t pid)
{
    char path[64], buf[4096], *p;
    int fd;
    ssize_t n;

    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
        return -1;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = 0;
    if (!(p = strrchr(buf, ')')))
        return -1;
    p += 2; /* ')', then the space before field 3 (state): now at field 3 */
    for (int i = 0; i < 19; i++) { /* fields 3..21, 19 of them, before field 22 */
        if (!(p = strchr(p, ' ')))
            return -1;
        p++;
    }
    return strtoll(p, NULL, 10);
}

static void join_argv(unsigned int argc, char *const argv[], char *out, size_t size)
{
    size_t len = 0;

    out[0] = 0;
    for (unsigned int i = 0; i < argc && len + 1 < size; i++) {
        int n = snprintf(out + len, size - len, "%s%s", len ? " " : "", argv[i]);
        if (n > 0)
            len += (size_t)n;
    }
}

/* A \n-terminated line from fd within timeout_s overall, or 0 on timeout,
 * EOF, error or overflow (consent.exe's messages are short and few, so a
 * byte-at-a-time read costs nothing here) */
static int read_line_timeout(int fd, char *buf, size_t size, int timeout_s)
{
    struct timespec deadline, now;
    size_t len = 0;

    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout_s;
    while (len + 1 < size) {
        struct pollfd pfd = {fd, POLLIN, 0};
        ssize_t n;
        int ms;

        clock_gettime(CLOCK_MONOTONIC, &now);
        ms = (int)((deadline.tv_sec - now.tv_sec) * 1000 + (deadline.tv_nsec - now.tv_nsec) / 1000000);
        if (ms <= 0 || poll(&pfd, 1, ms) <= 0)
            return 0;
        n = read(fd, buf + len, 1);
        if (n <= 0)
            return 0;
        if (buf[len] == '\n') {
            buf[len] = 0;
            return 1;
        }
        len++;
    }
    return 0;
}

enum consent_result { CONSENT_DENIED, CONSENT_GRANTED };

/* ---- the consent window, asked for as Windows asks for it ----
 *
 * The window belongs to the Application Information service (appinfo.dll in
 * svchost, runtime/wine/modules/dlls/appinfo), as in Windows: root starts no
 * Windows program itself, it asks the service over 127.0.0.1 and the service
 * puts consent.exe on the screen and carries the lines between the two. The
 * service decides nothing: root checks the password and root records the
 * answer.
 *
 *   C:\ProgramData\Arctic\Appinfo\session   "port token", written by the service
 */
static int appinfo_session(unsigned short *port, char *tok, size_t tok_len)
{
    FILE *f = fopen(APPINFO_SESSION, "re");
    unsigned int read_port = 0;
    char read_token[256];
    int ok;

    if (!f)
        return -1;
    ok = fscanf(f, "%u %255s", &read_port, read_token) == 2 && read_port && read_port < 65536 &&
         strlen(read_token) < tok_len;
    fclose(f);
    if (!ok)
        return -1;
    *port = (unsigned short)read_port;
    snprintf(tok, tok_len, "%s", read_token);
    return 0;
}

/* Only the service may show that window. Any Windows program could write its
 * own port and token into the session file (C: is mounted for nt) and put up
 * a window of its own that asks for the Linux password; so the process behind
 * the connection must be svchost.exe started by services.exe, which nothing
 * of uid nt can be.
 *
 * A connection still waiting in the listener's queue belongs to no process
 * yet (/proc/net/tcp gives it no inode), so this waits a moment for the
 * service to accept it; the service is already listening, so it is about to. */
static int is_appinfo_service(int sock)
{
    for (int attempt = 0; attempt < 30; attempt++) {
        pid_t pids[MAX_HOLDERS];
        int count = peer_pids(sock, pids, MAX_HOLDERS), known = 0;

        for (int p = 0; p < count; p++) {
            struct ntree_entry chain[NTREE_MAX];
            int n = query_ntree(pids[p], chain, NTREE_MAX);

            /* wineserver holds the sockets it makes for everybody: it says
             * nothing about whose connection this is */
            if (!n || is_server_image(chain[0].image)) continue;
            known = 1;
            if (strcasecmp(image_basename(chain[0].image), "svchost.exe"))
                continue;
            for (int i = 1; i < n; i++)
                if (!strcasecmp(image_basename(chain[i].image), "services.exe"))
                    return 1;
        }
        if (known)
            return 0; /* somebody else is listening there */
        usleep(100000); /* not accepted yet: the socket has no owner */
    }
    return 0;
}

/* A connection to the service with the token already sent, or -1. The first
 * request of a start may come before the services are up, so it waits a
 * while for it, as any program waiting for a service does. */
static int appinfo_connect(void)
{
    struct timeval tv = {10, 0};

    for (int attempt = 0; attempt < APPINFO_WAIT_S * 2; attempt++) {
        struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        char tok[256], line[300];
        unsigned short port;
        int fd, n;

        if (attempt)
            usleep(500000);
        if (appinfo_session(&port, tok, sizeof(tok)))
            continue;
        addr.sin_port = htons(port);
        if ((fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0)
            continue;
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr))) {
            close(fd);
            continue;
        }
        /* the token goes only to the service itself, and only once it is known
         * to be the service: nothing else gets so much as a window */
        if (!is_appinfo_service(fd)) {
            say("gate: 127.0.0.1:%u is not the Application Information service", port);
            close(fd);
            return -1;
        }
        n = snprintf(line, sizeof(line), "%s\n", tok);
        if (n < 0 || send_all(fd, line, (size_t)n)) {
            close(fd);
            return -1;
        }
        return fd;
    }
    return -1;
}

/* a field of the prompt line: no tab or line break of its own */
static void prompt_field(char *out, size_t len, const char *in)
{
    snprintf(out, len, "%s", in ? in : "");
    for (char *p = out; *p; p++)
        if (*p == '\t' || *p == '\n' || *p == '\r')
            *p = ' ';
}

/* kind: "host" (hamam.exe) or "settings"; through: the programs between the
 * one asking and the request, for the window to say (pwsh.exe → hamam.exe) */
static enum consent_result ask_consent(struct gate_state *st, int need_password, const char *source_name,
                                       const char *source_path, const char *purpose, const char *kind,
                                       const char *through)
{
    char line[2048], prompt[4096], f_name[256], f_path[PATH_MAX], f_purpose[1024], f_through[512];
    int wrong = 0, n, fd = appinfo_connect();
    enum consent_result result = CONSENT_DENIED;

    if (fd < 0) {
        say("gate: no window to ask in: the Application Information service did not answer");
        return CONSENT_DENIED;
    }
    prompt_field(f_name, sizeof(f_name), source_name);
    prompt_field(f_path, sizeof(f_path), source_path);
    prompt_field(f_purpose, sizeof(f_purpose), purpose);
    prompt_field(f_through, sizeof(f_through), through);
    n = snprintf(prompt, sizeof(prompt), "prompt\t%d\t%s\t%s\t%s\t%s\t%s\n", need_password, f_name, f_path, f_purpose,
                 kind, f_through);
    if (n < 0 || send_all(fd, prompt, (size_t)n)) {
        close(fd);
        return CONSENT_DENIED;
    }

    for (;;) {
        if (!read_line_timeout(fd, line, sizeof(line), CONSENT_TIMEOUT_S) || !strcmp(line, "no"))
            break;
        if (!strncmp(line, "yes", 3) && (line[3] == 0 || line[3] == '\t')) {
            const char *pw = line[3] == '\t' ? line + 4 : "";

            if (!need_password || gate_check_password(st, pw)) {
                if (need_password)
                    gate_shm->password_entered = 1;
                if (!send_all(fd, "ok\n", 3))
                    result = CONSENT_GRANTED;
                break;
            }
            if (++wrong >= MAX_WRONG_ATTEMPTS) {
                char wait_msg[32];

                snprintf(wait_msg, sizeof(wait_msg), "wait\t%d\n", WRONG_PAUSE_S);
                if (send_all(fd, wait_msg, strlen(wait_msg)))
                    break;
                sleep(WRONG_PAUSE_S);
                wrong = 0;
                continue;
            }
            if (send_all(fd, "wrong\n", 6))
                break;
            continue;
        }
        break; /* anything else: as good as "no" */
    }

    close(fd); /* the window goes with it */
    return result;
}

/* True once origin has a grant; otherwise asks (one window at a time: a
 * request that waited checks again first, in case the answer already
 * covered it) and records a "yes" before returning */
static int gate_authorize(struct gate_state *st, const struct ntree_entry *origin, const char *purpose,
                          const char *through)
{
    int lock_fd, ok;

    gate_lock();
    ok = grant_lookup(origin->unix_pid, origin->start_time);
    gate_unlock();
    if (ok)
        return 1;

    lock_fd = open(CONSENT_LOCK, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock_fd >= 0)
        flock(lock_fd, LOCK_EX);

    gate_lock();
    ok = grant_lookup(origin->unix_pid, origin->start_time);
    gate_unlock();
    if (!ok) {
        int need_password = st->password_on && !gate_shm->password_entered;

        if (ask_consent(st, need_password, image_basename(origin->image), origin->image, purpose, "host", through) ==
            CONSENT_GRANTED) {
            gate_lock();
            grant_add(origin->unix_pid, origin->start_time);
            gate_unlock();
            ok = 1;
        }
    }
    if (lock_fd >= 0) {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
    }
    return ok;
}

/* Who is asking: the program behind the connection (find_origin), or -1
 * once the client was told it could not be known */
static int identify(int sock, uint32_t flags, struct ntree_entry *origin, char *through, size_t through_len)
{
    struct ntree_entry chain[NTREE_MAX];
    pid_t peer = peer_pid(sock);
    int chain_len = peer > 0 ? query_ntree(peer, chain, NTREE_MAX) : 0;

    if (through_len)
        through[0] = 0;
    if (!chain_len && peer > 0) { /* wineserver had nothing for it: per-process, not per-origin */
        long long start = process_start_time_fallback(peer);

        if (start >= 0) {
            char path[64], comm[64] = "", ours[64] = "", theirs[64] = "";
            int fd;

            chain[0].unix_pid = peer;
            chain[0].win32_id = 0;
            chain[0].start_time = start;
            /* a process of Linux, not of Windows: in a container (its user
             * namespace is not the host's) it is named as one */
            snprintf(path, sizeof(path), "/proc/%d/comm", (int)peer);
            if ((fd = open(path, O_RDONLY | O_CLOEXEC)) >= 0) {
                ssize_t n = read(fd, comm, sizeof(comm) - 1);
                comm[n > 0 ? n : 0] = 0;
                comm[strcspn(comm, "\n")] = 0;
                close(fd);
            }
            snprintf(path, sizeof(path), "/proc/%d/ns/user", (int)peer);
            if (comm[0] && readlink(path, theirs, sizeof(theirs) - 1) > 0 &&
                readlink("/proc/self/ns/user", ours, sizeof(ours) - 1) > 0 && strcmp(ours, theirs))
                snprintf(chain[0].image, sizeof(chain[0].image), "%s (Linux, WSL)", comm);
            else
                snprintf(chain[0].image, sizeof(chain[0].image), "%s",
                         (flags & REQ_HOST) ? "hamam.exe" : (flags & REQ_GITBASH) ? "bash.exe" : "wsl.exe");
            chain_len = 1;
        }
    }
    if (!chain_len) {
        send_error(sock, "the Linux gate could not identify this request");
        return -1;
    }
    find_origin(chain, chain_len, origin);
    /* what lies between the program and the request, nearest first:
     * "pwsh.exe → hamam.exe" */
    for (int i = 0; i < chain_len && through_len; i++) {
        if (chain[i].unix_pid != origin->unix_pid)
            continue;
        size_t used = 0;
        for (int j = i - 1; j >= 0 && used + 1 < through_len; j--) {
            int n = snprintf(through + used, through_len - used, "%s%s", used ? " \xe2\x86\x92 " : "",
                             image_basename(chain[j].image));
            if (n < 0)
                break;
            used += (size_t)n;
        }
        break;
    }
    return 0;
}


/* REQ_GATE, for the Panel's "User Accounts": status needs no window;
 * changing the password or requiring it always does, with the current
 * password if one is set, independent of any standing grant */
static void serve_gate(int sock, struct gate_state *st, const struct ntree_entry *origin, unsigned int argc,
                       char **argv)
{
    char text[32];
    int n;

    if (argc >= 1 && !strcmp(argv[0], "status")) {
        n = snprintf(text, sizeof(text), "password=%s\n", st->password_on ? "on" : "off");
        send_frame(sock, F_STDOUT, text, (uint32_t)n);
        send_frame(sock, F_EXIT, "\0\0\0\0", 4);
        return;
    }
    if (!(argc >= 2 && (!strcmp(argv[0], "password") || !strcmp(argv[0], "require")))) {
        send_error(sock, "gate: unknown request");
        return;
    }
    if (ask_consent(st, st->password_on, image_basename(origin->image), origin->image, "", "settings", "") !=
        CONSENT_GRANTED) {
        send_frame(sock, F_DENIED, NULL, 0);
        return;
    }
    if (!strcmp(argv[0], "password")) {
        if (gate_set_password(st, argv[1])) {
            send_error(sock, "gate: could not set the password");
            return;
        }
    } else /* "require" */
        st->password_on = !strcmp(argv[1], "on");
    if (gate_save(st)) {
        send_error(sock, "gate: could not save gate.conf");
        return;
    }
    send_frame(sock, F_EXIT, "\0\0\0\0", 4);
}

/* the user's line in the distribution's /etc/passwd (read after chroot) */
static int find_user(const char *want, uid_t *uid, gid_t *gid, char *home, size_t home_len, char *shell,
                     size_t shell_len)
{
    char line[1024];
    FILE *f = fopen("/etc/passwd", "re");
    int found = 0;

    while (f && !found && fgets(line, sizeof(line), f)) {
        char *field[7] = {0}, *p = line;
        line[strcspn(line, "\n")] = 0;
        for (int i = 0; i < 7; i++) {
            field[i] = p;
            p = strchr(p, ':');
            if (!p) {
                if (i < 6)
                    field[0] = NULL;
                break;
            }
            *p++ = 0;
        }
        if (!field[0] || strcmp(field[0], want))
            continue;
        *uid = (uid_t)strtoul(field[2], NULL, 10);
        *gid = (gid_t)strtoul(field[3], NULL, 10);
        snprintf(home, home_len, "%s", field[5]);
        snprintf(shell, shell_len, "%s", field[6] && *field[6] ? field[6] : "/bin/sh");
        found = 1;
    }
    if (f)
        fclose(f);
    return found ? 0 : -1;
}

/* The working folder: D:\x as /mnt/d/x, or /d/x for Git Bash; a Linux
 * path as it is */
static void linux_cwd(const char *cwd, int git_bash, char *out, size_t len)
{
    out[0] = 0;
    if (isalpha((unsigned char)cwd[0]) && cwd[1] == ':') {
        snprintf(out, len, git_bash ? "/%c/%s" : "/mnt/%c/%s", tolower((unsigned char)cwd[0]),
                 cwd[2] == '\\' || cwd[2] == '/' ? cwd + 3 : cwd + 2);
        for (char *p = out; *p; p++)
            if (*p == '\\')
                *p = '/';
    } else if (cwd[0] == '/')
        snprintf(out, len, "%s", cwd);
}

static void exec_child(const char *root, const char *user, const char *cwd, int git_bash, int argc, char **argv,
                       int envc, char **env, const char *distro)
{
    char home[PATH_MAX] = "/root", shell[PATH_MAX] = "/bin/bash", dir[PATH_MAX], buf[PATH_MAX + 32];
    uid_t uid = 0;
    gid_t gid = 0;

    /* in a container (root NULL) the root is the container's already, and
     * the session's process group is the joining child's */
    if (root) {
        setsid();
        if (chroot(root) || chdir("/")) {
            fprintf(stderr, "wsl: chroot %s: %s\n", root, strerror(errno));
            _exit(126);
        }
    } else if (chdir("/")) {
        _exit(126);
    }
    if (find_user(*user ? user : "root", &uid, &gid, home, sizeof(home), shell, sizeof(shell)) && *user) {
        fprintf(stderr, "wsl: no user %s in %s\n", user, distro);
        _exit(126);
    }
    if (uid && (setgroups(0, NULL) || setgid(gid) || setuid(uid))) {
        fprintf(stderr, "wsl: user %s: %s\n", user, strerror(errno));
        _exit(126);
    }

    /* what the Windows side passed, then the Linux basics over it */
    clearenv();
    for (int i = 0; i < envc; i++)
        if (strchr(env[i], '='))
            putenv(env[i]);
    setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
    setenv("HOME", home, 1);
    setenv("SHELL", shell, 1);
    const char *name = *user ? user : "root";
    setenv("USER", name, 1);
    setenv("LOGNAME", name, 1);
    setenv("WSL_DISTRO_NAME", distro, 1);
    if (!getenv("LANG"))
        setenv("LANG", "C.UTF-8", 1);
    if (!getenv("TERM"))
        setenv("TERM", "xterm-256color", 1);

    linux_cwd(cwd, git_bash, dir, sizeof(dir));
    if (!strcmp(cwd, "~") || !dir[0] || chdir(dir))
        if (chdir(home))
            chdir("/");
    if (getcwd(buf, sizeof(buf)))
        setenv("PWD", buf, 1);

    if (!argc) { /* a login shell, as "wsl" alone gives */
        char *login[] = {shell, "-l", NULL};
        execv(shell, login);
    } else {
        argv[argc] = NULL;
        execvp(argv[0], argv);
    }
    fprintf(stderr, "wsl: %s: %s\n", argc ? argv[0] : shell, strerror(errno));
    _exit(127);
}

/* hamam.exe's command: root on the host, with the distribution's programs.
 * The host's /proc, /sys and /dev and the drives are bound in a mount
 * namespace of this command's own: they go when it ends, and none is left
 * under /run/lxss in the host's namespace. */
static void host_child(const char *root, const char *user, const char *cwd, int git_bash, int argc, char **argv,
                       int envc, char **env, const char *distro)
{
    char path[PATH_MAX];

    if (gate_shm)
        munmap(gate_shm, sizeof(*gate_shm));
    if (unshare(CLONE_NEWNS) || mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL)) {
        fprintf(stderr, "hamam: a mount namespace: %s\n", strerror(errno));
        _exit(126);
    }
    snprintf(path, sizeof(path), "%s/proc", root);
    make_dirs(path);
    mount("proc", path, "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
    snprintf(path, sizeof(path), "%s/sys", root);
    make_dirs(path);
    mount("/sys", path, NULL, MS_BIND | MS_REC, NULL);
    snprintf(path, sizeof(path), "%s/dev", root);
    make_dirs(path);
    mount("/dev", path, NULL, MS_BIND | MS_REC, NULL);
    bind_drives(root);
    exec_child(root, user, cwd, git_bash, argc, argv, envc, env, distro);
}

/* stdin, stdout and stderr between the socket and the command until it ends */
static void pump(int sock, pid_t child, int in_fd, int out_fd, int err_fd)
{
    char buf[65536], pending[65536];
    size_t pending_len = 0, pending_off = 0;
    int status = 0, exited = 0, close_in = 0, idle_after_exit = 0;

    fcntl(in_fd, F_SETFL, O_NONBLOCK);
    for (;;) {
        struct pollfd fds[4];
        int n = 0, i_sock = -1, i_out = -1, i_err = -1, i_in = -1;

        if (!pending_len && !close_in) {
            fds[n] = (struct pollfd){sock, POLLIN, 0};
            i_sock = n++;
        } else if (sock >= 0) {
            fds[n] = (struct pollfd){sock, 0, 0}; /* still told when it goes */
            i_sock = n++;
        }
        if (out_fd >= 0) {
            fds[n] = (struct pollfd){out_fd, POLLIN, 0};
            i_out = n++;
        }
        if (err_fd >= 0) {
            fds[n] = (struct pollfd){err_fd, POLLIN, 0};
            i_err = n++;
        }
        if (in_fd >= 0 && pending_len) {
            fds[n] = (struct pollfd){in_fd, POLLOUT, 0};
            i_in = n++;
        }
        int ready = poll(fds, (nfds_t)n, 100);
        if (ready < 0 && errno != EINTR)
            break;

        if (i_sock >= 0 && fds[i_sock].revents) {
            if (fds[i_sock].revents & (POLLHUP | POLLERR) && !(fds[i_sock].revents & POLLIN)) {
                kill(-child, SIGKILL); /* wsl.exe is gone: so is what it ran */
                return;
            }
            if (fds[i_sock].revents & POLLIN) {
                unsigned char head[5];
                if (read_all(sock, head, 5)) {
                    kill(-child, SIGKILL);
                    return;
                }
                uint32_t len = get32(head + 1);
                if (len > sizeof(pending) || (len && read_all(sock, pending, len))) {
                    kill(-child, SIGKILL);
                    return;
                }
                if (head[0] == F_STDIN && in_fd >= 0) {
                    pending_len = len;
                    pending_off = 0;
                } else if (head[0] == F_STDIN_EOF) {
                    close_in = 1;
                } else if (head[0] == F_SIGNAL && len >= 4) {
                    kill(-child, (int)get32((unsigned char *)pending));
                }
            }
        }
        if (i_in >= 0 && fds[i_in].revents) {
            ssize_t w = write(in_fd, pending + pending_off, pending_len);
            if (w > 0) {
                pending_off += (size_t)w;
                pending_len -= (size_t)w;
            } else if (w < 0 && errno != EAGAIN) {
                pending_len = 0; /* nobody reads it any more */
                close_in = 1;
            }
        }
        if (close_in && !pending_len && in_fd >= 0) {
            close(in_fd);
            in_fd = -1;
        }
        int got = 0;
        for (int which = 0; which < 2; which++) {
            int idx = which ? i_err : i_out, *fd = which ? &err_fd : &out_fd;
            if (idx < 0 || !fds[idx].revents)
                continue;
            ssize_t r = read(*fd, buf, sizeof(buf));
            if (r > 0) {
                got = 1;
                if (send_frame(sock, which ? F_STDERR : F_STDOUT, buf, (uint32_t)r)) {
                    kill(-child, SIGKILL);
                    return;
                }
            } else if (r == 0 || errno != EINTR) {
                close(*fd);
                *fd = -1;
            }
        }
        if (!exited && waitpid(child, &status, WNOHANG) == child)
            exited = 1;
        /* Once the command has ended, what it wrote is sent; something it
         * left running in the background may hold the pipes for ever */
        if (exited) {
            idle_after_exit = got ? 0 : idle_after_exit + 1;
            if ((out_fd < 0 && err_fd < 0) || idle_after_exit > 2)
                break;
        }
    }
    unsigned char code[4];
    uint32_t c = WIFEXITED(status) ? (uint32_t)WEXITSTATUS(status) : 128u + (uint32_t)WTERMSIG(status);
    code[0] = c & 0xff;
    code[1] = (c >> 8) & 0xff;
    code[2] = (c >> 16) & 0xff;
    code[3] = c >> 24;
    send_frame(sock, F_EXIT, code, 4);
}

static void serve(int sock)
{
    unsigned char head[8 + TOKEN_LEN + 12];
    char name[256], vhdx[PATH_MAX], err[512], root[PATH_MAX];
    struct timeval tv = {10, 0};

    signal(SIGCHLD, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (read_all(sock, head, sizeof(head)) || memcmp(head, MAGIC, 8) || memcmp(head + 8, token, TOKEN_LEN))
        _exit(0);
    uint32_t flags = get32(head + 8 + TOKEN_LEN), argc = get32(head + 12 + TOKEN_LEN),
             envc = get32(head + 16 + TOKEN_LEN);
    if (argc > 4096 || envc > 4096)
        _exit(0);
    char *distro = read_string(sock), *user = read_string(sock), *cwd = read_string(sock);
    char **argv = calloc(argc + 1, sizeof(char *)), **env = calloc(envc + 1, sizeof(char *));
    if (!distro || !user || !cwd || !argv || !env)
        _exit(0);
    for (uint32_t i = 0; i < argc; i++)
        if (!(argv[i] = read_string(sock)))
            _exit(0);
    for (uint32_t i = 0; i < envc; i++)
        if (!(env[i] = read_string(sock)))
            _exit(0);
    tv.tv_sec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* The gate (docs/linux-gate.md). wsl.exe's commands run in their
     * distribution's container and ask nothing, as WSL on Windows does not.
     * hamam.exe's (REQ_HOST) are root on the host kernel: they always ask,
     * whoever asks, a process of a container too (it is the way out of one).
     * The Panel's requests ask as well. Stopping distributions asks
     * nothing, as on Windows. */
    struct gate_state gate_st;
    struct ntree_entry origin;
    char through[512];
    int host = (flags & REQ_HOST) != 0;

    gate_load(&gate_st);
    if (flags & REQ_GATE) {
        if (!identify(sock, flags, &origin, through, sizeof(through)))
            serve_gate(sock, &gate_st, &origin, argc, argv);
        _exit(0);
    }
    if (host) {
        char purpose[2048];

        flags &= ~(uint32_t)(REQ_SHUTDOWN | REQ_TERMINATE);
        if (!*distro) /* hamam's own distribution, the one gate.conf names */
            distro = gate_st.distro;
        if (find_distro(distro, name, sizeof(name), vhdx, sizeof(vhdx))) {
            send_error(sock, *distro ? "There is no distribution with the supplied name: %s" : "No distribution in %s",
                       *distro ? distro : "C:\\ProgramData\\Arctic\\Lxss\\distros");
            _exit(0);
        }
        if (identify(sock, flags, &origin, through, sizeof(through)))
            _exit(0);
        join_argv(argc, argv, purpose, sizeof(purpose));
        if (!purpose[0])
            snprintf(purpose, sizeof(purpose), "hamam -d %s", name);
        if (!gate_authorize(&gate_st, &origin, purpose, through)) {
            send_frame(sock, F_DENIED, NULL, 0);
            _exit(0);
        }
        say("hamam: %s (%s) as root on the host: %s", image_basename(origin.image), name, purpose);
    }

    int lock = lock_roots();
    if (flags & REQ_SHUTDOWN) {
        detach_all();
        send_frame(sock, F_EXIT, "\0\0\0\0", 4);
        _exit(0);
    }
    if (find_distro(distro, name, sizeof(name), vhdx, sizeof(vhdx))) {
        send_error(sock, *distro ? "There is no distribution with the supplied name: %s" : "No distribution in %s",
                   *distro ? distro : "C:\\ProgramData\\Arctic\\Lxss\\distros");
        _exit(0);
    }
    if (flags & REQ_TERMINATE) {
        snprintf(root, sizeof(root), ROOTS "/%s", name);
        if (is_mountpoint(root))
            detach(name);
        send_frame(sock, F_EXIT, "\0\0\0\0", 4);
        _exit(0);
    }
    if (attach(name, vhdx, err, sizeof(err))) {
        say("%s: %s", name, err);
        send_error(sock, "%s", err);
        _exit(0);
    }
    snprintf(root, sizeof(root), ROOTS "/%s", name);
    pid_t init = 0;
    if (!host && !(init = container_running(name)) && !(init = start_container(name, err, sizeof(err)))) {
        say("%s", err);
        send_error(sock, "%s", err);
        _exit(0);
    }
    close(lock);

    int in[2], out[2], errp[2];
    if (pipe2(in, O_CLOEXEC) || pipe2(out, O_CLOEXEC) || pipe2(errp, O_CLOEXEC)) {
        send_error(sock, "pipe: %s", strerror(errno));
        _exit(0);
    }
    pid_t child = fork();
    if (!child) {
        close(sock);
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(errp[1], 2);
        if (host)
            host_child(root, user, cwd, (flags & REQ_GITBASH) != 0, (int)argc, argv, (int)envc, env, name);
        else
            container_child(init, user, cwd, (flags & REQ_GITBASH) != 0, (int)argc, argv, (int)envc, env, name);
    }
    close(in[0]);
    close(out[1]);
    close(errp[1]);
    if (child < 0) {
        send_error(sock, "fork: %s", strerror(errno));
        _exit(0);
    }
    pump(sock, child, in[1], out[0], errp[0]);
    _exit(0);
}

static void on_term(int sig)
{
    (void)sig;
    stopping = 1;
}

int main(void)
{
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t addr_len = sizeof(addr);
    unsigned char random_bytes[TOKEN_LEN / 2];
    char session[128];
    struct sigaction sa = {.sa_handler = on_term};

    sigaction(SIGTERM, &sa, NULL);
    /* the distributions bind the host's /dev, whose devtmpfs has no links to
     * the open files: bash's <(...) needs /dev/fd */
    static const char *const fd_links[][2] = {
        {"/proc/self/fd", "/dev/fd"},
        {"/proc/self/fd/0", "/dev/stdin"},
        {"/proc/self/fd/1", "/dev/stdout"},
        {"/proc/self/fd/2", "/dev/stderr"},
    };
    for (size_t i = 0; i < sizeof(fd_links) / sizeof(*fd_links); i++)
        if (symlink(fd_links[i][0], fd_links[i][1]) && errno != EEXIST)
            say("%s: %s", fd_links[i][1], strerror(errno));
    /* qemu-nbd --fork leaves its server as an orphan: it is ours to reap */
    prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0);
    signal(SIGPIPE, SIG_IGN);
    make_dirs(ROOTS);
    gate_shared_init();
    if (getrandom(random_bytes, sizeof(random_bytes), 0) != (ssize_t)sizeof(random_bytes))
        return 1;
    for (int i = 0; i < TOKEN_LEN / 2; i++)
        snprintf(token + 2 * i, 3, "%02x", random_bytes[i]);

    int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0 || bind(listener, (struct sockaddr *)&addr, sizeof(addr)) || listen(listener, 16) ||
        getsockname(listener, (struct sockaddr *)&addr, &addr_len)) {
        say("listen: %s", strerror(errno));
        return 1;
    }
    /* for wsl.exe: on C:, which Wine's processes (user nt) read */
    make_dirs(LXSS_DIR);
    snprintf(session, sizeof(session), "%u %s\n", ntohs(addr.sin_port), token);
    write_text(SESSION ".new", session);
    rename(SESSION ".new", SESSION);
    struct passwd *nt = getpwnam(NT_USER);
    if (nt) {
        chown(LXSS_DIR, nt->pw_uid, nt->pw_gid);
        chown(SESSION, nt->pw_uid, nt->pw_gid);
    }
    say("listening on 127.0.0.1:%u", ntohs(addr.sin_port));

    while (!stopping) {
        struct pollfd pfd = {listener, POLLIN, 0};
        while (waitpid(-1, NULL, WNOHANG) > 0)
            ;
        if (poll(&pfd, 1, 500) <= 0)
            continue;
        int sock = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
        if (sock < 0)
            continue;
        pid_t pid = fork();
        if (!pid) {
            close(listener);
            serve(sock);
        }
        close(sock);
    }
    /* going down: the distributions are written out first */
    close(listener);
    unlink(SESSION);
    int lock = lock_roots();
    detach_all();
    close(lock);
    say("stopped");
    return 0;
}
