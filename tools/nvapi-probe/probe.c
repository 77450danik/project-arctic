/* Read-only probe of NVIDIA's Linux NVAPI (libnvidia-api.so.1): calls only
 * GET functions MSI Afterburner uses, finds each struct's size by trying
 * versions/sizes (a wrong one returns INCOMPATIBLE_STRUCT_VERSION before any
 * driver call), prints the real status. Never calls a SET function. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>

typedef int (*qi_t)(uint32_t);
typedef void *(*qip_t)(uint32_t);
static qip_t qi;
typedef int (*f0)(void);
typedef int (*f2)(void *, void *);
typedef int (*f3)(void *, uint32_t, void *);
typedef int (*f3b)(void *, void *, uint32_t);
typedef int (*f4)(void *, uint32_t, void *, uint32_t *);
typedef int (*fpci)(void *, uint32_t *, uint32_t *, uint32_t *, uint32_t *);
typedef int (*ferr)(int, char *);

enum { S, U32, STR, IS, SI, ISP, PCI };
struct fn { uint32_t id; const char *name; int kind; uint32_t idx; };
static const struct fn fns[] = {
    { 0xCEEE8E9F, "GetFullName", STR }, { 0xD988F0F3, "GetShortName", STR },
    { 0xA561FD7D, "GetVbiosVersionString", STR }, { 0x1BE0B8E5, "GetBusId", U32 },
    { 0x2A0A350F, "GetBusSlotId", U32 }, { 0x2DDFB66E, "GetPCIIdentifiers", PCI },
    { 0x86F05D7A, "GetPartitionCount", U32 }, { 0x63E2F56F, "GetShaderPipeCount", U32 },
    { 0xD8CBF37B, "GetVPECount", U32 }, { 0x5F608315, "GetTachReading", U32 },
    { 0x927DA4F6, "GetCurrentPstate", U32 },
    { 0x1BD69F49, "GetAllClocks", S }, { 0xDCB616C3, "GetAllClockFrequencies", S },
    { 0x60DED2ED, "GetDynamicPstatesInfoEx", S }, { 0x6FF81213, "GetPstates20", S },
    { 0xBA94C56E, "GetPstatesInfo", S }, { 0x843C0256, "GetPstatesInfoEx", SI },
    { 0x1EA54A3B, "GetPerfClocks", S },
    { 0xE3640A56, "GetThermalSettings", IS, 15 }, { 0xDA141340, "GetCoolerSettings", IS, 7 },
    { 0x0518A32C, "GetCoolerPolicyTable", ISP, 0 },
    { 0xC16C7E2C, "GetVoltageDomainsStatus", S }, { 0x7D656244, "GetVoltages", S },
    { 0x465F9BCF, "ClientVoltRailsGetStatus (GetCurrentVoltage)", S },
    { 0x9DF23CA1, "ClientVoltRailsGetControl (GetCoreVoltageBoostPercent)", S },
    { 0xE440B867, "PerfClientLimitsGetStatus (GetClockBoostLock)", S },
    { 0x507B4B59, "ClockClientClkVfPointsGetInfo (GetClockBoostMask)", S },
    { 0x64B43A6A, "ClockClientClkDomainsGetInfo (GetClockBoostRanges)", S },
    { 0x23F1B133, "ClockClientClkVfPointsGetControl (GetClockBoostTable)", S },
    { 0x21537AD4, "ClockClientClkVfPointsGetStatus (GetVFPCurve)", S },
    { 0x34206D86, "ClientPowerPoliciesGetInfo", S }, { 0x70916171, "ClientPowerPoliciesGetStatus", S },
    { 0xA4DFD3F2, "ClientPowerTopologyGetInfo", S }, { 0xEDCF624E, "ClientPowerTopologyGetStatus", S },
    { 0x0D258BB5, "ClientThermalPoliciesGetInfo", S }, { 0xE9C425A1, "ClientThermalPoliciesGetStatus", S },
    { 0x409D9841, "PerfPoliciesGetInfo", S }, { 0x3D358A0C, "PerfPoliciesGetStatus", S },
    { 0xFB85B01E, "ClientFanCoolersGetInfo", S }, { 0x35AED5E8, "ClientFanCoolersGetStatus", S },
    { 0x814B209F, "ClientFanCoolersGetControl", S }, { 0x52B76D12, "ClientFanPoliciesGetInfo", S },
    { 0xE543C540, "ClientFanPoliciesGetControl", S }, { 0xDDDFDA38, "ClientFanArbitersGetInfo", S },
    { 0x0BC8163D, "ThermChannelGetInfo (hotspot/mem)", S }, { 0x65FE3AAD, "ThermChannelGetStatus", S },
};

static char *errstr(int st)
{
    static char b[64];
    ferr e = (ferr)qi(0x6C2D048C);
    b[0] = 0;
    if (e) e(st, b);
    return b;
}

static void dump(const uint32_t *p, unsigned n)
{
    for (unsigned i = 0; i < n; i++) printf("%s%08x", i % 12 ? " " : "\n      ", p[i]);
    printf("\n");
}

static void probe(void)
{
    int st = ((f0)qi(0x0150E828))();
    printf("  Initialize: %d %s\n", st, errstr(st));
    if (st) return;
    void *h[64] = {0}; uint32_t n = 0;
    st = ((f2)qi(0xE5AC921F))(h, &n);
    printf("  EnumPhysicalGPUs: %d, %u GPU(s)\n", st, n);
    if (st || !n) return;
    static uint32_t buf[0x10000];
    for (unsigned i = 0; i < sizeof(fns) / sizeof(fns[0]); i++) {
        const struct fn *f = &fns[i];
        pid_t c = fork();
        if (c) { int ws = 0; waitpid(c, &ws, 0); if (ws) printf("  %-56s CRASHED (status 0x%x)\n", f->name, ws); continue; }
        void *p = qi(f->id);
        if (!p) { printf("  %-56s NOT IN LIBRARY\n", f->name); _exit(0); }
        if (f->kind == U32) {
            uint32_t v = 0; st = ((f2)p)(h[0], &v);
            printf("  %-56s %4d %-28s value %u (0x%x)\n", f->name, st, errstr(st), v, v);
            _exit(0);
        }
        if (f->kind == STR) {
            char s[64] = {0}; st = ((f2)p)(h[0], s);
            printf("  %-56s %4d %-28s \"%s\"\n", f->name, st, errstr(st), s);
            _exit(0);
        }
        if (f->kind == PCI) {
            uint32_t a = 0, b = 0, c = 0, d = 0; st = ((fpci)p)(h[0], &a, &b, &c, &d);
            printf("  %-56s %4d %-28s dev %08x sub %08x rev %x ext %x\n", f->name, st, errstr(st), a, b, c, d);
            _exit(0);
        }
        int found = 0;
        for (unsigned ver = 1; ver <= 4 && !found; ver++)
            for (unsigned size = 8; size <= sizeof(buf) - 4 && !found; size += 4) {
                memset(buf, 0, size + 64);
                buf[0] = size | (ver << 16);
                uint32_t cnt = 0;
                switch (f->kind) {
                case S: st = ((f2)p)(h[0], buf); break;
                case IS: st = ((f3)p)(h[0], f->idx, buf); break;
                case SI: st = ((f3b)p)(h[0], buf, 0); break;
                case ISP: cnt = 0; st = ((f4)p)(h[0], f->idx, buf, &cnt); break;
                }
                if (st != -9) {
                    found = 1;
                    printf("  %-56s %4d %-28s v%u size 0x%x", f->name, st, errstr(st), ver, size);
                    if (!st) dump(buf, size / 4 < 36 ? size / 4 : 36); else printf("\n");
                }
            }
        if (!found) printf("  %-56s no struct version accepted\n", f->name);
        _exit(0);
    }
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("probe start\n");
    void *lib = dlopen(argv[1], RTLD_NOW);
    if (!lib) { printf("dlopen: %s\n", dlerror()); return 1; }
    qi = (qip_t)dlsym(lib, "nvapi_QueryInterface");
    if (!qi) { printf("no nvapi_QueryInterface\n"); return 1; }
    for (int pass = 0; pass < 2; pass++) {
        fflush(stdout);
        pid_t pid = fork();
        if (!pid) {
            if (pass && (setgid(65534) || setuid(65534))) { perror("setuid"); _exit(1); }
            printf("=== pass as uid %d\n", getuid());
            probe();
            fflush(stdout);
            _exit(0);
        }
        int ws = 0;
        waitpid(pid, &ws, 0);
        printf("=== pass %d ended: status 0x%x\n", pass, ws);
    }
    return 0;
}
