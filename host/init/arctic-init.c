/* arctic-init: PID 1 of the Arctic host.
 *
 * Starts the hardware side (udevd), prepares the NT world (registry from C:,
 * dosdevices without Z:) and runs it as the single user "nt": wineserver as the
 * NT executive, wineboot, which brings up services.exe, then wininit.exe, which
 * brings up the session.
 *
 * Output of every NT process goes to C:\Windows\Logs\Arctic\nt.log; own
 * messages go to host.log, the kernel log and, in development builds, the
 * serial console. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <net/if.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/klog.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "splash.h"
#include "stop.h"

#define NT_USER "nt"
#define PREFIX "/run/nt"
#define C_DRIVE "/mnt/c"
#define REG_DIR C_DRIVE "/Windows/System32/config"
#define LOG_DIR C_DRIVE "/Windows/Logs/Arctic"
#define LOGO "/usr/share/arctic/logo.bgra"
#define WAYLAND_SOCKET "arctic-0" /* served by dwmcore.dll */
#define FONT "/usr/share/arctic/bsod.font"
#define VOLUMES_DIR "/run/arctic/volumes" /* written by arctic-volume */
#define EJECT_DIR "/run/arctic/eject"     /* requests from mountmgr.sys */

static int console = -1, kmsg = -1, hostlog = -1, ntlog = -1;
static int dev_mode, splash = -1, stopped;
static uid_t nt_uid;
static gid_t nt_gid;
static pid_t udevd_pid, wineserver_pid, wininit_pid, shell_pid;

static double uptime(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void say(const char *fmt, ...)
{
    char msg[1024], line[1100];
    va_list ap;
    int n;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    n = snprintf(line, sizeof(line), "<5>arctic-init: %s", msg);
    if (kmsg >= 0)
        write(kmsg, line, (size_t)n);
    n = snprintf(line, sizeof(line), "[%8.3f] %s\n", uptime(), msg);
    if (hostlog >= 0)
        write(hostlog, line, (size_t)n);
    if (console >= 0 && dev_mode)
        write(console, line, (size_t)n);
}

/* Lines the smoke test looks for: always on the console */
static void announce(const char *fmt, ...)
{
    char msg[1024], line[1100];
    va_list ap;
    int n;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    n = snprintf(line, sizeof(line), "ARCTIC: %s\n", msg);
    if (console >= 0)
        write(console, line, (size_t)n);
    if (hostlog >= 0)
        write(hostlog, line, (size_t)n);
    if (kmsg >= 0) {
        n = snprintf(line, sizeof(line), "<5>ARCTIC: %s", msg);
        write(kmsg, line, (size_t)n);
    }
}

/* A dead NT world is a stop screen, as in Windows. Development builds keep
 * running afterwards so the serial shell stays usable. */
static void nt_stop(const char *code, const char *what)
{
    if (stopped)
        return;
    stopped = 1;
    announce("STOP %s (%s)", code, what);
    stop_screen(splash, FONT, code, what, dev_mode);
}

static char winedebug[256] = "WINEDEBUG=fixme-all"; /* arctic.winedebug= on the kernel command line */
static char dev_program[128];                        /* arctic.run= */

static char *const nt_env[] = {
    "WINEPREFIX=" PREFIX,
    "HOME=" PREFIX "/home",
    "USER=User",
    "LOGNAME=User",
    "LANG=uk_UA.UTF-8",
    "PATH=/usr/bin",
    /* games see an NVIDIA card as one (NVAPI, DLSS), not as the AMD card
     * DXVK would otherwise make of it */
    "DXVK_ENABLE_NVAPI=1",
    winedebug,
    "XDG_RUNTIME_DIR=" PREFIX "/xdg",
    "WAYLAND_DISPLAY=" WAYLAND_SOCKET,
    NULL,
};
static char *const host_env[] = {"PATH=/usr/bin", "LANG=C.UTF-8", NULL};

static pid_t spawn(char *const argv[], int as_nt, int out);

/* Networking. Wired interfaces get an address by DHCP as they come, the way
 * Windows sets up an Ethernet port by itself: one busybox udhcpc each,
 * started again if it ends. Wi-Fi belongs to iwd, which the Windows side
 * (wlanapi.dll) tells what to join over the system bus. */
static pid_t dbus_pid, iwd_pid;
static struct {
    char name[32];
    pid_t pid;
} wired[8];

static void network_child_exited(pid_t pid)
{
    if (pid == dbus_pid) {
        say("dbus-daemon exited");
        dbus_pid = 0;
    } else if (pid == iwd_pid) {
        say("iwd exited");
        iwd_pid = 0;
    }
    for (int i = 0; i < (int)(sizeof(wired) / sizeof(wired[0])); i++)
        if (wired[i].pid == pid)
            wired[i].pid = 0;
}

/* a real wired port: Ethernet, on a device, not Wi-Fi */
static int is_wired(const char *name)
{
    char path[PATH_MAX], text[16] = "";
    int fd;

    snprintf(path, sizeof(path), "/sys/class/net/%s/wireless", name);
    if (!access(path, F_OK))
        return 0;
    snprintf(path, sizeof(path), "/sys/class/net/%s/device", name);
    if (access(path, F_OK))
        return 0;
    snprintf(path, sizeof(path), "/sys/class/net/%s/type", name);
    if ((fd = open(path, O_RDONLY | O_CLOEXEC)) >= 0) {
        if (read(fd, text, sizeof(text) - 1) < 0)
            text[0] = 0;
        close(fd);
    }
    return atoi(text) == 1; /* ARPHRD_ETHER */
}

static void serve_wired(void)
{
    DIR *dir = opendir("/sys/class/net");
    struct dirent *de;

    while (dir && (de = readdir(dir))) {
        int i, slot = -1;

        if (de->d_name[0] == '.' || strlen(de->d_name) >= sizeof(wired[0].name) || !is_wired(de->d_name))
            continue;
        for (i = 0; i < (int)(sizeof(wired) / sizeof(wired[0])); i++) {
            if (!strcmp(wired[i].name, de->d_name))
                break;
            if (slot < 0 && !wired[i].name[0])
                slot = i;
        }
        if (i < (int)(sizeof(wired) / sizeof(wired[0])))
            slot = i;
        if (slot < 0 || wired[slot].pid)
            continue;
        snprintf(wired[slot].name, sizeof(wired[slot].name), "%s", de->d_name);
        char *udhcpc[] = {"/usr/bin/busybox", "udhcpc", "-f", "-S", "-R", "-i", wired[slot].name,
                          "-s", "/usr/lib/arctic/udhcpc.script", "-x", "hostname:arctic", NULL};
        wired[slot].pid = spawn(udhcpc, 0, hostlog);
        say("wired network %s: DHCP", wired[slot].name);
    }
    if (dir)
        closedir(dir);
}

/* 127.0.0.1: programs talk to their own parts over it (Steam to its UI) */
static void loopback_up(void)
{
    struct ifreq ifr = {0};
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

    if (fd < 0)
        return;
    strcpy(ifr.ifr_name, "lo");
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) == 0) {
        ifr.ifr_flags |= IFF_UP;
        if (ioctl(fd, SIOCSIFFLAGS, &ifr))
            say("loopback: %s", strerror(errno));
    }
    close(fd);
}

static void start_network(void)
{
    char *dbus[] = {"/usr/bin/dbus-daemon", "--system", "--nofork", "--nopidfile", NULL};
    /* how a join went (scan, association, handshake, DHCP) goes to host.log */
    char *iwd[] = {"/usr/lib/iwd/iwd", "-d", "*station*:*netdev*:*eapol*:*network*", NULL};

    loopback_up();
    mkdir("/run/dbus", 0755);
    mkdir("/run/arctic/resolv.d", 0755);
    /* the networks iwd knows live as long as the session; Windows keeps the profiles */
    mkdir("/run/arctic/iwd", 0700);
    /* wlanapi.dll hands iwd the key of a network as a file there */
    if (chown("/run/arctic/iwd", nt_uid, nt_gid))
        say("iwd state folder: %s", strerror(errno));
    if (mount("/run/arctic/iwd", "/var/lib/iwd", NULL, MS_BIND, NULL))
        say("no state folder for iwd: %s", strerror(errno));
    dbus_pid = spawn(dbus, 0, hostlog);
    for (int i = 0; i < 100 && access("/run/dbus/system_bus_socket", F_OK); i++)
        usleep(50000);
    iwd_pid = spawn(iwd, 0, hostlog);
    serve_wired();
}

static pid_t spawn(char *const argv[], int as_nt, int out)
{
    pid_t pid = fork();

    if (pid)
        return pid;

    sigset_t all;
    sigemptyset(&all);
    sigprocmask(SIG_SETMASK, &all, NULL);
    setsid();
    int null = open("/dev/null", O_RDWR);
    dup2(null, 0);
    dup2(out >= 0 ? out : null, 1);
    dup2(out >= 0 ? out : null, 2);
    if (as_nt) {
        if (initgroups(NT_USER, nt_gid) || setgid(nt_gid) || setuid(nt_uid))
            _exit(126);
        if (chdir(PREFIX "/home"))
            _exit(126);
        execve(argv[0], argv, nt_env);
    } else {
        execve(argv[0], argv, host_env);
    }
    _exit(127);
}

/* Logs on the boot stick. A live system keeps C: in memory, so what went
 * wrong on a PC is also kept where it can be read afterwards on any
 * computer: arctic\logs on the stick, rewritten every half minute and
 * before the power goes. A medium that cannot be written (a disc) keeps
 * none. */
#define MEDIUM "/run/arctic/media"
#define MEDIUM_LOGS MEDIUM "/arctic/logs"

static int medium_writable = -1; /* not tried yet */

static void write_whole(const char *path, const char *data, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

    if (fd < 0)
        return;
    while (len) {
        ssize_t n = write(fd, data, len);
        if (n <= 0)
            break;
        data += n;
        len -= (size_t)n;
    }
    fsync(fd);
    close(fd);
}

/* the last max bytes of a file, once it changed since the last copy */
static void copy_tail(const char *from, const char *to, off_t max, off_t *copied)
{
    struct stat st;
    char *data;
    int fd;

    if (stat(from, &st) || st.st_size == *copied)
        return;
    off_t start = st.st_size > max ? st.st_size - max : 0;
    size_t len = (size_t)(st.st_size - start);
    if ((fd = open(from, O_RDONLY | O_CLOEXEC)) < 0)
        return;
    if ((data = malloc(len ? len : 1)) && pread(fd, data, len, start) == (ssize_t)len) {
        write_whole(to, data, len);
        *copied = st.st_size;
    }
    free(data);
    close(fd);
}

static void write_kernel_log(void)
{
    int size = klogctl(10 /* SYSLOG_ACTION_SIZE_BUFFER */, NULL, 0);
    char *data;

    if (size <= 0 || !(data = malloc((size_t)size)))
        return;
    if ((size = klogctl(3 /* SYSLOG_ACTION_READ_ALL */, data, size)) > 0)
        write_whole(MEDIUM_LOGS "/kernel.log", data, (size_t)size);
    free(data);
}

static void append(char **text, size_t *len, const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    if (n >= (int)sizeof(line))
        n = sizeof(line) - 1;
    char *grown = realloc(*text, *len + (size_t)n + 1);
    if (!grown)
        return;
    memcpy(grown + *len, line, (size_t)n + 1);
    *text = grown;
    *len += (size_t)n;
}

static void read_line(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t n = fd >= 0 ? read(fd, buf, size - 1) : -1;

    if (fd >= 0)
        close(fd);
    buf[n > 0 ? n : 0] = 0;
    buf[strcspn(buf, "\n")] = 0;
}

static void link_name(const char *path, char *buf, size_t size)
{
    char target[PATH_MAX];
    ssize_t n = readlink(path, target, sizeof(target) - 1);

    if (n <= 0) {
        snprintf(buf, size, "-");
        return;
    }
    target[n] = 0;
    snprintf(buf, size, "%s", strrchr(target, '/') ? strrchr(target, '/') + 1 : target);
}

/* The PCI devices with their drivers, the display cards and their outputs:
 * what the PC has and what took it */
static void write_hardware(void)
{
    char *text = NULL, a[256], b[256], c[256], d[256], e[64], path[PATH_MAX];
    size_t len = 0;
    DIR *dir;

    read_line("/proc/sys/kernel/osrelease", a, sizeof(a));
    read_line("/proc/cmdline", b, sizeof(b));
    append(&text, &len, "kernel %s\ncmdline %s\n\nPCI devices (vendor:device subsystem rev class driver)\n", a, b);
    if ((dir = opendir("/sys/bus/pci/devices"))) {
        for (struct dirent *de; (de = readdir(dir));) {
            if (de->d_name[0] == '.')
                continue;
#define PCI_FILE(buf, name) \
            snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/" name, de->d_name); \
            read_line(path, buf, sizeof(buf))
            PCI_FILE(a, "vendor");
            PCI_FILE(b, "device");
            PCI_FILE(c, "subsystem_vendor");
            PCI_FILE(d, "subsystem_device");
            PCI_FILE(e, "revision");
            snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/class", de->d_name);
            char class[32], driver[64];
            read_line(path, class, sizeof(class));
            snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/driver", de->d_name);
            link_name(path, driver, sizeof(driver));
            append(&text, &len, "%s %s:%s %s:%s %s %s %s\n", de->d_name, a + 2, b + 2, c + 2, d + 2, e + 2,
                   class + 2, driver);
#undef PCI_FILE
        }
        closedir(dir);
    }

    append(&text, &len, "\nDisplay (card, output, status, first mode)\n");
    if ((dir = opendir("/sys/class/drm"))) {
        for (struct dirent *de; (de = readdir(dir));) {
            if (strncmp(de->d_name, "card", 4))
                continue;
            if (!strchr(de->d_name, '-')) {
                snprintf(path, sizeof(path), "/sys/class/drm/%s/device/driver", de->d_name);
                link_name(path, a, sizeof(a));
                snprintf(path, sizeof(path), "/sys/class/drm/%s/device/boot_vga", de->d_name);
                read_line(path, b, sizeof(b));
                append(&text, &len, "%s driver %s boot_vga %s\n", de->d_name, a, b[0] ? b : "-");
                continue;
            }
            snprintf(path, sizeof(path), "/sys/class/drm/%s/status", de->d_name);
            read_line(path, a, sizeof(a));
            snprintf(path, sizeof(path), "/sys/class/drm/%s/modes", de->d_name);
            read_line(path, b, sizeof(b));
            append(&text, &len, "%s %s %s\n", de->d_name, a, b[0] ? b : "-");
        }
        closedir(dir);
    }

    append(&text, &len, "\nModules\n");
    if ((dir = opendir("/sys/module"))) {
        for (struct dirent *de; (de = readdir(dir));) {
            snprintf(path, sizeof(path), "/sys/module/%s/initstate", de->d_name);
            if (!access(path, F_OK))
                append(&text, &len, "%s\n", de->d_name);
        }
        closedir(dir);
    }
    if (text)
        write_whole(MEDIUM_LOGS "/hardware.txt", text, len);
    free(text);
}

static void save_logs(void)
{
    static off_t host_copied = -1, nt_copied = -1;
    static int hardware_written;

    if (medium_writable < 0) {
        medium_writable = !mount(NULL, MEDIUM, NULL, MS_REMOUNT | MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
        if (medium_writable && mkdir(MEDIUM_LOGS, 0755) && errno != EEXIST)
            medium_writable = 0;
        say("logs on the boot medium: %s", medium_writable ? MEDIUM_LOGS : "no, it cannot be written");
    }
    if (!medium_writable)
        return;
    if (!hardware_written) {
        write_hardware();
        hardware_written = 1;
    }
    write_kernel_log();
    copy_tail(LOG_DIR "/host.log", MEDIUM_LOGS "/host.log", 4 << 20, &host_copied);
    copy_tail(LOG_DIR "/nt.log", MEDIUM_LOGS "/nt.log", 16 << 20, &nt_copied);
}

/* The session asked for it (winlogon.exe): every process goes, then the machine */
static void power_off(int restart)
{
    announce("%s", restart ? "restart" : "shut down");
    kill(-1, SIGTERM);
    sleep(2);
    kill(-1, SIGKILL);
    sync();
    save_logs();
    if (medium_writable > 0)
        mount(NULL, MEDIUM, NULL, MS_REMOUNT | MS_RDONLY, NULL);
    /* the other drives are written out whole before the power goes */
    DIR *drives = opendir("/run/arctic/drives");
    for (struct dirent *de; drives && (de = readdir(drives));) {
        char path[PATH_MAX];

        if (de->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "/run/arctic/drives/%.200s", de->d_name);
        if (umount2(path, 0))
            umount2(path, MNT_DETACH);
    }
    if (drives)
        closedir(drives);
    umount2(C_DRIVE, MNT_DETACH);
    sync();
    reboot(restart ? RB_AUTOBOOT : RB_POWER_OFF);
}

static void child_exited(pid_t pid, int status)
{
    if (pid == wineserver_pid) {
        say("wineserver exited (status %d)", status);
        wineserver_pid = 0;
        nt_stop("CRITICAL_PROCESS_DIED", "wineserver");
    } else if (pid == wininit_pid) {
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

        say("wininit.exe exited (status %d)", status);
        wininit_pid = 0;
        /* its exit codes: 1 dwm.exe could not take the display, 2 csrss.exe
         * ended, 3 restart, 4 shut down, 5 winlogon.exe ended */
        if (code == 3 || code == 4)
            power_off(code == 3);
        else if (code == 1)
            nt_stop("VIDEO_DWM_INIT_ERROR", "dwm.exe");
        else
            nt_stop("CRITICAL_PROCESS_DIED", code == 2 ? "csrss.exe" : code == 5 ? "winlogon.exe" : "wininit.exe");
    } else if (pid == udevd_pid) {
        say("udevd exited (status %d)", status);
        udevd_pid = 0;
    } else if (pid == shell_pid) {
        shell_pid = 0;
    } else {
        network_child_exited(pid);
    }
}

/* Waits for one child while still reaping everything else (PID 1 duty) */
static int wait_for(pid_t pid, int timeout_s, const char *what)
{
    double deadline = uptime() + timeout_s;
    int killed = 0;

    for (;;) {
        int status;
        pid_t r = waitpid(-1, &status, WNOHANG);

        if (r == pid)
            return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        if (r > 0) {
            child_exited(r, status);
            continue;
        }
        if (!killed && uptime() > deadline) {
            say("%s: no result after %d s, killing it", what, timeout_s);
            kill(pid, SIGKILL);
            killed = 1;
        }
        usleep(50000);
    }
}

static int run(char *const argv[], int as_nt, int out, int timeout_s)
{
    double t0 = uptime();
    int rc = wait_for(spawn(argv, as_nt, out), timeout_s, argv[0]);

    say("%s %s: exit %d in %.1f s", argv[0], argv[1] ? argv[1] : "", rc, uptime() - t0);
    return rc;
}

/* Runs an NT program and returns its output, with CR/LF collapsed to spaces */
static char *capture(char *const argv[], int timeout_s)
{
    static char buf[8192];
    char path[] = "/run/arctic/capture";
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    ssize_t n;

    buf[0] = 0;
    if (fd < 0)
        return buf;
    run(argv, 1, fd, timeout_s);
    lseek(fd, 0, SEEK_SET);
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    buf[n > 0 ? n : 0] = 0;
    return buf;
}

static void copy_file(const char *from, const char *to)
{
    char buf[65536];
    int in = open(from, O_RDONLY | O_CLOEXEC), out;
    ssize_t n;

    if (in < 0) {
        say("missing %s", from);
        return;
    }
    out = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    while (out >= 0 && (n = read(in, buf, sizeof(buf))) > 0)
        write(out, buf, (size_t)n);
    if (out >= 0) {
        fchown(out, nt_uid, nt_gid);
        close(out);
    }
    close(in);
}

static void make_dir(const char *path, mode_t mode, int nt_owned)
{
    mkdir(path, mode);
    if (nt_owned)
        chown(path, nt_uid, nt_gid);
}

/* The prefix directory holds only what Wine needs outside C:. Registry
 * hives live on C: like in Windows; there is no Z: drive. */
static void prepare_nt(void)
{
    static const char *hives[] = {"system.reg", "user.reg", "userdef.reg", ".update-timestamp"};
    char from[256], to[256];

    make_dir(PREFIX, 0700, 1);
    make_dir(PREFIX "/home", 0700, 1);
    make_dir(PREFIX "/xdg", 0700, 1);
    make_dir(PREFIX "/dosdevices", 0755, 1);
    make_dir(EJECT_DIR, 0755, 1);
    if (symlink(C_DRIVE, PREFIX "/dosdevices/c:") && errno != EEXIST)
        say("dosdevices/c: %s", strerror(errno));
    lchown(PREFIX "/dosdevices/c:", nt_uid, nt_gid);
    for (size_t i = 0; i < sizeof(hives) / sizeof(*hives); i++) {
        snprintf(from, sizeof(from), "%s/%s", REG_DIR, hives[i]);
        snprintf(to, sizeof(to), "%s/%s", PREFIX, hives[i]);
        copy_file(from, to);
    }
}

/* Safely Remove Hardware. mountmgr.sys, which runs as nt, asks for a disk
 * by creating EJECT_DIR/<disk>; the answer goes in EJECT_DIR/<disk>.done:
 * "ok", or "busy" when a program still has a file open on one of its
 * volumes, as Windows answers. The disk is left as Windows leaves it: its
 * volumes unmounted and their letters gone, its cache flushed, the disk
 * stopped and the USB port it is on switched off. */
static int write_sysfs(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC), ok;

    if (fd < 0)
        return -1;
    ok = write(fd, value, strlen(value)) == (ssize_t)strlen(value);
    close(fd);
    return ok ? 0 : -1;
}

/* the answer to mountmgr.sys; "ok" waits until it has been read, so the
 * eject request completes before the volumes and their letters go */
static void send_answer(const char *disk, const char *result)
{
    char answer[PATH_MAX], tmp[PATH_MAX];
    int fd;

    snprintf(tmp, sizeof(tmp), EJECT_DIR "/.%s.done", disk);
    snprintf(answer, sizeof(answer), EJECT_DIR "/%s.done", disk);
    if ((fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)) < 0)
        return;
    write(fd, result, strlen(result));
    fchown(fd, nt_uid, nt_gid);
    close(fd);
    rename(tmp, answer);
    if (strcmp(result, "ok"))
        return;
    for (int i = 0; i < 50 && !access(answer, F_OK); i++)
        usleep(100000);
}

static void eject_disk(const char *disk)
{
    char parts[16][64], mounts[16][PATH_MAX], path[PATH_MAX + 64], line[PATH_MAX + 64];
    char sys[PATH_MAX], usb[PATH_MAX];
    int n = 0, done = 0;
    struct dirent *de;
    DIR *dir;

    /* its volumes, from the records */
    if ((dir = opendir(VOLUMES_DIR))) {
        while (n < 16 && (de = readdir(dir))) {
            char mount[PATH_MAX] = "", on[64] = "";
            FILE *f;

            if (de->d_name[0] == '.')
                continue;
            snprintf(path, sizeof(path), VOLUMES_DIR "/%s", de->d_name);
            if (!(f = fopen(path, "re")))
                continue;
            while (fgets(line, sizeof(line), f)) {
                line[strcspn(line, "\n")] = 0;
                if (!strncmp(line, "mount=", 6))
                    snprintf(mount, sizeof(mount), "%.4000s", line + 6);
                else if (!strncmp(line, "disk=", 5))
                    snprintf(on, sizeof(on), "%.63s", line + 5);
            }
            fclose(f);
            if (strcmp(on, disk) || !mount[0])
                continue;
            snprintf(parts[n], sizeof(parts[n]), "%.63s", de->d_name);
            snprintf(mounts[n], sizeof(mounts[n]), "%s", mount);
            n++;
        }
        closedir(dir);
    }

    sync();
    for (; done < n; done++)
        if (umount2(mounts[done], 0))
            break;
    if (done < n) {
        int error = errno;

        say("eject %s: %s: %s", disk, mounts[done], strerror(error));
        /* the volumes already unmounted come back, with their letters */
        for (int i = 0; i < done; i++) {
            snprintf(path, sizeof(path), VOLUMES_DIR "/%s", parts[i]);
            unlink(path);
            rmdir(mounts[i]);
            snprintf(path, sizeof(path), "/sys/class/block/%s/uevent", parts[i]);
            write_sysfs(path, "add");
        }
        send_answer(disk, error == EBUSY ? "busy" : "error");
        return;
    }
    send_answer(disk, "ok");
    for (int i = 0; i < n; i++) {
        snprintf(path, sizeof(path), VOLUMES_DIR "/%s", parts[i]);
        unlink(path);
        rmdir(mounts[i]);
    }

    /* the USB device the disk is on, found before the disk goes */
    usb[0] = 0;
    snprintf(path, sizeof(path), "/sys/class/block/%s", disk);
    if (realpath(path, sys)) {
        snprintf(usb, sizeof(usb), "%s", sys);
        for (char *slash; (slash = strrchr(usb, '/')) && slash != usb;) {
            *slash = 0;
            snprintf(path, sizeof(path), "%s/idVendor", usb);
            if (!access(path, F_OK))
                break;
        }
        snprintf(path, sizeof(path), "%s/idVendor", usb);
        if (access(path, F_OK))
            usb[0] = 0;
    }
    /* a SCSI disk flushes its cache and stops as it is deleted */
    snprintf(path, sizeof(path), "/sys/class/block/%s/device/delete", disk);
    write_sysfs(path, "1");
    if (usb[0]) {
        snprintf(path, sizeof(path), "%s/remove", usb);
        write_sysfs(path, "1");
    }
    say("eject %s: %d volume(s), %s", disk, n, usb[0] ? "USB port off" : "stopped");
}

static void serve_ejects(void)
{
    char request[PATH_MAX];
    struct dirent *de;
    DIR *dir;

    if (!(dir = opendir(EJECT_DIR)))
        return;
    while ((de = readdir(dir))) {
        const char *name = de->d_name;

        if (name[0] == '.' || strchr(name, '.') || strspn(name, "abcdefghijklmnopqrstuvwxyz0123456789") != strlen(name))
            continue;
        snprintf(request, sizeof(request), EJECT_DIR "/%s", name);
        unlink(request);
        eject_disk(name);
    }
    closedir(dir);
}

static void open_logs(void)
{
    mkdir(C_DRIVE "/Windows/Logs", 0755);
    mkdir(LOG_DIR, 0755);
    hostlog = open(LOG_DIR "/host.log", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    ntlog = open(LOG_DIR "/nt.log", O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
}

/* Development builds mirror nt.log to the serial console */
static void mirror_ntlog(void)
{
    static off_t done;
    char buf[4096];
    ssize_t n;

    if (!dev_mode || ntlog < 0 || console < 0)
        return;
    while ((n = pread(ntlog, buf, sizeof(buf), done)) > 0) {
        write(console, buf, (size_t)n);
        done += n;
    }
}

static void start_shell(void)
{
    pid_t pid;

    if (!dev_mode || access("/dev/ttyS0", R_OK | W_OK))
        return;
    pid = fork();
    if (pid == 0) {
        setsid();
        int tty = open("/dev/ttyS0", O_RDWR);
        if (tty < 0)
            _exit(1);
        ioctl(tty, TIOCSCTTY, 1);
        dup2(tty, 0);
        dup2(tty, 1);
        dup2(tty, 2);
        char *argv[] = {"/usr/bin/busybox", "sh", NULL};
        char *envp[] = {"PATH=/usr/bin", "HOME=/root", "TERM=vt100", "PS1=arctic# ", NULL};
        execve(argv[0], argv, envp);
        _exit(127);
    }
    shell_pid = pid;
}

/* A GPU driver loaded by udev takes the screen over from simpledrm */
static int redraw_logo(int old_fd)
{
    char card[32];

    for (int i = 0; i < 8; i++) {
        snprintf(card, sizeof(card), "/dev/dri/card%d", i);
        if (access(card, F_OK))
            continue;
        int fd = splash_show(card, LOGO);
        if (fd >= 0) {
            fcntl(fd, F_SETFD, FD_CLOEXEC);
            say("logo on %s", card);
            if (old_fd >= 0)
                close(old_fd);
            return fd;
        }
    }
    return old_fd;
}

static char cmdline_buf[4096];

static void read_cmdline(void)
{
    int fd = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);

    if (fd >= 0) {
        read(fd, cmdline_buf, sizeof(cmdline_buf) - 1);
        close(fd);
    }
    dev_mode = strstr(cmdline_buf, "arctic.dev=1") != NULL;

    const char *debug = strstr(cmdline_buf, "arctic.winedebug=");
    if (debug) {
        size_t n = strcspn(debug += strlen("arctic.winedebug="), " \t\n");
        snprintf(winedebug, sizeof(winedebug), "WINEDEBUG=%.*s", (int)n, debug);
    }

    const char *run = strstr(cmdline_buf, "arctic.run=");
    if (run) {
        size_t n = strcspn(run += strlen("arctic.run="), " \t\n");
        snprintf(dev_program, sizeof(dev_program), "%.*s", (int)n, run);
    }
}

int main(void)
{
    const char *splash_env = getenv("ARCTIC_SPLASH_FD");
    struct passwd *pw;

    splash = splash_env ? atoi(splash_env) : -1;
    /* The logo fd came from the initrd across exec on purpose, but no child may
     * inherit it: while any process holds it, dwm.exe cannot become DRM master. */
    if (splash >= 0)
        fcntl(splash, F_SETFD, FD_CLOEXEC);

    signal(SIGPIPE, SIG_IGN);
    mkdir("/dev/pts", 0755);
    mount("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC, "gid=5,mode=620,ptmxmode=666");
    mkdir("/dev/shm", 01777);
    mount("tmpfs", "/dev/shm", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777");
    mkdir("/run/arctic", 0755);
    /* arctic-volume (run by udev) mounts and describes the volumes that get a
     * drive letter here; mountmgr.sys watches the descriptions */
    mkdir("/run/arctic/drives", 0755);
    mkdir("/run/arctic/volumes", 0755);
    sethostname("arctic", 6);

    console = open("/dev/console", O_WRONLY | O_NOCTTY | O_CLOEXEC);
    kmsg = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    read_cmdline();
    open_logs();
    say("start (dev=%d)", dev_mode);
    /* Every NT handle to a file, event or section is a descriptor in
     * wineserver and often in the process too: Steam's browser alone runs
     * past the kernel's 4096. Everything started from here inherits this. */
    struct rlimit files = {524288, 524288};
    if (setrlimit(RLIMIT_NOFILE, &files))
        say("open files limit: %s", strerror(errno));
    start_shell();

    pw = getpwnam(NT_USER);
    if (!pw) {
        say("no user %s", NT_USER);
        nt_stop("SESSION1_INITIALIZATION_FAILED", NT_USER);
        for (;;)
            pause();
    }
    nt_uid = pw->pw_uid;
    nt_gid = pw->pw_gid;

    /* Hardware: modules and firmware for whatever is plugged in */
    char *udevd[] = {"/usr/lib/systemd/systemd-udevd", NULL};
    char *trig_sub[] = {"/usr/bin/udevadm", "trigger", "--type=subsystems", "--action=add", NULL};
    char *trig_dev[] = {"/usr/bin/udevadm", "trigger", "--type=devices", "--action=add", NULL};
    char *settle[] = {"/usr/bin/udevadm", "settle", "--timeout=60", NULL};
    udevd_pid = spawn(udevd, 0, hostlog);
    /* events triggered before udevd listens are lost: no modules, no device owners */
    for (int i = 0; i < 100 && access("/run/udev/control", F_OK); i++)
        usleep(50000);
    run(trig_sub, 0, hostlog, 60);
    run(trig_dev, 0, hostlog, 60);
    run(settle, 0, hostlog, 90);
    splash = redraw_logo(splash);
    start_network();

    /* The NT world */
    prepare_nt();
    char *wineserver[] = {"/usr/bin/wineserver", "-f", "-p", NULL};
    wineserver_pid = spawn(wineserver, 1, ntlog);
    say("wineserver pid %d", wineserver_pid);

    char *wineboot[] = {"/usr/bin/wine", "wineboot.exe", NULL};
    int rc = run(wineboot, 1, ntlog, 300);
    mirror_ntlog();
    if (rc == 126 || rc == 127)
        nt_stop("SESSION3_INITIALIZATION_FAILED", "wineboot");

    char *ver[] = {"/usr/bin/wine", "cmd.exe", "/c", "ver", NULL};
    char *out = capture(ver, 120);
    for (char *p = out; *p; p++)
        if (*p == '\r' || *p == '\n')
            *p = ' ';
    announce("NT ready in %.1f s: %s", uptime(), out);

    char *tasklist[] = {"/usr/bin/wine", "tasklist.exe", NULL};
    out = capture(tasklist, 120);
    for (char *line = strtok(out, "\r\n"); line; line = strtok(NULL, "\r\n"))
        announce("  %s", line);

    if (strstr(cmdline_buf, "arctic.stoptest=nt"))
        nt_stop("MANUALLY_INITIATED_CRASH", "arctic.stoptest");

    /* The session: wininit.exe starts dwm.exe, csrss.exe and winlogon.exe.
     * The display now belongs to dwm.exe: releasing the logo fd drops DRM
     * master, and dwm becomes master by opening the card first. */
    if (!stopped) {
        char *wininit[] = {"/usr/bin/wine", "wininit.exe", NULL};
        if (splash >= 0)
            close(splash);
        splash = -1;
        wininit_pid = spawn(wininit, 1, ntlog);
        say("wininit.exe pid %d", wininit_pid);

        /* windows can only appear once dwm serves buffers */
        int served = 0;
        for (int i = 0; i < 200 && !(served = !access(PREFIX "/xdg/" WAYLAND_SOCKET, F_OK)); i++)
            usleep(50000);
        if (served)
            announce("dwm ready in %.1f s", uptime());
        else
            say("dwm.exe did not start serving");

        /* a program named on the command line, for tests */
        if (served && dev_program[0]) {
            char *program[] = {"/usr/bin/wine", dev_program, NULL};
            say("%s pid %d", dev_program, spawn(program, 1, ntlog));
        }
    }

    for (unsigned int tick = 0;; tick++) {
        int status;
        pid_t r;

        while ((r = waitpid(-1, &status, WNOHANG)) > 0)
            child_exited(r, status);
        if (!shell_pid)
            start_shell();
        mirror_ntlog();
        serve_ejects();
        /* the first copy once the desktop had time to come, then every half minute */
        if (tick % 150 == 75)
            save_logs();
        /* a USB network adapter plugged in, a udhcpc that ended */
        if (tick % 10 == 0)
            serve_wired();
        usleep(200000);
    }
}
