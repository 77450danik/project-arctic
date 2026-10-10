/* arctic-smbd: Arctic's own folders, given to the other computers of the
 * network as Windows gives them (docs/M5-network.md, "Спільний доступ з
 * Arctic"), and Arctic itself seen in their Network.
 *
 * The other half of arctic-smb: that one mounts \\server\share from somewhere
 * else, this one answers \\ARCTIC\share. The Windows side does not talk to it:
 * it writes what is shared as files, and this service watches them (inotify),
 * as arctic-init watches the settings of a network interface:
 *
 *   C:\ProgramData\Arctic\Smb\shares\<name>.share   a share, written by the
 *       Sharing tab of a folder (ntshrui.dll); the file's name is the share's:
 *         path=C:\Users\Public       the folder, as Windows names it
 *         comment=                   "Comments" of Advanced Sharing
 *         allow=read|change|full|none    what Everyone may do
 *         deny=none|read|change|full     and what Everyone may not
 *         limit=0                    simultaneous users, 0 for no limit
 *         caching=manual|auto|vdo|none   Offline Files, told to the client
 *   C:\ProgramData\Arctic\Smb\config                the switches of "Change
 *       advanced sharing settings" (netcenter.dll):
 *         sharing=0|1                file and printer sharing
 *         discovery=0|1              network discovery
 *
 * The protocol is SMB 2.0.2 and 2.1, what ntlanman.dll's own client speaks
 * (runtime/wine/modules/dlls/ntlanman/smb2.c is this file's mirror: it asks,
 * this answers). Files, folders, renaming, deleting, the times and attributes
 * of NTFS, compounded requests, the pipe of the Server service (srvsvc: the
 * list of shares) are there, and a folder open in Explorer over there is told
 * when something in it changes; oplocks, leases, byte-range locks that exclude
 * (they are granted and not kept), alternate streams, DFS and printers are not.
 *
 * Nobody's password is checked: every logon is a guest, who is Everyone, and
 * what Everyone may do is the share's. Windows has no account here whose
 * password it could be (LogonUser in Wine checks nothing), so "password
 * protected sharing" is kept by the Windows side and not enforced. Windows 10
 * and 11 refuse a guest logon when their policy says so (AllowInsecureGuestAuth,
 * off in Enterprise and in Windows 11 24H2); such a computer cannot open the
 * folders of Arctic until Arctic has accounts.
 *
 * With network discovery on, Arctic answers for its name as Windows does:
 * NetBIOS (name and node status, UDP 137), LLMNR (UDP 5355), mDNS (UDP 5353,
 * the name and the _smb._tcp service) and WS-Discovery (UDP 3702 and the
 * device's description over HTTP on 5357), which is what puts a computer in
 * the Network folder of Windows 10 and 11. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
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
#include <sys/inotify.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

#define C_DRIVE "/mnt/c"
#define DOSDEVICES "/run/nt/dosdevices"
#define SMB_DIR C_DRIVE "/ProgramData/Arctic/Smb"
#define SHARES_DIR SMB_DIR "/shares"
#define CONFIG_FILE SMB_DIR "/config"
#define NT_USER "nt"
#define SMB_PORT 445
#define WORKGROUP "WORKGROUP"

#define MAX_SHARES 64
#define MAX_NAME 256
#define MAX_PATH_LEN 2048
#define MAX_TREES 32
#define MAX_HANDLES 512
#define MAX_NOTIFIES 32
#define MAX_IO 65536
#define MAX_FRAME (1 << 20)
#define BODY_SIZE (MAX_IO + 4096)

typedef unsigned char u8;

enum { RIGHT_NONE, RIGHT_READ, RIGHT_CHANGE, RIGHT_FULL };

struct share {
    char name[MAX_NAME];
    char host_path[MAX_PATH_LEN];
    char comment[256];
    int allow, deny;
    unsigned limit, caching, users;
};

static uid_t nt_uid = 1000;
static gid_t nt_gid = 1000;
static char computer[64] = "ARCTIC"; /* upper case, as NetBIOS has it */
static volatile int sharing, discovery = 1;
static volatile sig_atomic_t stopping;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct share shares[MAX_SHARES];
static int share_count;
static u8 server_guid[16];
static uint64_t started;

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
    fprintf(stderr, "arctic-smbd: %s\n", msg);
    if ((fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC)) >= 0) {
        char line[1100];
        n = snprintf(line, sizeof(line), "<6>arctic-smbd: %s\n", msg);
        if (write(fd, line, n) < 0) {
        }
        close(fd);
    }
}

static void set16(u8 *p, unsigned v) { p[0] = v; p[1] = v >> 8; }
static void set32(u8 *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void set64(u8 *p, uint64_t v) { set32(p, (uint32_t)v); set32(p + 4, (uint32_t)(v >> 32)); }
static unsigned get16(const u8 *p) { return p[0] | p[1] << 8; }
static uint32_t get32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t get64(const u8 *p) { return get32(p) | (uint64_t)get32(p + 4) << 32; }

static void random_bytes(void *out, size_t len)
{
    if (getrandom(out, len, 0) != (ssize_t)len) {
        /* never on a started system; a clock is still better than zeros */
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        for (size_t i = 0; i < len; i++)
            ((u8 *)out)[i] = (u8)(ts.tv_nsec >> (i % 4 * 8)) ^ (u8)(i * 37);
    }
}

/* 100 ns since 1601, as Windows counts time */
static uint64_t filetime(int64_t sec, uint32_t nsec)
{
    return ((uint64_t)(sec + 11644473600LL)) * 10000000ULL + nsec / 100;
}

static uint64_t filetime_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    return filetime(ts.tv_sec, ts.tv_nsec);
}

/**********************************************************************
 *          Names: UTF-16 of the wire, UTF-8 of the files
 */

static int utf16_to_utf8(const u8 *in, unsigned chars, char *out, size_t size)
{
    size_t pos = 0;

    for (unsigned i = 0; i < chars; i++) {
        uint32_t cp = get16(in + i * 2);

        if (cp >= 0xd800 && cp < 0xdc00 && i + 1 < chars) {
            uint32_t low = get16(in + (i + 1) * 2);
            if (low >= 0xdc00 && low < 0xe000) {
                cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                i++;
            }
        }
        if (pos + 5 > size)
            return 0;
        if (cp < 0x80) {
            out[pos++] = cp;
        } else if (cp < 0x800) {
            out[pos++] = 0xc0 | cp >> 6;
            out[pos++] = 0x80 | (cp & 0x3f);
        } else if (cp < 0x10000) {
            out[pos++] = 0xe0 | cp >> 12;
            out[pos++] = 0x80 | ((cp >> 6) & 0x3f);
            out[pos++] = 0x80 | (cp & 0x3f);
        } else {
            out[pos++] = 0xf0 | cp >> 18;
            out[pos++] = 0x80 | ((cp >> 12) & 0x3f);
            out[pos++] = 0x80 | ((cp >> 6) & 0x3f);
            out[pos++] = 0x80 | (cp & 0x3f);
        }
    }
    out[pos] = 0;
    return 1;
}

/* one character of UTF-8; a byte that is not one stands for itself */
static uint32_t utf8_next(const char **text)
{
    const unsigned char *p = (const unsigned char *)*text;
    uint32_t cp = *p;
    int more = cp >= 0xf0 ? 3 : cp >= 0xe0 ? 2 : cp >= 0xc0 ? 1 : 0;

    if (more) {
        cp &= 0x3f >> more;
        for (int i = 1; i <= more; i++) {
            if ((p[i] & 0xc0) != 0x80) {
                *text += 1;
                return *p;
            }
            cp = cp << 6 | (p[i] & 0x3f);
        }
    }
    *text += more + 1;
    return cp;
}

/* the bytes written */
static unsigned utf8_to_utf16(const char *in, u8 *out, size_t size)
{
    unsigned pos = 0;

    while (*in && pos + 4 <= size) {
        uint32_t cp = utf8_next(&in);

        if (cp >= 0x10000) {
            cp -= 0x10000;
            set16(out + pos, 0xd800 + (cp >> 10));
            set16(out + pos + 2, 0xdc00 + (cp & 0x3ff));
            pos += 4;
        } else {
            set16(out + pos, cp);
            pos += 2;
        }
    }
    return pos;
}

/* a letter without its case: Latin, its accented letters and Cyrillic, which
 * is what the names of files here are written in */
static uint32_t fold(uint32_t cp)
{
    if (cp < 0x80)
        return tolower(cp);
    if ((cp >= 0xc0 && cp <= 0xde && cp != 0xd7) || (cp >= 0x410 && cp <= 0x42f))
        return cp + 0x20;
    if (cp >= 0x400 && cp <= 0x40f)
        return cp + 0x50;
    if (cp == 0x490)
        return 0x491;
    return cp;
}

/* does the name answer the pattern of a search ("*", "*.txt", "name")? */
static int name_matches(const char *pattern, const char *name)
{
    const char *star = NULL, *star_name = NULL;

    for (;;) {
        const char *p = pattern, *n = name;
        uint32_t pc = *p ? utf8_next(&p) : 0, nc = *n ? utf8_next(&n) : 0;

        if (pc == '<')
            pc = '*';
        else if (pc == '>')
            pc = '?';
        else if (pc == '"')
            pc = '.';
        if (pc == '*') {
            star = pattern = p;
            star_name = name;
            if (!*pattern)
                return 1;
            continue;
        }
        if (!nc)
            return !pc;
        if (pc == '?' || fold(pc) == fold(nc)) {
            pattern = p;
            name = n;
            continue;
        }
        if (!star)
            return 0;
        pattern = star;
        utf8_next(&star_name);
        name = star_name;
    }
}

/* the same name, whatever the case of its letters */
static int same_name(const char *a, const char *b)
{
    for (;;) {
        uint32_t x = *a ? fold(utf8_next(&a)) : 0, y = *b ? fold(utf8_next(&b)) : 0;

        if (x != y)
            return 0;
        if (!x)
            return 1;
    }
}

/**********************************************************************
 *          The shares and the switches, as the Windows side wrote them
 */

/* C:\Users\Public -> where Wine has that drive: dosdevices/c: is a link to it */
static int resolve_dos_path(const char *dos_path, char *out, size_t size)
{
    char link[64], target[PATH_MAX];
    ssize_t n;
    size_t len;

    if (!isalpha((unsigned char)dos_path[0]) || dos_path[1] != ':')
        return 0;
    snprintf(link, sizeof(link), DOSDEVICES "/%c:", tolower((unsigned char)dos_path[0]));
    if ((n = readlink(link, target, sizeof(target) - 1)) <= 0)
        return 0;
    target[n] = 0;
    while (n > 1 && target[n - 1] == '/')
        target[--n] = 0;
    if ((len = snprintf(out, size, "%s", target)) >= size)
        return 0;
    for (dos_path += 2; *dos_path;) {
        size_t part = strcspn(dos_path, "\\/");

        if (part == 2 && !strncmp(dos_path, "..", 2))
            return 0;
        if (part && !(part == 1 && dos_path[0] == '.')) {
            if (len + part + 2 > size)
                return 0;
            out[len++] = '/';
            memcpy(out + len, dos_path, part);
            len += part;
            out[len] = 0;
        }
        dos_path += part;
        if (*dos_path)
            dos_path++;
    }
    return 1;
}

static int right_of(const char *word)
{
    if (!strcmp(word, "full"))
        return RIGHT_FULL;
    if (!strcmp(word, "change"))
        return RIGHT_CHANGE;
    if (!strcmp(word, "read"))
        return RIGHT_READ;
    return RIGHT_NONE;
}

static void chomp(char *line)
{
    size_t len = strlen(line);

    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        line[--len] = 0;
}

static void load_config(void)
{
    FILE *f = fopen(CONFIG_FILE, "re");
    char line[256];
    int new_sharing = 0, new_discovery = 1;

    while (f && fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "sharing=", 8))
            new_sharing = atoi(line + 8) != 0;
        else if (!strncmp(line, "discovery=", 10))
            new_discovery = atoi(line + 10) != 0;
    }
    if (f)
        fclose(f);
    if (new_sharing != sharing || new_discovery != discovery)
        say("file sharing %s, network discovery %s", new_sharing ? "on" : "off", new_discovery ? "on" : "off");
    sharing = new_sharing;
    discovery = new_discovery;
}

static int load_share(const char *file, struct share *share)
{
    char path[PATH_MAX], line[4096], dos_path[1024] = "";
    size_t len = strlen(file) - 6; /* ".share" */
    struct stat st;
    FILE *f;

    if (!len || len >= sizeof(share->name) || len > 80)
        return 0;
    memset(share, 0, sizeof(*share));
    memcpy(share->name, file, len);
    share->allow = RIGHT_READ;
    snprintf(path, sizeof(path), SHARES_DIR "/%s", file);
    if (!(f = fopen(path, "re")))
        return 0;
    while (fgets(line, sizeof(line), f)) {
        chomp(line);
        if (!strncmp(line, "path=", 5))
            snprintf(dos_path, sizeof(dos_path), "%.1000s", line + 5);
        else if (!strncmp(line, "comment=", 8))
            snprintf(share->comment, sizeof(share->comment), "%.255s", line + 8);
        else if (!strncmp(line, "allow=", 6))
            share->allow = right_of(line + 6);
        else if (!strncmp(line, "deny=", 5))
            share->deny = right_of(line + 5);
        else if (!strncmp(line, "limit=", 6))
            share->limit = strtoul(line + 6, NULL, 10);
        else if (!strncmp(line, "caching=", 8))
            share->caching = !strcmp(line + 8, "auto") ? 0x10 : !strcmp(line + 8, "vdo") ? 0x20 :
                             !strcmp(line + 8, "none") ? 0x30 : 0;
    }
    fclose(f);
    return resolve_dos_path(dos_path, share->host_path, sizeof(share->host_path)) &&
           !stat(share->host_path, &st) && S_ISDIR(st.st_mode);
}

static void load_shares(void)
{
    static struct share loaded[MAX_SHARES];
    struct dirent *de;
    DIR *dir = opendir(SHARES_DIR);
    int count = 0;

    while (dir && (de = readdir(dir)) && count < MAX_SHARES) {
        size_t len = strlen(de->d_name);

        if (len > 6 && !strcasecmp(de->d_name + len - 6, ".share") && load_share(de->d_name, &loaded[count]))
            count++;
    }
    if (dir)
        closedir(dir);
    pthread_mutex_lock(&lock);
    for (int i = 0; i < count; i++)
        for (int j = 0; j < share_count; j++)
            if (same_name(loaded[i].name, shares[j].name))
                loaded[i].users = shares[j].users;
    memcpy(shares, loaded, count * sizeof(*shares));
    share_count = count;
    pthread_mutex_unlock(&lock);
}

static void read_computer_name(void)
{
    char name[64] = "";

    if (gethostname(name, sizeof(name) - 1) || !name[0])
        return;
    name[strcspn(name, ".")] = 0;
    name[15] = 0;
    for (int i = 0; name[i]; i++)
        name[i] = toupper((unsigned char)name[i]);
    snprintf(computer, sizeof(computer), "%s", name);
}

static void *watch_settings(void *arg)
{
    int fd = inotify_init1(IN_CLOEXEC);

    (void)arg;
    if (fd >= 0) {
        inotify_add_watch(fd, SMB_DIR, IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE | IN_CREATE);
        inotify_add_watch(fd, SHARES_DIR, IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE);
    }
    for (;;) {
        struct pollfd pfd = {fd, POLLIN, 0};
        char events[4096];

        /* a look every minute as well: a drive that came later has its folders now */
        if (fd < 0)
            sleep(5);
        else if (poll(&pfd, 1, 60000) > 0 && read(fd, events, sizeof(events)) < 0) {
        }
        read_computer_name();
        load_config();
        load_shares();
    }
    return NULL;
}

/**********************************************************************
 *          What a file is, as Windows wants to know it
 */

struct meta {
    uint64_t created, accessed, written, changed, size, allocated, id;
    uint32_t attributes;
    int is_dir;
};

static int get_meta(int fd, const char *path, struct meta *m)
{
    struct statx sx;
    u8 ntfs[4];
    ssize_t got;

    if (statx(fd >= 0 ? fd : AT_FDCWD, fd >= 0 ? "" : path, fd >= 0 ? AT_EMPTY_PATH : 0,
              STATX_BASIC_STATS | STATX_BTIME, &sx))
        return -1;
    m->is_dir = S_ISDIR(sx.stx_mode);
    m->accessed = filetime(sx.stx_atime.tv_sec, sx.stx_atime.tv_nsec);
    m->written = filetime(sx.stx_mtime.tv_sec, sx.stx_mtime.tv_nsec);
    m->changed = filetime(sx.stx_ctime.tv_sec, sx.stx_ctime.tv_nsec);
    m->created = (sx.stx_mask & STATX_BTIME) && sx.stx_btime.tv_sec > 0
                     ? filetime(sx.stx_btime.tv_sec, sx.stx_btime.tv_nsec) : m->written;
    m->size = m->is_dir ? 0 : sx.stx_size;
    m->allocated = m->is_dir ? 0 : (uint64_t)sx.stx_blocks * 512;
    m->id = sx.stx_ino;
    /* NTFS keeps Windows' own attributes: hidden, system, read-only */
    got = fd >= 0 ? fgetxattr(fd, "system.ntfs_attrib", ntfs, 4) : getxattr(path, "system.ntfs_attrib", ntfs, 4);
    m->attributes = got == 4 ? get32(ntfs) & 0x27 : sx.stx_mode & 0222 ? 0 : 1;
    if (m->is_dir)
        m->attributes = (m->attributes & ~0x20u) | 0x10;
    else if (!m->attributes)
        m->attributes = 0x80; /* normal */
    return 0;
}

static uint32_t nt_status_of(int err)
{
    switch (err) {
    case 0: return 0;
    case ENOENT: return 0xc0000034;     /* object name not found */
    case ENOTDIR: return 0xc000003a;    /* object path not found */
    case EEXIST: return 0xc0000035;     /* object name collision */
    case EISDIR: return 0xc00000ba;     /* file is a directory */
    case ENOTEMPTY: return 0xc0000101;  /* directory not empty */
    case ENOSPC:
    case EDQUOT: return 0xc000007f;     /* disk full */
    case EROFS: return 0xc00000a2;      /* media write protected */
    case ENAMETOOLONG:
    case EINVAL: return 0xc0000033;     /* object name invalid */
    case EMFILE:
    case ENFILE: return 0xc000011f;     /* too many opened files */
    case EBUSY:
    case ETXTBSY: return 0xc0000043;    /* sharing violation */
    case ENOMEM: return 0xc000009a;     /* insufficient resources */
    case EIO: return 0xc0000185;        /* device I/O error */
    default: return 0xc0000022;         /* access denied */
    }
}

#define ST_MORE_PROCESSING   0xc0000016u
#define ST_BUFFER_OVERFLOW   0x80000005u
#define ST_NO_MORE_FILES     0x80000006u
#define ST_INVALID_PARAMETER 0xc000000du
#define ST_INVALID_INFO      0xc0000003u
#define ST_LENGTH_MISMATCH   0xc0000004u
#define ST_INVALID_HANDLE    0xc0000008u /* "file closed" */
#define ST_NO_SUCH_FILE      0xc000000fu
#define ST_INVALID_REQUEST   0xc0000010u /* invalid device request */
#define ST_END_OF_FILE       0xc0000011u
#define ST_ACCESS_DENIED     0xc0000022u
#define ST_BUFFER_TOO_SMALL  0xc0000023u
#define ST_NAME_INVALID      0xc0000033u
#define ST_NAME_NOT_FOUND    0xc0000034u
#define ST_NAME_COLLISION    0xc0000035u
#define ST_IS_DIRECTORY      0xc00000bau
#define ST_NOT_SUPPORTED     0xc00000bbu
#define ST_NETNAME_DELETED   0xc00000c9u
#define ST_BAD_NETWORK_NAME  0xc00000ccu
#define ST_REQUEST_NOT_ACCEPTED 0xc00000d0u
#define ST_DIRECTORY_NOT_EMPTY 0xc0000101u
#define ST_NOT_A_DIRECTORY   0xc0000103u
#define ST_SESSION_DELETED   0xc0000203u
#define ST_NOT_FOUND         0xc0000225u
#define ST_PIPE_EMPTY        0xc00000d9u
#define ST_PENDING           0x00000103u
#define ST_NOTIFY_CLEANUP    0x0000010bu
#define ST_NOTIFY_ENUM_DIR   0x0000010cu
#define ST_CANCELLED         0xc0000120u

/* a name inside a share, "Folder\file.txt", joined to the share's folder:
 * no "..", nothing Windows would not name a file with */
static uint32_t join_path(const char *root, const char *rel, char *out, size_t size)
{
    size_t len = strlen(root);

    if (len >= size)
        return ST_NAME_INVALID;
    memcpy(out, root, len + 1);
    while (*rel) {
        size_t part = strcspn(rel, "\\");

        if (part) {
            if ((part == 1 && rel[0] == '.') || (part == 2 && rel[0] == '.' && rel[1] == '.') || part > 255 ||
                len + part + 2 > size)
                return ST_NAME_INVALID;
            for (size_t i = 0; i < part; i++)
                if ((unsigned char)rel[i] < 0x20 || strchr("/:*?\"<>|", rel[i]))
                    return rel[i] == ':' ? ST_NAME_NOT_FOUND /* a stream: there are none */ : ST_NAME_INVALID;
            out[len++] = '/';
            memcpy(out + len, rel, part);
            len += part;
            out[len] = 0;
        }
        rel += part;
        if (*rel)
            rel++;
    }
    return 0;
}

/**********************************************************************
 *          The logon: NTLMSSP inside SPNEGO, the server's half
 */

static const u8 ntlmssp_oid[] = {0x06, 0x0a, 0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x02, 0x0a};

/* tag, length, content; dst and content apart */
static unsigned der(u8 *dst, u8 tag, const u8 *content, unsigned len)
{
    unsigned head = len < 0x80 ? 2 : len < 0x100 ? 3 : 4;

    dst[0] = tag;
    if (len < 0x80) {
        dst[1] = len;
    } else if (len < 0x100) {
        dst[1] = 0x81;
        dst[2] = len;
    } else {
        dst[1] = 0x82;
        dst[2] = len >> 8;
        dst[3] = len;
    }
    memcpy(dst + head, content, len);
    return head + len;
}

/* what the answer to NEGOTIATE says can log on here: NTLMSSP alone */
static unsigned spnego_init(u8 *out)
{
    static const u8 spnego_oid[] = {0x06, 0x06, 0x2b, 0x06, 0x01, 0x05, 0x05, 0x02};
    static const char hint[] = "not_defined_in_RFC4178@please_ignore";
    u8 a[128], b[128], c[160];
    unsigned len, n;

    n = der(a, 0x30, ntlmssp_oid, sizeof(ntlmssp_oid));
    len = der(c, 0xa0, a, n);                           /* mechTypes */
    n = der(a, 0x1b, (const u8 *)hint, sizeof(hint) - 1);
    n = der(b, 0xa0, a, n);
    n = der(a, 0x30, b, n);
    len += der(c + len, 0xa3, a, n);                    /* negHints, as Windows sends them */
    n = der(a, 0x30, c, len);
    n = der(b, 0xa0, a, n);
    memcpy(c, spnego_oid, sizeof(spnego_oid));
    memcpy(c + sizeof(spnego_oid), b, n);
    return der(out, 0x60, c, sizeof(spnego_oid) + n);
}

/* the answer of a server to the first or the last message of a logon */
static unsigned spnego_reply(u8 *out, int state, int with_mech, const u8 *token, unsigned token_len)
{
    u8 a[1200], b[1200], seq[1300];
    unsigned len = 0, n;

    seq[len++] = 0xa0; seq[len++] = 3; seq[len++] = 0x0a; seq[len++] = 1; seq[len++] = state;
    if (with_mech)
        len += der(seq + len, 0xa1, ntlmssp_oid, sizeof(ntlmssp_oid));
    if (token_len) {
        n = der(a, 0x04, token, token_len);
        len += der(seq + len, 0xa2, a, n);
    }
    n = der(b, 0x30, seq, len);
    return der(out, 0xa1, b, n);
}

static unsigned av_pair(u8 *out, unsigned id, const char *text)
{
    unsigned bytes = utf8_to_utf16(text, out + 4, 64);

    set16(out, id);
    set16(out + 2, bytes);
    return 4 + bytes;
}

/* the challenge: who this is, for the client to answer; nobody's answer is
 * checked afterwards (the comment at the top) */
static unsigned ntlm_challenge(u8 *out, uint32_t client_flags)
{
    char lower[64];
    unsigned name_len, info_len = 0, i;
    u8 *info;

    for (i = 0; computer[i]; i++)
        lower[i] = tolower((unsigned char)computer[i]);
    lower[i] = 0;
    memset(out, 0, 56);
    memcpy(out, "NTLMSSP", 8);
    set32(out + 8, 2);
    name_len = utf8_to_utf16(computer, out + 56, 64);
    set16(out + 12, name_len);
    set16(out + 14, name_len);
    set32(out + 16, 56);
    /* Unicode, target asked for, NTLM, a server, target info, its version; and
     * of what the client offered: signing, extended security, 128, key exchange, 56 */
    set32(out + 20, 0x02820205 | (client_flags & 0xe0088010));
    random_bytes(out + 24, 8);
    info = out + 56 + name_len;
    info_len += av_pair(info + info_len, 2, computer);  /* NetBIOS domain: a workgroup's computer is its own */
    info_len += av_pair(info + info_len, 1, computer);  /* NetBIOS computer */
    info_len += av_pair(info + info_len, 4, lower);     /* DNS domain */
    info_len += av_pair(info + info_len, 3, lower);     /* DNS computer */
    set16(info + info_len, 7);                          /* the time */
    set16(info + info_len + 2, 8);
    set64(info + info_len + 4, filetime_now());
    info_len += 12;
    set32(info + info_len, 0);                          /* the end */
    info_len += 4;
    set16(out + 40, info_len);
    set16(out + 42, info_len);
    set32(out + 44, 56 + name_len);
    out[48] = 6; out[49] = 1; set16(out + 50, 7601); out[55] = 15; /* Windows 6.1, NTLM revision 15 */
    return 56 + name_len + info_len;
}

/**********************************************************************
 *          A connection: its logon, the shares it opened, its files
 */

struct tree {
    int used, is_ipc, allow;
    uint32_t id;
    unsigned caching;
    char share[MAX_NAME];
    char path[MAX_PATH_LEN];
};

struct handle {
    u8 id[16];
    int fd, is_dir, is_pipe, delete_on_close;
    struct tree *tree;
    char path[MAX_PATH_LEN];    /* the file itself */
    char name[1024];            /* its name in the share, "Folder\file.txt" */
    /* a folder being listed: its names taken once, given out in parts */
    char **names;
    int name_count, name_at, listed, nothing_yet;
    char pattern[1024];
    /* the pipe of the Server service: the fragments of an answer, each read as a message */
    u8 *pipe;
    unsigned fragment[64], fragment_count, fragment_at, fragment_read, pipe_len, max_fragment;
};

/* a folder somebody waits to hear about: the request stays unanswered until
 * something changes in it */
struct notify {
    struct handle *handle;
    int watch;
    uint64_t id;
    u8 header[64];
};

struct conn {
    int sock;
    char peer[64];
    unsigned dialect;
    uint64_t session_id;
    int logged_on;
    struct tree trees[MAX_TREES];
    struct handle *handles[MAX_HANDLES];
    int inotify;
    uint64_t notify_count;
    struct notify notifies[MAX_NOTIFIES];
};

struct request {
    struct conn *c;
    unsigned command;
    const u8 *header;
    const u8 *in;       /* the body: what follows the 64 bytes of the header */
    unsigned in_len;
    u8 *out;            /* the body of the answer, BODY_SIZE of room */
    unsigned out_len;
    uint64_t session_id;
    uint32_t tree_id;
    uint32_t needed;    /* with "buffer too small": the size that would do */
    uint64_t async_id;  /* with "pending": the name the answer will come under */
};

/* an offset of a request counts from its header */
static const u8 *in_buffer(const struct request *r, unsigned offset, unsigned len)
{
    if (!len)
        return r->in;
    if (offset < 64 || offset - 64 > r->in_len || len > r->in_len - (offset - 64))
        return NULL;
    return r->in + offset - 64;
}

static struct tree *find_tree(struct conn *c, uint32_t id)
{
    for (int i = 0; i < MAX_TREES; i++)
        if (c->trees[i].used && c->trees[i].id == id)
            return &c->trees[i];
    return NULL;
}

static struct handle *find_handle(struct conn *c, const u8 *id)
{
    uint32_t index = get32(id + 8);

    if (index >= MAX_HANDLES || !c->handles[index] || memcmp(c->handles[index]->id, id, 16))
        return NULL;
    return c->handles[index];
}

static struct handle *new_handle(struct conn *c, struct tree *tree)
{
    for (uint32_t i = 0; i < MAX_HANDLES; i++) {
        struct handle *h;

        if (c->handles[i])
            continue;
        if (!(h = calloc(1, sizeof(*h))))
            return NULL;
        random_bytes(h->id, 16);
        set32(h->id + 8, i);
        h->fd = -1;
        h->tree = tree;
        c->handles[i] = h;
        return h;
    }
    return NULL;
}

static void forget_listing(struct handle *h)
{
    for (int i = 0; i < h->name_count; i++)
        free(h->names[i]);
    free(h->names);
    h->names = NULL;
    h->name_count = h->name_at = 0;
}

static void end_notify(struct conn *c, struct notify *n, uint32_t status);

static void free_handle(struct conn *c, struct handle *h)
{
    for (int i = 0; i < MAX_NOTIFIES; i++)
        if (c->notifies[i].handle == h)
            end_notify(c, &c->notifies[i], ST_NOTIFY_CLEANUP);
    if (h->delete_on_close) {
        if (h->is_dir ? rmdir(h->path) : unlink(h->path)) {
        }
    }
    if (h->fd >= 0)
        close(h->fd);
    forget_listing(h);
    free(h->pipe);
    c->handles[get32(h->id + 8)] = NULL;
    free(h);
}

static void free_tree(struct conn *c, struct tree *t)
{
    for (int i = 0; i < MAX_HANDLES; i++)
        if (c->handles[i] && c->handles[i]->tree == t)
            free_handle(c, c->handles[i]);
    if (!t->is_ipc) {
        pthread_mutex_lock(&lock);
        for (int i = 0; i < share_count; i++)
            if (same_name(shares[i].name, t->share) && shares[i].users)
                shares[i].users--;
        pthread_mutex_unlock(&lock);
    }
    t->used = 0;
}

/**********************************************************************
 *          The pipe of the Server service (srvsvc over DCE/RPC): the list
 *          of the shares, a share, the computer
 */

struct ndr {
    u8 *data;
    unsigned len, size;
};

static void ndr32(struct ndr *n, uint32_t value)
{
    while (n->len & 3 && n->len < n->size)
        n->data[n->len++] = 0;
    if (n->len + 4 > n->size)
        return;
    set32(n->data + n->len, value);
    n->len += 4;
}

/* a string as NDR carries one: how long it may be, from where, how long it is */
static void ndr_string(struct ndr *n, const char *text)
{
    u8 wide[1024];
    unsigned bytes = utf8_to_utf16(text, wide, sizeof(wide) - 2), chars = bytes / 2 + 1;

    ndr32(n, chars);
    ndr32(n, 0);
    ndr32(n, chars);
    if (n->len + bytes + 2 > n->size)
        return;
    memcpy(n->data + n->len, wide, bytes);
    set16(n->data + n->len + bytes, 0);
    n->len += bytes + 2;
}

struct ndr_in {
    const u8 *data;
    unsigned len, pos;
};

static uint32_t in32(struct ndr_in *n)
{
    uint32_t value;

    n->pos = (n->pos + 3) & ~3u;
    if (n->pos + 4 > n->len) {
        n->pos = n->len;
        return 0;
    }
    value = get32(n->data + n->pos);
    n->pos += 4;
    return value;
}

static void in_string(struct ndr_in *n, char *out, size_t size)
{
    uint32_t chars;

    out[0] = 0;
    in32(n);
    in32(n);
    chars = in32(n);
    if (chars > 1024 || n->pos + chars * 2 > n->len) {
        n->pos = n->len;
        return;
    }
    if (chars && !utf16_to_utf8(n->data + n->pos, chars - 1, out, size))
        out[0] = 0;
    n->pos += chars * 2;
}

static void in_server_name(struct ndr_in *n)
{
    char name[1024];

    if (in32(n))
        in_string(n, name, sizeof(name));
}

/* NetrShareEnum: the names of level 0, with their kinds and remarks of level 1 */
static uint32_t srvsvc_share_enum(struct ndr_in *in, struct ndr *out)
{
    uint32_t level, resume, status = 0, count = 0, ref = 0x20010;

    in_server_name(in);
    level = in32(in);
    in32(in);
    if (in32(in)) { /* the caller's own container: empty */
        in32(in);
        in32(in);
    }
    in32(in);
    if ((resume = in32(in)))
        in32(in);

    ndr32(out, level);
    ndr32(out, level);
    ndr32(out, 0x20000);
    if (level > 1) {
        /* the paths and the passwords are for administrators; the other levels are not there */
        status = level == 2 || level == 502 || level == 503 ? 5 : 124;
        ndr32(out, 0);
        ndr32(out, 0);
    } else {
        pthread_mutex_lock(&lock);
        count = share_count + 1;
        ndr32(out, count);
        ndr32(out, 0x20004);
        ndr32(out, count);
        for (uint32_t i = 0; i < count; i++) {
            ndr32(out, ref += 4);
            if (level == 1) {
                ndr32(out, i < (uint32_t)share_count ? 0 : 0x80000003); /* a folder; the pipes, hidden */
                ndr32(out, ref += 4);
            }
        }
        for (uint32_t i = 0; i < count; i++) {
            ndr_string(out, i < (uint32_t)share_count ? shares[i].name : "IPC$");
            if (level == 1)
                ndr_string(out, i < (uint32_t)share_count ? shares[i].comment : "Remote IPC");
        }
        pthread_mutex_unlock(&lock);
    }
    ndr32(out, count);
    if (resume) {
        ndr32(out, 0x20008);
        ndr32(out, 0);
    } else {
        ndr32(out, 0);
    }
    return status;
}

static uint32_t srvsvc_share_info(struct ndr_in *in, struct ndr *out)
{
    char name[1024], comment[256] = "Remote IPC";
    uint32_t level, type = 0x80000003;
    int found;

    in_server_name(in);
    in_string(in, name, sizeof(name));
    level = in32(in);
    found = !strcasecmp(name, "IPC$");
    pthread_mutex_lock(&lock);
    for (int i = 0; i < share_count && !found; i++) {
        if (!same_name(shares[i].name, name))
            continue;
        snprintf(name, sizeof(name), "%.255s", shares[i].name);
        snprintf(comment, sizeof(comment), "%.255s", shares[i].comment);
        type = 0;
        found = 1;
    }
    pthread_mutex_unlock(&lock);

    ndr32(out, level);
    if (!found || level > 1) {
        ndr32(out, 0);
        return !found ? 2310 /* this shared resource does not exist */ : level == 2 || level == 502 ? 5 : 124;
    }
    ndr32(out, 0x20000);
    ndr32(out, 0x20004);
    if (level == 1) {
        ndr32(out, type);
        ndr32(out, 0x20008);
    }
    ndr_string(out, name);
    if (level == 1)
        ndr_string(out, comment);
    return 0;
}

static uint32_t srvsvc_server_info(struct ndr_in *in, struct ndr *out)
{
    uint32_t level;

    in_server_name(in);
    level = in32(in);
    ndr32(out, level);
    if (level != 100 && level != 101) {
        ndr32(out, 0);
        return level == 102 || level == 502 || level == 503 ? 5 : 124;
    }
    ndr32(out, 0x20000);
    ndr32(out, 500);        /* the platform: Windows NT */
    ndr32(out, 0x20004);
    if (level == 101) {
        ndr32(out, 6);
        ndr32(out, 1);
        ndr32(out, 0x1003); /* a workstation, a server, NT */
        ndr32(out, 0x20008);
    }
    ndr_string(out, computer);
    if (level == 101)
        ndr_string(out, "");
    return 0;
}

static void pipe_message(struct handle *h, const u8 *data, unsigned len)
{
    u8 *grown;

    if (h->fragment_at == h->fragment_count) { /* everything before was read */
        h->fragment_at = h->fragment_count = h->fragment_read = h->pipe_len = 0;
    }
    if (h->fragment_count == 64 || !(grown = realloc(h->pipe, h->pipe_len + len)))
        return;
    memcpy(grown + h->pipe_len, data, len);
    h->pipe = grown;
    h->pipe_len += len;
    h->fragment[h->fragment_count++] = len;
}

/* what is left of the message being read, as much as fits */
static unsigned pipe_read(struct handle *h, u8 *out, unsigned room, int *more)
{
    unsigned start = 0, left, n;

    *more = 0;
    if (h->fragment_at == h->fragment_count)
        return 0;
    for (unsigned i = 0; i < h->fragment_at; i++)
        start += h->fragment[i];
    left = h->fragment[h->fragment_at] - h->fragment_read;
    n = left < room ? left : room;
    memcpy(out, h->pipe + start + h->fragment_read, n);
    if (n == left) {
        h->fragment_at++;
        h->fragment_read = 0;
    } else {
        h->fragment_read += n;
        *more = 1;
    }
    return n;
}

static void rpc_header(u8 *out, const u8 *request, unsigned type, unsigned flags, unsigned len)
{
    memset(out, 0, 16);
    out[0] = 5;
    out[2] = type;
    out[3] = flags;
    out[4] = 0x10;                  /* little endian, ASCII, IEEE */
    set16(out + 8, len);
    memcpy(out + 12, request + 12, 4);
}

/* a call's answer, in as many fragments as the size the two agreed on makes */
static void rpc_answer(struct handle *h, const u8 *request, const u8 *stub, unsigned stub_len)
{
    unsigned room = h->max_fragment - 24, sent = 0;
    u8 fragment[4280];

    do {
        unsigned n = stub_len - sent < room ? stub_len - sent : room;

        rpc_header(fragment, request, 2, (sent ? 0 : 1) | (sent + n == stub_len ? 2 : 0), 24 + n);
        set32(fragment + 16, stub_len - sent);
        memcpy(fragment + 20, request + 20, 2);
        fragment[22] = fragment[23] = 0;
        memcpy(fragment + 24, stub + sent, n);
        pipe_message(h, fragment, 24 + n);
        sent += n;
    } while (sent < stub_len);
}

static void rpc_bind(struct handle *h, const u8 *in, unsigned len)
{
    static const u8 srvsvc[16] = {0xc8, 0x4f, 0x32, 0x4b, 0x70, 0x16, 0xd3, 0x01,
                                  0x12, 0x78, 0x5a, 0x47, 0xbf, 0x6e, 0xe1, 0x88};
    static const u8 ndr[20] = {0x04, 0x5d, 0x88, 0x8a, 0xeb, 0x1c, 0xc9, 0x11, 0x9f, 0xe8,
                               0x08, 0x00, 0x2b, 0x10, 0x48, 0x60, 2, 0, 0, 0};
    static const char address[] = "\\PIPE\\srvsvc";
    u8 out[1024];
    unsigned pos, contexts, at = 28, count_at;

    if (len < 28)
        return;
    h->max_fragment = get16(in + 16);
    if (h->max_fragment > 4280 || h->max_fragment < 1024)
        h->max_fragment = 4280;
    contexts = in[24];
    set16(out + 16, h->max_fragment);
    set16(out + 18, h->max_fragment);
    set32(out + 20, 0x53f0);
    pos = 24;
    if (in[2] == 11) {
        set16(out + pos, sizeof(address));
        memcpy(out + pos + 2, address, sizeof(address));
        pos += 2 + sizeof(address);
    } else {
        set16(out + pos, 0);
        pos += 2;
    }
    while (pos & 3)
        out[pos++] = 0;
    count_at = pos;
    set32(out + pos, 0);
    pos += 4;
    for (unsigned i = 0; i < contexts && at + 24 <= len && pos + 24 <= sizeof(out); i++) {
        unsigned syntaxes = in[at + 2], accepted = 0;

        for (unsigned j = 0; j < syntaxes && at + 24 + (j + 1) * 20 <= len; j++)
            if (!memcmp(in + at + 24 + j * 20, ndr, 20) && !memcmp(in + at + 4, srvsvc, 16))
                accepted = 1;
        memset(out + pos, 0, 24);
        if (accepted) {
            memcpy(out + pos + 4, ndr, 20);
        } else {
            set16(out + pos, 2);        /* refused by the provider: */
            set16(out + pos + 2, 2);    /* none of the transfer syntaxes it proposed */
        }
        pos += 24;
        out[count_at]++;
        at += 24 + syntaxes * 20;
    }
    rpc_header(out, in, in[2] == 11 ? 12 : 15, 3, pos);
    pipe_message(h, out, pos);
}

/* what was written to the pipe: a bind or a call; its answer waits to be read */
static void rpc_request(struct handle *h, const u8 *in, unsigned len)
{
    struct ndr_in args;
    struct ndr out = {NULL, 0, 60000};
    unsigned stub = 24;
    uint32_t status;

    if (len < 16 || in[0] != 5)
        return;
    if (in[2] == 11 || in[2] == 14) {
        rpc_bind(h, in, len);
        return;
    }
    if (in[2] != 0 || len < 24)
        return;
    if (!h->max_fragment)
        h->max_fragment = 4280;
    if (in[3] & 0x80)
        stub += 16; /* an object's id before the arguments */
    args.data = in + (stub < len ? stub : len);
    args.len = stub < len ? len - stub : 0;
    args.pos = 0;
    if (!(out.data = malloc(out.size)))
        return;
    switch (get16(in + 22)) {
    case 15:
        status = srvsvc_share_enum(&args, &out);
        break;
    case 16:
        status = srvsvc_share_info(&args, &out);
        break;
    case 21:
        status = srvsvc_server_info(&args, &out);
        break;
    default: {
        /* a call this service does not have: a fault, "operation out of range" */
        u8 fault[32];

        rpc_header(fault, in, 3, 3, sizeof(fault));
        memset(fault + 16, 0, 16);
        memcpy(fault + 20, in + 20, 2);
        set32(fault + 24, 0x1c010002);
        pipe_message(h, fault, sizeof(fault));
        free(out.data);
        return;
    }
    }
    ndr32(&out, status);
    rpc_answer(h, in, out.data, out.len);
    free(out.data);
}

/**********************************************************************
 *          The commands of SMB 2
 */

static void negotiate_answer(struct request *r, unsigned dialect)
{
    u8 *out = r->out;
    unsigned len;

    memset(out, 0, 64);
    set16(out, 65);
    set16(out + 2, 1);                  /* signing: can, need not */
    set16(out + 4, dialect);
    memcpy(out + 8, server_guid, 16);
    set32(out + 28, MAX_IO);
    set32(out + 32, MAX_IO);
    set32(out + 36, MAX_IO);
    set64(out + 40, filetime_now());
    set64(out + 48, started);
    len = spnego_init(out + 64);
    set16(out + 56, 64 + 64);
    set16(out + 58, len);
    r->out_len = 64 + len;
}

static uint32_t smb_negotiate(struct request *r)
{
    unsigned count, dialect = 0;

    if (r->in_len < 36)
        return ST_INVALID_PARAMETER;
    count = get16(r->in + 2);
    for (unsigned i = 0; i < count && 36 + i * 2 + 2 <= r->in_len; i++) {
        unsigned offered = get16(r->in + 36 + i * 2);

        if ((offered == 0x0202 || offered == 0x0210) && offered > dialect)
            dialect = offered;
    }
    if (!dialect)
        return ST_NOT_SUPPORTED;
    r->c->dialect = dialect;
    negotiate_answer(r, dialect);
    return 0;
}

static uint32_t smb_session_setup(struct request *r)
{
    struct conn *c = r->c;
    const u8 *blob, *ntlm = NULL;
    unsigned len, left = 0, n = 0, token_len = 0;
    u8 token[600], *out = r->out;
    int bare;

    if (r->in_len < 24)
        return ST_INVALID_PARAMETER;
    len = get16(r->in + 14);
    if (!(blob = in_buffer(r, get16(r->in + 12), len)))
        return ST_INVALID_PARAMETER;
    if (len >= 12 && (ntlm = memmem(blob, len, "NTLMSSP", 8)))
        left = len - (ntlm - blob);
    bare = ntlm == blob; /* NTLMSSP as it is, not inside SPNEGO: answered the same way */

    if (!r->session_id) {
        do
            random_bytes(&c->session_id, sizeof(c->session_id));
        while (!c->session_id || c->session_id == ~0ULL);
        c->logged_on = 0;
    } else if (r->session_id != c->session_id) {
        return ST_SESSION_DELETED;
    }
    r->session_id = c->session_id;
    memset(out, 0, 8);
    set16(out, 9);
    set16(out + 4, 64 + 8);

    if (ntlm && left >= 12 && get32(ntlm + 8) == 3) {
        /* the answer to the challenge. Whose it is and whether it is right is
         * not looked at: whoever it is, a guest */
        c->logged_on = 1;
        set16(out + 2, 1);
        if (!bare)
            n = spnego_reply(out + 8, 0, 0, NULL, 0);
        set16(out + 6, n);
        r->out_len = 8 + n;
        return 0;
    }
    if (ntlm && left >= 12 && get32(ntlm + 8) == 1)
        token_len = ntlm_challenge(token, left >= 16 ? get32(ntlm + 12) : 0);
    if (bare) {
        memcpy(out + 8, token, token_len);
        n = token_len;
    } else {
        /* no NTLMSSP yet (Kerberos was tried first): NTLMSSP is what there is here */
        n = spnego_reply(out + 8, 1, 1, token, token_len);
    }
    set16(out + 6, n);
    r->out_len = 8 + n;
    return ST_MORE_PROCESSING;
}

static uint32_t smb_tree_connect(struct request *r)
{
    struct conn *c = r->c;
    struct tree *t = NULL;
    char unc[1024], *name;
    const u8 *path;
    unsigned len;
    uint32_t status = 0;

    if (r->in_len < 8)
        return ST_INVALID_PARAMETER;
    len = get16(r->in + 6);
    if (!(path = in_buffer(r, get16(r->in + 4), len)) || len / 2 > 300 ||
        !utf16_to_utf8(path, len / 2, unc, sizeof(unc)))
        return ST_BAD_NETWORK_NAME;
    name = strrchr(unc, '\\');
    name = name ? name + 1 : unc;
    for (int i = 0; i < MAX_TREES && !t; i++)
        if (!c->trees[i].used)
            t = &c->trees[i];
    if (!t)
        return 0xc000009a; /* insufficient resources */
    memset(t, 0, sizeof(*t));

    if (!strcasecmp(name, "IPC$")) {
        t->is_ipc = 1;
        t->allow = RIGHT_READ;
    } else {
        struct share *share = NULL;

        pthread_mutex_lock(&lock);
        for (int i = 0; i < share_count && !share; i++)
            if (same_name(shares[i].name, name))
                share = &shares[i];
        if (!share)
            status = ST_BAD_NETWORK_NAME;
        else if (share->deny != RIGHT_NONE || share->allow == RIGHT_NONE)
            status = ST_ACCESS_DENIED;
        else if (share->limit && share->users >= share->limit)
            status = ST_REQUEST_NOT_ACCEPTED;
        else {
            share->users++;
            t->allow = share->allow;
            t->caching = share->caching;
            snprintf(t->share, sizeof(t->share), "%s", share->name);
            snprintf(t->path, sizeof(t->path), "%s", share->host_path);
        }
        pthread_mutex_unlock(&lock);
        if (status)
            return status;
        say("\\\\%s\\%s opened from %s", computer, t->share, c->peer);
    }
    do
        random_bytes(&t->id, sizeof(t->id));
    while (!t->id || t->id == ~0u || find_tree(c, t->id));
    t->used = 1;
    r->tree_id = t->id;

    memset(r->out, 0, 16);
    set16(r->out, 16);
    r->out[2] = t->is_ipc ? 2 : 1;      /* a pipe, a disk */
    set32(r->out + 4, t->caching);      /* what Offline Files may keep */
    set32(r->out + 12, t->allow >= RIGHT_CHANGE ? 0x001f01ff : 0x001200a9);
    r->out_len = 16;
    return 0;
}

static void put_times(u8 *out, const struct meta *m)
{
    set64(out, m->created);
    set64(out + 8, m->accessed);
    set64(out + 16, m->written);
    set64(out + 24, m->changed);
}

static uint32_t smb_create(struct request *r, struct tree *t)
{
    struct conn *c = r->c;
    const u8 *in = r->in, *wide;
    char name[1024], path[MAX_PATH_LEN], *end;
    uint32_t access, disposition, options, status, action = 1;
    unsigned name_len;
    int exists, creating = 0, truncating = 0, want_data, want_change, fd = -1;
    struct handle *h;
    struct meta m;

    if (r->in_len < 56)
        return ST_INVALID_PARAMETER;
    access = get32(in + 24);
    disposition = get32(in + 36);
    options = get32(in + 40);
    name_len = get16(in + 46);
    if (!(wide = in_buffer(r, get16(in + 44), name_len)) || name_len / 2 > 500 ||
        !utf16_to_utf8(wide, name_len / 2, name, sizeof(name)))
        return ST_NAME_INVALID;
    while (name[0] == '\\')
        memmove(name, name + 1, strlen(name));
    for (end = name + strlen(name); end > name && end[-1] == '\\'; end--)
        end[-1] = 0;

    if (t->is_ipc) {
        if (strcasecmp(name, "srvsvc"))
            return ST_NAME_NOT_FOUND;
        if (!(h = new_handle(c, t)))
            return 0xc000011f;
        h->is_pipe = 1;
        memset(r->out, 0, 88);
        set16(r->out, 89);
        set32(r->out + 4, 1);
        set32(r->out + 56, 0x80);
        memcpy(r->out + 64, h->id, 16);
        r->out_len = 88;
        return 0;
    }

    if ((status = join_path(t->path, name, path, sizeof(path))))
        return status;
    exists = !get_meta(-1, path, &m);
    if (!exists && name[0]) {
        /* is it the file that is not there, or the folder it would be in? */
        char parent[MAX_PATH_LEN];
        struct stat st;

        snprintf(parent, sizeof(parent), "%s", path);
        *strrchr(parent, '/') = 0;
        if (stat(parent, &st) || !S_ISDIR(st.st_mode))
            return 0xc000003a; /* object path not found */
    }
    if (exists && (options & 0x01) && !m.is_dir)
        return ST_NOT_A_DIRECTORY;
    if (exists && (options & 0x40) && m.is_dir)
        return ST_IS_DIRECTORY;
    switch (disposition) {
    case 1: /* open */
        if (!exists) return ST_NAME_NOT_FOUND;
        break;
    case 2: /* create */
        if (exists) return ST_NAME_COLLISION;
        creating = 1;
        break;
    case 3: /* open, or create */
        creating = !exists;
        break;
    case 4: /* overwrite */
        if (!exists) return ST_NAME_NOT_FOUND;
        truncating = 1;
        break;
    case 0: /* supersede */
    case 5: /* overwrite, or create */
        creating = !exists;
        truncating = exists;
        break;
    default:
        return ST_INVALID_PARAMETER;
    }
    if (truncating && m.is_dir)
        return ST_IS_DIRECTORY;
    want_data = (access & 0x50000006) != 0;                 /* write, append, generic write and all */
    want_change = want_data || (access & 0x000d0150) != 0;  /* attributes, delete, the owner and the rights */
    if (t->allow < RIGHT_CHANGE && (want_change || creating || truncating || (options & 0x1000)))
        return ST_ACCESS_DENIED;

    if (creating && (options & 0x01)) {
        if (mkdir(path, 0755))
            return nt_status_of(errno);
        fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    } else if (creating) {
        fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    } else if (m.is_dir) {
        fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    } else {
        /* "as much as I may" is to write as well where the share lets one */
        int writing = want_data || ((access & 0x02000000) && t->allow >= RIGHT_CHANGE);

        fd = open(path, (writing ? O_RDWR : O_RDONLY) | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0 && writing && !want_data)
            fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    }
    if (fd < 0)
        return nt_status_of(errno);
    if (creating) {
        action = 2;
        if (fchown(fd, nt_uid, nt_gid)) { /* Windows' user owns what Windows' users make */
        }
    } else if (truncating) {
        action = disposition == 0 ? 0 : 3;
        if (ftruncate(fd, 0)) {
            int err = errno;
            close(fd);
            return nt_status_of(err);
        }
    }
    if (get_meta(fd, NULL, &m) || !(h = new_handle(c, t))) {
        close(fd);
        return 0xc000011f;
    }
    h->fd = fd;
    h->is_dir = m.is_dir;
    h->delete_on_close = (options & 0x1000) != 0;
    snprintf(h->path, sizeof(h->path), "%s", path);
    snprintf(h->name, sizeof(h->name), "%s", name);

    memset(r->out, 0, 88);
    set16(r->out, 89);
    set32(r->out + 4, action);
    put_times(r->out + 8, &m);
    set64(r->out + 40, m.allocated);
    set64(r->out + 48, m.size);
    set32(r->out + 56, m.attributes);
    memcpy(r->out + 64, h->id, 16);
    r->out_len = 88;
    return 0;
}

static uint32_t smb_close(struct request *r)
{
    struct handle *h;
    struct meta m;

    if (r->in_len < 24 || !(h = find_handle(r->c, r->in + 8)))
        return ST_INVALID_HANDLE;
    memset(r->out, 0, 60);
    set16(r->out, 60);
    if ((get16(r->in + 2) & 1) && h->fd >= 0 && !get_meta(h->fd, NULL, &m)) {
        /* what the file is as it is closed, when that was asked for */
        set16(r->out + 2, 1);
        put_times(r->out + 8, &m);
        set64(r->out + 40, m.allocated);
        set64(r->out + 48, m.size);
        set32(r->out + 56, m.attributes);
    }
    free_handle(r->c, h);
    r->out_len = 60;
    return 0;
}

static uint32_t smb_read(struct request *r)
{
    struct handle *h;
    uint32_t length, status = 0;
    ssize_t n;

    if (r->in_len < 48 || !(h = find_handle(r->c, r->in + 16)))
        return ST_INVALID_HANDLE;
    length = get32(r->in + 4);
    if (length > MAX_IO)
        length = MAX_IO;
    if (h->is_pipe) {
        int more;

        if (!(n = pipe_read(h, r->out + 16, length, &more)))
            return ST_PIPE_EMPTY;
        if (more)
            status = ST_BUFFER_OVERFLOW;
    } else {
        if (h->is_dir)
            return ST_INVALID_REQUEST;
        if ((n = pread(h->fd, r->out + 16, length, get64(r->in + 8))) < 0)
            return nt_status_of(errno);
        if (!n && length)
            return ST_END_OF_FILE;
    }
    memset(r->out, 0, 16);
    set16(r->out, 17);
    r->out[2] = 64 + 16;
    set32(r->out + 4, n);
    r->out_len = 16 + n;
    return status;
}

static uint32_t smb_write(struct request *r)
{
    struct handle *h;
    const u8 *data;
    uint32_t length;
    ssize_t n;

    if (r->in_len < 48 || !(h = find_handle(r->c, r->in + 16)))
        return ST_INVALID_HANDLE;
    length = get32(r->in + 4);
    if (!(data = in_buffer(r, get16(r->in + 2), length)))
        return ST_INVALID_PARAMETER;
    if (h->is_pipe) {
        rpc_request(h, data, length);
        n = length;
    } else {
        uint64_t offset = get64(r->in + 8);
        uint32_t done = 0;

        if (h->is_dir)
            return ST_INVALID_REQUEST;
        if (h->tree->allow < RIGHT_CHANGE)
            return ST_ACCESS_DENIED;
        while (done < length) {
            if ((n = pwrite(h->fd, data + done, length - done, offset + done)) <= 0)
                return nt_status_of(n ? errno : ENOSPC);
            done += n;
        }
        n = done;
    }
    memset(r->out, 0, 16);
    set16(r->out, 17);
    set32(r->out + 4, n);
    r->out_len = 16;
    return 0;
}

static int compare_names(const void *a, const void *b)
{
    const char *x = *(char *const *)a, *y = *(char *const *)b;

    for (;;) {
        uint32_t cx = *x ? fold(utf8_next(&x)) : 0, cy = *y ? fold(utf8_next(&y)) : 0;

        if (cx != cy || !cx)
            return cx < cy ? -1 : cx > cy;
    }
}

/* the names of a folder: itself and its parent first, the rest as NTFS orders them */
static void list_folder(struct handle *h)
{
    DIR *dir = opendir(h->path);
    struct dirent *de;
    char **grown;

    forget_listing(h);
    if (!(h->names = malloc(2 * sizeof(char *))))
        return;
    h->names[0] = strdup(".");
    h->names[1] = strdup("..");
    h->name_count = 2;
    while (dir && (de = readdir(dir))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (!(grown = realloc(h->names, (h->name_count + 1) * sizeof(char *))))
            break;
        h->names = grown;
        if (!(h->names[h->name_count] = strdup(de->d_name)))
            break;
        h->name_count++;
    }
    if (dir)
        closedir(dir);
    if (!h->names[0] || !h->names[1]) {
        forget_listing(h);
        return;
    }
    qsort(h->names + 2, h->name_count - 2, sizeof(char *), compare_names);
}

/* how much of an entry of a listing comes before its name, by the kind asked for */
static int entry_head(unsigned kind)
{
    switch (kind) {
    case 1: return 64;      /* FileDirectoryInformation */
    case 2: return 68;      /* FileFullDirectoryInformation */
    case 3: return 94;      /* FileBothDirectoryInformation */
    case 12: return 12;     /* FileNamesInformation */
    case 37: return 104;    /* FileIdBothDirectoryInformation */
    case 38: return 80;     /* FileIdFullDirectoryInformation */
    default: return -1;
    }
}

static unsigned put_entry(u8 *out, unsigned room, unsigned kind, const char *name, const struct meta *m,
                          unsigned index)
{
    u8 wide[1100];
    unsigned head = entry_head(kind), bytes = utf8_to_utf16(name, wide, sizeof(wide));

    if (head + bytes > room)
        return 0;
    memset(out, 0, head);
    set32(out + 4, index);
    if (kind == 12) {
        set32(out + 8, bytes);
    } else {
        put_times(out + 8, m);
        set64(out + 40, m->size);
        set64(out + 48, m->allocated);
        set32(out + 56, m->attributes);
        set32(out + 60, bytes);
        if (kind == 37)
            set64(out + 96, m->id);
        if (kind == 38)
            set64(out + 72, m->id);
    }
    memcpy(out + head, wide, bytes);
    return head + bytes;
}

static uint32_t smb_query_directory(struct request *r)
{
    struct handle *h;
    const u8 *wide;
    char pattern[1024];
    unsigned kind, flags, room, len, pos = 0, end = 0, last = 0, count = 0;

    if (r->in_len < 32 || !(h = find_handle(r->c, r->in + 8)))
        return ST_INVALID_HANDLE;
    if (!h->is_dir)
        return ST_INVALID_PARAMETER;
    kind = r->in[2];
    flags = r->in[3];
    if (entry_head(kind) < 0)
        return ST_INVALID_INFO;
    room = get32(r->in + 28);
    if (room > MAX_IO)
        room = MAX_IO;
    len = get16(r->in + 26);
    if (!(wide = in_buffer(r, get16(r->in + 24), len)) || len / 2 > 255 ||
        !utf16_to_utf8(wide, len / 2, pattern, sizeof(pattern)))
        return ST_NAME_INVALID;

    if (!h->listed || (flags & 0x11)) { /* the first time, or from the start again */
        list_folder(h);
        snprintf(h->pattern, sizeof(h->pattern), "%s", pattern[0] ? pattern : "*");
        h->listed = h->nothing_yet = 1;
    }
    while (h->name_at < h->name_count) {
        const char *name = h->names[h->name_at];
        char path[MAX_PATH_LEN + NAME_MAX + 2];
        struct meta m;
        unsigned n;

        if (!name_matches(h->pattern, name)) {
            h->name_at++;
            continue;
        }
        if (!strcmp(name, ".") || (!strcmp(name, "..") && !h->name[0])) {
            snprintf(path, sizeof(path), "%s", h->path);
        } else if (!strcmp(name, "..")) {
            snprintf(path, sizeof(path), "%s", h->path);
            *strrchr(path, '/') = 0;
        } else {
            snprintf(path, sizeof(path), "%s/%s", h->path, name);
        }
        if (get_meta(-1, path, &m)) { /* gone since the names were taken */
            h->name_at++;
            continue;
        }
        if (!(n = put_entry(r->out + 8 + pos, pos < room ? room - pos : 0, kind, name, &m, h->name_at))) {
            if (!count)
                return ST_LENGTH_MISMATCH;
            break;
        }
        if (count)
            set32(r->out + 8 + last, pos - last);
        last = pos;
        end = pos + n;
        pos = (end + 7) & ~7u;
        memset(r->out + 8 + end, 0, pos - end);
        count++;
        h->name_at++;
        if (flags & 0x02) /* one at a time */
            break;
    }
    if (!count)
        return h->nothing_yet ? ST_NO_SUCH_FILE : ST_NO_MORE_FILES;
    h->nothing_yet = 0;
    set16(r->out, 9);
    set16(r->out + 2, 64 + 8);
    set32(r->out + 4, end);
    r->out_len = 8 + end;
    return 0;
}

/* what a share of this kind is, for the questions about the volume */
static uint32_t volume_info(struct request *r, struct handle *h, unsigned kind, u8 *out, unsigned *len,
                            unsigned *head)
{
    struct statvfs fs;
    uint64_t unit;

    if (statvfs(h->path, &fs))
        return nt_status_of(errno);
    unit = fs.f_frsize ? fs.f_frsize : 4096;
    switch (kind) {
    case 1: { /* FileFsVolumeInformation: the label is the share's name */
        unsigned bytes = utf8_to_utf16(h->tree->share, out + 18, 512);

        memset(out, 0, 18);
        set64(out, started);
        set32(out + 8, 0x41524354);
        set32(out + 12, bytes);
        *head = 18;
        *len = 18 + bytes;
        return 0;
    }
    case 3: /* FileFsSizeInformation */
        set64(out, fs.f_blocks);
        set64(out + 8, fs.f_bavail);
        set32(out + 16, unit / 512);
        set32(out + 20, 512);
        *head = *len = 24;
        return 0;
    case 4: /* FileFsDeviceInformation: a disk, mounted */
        set32(out, 7);
        set32(out + 4, 0x20);
        *head = *len = 8;
        return 0;
    case 5: /* FileFsAttributeInformation: names keep their case, in Unicode */
        set32(out, 0x06);
        set32(out + 4, 255);
        set32(out + 8, 8);
        memcpy(out + 12, "N\0T\0F\0S\0", 8);
        *head = 12;
        *len = 20;
        return 0;
    case 7: /* FileFsFullSizeInformation */
        set64(out, fs.f_blocks);
        set64(out + 8, fs.f_bavail);
        set64(out + 16, fs.f_bfree);
        set32(out + 24, unit / 512);
        set32(out + 28, 512);
        *head = *len = 32;
        return 0;
    case 11: /* FileFsSectorSizeInformation */
        memset(out, 0, 28);
        set32(out, 512);
        set32(out + 4, 4096);
        set32(out + 8, 4096);
        set32(out + 12, 4096);
        set32(out + 16, 3);
        *head = *len = 28;
        return 0;
    }
    (void)r;
    return ST_INVALID_INFO;
}

static uint32_t smb_query_info(struct request *r)
{
    struct handle *h;
    unsigned type, kind, room, len = 0, head = 0, name_bytes;
    u8 *out = r->out + 8, name[2100];
    uint32_t status = 0;
    struct meta m;

    if (r->in_len < 40 || !(h = find_handle(r->c, r->in + 24)))
        return ST_INVALID_HANDLE;
    if (h->is_pipe)
        return ST_INVALID_REQUEST;
    type = r->in[2];
    kind = r->in[3];
    room = get32(r->in + 4);
    if (get_meta(h->fd, NULL, &m))
        return nt_status_of(errno);
    /* the name in the share, "\Folder\file.txt" */
    set16(name, '\\');
    name_bytes = 2 + utf8_to_utf16(h->name, name + 2, sizeof(name) - 2);

    if (type == 2) {
        status = volume_info(r, h, kind, out, &len, &head);
    } else if (type == 3) {
        /* the security: owned by Everyone, who may do what the share lets them */
        static const u8 everyone[12] = {1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0};

        memset(out, 0, 72);
        out[0] = 1;
        set16(out + 2, 0x8004);     /* self-relative, with a list of rights */
        set32(out + 4, 20);
        set32(out + 8, 32);
        set32(out + 16, 44);
        memcpy(out + 20, everyone, 12);
        memcpy(out + 32, everyone, 12);
        out[44] = 2;                /* the list: one entry */
        set16(out + 46, 28);
        set16(out + 48, 1);
        out[53] = m.is_dir ? 3 : 0; /* for what is inside as well */
        set16(out + 54, 20);
        set32(out + 56, h->tree->allow >= RIGHT_CHANGE ? 0x001f01ff : 0x001200a9);
        memcpy(out + 60, everyone, 12);
        head = len = 72;
        if (room < len) {
            r->needed = len;
            return ST_BUFFER_TOO_SMALL;
        }
    } else if (type != 1) {
        return ST_NOT_SUPPORTED;
    } else {
        switch (kind) {
        case 4: /* FileBasicInformation */
            memset(out, 0, 40);
            put_times(out, &m);
            set32(out + 32, m.attributes);
            head = len = 40;
            break;
        case 5: /* FileStandardInformation */
            memset(out, 0, 24);
            set64(out, m.allocated);
            set64(out + 8, m.size);
            set32(out + 16, 1);
            out[20] = h->delete_on_close;
            out[21] = m.is_dir;
            head = len = 24;
            break;
        case 6: /* FileInternalInformation */
            set64(out, m.id);
            head = len = 8;
            break;
        case 7: /* FileEaInformation: none */
            set32(out, 0);
            head = len = 4;
            break;
        case 15: /* FileFullEaInformation */
            return 0xc0000052; /* no extended attributes on the file */
        case 8: /* FileAccessInformation */
            set32(out, h->tree->allow >= RIGHT_CHANGE ? 0x001f01ff : 0x001200a9);
            head = len = 4;
            break;
        case 9:  /* FileNameInformation */
        case 48: /* FileNormalizedNameInformation */
            set32(out, name_bytes);
            memcpy(out + 4, name, name_bytes);
            head = 4;
            len = 4 + name_bytes;
            break;
        case 14: /* FilePositionInformation */
            set64(out, 0);
            head = len = 8;
            break;
        case 16: /* FileModeInformation */
        case 17: /* FileAlignmentInformation */
            set32(out, 0);
            head = len = 4;
            break;
        case 18: /* FileAllInformation: all of the above, one after another */
            memset(out, 0, 100);
            put_times(out, &m);
            set32(out + 32, m.attributes);
            set64(out + 40, m.allocated);
            set64(out + 48, m.size);
            set32(out + 56, 1);
            out[60] = h->delete_on_close;
            out[61] = m.is_dir;
            set64(out + 64, m.id);
            set32(out + 76, h->tree->allow >= RIGHT_CHANGE ? 0x001f01ff : 0x001200a9);
            set32(out + 96, name_bytes);
            memcpy(out + 100, name, name_bytes);
            head = 100;
            len = 100 + name_bytes;
            break;
        case 21: /* FileAlternateNameInformation: there are no short names */
            return ST_NAME_NOT_FOUND;
        case 22: /* FileStreamInformation: the data of a file is its one stream */
            if (!m.is_dir) {
                memset(out, 0, 24);
                set32(out + 4, 14);
                set64(out + 8, m.size);
                set64(out + 16, m.allocated);
                memcpy(out + 24, ":\0:\0$\0D\0A\0T\0A\0", 14);
                head = 24;
                len = 38;
            }
            break;
        case 28: /* FileCompressionInformation: not compressed */
            memset(out, 0, 16);
            set64(out, m.size);
            head = len = 16;
            break;
        case 34: /* FileNetworkOpenInformation */
            memset(out, 0, 56);
            put_times(out, &m);
            set64(out + 32, m.allocated);
            set64(out + 40, m.size);
            set32(out + 48, m.attributes);
            head = len = 56;
            break;
        case 35: /* FileAttributeTagInformation */
            set32(out, m.attributes);
            set32(out + 4, 0);
            head = len = 8;
            break;
        default:
            return ST_INVALID_INFO;
        }
    }
    if (status)
        return status;
    if (room < head)
        return ST_LENGTH_MISMATCH;
    if (room < len) { /* the name does not fit whole */
        len = room;
        status = ST_BUFFER_OVERFLOW;
    }
    set16(r->out, 9);
    set16(r->out + 2, 64 + 8);
    set32(r->out + 4, len);
    r->out_len = 8 + len;
    return status;
}

/* a time Windows sets: 0 and -1 leave what there is */
static void time_to_set(uint64_t value, struct timespec *ts)
{
    if (!value || value >= 0xfffffffffffffffeULL) {
        ts->tv_sec = 0;
        ts->tv_nsec = UTIME_OMIT;
    } else {
        ts->tv_sec = (int64_t)(value / 10000000) - 11644473600LL;
        ts->tv_nsec = value % 10000000 * 100;
    }
}

static uint32_t smb_set_info(struct request *r)
{
    struct handle *h;
    const u8 *in;
    unsigned type, kind, len;

    if (r->in_len < 32 || !(h = find_handle(r->c, r->in + 16)))
        return ST_INVALID_HANDLE;
    type = r->in[2];
    kind = r->in[3];
    len = get32(r->in + 4);
    if (!(in = in_buffer(r, get16(r->in + 8), len)))
        return ST_INVALID_PARAMETER;
    if (h->is_pipe)
        return ST_INVALID_REQUEST;
    if (type != 1)
        return ST_NOT_SUPPORTED; /* the volume's label, the rights: none of them are kept */
    if (h->tree->allow < RIGHT_CHANGE)
        return ST_ACCESS_DENIED;

    switch (kind) {
    case 4: { /* FileBasicInformation: the times, hidden, read-only */
        struct timespec times[2];
        uint32_t attributes;

        if (len < 36)
            return ST_LENGTH_MISMATCH;
        time_to_set(get64(in + 8), &times[0]);
        time_to_set(get64(in + 16), &times[1]);
        if (futimens(h->fd, times) && errno != EPERM)
            return nt_status_of(errno);
        if ((attributes = get32(in + 32))) {
            u8 ntfs[4];

            set32(ntfs, (attributes & 0x27) | (h->is_dir ? 0x10 : 0));
            if (fsetxattr(h->fd, "system.ntfs_attrib", ntfs, 4, 0)) { /* a volume without them */
            }
        }
        break;
    }
    case 10: { /* FileRenameInformation: the new name counts from the share's top */
        char name[1024], path[MAX_PATH_LEN], *end;
        unsigned name_len;
        uint32_t status;
        struct stat st;

        if (len < 20)
            return ST_LENGTH_MISMATCH;
        name_len = get32(in + 16);
        if (name_len > len - 20 || name_len / 2 > 500 || !utf16_to_utf8(in + 20, name_len / 2, name, sizeof(name)))
            return ST_NAME_INVALID;
        while (name[0] == '\\')
            memmove(name, name + 1, strlen(name));
        for (end = name + strlen(name); end > name && end[-1] == '\\'; end--)
            end[-1] = 0;
        if (!name[0] || !h->name[0])
            return ST_ACCESS_DENIED; /* the share itself has no other name */
        if ((status = join_path(h->tree->path, name, path, sizeof(path))))
            return status;
        if (!lstat(path, &st) && strcasecmp(path, h->path)) {
            if (!in[0])
                return ST_NAME_COLLISION;
            if (S_ISDIR(st.st_mode))
                return ST_ACCESS_DENIED;
        }
        if (rename(h->path, path))
            return errno == ENOENT ? 0xc000003a : nt_status_of(errno);
        snprintf(h->path, sizeof(h->path), "%s", path);
        snprintf(h->name, sizeof(h->name), "%s", name);
        break;
    }
    case 13: /* FileDispositionInformation: to be deleted when closed, or not after all */
    case 64: /* FileDispositionInformationEx */
        if (len < 1)
            return ST_LENGTH_MISMATCH;
        if (!(in[0] & 1)) {
            h->delete_on_close = 0;
            break;
        }
        if (!h->name[0])
            return ST_ACCESS_DENIED;
        if (h->is_dir) {
            DIR *dir = opendir(h->path);
            struct dirent *de;
            int entries = 0;

            while (dir && (de = readdir(dir)))
                if (strcmp(de->d_name, ".") && strcmp(de->d_name, ".."))
                    entries++;
            if (dir)
                closedir(dir);
            if (entries)
                return ST_DIRECTORY_NOT_EMPTY;
        }
        h->delete_on_close = 1;
        break;
    case 19: /* FileAllocationInformation: room is given as the file is written */
        break;
    case 20: /* FileEndOfFileInformation */
        if (len < 8)
            return ST_LENGTH_MISMATCH;
        if (h->is_dir)
            return ST_INVALID_REQUEST;
        if (ftruncate(h->fd, get64(in)))
            return nt_status_of(errno == EINVAL || errno == EBADF ? EACCES : errno);
        break;
    case 14: /* FilePositionInformation */
    case 16: /* FileModeInformation */
        break;
    default:
        return ST_INVALID_INFO;
    }
    set16(r->out, 2);
    r->out_len = 2;
    return 0;
}

static uint32_t smb_ioctl(struct request *r)
{
    struct conn *c = r->c;
    struct handle *h;
    const u8 *in;
    uint32_t code, status = 0;
    unsigned in_len, room, n = 0;
    u8 *out = r->out + 48;

    if (r->in_len < 56)
        return ST_INVALID_PARAMETER;
    code = get32(r->in + 4);
    in_len = get32(r->in + 28);
    room = get32(r->in + 44);
    if (room > MAX_IO)
        room = MAX_IO;
    if (!(in = in_buffer(r, get32(r->in + 24), in_len)))
        return ST_INVALID_PARAMETER;
    switch (code) {
    case 0x0011c017: { /* a message to the pipe and what it answers, at once */
        int more;

        if (!(h = find_handle(c, r->in + 8)) || !h->is_pipe)
            return ST_INVALID_HANDLE;
        rpc_request(h, in, in_len);
        if (!(n = pipe_read(h, out, room, &more)))
            return ST_PIPE_EMPTY;
        if (more)
            status = ST_BUFFER_OVERFLOW;
        break;
    }
    case 0x00110018: /* waiting for the pipe: it is there */
        break;
    case 0x00140204: /* is the dialect agreed on the one that was offered? */
        if (room < 24)
            return ST_BUFFER_TOO_SMALL;
        memset(out, 0, 24);
        memcpy(out + 4, server_guid, 16);
        set16(out + 20, 1);
        set16(out + 22, c->dialect);
        n = 24;
        break;
    case 0x00060194: /* where the folder is in a distributed file system: there is none */
        return ST_NOT_FOUND;
    case 0x001401fc: /* the network cards, for several channels */
        return ST_NOT_SUPPORTED;
    default:
        return ST_INVALID_REQUEST;
    }
    memset(r->out, 0, 48);
    set16(r->out, 49);
    set32(r->out + 4, code);
    memcpy(r->out + 8, r->in + 8, 16);
    set32(r->out + 24, 64 + 48);
    set32(r->out + 32, 64 + 48);
    set32(r->out + 36, n);
    r->out_len = 48 + n;
    return status;
}

/**********************************************************************
 *          Requests in, answers out
 */

static void put_header(u8 *out, const u8 *request, uint32_t status, unsigned command, uint32_t flags,
                       uint32_t tree_id, uint64_t session_id)
{
    unsigned asked = request ? get16(request + 14) : 1;

    memset(out, 0, 64);
    memcpy(out, "\xfeSMB", 4);
    set16(out + 4, 64);
    set32(out + 8, status);
    set16(out + 12, command);
    set16(out + 14, asked > 64 ? 64 : asked ? asked : 1); /* the requests it may send next */
    set32(out + 16, flags | 1);
    if (request) {
        memcpy(out + 6, request + 6, 2);
        memcpy(out + 24, request + 24, 12);
    }
    set32(out + 36, tree_id);
    set64(out + 40, session_id);
}

static int send_all(int sock, const u8 *data, unsigned len)
{
    while (len) {
        ssize_t n = send(sock, data, len, MSG_NOSIGNAL);

        if (n <= 0)
            return 0;
        data += n;
        len -= n;
    }
    return 1;
}

/* the answer a change notification waited for: "look at the folder again",
 * or that it will not come */
static void end_notify(struct conn *c, struct notify *n, uint32_t status)
{
    u8 frame[4 + 64 + 9] = {0, 0, 0, 64 + 9};
    int shared = 0;

    put_header(frame + 4, n->header, status, 15, 2, 0, get64(n->header + 40));
    set16(frame + 4 + 14, 0);
    set64(frame + 4 + 32, n->id);
    set16(frame + 4 + 64, 9);
    send_all(c->sock, frame, sizeof(frame));
    n->handle = NULL;
    for (int i = 0; i < MAX_NOTIFIES; i++)
        if (c->notifies[i].handle && c->notifies[i].watch == n->watch)
            shared = 1;
    if (!shared)
        inotify_rm_watch(c->inotify, n->watch);
}

/* something changed in the folders that are watched */
static void folders_changed(struct conn *c)
{
    char events[4096];
    ssize_t len = read(c->inotify, events, sizeof(events));

    for (ssize_t pos = 0; pos + (ssize_t)sizeof(struct inotify_event) <= len;) {
        struct inotify_event *event = (struct inotify_event *)(events + pos);

        for (int i = 0; i < MAX_NOTIFIES; i++)
            if (c->notifies[i].handle && c->notifies[i].watch == event->wd)
                end_notify(c, &c->notifies[i], ST_NOTIFY_ENUM_DIR);
        pos += sizeof(*event) + event->len;
    }
}

/* "tell me when this folder changes": told once, without saying what; Explorer
 * reads the folder again and asks anew */
static uint32_t smb_change_notify(struct request *r)
{
    struct conn *c = r->c;
    struct notify *n = NULL;
    struct handle *h;

    if (r->in_len < 32 || !(h = find_handle(c, r->in + 8)))
        return ST_INVALID_HANDLE;
    if (!h->is_dir)
        return ST_INVALID_PARAMETER;
    for (int i = 0; i < MAX_NOTIFIES && !n; i++)
        if (!c->notifies[i].handle)
            n = &c->notifies[i];
    if (c->inotify < 0)
        c->inotify = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (!n || c->inotify < 0 ||
        (n->watch = inotify_add_watch(c->inotify, h->path, IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
                                                           IN_CLOSE_WRITE | IN_ATTRIB)) < 0)
        return ST_NOT_SUPPORTED;
    n->handle = h;
    n->id = ++c->notify_count;
    memcpy(n->header, r->header, 64);
    r->async_id = n->id;
    return ST_PENDING;
}

/* the one who asked does not want the answer any more */
static void cancel_notify(struct conn *c, const u8 *header)
{
    for (int i = 0; i < MAX_NOTIFIES; i++) {
        struct notify *n = &c->notifies[i];

        if (n->handle && (get32(header + 16) & 2 ? get64(header + 32) == n->id
                                                 : !memcmp(header + 24, n->header + 24, 8)))
            end_notify(c, n, ST_CANCELLED);
    }
}

static uint32_t dispatch(struct request *r)
{
    struct conn *c = r->c;
    struct handle *h;
    struct tree *t;

    switch (r->command) {
    case 0:
        return smb_negotiate(r);
    case 1:
        return smb_session_setup(r);
    case 13: /* echo: is anybody there? */
        set16(r->out, 4);
        r->out_len = 4;
        return 0;
    }
    if (!c->logged_on || r->session_id != c->session_id)
        return ST_SESSION_DELETED;
    switch (r->command) {
    case 2: /* logoff */
        for (int i = 0; i < MAX_TREES; i++)
            if (c->trees[i].used)
                free_tree(c, &c->trees[i]);
        c->logged_on = 0;
        set16(r->out, 4);
        r->out_len = 4;
        return 0;
    case 3:
        return smb_tree_connect(r);
    }
    if (!(t = find_tree(c, r->tree_id)))
        return ST_NETNAME_DELETED;
    switch (r->command) {
    case 4: /* tree disconnect */
        free_tree(c, t);
        set16(r->out, 4);
        r->out_len = 4;
        return 0;
    case 5:
        return smb_create(r, t);
    case 6:
        return smb_close(r);
    case 7: /* flush */
        if (r->in_len < 24 || !(h = find_handle(c, r->in + 8)))
            return ST_INVALID_HANDLE;
        if (h->fd >= 0 && !h->is_dir && fsync(h->fd) && errno != EINVAL && errno != EBADF)
            return nt_status_of(errno);
        set16(r->out, 4);
        r->out_len = 4;
        return 0;
    case 8:
        return smb_read(r);
    case 9:
        return smb_write(r);
    case 10: /* lock: granted, and nobody is kept out by it (the comment at the top) */
        if (r->in_len < 24 || !find_handle(c, r->in + 8))
            return ST_INVALID_HANDLE;
        set16(r->out, 4);
        r->out_len = 4;
        return 0;
    case 11:
        return smb_ioctl(r);
    case 14:
        return smb_query_directory(r);
    case 16:
        return smb_query_info(r);
    case 17:
        return smb_set_info(r);
    case 15:
        return smb_change_notify(r);
    default: /* oplock breaks: no oplock is given */
        return ST_NOT_SUPPORTED;
    }
}

/* where a command names its file, for "the file the command before this one opened" */
static int file_id_at(unsigned command)
{
    switch (command) {
    case 6: case 7: case 10: case 11: case 14: return 8;
    case 8: case 9: case 17: return 16;
    case 16: return 24;
    default: return -1;
    }
}

/* the answer to what one frame brought: one request, or several in a row */
static unsigned answer_frame(struct conn *c, u8 *in, unsigned in_len, u8 *out, u8 *body)
{
    static const u8 unnamed[16] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                   0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    unsigned pos = 0, out_pos = 0;
    uint32_t failed = 0, last_tree = 0;
    uint64_t last_session = 0;
    u8 last_file[16];
    int have_file = 0;

    if (in_len >= 35 && !memcmp(in, "\xffSMB", 4) && in[4] == 0x72) {
        /* the negotiation of SMB 1, as Windows starts when it does not know the
         * server: of the dialects it names, SMB 2 is the one spoken here */
        struct request r = {.c = c, .out = body};
        unsigned dialect = memmem(in, in_len, "SMB 2.???", 9) ? 0x02ff : memmem(in, in_len, "SMB 2.002", 9) ? 0x0202 : 0;

        if (!dialect)
            return 0;
        if (dialect == 0x0202)
            c->dialect = dialect;
        negotiate_answer(&r, dialect);
        put_header(out, NULL, 0, 0, 0, 0, 0);
        memcpy(out + 64, body, r.out_len);
        return 64 + r.out_len;
    }

    while (pos + 64 <= in_len) {
        u8 *hdr = in + pos;
        uint32_t next = get32(hdr + 20), flags = get32(hdr + 16), status;
        unsigned total = next ? next : in_len - pos, len;
        struct request r = {.c = c, .command = get16(hdr + 12), .header = hdr, .in = hdr + 64, .out = body,
                            .session_id = get64(hdr + 40), .tree_id = get32(hdr + 36)};
        int related = (flags & 4) && pos, at;

        if (memcmp(hdr, "\xfeSMB", 4) || total < 64 || total > in_len - pos)
            break;
        r.in_len = total - 64;
        if (r.command == 12) { /* cancel: answered by what it cancels */
            cancel_notify(c, hdr);
            if (!next)
                break;
            pos += total;
            continue;
        }
        if (!related || r.command == 5)
            failed = 0;
        if (related) {
            r.session_id = last_session;
            r.tree_id = last_tree;
            if ((at = file_id_at(r.command)) >= 0 && (unsigned)at + 16 <= r.in_len && have_file &&
                !memcmp(hdr + 64 + at, unnamed, 16))
                memcpy(hdr + 64 + at, last_file, 16);
        }
        status = related && failed ? failed : dispatch(&r);
        if (status && status != ST_MORE_PROCESSING && status != ST_BUFFER_OVERFLOW) {
            /* an error; or "pending": the answer comes later, under another name */
            memset(body, 0, 12);
            set16(body, 9);
            r.out_len = 9;
            if (r.needed) {
                set32(body + 4, 4);
                set32(body + 8, r.needed);
                r.out_len = 12;
            }
            if (r.command == 5)
                failed = status; /* no file: what follows about it fails the same way */
        }
        len = 64 + r.out_len;
        if (out_pos + len + 8 > MAX_FRAME)
            break;
        put_header(out + out_pos, hdr, status, r.command, (related ? 4 : 0) | (r.async_id ? 2 : 0), r.tree_id,
                   r.session_id);
        if (r.async_id)
            set64(out + out_pos + 32, r.async_id);
        memcpy(out + out_pos + 64, body, r.out_len);
        last_session = r.session_id;
        last_tree = r.tree_id;
        if (r.command == 5 && !status) {
            memcpy(last_file, body + 64, 16);
            have_file = 1;
        }
        if (!next) {
            out_pos += len;
            break;
        }
        while (len & 7)
            out[out_pos + len++] = 0;
        set32(out + out_pos + 20, len);
        out_pos += len;
        pos += total;
    }
    return out_pos;
}

/* 1 when all of it came; 0 when the other side left, or file sharing was turned
 * off. Meanwhile the folders that are watched are heard too */
static int receive(struct conn *c, u8 *data, unsigned len)
{
    while (len) {
        struct pollfd pfd[2] = {{c->sock, POLLIN, 0}, {c->inotify, POLLIN, 0}};
        ssize_t n;

        if (!sharing || stopping)
            return 0;
        if (poll(pfd, 2, 2000) <= 0)
            continue;
        if (pfd[1].revents & POLLIN)
            folders_changed(c);
        if (!(pfd[0].revents & (POLLIN | POLLHUP | POLLERR)))
            continue;
        if ((n = recv(c->sock, data, len, 0)) <= 0)
            return 0;
        data += n;
        len -= n;
    }
    return 1;
}

static void *serve(void *arg)
{
    struct conn *c = arg;
    u8 *in = malloc(MAX_FRAME), *out = malloc(MAX_FRAME + 4), *body = malloc(BODY_SIZE);

    while (in && out && body) {
        u8 prefix[4];
        unsigned len;

        if (!receive(c, prefix, 4))
            break;
        len = prefix[1] << 16 | prefix[2] << 8 | prefix[3];
        if (prefix[0] == 0x85) /* a keep-alive of NetBIOS */
            continue;
        if (prefix[0] || len < 4 || len > MAX_FRAME || !receive(c, in, len))
            break;
        if (!(len = answer_frame(c, in, len, out + 4, body)))
            continue;
        out[0] = 0;
        out[1] = len >> 16;
        out[2] = len >> 8;
        out[3] = len;
        if (!send_all(c->sock, out, len + 4))
            break;
    }
    for (int i = 0; i < MAX_TREES; i++)
        if (c->trees[i].used)
            free_tree(c, &c->trees[i]);
    for (int i = 0; i < MAX_HANDLES; i++)
        if (c->handles[i])
            free_handle(c, c->handles[i]);
    if (c->inotify >= 0)
        close(c->inotify);
    close(c->sock);
    free(in);
    free(out);
    free(body);
    free(c);
    return NULL;
}

static int listen_tcp(int port)
{
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port)};
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0), on = 1;

    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) || listen(fd, 32)) {
        close(fd);
        return -1;
    }
    return fd;
}

/* the port is open while file sharing is on, and closed while it is off */
static void *listen_smb(void *arg)
{
    int listener = -1, complained = 0;

    (void)arg;
    for (;;) {
        struct pollfd pfd = {listener, POLLIN, 0};
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        struct conn *c;
        pthread_t thread;
        int on = 1, sock;

        if (!sharing) {
            if (listener >= 0)
                close(listener);
            listener = -1;
            sleep(1);
            continue;
        }
        if (listener < 0 && (listener = listen_tcp(SMB_PORT)) < 0) {
            if (!complained++)
                say("port %d: %s", SMB_PORT, strerror(errno));
            sleep(5);
            continue;
        }
        complained = 0;
        if (poll(&pfd, 1, 1000) <= 0)
            continue;
        if ((sock = accept4(listener, (struct sockaddr *)&from, &from_len, SOCK_CLOEXEC)) < 0)
            continue;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
        setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
        if (!(c = calloc(1, sizeof(*c)))) {
            close(sock);
            continue;
        }
        c->sock = sock;
        c->inotify = -1;
        inet_ntop(AF_INET, &from.sin_addr, c->peer, sizeof(c->peer));
        if (pthread_create(&thread, NULL, serve, c)) {
            close(sock);
            free(c);
        } else {
            pthread_detach(thread);
        }
    }
    return NULL;
}

/**********************************************************************
 *          Being found: the answers to those who look for computers
 *          (ntlanman's discover.c and arctic-smb.c are the ones who ask)
 */

struct nic {
    struct in_addr address, mask;
};

static int list_nics(struct nic *nics, int max)
{
    struct ifaddrs *list = NULL, *ifa;
    int count = 0;

    if (getifaddrs(&list))
        return 0;
    for (ifa = list; ifa && count < max; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET || (ifa->ifa_flags & IFF_LOOPBACK) ||
            !(ifa->ifa_flags & IFF_UP) || !ifa->ifa_netmask)
            continue;
        nics[count].address = ((struct sockaddr_in *)ifa->ifa_addr)->sin_addr;
        nics[count].mask = ((struct sockaddr_in *)ifa->ifa_netmask)->sin_addr;
        count++;
    }
    freeifaddrs(list);
    return count;
}

/* the address of this computer that the one who asked can reach */
static int address_for(const struct sockaddr_in *peer, struct in_addr *address)
{
    struct nic nics[16];
    int count = list_nics(nics, 16);

    for (int i = 0; i < count; i++) {
        if ((nics[i].address.s_addr & nics[i].mask.s_addr) == (peer->sin_addr.s_addr & nics[i].mask.s_addr)) {
            *address = nics[i].address;
            return 1;
        }
    }
    if (count)
        *address = nics[0].address;
    return count > 0;
}

/* a socket that hears a port, and a multicast group on every network */
static int udp_socket(int port)
{
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port)};
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0), on = 1;

    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr))) {
        say("UDP port %d: %s", port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static void join_group(int fd, const char *group)
{
    struct nic nics[16];
    int count = list_nics(nics, 16);
    struct ip_mreq mreq;

    inet_pton(AF_INET, group, &mreq.imr_multiaddr);
    for (int i = 0; i < count; i++) {
        mreq.imr_interface = nics[i].address;
        setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)); /* again is no harm */
    }
}

/* what came, or nothing after a while: networks come and go, the group is joined on the new ones */
static ssize_t hear(int fd, const char *group, void *buf, size_t size, struct sockaddr_in *from)
{
    struct pollfd pfd = {fd, POLLIN, 0};
    socklen_t from_len = sizeof(*from);

    if (poll(&pfd, 1, 20000) <= 0) {
        if (group)
            join_group(fd, group);
        return 0;
    }
    return recvfrom(fd, buf, size, 0, (struct sockaddr *)from, &from_len);
}

/* NetBIOS: "who is ARCTIC?" and "what are your names?" (UDP 137) */
static void *answer_netbios(void *arg)
{
    int fd = udp_socket(137);

    (void)arg;
    while (fd >= 0) {
        u8 buf[512], reply[200], name[16];
        struct sockaddr_in from;
        struct in_addr address;
        char mine[17];
        ssize_t n = hear(fd, NULL, buf, sizeof(buf), &from);
        unsigned type, len;
        int star, ours;

        if (n < 50 || !discovery || (buf[2] & 0xf8) || buf[5] != 1 || buf[12] != 32)
            continue;
        for (int i = 0; i < 16; i++)
            name[i] = (buf[13 + i * 2] - 'A') << 4 | ((buf[14 + i * 2] - 'A') & 15);
        snprintf(mine, sizeof(mine), "%-15.15s", computer);
        star = name[0] == '*' && !name[1];
        ours = !memcmp(name, mine, 15) && (name[15] == 0x20 || name[15] == 0);
        type = buf[46] << 8 | buf[47];
        if (!address_for(&from, &address))
            continue;
        memset(reply, 0, sizeof(reply));
        memcpy(reply, buf, 2);
        reply[7] = 1;
        memcpy(reply + 12, buf + 12, 34);
        reply[47] = type;
        reply[49] = 1;
        if (type == 0x20 && ours) {
            reply[2] = 0x85;
            reply[51] = 0x04; reply[52] = 0x93; reply[53] = 0xe0; /* for 300000 s */
            reply[55] = 6;
            memcpy(reply + 58, &address, 4);
            len = 62;
        } else if (type == 0x21 && (star || ours)) {
            /* the names: the computer, its workgroup, its file server */
            static const struct { const char *name; u8 suffix, flags; } names[] = {
                {NULL, 0x00, 0x04}, {WORKGROUP, 0x00, 0x84}, {NULL, 0x20, 0x04}};

            reply[2] = 0x84;
            reply[55] = 1 + 3 * 18 + 46;
            reply[56] = 3;
            for (int i = 0; i < 3; i++) {
                u8 *entry = reply + 57 + i * 18;
                char padded[17];

                snprintf(padded, sizeof(padded), "%-15.15s", names[i].name ? names[i].name : computer);
                memcpy(entry, padded, 15);
                entry[15] = names[i].suffix;
                entry[16] = names[i].flags;
            }
            len = 57 + 3 * 18 + 46;
        } else {
            continue;
        }
        sendto(fd, reply, len, 0, (struct sockaddr *)&from, sizeof(from));
    }
    return NULL;
}

/* a name of a DNS message, its shortcuts followed; where the message goes on, or -1 */
static int dns_name(const u8 *msg, int len, int pos, char *out, int size)
{
    int end = -1, jumps = 0, used = 0;

    for (;;) {
        int label;

        if (pos >= len)
            return -1;
        label = msg[pos];
        if (!label) {
            if (end < 0)
                end = pos + 1;
            break;
        }
        if ((label & 0xc0) == 0xc0) {
            if (pos + 1 >= len || ++jumps > 16)
                return -1;
            if (end < 0)
                end = pos + 2;
            pos = (label & 0x3f) << 8 | msg[pos + 1];
            continue;
        }
        if (pos + 1 + label > len || used + label + 2 > size)
            return -1;
        if (used)
            out[used++] = '.';
        memcpy(out + used, msg + pos + 1, label);
        used += label;
        pos += 1 + label;
    }
    out[used] = 0;
    return end;
}

static int put_dns_name(u8 *out, const char *name)
{
    int pos = 0;

    while (*name) {
        int len = strcspn(name, ".");

        out[pos++] = len;
        memcpy(out + pos, name, len);
        pos += len;
        name += len;
        if (*name)
            name++;
    }
    out[pos++] = 0;
    return pos;
}

/* a record: its name, kind, class and life, then room for its data, whose length is put by the caller */
static int put_record(u8 *out, const char *name, unsigned type, unsigned class, unsigned ttl)
{
    int pos = put_dns_name(out, name);

    out[pos++] = type >> 8;
    out[pos++] = type;
    out[pos++] = class >> 8;
    out[pos++] = class;
    out[pos++] = ttl >> 24;
    out[pos++] = ttl >> 16;
    out[pos++] = ttl >> 8;
    out[pos++] = ttl;
    return pos + 2;
}

/* LLMNR: how Windows finds a neighbour by its name alone (UDP 5355) */
static void *answer_llmnr(void *arg)
{
    static const char group[] = "224.0.0.252";
    int fd = udp_socket(5355);

    (void)arg;
    if (fd >= 0)
        join_group(fd, group);
    while (fd >= 0) {
        u8 buf[512], reply[600];
        struct sockaddr_in from;
        struct in_addr address;
        char name[256];
        ssize_t n = hear(fd, group, buf, sizeof(buf), &from);
        int end, type, len;

        if (n < 17 || !discovery || (buf[2] & 0xf8) || buf[4] || buf[5] != 1)
            continue;
        if ((end = dns_name(buf, n, 12, name, sizeof(name))) < 0 || end + 4 > n || strcasecmp(name, computer))
            continue;
        type = buf[end] << 8 | buf[end + 1];
        if (!address_for(&from, &address))
            continue;
        len = end + 4;
        memcpy(reply, buf, len);
        memset(reply + 2, 0, 10);
        reply[2] = 0x80;
        reply[5] = 1;
        if (type == 1 || type == 255) {
            reply[7] = 1;
            reply[len++] = 0xc0; reply[len++] = 12;     /* the name of the question */
            reply[len++] = 0; reply[len++] = 1;
            reply[len++] = 0; reply[len++] = 1;
            reply[len++] = 0; reply[len++] = 0; reply[len++] = 0; reply[len++] = 30;
            reply[len++] = 0; reply[len++] = 4;
            memcpy(reply + len, &address, 4);
            len += 4;
        } /* an IPv6 address is asked for too: there is none, said at once */
        sendto(fd, reply, len, 0, (struct sockaddr *)&from, sizeof(from));
    }
    return NULL;
}

/* mDNS: the name, and the file server as a service (UDP 5353) */
static void *answer_mdns(void *arg)
{
    static const char group[] = "224.0.0.251", service[] = "_smb._tcp.local";
    int fd = udp_socket(5353);

    (void)arg;
    if (fd >= 0)
        join_group(fd, group);
    while (fd >= 0) {
        u8 buf[1500], reply[1500];
        struct sockaddr_in from, to = {.sin_family = AF_INET, .sin_port = htons(5353)};
        struct nic nics[16];
        char name[256], host[80], instance[100];
        ssize_t n = hear(fd, group, buf, sizeof(buf), &from);
        int pos = 12, questions, want_address = 0, want_service = 0, want_types = 0, alone = 0, count, len, at;
        int legacy;

        if (n < 12 || !discovery || (buf[2] & 0xf8))
            continue;
        snprintf(host, sizeof(host), "%s.local", computer);
        snprintf(instance, sizeof(instance), "%s.%s", computer, service);
        questions = buf[4] << 8 | buf[5];
        while (questions-- > 0 && (pos = dns_name(buf, n, pos, name, sizeof(name))) > 0 && pos + 4 <= n) {
            int type = buf[pos] << 8 | buf[pos + 1];

            if (buf[pos + 2] & 0x80)
                alone = 1; /* the answer is wanted by the one who asks, not by everybody */
            if (!strcasecmp(name, host) && (type == 1 || type == 255))
                want_address = 1;
            else if (sharing && (!strcasecmp(name, service) || !strcasecmp(name, instance)))
                want_service = 1;
            else if (sharing && !strcasecmp(name, "_services._dns-sd._udp.local") && type == 12)
                want_types = 1;
            pos += 4;
        }
        if (pos < 0 || (!want_address && !want_service && !want_types))
            continue;
        legacy = ntohs(from.sin_port) != 5353;
        inet_pton(AF_INET, group, &to.sin_addr);
        count = legacy || alone ? 1 : list_nics(nics, 16);
        for (int i = 0; i < count; i++) {
            struct in_addr address;
            unsigned ttl = legacy ? 10 : 120, own = legacy ? 1 : 0x8001;
            int records = 0;

            if (legacy || alone) {
                if (!address_for(&from, &address))
                    break;
            } else {
                address = nics[i].address;
            }
            memset(reply, 0, 12);
            reply[2] = 0x84;
            len = 12;
            if (legacy) { /* an ordinary resolver: its number and its question come back */
                memcpy(reply, buf, 2);
                memcpy(reply + 4, buf + 4, 2);
                memcpy(reply + 12, buf + 12, pos - 12);
                len = pos;
            }
            if (want_types) {
                at = len + put_record(reply + len, "_services._dns-sd._udp.local", 12, 1, ttl);
                len = at + put_dns_name(reply + at, service);
                reply[at - 2] = (len - at) >> 8; reply[at - 1] = len - at;
                records++;
            }
            if (want_service) {
                at = len + put_record(reply + len, service, 12, 1, ttl);
                len = at + put_dns_name(reply + at, instance);
                reply[at - 2] = (len - at) >> 8; reply[at - 1] = len - at;
                at = len + put_record(reply + len, instance, 33, own, ttl);
                memset(reply + at, 0, 4);
                reply[at + 4] = SMB_PORT >> 8; reply[at + 5] = SMB_PORT & 255;
                len = at + 6 + put_dns_name(reply + at + 6, host);
                reply[at - 2] = (len - at) >> 8; reply[at - 1] = len - at;
                at = len + put_record(reply + len, instance, 16, own, ttl);
                reply[at] = 0;
                len = at + 1;
                reply[at - 2] = 0; reply[at - 1] = 1;
                records += 3;
            }
            if (want_address || want_service) {
                at = len + put_record(reply + len, host, 1, own, ttl);
                memcpy(reply + at, &address, 4);
                len = at + 4;
                reply[at - 2] = 0; reply[at - 1] = 4;
                records++;
            }
            reply[7] = records;
            if (legacy || alone) {
                sendto(fd, reply, len, 0, (struct sockaddr *)&from, sizeof(from));
            } else {
                setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &address, sizeof(address));
                sendto(fd, reply, len, 0, (struct sockaddr *)&to, sizeof(to));
            }
        }
    }
    return NULL;
}

/**********************************************************************
 *          WS-Discovery: the device a computer is in the Network folder of
 *          Windows; found by a probe, described over HTTP
 */

#define WSD_GROUP "239.255.255.250"
#define WSD_ENVELOPE \
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>" \
    "<soap:Envelope xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\"" \
    " xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\"" \
    " xmlns:wsd=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\"" \
    " xmlns:wsx=\"http://schemas.xmlsoap.org/ws/2004/09/mex\"" \
    " xmlns:wsdp=\"http://schemas.xmlsoap.org/ws/2006/02/devprof\"" \
    " xmlns:pnpx=\"http://schemas.microsoft.com/windows/pnpx/2005/10\"" \
    " xmlns:pub=\"http://schemas.microsoft.com/windows/pub/2005/07\">"
#define WSD_ANYONE "http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous"
#define WSD_EVERYONE "urn:schemas-xmlsoap-org:ws:2005:04:discovery"
#define WSD_ACTION "http://schemas.xmlsoap.org/ws/2005/04/discovery/"

static unsigned wsd_number; /* of the messages sent since the start */

static void random_uuid(char *out)
{
    u8 raw[16];

    random_bytes(raw, sizeof(raw));
    sprintf(out, "%02x%02x%02x%02x-%02x%02x-4%01x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", raw[0], raw[1], raw[2],
            raw[3], raw[4], raw[5], raw[6] & 15, raw[7], (raw[8] & 0x3f) | 0x80, raw[9], raw[10], raw[11], raw[12],
            raw[13], raw[14], raw[15]);
}

/* the device's own id: the same while the computer keeps its name */
static void device_uuid(char *out)
{
    uint64_t a = 0xcbf29ce484222325ULL, b = 0x84222325cbf29ce4ULL;

    for (const char *p = computer; *p; p++) {
        a = (a ^ (u8)*p) * 0x100000001b3ULL;
        b = (b ^ (u8)*p) * 0x100000001b3ULL + 0x9e3779b97f4a7c15ULL;
    }
    sprintf(out, "%08x-%04x-5%03x-%04x-%012llx", (unsigned)(a >> 32), (unsigned)(a >> 16) & 0xffff,
            (unsigned)a & 0xfff, (unsigned)((b >> 48) & 0x3fff) | 0x8000, (unsigned long long)(b & 0xffffffffffffULL));
}

/* the text of an element, whatever prefix its namespace has in the message */
static int xml_text(const char *xml, const char *tag, char *out, size_t size)
{
    size_t tag_len = strlen(tag);
    const char *p = xml;

    while ((p = strstr(p, tag))) {
        const char *start = p + tag_len, *end;

        if (p > xml && (p[-1] == ':' || p[-1] == '<') && *start == '>' && (end = strchr(++start, '<')) &&
            end > start) {
            const char *open = p;

            while (open > xml && open[-1] != '<' && open[-1] != '/')
                open--;
            if (open > xml && open[-1] == '<') {
                while (start < end && isspace((unsigned char)*start))
                    start++;
                while (end > start && isspace((unsigned char)end[-1]))
                    end--;
                snprintf(out, size, "%.*s", (int)(end - start), start);
                return out[0] != 0;
            }
        }
        p += tag_len;
    }
    return 0;
}

/* a message of the discovery: its header, then what it says about this device */
static int wsd_message(char *out, size_t size, const char *action, const char *to, const char *relates,
                       const char *element, const struct in_addr *address)
{
    char id[40], device[40], host[32] = "", about[700] = "", answers[160] = "";

    random_uuid(id);
    device_uuid(device);
    if (address) {
        inet_ntop(AF_INET, address, host, sizeof(host));
        snprintf(about, sizeof(about),
                 "<wsd:Types>wsdp:Device pub:Computer</wsd:Types>"
                 "<wsd:XAddrs>http://%s:5357/%s</wsd:XAddrs>"
                 "<wsd:MetadataVersion>1</wsd:MetadataVersion>", host, device);
    }
    if (relates)
        snprintf(answers, sizeof(answers), "<wsa:RelatesTo>%.100s</wsa:RelatesTo>", relates);
    return snprintf(out, size,
                    WSD_ENVELOPE "<soap:Header><wsa:To>%s</wsa:To><wsa:Action>" WSD_ACTION "%s</wsa:Action>"
                    "<wsa:MessageID>urn:uuid:%s</wsa:MessageID>%s"
                    "<wsd:AppSequence InstanceId=\"%u\" MessageNumber=\"%u\"/></soap:Header>"
                    "<soap:Body><wsd:%s%s%s><wsa:EndpointReference><wsa:Address>urn:uuid:%s</wsa:Address>"
                    "</wsa:EndpointReference>%s</wsd:%s%s%s></soap:Body></soap:Envelope>",
                    to, action, id, answers, (unsigned)(started / 10000000), __sync_add_and_fetch(&wsd_number, 1),
                    relates ? action : element, relates ? "><wsd:" : "", relates ? element : "", device, about,
                    element, relates ? "></wsd:" : "", relates ? action : "");
}

/* "I am here" and "I am leaving", to everybody on every network */
static void wsd_announce(int fd, const char *what)
{
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(3702)};
    struct nic nics[16];
    int count = list_nics(nics, 16);
    char message[2400];

    inet_pton(AF_INET, WSD_GROUP, &to.sin_addr);
    for (int i = 0; i < count; i++) {
        int len = wsd_message(message, sizeof(message), what, WSD_EVERYONE, NULL, what,
                              strcmp(what, "Bye") ? &nics[i].address : NULL);

        setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &nics[i].address, sizeof(struct in_addr));
        sendto(fd, message, len, 0, (struct sockaddr *)&to, sizeof(to));
        sendto(fd, message, len, 0, (struct sockaddr *)&to, sizeof(to));
    }
}

static void *answer_wsd(void *arg)
{
    int fd = udp_socket(3702), announced = 0;
    uint32_t networks = 0;

    (void)arg;
    if (fd >= 0)
        join_group(fd, WSD_GROUP);
    while (fd >= 0) {
        char buf[8192], reply[2400], action[200], id[128], types[400], device[40];
        struct sockaddr_in from;
        struct in_addr address;
        struct pollfd pfd = {fd, POLLIN, 0};
        socklen_t from_len = sizeof(from);
        struct nic nics[16];
        uint32_t now = 0;
        ssize_t n;
        int len;

        /* a word to everybody when discovery is turned on and when a network comes */
        for (int i = 0, count = list_nics(nics, 16); i < count; i++)
            now = now * 31 + nics[i].address.s_addr;
        if (discovery && (!announced || now != networks)) {
            join_group(fd, WSD_GROUP);
            wsd_announce(fd, "Hello");
            announced = 1;
            networks = now;
        } else if (!discovery && announced) {
            wsd_announce(fd, "Bye");
            announced = 0;
        }
        if (poll(&pfd, 1, 3000) <= 0)
            continue;
        if ((n = recvfrom(fd, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &from_len)) <= 0 || !discovery)
            continue;
        buf[n] = 0;
        if (!xml_text(buf, "Action", action, sizeof(action)) || !xml_text(buf, "MessageID", id, sizeof(id)) ||
            !address_for(&from, &address))
            continue;
        if (strstr(action, "/Probe") && !strstr(action, "Matches")) {
            /* those who look for computers, not for printers and cameras */
            if (xml_text(buf, "Types", types, sizeof(types)) && !strstr(types, ":Device") &&
                !strstr(types, "Computer"))
                continue;
            len = wsd_message(reply, sizeof(reply), "ProbeMatches", WSD_ANYONE, id, "ProbeMatch", &address);
        } else if (strstr(action, "/Resolve") && !strstr(action, "Matches")) {
            device_uuid(device);
            if (!strstr(buf, device)) /* somebody else's address is being asked for */
                continue;
            len = wsd_message(reply, sizeof(reply), "ResolveMatches", WSD_ANYONE, id, "ResolveMatch", &address);
        } else {
            continue;
        }
        sendto(fd, reply, len, 0, (struct sockaddr *)&from, sizeof(from));
    }
    return NULL;
}

/* what the device is, to whoever asks over HTTP: a computer, its name and its workgroup */
static void describe_device(int sock)
{
    static const char description[] =
        WSD_ENVELOPE "<soap:Header><wsa:To>" WSD_ANYONE "</wsa:To>"
        "<wsa:Action>http://schemas.xmlsoap.org/ws/2004/09/transfer/GetResponse</wsa:Action>"
        "<wsa:MessageID>urn:uuid:%s</wsa:MessageID><wsa:RelatesTo>%.100s</wsa:RelatesTo></soap:Header>"
        "<soap:Body><wsx:Metadata>"
        "<wsx:MetadataSection Dialect=\"http://schemas.xmlsoap.org/ws/2006/02/devprof/ThisDevice\">"
        "<wsdp:ThisDevice><wsdp:FriendlyName>%s</wsdp:FriendlyName><wsdp:FirmwareVersion>1.0</wsdp:FirmwareVersion>"
        "<wsdp:SerialNumber>1</wsdp:SerialNumber></wsdp:ThisDevice></wsx:MetadataSection>"
        "<wsx:MetadataSection Dialect=\"http://schemas.xmlsoap.org/ws/2006/02/devprof/ThisModel\">"
        "<wsdp:ThisModel><wsdp:Manufacturer>Arctic</wsdp:Manufacturer><wsdp:ModelName>Arctic</wsdp:ModelName>"
        "<pnpx:DeviceCategory>Computers</pnpx:DeviceCategory></wsdp:ThisModel></wsx:MetadataSection>"
        "<wsx:MetadataSection Dialect=\"http://schemas.xmlsoap.org/ws/2006/02/devprof/Relationship\">"
        "<wsdp:Relationship Type=\"http://schemas.xmlsoap.org/ws/2006/02/devprof/host\"><wsdp:Host>"
        "<wsa:EndpointReference><wsa:Address>urn:uuid:%s</wsa:Address></wsa:EndpointReference>"
        "<wsdp:Types>pub:Computer</wsdp:Types><wsdp:ServiceId>urn:uuid:%s</wsdp:ServiceId>"
        "<pub:Computer>%s/Workgroup:" WORKGROUP "</pub:Computer></wsdp:Host></wsdp:Relationship>"
        "</wsx:MetadataSection></wsx:Metadata></soap:Body></soap:Envelope>";
    struct timeval wait = {3, 0};
    char request[16384], id[128], reply[4096], head[256], uuid[40], device[40], *body;
    size_t total = 0, length = 0;
    ssize_t n;
    int len;

    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &wait, sizeof(wait));
    for (;;) {
        if ((n = recv(sock, request + total, sizeof(request) - 1 - total, 0)) <= 0)
            return;
        total += n;
        request[total] = 0;
        if ((body = strstr(request, "\r\n\r\n"))) {
            char *header = strcasestr(request, "Content-Length:");

            body += 4;
            if (header && header < body)
                length = strtoul(header + 15, NULL, 10);
            if (total - (body - request) >= length || total == sizeof(request) - 1)
                break;
        }
    }
    if (strncmp(request, "POST ", 5) || !strstr(body, "transfer/Get") ||
        !xml_text(body, "MessageID", id, sizeof(id))) {
        static const char refused[] = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";

        if (send(sock, refused, sizeof(refused) - 1, MSG_NOSIGNAL) < 0) {
        }
        return;
    }
    random_uuid(uuid);
    device_uuid(device);
    len = snprintf(reply, sizeof(reply), description, uuid, id, computer, device, device, computer);
    n = snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: application/soap+xml\r\n"
                 "Content-Length: %d\r\nConnection: close\r\n\r\n", len);
    if (send(sock, head, n, MSG_NOSIGNAL) < 0 || send(sock, reply, len, MSG_NOSIGNAL) < 0) {
    }
}

static void *describe_wsd(void *arg)
{
    int listener = -1;

    (void)arg;
    for (;;) {
        struct pollfd pfd = {listener, POLLIN, 0};
        int sock;

        if (!discovery) {
            if (listener >= 0)
                close(listener);
            listener = -1;
            sleep(1);
            continue;
        }
        if (listener < 0 && (listener = listen_tcp(5357)) < 0) {
            sleep(5);
            continue;
        }
        if (poll(&pfd, 1, 1000) <= 0 || (sock = accept4(listener, NULL, NULL, SOCK_CLOEXEC)) < 0)
            continue;
        describe_device(sock);
        close(sock);
    }
    return NULL;
}

static void on_term(int sig)
{
    (void)sig;
    stopping = 1;
}

int main(void)
{
    static void *(*const services[])(void *) = {watch_settings, listen_smb, answer_netbios, answer_llmnr,
                                                answer_mdns, answer_wsd, describe_wsd};
    struct sigaction sa = {.sa_handler = on_term};
    struct passwd *pw = getpwnam(NT_USER);
    pthread_t thread;

    if (pw) {
        nt_uid = pw->pw_uid;
        nt_gid = pw->pw_gid;
    }
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    random_bytes(server_guid, sizeof(server_guid));
    started = filetime_now();

    /* the Windows side writes here as its own user */
    mkdir(C_DRIVE "/ProgramData", 0755);
    mkdir(C_DRIVE "/ProgramData/Arctic", 0755);
    mkdir(SMB_DIR, 0755);
    mkdir(SHARES_DIR, 0755);
    if (chown(SMB_DIR, nt_uid, nt_gid) || chown(SHARES_DIR, nt_uid, nt_gid)) {
    }
    read_computer_name();
    load_config();
    load_shares();
    say("%s: %d shared folders, file sharing %s, network discovery %s", computer, share_count,
        sharing ? "on" : "off", discovery ? "on" : "off");

    for (size_t i = 0; i < sizeof(services) / sizeof(services[0]); i++)
        if (!pthread_create(&thread, NULL, services[i], NULL))
            pthread_detach(thread);
    while (!stopping)
        pause();
    if (discovery) { /* the others need not wait for the name to stop answering */
        int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

        if (fd >= 0)
            wsd_announce(fd, "Bye");
    }
    say("stopped");
    return 0;
}
