/* arctic-volume: udev runs it for every block device that comes, changes or
 * goes. A volume Windows would give a drive letter is mounted under
 * /run/arctic/drives/<name> and described in /run/arctic/volumes/<name>;
 * mountmgr.sys watches that folder and gives the volume its letter.
 *
 *   arctic-volume add     the device in DEVNAME came or changed (udev's
 *                         ID_FS_* properties say what file system it has)
 *   arctic-volume remove  the device is gone
 *
 * A USB disk nothing of the host's is on also goes to the user nt, for the
 * programs of Windows that write disks directly (docs/disks.md): the disk is
 * described in /run/arctic/disks/<name>, and each of its partitions gets a
 * record even with no file system on it (mount= is empty then).
 *
 * Hidden, as Windows hides them: EFI system, MSR and recovery partitions,
 * partitions marked "no drive letter", and what the host itself uses (the
 * boot medium, C: and the devices under it). */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "lxss-binds.h"

#define DRIVES_DIR "/run/arctic/drives"
#define VOLUMES_DIR "/run/arctic/volumes"
#define DISKS_DIR "/run/arctic/disks"
#define HOLD_DIR "/run/arctic/hold" /* made by mountmgr.sys while a program has a volume locked */
#define NT_USER "nt"

static void say(const char *fmt, ...)
{
    char buf[512];
    int len = snprintf(buf, sizeof(buf), "arctic-volume: ");
    int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    va_list ap;

    va_start(ap, fmt);
    len += vsnprintf(buf + len, sizeof(buf) - len - 1, fmt, ap);
    va_end(ap);
    if (len > (int)sizeof(buf) - 2)
        len = sizeof(buf) - 2;
    buf[len++] = '\n';
    if (fd >= 0) {
        write(fd, buf, len);
        close(fd);
    }
}

static const char *env(const char *name)
{
    const char *value = getenv(name);
    return value ? value : "";
}

/* the partition types Windows gives no letter */
static int hidden_partition(void)
{
    static const char *const types[] = {
        "c12a7328-f81f-11d2-ba4b-00a0c93ec93b", /* EFI system */
        "e3c9e316-0b5c-4db8-817d-f92df00215ae", /* Microsoft reserved */
        "de94bba4-06d1-4d40-a16a-bfd50179d6ac", /* Windows recovery */
        "0xef",                                 /* EFI system, MBR */
        "0x27",                                 /* recovery, MBR */
        NULL,
    };
    const char *type = env("ID_PART_ENTRY_TYPE");

    for (int i = 0; types[i]; i++)
        if (!strcasecmp(type, types[i]))
            return 1;
    /* GPT attribute 63: no drive letter */
    return strtoull(env("ID_PART_ENTRY_FLAGS"), NULL, 16) >> 63;
}

/* the partition types Windows gives a letter with no file system on them yet */
static int recognized_partition(void)
{
    static const char *const types[] = {
        "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7", /* basic data, GPT */
        "0x1", "0x4", "0x6", "0x7", "0xb", "0xc", "0xe", /* FAT and IFS, MBR */
        NULL,
    };
    const char *type = env("ID_PART_ENTRY_TYPE");

    for (int i = 0; types[i]; i++)
        if (!strcasecmp(type, types[i]))
            return 1;
    return 0;
}

/* the sysfs folder of the disk the device is on (the device itself for a
 * whole disk) */
static int disk_dir(const char *name, char *out)
{
    char path[PATH_MAX];

    snprintf(path, sizeof(path), "/sys/class/block/%s", name);
    if (!realpath(path, out))
        return -1;
    snprintf(path, sizeof(path), "%s/partition", out);
    if (!access(path, F_OK))
        *strrchr(out, '/') = 0;
    return 0;
}

static int read_dev(const char *dir, dev_t *dev)
{
    char path[PATH_MAX];
    unsigned int major, minor;
    int ok = 0;
    FILE *f;

    snprintf(path, sizeof(path), "%s/dev", dir);
    if ((f = fopen(path, "re"))) {
        ok = fscanf(f, "%u:%u", &major, &minor) == 2;
        fclose(f);
    }
    if (ok)
        *dev = makedev(major, minor);
    return ok;
}

static int has_holders(const char *dir)
{
    char path[PATH_MAX];
    struct dirent *de;
    int held = 0;
    DIR *d;

    snprintf(path, sizeof(path), "%s/holders", dir);
    if ((d = opendir(path))) {
        while (!held && (de = readdir(d)))
            held = de->d_name[0] != '.';
        closedir(d);
    }
    return held;
}

/* the host's own devices: loop, device-mapper, zram and NBD (the WSL disks
 * arctic-lxss attaches), and what the host
 * uses: a device-mapper table holds it (C:'s origin) or it is mounted
 * outside DRIVES_DIR. When that is a whole disk (an ISO written to a stick:
 * the whole disk and two partitions at once) all of the disk goes; when it
 * is a partition (C:), that partition, and the rest of the disk keeps its
 * letters (installed next to D:, D: stays) */
static int host_device(const char *name)
{
    char disk[PATH_MAX], part[PATH_MAX + 512], line[1024];
    dev_t devs[2];
    int n = 0, used = 0;
    FILE *f;

    if (!strncmp(name, "loop", 4) || !strncmp(name, "dm-", 3) || !strncmp(name, "zram", 4) ||
        !strncmp(name, "ram", 3) || !strncmp(name, "nbd", 3))
        return 1;
    if (disk_dir(name, disk))
        return 0;

    if (read_dev(disk, &devs[n]))
        n++;
    used = has_holders(disk);
    snprintf(part, sizeof(part), "%s/%s/partition", disk, name);
    if (!access(part, F_OK)) {
        *strrchr(part, '/') = 0;
        used |= has_holders(part);
        if (read_dev(part, &devs[n]))
            n++;
    }

    if ((f = fopen("/proc/self/mountinfo", "re"))) {
        unsigned int major, minor;
        while (!used && fgets(line, sizeof(line), f)) {
            /* the drive letters and their binds in the WSL distributions are not the host's use */
            if (sscanf(line, "%*u %*u %u:%u", &major, &minor) != 2 || strstr(line, " " DRIVES_DIR "/") ||
                strstr(line, " " LXSS_ROOTS))
                continue;
            for (int i = 0; i < n; i++)
                used |= devs[i] == makedev(major, minor);
        }
        fclose(f);
    }
    return used;
}

/* "removable", "cdrom" or "fixed", as Windows tells them apart: a USB stick
 * says its medium is removable, a USB hard disk does not */
static const char *drive_type(const char *name)
{
    char path[PATH_MAX + 16], disk[PATH_MAX];
    int removable = 0;
    FILE *f;

    if (!strcmp(env("ID_CDROM"), "1") || !strncmp(name, "sr", 2))
        return "cdrom";
    if (disk_dir(name, disk))
        return "fixed";
    snprintf(path, sizeof(path), "%s/removable", disk);
    if ((f = fopen(path, "re"))) {
        removable = fgetc(f) == '1';
        fclose(f);
    }
    return removable ? "removable" : "fixed";
}

static void read_line(const char *path, char *out, size_t size)
{
    FILE *f = fopen(path, "re");

    out[0] = 0;
    if (f) {
        if (fgets(out, size, f))
            out[strcspn(out, "\n")] = 0;
        fclose(f);
    }
}

/* the USB device the disk sits on, if it does: the folder above it in sysfs
 * with idVendor. Its product string is the name Windows shows in the Safely
 * Remove Hardware menu. */
static int usb_device(const char *disk, char *dir, char *product, size_t size)
{
    char path[PATH_MAX + 16];

    snprintf(dir, PATH_MAX, "%s", disk);
    for (char *slash; (slash = strrchr(dir, '/')) && slash != dir;) {
        *slash = 0;
        snprintf(path, sizeof(path), "%s/idVendor", dir);
        if (!access(path, F_OK)) {
            snprintf(path, sizeof(path), "%s/product", dir);
            read_line(path, product, size);
            return 1;
        }
    }
    return 0;
}

/* ID_FS_LABEL_ENC keeps the label as it is, with \xNN for special bytes */
static void decode_label(const char *in, char *out, size_t size)
{
    size_t n = 0;

    while (*in && n + 1 < size) {
        unsigned int c;
        if (in[0] == '\\' && in[1] == 'x' && sscanf(in + 2, "%2x", &c) == 1) {
            out[n++] = c;
            in += 4;
        } else {
            out[n++] = *in++;
        }
    }
    out[n] = 0;
}

static int try_mount(const char *dev, const char *dir, const char *fs, unsigned long flags,
                     const char *opts)
{
    if (!mount(dev, dir, fs, MS_NOSUID | MS_NODEV | flags, opts))
        return 0;
    say("%s: %s %s(%s): %s", dev, fs, flags & MS_RDONLY ? "read-only " : "", opts, strerror(errno));
    return -1;
}

static int mount_volume(const char *dev, const char *dir, const char *fs, int *read_only)
{
    struct passwd *pw = getpwnam(NT_USER);
    unsigned int uid = pw ? pw->pw_uid : 1000, gid = pw ? pw->pw_gid : 1000;
    char opts[256];

    *read_only = 0;
    if (!strcmp(fs, "ntfs")) {
        snprintf(opts, sizeof(opts), "nocase,windows_names,uid=%u,gid=%u,umask=022", uid, gid);
    } else if (!strcmp(fs, "vfat")) {
        snprintf(opts, sizeof(opts), "uid=%u,gid=%u,umask=022,utf8,shortname=mixed,flush", uid, gid);
    } else if (!strcmp(fs, "exfat")) {
        snprintf(opts, sizeof(opts), "uid=%u,gid=%u,umask=022,iocharset=utf8", uid, gid);
    } else if (!strcmp(fs, "iso9660")) {
        snprintf(opts, sizeof(opts), "uid=%u,gid=%u,iocharset=utf8", uid, gid);
        *read_only = 1;
    } else if (!strcmp(fs, "udf")) {
        snprintf(opts, sizeof(opts), "uid=%u,gid=%u,utf8", uid, gid);
        *read_only = 1;
    } else if (!strcmp(fs, "ext2") || !strcmp(fs, "ext3") || !strcmp(fs, "ext4")) {
        /* a Linux next to Windows on the same PC; its files keep their owners */
        fs = "ext4";
        opts[0] = 0;
    } else if (!strcmp(fs, "btrfs")) {
        opts[0] = 0;
    } else {
        return -1;
    }

    if (!*read_only && !try_mount(dev, dir, fs, 0, opts))
        return 0;
    /* a volume the driver will not write to (a hibernated Windows, errors to
     * repair) is still shown, read-only */
    *read_only = 1;
    return try_mount(dev, dir, fs, MS_RDONLY, opts);
}

static void remove_volume(const char *name)
{
    char path[PATH_MAX];

    snprintf(path, sizeof(path), VOLUMES_DIR "/%s", name);
    if (!unlink(path))
        say("%s removed", name);
    release_lxss_binds(name, 1);
    snprintf(path, sizeof(path), DRIVES_DIR "/%s", name);
    /* the medium may be gone already: detach, nothing can be flushed to it */
    umount2(path, MNT_DETACH);
    rmdir(path);
}

/* the node goes to the NT world; udev made it root's */
static void give_to_nt(const char *dev)
{
    struct passwd *pw = getpwnam(NT_USER);

    if (pw)
        chown(dev, pw->pw_uid, pw->pw_gid);
}

/* the disk or any of its partitions is the host's own */
static int disk_in_use(const char *name, const char *dir)
{
    struct dirent *de;
    int used = host_device(name);
    DIR *d;

    if (used)
        return 1;
    if (!(d = opendir(dir)))
        return 1;
    while (!used && (de = readdir(d))) {
        char path[PATH_MAX + 512];

        if (strncmp(de->d_name, name, strlen(name)))
            continue;
        snprintf(path, sizeof(path), "%s/%s/partition", dir, de->d_name);
        if (!access(path, F_OK))
            used = host_device(de->d_name);
    }
    closedir(d);
    return used;
}

static void read_trimmed(const char *path, char *out, size_t size)
{
    size_t len;

    read_line(path, out, size);
    len = strlen(out);
    while (len && out[len - 1] == ' ')
        out[--len] = 0;
}

/* replaces the file only when its content changes: every close of a device
 * written to is a udev event, and mountmgr.sys acts on each new record */
static void write_record(const char *dir, const char *name, const char *text)
{
    char path[PATH_MAX], tmp[PATH_MAX], old[4096];
    ssize_t len = -1;
    int fd;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if ((fd = open(path, O_RDONLY | O_CLOEXEC)) >= 0) {
        len = read(fd, old, sizeof(old));
        close(fd);
    }
    if (len == (ssize_t)strlen(text) && !memcmp(old, text, len))
        return;
    mkdir(dir, 0755);
    snprintf(tmp, sizeof(tmp), "%s/.%s", dir, name);
    if ((fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)) < 0) {
        say("%s: %s", tmp, strerror(errno));
        return;
    }
    write(fd, text, strlen(text));
    close(fd);
    /* mountmgr sees the record whole or not at all */
    rename(tmp, path);
}

/* what Windows names a USB disk by; dir is the sysfs folder of the disk */
static void describe_disk(const char *name, const char *dir, const char *usb)
{
    char path[PATH_MAX + 32], vendor[64], model[64], rev[16], serial[128], vid[8], pid[8];
    char removable[8], sector[16], size[32], text[1024];

    snprintf(path, sizeof(path), "%s/device/vendor", dir);
    read_trimmed(path, vendor, sizeof(vendor));
    snprintf(path, sizeof(path), "%s/device/model", dir);
    read_trimmed(path, model, sizeof(model));
    snprintf(path, sizeof(path), "%s/device/rev", dir);
    read_trimmed(path, rev, sizeof(rev));
    snprintf(path, sizeof(path), "%s/serial", usb);
    read_trimmed(path, serial, sizeof(serial));
    if (!serial[0])
        snprintf(serial, sizeof(serial), "%s", env("ID_SERIAL_SHORT"));
    snprintf(path, sizeof(path), "%s/idVendor", usb);
    read_trimmed(path, vid, sizeof(vid));
    snprintf(path, sizeof(path), "%s/idProduct", usb);
    read_trimmed(path, pid, sizeof(pid));
    snprintf(path, sizeof(path), "%s/removable", dir);
    read_trimmed(path, removable, sizeof(removable));
    snprintf(path, sizeof(path), "%s/queue/logical_block_size", dir);
    read_trimmed(path, sector, sizeof(sector));
    snprintf(path, sizeof(path), "%s/size", dir);
    read_trimmed(path, size, sizeof(size));

    snprintf(text, sizeof(text),
             "vendor=%s\nmodel=%s\nrev=%s\nserial=%s\nvid=%s\npid=%s\nremovable=%s\nsector=%s\nsectors=%s\nuas=%d\n",
             vendor, model, rev, serial, vid, pid, removable, sector, size, !strcmp(env("ID_USB_DRIVER"), "uas"));
    write_record(DISKS_DIR, name, text);
}

/* where a partition lies on its disk, in bytes */
static void partition_place(const char *name, unsigned long long *offset, unsigned long long *length, int *number)
{
    char path[PATH_MAX], value[32];

    snprintf(path, sizeof(path), "/sys/class/block/%s/start", name);
    read_line(path, value, sizeof(value));
    *offset = strtoull(value, NULL, 10) * 512;
    snprintf(path, sizeof(path), "/sys/class/block/%s/size", name);
    read_line(path, value, sizeof(value));
    *length = strtoull(value, NULL, 10) * 512;
    snprintf(path, sizeof(path), "/sys/class/block/%s/partition", name);
    read_line(path, value, sizeof(value));
    *number = atoi(value);
}

/* a program has this volume or its disk locked, and is writing to it */
static int held(const char *name, const char *disk)
{
    char path[PATH_MAX];

    snprintf(path, sizeof(path), HOLD_DIR "/%s", name);
    if (!access(path, F_OK))
        return 1;
    snprintf(path, sizeof(path), HOLD_DIR "/%s", disk);
    return !access(path, F_OK);
}

/* the mount in a volume's record: 0 when there is no record */
static int recorded_mount(const char *record, char *mount, size_t size)
{
    char line[1024];
    FILE *f = fopen(record, "re");

    mount[0] = 0;
    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f))
        if (!strncmp(line, "mount=", 6)) {
            line[strcspn(line, "\n")] = 0;
            snprintf(mount, size, "%s", line + 6);
            break;
        }
    fclose(f);
    return 1;
}

static void add_volume(const char *name, const char *dev, int managed, const char *disk_name)
{
    const char *fs = env("ID_FS_TYPE"), *type;
    int has_fs = !strcmp(env("ID_FS_USAGE"), "filesystem");
    char dir[PATH_MAX], record[PATH_MAX], text[2048], label[256] = "", disk[PATH_MAX], old[PATH_MAX];
    struct stat st;
    int read_only = 0, len, hidden = hidden_partition();

    snprintf(record, sizeof(record), VOLUMES_DIR "/%s", name);
    if (recorded_mount(record, old, sizeof(old)) && (old[0] || !managed))
        return; /* a change event for a volume already there */
    if (stat(dev, &st) || !S_ISBLK(st.st_mode))
        return;
    /* A partition the host would not mount: only a USB disk programs may
     * write still gets a volume, for them */
    if ((host_device(name) || hidden || !has_fs) && !managed)
        return;

    dir[0] = 0;
    if (has_fs && !hidden && !held(name, disk_name)) {
        snprintf(dir, sizeof(dir), DRIVES_DIR "/%s", name);
        mkdir(DRIVES_DIR, 0755);
        mkdir(VOLUMES_DIR, 0755);
        mkdir(dir, 0755);
        if (mount_volume(dev, dir, fs, &read_only)) {
            rmdir(dir);
            dir[0] = 0;
            if (!managed)
                return;
        }
    }
    if (!dir[0]) {
        fs = "";
        read_only = 0;
    } else {
        decode_label(env("ID_FS_LABEL_ENC"), label, sizeof(label));
    }

    type = drive_type(name);
    len = snprintf(text, sizeof(text), "device=%s\nmount=%s\ntype=%s\nfs=%s\nlabel=%s\nuuid=%s\npartuuid=%s\nread_only=%d\n",
                   dev, dir, type, fs, label, env("ID_FS_UUID"), env("ID_PART_ENTRY_UUID"), read_only);
    /* the disk it is on, for Safely Remove Hardware */
    if (!disk_dir(name, disk)) {
        char usb[PATH_MAX], product[256] = "";
        int on_usb = usb_device(disk, usb, product, sizeof(product));

        if (!product[0])
            decode_label(env("ID_MODEL_ENC"), product, sizeof(product));
        len += snprintf(text + len, sizeof(text) - len, "disk=%s\nusb=%d\nproduct=%s\n", strrchr(disk, '/') + 1, on_usb, product);
    }
    {
        unsigned long long offset = 0, length = 0;
        int number = 0;

        /* where it lies: what Windows knows an MBR partition by */
        partition_place(name, &offset, &length, &number);
        len += snprintf(text + len, sizeof(text) - len, "offset=%llu\nlength=%llu\npartnum=%d\n", offset, length, number);
    }
    if (managed)
        len += snprintf(text + len, sizeof(text) - len, "managed=1\nhidden=%d\nrawletter=%d\n", hidden,
                        !hidden && recognized_partition());
    write_record(VOLUMES_DIR, name, text);
    say("%s: %s %s '%s'%s%s", name, type, fs, label, read_only ? " read-only" : "", dir[0] ? "" : " (not mounted)");
}

static void device_changed(const char *name, const char *dev)
{
    char disk[PATH_MAX] = "", usb[PATH_MAX], product[256];
    const char *disk_name = name;
    int managed = 0;

    if (strncmp(name, "loop", 4) && strncmp(name, "ram", 3) && strncmp(name, "dm-", 3) &&
        strncmp(name, "zram", 4) && strncmp(name, "nbd", 3) && strncmp(name, "sr", 2) && !disk_dir(name, disk)) {
        disk_name = strrchr(disk, '/') + 1;
        managed = usb_device(disk, usb, product, sizeof(product)) && !disk_in_use(disk_name, disk);
    }

    if (managed) {
        give_to_nt(dev);
        if (!strcmp(env("DEVTYPE"), "disk")) {
            char whole[PATH_MAX + 64];

            describe_disk(name, disk, usb);
            /* a disk with partitions has no volume of its own */
            snprintf(whole, sizeof(whole), "%s/%s1", disk, name);
            if (strcmp(env("ID_FS_USAGE"), "filesystem") || !access(whole, F_OK))
                return;
        }
        add_volume(name, dev, 1, disk_name);
        return;
    }
    if (!strcmp(env("ID_FS_USAGE"), "filesystem"))
        add_volume(name, dev, 0, disk_name);
    else if (!strcmp(env("ACTION"), "change"))
        remove_volume(name); /* a CD's medium went */
}

int main(int argc, char **argv)
{
    const char *dev = env("DEVNAME"), *name = strrchr(dev, '/');

    if (argc < 2 || !name || !name[1])
        return 1;
    name++;
    if (!strcmp(argv[1], "add"))
        device_changed(name, dev);
    else if (!strcmp(argv[1], "remove")) {
        char path[PATH_MAX];

        remove_volume(name);
        snprintf(path, sizeof(path), DISKS_DIR "/%s", name);
        unlink(path);
    } else
        return 1;
    return 0;
}
