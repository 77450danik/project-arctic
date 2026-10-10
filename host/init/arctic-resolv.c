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
#include <time.h>
#include <unistd.h>

#define DIR_PATH "/run/arctic/resolv.d"
#define CONF "/run/arctic/resolv.conf"
#define NETCFG_DIR "/run/arctic/network" /* the TCP/IP settings of each interface (arctic-init) */

/* The name servers set by hand for the interface (dns=, dns6= of its
 * settings, "Використовувати такі адреси DNS-серверів" in Windows): they take
 * the place of those DHCP or iwd found. 0 when there are none. */
static int manual_servers(const char *iface, FILE *out)
{
    char path[512], line[512];
    int found = 0;
    FILE *in;

    snprintf(path, sizeof(path), NETCFG_DIR "/%s.conf", iface);
    if (!(in = fopen(path, "r")))
        return 0;
    while (fgets(line, sizeof(line), in)) {
        char *value, *server;

        if (strncmp(line, "dns=", 4) && strncmp(line, "dns6=", 5))
            continue;
        value = strchr(line, '=') + 1;
        for (server = strtok(value, " \t\r\n"); server; server = strtok(NULL, " \t\r\n")) {
            if (strspn(server, "0123456789abcdefABCDEF.:") != strlen(server))
                continue;
            fprintf(out, "nameserver %s\n", server);
            found = 1;
        }
    }
    fclose(in);
    return found;
}

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
        if (manual_servers(de->d_name, out))
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

/* Since when the interface has had its addresses: the seconds the machine
 * had been up then (the clock on the wall is set after the start, and moves),
 * for the "Тривалість" of the connection's status in Windows. Written when
 * the name servers first come, kept over the renewals of the lease. */
static void mark_since(const char *iface)
{
    char path[512];
    struct timespec now;
    FILE *f;

    snprintf(path, sizeof(path), DIR_PATH "/.since-%s", iface);
    if (!access(path, F_OK) || clock_gettime(CLOCK_BOOTTIME, &now) || !(f = fopen(path, "w")))
        return;
    fprintf(f, "%lld\n", (long long)now.tv_sec);
    fclose(f);
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
        mark_since(iface);
    } else if (del && safe_name(iface)) {
        snprintf(path, sizeof(path), DIR_PATH "/%s", iface);
        unlink(path);
        snprintf(path, sizeof(path), DIR_PATH "/.since-%s", iface);
        unlink(path);
    }
    rebuild();
    return 0;
}
