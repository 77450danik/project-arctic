/* arctic-volume: udev runs it for every block device that comes, changes or
 * goes. A volume Windows would give a drive letter is mounted under
 * /run/arctic/media/<name> and described in /run/arctic/volumes/<name>;
 * mountmgr.sys watches that folder and gives the volume its letter.
 *
 *   arctic-volume add     the device in DEVNAME has a file system (udev's
 *                         ID_FS_* properties say which)
 *   arctic-volume remove  the device or its medium is gone
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

#define MEDIA_DIR "/run/arctic/media"
#define VOLUMES_DIR "/run/arctic/volumes"
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

/* the host's own devices: loop, device-mapper and zram, and every volume of a
 * disk the host uses: a device-mapper table holds it (C:'s origin) or it is
 * mounted outside MEDIA_DIR (the boot medium, which on a stick written from
 * the ISO is the whole disk and two partitions at once) */
static int host_device(const char *name)
{
    char disk[PATH_MAX], part[PATH_MAX + 512], line[1024];
    dev_t devs[64];
    int n = 0, used = 0;
    struct dirent *de;
    DIR *d;
    FILE *f;

    if (!strncmp(name, "loop", 4) || !strncmp(name, "dm-", 3) || !strncmp(name, "zram", 4) ||
        !strncmp(name, "ram", 3))
        return 1;
    if (disk_dir(name, disk))
        return 0;

    if (read_dev(disk, &devs[n]))
        n++;
    used = has_holders(disk);
    if ((d = opendir(disk))) {
        while (!used && n < 64 && (de = readdir(d))) {
            snprintf(part, sizeof(part), "%s/%s/partition", disk, de->d_name);
            if (de->d_name[0] == '.' || access(part, F_OK))
                continue;
            *strrchr(part, '/') = 0;
            used = has_holders(part);
            if (read_dev(part, &devs[n]))
                n++;
        }
        closedir(d);
    }

    if ((f = fopen("/proc/self/mountinfo", "re"))) {
        unsigned int major, minor;
        while (!used && fgets(line, sizeof(line), f)) {
            if (sscanf(line, "%*u %*u %u:%u", &major, &minor) != 2 || strstr(line, " " MEDIA_DIR "/"))
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
    snprintf(path, sizeof(path), MEDIA_DIR "/%s", name);
    /* the medium may be gone already: detach, nothing can be flushed to it */
    umount2(path, MNT_DETACH);
    rmdir(path);
}

static void add_volume(const char *name, const char *dev)
{
    const char *fs = env("ID_FS_TYPE"), *type;
    char dir[PATH_MAX], record[PATH_MAX], tmp[PATH_MAX], label[256];
    struct stat st;
    int read_only;
    FILE *f;

    snprintf(record, sizeof(record), VOLUMES_DIR "/%s", name);
    if (!access(record, F_OK))
        return; /* a change event for a volume already there */
    if (stat(dev, &st) || !S_ISBLK(st.st_mode) || host_device(name) || hidden_partition())
        return;

    snprintf(dir, sizeof(dir), MEDIA_DIR "/%s", name);
    mkdir(MEDIA_DIR, 0755);
    mkdir(VOLUMES_DIR, 0755);
    mkdir(dir, 0755);
    if (mount_volume(dev, dir, fs, &read_only)) {
        rmdir(dir);
        return;
    }

    type = drive_type(name);
    decode_label(env("ID_FS_LABEL_ENC"), label, sizeof(label));
    snprintf(tmp, sizeof(tmp), VOLUMES_DIR "/.%s", name);
    if (!(f = fopen(tmp, "we"))) {
        say("%s: %s", tmp, strerror(errno));
        return;
    }
    fprintf(f, "device=%s\nmount=%s\ntype=%s\nfs=%s\nlabel=%s\nuuid=%s\npartuuid=%s\nread_only=%d\n",
            dev, dir, type, fs, label, env("ID_FS_UUID"), env("ID_PART_ENTRY_UUID"), read_only);
    fclose(f);
    /* mountmgr sees the record whole or not at all */
    rename(tmp, record);
    say("%s: %s %s '%s'%s", name, type, fs, label, read_only ? " read-only" : "");
}

int main(int argc, char **argv)
{
    const char *dev = env("DEVNAME"), *name = strrchr(dev, '/');

    if (argc < 2 || !name || !name[1])
        return 1;
    name++;
    if (!strcmp(argv[1], "add"))
        add_volume(name, dev);
    else if (!strcmp(argv[1], "remove"))
        remove_volume(name);
    else
        return 1;
    return 0;
}
