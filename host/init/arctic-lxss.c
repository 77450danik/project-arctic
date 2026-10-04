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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define C_DRIVE "/mnt/c"
#define DOSDEVICES "/run/nt/dosdevices"
#define LXSS_DIR C_DRIVE "/ProgramData/Arctic/Lxss"
#define DISTROS LXSS_DIR "/distros"
#define SESSION LXSS_DIR "/session"
#define ROOTS "/run/lxss"
#define LOCK ROOTS "/.lock"
#define NT_USER "nt"
#define MAX_NBD 16

/* the protocol, shared with programs/wsl/main.c */
#define MAGIC "ARCTLXS1"
#define TOKEN_LEN 32
enum { F_STDIN, F_STDIN_EOF, F_STDOUT, F_STDERR, F_EXIT, F_ERROR, F_SIGNAL };
#define REQ_SHUTDOWN 1  /* wsl --shutdown */
#define REQ_GITBASH 2   /* the working folder as Git Bash names it: /d/... */
#define REQ_TERMINATE 4 /* wsl --terminate <name> */

static char token[TOKEN_LEN + 1];
static volatile sig_atomic_t stopping;

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
    if (access("/sys/module/nbd", F_OK)) {
        char *modprobe[] = {"/usr/bin/modprobe", "nbd", "nbds_max=16", "max_part=0", NULL};
        run(modprobe);
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
    if (run(qemu_nbd)) {
        snprintf(err, err_len, "qemu-nbd could not attach %s", vhdx_win);
        return -1;
    }
    snprintf(path, sizeof(path), "/sys/block/nbd%d/size", nbd);
    for (int i = 0; i < 100 && read_number(path) <= 0; i++)
        usleep(100000);
    snprintf(path, sizeof(path), ROOTS "/%s.nbd", name);
    write_text(path, dev);

    make_dirs(root);
    if (mount(dev, root, "ext4", MS_NOATIME, NULL)) {
        snprintf(err, err_len, "%s: ext4: %s", vhdx_win, strerror(errno));
        char *detach[] = {"/usr/bin/qemu-nbd", "--disconnect", dev, NULL};
        run(detach);
        return -1;
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

    setsid();
    if (chroot(root) || chdir("/")) {
        fprintf(stderr, "wsl: chroot %s: %s\n", root, strerror(errno));
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
    bind_drives(root);
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
        exec_child(root, user, cwd, (flags & REQ_GITBASH) != 0, (int)argc, argv, (int)envc, env, name);
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
    /* qemu-nbd --fork leaves its server as an orphan: it is ours to reap */
    prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0);
    signal(SIGPIPE, SIG_IGN);
    make_dirs(ROOTS);
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
