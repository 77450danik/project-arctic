/* hang.c: where the NT world's threads wait when it hangs.
 *
 * Twice the whole session stopped (2026-10-22 and 23): every program that
 * started stayed in its start, the desktop with them, and Ctrl+Alt+Del only
 * made the screen blink. nt.log said which thread held a program's loader
 * lock, never what that thread waited for, and the session was gone with
 * the power button. NT itself cannot tell then: anything it starts to look
 * hangs in the same place. arctic-init can, from /proc: each thread's
 * system call, its kernel stack, and the return addresses on its stack,
 * named by the module they are in (win32u.so+0x1234: an offset in the file,
 * which addr2line turns into a function with the build's unstripped copy).
 *
 * Nothing is stopped or attached to: only /proc is read. */
#define _GNU_SOURCE
#include "hang.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void (*hang_log)(const char *fmt, ...);

/* what NT writes when a thread waited too long */
static const char *const triggers[] = {
    "wait timed out",            /* ntdll: a critical section, after a minute */
    "Ctrl+Alt+Del again",        /* csrss: somebody found the screen stuck */
    "display_device_init: held", /* win32u: the display configuration's mutex */
    "has not answered for 20 s", /* win32u: the desktop window */
    NULL,
};

#define MAX_DUMPS 6
#define MIN_APART 90 /* seconds between two */
#define STACK_BYTES (256 << 10)
#define MAX_FRAMES 48

struct mapping {
    uint64_t start, end, offset;
    char path[128];
};

static int read_maps(pid_t pid, struct mapping **out)
{
    char path[64], line[512];
    struct mapping *maps = NULL;
    int n = 0, size = 0;
    FILE *f;

    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    if (!(f = fopen(path, "re")))
        return 0;
    while (fgets(line, sizeof(line), f)) {
        unsigned long long start, end, offset;
        char perms[8], file[384] = "";
        const char *base;

        if (sscanf(line, "%llx-%llx %7s %llx %*s %*s %383[^\n]", &start, &end, perms, &offset, file) < 4 ||
            perms[2] != 'x')
            continue;
        if (n == size) {
            struct mapping *bigger = realloc(maps, (size = size ? size * 2 : 64) * sizeof(*maps));
            if (!bigger)
                break;
            maps = bigger;
        }
        base = strrchr(file, '/');
        maps[n].start = start;
        maps[n].end = end;
        maps[n].offset = offset;
        snprintf(maps[n].path, sizeof(maps[n].path), "%s", base ? base + 1 : file[0] ? file : "[anon]");
        n++;
    }
    fclose(f);
    *out = maps;
    return n;
}

static const struct mapping *code_at(const struct mapping *maps, int n, uint64_t addr)
{
    for (int i = 0; i < n; i++)
        if (addr >= maps[i].start && addr < maps[i].end)
            return &maps[i];
    return NULL;
}

static void read_line(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t len = fd >= 0 ? read(fd, buf, size - 1) : -1;

    if (fd >= 0)
        close(fd);
    buf[len > 0 ? len : 0] = 0;
    buf[strcspn(buf, "\n")] = 0;
}

static void dump_thread(FILE *f, pid_t pid, pid_t tid, int mem, const struct mapping *maps, int n)
{
    static uint64_t stack[STACK_BYTES / 8];
    char path[96], comm[64], wchan[64], sys[256], *word[16], kstack[2048];
    unsigned long long sp = 0, pc = 0;
    const struct mapping *m;
    int words = 0, frames = 0;
    ssize_t got = 0;

    snprintf(path, sizeof(path), "/proc/%d/task/%d/comm", pid, tid);
    read_line(path, comm, sizeof(comm));
    snprintf(path, sizeof(path), "/proc/%d/task/%d/wchan", pid, tid);
    read_line(path, wchan, sizeof(wchan));
    snprintf(path, sizeof(path), "/proc/%d/task/%d/syscall", pid, tid);
    read_line(path, sys, sizeof(sys));
    fprintf(f, "  %d %-16s %-28s %s\n", tid, comm, wchan, sys);

    /* "nr args... sp pc", or "-1 sp pc" outside a system call, or "running" */
    for (char *p = strtok(sys, " "); p && words < 16; p = strtok(NULL, " "))
        word[words++] = p;
    if (words < 3)
        return;
    sp = strtoull(word[words - 2], NULL, 16);
    pc = strtoull(word[words - 1], NULL, 16);
    if ((m = code_at(maps, n, pc)))
        fprintf(f, "    at %s+0x%llx\n", m->path, (unsigned long long)(pc - m->start + m->offset));

    /* the stack upwards from sp, a page at a time until it ends */
    while (mem >= 0 && got < STACK_BYTES) {
        ssize_t r = pread(mem, (char *)stack + got, 4096, (off_t)(sp & ~7ull) + got);
        if (r <= 0)
            break;
        got += r;
    }
    for (ssize_t i = 0; i < got / 8 && frames < MAX_FRAMES; i++) {
        if (!(m = code_at(maps, n, stack[i])) || (i && stack[i] == stack[i - 1]))
            continue;
        fprintf(f, "    %s+0x%llx\n", m->path, (unsigned long long)(stack[i] - m->start + m->offset));
        frames++;
    }

    snprintf(path, sizeof(path), "/proc/%d/task/%d/stack", pid, tid);
    read_line(path, kstack, sizeof(kstack)); /* the first line: where the kernel holds it */
    if (kstack[0])
        fprintf(f, "    kernel: %s\n", kstack);
}

static void dump(const char *dir, uid_t nt_uid, const char *why)
{
    char path[PATH_MAX], name[64], exe[256];
    struct dirent *de, *te;
    struct tm tm;
    time_t now = time(NULL);
    DIR *procs, *tasks;
    FILE *f;

    localtime_r(&now, &tm);
    snprintf(path, sizeof(path), "%s/hang-%04d%02d%02d-%02d%02d%02d.txt", dir, tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    if (!(f = fopen(path, "we")))
        return;
    fprintf(f, "%s\n\n", why);
    if ((procs = opendir("/proc"))) {
        while ((de = readdir(procs))) {
            pid_t pid = atoi(de->d_name);
            struct mapping *maps = NULL;
            char proc[64];
            struct stat st;
            int n, mem;

            snprintf(proc, sizeof(proc), "/proc/%s", de->d_name);
            if (pid <= 1 || stat(proc, &st) || st.st_uid != nt_uid)
                continue;
            snprintf(path, sizeof(path), "%s/comm", proc);
            read_line(path, name, sizeof(name));
            snprintf(path, sizeof(path), "%s/cmdline", proc);
            read_line(path, exe, sizeof(exe)); /* the first argument: the program */
            fprintf(f, "== %d %s %s\n", pid, name, exe);

            n = read_maps(pid, &maps);
            snprintf(path, sizeof(path), "%s/mem", proc);
            mem = open(path, O_RDONLY | O_CLOEXEC);
            snprintf(path, sizeof(path), "%s/task", proc);
            if ((tasks = opendir(path))) {
                while ((te = readdir(tasks)))
                    if (atoi(te->d_name) > 0)
                        dump_thread(f, pid, atoi(te->d_name), mem, maps, n);
                closedir(tasks);
            }
            if (mem >= 0)
                close(mem);
            free(maps);
        }
        closedir(procs);
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    snprintf(path, sizeof(path), "hang-%04d%02d%02d-%02d%02d%02d.txt", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    if (hang_log)
        hang_log("NT waits (%s): every thread's state is in %s", why, path);
}

void hang_watch(int ntlog, const char *dir, uid_t nt_uid)
{
    static char buf[65536 + 256];
    static size_t kept;
    static off_t done;
    static time_t last;
    static int dumps;
    ssize_t n;

    if (ntlog < 0)
        return;
    while ((n = pread(ntlog, buf + kept, sizeof(buf) - kept, done)) > 0) {
        size_t len = kept + (size_t)n;

        done += n;
        for (int i = 0; triggers[i] && dumps < MAX_DUMPS; i++) {
            size_t size = strlen(triggers[i]), from = kept >= size ? kept - size + 1 : 0;
            char *hit = memmem(buf + from, len - from, triggers[i], size), *start, *end, why[512];

            /* only what came now: the kept tail was looked at last time */
            if (!hit || (last && time(NULL) - last < MIN_APART))
                continue;
            for (start = hit; start > buf && start[-1] != '\n'; start--)
                ;
            end = memchr(hit, '\n', len - (size_t)(hit - buf));
            snprintf(why, sizeof(why), "%.*s", (int)((end ? end : buf + len) - start), start);
            dump(dir, nt_uid, why);
            last = time(NULL);
            dumps++;
        }
        /* a line cut in two by the read is whole in the next round */
        kept = len < 256 ? len : 256;
        memmove(buf, buf + len - kept, kept);
    }
}
