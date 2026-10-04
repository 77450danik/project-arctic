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
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "splash.h"
#include "stop.h"

#define NEWROOT "/newroot"
#define MEDIA "/media"
#define WINDOWS "/windows" /* windows.sqfs of the ISO */
#define CDRIVE "/c"        /* C: until it moves into the new root */
#define HOST_SQFS "/Windows/System32/Host/host.sqfs"
#define C_DEV "/dev/arctic-c"
#define NT_OPTS "uid=1000,gid=1000,umask=022"
#define FONT "/bsod.font"

static int kmsg = -1, console = -1, splash = -1, dev_mode, live;
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
    stop_screen(splash, FONT, code, what, dev_mode);
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

/* The partition that is C:. USB sticks can take several seconds to appear. */
static void find_root(char *dev, size_t len)
{
    for (int attempt = 0; attempt < 150; attempt++) {
        DIR *dir = opendir("/sys/class/block");
        struct dirent *de;
        char uuid[40];

        while (dir && (de = readdir(dir))) {
            if (skip_block_device(de->d_name) || mbr_partuuid(de->d_name, uuid, sizeof(uuid)) ||
                strcasecmp(uuid, root_partuuid))
                continue;
            snprintf(dev, len, "/dev/%s", de->d_name);
            say("C: is %s (PARTUUID %s)", dev, uuid);
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
    mount_c(dev, MS_NOATIME);
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
    char fdenv[32];

    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    kmsg = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    console = open("/dev/console", O_WRONLY | O_CLOEXEC);
    read_cmdline();
    say("start (dev=%d)", dev_mode);

    for (int i = 0; i < 20 && splash < 0; i++) {
        splash = splash_show("/dev/dri/card0", "/logo.bgra");
        if (splash < 0)
            usleep(100000);
    }
    say(splash >= 0 ? "logo on /dev/dri/card0" : "no framebuffer for the logo");

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

    snprintf(fdenv, sizeof(fdenv), "ARCTIC_SPLASH_FD=%d", splash);
    char *argv[] = {"/usr/bin/arctic-init", NULL};
    /* arctic-init keeps logs and shuts down by whether C: keeps what is written */
    char *envp[] = {"PATH=/usr/bin", fdenv, root_partuuid[0] && !live ? "ARCTIC_C=disk" : "ARCTIC_C=memory", NULL};
    say("handing over to arctic-init");
    execve(argv[0], argv, envp);
    fatal("CRITICAL_PROCESS_DIED", "arctic-init", "exec arctic-init");
    return 1;
}
