/* arctic-smb: the network folders of Windows, \\server\share (docs/M5-network.md).
 *
 * Wine finds a UNC path under dosdevices/unc/<server>/<share>. That folder is
 * an autofs here, as the redirector is in Windows: the first time any program
 * names \\server, a folder for the server appears (an autofs of its own), and
 * the first time it names \\server\share, the share is mounted there with the
 * kernel's SMB client (cifs), with the name and password the Windows side
 * gave for the server, or as a guest. Nothing is asked of the user here: a
 * mount that the server refuses fails, and the Windows side (ntlanman.dll,
 * the network provider) asks for the password and tells it to this service.
 *
 * The Windows side talks to it over TCP on 127.0.0.1, the port and a token
 * in C:\ProgramData\Arctic\Smb\session (as arctic-lxss has them): the token,
 * then one command a line, its fields in hexadecimal:
 *
 *   CRED server user domain password    the logon for a server, kept in memory
 *   GETCRED server                      "OK user domain password", for the list of its shares
 *   HOST name address                   an address the provider found for a name
 *   RESOLVE name                        "OK address", found as below
 *   MOUNT server share                  mount now; "OK" or "ERR errno"
 *   UMOUNT server share                 "" for every share of the server
 *   LIST                                "server share" a line for each mount
 *
 * Names are kept in lower case: Wine looks a name up as it was typed and then
 * by any case in the folder. A server is found by DNS, then LLMNR, NetBIOS
 * (broadcast) and mDNS, as Windows finds the computers of a home network. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/auto_fs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define C_DRIVE "/mnt/c"
#define DOSDEVICES "/run/nt/dosdevices"
#define UNC_ROOT "/run/arctic/unc"
#define SMB_DIR C_DRIVE "/ProgramData/Arctic/Smb"
#define SESSION SMB_DIR "/session"
#define NT_USER "nt"
#define TOKEN_LEN 32
#define MAX_SERVERS 64
#define MAX_NAME 256

static uid_t nt_uid = 1000;
static gid_t nt_gid = 1000;
static char token[TOKEN_LEN + 1];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

struct server {
    char name[MAX_NAME];        /* lower case */
    char address[64];           /* what it resolved to, or the provider told */
    time_t address_at, failed_at;
    char user[256], domain[256], password[256];
    int has_cred;
    int autofs;                 /* its folder is there, with its own autofs */
    int ioctl_fd;
};
static struct server servers[MAX_SERVERS];
static int root_ioctl = -1;

static void say(const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    int fd, n;

    va_start(ap, fmt);
    n = vsnprintf(msg, sizeof(msg) - 1, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(msg) - 2)
        n = sizeof(msg) - 2;
    fprintf(stderr, "arctic-smb: %s\n", msg);
    if ((fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC)) >= 0) {
        char line[1100];
        n = snprintf(line, sizeof(line), "<6>arctic-smb: %s\n", msg);
        if (write(fd, line, n) < 0) {
        }
        close(fd);
    }
}

static void lower(char *dst, const char *src, size_t size)
{
    size_t i;

    for (i = 0; src[i] && i + 1 < size; i++)
        dst[i] = tolower((unsigned char)src[i]);
    dst[i] = 0;
}

/* a computer's or a share's name: nothing that leaves its folder, and none of
 * the files Windows looks for in every folder it opens */
static int good_name(const char *name)
{
    static const char *const probes[] = {".ini", ".inf", ".exe", ".dll", ".lnk", ".ico", ".db", ".url", ".bat", ".cmd", NULL};
    size_t len = strlen(name);

    if (!len || len >= MAX_NAME || name[0] == '.' || strpbrk(name, "/\\*?<>|\":"))
        return 0;
    for (int i = 0; probes[i]; i++)
        if (len > strlen(probes[i]) && !strcasecmp(name + len - strlen(probes[i]), probes[i]))
            return 0;
    return 1;
}

static struct server *find_server(const char *name, int create)
{
    struct server *free_one = NULL;

    for (int i = 0; i < MAX_SERVERS; i++) {
        if (!strcmp(servers[i].name, name))
            return &servers[i];
        if (!free_one && !servers[i].name[0])
            free_one = &servers[i];
    }
    if (!create || !free_one)
        return NULL;
    memset(free_one, 0, sizeof(*free_one));
    free_one->ioctl_fd = -1;
    snprintf(free_one->name, sizeof(free_one->name), "%s", name);
    return free_one;
}

/**********************************************************************
 *          Finding a computer by its name
 */

/* a DNS name as the queries carry it: labels with their lengths */
static int dns_name(unsigned char *out, const char *name)
{
    int pos = 0;

    while (*name) {
        const char *dot = strchrnul(name, '.');
        int len = dot - name;

        if (len < 1 || len > 63)
            return -1;
        out[pos++] = len;
        memcpy(out + pos, name, len);
        pos += len;
        name = *dot ? dot + 1 : dot;
    }
    out[pos++] = 0;
    return pos;
}

/* the first A record of an answer */
static int dns_answer(const unsigned char *buf, int len, char *address, size_t size)
{
    int questions, answers, pos = 12;

    if (len < 12 || !(buf[2] & 0x80))
        return 0;
    questions = buf[4] << 8 | buf[5];
    answers = buf[6] << 8 | buf[7];
    for (int q = 0; q < questions && pos < len; q++) {
        while (pos < len && buf[pos] && !(buf[pos] & 0xc0))
            pos += buf[pos] + 1;
        pos += (pos < len && (buf[pos] & 0xc0)) ? 2 : 1;
        pos += 4;
    }
    for (int a = 0; a < answers && pos + 10 < len; a++) {
        int type, rdlen;

        while (pos < len && buf[pos] && !(buf[pos] & 0xc0))
            pos += buf[pos] + 1;
        pos += (pos < len && (buf[pos] & 0xc0)) ? 2 : 1;
        if (pos + 10 > len)
            break;
        type = buf[pos] << 8 | buf[pos + 1];
        rdlen = buf[pos + 8] << 8 | buf[pos + 9];
        pos += 10;
        if (type == 1 && rdlen == 4 && pos + 4 <= len) {
            snprintf(address, size, "%u.%u.%u.%u", buf[pos], buf[pos + 1], buf[pos + 2], buf[pos + 3]);
            return 1;
        }
        pos += rdlen;
    }
    return 0;
}

/* LLMNR (224.0.0.252:5355) and mDNS (224.0.0.251:5353): a DNS question sent to
 * the neighbours, out of every interface */
static int multicast_query(const char *name, const char *group, int port, char *address, size_t size)
{
    unsigned char packet[512] = {0}, reply[1500];
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(port)};
    struct ifaddrs *list = NULL, *ifa;
    int fd, len, found = 0, n;

    if ((n = dns_name(packet + 12, name)) < 0)
        return 0;
    getrandom(packet, 2, 0);
    if (port == 5353)
        packet[0] = packet[1] = 0;
    packet[5] = 1;
    len = 12 + n;
    packet[len + 1] = 1;                          /* A */
    packet[len + 2] = port == 5353 ? 0x80 : 0;    /* a unicast answer, please */
    packet[len + 3] = 1;                          /* IN */
    len += 4;
    inet_pton(AF_INET, group, &to.sin_addr);

    if ((fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)) < 0)
        return 0;
    getifaddrs(&list);
    for (ifa = list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET || (ifa->ifa_flags & IFF_LOOPBACK) ||
            !(ifa->ifa_flags & IFF_UP))
            continue;
        setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr,
                   sizeof(struct in_addr));
        sendto(fd, packet, len, 0, (struct sockaddr *)&to, sizeof(to));
    }
    freeifaddrs(list);
    for (int tries = 0; tries < 4 && !found; tries++) {
        struct pollfd pfd = {fd, POLLIN, 0};

        if (poll(&pfd, 1, 400) <= 0)
            break;
        if ((n = recv(fd, reply, sizeof(reply), 0)) > 0)
            found = dns_answer(reply, n, address, size);
    }
    close(fd);
    return found;
}

/* NetBIOS: the name asked of everybody on the wire (UDP 137) */
static int netbios_query(const char *name, char *address, size_t size)
{
    unsigned char packet[50] = {0}, reply[512];
    char padded[17];
    struct ifaddrs *list = NULL, *ifa;
    int fd, on = 1, found = 0, n;

    if (strlen(name) > 15 || strchr(name, '.'))
        return 0;
    snprintf(padded, sizeof(padded), "%-15s", name);
    for (int i = 0; i < 15; i++)
        padded[i] = toupper((unsigned char)padded[i]);
    padded[15] = 0x20;      /* the file server's name */
    getrandom(packet, 2, 0);
    packet[2] = 0x01;       /* recursion desired */
    packet[3] = 0x10;       /* broadcast */
    packet[5] = 1;
    packet[12] = 32;
    for (int i = 0; i < 16; i++) {
        packet[13 + i * 2] = 'A' + ((unsigned char)padded[i] >> 4);
        packet[14 + i * 2] = 'A' + (padded[i] & 15);
    }
    /* 12 of the header, 34 of the name, then the type (NB) and the class (IN) */
    packet[47] = 0x20;
    packet[49] = 0x01;

    if ((fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)) < 0)
        return 0;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    getifaddrs(&list);
    for (ifa = list; ifa; ifa = ifa->ifa_next) {
        struct sockaddr_in to;

        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET || !(ifa->ifa_flags & IFF_BROADCAST) ||
            !(ifa->ifa_flags & IFF_UP) || !ifa->ifa_broadaddr)
            continue;
        to = *(struct sockaddr_in *)ifa->ifa_broadaddr;
        to.sin_port = htons(137);
        sendto(fd, packet, sizeof(packet), 0, (struct sockaddr *)&to, sizeof(to));
    }
    freeifaddrs(list);
    for (int tries = 0; tries < 3 && !found; tries++) {
        struct pollfd pfd = {fd, POLLIN, 0};

        if (poll(&pfd, 1, 500) <= 0)
            break;
        /* header, the name again, NB, IN, ttl, length, flags, the address */
        if ((n = recv(fd, reply, sizeof(reply), 0)) >= 62 && (reply[2] & 0x80) && !(reply[3] & 0x0f) &&
            reply[47] == 0x20) {
            snprintf(address, size, "%u.%u.%u.%u", reply[58], reply[59], reply[60], reply[61]);
            found = 1;
        }
    }
    close(fd);
    return found;
}

/* 0 when nobody is called so; the address otherwise */
static int resolve(const char *name, char *address, size_t size)
{
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *info = NULL;
    struct in_addr numeric;
    struct server *server;
    char local[MAX_NAME + 8];
    time_t now = time(NULL);
    int found = 0;

    if (inet_pton(AF_INET, name, &numeric) == 1) {
        snprintf(address, size, "%s", name);
        return 1;
    }
    pthread_mutex_lock(&lock);
    if ((server = find_server(name, 0))) {
        if (server->address[0] && now - server->address_at < 300) {
            snprintf(address, size, "%s", server->address);
            found = 1;
        } else if (server->failed_at && now - server->failed_at < 20) {
            found = -1;
        }
    }
    pthread_mutex_unlock(&lock);
    if (found)
        return found > 0;

    if (strchr(name, '.') && strcasecmp(name + strlen(name) - (strlen(name) > 6 ? 6 : 0), ".local")) {
        /* a name of the Internet's: DNS */
        if (!getaddrinfo(name, NULL, &hints, &info) && info) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)info->ai_addr)->sin_addr, address, size);
            found = 1;
        }
        if (info)
            freeaddrinfo(info);
    } else if (strchr(name, '.')) {
        found = multicast_query(name, "224.0.0.251", 5353, address, size);
    } else {
        snprintf(local, sizeof(local), "%s.local", name);
        found = multicast_query(name, "224.0.0.252", 5355, address, size) || netbios_query(name, address, size) ||
                multicast_query(local, "224.0.0.251", 5353, address, size);
        if (!found && !getaddrinfo(name, NULL, &hints, &info) && info) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)info->ai_addr)->sin_addr, address, size);
            found = 1;
        }
        if (info)
            freeaddrinfo(info);
    }

    pthread_mutex_lock(&lock);
    if ((server = find_server(name, 1))) {
        if (found) {
            snprintf(server->address, sizeof(server->address), "%s", address);
            server->address_at = now;
            server->failed_at = 0;
        } else {
            server->failed_at = now;
        }
    }
    pthread_mutex_unlock(&lock);
    return found;
}

/* does anything answer on the SMB port there? (a second at most) */
static int smb_listens(const char *address)
{
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(445)};
    struct pollfd pfd;
    int fd, ok = 0, err = 0;
    socklen_t len = sizeof(err);

    inet_pton(AF_INET, address, &to.sin_addr);
    if ((fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)) < 0)
        return 0;
    if (!connect(fd, (struct sockaddr *)&to, sizeof(to))) {
        ok = 1;
    } else if (errno == EINPROGRESS) {
        pfd = (struct pollfd){fd, POLLOUT, 0};
        if (poll(&pfd, 1, 1500) > 0 && !getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) && !err)
            ok = 1;
    }
    close(fd);
    return ok;
}

/**********************************************************************
 *          Mounting
 */

/* an option's value: cifs takes a comma in it doubled */
static void append_value(char *options, size_t size, const char *value)
{
    size_t len = strlen(options);

    for (; *value && len + 3 < size; value++) {
        if (*value == ',')
            options[len++] = ',';
        options[len++] = *value;
    }
    options[len] = 0;
}

/* 0, or the errno the kernel's client gave */
static int mount_share(const char *server_name, const char *share)
{
    char path[PATH_MAX], source[2 * MAX_NAME + 8], options[2048], address[64];
    char user[256] = "", domain[256] = "", password[256] = "";
    struct server *server;
    int has_cred = 0, err;

    if (!resolve(server_name, address, sizeof(address)))
        return EHOSTUNREACH;
    pthread_mutex_lock(&lock);
    if ((server = find_server(server_name, 0)) && server->has_cred) {
        has_cred = 1;
        snprintf(user, sizeof(user), "%s", server->user);
        snprintf(domain, sizeof(domain), "%s", server->domain);
        snprintf(password, sizeof(password), "%s", server->password);
    }
    pthread_mutex_unlock(&lock);

    snprintf(path, sizeof(path), UNC_ROOT "/%s/%s", server_name, share);
    snprintf(source, sizeof(source), "//%s/%s", server_name, share);
    for (int attempt = 0; attempt < 2; attempt++) {
        snprintf(options, sizeof(options), "ip=%s,unc=\\\\%s\\%s,uid=%u,gid=%u,file_mode=0644,dir_mode=0755,noperm,"
                 "iocharset=utf8,nounix,serverino,soft,echo_interval=30,actimeo=2",
                 address, server_name, share, nt_uid, nt_gid);
        if (has_cred) {
            strncat(options, ",username=", sizeof(options) - strlen(options) - 1);
            append_value(options, sizeof(options), user);
            if (domain[0]) {
                strncat(options, ",domain=", sizeof(options) - strlen(options) - 1);
                append_value(options, sizeof(options), domain);
            }
            strncat(options, ",password=", sizeof(options) - strlen(options) - 1);
            append_value(options, sizeof(options), password);
        } else {
            /* as a guest, then as nobody: what a home NAS lets in */
            strncat(options, attempt ? ",sec=none" : ",guest,username=guest", sizeof(options) - strlen(options) - 1);
        }
        if (!mount(source, path, "cifs", MS_NOSUID | MS_NODEV, options))
            return 0;
        err = errno;
        if (has_cred || (err != EACCES && err != EPERM && err != EKEYREJECTED))
            break;
    }
    return err ? err : EIO;
}

/* A share the server refused is not asked for again at every look of
 * Explorer's: for a few seconds, or until the Windows side gives a logon */
static struct { char server[MAX_NAME], share[MAX_NAME]; time_t at; } refused[32];

static int was_refused(const char *server, const char *share, int forget)
{
    time_t now = time(NULL);
    int found = 0;

    pthread_mutex_lock(&lock);
    for (int i = 0; i < 32; i++) {
        if (strcmp(refused[i].server, server) || (share && strcmp(refused[i].share, share)))
            continue;
        if (forget)
            refused[i].server[0] = 0;
        else if (now - refused[i].at < 5)
            found = 1;
    }
    pthread_mutex_unlock(&lock);
    return found;
}

static void note_refused(const char *server, const char *share)
{
    int slot = 0;

    pthread_mutex_lock(&lock);
    for (int i = 0; i < 32; i++)
        if (!refused[i].server[0] || refused[i].at < refused[slot].at)
            slot = i;
    snprintf(refused[slot].server, MAX_NAME, "%s", server);
    snprintf(refused[slot].share, MAX_NAME, "%s", share);
    refused[slot].at = time(NULL);
    pthread_mutex_unlock(&lock);
}

/**********************************************************************
 *          The autofs of the servers and of a server's shares
 */

struct autofs {
    int pipe;
    char server[MAX_NAME];      /* "" for the root */
};

struct miss {
    struct autofs *fs;
    autofs_wqt_t wait_token;
    char name[NAME_MAX + 1];
};

static int mount_autofs(const char *path, struct autofs *fs, int *ioctl_fd);

static void answer(const struct autofs *fs, autofs_wqt_t wait_token, int ok)
{
    int fd = root_ioctl;

    if (fs->server[0]) {
        struct server *server;

        pthread_mutex_lock(&lock);
        server = find_server(fs->server, 0);
        fd = server ? server->ioctl_fd : -1;
        pthread_mutex_unlock(&lock);
    }
    if (fd >= 0)
        ioctl(fd, ok ? AUTOFS_IOC_READY : AUTOFS_IOC_FAIL, wait_token);
}

static void *reader(void *arg);

/* \\name: a computer that answers gets its folder */
static int add_server(const char *name)
{
    char path[PATH_MAX], address[64];
    struct server *server;
    struct autofs *fs;
    pthread_t thread;
    int ioctl_fd = -1, known;

    pthread_mutex_lock(&lock);
    server = find_server(name, 0);
    known = server && server->autofs;
    pthread_mutex_unlock(&lock);
    if (known)
        return 1;
    if (!resolve(name, address, sizeof(address)) || !smb_listens(address))
        return 0;

    snprintf(path, sizeof(path), UNC_ROOT "/%s", name);
    if (mkdir(path, 0755) && errno != EEXIST)
        return 0;
    if (!(fs = calloc(1, sizeof(*fs))))
        return 0;
    snprintf(fs->server, sizeof(fs->server), "%s", name);
    if (mount_autofs(path, fs, &ioctl_fd)) {
        say("%s: no autofs: %s", path, strerror(errno));
        free(fs);
        rmdir(path);
        return 0;
    }
    pthread_mutex_lock(&lock);
    if ((server = find_server(name, 1))) {
        server->autofs = 1;
        server->ioctl_fd = ioctl_fd;
    }
    pthread_mutex_unlock(&lock);
    pthread_create(&thread, NULL, reader, fs);
    pthread_detach(thread);
    say("\\\\%s is %s", name, address);
    return 1;
}

static void *handle_miss(void *arg)
{
    struct miss *miss = arg;
    char name[NAME_MAX + 1], path[PATH_MAX];
    int ok = 0;

    lower(name, miss->name, sizeof(name));
    if (!good_name(miss->name)) {
        ok = 0;
    } else if (!miss->fs->server[0]) {
        /* a name typed in another case: its folder is the lower case one,
         * which Wine finds by reading the folder once this one is not there */
        ok = add_server(name);
        if (ok && strcmp(name, miss->name))
            ok = 0;
    } else {
        snprintf(path, sizeof(path), UNC_ROOT "/%s/%s", miss->fs->server, name);
        if (strcmp(name, miss->name)) {
            /* the share in lower case is the mount; mounted now if it was not */
            struct stat st, parent;
            char dir[PATH_MAX];

            snprintf(dir, sizeof(dir), UNC_ROOT "/%s", miss->fs->server);
            if (!was_refused(miss->fs->server, name, 0) &&
                (stat(path, &st) || stat(dir, &parent) || st.st_dev == parent.st_dev)) {
                mkdir(path, 0755);
                if (mount_share(miss->fs->server, name)) {
                    rmdir(path);
                    note_refused(miss->fs->server, name);
                }
            }
            ok = 0;
        } else if (was_refused(miss->fs->server, name, 0)) {
            ok = 0;
        } else if (!mkdir(path, 0755) || errno == EEXIST) {
            int err = mount_share(miss->fs->server, name);

            if (err) {
                rmdir(path);
                note_refused(miss->fs->server, name);
                say("\\\\%s\\%s: %s", miss->fs->server, name, strerror(err));
            } else {
                say("\\\\%s\\%s mounted", miss->fs->server, name);
                ok = 1;
            }
        }
    }
    answer(miss->fs, miss->wait_token, ok);
    free(miss);
    return NULL;
}

static void *reader(void *arg)
{
    struct autofs *fs = arg;
    union autofs_v5_packet_union packet;

    for (;;) {
        ssize_t n = read(fs->pipe, &packet, sizeof(packet));
        struct miss *miss;
        pthread_t thread;

        if (n < 0 && errno == EINTR)
            continue;
        if (n < (ssize_t)sizeof(packet.hdr))
            break;
        if (packet.hdr.type != autofs_ptype_missing_indirect) {
            /* nothing expires here */
            if (packet.hdr.type == autofs_ptype_expire_indirect)
                answer(fs, packet.v5_packet.wait_queue_token, 0);
            continue;
        }
        if (!(miss = calloc(1, sizeof(*miss))))
            continue;
        miss->fs = fs;
        miss->wait_token = packet.v5_packet.wait_queue_token;
        snprintf(miss->name, sizeof(miss->name), "%.*s", (int)packet.v5_packet.len, packet.v5_packet.name);
        if (pthread_create(&thread, NULL, handle_miss, miss)) {
            answer(fs, miss->wait_token, 0);
            free(miss);
        } else {
            pthread_detach(thread);
        }
    }
    return NULL;
}

static int mount_autofs(const char *path, struct autofs *fs, int *ioctl_fd)
{
    char options[128];
    int fds[2];

    if (pipe2(fds, O_CLOEXEC))
        return -1;
    snprintf(options, sizeof(options), "fd=%d,pgrp=%d,minproto=5,maxproto=5,indirect", fds[1], (int)getpgrp());
    if (mount("arctic-smb", path, "autofs", MS_NOSUID | MS_NODEV, options)) {
        int err = errno;
        close(fds[0]);
        close(fds[1]);
        errno = err;
        return -1;
    }
    close(fds[1]);
    fs->pipe = fds[0];
    *ioctl_fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    return *ioctl_fd < 0 ? -1 : 0;
}

/**********************************************************************
 *          What the Windows side asks
 */

static int unhex(const char *hex, char *out, size_t size)
{
    size_t len = strlen(hex) / 2;

    if (len >= size)
        return -1;
    for (size_t i = 0; i < len; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1)
            return -1;
        out[i] = byte;
    }
    out[len] = 0;
    return 0;
}

static void put_hex(FILE *out, const char *text)
{
    if (!*text)
        fputc('-', out);
    for (; *text; text++)
        fprintf(out, "%02x", (unsigned char)*text);
}

static void unmount_server(const char *name, const char *share)
{
    char path[PATH_MAX], sub[PATH_MAX + NAME_MAX + 2];
    struct dirent *de;
    DIR *dir;

    snprintf(path, sizeof(path), UNC_ROOT "/%s", name);
    if (share[0]) {
        snprintf(sub, sizeof(sub), "%s/%s", path, share);
        umount2(sub, MNT_DETACH);
        rmdir(sub);
        return;
    }
    if (!(dir = opendir(path)))
        return;
    while ((de = readdir(dir))) {
        if (de->d_name[0] == '.')
            continue;
        snprintf(sub, sizeof(sub), "%s/%s", path, de->d_name);
        umount2(sub, MNT_DETACH);
        rmdir(sub);
    }
    closedir(dir);
}

static void list_mounts(FILE *out)
{
    FILE *mounts = fopen("/proc/self/mounts", "re");
    char line[2048], source[512], target[512], type[64];

    while (mounts && fgets(line, sizeof(line), mounts)) {
        if (sscanf(line, "%511s %511s %63s", source, target, type) != 3 || strcmp(type, "cifs") ||
            strncmp(target, UNC_ROOT "/", strlen(UNC_ROOT) + 1))
            continue;
        char *server = target + strlen(UNC_ROOT) + 1, *share = strchr(server, '/');
        if (!share)
            continue;
        *share++ = 0;
        fprintf(out, "%s %s\n", server, share);
    }
    if (mounts)
        fclose(mounts);
}

static void *serve_client(void *arg)
{
    int fd = (intptr_t)arg;
    FILE *io = fdopen(fd, "r+");
    char line[4096], cmd[16], a[1024], b[1024], c[1024], d[1024];
    char f1[MAX_NAME], f2[MAX_NAME], f3[MAX_NAME], f4[MAX_NAME];

    if (!io) {
        close(fd);
        return NULL;
    }
    if (!fgets(line, sizeof(line), io) || strncmp(line, token, TOKEN_LEN))
        goto done;
    while (fgets(line, sizeof(line), io)) {
        int n;

        a[0] = b[0] = c[0] = d[0] = 0;
        n = sscanf(line, "%15s %1023s %1023s %1023s %1023s", cmd, a, b, c, d);
        if (n < 1)
            break;
        /* "-" stands for an empty field */
        if (!strcmp(a, "-")) a[0] = 0;
        if (!strcmp(b, "-")) b[0] = 0;
        if (!strcmp(c, "-")) c[0] = 0;
        if (!strcmp(d, "-")) d[0] = 0;
        if (unhex(a, f1, sizeof(f1)) || unhex(b, f2, sizeof(f2)) || unhex(c, f3, sizeof(f3)) ||
            unhex(d, f4, sizeof(f4))) {
            fprintf(io, "ERR %d\n", EINVAL);
        } else if (!strcmp(cmd, "CRED")) {
            struct server *server;
            char name[MAX_NAME];

            lower(name, f1, sizeof(name));
            pthread_mutex_lock(&lock);
            if ((server = find_server(name, 1))) {
                snprintf(server->user, sizeof(server->user), "%s", f2);
                snprintf(server->domain, sizeof(server->domain), "%s", f3);
                snprintf(server->password, sizeof(server->password), "%s", f4);
                server->has_cred = f2[0] != 0;
            }
            pthread_mutex_unlock(&lock);
            was_refused(name, NULL, 1);
            fprintf(io, "OK\n");
        } else if (!strcmp(cmd, "HOST")) {
            struct server *server;
            struct in_addr addr;
            char name[MAX_NAME];

            lower(name, f1, sizeof(name));
            pthread_mutex_lock(&lock);
            if (inet_pton(AF_INET, f2, &addr) == 1 && (server = find_server(name, 1))) {
                snprintf(server->address, sizeof(server->address), "%.63s", f2);
                server->address_at = time(NULL);
                server->failed_at = 0;
            }
            pthread_mutex_unlock(&lock);
            fprintf(io, "OK\n");
        } else if (!strcmp(cmd, "RESOLVE")) {
            char name[MAX_NAME], address[64];

            lower(name, f1, sizeof(name));
            if (good_name(name) && resolve(name, address, sizeof(address)))
                fprintf(io, "OK %s\n", address);
            else
                fprintf(io, "ERR %d\n", EHOSTUNREACH);
        } else if (!strcmp(cmd, "GETCRED")) {
            struct server *server;
            char name[MAX_NAME], user[256] = "", domain[256] = "", password[256] = "";
            int has = 0;

            lower(name, f1, sizeof(name));
            pthread_mutex_lock(&lock);
            if ((server = find_server(name, 0)) && server->has_cred) {
                has = 1;
                snprintf(user, sizeof(user), "%s", server->user);
                snprintf(domain, sizeof(domain), "%s", server->domain);
                snprintf(password, sizeof(password), "%s", server->password);
            }
            pthread_mutex_unlock(&lock);
            if (has) {
                fprintf(io, "OK ");
                put_hex(io, user);
                fputc(' ', io);
                put_hex(io, domain);
                fputc(' ', io);
                put_hex(io, password);
                fputc('\n', io);
            } else {
                fprintf(io, "ERR %d\n", ENOENT);
            }
        } else if (!strcmp(cmd, "MOUNT")) {
            char name[MAX_NAME], share[MAX_NAME], path[PATH_MAX + MAX_NAME + 2], dir[PATH_MAX];
            struct stat st, parent;
            int err = 0;

            lower(name, f1, sizeof(name));
            lower(share, f2, sizeof(share));
            if (!good_name(name) || !good_name(share)) {
                err = EINVAL;
            } else if (!add_server(name)) {
                err = EHOSTUNREACH;
            } else {
                snprintf(dir, sizeof(dir), UNC_ROOT "/%s", name);
                snprintf(path, sizeof(path), "%s/%s", dir, share);
                if (stat(path, &st) || stat(dir, &parent) || st.st_dev == parent.st_dev) {
                    if (mkdir(path, 0755) && errno != EEXIST)
                        err = errno;
                    else if ((err = mount_share(name, share)))
                        rmdir(path);
                }
            }
            if (err)
                fprintf(io, "ERR %d\n", err);
            else
                fprintf(io, "OK\n");
        } else if (!strcmp(cmd, "UMOUNT")) {
            char name[MAX_NAME], share[MAX_NAME];

            lower(name, f1, sizeof(name));
            lower(share, f2, sizeof(share));
            if (good_name(name) && (!share[0] || good_name(share)))
                unmount_server(name, share);
            fprintf(io, "OK\n");
        } else if (!strcmp(cmd, "LIST")) {
            list_mounts(io);
            fprintf(io, "OK\n");
        } else {
            fprintf(io, "ERR %d\n", ENOSYS);
        }
        fflush(io);
    }
done:
    fclose(io);
    return NULL;
}

static volatile sig_atomic_t stopping;

static void on_term(int sig)
{
    stopping = 1;
}

static void unmount_all(void)
{
    struct dirent *de;
    DIR *dir = opendir(UNC_ROOT);
    char path[PATH_MAX];

    while (dir && (de = readdir(dir))) {
        if (de->d_name[0] == '.')
            continue;
        unmount_server(de->d_name, "");
        snprintf(path, sizeof(path), UNC_ROOT "/%s", de->d_name);
        umount2(path, MNT_DETACH);
        rmdir(path);
    }
    if (dir)
        closedir(dir);
    if (root_ioctl >= 0)
        ioctl(root_ioctl, AUTOFS_IOC_CATATONIC, 0);
    umount2(UNC_ROOT, MNT_DETACH);
}

int main(void)
{
    static const char hexdigits[] = "0123456789abcdef";
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    struct sigaction sa = {.sa_handler = on_term};
    socklen_t addr_len = sizeof(addr);
    struct passwd *pw = getpwnam(NT_USER);
    struct autofs *root;
    unsigned char raw[TOKEN_LEN / 2];
    pthread_t thread;
    int listener, on = 1;
    FILE *f;

    if (pw) {
        nt_uid = pw->pw_uid;
        nt_gid = pw->pw_gid;
    }
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    setpgid(0, 0);

    /* the kernel's SMB client */
    if (!fork()) {
        execl("/usr/bin/modprobe", "modprobe", "-q", "cifs", (char *)NULL);
        _exit(127);
    }
    wait(NULL);

    mkdir("/run/arctic", 0755);
    mkdir(UNC_ROOT, 0755);
    /* what a run before this one left there */
    while (!umount2(UNC_ROOT, MNT_DETACH))
        ;
    if (!(root = calloc(1, sizeof(*root))) || mount_autofs(UNC_ROOT, root, &root_ioctl)) {
        say("no autofs at " UNC_ROOT ": %s", strerror(errno));
        return 1;
    }
    unlink(DOSDEVICES "/unc");
    if (symlink(UNC_ROOT, DOSDEVICES "/unc"))
        say(DOSDEVICES "/unc: %s", strerror(errno));
    else if (lchown(DOSDEVICES "/unc", nt_uid, nt_gid)) {
    }
    pthread_create(&thread, NULL, reader, root);
    pthread_detach(thread);

    if ((listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0 ||
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) ||
        bind(listener, (struct sockaddr *)&addr, sizeof(addr)) || listen(listener, 16) ||
        getsockname(listener, (struct sockaddr *)&addr, &addr_len)) {
        say("no socket: %s", strerror(errno));
        unmount_all();
        return 1;
    }
    if (getrandom(raw, sizeof(raw), 0) != sizeof(raw))
        return 1;
    for (int i = 0; i < TOKEN_LEN / 2; i++) {
        token[i * 2] = hexdigits[raw[i] >> 4];
        token[i * 2 + 1] = hexdigits[raw[i] & 15];
    }
    mkdir(C_DRIVE "/ProgramData", 0755);
    mkdir(C_DRIVE "/ProgramData/Arctic", 0755);
    mkdir(SMB_DIR, 0755);
    if ((f = fopen(SESSION ".new", "we"))) {
        fprintf(f, "%d %s\n", ntohs(addr.sin_port), token);
        fclose(f);
        if (chown(SESSION ".new", nt_uid, nt_gid)) {
        }
        rename(SESSION ".new", SESSION);
    }
    say("network folders at " UNC_ROOT ", port %d", ntohs(addr.sin_port));

    while (!stopping) {
        struct pollfd pfd = {listener, POLLIN, 0};
        int client;

        if (poll(&pfd, 1, 1000) <= 0)
            continue;
        if ((client = accept4(listener, NULL, NULL, SOCK_CLOEXEC)) < 0)
            continue;
        if (pthread_create(&thread, NULL, serve_client, (void *)(intptr_t)client))
            close(client);
        else
            pthread_detach(thread);
    }
    unlink(SESSION);
    unmount_all();
    say("stopped");
    return 0;
}
