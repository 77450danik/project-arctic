/* Write-path probe: for each SET function MSI Afterburner uses, read the
 * current value with its GET twin and write exactly that back (no change),
 * then read again and compare. Run as root and as an unprivileged user. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>

typedef void *(*qip_t)(uint32_t);
static qip_t qi;
typedef int (*f0)(void);
typedef int (*f2)(void *, void *);
typedef int (*ferr)(int, char *);

struct rt { const char *name; uint32_t get, set, ver_size; };
static const struct rt rts[] = {
    { "VF curve table (ClockBoostTable)",   0x23F1B133, 0x0733E009, 0x00012420 },
    { "Core voltage % (VoltRailsControl)",  0x9DF23CA1, 0xB9306D9B, 0x00010028 },
    { "Power limit (PowerPolicies)",        0x70916171, 0xAD95F5ED, 0x00010048 },
    { "Temp limit (ThermalPolicies)",       0xE9C425A1, 0x34C0B13D, 0x00010028 },
    { "Clock offsets (Pstates20)",          0x6FF81213, 0x0F4DAE6B, 0x00011C94 },
    { "Clock/voltage lock (ClientLimits)",  0xE440B867, 0x39442CFB, 0x0001030C },
};

static const char *es(int st)
{
    static char b[64]; ferr e = (ferr)qi(0x6C2D048C); b[0] = 0; if (e) e(st, b); return b;
}

static void run(void)
{
    if (((f0)qi(0x0150E828))()) { printf("  init failed\n"); return; }
    void *h[64] = {0}; uint32_t n = 0;
    ((f2)qi(0xE5AC921F))(h, &n);
    static uint8_t a[0x10000], b[0x10000];
    for (unsigned i = 0; i < sizeof(rts) / sizeof(rts[0]); i++) {
        const struct rt *r = &rts[i];
        unsigned size = r->ver_size & 0xffff;
        memset(a, 0, sizeof(a)); *(uint32_t *)a = r->ver_size;
        int st = ((f2)qi(r->get))(h[0], a);
        if (st) { printf("  %-38s GET %d %s\n", r->name, st, es(st)); continue; }
        memcpy(b, a, size);
        int ss = ((f2)qi(r->set))(h[0], b);
        memset(b, 0, sizeof(b)); *(uint32_t *)b = r->ver_size;
        int sg = ((f2)qi(r->get))(h[0], b);
        int diff = 0; for (unsigned k = 0; k < size; k++) diff += a[k] != b[k];
        printf("  %-38s SET same value: %4d %-28s re-GET %d, bytes changed %d\n", r->name, ss, es(ss), sg, diff);
    }
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    void *lib = dlopen(argv[1], RTLD_NOW);
    if (!lib) { printf("dlopen: %s\n", dlerror()); return 1; }
    qi = (qip_t)dlsym(lib, "nvapi_QueryInterface");
    for (int pass = 0; pass < 2; pass++) {
        pid_t pid = fork();
        if (!pid) {
            if (pass && (setgid(65534) || setuid(65534))) _exit(1);
            printf("=== uid %d\n", getuid());
            run();
            _exit(0);
        }
        int ws; waitpid(pid, &ws, 0);
        if (ws) printf("=== pass %d status 0x%x\n", pass, ws);
    }
    return 0;
}
