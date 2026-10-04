/* /init of the Arctic initrd.
 *
 * Shows the logo, mounts C: and the host image and hands over to
 * /usr/bin/arctic-init. C: is one of two things:
 *
 *   a stick or a disk   arctic.root=PARTUUID=<disk signature>-<partition>:
 *                       the NTFS partition itself, written to directly. The
 *                       host image lies on it, in C:\Windows\System32\Host.
 *                       With arctic.live=1 its changes go to zram instead.
 *   a disc (the ISO)    the boot medium holds arctic/host.sqfs and
 *                       arctic/windows.sqfs with the NTFS image of C:, under a
 *                       device-mapper snapshot whose changes live in zram.
 *
 * Everything it needs is built into the kernel, so the initrd carries no
 * modules. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/blkpg.h>
#include <linux/dm-ioctl.h>
#include <linux/fs.h>
#include <linux/loop.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bootanim.h"
#include "stop.h"

#define NEWROOT "/newroot"
#define MEDIA "/media"
#define WINDOWS "/windows" /* windows.sqfs of the ISO */
#define CDRIVE "/c"        /* C: until it moves into the new root */
#define HOST_SQFS "/Windows/System32/Host/host.sqfs"
#define FIRST_START "/Windows/System32/Host/firststart" /* left by make-usb-img.sh: C: grows */
#define NTFSCK "/bin/ntfsck"
#define NTFSRESIZE "/bin/ntfsresize"
#define TOOL_LOG "/chkdsk.log" /* ends up in C:\Windows\Logs\Arctic */
#define C_DEV "/dev/arctic-c"
#define NT_OPTS "uid=1000,gid=1000,umask=022"
#define FONT "/bsod.font"

static int kmsg = -1, console = -1, anim = -1, dev_mode, live; /* anim: the boot screen's commands */
static pid_t anim_pid;
static char cmdline[4096], root_partuuid[40];

static void say(const char *fmt, ...)
{
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "<5>arctic-initrd: ");
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf + n, sizeof(buf) - (size_t)n, fmt, ap);
    va_end(ap);
    if (kmsg >= 0)
        write(kmsg, buf, strlen(buf));
}

/* Stop codes follow Windows: INACCESSIBLE_BOOT_DEVICE when the medium is
 * missing, UNMOUNTABLE_BOOT_VOLUME when C: fails, and so on. */
static void fatal(const char *code, const char *what, const char *detail)
{
    char line[512];

    say("%s: %s", detail, strerror(errno));
    /* One record per write(), the way say() and arctic-init do it: that is
     * what /dev/kmsg takes, rather than leaving it to the C library. */
    if (kmsg >= 0) {
        snprintf(line, sizeof(line), "<3>ARCTIC: STOP %s (%s)", code, what);
        write(kmsg, line, strlen(line));
    }
    /* And straight to the console, the way arctic-init announces a stop: a
     * line written to /dev/kmsg reaches the console only when the next one
     * pushes it out, and this is the last thing the initrd ever says. */
    if (console >= 0) {
        snprintf(line, sizeof(line), "ARCTIC: STOP %s (%s)\n", code, what);
        write(console, line, strlen(line));
    }
    bootanim_kill(&anim, &anim_pid); /* the stop screen needs the display */
    stop_screen(-1, FONT, code, what, dev_mode);
    for (;;)
        pause();
}

static void read_cmdline(void)
{
    int fd = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);

    if (fd >= 0) {
        read(fd, cmdline, sizeof(cmdline) - 1);
        close(fd);
    }
    dev_mode = strstr(cmdline, "arctic.dev=1") != NULL;
    live = strstr(cmdline, "arctic.live=1") != NULL;

    const char *root = strstr(cmdline, "arctic.root=PARTUUID=");
    if (root) {
        root += strlen("arctic.root=PARTUUID=");
        snprintf(root_partuuid, sizeof(root_partuuid), "%.*s", (int)strcspn(root, " \t\n"), root);
    }
}

static int write_file(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC), ok;

    if (fd < 0)
        return -1;
    ok = write(fd, value, strlen(value)) == (ssize_t)strlen(value);
    close(fd);
    return ok ? 0 : -1;
}

static int skip_block_device(const char *name)
{
    return name[0] == '.' || !strncmp(name, "loop", 4) || !strncmp(name, "ram", 3) || !strncmp(name, "zram", 4) ||
           !strncmp(name, "dm-", 3);
}

/* The PARTUUID Linux gives a partition of an MBR disk: the disk signature
 * and the partition number, "1a2b3c4d-02" */
static int mbr_partuuid(const char *name, char *out, size_t len)
{
    char path[PATH_MAX], disk[PATH_MAX], num[16];
    unsigned char mbr[512];
    ssize_t n;
    int fd;

    snprintf(path, sizeof(path), "/sys/class/block/%s/partition", name);
    if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
        return -1;
    n = read(fd, num, sizeof(num) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    num[n] = 0;

    /* the disk is the folder above the partition in sysfs */
    snprintf(path, sizeof(path), "/sys/class/block/%s", name);
    if (!realpath(path, disk) || !strrchr(disk, '/'))
        return -1;
    *strrchr(disk, '/') = 0;
    snprintf(path, sizeof(path), "/dev/%s", strrchr(disk, '/') + 1);
    if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
        return -1;
    n = read(fd, mbr, sizeof(mbr));
    close(fd);
    /* no MBR, or the protective one of a GPT disk */
    if (n != (ssize_t)sizeof(mbr) || mbr[510] != 0x55 || mbr[511] != 0xaa || mbr[446 + 4] == 0xee)
        return -1;
    snprintf(out, len, "%08x-%02x", mbr[440] | mbr[441] << 8 | mbr[442] << 16 | (unsigned)mbr[443] << 24, atoi(num));
    return 0;
}

/* An NTFS partition with the host in its Windows folder */
static int holds_host(const char *dev)
{
    int found;

    if (mount(dev, CDRIVE, "ntfs", MS_RDONLY, "nocase"))
        return 0;
    found = !access(CDRIVE HOST_SQFS, R_OK);
    umount(CDRIVE);
    return found;
}

/* The partition that is C:: the one with the PARTUUID the boot entry names.
 * Should there be none after a few seconds (Windows gives a disk a new
 * signature when two of them have the same), then the NTFS partition that
 * holds the host. USB sticks can take several seconds to appear. */
static void find_root(char *dev, size_t len)
{
    for (int attempt = 0; attempt < 150; attempt++) {
        int by_contents = attempt >= 25 && attempt % 5 == 0;
        DIR *dir = opendir("/sys/class/block");
        struct dirent *de;
        char uuid[40], path[300];

        while (dir && (de = readdir(dir))) {
            if (skip_block_device(de->d_name))
                continue;
            snprintf(path, sizeof(path), "/sys/class/block/%s/partition", de->d_name);
            if (access(path, F_OK))
                continue;
            snprintf(path, sizeof(path), "/dev/%s", de->d_name);
            if (!mbr_partuuid(de->d_name, uuid, sizeof(uuid)) && !strcasecmp(uuid, root_partuuid))
                say("C: is %s (PARTUUID %s)", path, uuid);
            else if (by_contents && holds_host(path))
                say("C: is %s (no PARTUUID %s, but it holds the host)", path, root_partuuid);
            else
                continue;
            snprintf(dev, len, "%s", path);
            closedir(dir);
            return;
        }
        if (dir)
            closedir(dir);
        usleep(200000);
    }
    errno = ENOENT;
    fatal("INACCESSIBLE_BOOT_DEVICE", dev_mode ? root_partuuid : "C:", "no partition with that PARTUUID");
}

/* The medium is whatever holds arctic/host.sqfs: an ISO written to a disc or
 * copied to a stick sector by sector, or its files unpacked onto a FAT stick. */
static const char *const media_fs[] = {"iso9660", "vfat", "exfat", "udf", "ext4", NULL};

static int is_media(const char *dev)
{
    for (int i = 0; media_fs[i]; i++) {
        if (mount(dev, MEDIA, media_fs[i], MS_RDONLY, NULL))
            continue;
        if (!access(MEDIA "/arctic/host.sqfs", R_OK))
            return 1;
        umount(MEDIA);
    }
    return 0;
}

/* USB sticks can take several seconds to appear */
static void find_media(void)
{
    char seen[192] = "";

    for (int attempt = 0; attempt < 150; attempt++) {
        DIR *dir = opendir("/sys/class/block");
        struct dirent *de;
        size_t len = 0;

        seen[0] = 0;
        while (dir && (de = readdir(dir))) {
            char dev[300];
            if (skip_block_device(de->d_name))
                continue;
            if (len + strlen(de->d_name) + 2 < sizeof(seen))
                len += (size_t)snprintf(seen + len, sizeof(seen) - len, "%s ", de->d_name);
            snprintf(dev, sizeof(dev), "/dev/%s", de->d_name);
            if (is_media(dev)) {
                say("boot medium: %s", dev);
                closedir(dir);
                return;
            }
        }
        if (dir)
            closedir(dir);
        usleep(200000);
    }
    errno = ENOENT;
    say("no medium among: %s", seen[0] ? seen : "(no block devices)");
    /* development builds name the disks that were there, to tell a missing
     * driver from a medium the kernel cannot read */
    fatal("INACCESSIBLE_BOOT_DEVICE", dev_mode && seen[0] ? seen : "ARCTIC", "boot medium not found");
}

/* A read-only loop device over a file; writable and with 512-byte blocks
 * over a block device (the zram of the live-mode snapshot) */
static int loop_setup(const char *file, char *dev, size_t len, int writable)
{
    int ctl = open("/dev/loop-control", O_RDWR | O_CLOEXEC), num, lfd, ffd;
    struct loop_config cfg;

    if (ctl < 0)
        return -1;
    num = ioctl(ctl, LOOP_CTL_GET_FREE);
    close(ctl);
    if (num < 0)
        return -1;
    snprintf(dev, len, "/dev/loop%d", num);
    lfd = open(dev, O_RDWR | O_CLOEXEC);
    ffd = open(file, (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (lfd < 0 || ffd < 0)
        return -1;
    memset(&cfg, 0, sizeof(cfg));
    cfg.fd = (uint32_t)ffd;
    if (writable)
        cfg.block_size = 512;
    else
        cfg.info.lo_flags = LO_FLAGS_READ_ONLY;
    num = ioctl(lfd, LOOP_CONFIGURE, &cfg);
    close(ffd);
    close(lfd);
    return num;
}

static int loop_attach(const char *file, char *dev, size_t len)
{
    return loop_setup(file, dev, len, 0);
}

static void dm_init(struct dm_ioctl *io, size_t size, const char *name)
{
    memset(io, 0, size);
    io->version[0] = DM_VERSION_MAJOR;
    io->data_size = (uint32_t)size;
    io->data_start = sizeof(*io);
    snprintf(io->name, sizeof(io->name), "%s", name);
}

/* Non-persistent snapshot: reads come from the image, writes go to zram */
static int dm_snapshot(const char *name, dev_t origin, dev_t cow, uint64_t sectors, dev_t *out)
{
    union { struct dm_ioctl io; char raw[16384]; } buf;
    struct dm_target_spec *spec = (struct dm_target_spec *)(buf.raw + sizeof(struct dm_ioctl));
    char *params = (char *)(spec + 1);
    int ctl = open("/dev/mapper/control", O_RDWR | O_CLOEXEC);

    if (ctl < 0)
        return -1;
    dm_init(&buf.io, sizeof(buf), name);
    if (ioctl(ctl, DM_DEV_CREATE, &buf.io))
        goto fail;
    *out = (dev_t)buf.io.dev;

    dm_init(&buf.io, sizeof(buf), name);
    buf.io.target_count = 1;
    spec->sector_start = 0;
    spec->length = sectors;
    snprintf(spec->target_type, sizeof(spec->target_type), "snapshot");
    snprintf(params, 256, "%u:%u %u:%u N 8", major(origin), minor(origin), major(cow), minor(cow));
    spec->next = (uint32_t)((sizeof(*spec) + strlen(params) + 1 + 7) & ~7u);
    if (ioctl(ctl, DM_TABLE_LOAD, &buf.io))
        goto fail;

    dm_init(&buf.io, sizeof(buf), name);
    if (ioctl(ctl, DM_DEV_SUSPEND, &buf.io)) /* no SUSPEND flag: this resumes, i.e. activates */
        goto fail;
    close(ctl);
    return 0;
fail:
    close(ctl);
    return -1;
}

/* C: that keeps nothing: reads come from origin, writes go to zram */
static void snapshot_c(const char *origin, const char *what)
{
    struct stat st_origin, st_cow;
    struct sysinfo si;
    uint64_t bytes = 0;
    dev_t cdev;
    char size[32], cow[64];
    int fd;

    fd = open(origin, O_RDONLY | O_CLOEXEC);
    if (fd < 0 || ioctl(fd, BLKGETSIZE64, &bytes))
        fatal("UNMOUNTABLE_BOOT_VOLUME", what, "size of C:");
    close(fd);

    /* The zram disk is only as big as the RAM, and it only uses RAM for what is written */
    sysinfo(&si);
    write_file("/sys/block/zram0/comp_algorithm", "zstd");
    snprintf(size, sizeof(size), "%llu", (unsigned long long)si.totalram * si.mem_unit);
    if (write_file("/sys/block/zram0/disksize", size))
        fatal("UNMOUNTABLE_BOOT_VOLUME", "zram", "zram disksize");

    /* zram has 4 KiB blocks, and a snapshot takes the largest block of its
     * devices; the ntfs driver refuses a volume whose 512-byte sectors are
     * smaller than that. Through a loop device zram has 512-byte ones. The
     * snapshot still writes it whole 4 KiB chunks. */
    if (loop_setup("/dev/zram0", cow, sizeof(cow), 1))
        fatal("UNMOUNTABLE_BOOT_VOLUME", "zram", "loop over zram");
    if (stat(origin, &st_origin) || stat(cow, &st_cow))
        fatal("UNMOUNTABLE_BOOT_VOLUME", "zram", "stat origin/zram");
    if (dm_snapshot("arctic-c", st_origin.st_rdev, st_cow.st_rdev, bytes / 512, &cdev))
        fatal("UNMOUNTABLE_BOOT_VOLUME", "dm-snapshot", "device-mapper snapshot");
    if (mknod(C_DEV, S_IFBLK | 0600, cdev) && errno != EEXIST)
        fatal("UNMOUNTABLE_BOOT_VOLUME", "dm-snapshot", "mknod C:");
}

static void mount_c(const char *dev, unsigned long flags)
{
    if (!mount(dev, CDRIVE, "ntfs", flags, "nocase,windows_names," NT_OPTS)) {
        say("C: mounted (ntfs, nocase)");
        return;
    }
    say("ntfs mount failed (%s), trying ntfs3", strerror(errno));
    if (mount(dev, CDRIVE, "ntfs3", flags, NT_OPTS))
        fatal("UNMOUNTABLE_BOOT_VOLUME", "ntfs", "mount C:");
    say("C: mounted (ntfs3)");
}

/* How far ntfsck is, from what it has written since offset: it goes
 * through 6 passes ("Parse #n"), each to "100.00 percent completed" */
static int ntfsck_percent(off_t offset)
{
    char buf[8192], *p;
    int fd = open(TOOL_LOG, O_RDONLY | O_CLOEXEC), pass = 1;
    double percent = 0;
    off_t size;
    ssize_t n;

    if (fd < 0)
        return 0;
    size = lseek(fd, 0, SEEK_END);
    if (size - offset > (off_t)sizeof(buf) - 1) /* the last pass shows in what came last */
        offset = size - (off_t)sizeof(buf) + 1;
    n = pread(fd, buf, sizeof(buf) - 1, offset);
    close(fd);
    buf[n > 0 ? n : 0] = 0;
    for (p = buf; (p = strstr(p, "Parse #")); p++)
        pass = atoi(p + 7);
    for (p = buf; (p = strstr(p, " percent completed")); p++) {
        char *start = p;
        while (start > buf && (start[-1] == '.' || (start[-1] >= '0' && start[-1] <= '9')))
            start--;
        percent = strtod(start, NULL);
    }
    if (pass < 1 || pass > 6)
        pass = 6;
    return (int)(((pass - 1) * 100 + percent) / 6);
}

/* Runs a tool of the initrd (ntfsprogs-plus) with its output in TOOL_LOG,
 * answer on its input; while it runs, progress gets how far it is (for
 * ntfsck). Returns its exit code, or -1. */
static int run_tool(char *const argv[], const char *answer, void (*progress)(int percent))
{
    int in[2], status, log = open(TOOL_LOG, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    off_t start = log >= 0 ? lseek(log, 0, SEEK_END) : 0;
    pid_t pid;

    if (pipe2(in, O_CLOEXEC))
        return -1;
    if (log >= 0) {
        dprintf(log, "\n[%s]", argv[0]);
        for (int i = 1; argv[i]; i++)
            dprintf(log, " %s", argv[i]);
        dprintf(log, "\n");
    }
    pid = fork();
    if (pid == 0) {
        char *envp[] = {"PATH=/bin", NULL};

        dup2(in[0], 0);
        if (log >= 0) {
            dup2(log, 1);
            dup2(log, 2);
        }
        execve(argv[0], argv, envp);
        _exit(127);
    }
    close(in[0]);
    if (answer && write(in[1], answer, strlen(answer)) < 0)
        say("%s: no answer: %s", argv[0], strerror(errno));
    close(in[1]);
    if (log >= 0)
        close(log);
    if (pid < 0)
        return -1;
    for (pid_t done = 0; !done;) {
        if ((done = waitpid(pid, &status, progress ? WNOHANG : 0)) < 0)
            return -1;
        if (!done) {
            progress(ntfsck_percent(start));
            usleep(100000);
        }
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* What Windows says under the spinner while chkdsk runs at boot */
static void check_progress(int percent)
{
    static int shown = -1;

    if (percent != shown)
        bootanim_send(anim, "status Сканування й відновлення диска (C:): виконано %d%%", percent);
    shown = percent;
}

/* A stick pulled out leaves C: marked dirty (the ntfs driver marks it while
 * mounted for writing). It is checked and repaired before it is mounted, as
 * chkdsk does at boot. */
static void check_c(const char *dev)
{
    char *dirty[] = {NTFSCK, "-C", (char *)dev, NULL};
    char *repair[] = {NTFSCK, "-a", (char *)dev, NULL};
    int rc;

    rc = run_tool(dirty, NULL, NULL);
    if (rc == 0)
        return;
    if (rc < 0 || rc == 127) {
        say("C: cannot be checked: no ntfsck");
        return;
    }
    say("C: is marked dirty, checking it");
    check_progress(0);
    rc = run_tool(repair, NULL, check_progress);
    check_progress(100);
    sleep(1); /* a check of a small C: takes a blink; the 100 % stays a moment, as in Windows */
    say("C: checked (ntfsck exit %d: %s)", rc,
        rc == 0 ? "no errors" : rc == 1 ? "errors fixed" : "errors left");
    bootanim_send(anim, "boot");
}

static uint64_t sys_number(const char *path)
{
    char text[32] = "";
    int fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd >= 0) {
        if (read(fd, text, sizeof(text) - 1) < 0)
            text[0] = 0;
        close(fd);
    }
    return strtoull(text, NULL, 10);
}

static uint32_t get_le32(const unsigned char *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* The partition of C: over the rest of the disk: its entry in the MBR, then
 * the kernel's view of it. Nothing when it is already as big as it can be. */
static void grow_partition(const char *dev)
{
    const char *name = strrchr(dev, '/') + 1;
    char path[PATH_MAX], disk[PATH_MAX];
    unsigned char mbr[512], *entry;
    uint64_t start, size, end;
    int num, fd;

    snprintf(path, sizeof(path), "/sys/class/block/%s/partition", name);
    num = (int)sys_number(path);
    snprintf(path, sizeof(path), "/sys/class/block/%s/start", name);
    start = sys_number(path);
    snprintf(path, sizeof(path), "/sys/class/block/%s/size", name);
    size = sys_number(path);
    snprintf(path, sizeof(path), "/sys/class/block/%s", name);
    if (num < 1 || num > 4 || !realpath(path, disk) || !strrchr(disk, '/'))
        return;
    *strrchr(disk, '/') = 0;
    snprintf(path, sizeof(path), "%s/size", disk);
    end = sys_number(path) & ~2047ull; /* whole MiB */
    if (end > 0xffffffffull)           /* as far as MBR reaches: 2 TiB */
        end = 0xffffffffull & ~2047ull;

    snprintf(path, sizeof(path), "/dev/%s", strrchr(disk, '/') + 1);
    if ((fd = open(path, O_RDWR | O_CLOEXEC)) < 0 || pread(fd, mbr, sizeof(mbr), 0) != (ssize_t)sizeof(mbr) ||
        mbr[510] != 0x55 || mbr[511] != 0xaa || mbr[446 + 4] == 0xee) {
        say("C: not grown: no MBR on %s", path);
        if (fd >= 0)
            close(fd);
        return;
    }
    entry = mbr + 446 + 16 * (num - 1);
    if (get_le32(entry + 8) != start || get_le32(entry + 12) != size) {
        say("C: not grown: the MBR does not describe it as the kernel does");
        close(fd);
        return;
    }
    for (int i = 0; i < 4; i++) {
        const unsigned char *other = mbr + 446 + 16 * i;
        if (i != num - 1 && get_le32(other + 12) && get_le32(other + 8) >= start + size) {
            say("C: not grown: partition %d follows it", i + 1);
            close(fd);
            return;
        }
    }
    if (end < start + size + 64 * 2048) { /* it fills the disk already */
        close(fd);
        return;
    }

    uint32_t sectors = (uint32_t)(end - start);
    memcpy(entry + 12, (unsigned char[]){sectors, sectors >> 8, sectors >> 16, sectors >> 24}, 4);
    entry[5] = 0xfe; /* CHS of the end: past what CHS can say, as for every partition today */
    entry[6] = 0xff;
    entry[7] = 0xff;
    struct blkpg_partition part = {.start = (long long)(start * 512), .length = (long long)(end - start) * 512,
                                   .pno = num};
    struct blkpg_ioctl_arg arg = {.op = BLKPG_RESIZE_PARTITION, .datalen = sizeof(part), .data = &part};
    if (pwrite(fd, mbr, sizeof(mbr), 0) != (ssize_t)sizeof(mbr) || fsync(fd) || ioctl(fd, BLKPG, &arg)) {
        say("C: not grown: %s", strerror(errno));
        close(fd);
        return;
    }
    close(fd);
    say("C: partition grew from %llu to %llu MB", (unsigned long long)(size >> 11),
        (unsigned long long)((end - start) >> 11));
}

/* The first start of a stick written from the image: C: is as small as the
 * image was and grows over the whole stick, the partition first, then the
 * NTFS volume to the partition's size. Returns 0 once that is done, -1 to
 * try again on the next start. */
static int grow_c(const char *dev)
{
    char *resize[] = {NTFSRESIZE, "-f", "-P", (char *)dev, NULL}; /* no size: as big as the partition */
    char *check[] = {NTFSCK, "-a", (char *)dev, NULL};            /* ntfsresize leaves it marked for one */
    int rc;

    bootanim_send(anim, "status Підготовка пристрою"); /* what Windows' first start says */
    grow_partition(dev);
    rc = run_tool(resize, "y\n", NULL);
    run_tool(check, NULL, NULL);
    say("C: volume over the partition (ntfsresize exit %d)", rc);
    bootanim_send(anim, "boot");
    return rc ? -1 : 0;
}

/* Updates. An update lies in Host\Update: host.sqfs, Boot (the files of the
 * EFI system partition: shim, Limine, its config, the kernel and the initrd,
 * signed together) and "ready", which names the size host.sqfs must have,
 * written last. The initrd installs it before anything runs from C:: the
 * running version goes to Host\Previous, the new one takes its place, the
 * EFI system partition is written from C:\Windows\Boot\Arctic as bcdboot
 * writes it from C:\Windows\Boot, and the machine restarts into the new
 * kernel. "Undo the last update" (arctic.rollback=1) swaps the two. */
#define HOST_DIR CDRIVE "/Windows/System32/Host"
#define UPDATE_DIR HOST_DIR "/Update"
#define PREVIOUS_DIR HOST_DIR "/Previous"
#define BOOT_SET CDRIVE "/Windows/Boot/Arctic"
#define ESP "/esp"

static int copy_file(const char *from, const char *to)
{
    char buf[1 << 16];
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

static int copy_tree(const char *from, const char *to)
{
    char a[PATH_MAX], b[PATH_MAX];
    struct dirent *de;
    struct stat st;
    DIR *dir;
    int rc = 0;

    if (stat(from, &st))
        return -1;
    if (!S_ISDIR(st.st_mode))
        return copy_file(from, to);
    if ((mkdir(to, 0755) && errno != EEXIST) || !(dir = opendir(from)))
        return -1;
    while (!rc && (de = readdir(dir))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        snprintf(a, sizeof(a), "%s/%s", from, de->d_name);
        snprintf(b, sizeof(b), "%s/%s", to, de->d_name);
        rc = copy_tree(a, b);
    }
    closedir(dir);
    return rc;
}

static void remove_tree(const char *path)
{
    char sub[PATH_MAX];
    struct dirent *de;
    DIR *dir;

    if (!unlink(path) || !(dir = opendir(path)))
        return;
    while ((de = readdir(dir))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        snprintf(sub, sizeof(sub), "%s/%s", path, de->d_name);
        remove_tree(sub);
    }
    closedir(dir);
    rmdir(path);
}

/* The EFI system partition on C:'s disk: the MBR entry of type 0xEF */
static int find_esp(const char *c_dev, char *esp, size_t len)
{
    char path[PATH_MAX], disk[PATH_MAX];
    unsigned char mbr[512];
    struct dirent *de;
    int fd, num = 0;
    DIR *dir;

    snprintf(path, sizeof(path), "/sys/class/block/%s", strrchr(c_dev, '/') + 1);
    if (!realpath(path, disk) || !strrchr(disk, '/'))
        return -1;
    *strrchr(disk, '/') = 0;
    snprintf(path, sizeof(path), "/dev/%s", strrchr(disk, '/') + 1);
    if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
        return -1;
    if (pread(fd, mbr, sizeof(mbr), 0) == (ssize_t)sizeof(mbr) && mbr[510] == 0x55 && mbr[511] == 0xaa)
        for (int i = 0; i < 4 && !num; i++)
            if (mbr[446 + 16 * i + 4] == 0xef)
                num = i + 1;
    close(fd);
    if (!num || !(dir = opendir(disk)))
        return -1;
    while ((de = readdir(dir))) {
        snprintf(path, sizeof(path), "%s/%s/partition", disk, de->d_name);
        if (de->d_name[0] != '.' && (int)sys_number(path) == num) {
            snprintf(esp, len, "/dev/%s", de->d_name);
            closedir(dir);
            return 0;
        }
    }
    closedir(dir);
    return -1;
}

/* Arctic's bcdboot: the EFI system partition gets the boot files of the
 * version on C:. They are copied in beside the old ones first and then
 * renamed into place, a directory at a time: Limine, its config and the
 * kernel only work as one signed set. */
static int write_esp(const char *c_dev)
{
    static const char *const items[] = {"EFI", "boot", "ARCTIC.cer"};
    char esp[300], a[PATH_MAX], b[PATH_MAX];
    int rc = -1;

    if (find_esp(c_dev, esp, sizeof(esp))) {
        say("bcdboot: no EFI system partition next to C:");
        return -1;
    }
    mkdir(ESP, 0755);
    if (mount(esp, ESP, "vfat", MS_NOATIME, NULL)) {
        say("bcdboot: mount %s: %s", esp, strerror(errno));
        return -1;
    }
    remove_tree(ESP "/staging");
    remove_tree(ESP "/old");
    if (copy_tree(BOOT_SET, ESP "/staging")) {
        say("bcdboot: copying the boot files: %s", strerror(errno));
        goto done;
    }
    sync();
    mkdir(ESP "/old", 0755);
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        snprintf(a, sizeof(a), ESP "/%s", items[i]);
        snprintf(b, sizeof(b), ESP "/old/%s", items[i]);
        rename(a, b);
        snprintf(b, sizeof(b), ESP "/staging/%s", items[i]);
        if (rename(b, a))
            say("bcdboot: %s: %s", items[i], strerror(errno));
    }
    sync();
    remove_tree(ESP "/old");
    remove_tree(ESP "/staging");
    rc = 0;
    say("bcdboot: %s has the boot files of C:", esp);
done:
    umount(ESP);
    sync();
    return rc;
}

/* the kernel and the initrd also lie by the host, as Windows keeps its own */
static void copy_kernel(void)
{
    copy_file(BOOT_SET "/EFI/Arctic/vmlinuz", HOST_DIR "/vmlinuz");
    copy_file(BOOT_SET "/EFI/Arctic/initrd.img", HOST_DIR "/initrd.img");
}

/* Windows' screen while it installs updates before it starts */
static void update_screen(const char *what, int percent)
{
    bootanim_send(anim, "update %s, виконано %d%%\tНе вимикайте комп’ютер", what, percent);
}

#define INSTALLING "Робота з оновленнями"
#define UNDOING "Скасування змін, внесених до комп’ютера"

static void restart_now(void)
{
    say("restarting into the version now on C:");
    sleep(1); /* the 100 % stays a moment, as in Windows */
    mount(NULL, CDRIVE, NULL, MS_REMOUNT | MS_RDONLY, NULL);
    umount(CDRIVE);
    sync();
    reboot(RB_AUTOBOOT);
}

/* a rename that swaps two paths through a third */
static int swap_paths(const char *a, const char *b, const char *via)
{
    if (rename(a, via))
        return -1;
    if (rename(b, a)) {
        rename(via, a);
        return -1;
    }
    return rename(via, b);
}

static void install_update(const char *dev)
{
    char ready[64] = "";
    struct stat st;
    int fd;

    if ((fd = open(UPDATE_DIR "/ready", O_RDONLY | O_CLOEXEC)) < 0)
        return;
    if (read(fd, ready, sizeof(ready) - 1) < 0)
        ready[0] = 0;
    close(fd);
    if (stat(UPDATE_DIR "/host.sqfs", &st) || (unsigned long long)st.st_size != strtoull(ready, NULL, 10) ||
        access(UPDATE_DIR "/Boot/EFI/Arctic/vmlinuz", R_OK) || access(UPDATE_DIR "/Boot/boot/limine/limine.conf", R_OK)) {
        say("update: incomplete, left as it is");
        return;
    }
    say("update: installing");
    update_screen(INSTALLING, 0);
    remove_tree(PREVIOUS_DIR);
    mkdir(PREVIOUS_DIR, 0755);
    if (rename(HOST_DIR "/host.sqfs", PREVIOUS_DIR "/host.sqfs") ||
        (rename(BOOT_SET, PREVIOUS_DIR "/Boot") && errno != ENOENT) ||
        rename(UPDATE_DIR "/host.sqfs", HOST_DIR "/host.sqfs") || rename(UPDATE_DIR "/Boot", BOOT_SET)) {
        say("update: %s; the version before stays", strerror(errno));
        rename(PREVIOUS_DIR "/host.sqfs", HOST_DIR "/host.sqfs");
        rename(PREVIOUS_DIR "/Boot", BOOT_SET);
        bootanim_send(anim, "boot");
        return;
    }
    update_screen(INSTALLING, 30);
    remove_tree(UPDATE_DIR);
    copy_kernel();
    sync();
    update_screen(INSTALLING, 60);
    write_esp(dev);
    update_screen(INSTALLING, 100);
    restart_now();
}

static void undo_update(const char *dev)
{
    if (access(PREVIOUS_DIR "/host.sqfs", R_OK) || access(PREVIOUS_DIR "/Boot/EFI/Arctic/vmlinuz", R_OK)) {
        say("rollback: there is no version before this one");
        return;
    }
    say("rollback: going back to the version before");
    update_screen(UNDOING, 0);
    if (swap_paths(HOST_DIR "/host.sqfs", PREVIOUS_DIR "/host.sqfs", HOST_DIR "/host.sqfs.swap")) {
        say("rollback: %s", strerror(errno));
        bootanim_send(anim, "boot");
        return;
    }
    if (swap_paths(BOOT_SET, PREVIOUS_DIR "/Boot", CDRIVE "/Windows/Boot/Arctic.swap")) {
        say("rollback: %s", strerror(errno));
        swap_paths(HOST_DIR "/host.sqfs", PREVIOUS_DIR "/host.sqfs", HOST_DIR "/host.sqfs.swap");
        bootanim_send(anim, "boot");
        return;
    }
    update_screen(UNDOING, 30);
    copy_kernel();
    sync();
    update_screen(UNDOING, 60);
    write_esp(dev);
    update_screen(UNDOING, 100);
    restart_now();
}

/* what the tools said goes with the other logs on C: */
static void keep_tool_log(void)
{
    char buf[4096];
    ssize_t n;
    int in = open(TOOL_LOG, O_RDONLY | O_CLOEXEC), out;

    if (in < 0)
        return;
    mkdir(CDRIVE "/Windows/Logs", 0755);
    mkdir(CDRIVE "/Windows/Logs/Arctic", 0755);
    out = open(CDRIVE "/Windows/Logs/Arctic/chkdsk.log", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    while (out >= 0 && (n = read(in, buf, sizeof(buf))) > 0)
        if (write(out, buf, (size_t)n) != n)
            break;
    if (out >= 0)
        close(out);
    close(in);
}

/* A stick or a disk: C: is the partition */
static void partition_c_drive(void)
{
    char dev[300];

    find_root(dev, sizeof(dev));
    if (live) {
        say("live: the changes to C: stay in memory");
        snapshot_c(dev, "C:");
        mount_c(C_DEV, 0);
        return;
    }
    /* Written-to data reaches the stick within seconds, not half a minute:
     * a stick pulled out loses little, and the kernel's NTFS has no journal */
    write_file("/proc/sys/vm/dirty_expire_centisecs", "300");
    write_file("/proc/sys/vm/dirty_writeback_centisecs", "100");
    check_c(dev);
    mount_c(dev, MS_NOATIME);
    if (!access(CDRIVE FIRST_START, F_OK) && !umount(CDRIVE)) {
        int grown = grow_c(dev);
        mount_c(dev, MS_NOATIME);
        if (!grown)
            unlink(CDRIVE FIRST_START);
    }
    rmdir(CDRIVE "/lost+found"); /* ntfsck makes one; Windows has none, so it goes while empty */
    keep_tool_log();
    if (strstr(cmdline, "arctic.rollback=1"))
        undo_update(dev);
    install_update(dev);
}

/* The ISO: C: is windows.img in windows.sqfs, under a snapshot */
static void image_c_drive(void)
{
    char loop[64], img[64];

    if (loop_attach(MEDIA "/arctic/windows.sqfs", loop, sizeof(loop)) ||
        mount(loop, WINDOWS, "squashfs", MS_RDONLY, NULL))
        fatal("UNMOUNTABLE_BOOT_VOLUME", "windows.sqfs", "mount windows.sqfs");
    if (loop_attach(WINDOWS "/windows.img", img, sizeof(img)))
        fatal("UNMOUNTABLE_BOOT_VOLUME", "windows.img", "attach windows.img");
    snapshot_c(img, "windows.img");
    mount_c(C_DEV, 0);
}

static void move_into(const char *from, const char *to)
{
    if (mount(from, to, NULL, MS_MOVE, NULL))
        fatal("PHASE1_INITIALIZATION_FAILED", "initrd", to);
}

int main(void)
{
    char loop[64];
    char animenv[48];

    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    kmsg = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    console = open("/dev/console", O_WRONLY | O_CLOEXEC);
    read_cmdline();
    say("start (dev=%d)", dev_mode);

    /* the boot screen, from now until the desktop */
    anim = bootanim_start("/spinner.bin", "/logo.bgra", FONT, &anim_pid);
    if (anim < 0)
        say("no boot screen: %s", strerror(errno));

    if (strstr(cmdline, "arctic.stoptest=initrd")) {
        sleep(1);
        errno = 0;
        fatal("MANUALLY_INITIATED_CRASH", "arctic.stoptest", "stop screen test");
    }

    mkdir(MEDIA, 0755);
    mkdir(WINDOWS, 0755);
    mkdir(CDRIVE, 0755);
    mkdir(NEWROOT, 0755);
    if (root_partuuid[0]) {
        partition_c_drive();
    } else {
        find_media();
        image_c_drive();
    }

    /* the host: on a stick it lies in C:\Windows, on the ISO next to C: */
    const char *host = root_partuuid[0] ? CDRIVE HOST_SQFS : MEDIA "/arctic/host.sqfs";
    if (loop_attach(host, loop, sizeof(loop)))
        fatal("INACCESSIBLE_BOOT_DEVICE", "host.sqfs", "attach host.sqfs");
    if (mount(loop, NEWROOT, "squashfs", MS_RDONLY, NULL))
        fatal("INACCESSIBLE_BOOT_DEVICE", "host.sqfs", "mount host.sqfs");
    if (mount("tmpfs", NEWROOT "/run", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755") ||
        mount("tmpfs", NEWROOT "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777"))
        fatal("PHASE1_INITIALIZATION_FAILED", "tmpfs", "tmpfs");
    mkdir(NEWROOT "/run/arctic", 0755);
    if (!root_partuuid[0]) {
        mkdir(NEWROOT "/run/arctic/media", 0755);
        mkdir(NEWROOT "/run/arctic/windows", 0755);
        move_into(MEDIA, NEWROOT "/run/arctic/media");
        move_into(WINDOWS, NEWROOT "/run/arctic/windows");
    }
    move_into(CDRIVE, NEWROOT "/mnt/c");

    move_into("/dev", NEWROOT "/dev");
    move_into("/proc", NEWROOT "/proc");
    move_into("/sys", NEWROOT "/sys");

    if (chdir(NEWROOT) || mount(".", "/", NULL, MS_MOVE, NULL) || chroot(".") || chdir("/"))
        fatal("PHASE1_INITIALIZATION_FAILED", "initrd", "switch root");

    /* arctic-init takes over the boot screen's commands and ends it */
    if (anim >= 0)
        fcntl(anim, F_SETFD, 0);
    snprintf(animenv, sizeof(animenv), "ARCTIC_BOOTANIM=%d:%d", anim, (int)anim_pid);
    char *argv[] = {"/usr/bin/arctic-init", NULL};
    /* arctic-init keeps logs and shuts down by whether C: keeps what is written */
    char *envp[] = {"PATH=/usr/bin", animenv, root_partuuid[0] && !live ? "ARCTIC_C=disk" : "ARCTIC_C=memory", NULL};
    say("handing over to arctic-init");
    execve(argv[0], argv, envp);
    fatal("CRITICAL_PROCESS_DIED", "arctic-init", "exec arctic-init");
    return 1;
}
