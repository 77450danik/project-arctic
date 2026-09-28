/* resolvconf for Arctic: iwd (Wi-Fi) and the wired DHCP script hand over the
 * name servers of each interface; /run/arctic/resolv.conf, which
 * /etc/resolv.conf points to, holds those of every interface up.
 *
 *   resolvconf -a IFACE   the name servers on stdin ("nameserver ..." lines)
 *   resolvconf -d IFACE   the interface is gone
 * Other options are accepted and change nothing. */
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DIR_PATH "/run/arctic/resolv.d"
#define CONF "/run/arctic/resolv.conf"

static void rebuild(void)
{
    char line[512], path[512];
    FILE *out = fopen(CONF ".new", "w");
    DIR *dir = opendir(DIR_PATH);
    struct dirent *de;

    if (!out)
        return;
    while (dir && (de = readdir(dir))) {
        FILE *in;

        if (de->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), DIR_PATH "/%s", de->d_name);
        if (!(in = fopen(path, "r")))
            continue;
        while (fgets(line, sizeof(line), in))
            if (!strncmp(line, "nameserver", 10) || !strncmp(line, "search", 6) || !strncmp(line, "domain", 6))
                fputs(line, out);
        fclose(in);
    }
    if (dir)
        closedir(dir);
    fclose(out);
    chmod(CONF ".new", 0644);
    rename(CONF ".new", CONF);
}

static int safe_name(const char *name)
{
    return name && *name && !strchr(name, '/') && name[0] != '.' && strlen(name) < 64;
}

int main(int argc, char **argv)
{
    char path[512], buf[4096];
    int opt, add = 0, del = 0;
    const char *iface = NULL;

    /* iwd runs "resolvconf -a IFACE -m 0 -x" and "resolvconf -d IFACE" */
    while ((opt = getopt(argc, argv, "a:d:xulfm:p")) != -1) {
        if (opt == 'a') {
            add = 1;
            iface = optarg;
        } else if (opt == 'd') {
            del = 1;
            iface = optarg;
        }
    }
    mkdir("/run/arctic", 0755);
    mkdir(DIR_PATH, 0755);
    if (add && safe_name(iface)) {
        int fd;
        ssize_t n;

        snprintf(path, sizeof(path), DIR_PATH "/%s", iface);
        if ((fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)) >= 0) {
            while ((n = read(0, buf, sizeof(buf))) > 0)
                if (write(fd, buf, (size_t)n) != n)
                    break;
            close(fd);
        }
    } else if (del && safe_name(iface)) {
        snprintf(path, sizeof(path), DIR_PATH "/%s", iface);
        unlink(path);
    }
    rebuild();
    return 0;
}
