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
#include <linux/rtc.h>
#include <netdb.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#ifndef PR_SET_MEMORY_MERGE
#define PR_SET_MEMORY_MERGE 67 /* Linux 6.4 */
#endif
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/swap.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <termios.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "bootanim.h"
#include "stop.h"

#define NT_USER "nt"
#define PREFIX "/run/nt"
#define C_DRIVE "/mnt/c"
#define REG_DIR C_DRIVE "/Windows/System32/config"
#define HOST_STATE REG_DIR "/Host" /* what the host itself keeps, on a C: that keeps what is written */
#define LOG_DIR C_DRIVE "/Windows/Logs/Arctic"
#define WAYLAND_SOCKET "arctic-0" /* served by dwmcore.dll */
#define FONT "/usr/share/arctic/bsod.font"
#define VOLUMES_DIR "/run/arctic/volumes" /* written by arctic-volume */
#define EJECT_DIR "/run/arctic/eject"     /* requests from mountmgr.sys */

static int console = -1, kmsg = -1, hostlog = -1, ntlog = -1;
static int dev_mode, anim = -1, stopped; /* anim: the boot screen's commands (bootanim.h) */
static pid_t anim_pid;
static int c_on_disk; /* C: is a partition that keeps what is written (ARCTIC_C=disk or stick, initrd) */
static uid_t nt_uid;
static gid_t nt_gid;
static pid_t udevd_pid, wineserver_pid, wininit_pid, shell_pid, lxss_pid;

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
    bootanim_kill(&anim, &anim_pid); /* the stop screen needs the display */
    stop_screen(-1, FONT, code, what, dev_mode);
}

static char winedebug[256] = "WINEDEBUG=fixme-all"; /* arctic.winedebug= on the kernel command line */
static char tz_env[128] = "TZ=:/usr/share/zoneinfo/Europe/Kyiv"; /* set_clock: the system's time zone */
static char dev_program[128];                        /* arctic.run= */
/* C: on a stick: writes go to it in the background, nobody waits for it
 * (docs/persistence.md, "Повільна флешка"). A flush returns at once (Wine
 * 0058) and the registry is saved every 4 minutes (0057), in a child. */
static char lazy_flush[] = "WINE_LAZY_FLUSH=0";
static char dll_overrides[64] = "WINEDLLOVERRIDES="; /* for wineboot: no Mono or Gecko installers */
static char save_period[] = "WINE_REGISTRY_SAVE_PERIOD=30 ";

static char *const nt_env[] = {
    "WINEPREFIX=" PREFIX,
    "HOME=" PREFIX "/home",
    "USER=User",
    "LOGNAME=User",
    "LANG=uk_UA.UTF-8",
    tz_env,
    "PATH=/usr/bin",
    /* games see an NVIDIA card as one (NVAPI, DLSS), not as the AMD card
     * DXVK would otherwise make of it */
    "DXVK_ENABLE_NVAPI=1",
    winedebug,
    lazy_flush,
    save_period,
    dll_overrides,
    /* images Wine cannot map from their file, shared by every process (Wine 0059) */
    "WINE_IMAGE_CACHE=" PREFIX "/imagecache",
    "XDG_RUNTIME_DIR=" PREFIX "/xdg",
    "WAYLAND_DISPLAY=" WAYLAND_SOCKET,
    NULL,
};
static char *const host_env[] = {"PATH=/usr/bin", "LANG=C.UTF-8", tz_env, NULL};

static pid_t spawn(char *const argv[], int as_nt, int out);
static int write_sysfs(const char *path, const char *value);

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
    /* The networks iwd knows: on a C: that keeps what is written they stay,
     * with the rest of the host's state; otherwise they live as long as the
     * session. wlanapi.dll hands iwd the key of a network as a file there. */
    const char *iwd_state = c_on_disk ? HOST_STATE "/iwd" : "/run/arctic/iwd";
    mkdir(iwd_state, 0700);
    if (!c_on_disk && chown(iwd_state, nt_uid, nt_gid)) /* on C: everything is nt's already */
        say("iwd state folder: %s", strerror(errno));
    if (mount(iwd_state, "/var/lib/iwd", NULL, MS_BIND, NULL))
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
        /* Wine reads a big DLL whose sections are not page aligned (chrome.dll,
         * 250 MB) into each process as a copy of its own, where Windows maps it
         * once for all of them: identical pages the kernel merges (KSM) for
         * every NT process, which inherit this */
        prctl(PR_SET_MEMORY_MERGE, 1, 0, 0, 0);
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

/* Logs on the boot medium. A live system keeps C: in memory, so what went
 * wrong on a PC is also kept where it can be read afterwards on any
 * computer: arctic\logs on the medium, rewritten every half minute and
 * before the power goes. A medium that cannot be written (a disc) keeps
 * none, and a C: on a partition needs none: its own logs stay. */
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

static void write_kernel_log(const char *dir)
{
    int size = klogctl(10 /* SYSLOG_ACTION_SIZE_BUFFER */, NULL, 0);
    char *data;

    if (size <= 0 || !(data = malloc((size_t)size)))
        return;
    if ((size = klogctl(3 /* SYSLOG_ACTION_READ_ALL */, data, size)) > 0) {
        char path[PATH_MAX];

        snprintf(path, sizeof(path), "%s/kernel.log", dir);
        write_whole(path, data, (size_t)size);
    }
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
static void write_hardware(const char *to)
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

    /* the PC maker's boot logo (ACPI BGRT), which the boot screen keeps */
    if (!access("/sys/firmware/acpi/bgrt", F_OK)) {
        read_line("/sys/firmware/acpi/bgrt/status", a, sizeof(a));
        read_line("/sys/firmware/acpi/bgrt/xoffset", b, sizeof(b));
        read_line("/sys/firmware/acpi/bgrt/yoffset", c, sizeof(c));
        read_line("/sys/firmware/acpi/bgrt/type", d, sizeof(d));
        append(&text, &len, "\nBGRT status %s type %s at %s,%s\n", a, d, b, c);
    } else {
        append(&text, &len, "\nBGRT none\n");
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
    if (text) {
        snprintf(path, sizeof(path), "%s/hardware.txt", to);
        write_whole(path, text, len);
    }
    free(text);
}

/* The logs of the last few starts stay: logs is this one, logs.1 the one
 * before, up to logs.4. One PC tried after another no longer wipes the first. */
#define KEPT_LOGS 4

static void remove_logs(const char *dir)
{
    static const char *const files[] = {"hardware.txt", "host.log", "kernel.log", "nt.log"};
    char path[PATH_MAX];

    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, files[i]);
        unlink(path);
    }
    rmdir(dir);
}

static void rotate_logs(void)
{
    char from[PATH_MAX], to[PATH_MAX];

    snprintf(to, sizeof(to), MEDIUM_LOGS ".%d", KEPT_LOGS);
    remove_logs(to);
    for (int i = KEPT_LOGS - 1; i >= 0; i--) {
        if (i)
            snprintf(from, sizeof(from), MEDIUM_LOGS ".%d", i);
        else
            snprintf(from, sizeof(from), MEDIUM_LOGS);
        snprintf(to, sizeof(to), MEDIUM_LOGS ".%d", i + 1);
        rename(from, to);
    }
}

/* Where the memory goes, every half minute in host.log: a session with 16 GB
 * and two programs open ran out of it (programs killed one by one), so a
 * leak is to be seen as a number that only grows. Shmem is tmpfs, shared
 * sections and the GPU's buffers on Intel (i915 keeps them in shmem). */
static void log_memory(void)
{
    struct { char name[20]; long rss; int pid; } top[5] = {{"", 0, 0}};
    long total = 0, avail = 0, shmem = 0, slab = 0, swap_total = 0, swap_free = 0, value;
    char line[256], key[64], text[512];
    size_t len;
    DIR *proc;
    FILE *f;

    if ((f = fopen("/proc/meminfo", "re"))) {
        while (fgets(line, sizeof(line), f))
            if (sscanf(line, "%63[^:]: %ld", key, &value) == 2) {
                if (!strcmp(key, "MemTotal")) total = value;
                else if (!strcmp(key, "MemAvailable")) avail = value;
                else if (!strcmp(key, "Shmem")) shmem = value;
                else if (!strcmp(key, "Slab")) slab = value;
                else if (!strcmp(key, "SwapTotal")) swap_total = value;
                else if (!strcmp(key, "SwapFree")) swap_free = value;
            }
        fclose(f);
    }
    if ((proc = opendir("/proc"))) {
        for (struct dirent *de; (de = readdir(proc));) {
            char name[20] = "", path[300];
            long rss = 0;
            int pid = atoi(de->d_name);

            if (pid <= 0)
                continue;
            snprintf(path, sizeof(path), "/proc/%d/status", pid);
            if (!(f = fopen(path, "re")))
                continue;
            while (fgets(line, sizeof(line), f))
                if (!strncmp(line, "Name:", 5))
                    sscanf(line + 5, " %19s", name);
                else if (!strncmp(line, "VmRSS:", 6))
                    sscanf(line + 6, " %ld", &rss);
            fclose(f);
            for (int i = 0; i < 5; i++)
                if (rss > top[i].rss) {
                    memmove(&top[i + 1], &top[i], sizeof(top[0]) * (size_t)(4 - i));
                    snprintf(top[i].name, sizeof(top[i].name), "%s", name);
                    top[i].rss = rss;
                    top[i].pid = pid;
                    break;
                }
        }
        closedir(proc);
    }
    /* KSM: pages_sharing is how many pages point at a merged one, i.e. saved */
    char ksm[32] = "";
    if ((f = fopen("/sys/kernel/mm/ksm/pages_sharing", "re"))) {
        if (!fgets(ksm, sizeof(ksm), f))
            ksm[0] = 0;
        fclose(f);
    }
    len = (size_t)snprintf(text, sizeof(text),
                           "memory: %ld of %ld MB free, shmem %ld MB, slab %ld MB, compressed %ld MB, combined %ld MB;",
                           avail >> 10, total >> 10, shmem >> 10, slab >> 10, (swap_total - swap_free) >> 10,
                           atol(ksm) * 4 >> 10);
    for (int i = 0; i < 5 && top[i].rss && len < sizeof(text); i++)
        len += (size_t)snprintf(text + len, sizeof(text) - len, " %s(%d) %ld MB", top[i].name, top[i].pid,
                                top[i].rss >> 10);
    say("%s", text);
}

/* Memory compression, as Windows 10 has it: a swap on zram, which keeps
 * what goes there compressed in RAM. Without any swap, when memory runs
 * out the kernel kills the biggest program at once; with it, rarely used
 * pages get squeezed first. */
static void start_memory_compression(void)
{
    char id[16] = "", path[64], size[32];
    unsigned char page[4096] = {0};
    struct sysinfo si;
    int fd, n;

    if ((fd = open("/sys/class/zram-control/hot_add", O_RDONLY | O_CLOEXEC)) < 0)
        return;
    n = (int)read(fd, id, sizeof(id) - 1);
    close(fd);
    if (n <= 0)
        return;
    n = atoi(id);
    sysinfo(&si);
    snprintf(path, sizeof(path), "/sys/block/zram%d/comp_algorithm", n);
    write_sysfs(path, "zstd");
    snprintf(path, sizeof(path), "/sys/block/zram%d/disksize", n);
    snprintf(size, sizeof(size), "%llu", (unsigned long long)si.totalram * si.mem_unit / 2);
    if (write_sysfs(path, size)) {
        say("memory compression: %s", strerror(errno));
        return;
    }
    /* a swap signature, as mkswap writes it: version 1, the last page */
    snprintf(path, sizeof(path), "/dev/zram%d", n);
    for (int i = 0; i < 50 && access(path, F_OK); i++)
        usleep(20000);
    uint32_t version = 1, last = (uint32_t)(strtoull(size, NULL, 10) / sizeof(page) - 1);
    memcpy(page + 1024, &version, 4);
    memcpy(page + 1028, &last, 4);
    memcpy(page + sizeof(page) - 10, "SWAPSPACE2", 10);
    if ((fd = open(path, O_WRONLY | O_CLOEXEC)) < 0 || pwrite(fd, page, sizeof(page), 0) != (ssize_t)sizeof(page)) {
        say("memory compression: %s: %s", path, strerror(errno));
        if (fd >= 0)
            close(fd);
        return;
    }
    close(fd);
    if (swapon(path, 0))
        say("memory compression: swapon: %s", strerror(errno));
    else
        say("memory compression: up to %llu MB on %s", strtoull(size, NULL, 10) >> 20, path);
}

/* Memory combining, as Windows 8 and later do it: the kernel's KSM finds
 * pages that are the same in different processes and keeps one copy. Here
 * it is what keeps a browser from running out of memory: every Chromium
 * process holds its own copy of chrome.dll (see spawn). The advisor sets
 * how fast it scans, a pass in about 30 s, at most a quarter of a CPU. */
static void start_memory_combining(void)
{
    if (write_sysfs("/sys/kernel/mm/ksm/advisor_mode", "scan-time")) {
        write_sysfs("/sys/kernel/mm/ksm/pages_to_scan", "2000"); /* no advisor: a fixed pace */
    } else {
        write_sysfs("/sys/kernel/mm/ksm/advisor_target_scan_time", "30");
        write_sysfs("/sys/kernel/mm/ksm/advisor_max_cpu", "25");
        write_sysfs("/sys/kernel/mm/ksm/advisor_max_pages_to_scan", "30000");
    }
    if (write_sysfs("/sys/kernel/mm/ksm/run", "1"))
        say("memory combining: %s", strerror(errno));
}

static void save_logs(void)
{
    static off_t host_copied = -1, nt_copied = -1;
    static int hardware_written;

    if (c_on_disk)
        return;
    if (medium_writable < 0) {
        medium_writable = !mount(NULL, MEDIUM, NULL, MS_REMOUNT | MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
        if (medium_writable)
            rotate_logs();
        if (medium_writable && mkdir(MEDIUM_LOGS, 0755) && errno != EEXIST)
            medium_writable = 0;
        say("logs on the boot medium: %s", medium_writable ? MEDIUM_LOGS : "no, it cannot be written");
    }
    if (!medium_writable)
        return;
    if (!hardware_written) {
        write_hardware(MEDIUM_LOGS);
        hardware_written = 1;
    }
    write_kernel_log(MEDIUM_LOGS);
    copy_tail(LOG_DIR "/host.log", MEDIUM_LOGS "/host.log", 4 << 20, &host_copied);
    copy_tail(LOG_DIR "/nt.log", MEDIUM_LOGS "/nt.log", 16 << 20, &nt_copied);
}

/* The session asked for it (winlogon.exe): every process goes, then the machine */
/* every process but this one and the shutdown screen */
static void kill_all_but(pid_t keep, int signal)
{
    DIR *proc = opendir("/proc");

    for (struct dirent *de; proc && (de = readdir(proc));) {
        pid_t pid = (pid_t)atoi(de->d_name);
        if (pid > 1 && pid != keep)
            kill(pid, signal);
    }
    if (proc)
        closedir(proc);
}

static void power_off(int restart)
{
    /* wineserver saves the registry as it goes (its last save waits for any
     * save still running in a child): on a slow stick that takes its time */
    double wait_s = c_on_disk ? 120 : 15;
    pid_t screen = 0;

    announce("%s", restart ? "restart" : "shut down");
    /* The WSL distributions go first: their disks are VHDX files on D:,
     * which qemu-nbd must write out before anything else ends */
    if (lxss_pid > 0) {
        kill(lxss_pid, SIGTERM);
        for (int i = 0; i < 600 && waitpid(lxss_pid, NULL, WNOHANG) == 0; i++)
            usleep(100000);
        say("arctic-lxss %s", waitpid(lxss_pid, NULL, WNOHANG) < 0 ? "stopped" : "did not stop in 60 s");
        lxss_pid = 0;
    }
    kill(-1, SIGTERM);
    /* Windows' "Завершення роботи" while everything is written out: on a
     * slow stick the minutes of changes kept in memory take their time.
     * dwm.exe has just been told to go and gives the display up. */
    if (anim >= 0)
        close(anim);
    anim = bootanim_start("/usr/share/arctic/spinner.bin", "/usr/share/arctic/logo.bgra", FONT, &screen);
    bootanim_send(anim, "update %s\t", restart ? "Перезавантаження" : "Завершення роботи");
    for (int i = 0; i < wait_s * 10; i++) {
        pid_t pid;
        int status;

        while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
            if (pid == wineserver_pid)
                wineserver_pid = 0;
        if (!wineserver_pid && i >= 20)
            break;
        usleep(100000);
    }
    if (wineserver_pid)
        say("wineserver did not end in %.0f s", wait_s);
    kill_all_but(screen, SIGKILL);
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
    /* C: cannot be unmounted, the host itself runs from it on a stick. Read
     * only, it is written out and marked clean; that needs no file left open
     * for writing, the logs included. */
    log_memory();
    if (c_on_disk)
        write_kernel_log(LOG_DIR);
    say("C: goes read-only");
    close(hostlog);
    close(ntlog);
    hostlog = ntlog = -1;
    sync();
    /* the session ended cleanly: the next start needs no check (initrd) */
    unlink(HOST_STATE "/session");
    sync();
    if (mount(NULL, C_DRIVE, NULL, MS_REMOUNT | MS_RDONLY, NULL))
        say("C: stays writable: %s", strerror(errno));
    else
        say("C: read-only");
    if (!c_on_disk)
        umount2(C_DRIVE, MNT_DETACH);
    sync();
    reboot(restart ? RB_AUTOBOOT : RB_POWER_OFF);
}

static void child_exited(pid_t pid, int status)
{
    if (pid == lxss_pid) {
        say("arctic-lxss exited (status %d)", status);
        lxss_pid = 0;
    } else if (pid == wineserver_pid) {
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

/* After wineboot brought C:'s registry up to a newer Wine from wine.inf,
 * Arctic's own settings go over it again, as the image was made with them
 * (ci/build-rootfs.sh): Windows 11 as the version, and the .reg files in
 * /usr/share/arctic/registry, the build number among them. There is no Z:,
 * so they go through C:\Windows\Temp. */
static void reapply_registry(void)
{
    char *winecfg[] = {"/usr/bin/wine", "winecfg.exe", "-v", "win11", NULL};
    char from[PATH_MAX], to[PATH_MAX], winpath[PATH_MAX], buf[65536];
    struct dirent *de;
    DIR *dir;

    run(winecfg, 1, ntlog, 120);
    mkdir(C_DRIVE "/Windows/Temp/Arctic", 0755);
    if (!(dir = opendir("/usr/share/arctic/registry")))
        return;
    while ((de = readdir(dir))) {
        int in, out;
        ssize_t n;

        if (!strstr(de->d_name, ".reg"))
            continue;
        snprintf(from, sizeof(from), "/usr/share/arctic/registry/%s", de->d_name);
        snprintf(to, sizeof(to), C_DRIVE "/Windows/Temp/Arctic/%s", de->d_name);
        if ((in = open(from, O_RDONLY | O_CLOEXEC)) < 0)
            continue;
        if ((out = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)) >= 0) {
            while ((n = read(in, buf, sizeof(buf))) > 0)
                if (write(out, buf, (size_t)n) != n)
                    break;
            close(out);
            snprintf(winpath, sizeof(winpath), "C:\\Windows\\Temp\\Arctic\\%s", de->d_name);
            char *import[] = {"/usr/bin/wine", "reg.exe", "import", winpath, NULL};
            run(import, 1, ntlog, 60);
            unlink(to);
        }
        close(in);
    }
    closedir(dir);
    rmdir(C_DRIVE "/Windows/Temp/Arctic");
}

/* How good a card is as the default output, and which of its devices: an
 * analog or USB output first, then HDMI/DisplayPort with a monitor on it,
 * then any other that plays. 0: none plays (a webcam's microphone, say). */
static int sound_card_rank(int card, int *device)
{
    char path[PATH_MAX], line[256];
    int rank = 0, monitor = 0, hdmi_device = -1;
    DIR *dir;
    struct dirent *e;

    snprintf(path, sizeof(path), "/proc/asound/card%d", card);
    if (!(dir = opendir(path)))
        return 0;
    while ((e = readdir(dir))) {
        size_t len = strlen(e->d_name);
        int digital = 0;
        FILE *f;

        if (!strncmp(e->d_name, "eld#", 4)) {
            /* HDMI: a monitor that takes sound is there */
            snprintf(path, sizeof(path), "/proc/asound/card%d/%s", card, e->d_name);
            if ((f = fopen(path, "re"))) {
                while (fgets(line, sizeof(line), f))
                    if (!strncmp(line, "monitor_present", 15) && strchr(line, '1'))
                        monitor = 1;
                fclose(f);
            }
            continue;
        }
        if (strncmp(e->d_name, "pcm", 3) || e->d_name[len - 1] != 'p')
            continue;
        snprintf(path, sizeof(path), "/proc/asound/card%d/%s/info", card, e->d_name);
        if ((f = fopen(path, "re"))) {
            while (fgets(line, sizeof(line), f))
                if (!strncmp(line, "name:", 5) && (strstr(line, "HDMI") || strstr(line, "DP")))
                    digital = 1;
            fclose(f);
        }
        int dev = atoi(e->d_name + 3);
        if (!digital) {
            if (rank < 3 || dev < *device)
                *device = dev;
            rank = 3;
        }
        else if (hdmi_device < 0 || dev < hdmi_device)
            hdmi_device = dev;
    }
    closedir(dir);
    if (rank == 3 || hdmi_device < 0)
        return rank;
    *device = hdmi_device;
    return monitor ? 2 : 1;
}

/* The volume Windows programs hear is the endpoint's, applied in software by
 * mmdevapi as Windows' audio engine does, and 100% there is the card at full,
 * as with the card's Windows driver. alsactl init leaves Master at -20 dB, so
 * the main playback controls go to 0 dB (their top, when that is lower) and
 * are unmuted; a card without one of them just has nothing to set. */
static void raise_playback(int card)
{
    static const char *const controls[] = {"Master", "PCM", "Speaker", "Headphone", "Front"};
    char number[12];

    snprintf(number, sizeof(number), "%d", card);
    for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); i++) {
        char *argv[] = {"/usr/bin/amixer", "-q", "-c", number, "sset", (char *)controls[i], "0dB", "unmute", NULL};
        run(argv, 0, -1, 5);
    }
}

/* Sound: every card's mixer to sensible levels (the kernel leaves them
 * muted), and the best one as ALSA's default, which is what Windows
 * programs play to. The config lies on /run; /etc/asound.conf points to it. */
static void setup_sound(void)
{
    char *init[] = {"/usr/bin/alsactl", "init", NULL};
    char conf[160], name[64], path[PATH_MAX];
    int best = -1, best_rank = 0, best_device = 0;

    run(init, 0, hostlog, 20);
    for (int card = 0; card < 32; card++) {
        snprintf(path, sizeof(path), "/proc/asound/card%d", card);
        if (!access(path, F_OK))
            raise_playback(card);
    }
    for (int card = 0; card < 32; card++) {
        int device = 0, rank = sound_card_rank(card, &device);
        if (rank > best_rank) {
            best = card;
            best_rank = rank;
            best_device = device;
        }
    }
    if (best < 0) {
        say("sound: no card plays");
        return;
    }
    snprintf(path, sizeof(path), "/proc/asound/card%d/id", best);
    read_line(path, name, sizeof(name));
    say("sound: card %d (%s), device %d", best, name, best_device);
    int n = snprintf(conf, sizeof(conf), "defaults.pcm.card %d\ndefaults.pcm.device %d\ndefaults.ctl.card %d\n",
                     best, best_device, best);
    write_whole("/run/arctic/asound.conf", conf, (size_t)n);
}

static void make_dir(const char *path, mode_t mode, int nt_owned)
{
    mkdir(path, mode);
    if (nt_owned)
        chown(path, nt_uid, nt_gid);
}

/* RegBack, as Windows keeps one in config\RegBack: the hives of the last
 * start that came up. wineserver replaces a hive by renaming a new file over
 * it; when the power goes before that reaches the disk, the check of C:
 * at the next start can drop both names, and Wine would start with an empty
 * registry: Windows 10's defaults, Wine's own shell32 in place of
 * ReactOS', no explorer. */
#define REG_BACK REG_DIR "/RegBack"
static const char *const hives[] = {"system.reg", "user.reg", "userdef.reg"};
static int arctic_registry; /* the registry this start came up with is Arctic's */

static int copy_whole(const char *from, const char *to)
{
    char buf[65536];
    ssize_t n = 0;
    int in = open(from, O_RDONLY | O_CLOEXEC), out = -1, ok = 0;

    if (in >= 0 && (out = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)) >= 0) {
        while ((n = read(in, buf, sizeof(buf))) > 0)
            if (write(out, buf, (size_t)n) != n)
                break;
        ok = n == 0 && !fsync(out);
    }
    if (out >= 0)
        close(out);
    if (in >= 0)
        close(in);
    return ok ? 0 : -1;
}

static void restore_hives(void)
{
    char path[256], back[256];
    struct stat st;

    for (size_t i = 0; i < sizeof(hives) / sizeof(*hives); i++) {
        snprintf(path, sizeof(path), "%s/%s", REG_DIR, hives[i]);
        snprintf(back, sizeof(back), "%s/%s", REG_BACK, hives[i]);
        if ((!stat(path, &st) && st.st_size > 0) || stat(back, &st) || !st.st_size)
            continue;
        if (copy_whole(back, path))
            say("registry: %s could not come back from RegBack: %s", hives[i], strerror(errno));
        else {
            chown(path, nt_uid, nt_gid);
            say("registry: %s was lost, it came back from RegBack", hives[i]);
        }
    }
}

/* once the desktop is up: these hives work */
static void back_up_hives(void)
{
    char path[256], back[256], next[264];

    make_dir(REG_BACK, 0755, 1);
    for (size_t i = 0; i < sizeof(hives) / sizeof(*hives); i++) {
        snprintf(path, sizeof(path), "%s/%s", REG_DIR, hives[i]);
        snprintf(back, sizeof(back), "%s/%s", REG_BACK, hives[i]);
        snprintf(next, sizeof(next), "%s.new", back);
        if (!copy_whole(path, next) && !rename(next, back))
            chown(back, nt_uid, nt_gid);
        else
            unlink(next);
    }
    say("registry: backed up to RegBack");
}

/* The prefix directory holds only what Wine needs outside C:. Registry
 * hives live on C: like in Windows, and wineserver writes them there: the
 * prefix only links to them (a hive is replaced whole, next to its target).
 * There is no Z: drive. */
static void prepare_nt(void)
{
    char from[256], to[256];

    make_dir(PREFIX, 0700, 1);
    make_dir(PREFIX "/home", 0700, 1);
    make_dir(PREFIX "/xdg", 0700, 1);
    make_dir(PREFIX "/dosdevices", 0755, 1);
    make_dir(EJECT_DIR, 0755, 1);
    if (symlink(C_DRIVE, PREFIX "/dosdevices/c:") && errno != EEXIST)
        say("dosdevices/c: %s", strerror(errno));
    lchown(PREFIX "/dosdevices/c:", nt_uid, nt_gid);
    restore_hives();
    for (size_t i = 0; i < sizeof(hives) / sizeof(*hives); i++) {
        snprintf(from, sizeof(from), "%s/%s", REG_DIR, hives[i]);
        snprintf(to, sizeof(to), "%s/%s", PREFIX, hives[i]);
        if (symlink(from, to) && errno != EEXIST)
            say("%s: %s", to, strerror(errno));
        lchown(to, nt_uid, nt_gid);
    }
    /* the prefix's version on C: too: wineboot brings the registry up to a
     * new Wine once (an update brings one), not at every start after it */
    if (symlink(REG_DIR "/.update-timestamp", PREFIX "/.update-timestamp") && errno != EEXIST)
        say(".update-timestamp: %s", strerror(errno));
    lchown(PREFIX "/.update-timestamp", nt_uid, nt_gid);
}

/* The clock, as Windows keeps it. The PC's real-time clock holds local time
 * (Windows writes it so; the kernel read it as UTC, and the time was hours
 * off: the browser's sign-in to a site failed with "400"). It is read in
 * the system's time zone, C:\Windows\System32\config\Host\timezone ("Europe/
 * Warsaw"; Kyiv when there is none), which every program gets as TZ. */
static void set_clock(void)
{
    char zone[96] = "", path[160];
    struct rtc_time rtc;
    struct tm tm = {0};
    int fd;

    read_line(HOST_STATE "/timezone", zone, sizeof(zone));
    if (zone[0] && strspn(zone, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_/+-0123456789") == strlen(zone) &&
        !strstr(zone, "..")) {
        snprintf(path, sizeof(path), "/usr/share/zoneinfo/%s", zone);
        if (!access(path, R_OK))
            snprintf(tz_env, sizeof(tz_env), "TZ=:%s", path);
        else
            say("time zone %s: no such zone", zone);
    }
    putenv(tz_env);
    tzset();

    if ((fd = open("/dev/rtc0", O_RDONLY | O_CLOEXEC)) < 0)
        return;
    if (!ioctl(fd, RTC_RD_TIME, &rtc)) {
        tm.tm_sec = rtc.tm_sec;
        tm.tm_min = rtc.tm_min;
        tm.tm_hour = rtc.tm_hour;
        tm.tm_mday = rtc.tm_mday;
        tm.tm_mon = rtc.tm_mon;
        tm.tm_year = rtc.tm_year;
        tm.tm_isdst = -1;
        struct timeval tv = {.tv_sec = mktime(&tm)};
        if (tv.tv_sec > 0 && !settimeofday(&tv, NULL))
            say("clock from the real-time clock as local time (%s)", tz_env + 4);
    }
    close(fd);
}

/* Then the time from the network, as Windows' time service takes it from
 * time.windows.com (SNTP). The real-time clock is left as it is: Windows,
 * next to Arctic, keeps it. In the background, while the network comes. */
static void sync_time(void)
{
    if (fork())
        return;
    for (int attempt = 0; attempt < 24; attempt++) {
        struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM}, *ai = NULL;
        unsigned char packet[48] = {0x1b}; /* client, version 3 */
        struct timeval wait = {3, 0};
        int s;

        if (attempt)
            sleep(5);
        if (getaddrinfo("time.windows.com", "123", &hints, &ai) || !ai)
            continue;
        s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
        if (s >= 0 && sendto(s, packet, sizeof(packet), 0, ai->ai_addr, ai->ai_addrlen) == sizeof(packet) &&
            recv(s, packet, sizeof(packet), 0) == sizeof(packet)) {
            /* the transmit time: seconds since 1900, then a binary fraction */
            uint32_t sec = (uint32_t)packet[40] << 24 | packet[41] << 16 | packet[42] << 8 | packet[43];
            uint32_t frac = (uint32_t)packet[44] << 24 | packet[45] << 16 | packet[46] << 8 | packet[47];
            struct timeval now, tv = {.tv_sec = (time_t)sec - 2208988800u, .tv_usec = (suseconds_t)((uint64_t)frac * 1000000 >> 32)};
            gettimeofday(&now, NULL);
            if (sec && !settimeofday(&tv, NULL))
                say("clock from time.windows.com, %+ld s", (long)(tv.tv_sec - now.tv_sec));
            close(s);
            freeaddrinfo(ai);
            _exit(0);
        }
        if (s >= 0)
            close(s);
        freeaddrinfo(ai);
    }
    say("no time from time.windows.com");
    _exit(1);
}

/* The host's own state that a C: keeping what is written keeps too, in
 * C:\Windows\System32\config\Host: the networks iwd knows (start_network)
 * and the machine's id, made once for each stick rather than for each build */
static void keep_host_state(void)
{
    char id[64] = "";
    int fd;

    if (!c_on_disk)
        return;
    mkdir(HOST_STATE, 0755);
    if (access(HOST_STATE "/machine-id", F_OK)) {
        read_line("/proc/sys/kernel/random/uuid", id, sizeof(id));
        for (char *from = id, *to = id;; from++)
            if (*from != '-' && !(*to++ = *from))
                break;
        strcat(id, "\n");
        write_whole(HOST_STATE "/machine-id", id, strlen(id));
    }
    if ((fd = open(HOST_STATE "/machine-id", O_RDONLY | O_CLOEXEC)) >= 0) {
        close(fd);
        if (mount(HOST_STATE "/machine-id", "/etc/machine-id", NULL, MS_BIND, NULL))
            say("machine-id: %s", strerror(errno));
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

/* A C: that keeps what is written keeps the logs of the last few starts:
 * host.log is this one, host.1.log the one before, up to host.4.log */
static void rotate_c_logs(void)
{
    static const char *const logs[] = {"host", "nt"};
    char from[PATH_MAX], to[PATH_MAX];

    for (size_t i = 0; i < sizeof(logs) / sizeof(logs[0]); i++)
        for (int n = KEPT_LOGS - 1; n >= 0; n--) {
            if (n)
                snprintf(from, sizeof(from), LOG_DIR "/%s.%d.log", logs[i], n);
            else
                snprintf(from, sizeof(from), LOG_DIR "/%s.log", logs[i]);
            snprintf(to, sizeof(to), LOG_DIR "/%s.%d.log", logs[i], n + 1);
            rename(from, to);
        }
}

static void open_logs(void)
{
    mkdir(C_DRIVE "/Windows/Logs", 0755);
    mkdir(LOG_DIR, 0755);
    if (c_on_disk)
        rotate_c_logs();
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
    const char *anim_env = getenv("ARCTIC_BOOTANIM"), *c_env = getenv("ARCTIC_C");
    struct passwd *pw;

    /* "stick": C: on a USB stick, written in the background; "disk": on an
     * internal disk (installed), written as any system writes */
    c_on_disk = c_env && (!strcmp(c_env, "disk") || !strcmp(c_env, "stick"));
    if (c_env && !strcmp(c_env, "stick")) {
        lazy_flush[sizeof(lazy_flush) - 2] = '1';
        snprintf(save_period, sizeof(save_period), "WINE_REGISTRY_SAVE_PERIOD=240");
    }
    /* The boot screen's command pipe came from the initrd across exec on
     * purpose, but no child may inherit it: the boot screen ends when the
     * last writer closes it. */
    if (anim_env && sscanf(anim_env, "%d:%d", &anim, &anim_pid) == 2 && anim >= 0)
        fcntl(anim, F_SETFD, FD_CLOEXEC);
    else
        anim = -1;

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
    say("start (dev=%d, C: %s)", dev_mode, c_on_disk ? "keeps what is written" : "changes stay in memory");
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
    keep_host_state();
    set_clock();
    start_memory_compression();
    start_memory_combining();

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
    start_network();
    sync_time();
    setup_sound();

    /* The NT world */
    prepare_nt();
    char *wineserver[] = {"/usr/bin/wineserver", "-f", "-p", NULL};
    wineserver_pid = spawn(wineserver, 1, ntlog);
    say("wineserver pid %d", wineserver_pid);

    /* WSL: wsl.exe and bash.exe run commands in the distributions of
     * C:\ProgramData\Arctic\Lxss\distros through it (docs/updates.md) */
    char *lxss[] = {"/usr/bin/arctic-lxss", NULL};
    if (!access(lxss[0], X_OK))
        lxss_pid = spawn(lxss, 0, hostlog);

    /* A Wine newer than what C:'s registry was made with (an update) makes
     * wineboot bring it up to date: Windows' "getting ready" after updates.
     * Without Mono and Gecko it must not offer to download them: that dialog
     * has no screen yet and waited for ever (the start hung). */
    char *wineboot[] = {"/usr/bin/wine", "wineboot.exe", NULL};
    char stamp[32] = "";
    struct stat inf;
    int updating = 0;
    read_line(REG_DIR "/.update-timestamp", stamp, sizeof(stamp));
    if (!stat("/usr/share/wine/wine.inf", &inf) && strtoull(stamp, NULL, 10) != (unsigned long long)inf.st_mtime &&
        strncmp(stamp, "disable", 7)) {
        say("the registry is brought up to this Wine (%s -> %llu)", stamp, (unsigned long long)inf.st_mtime);
        bootanim_send(anim, "status Підготовка системи");
        updating = 1;
    }
    /* Only then: wineboot also starts the Run keys' programs, which keep
     * the variable, and a .NET program without mscoree loads no assembly */
    if (updating)
        snprintf(dll_overrides, sizeof(dll_overrides), "WINEDLLOVERRIDES=mscoree,mshtml=");
    int rc = run(wineboot, 1, ntlog, 300);
    if (updating)
        reapply_registry();
    snprintf(dll_overrides, sizeof(dll_overrides), "WINEDLLOVERRIDES=");
    bootanim_send(anim, "boot");
    mirror_ntlog();
    if (rc == 126 || rc == 127)
        nt_stop("SESSION3_INITIALIZATION_FAILED", "wineboot");

    char *ver[] = {"/usr/bin/wine", "cmd.exe", "/c", "ver", NULL};
    char *out = capture(ver, 120);
    for (char *p = out; *p; p++)
        if (*p == '\r' || *p == '\n')
            *p = ' ';
    announce("NT ready in %.1f s: %s", uptime(), out);
    /* Arctic's registry says Windows 11; an empty one, Wine's Windows 10 */
    arctic_registry = strstr(out, "10.0.22") != NULL;

    char *tasklist[] = {"/usr/bin/wine", "tasklist.exe", NULL};
    out = capture(tasklist, 120);
    for (char *line = strtok(out, "\r\n"); line; line = strtok(NULL, "\r\n"))
        announce("  %s", line);

    if (strstr(cmdline_buf, "arctic.stoptest=nt"))
        nt_stop("MANUALLY_INITIATED_CRASH", "arctic.stoptest");

    /* The session: wininit.exe starts dwm.exe, csrss.exe and winlogon.exe.
     * The display now belongs to dwm.exe: the boot screen gives up DRM master
     * but leaves its picture, and dwm becomes master by opening the card; the
     * boot screen ends a little after the desktop is there (desktop_at). */
    double desktop_at = 0;
    if (!stopped) {
        char *wininit[] = {"/usr/bin/wine", "wininit.exe", NULL};
        bootanim_send(anim, "release");
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
        desktop_at = uptime() + 5;

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
        /* dwm.exe has had its first frames on the screen: the boot screen goes */
        if (anim >= 0 && !stopped && desktop_at && uptime() > desktop_at) {
            close(anim);
            anim = -1;
        }
        /* the first copy once the desktop had time to come, then every half
         * minute; on a C: that keeps them the kernel's log goes with the others */
        if (tick % 150 == 75) {
            save_logs();
            log_memory();
            if (c_on_disk) {
                static int hardware_written, hives_backed_up;
                if (!hardware_written++)
                    write_hardware(LOG_DIR);
                write_kernel_log(LOG_DIR);
                /* a desktop up for a minute with its shell: the registry works */
                if (!hives_backed_up && arctic_registry && !stopped && desktop_at && uptime() > desktop_at + 60 &&
                    shell_pid) {
                    back_up_hives();
                    hives_backed_up = 1;
                }
            }
        }
        /* a USB network adapter plugged in, a udhcpc that ended */
        if (tick % 10 == 0)
            serve_wired();
        usleep(200000);
    }
}
