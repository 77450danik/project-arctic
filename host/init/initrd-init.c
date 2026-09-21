/* /init of the Arctic initrd.
 *
 * Shows the logo, finds the boot medium, mounts the host image, builds C:
 * (an NTFS image under a device-mapper snapshot whose changes live in zram)
 * and hands over to /usr/bin/arctic-init. Everything it needs is built into
 * the kernel, so the initrd carries no modules. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/dm-ioctl.h>
#include <linux/fs.h>
#include <linux/loop.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#define WINDOWS_IMG NEWROOT "/usr/share/arctic/windows.img"
#define C_DEV "/dev/arctic-c"
#define NT_OPTS "uid=1000,gid=1000,umask=022"
#define FONT "/bsod.font"

static int kmsg = -1, splash = -1, dev_mode;
static char cmdline[4096];

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
    say("%s: %s", detail, strerror(errno));
    if (kmsg >= 0)
        dprintf(kmsg, "<3>ARCTIC: STOP %s (%s)", code, what);
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
            if (de->d_name[0] == '.' || !strncmp(de->d_name, "loop", 4) || !strncmp(de->d_name, "ram", 3) ||
                !strncmp(de->d_name, "zram", 4) || !strncmp(de->d_name, "dm-", 3))
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

static int loop_attach(const char *file, char *dev, size_t len)
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
    ffd = open(file, O_RDONLY | O_CLOEXEC);
    if (lfd < 0 || ffd < 0)
        return -1;
    memset(&cfg, 0, sizeof(cfg));
    cfg.fd = (uint32_t)ffd;
    cfg.info.lo_flags = LO_FLAGS_READ_ONLY;
    num = ioctl(lfd, LOOP_CONFIGURE, &cfg);
    close(ffd);
    close(lfd);
    return num;
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

static void setup_c_drive(void)
{
    char loop[64];
    struct stat st_loop, st_zram;
    struct sysinfo si;
    uint64_t bytes = 0;
    dev_t cdev;
    char size[32];
    int fd;

    if (loop_attach(WINDOWS_IMG, loop, sizeof(loop)))
        fatal("UNMOUNTABLE_BOOT_VOLUME", "windows.img", "attach windows.img");
    fd = open(loop, O_RDONLY | O_CLOEXEC);
    if (fd < 0 || ioctl(fd, BLKGETSIZE64, &bytes))
        fatal("UNMOUNTABLE_BOOT_VOLUME", "windows.img", "size of windows.img");
    close(fd);

    /* The zram disk is only as big as the RAM, and it only uses RAM for what is written */
    sysinfo(&si);
    write_file("/sys/block/zram0/comp_algorithm", "zstd");
    snprintf(size, sizeof(size), "%llu", (unsigned long long)si.totalram * si.mem_unit);
    if (write_file("/sys/block/zram0/disksize", size))
        fatal("UNMOUNTABLE_BOOT_VOLUME", "zram", "zram disksize");

    if (stat(loop, &st_loop) || stat("/dev/zram0", &st_zram))
        fatal("UNMOUNTABLE_BOOT_VOLUME", "zram", "stat loop/zram");
    if (dm_snapshot("arctic-c", st_loop.st_rdev, st_zram.st_rdev, bytes / 512, &cdev))
        fatal("UNMOUNTABLE_BOOT_VOLUME", "dm-snapshot", "device-mapper snapshot");
    if (mknod(C_DEV, S_IFBLK | 0600, cdev) && errno != EEXIST)
        fatal("UNMOUNTABLE_BOOT_VOLUME", "dm-snapshot", "mknod C:");

    if (!mount(C_DEV, NEWROOT "/mnt/c", "ntfs", 0, "nocase,windows_names," NT_OPTS)) {
        say("C: mounted (ntfs, nocase)");
        return;
    }
    say("ntfs mount failed (%s), trying ntfs3", strerror(errno));
    if (mount(C_DEV, NEWROOT "/mnt/c", "ntfs3", 0, NT_OPTS))
        fatal("UNMOUNTABLE_BOOT_VOLUME", "ntfs", "mount C:");
    say("C: mounted (ntfs3)");
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
    mkdir(NEWROOT, 0755);
    find_media();

    if (loop_attach(MEDIA "/arctic/host.sqfs", loop, sizeof(loop)))
        fatal("INACCESSIBLE_BOOT_DEVICE", "host.sqfs", "attach host.sqfs");
    if (mount(loop, NEWROOT, "squashfs", MS_RDONLY, NULL))
        fatal("INACCESSIBLE_BOOT_DEVICE", "host.sqfs", "mount host.sqfs");
    if (mount("tmpfs", NEWROOT "/run", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755") ||
        mount("tmpfs", NEWROOT "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777"))
        fatal("PHASE1_INITIALIZATION_FAILED", "tmpfs", "tmpfs");
    mkdir(NEWROOT "/run/arctic", 0755);
    mkdir(NEWROOT "/run/arctic/media", 0755);
    move_into(MEDIA, NEWROOT "/run/arctic/media");

    setup_c_drive();

    move_into("/dev", NEWROOT "/dev");
    move_into("/proc", NEWROOT "/proc");
    move_into("/sys", NEWROOT "/sys");

    if (chdir(NEWROOT) || mount(".", "/", NULL, MS_MOVE, NULL) || chroot(".") || chdir("/"))
        fatal("PHASE1_INITIALIZATION_FAILED", "initrd", "switch root");

    snprintf(fdenv, sizeof(fdenv), "ARCTIC_SPLASH_FD=%d", splash);
    char *argv[] = {"/usr/bin/arctic-init", NULL};
    char *envp[] = {"PATH=/usr/bin", fdenv, NULL};
    say("handing over to arctic-init");
    execve(argv[0], argv, envp);
    fatal("CRITICAL_PROCESS_DIED", "arctic-init", "exec arctic-init");
    return 1;
}
