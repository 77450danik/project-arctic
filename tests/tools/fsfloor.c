/* What the NTFS driver of the kernel charges by itself, with no Wine: the
 * same file operations ntbench times, done with Linux calls on a directory of
 * C: (run in a WSL container, whose /mnt/c is the same mount).
 *
 * gcc -O2 -o fsfloor tests/tools/fsfloor.c && ./fsfloor /mnt/c/Users/Public/fsfloor.tmp */
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

static double now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

#define TIME(name, n, body)                                                  \
    do {                                                                     \
        double t0_ = now_us();                                               \
        for (int i = 0; i < (n); i++) { body; }                             \
        printf("%-48s %9.2f us\n", name, (now_us() - t0_) / (n));            \
    } while (0)

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "/mnt/c/Users/Public/fsfloor.tmp";
    char path[4096], buf[4096];
    struct stat st;

    mkdir(dir, 0755);
    for (int i = 0; i < 1000; i++) {
        snprintf(path, sizeof(path), "%s/File%04d.txt", dir, i);
        int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        write(fd, path, strlen(path));
        close(fd);
    }
    snprintf(path, sizeof(path), "%s/File0500.txt", dir);
    TIME("stat existing", 100000, stat(path, &st));
    snprintf(path, sizeof(path), "%s/FILE0500.TXT", dir);
    TIME("stat other case (nocase mount)", 100000, stat(path, &st));
    snprintf(path, sizeof(path), "%s/NoSuchFile.txt", dir);
    TIME("stat missing (one kernel lookup)", 100000, stat(path, &st));
    TIME("readdir whole dir of 1000 (Wine's scan on a miss)", 2000, {
        DIR *d = opendir(dir);
        struct dirent *e;
        while ((e = readdir(d)) && strcasecmp(e->d_name, "NoSuchFile.txt"));
        closedir(d);
    });
    TIME("readdir + stat + getxattr of 1000 (FindNext)", 200, {
        DIR *d = opendir(dir);
        struct dirent *e;
        char p2[4096];
        while ((e = readdir(d))) {
            snprintf(p2, sizeof(p2), "%s/%s", dir, e->d_name);
            stat(p2, &st);
            getxattr(p2, "user.DOSATTRIB", buf, sizeof(buf));
        }
        closedir(d);
    });
    snprintf(path, sizeof(path), "%s/File0500.txt", dir);
    TIME("open + close", 100000, close(open(path, O_RDONLY)));
    memset(buf, 'x', sizeof(buf));
    TIME("create + write 4 KB + close + unlink", 3000, {
        snprintf(path, sizeof(path), "%s/new%d.tmp", dir, i);
        int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        write(fd, buf, sizeof(buf));
        close(fd);
        unlink(path);
    });
    for (int i = 0; i < 1000; i++) {
        snprintf(path, sizeof(path), "%s/File%04d.txt", dir, i);
        unlink(path);
    }
    rmdir(dir);
    return 0;
}
