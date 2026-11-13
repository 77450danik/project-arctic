/* arctic-disk: what mountmgr.sys, which runs as the unprivileged user nt, needs
 * root for when a program of Windows works on a USB disk (docs/disks.md).
 * One request per connection on /run/arctic/disk.sock, one word in answer:
 *
 *   rescan sda     the disk's partition table changed: its volumes are
 *                  unmounted and the kernel reads the table again
 *   lock sda1      the file system comes off if nothing has a file open on it
 *   dismount sda1  the file system comes off, open files or not; the volume stays
 *   mount sda1     udev looks at the volume again, and arctic-volume mounts
 *                  what it finds there
 *
 * Answers: ok, busy, none (no file system to mount), error. Only the disks
 * arctic-volume described in /run/arctic/disks can be named: never the disk
 * the system is on, never an internal one. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "lxss-binds.h"

#define SOCKET_PATH "/run/arctic/disk.sock"
#define DISKS_DIR "/run/arctic/disks"
#define VOLUMES_DIR "/run/arctic/volumes"
#define NT_USER "nt"

static void say(const char *fmt, ...)
{
    char buf[512];
    int len = snprintf(buf, sizeof(buf), "arctic-disk: ");
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

static void wait_ms(int ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};

    nanosleep(&ts, NULL);
}

/* a device's name, as the kernel gives them: nothing that leaves the folder */
static int valid_name(const char *name)
{
    return name[0] && strlen(name) < 32 &&
           strspn(name, "abcdefghijklmnopqrstuvwxyz0123456789") == strlen(name);
}

static int managed_disk(const char *disk)
{
    char path[PATH_MAX];

    snprintf(path, sizeof(path), DISKS_DIR "/%s", disk);
    return !access(path, F_OK);
}

/* one key of a volume's record; empty when there is none */
static void record_value(const char *name, const char *key, char *value, size_t size)
{
    char path[PATH_MAX], line[1024];
    size_t len = strlen(key);
    FILE *f;

    value[0] = 0;
    snprintf(path, sizeof(path), VOLUMES_DIR "/%s", name);
    if (!(f = fopen(path, "re")))
        return;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, len) || line[len] != '=')
            continue;
        line[strcspn(line, "\n")] = 0;
        snprintf(value, size, "%s", line + len + 1);
        break;
    }
    fclose(f);
}

/* the volume stays and its file system goes: the same record with no mount */
static void clear_mount_in_record(const char *name)
{
    char path[PATH_MAX], tmp[PATH_MAX], line[1024];
    FILE *in, *out;

    snprintf(path, sizeof(path), VOLUMES_DIR "/%s", name);
    snprintf(tmp, sizeof(tmp), VOLUMES_DIR "/.%s", name);
    if (!(in = fopen(path, "re")))
        return;
    if (!(out = fopen(tmp, "we"))) {
        fclose(in);
        return;
    }
    while (fgets(line, sizeof(line), in)) {
        if (!strncmp(line, "mount=", 6))
            fputs("mount=\n", out);
        else if (strncmp(line, "fs=", 3) && strncmp(line, "label=", 6))
            fputs(line, out);
    }
    fputs("fs=\nlabel=\n", out);
    fclose(in);
    fclose(out);
    rename(tmp, path);
}

/* FSCTL_DISMOUNT_VOLUME (force): a program that formats or wipes a volume
 * wants its file system gone at once, open files or not; what is still open
 * keeps a detached mount. FSCTL_LOCK_VOLUME (no force): refused while
 * anything has a file open there, as Windows refuses it. */
static const char *dismount(const char *name, int force)
{
    char mount[PATH_MAX], disk[64];

    record_value(name, "disk", disk, sizeof(disk));
    if (!disk[0] || !managed_disk(disk))
        return "error";
    /* the distributions' binds first: a volume they hold is not off */
    if (release_lxss_binds(name, force)) {
        say("%s: in use in a Linux distribution: %s, not locked", name, strerror(errno));
        return errno == EBUSY ? "busy" : "error";
    }
    record_value(name, "mount", mount, sizeof(mount));
    if (!mount[0])
        return "ok";
    sync();
    if (umount2(mount, 0) && errno != EINVAL && errno != ENOENT) {
        if (!force) {
            say("%s: %s: %s, not locked", name, mount, strerror(errno));
            return errno == EBUSY ? "busy" : "error";
        }
        say("%s: %s: %s, detaching", name, mount, strerror(errno));
        umount2(mount, MNT_DETACH);
    }
    rmdir(mount);
    clear_mount_in_record(name);
    say("%s dismounted", name);
    return "ok";
}

/* the volumes of a disk come off before the kernel can read its table again */
static void dismount_disk(const char *disk)
{
    struct dirent *de;
    DIR *dir;

    if (!(dir = opendir(VOLUMES_DIR)))
        return;
    while ((de = readdir(dir))) {
        char on[64];

        if (de->d_name[0] == '.')
            continue;
        record_value(de->d_name, "disk", on, sizeof(on));
        if (!strcmp(on, disk))
            dismount(de->d_name, 1);
    }
    closedir(dir);
}

/* A partition still open (a program's handle to the volume) keeps the kernel
 * from reading the table: mountmgr.sys asks again when that handle closes. */
static const char *rescan(const char *disk)
{
    char path[PATH_MAX];
    int fd, i;

    if (!managed_disk(disk))
        return "error";
    dismount_disk(disk);
    snprintf(path, sizeof(path), "/dev/%s", disk);
    if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
        return "error";
    fsync(fd);
    /* short: mountmgr.sys waits for this answer, and asks again if it is "busy" */
    for (i = 0; i < 5; i++) {
        if (!ioctl(fd, BLKRRPART)) {
            close(fd);
            say("%s: partition table read again", disk);
            return "ok";
        }
        if (errno != EBUSY)
            break;
        wait_ms(100);
    }
    say("%s: partition table: %s", disk, strerror(errno));
    i = errno;
    close(fd);
    return i == EBUSY ? "busy" : "error";
}

/* udev finds what the volume holds now, and arctic-volume mounts it */
static const char *mount_volume(const char *name)
{
    char path[PATH_MAX], disk[64], mount[PATH_MAX];
    int fd, i;

    record_value(name, "disk", disk, sizeof(disk));
    if (!disk[0] || !managed_disk(disk))
        return "error";
    record_value(name, "mount", mount, sizeof(mount));
    if (mount[0])
        return "ok";
    snprintf(path, sizeof(path), "/sys/class/block/%s/uevent", name);
    if ((fd = open(path, O_WRONLY | O_CLOEXEC)) < 0)
        return "error";
    write(fd, "change", 6);
    close(fd);
    for (i = 0; i < 40; i++) {
        record_value(name, "mount", mount, sizeof(mount));
        if (mount[0])
            return "ok";
        wait_ms(100);
    }
    return "none";
}

static void serve(int client)
{
    char request[128], *space;
    const char *answer = "error", *name;
    ssize_t len = read(client, request, sizeof(request) - 1);

    if (len > 0) {
        request[len] = 0;
        request[strcspn(request, "\r\n")] = 0;
        if ((space = strchr(request, ' '))) {
            *space = 0;
            name = space + 1;
            if (!valid_name(name))
                answer = "error";
            else if (!strcmp(request, "rescan"))
                answer = rescan(name);
            else if (!strcmp(request, "dismount"))
                answer = dismount(name, 1);
            else if (!strcmp(request, "lock"))
                answer = dismount(name, 0);
            else if (!strcmp(request, "mount"))
                answer = mount_volume(name);
        }
    }
    write(client, answer, strlen(answer));
}

int main(void)
{
    struct sockaddr_un addr = {.sun_family = AF_UNIX, .sun_path = SOCKET_PATH};
    struct passwd *pw = getpwnam(NT_USER);
    int fd;

    /* children are not waited for */
    signal(SIGCHLD, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    if ((fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0)
        return 1;
    unlink(SOCKET_PATH);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) || listen(fd, 16)) {
        say("%s: %s", SOCKET_PATH, strerror(errno));
        return 1;
    }
    chmod(SOCKET_PATH, 0600);
    if (pw)
        chown(SOCKET_PATH, pw->pw_uid, pw->pw_gid);

    for (;;) {
        int client = accept4(fd, NULL, NULL, SOCK_CLOEXEC);

        if (client < 0) {
            if (errno == EINTR)
                continue;
            return 1;
        }
        /* flushing a slow stick can take a while: the others do not wait */
        if (!fork()) {
            close(fd);
            serve(client);
            _exit(0);
        }
        close(client);
    }
}
