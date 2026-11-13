/* The WSL distributions have every drive letter bound under /run/lxss
 * (arctic-lxss.c, bind_drives). A bind of a volume holds its file system, and
 * with it the disk: the kernel will not read the partition table again, and a
 * stick pulled out keeps its name (sdd) taken. On Windows DrvFs holds nothing
 * of a volume, so a volume that leaves here leaves those binds too. */
#ifndef LXSS_BINDS_H
#define LXSS_BINDS_H

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>

#define LXSS_ROOTS "/run/lxss/"
#define LXSS_MAX_BINDS 64

/* the binds of /dev/<name>; force detaches what is in use, without it a bind
 * in use is left and the answer is -1 with errno set, as a locked volume's */
static int release_lxss_binds(const char *name, int force)
{
    char line[3 * PATH_MAX], source[NAME_MAX + 8], *binds[LXSS_MAX_BINDS];
    FILE *f = fopen("/proc/self/mountinfo", "re");
    int count = 0, result = 0, error = 0;

    if (!f)
        return 0;
    snprintf(source, sizeof(source), "/dev/%s", name);
    while (count < LXSS_MAX_BINDS && fgets(line, sizeof(line), f)) {
        /* "id parent dev root MOUNTPOINT options ... - type SOURCE options" */
        char *point = line, *end, *dash = strstr(line, " - "), *src;

        for (int i = 0; i < 4 && point; i++)
            point = strchr(point, ' ') ? strchr(point, ' ') + 1 : NULL;
        if (!point || !dash || !(end = strchr(point, ' ')) || strncmp(point, LXSS_ROOTS, strlen(LXSS_ROOTS)))
            continue;
        src = strchr(dash + 3, ' ');
        if (!src)
            continue;
        src++;
        if (strncmp(src, source, strlen(source)) || src[strlen(source)] != ' ')
            continue;
        *end = 0;
        binds[count] = strdup(point);
        if (binds[count])
            count++;
    }
    fclose(f);

    /* deepest first, as they were made */
    for (int i = count - 1; i >= 0; i--) {
        if (force ? umount2(binds[i], MNT_DETACH) : umount2(binds[i], 0)) {
            if (errno != EINVAL && errno != ENOENT) {
                error = errno;
                result = -1;
            }
        }
        free(binds[i]);
    }
    if (result)
        errno = error;
    return result;
}

#endif
