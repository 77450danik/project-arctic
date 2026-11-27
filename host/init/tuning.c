/* The Games panel's tuning, done by the host (docs/games.md).
 *
 * The NT world runs as an ordinary user and cannot touch the processor's
 * registers or the graphics driver's limits; the panel writes what it wants
 * in C:\ProgramData\Arctic\games.ini and asks (powrprof's "games" request),
 * and this applies it. What each machine allows is found, not assumed:
 *
 *  - Intel's voltage offsets (MSR 0x150), unless the firmware locked
 *    overclocking (MSR 0x194 bit 20) or the microcode ignores them;
 *  - the package's power limits (RAPL: powercap, the MMIO copy many laptops
 *    have as well), unless locked (MSR 0x610 bit 63);
 *  - the cores' top frequency (cpufreq);
 *  - NVIDIA's power limit, clock offsets and top clock (NVML), AMD's power
 *    cap (hwmon), Intel graphics' top frequency (i915).
 *
 * What the machine allows and what is set now goes to
 * C:\ProgramData\Arctic\tuning-info.ini for the panel. A start that applies
 * tuning leaves a mark on C: that goes once the machine has run two
 * minutes; a start that finds it (the tuning hung or reset the machine)
 * holds the tuning back until the panel applies it again. */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "tuning.h"

#define PROGRAM_DATA "/mnt/c/ProgramData/Arctic"
#define GAMES_INI PROGRAM_DATA "/games.ini"
#define TUNING_INFO PROGRAM_DATA "/tuning-info.ini"
#define HOST_STATE "/mnt/c/Windows/System32/config/Host"
#define TRIAL HOST_STATE "/tuning-trial" /* applied, not yet two minutes up */
#define HELD HOST_STATE "/tuning-held"   /* a start with it did not last */
#define TRIAL_SECONDS 120
#define VOLTAGE_WANTED "/run/arctic/power/sensors-wanted" /* touched by powrprof's ArcticSensors */
#define VOLTAGE_FILE "/run/arctic/power/cpu-voltage"      /* mV */

struct games_settings games = {.game_mode = 1};
void (*tuning_log)(const char *fmt, ...);

static int keeps_state;   /* C: keeps what is written */
static uid_t owner_uid;
static gid_t owner_gid;
static double trial_until; /* uptime, 0 without a trial */
static char status[128] = "none";

#define LOG(...) do { if (tuning_log) tuning_log(__VA_ARGS__); } while (0)

static double uptime(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int read_text(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t n = fd >= 0 ? read(fd, buf, size - 1) : -1;

    if (fd >= 0)
        close(fd);
    buf[n > 0 ? n : 0] = 0;
    buf[strcspn(buf, "\n")] = 0;
    return n > 0;
}

static long long read_ll(const char *path)
{
    char buf[64], *end;
    long long v;

    if (!read_text(path, buf, sizeof(buf)))
        return -1;
    v = strtoll(buf, &end, 10);
    return end == buf ? -1 : v;
}

static int write_ll(const char *path, long long value)
{
    char buf[32];
    int fd = open(path, O_WRONLY | O_CLOEXEC), ok;

    if (fd < 0)
        return 0;
    snprintf(buf, sizeof(buf), "%lld", value);
    ok = write(fd, buf, strlen(buf)) == (ssize_t)strlen(buf);
    close(fd);
    return ok;
}

/**********************************************************************
 *          Settings
 */

void games_load(void)
{
    struct games_settings s = {.game_mode = 1};
    char line[256];
    FILE *f = fopen(GAMES_INI, "re");

    if (f) {
        while (fgets(line, sizeof(line), f)) {
            char *eq = strchr(line, '='), *key = line;
            long v;

            if (!eq || line[0] == '[' || line[0] == ';')
                continue;
            *eq = 0;
            while (*key == ' ')
                key++;
            key[strcspn(key, " \t")] = 0;
            v = strtol(eq + 1, NULL, 10);
#define KEY(name, field) if (!strcasecmp(key, name)) s.field = (int)v;
            KEY("GameMode", game_mode)
            KEY("ApplyAtStart", apply_at_boot)
            KEY("CpuCoreOffset", cpu_core_mv)
            KEY("CpuCacheOffset", cpu_cache_mv)
            KEY("CpuPl1", cpu_pl1_w)
            KEY("CpuPl2", cpu_pl2_w)
            KEY("CpuMaxMhz", cpu_max_mhz)
            KEY("CpuTempTarget", cpu_temp_target)
            KEY("CpuTurboRatio", cpu_turbo_ratio)
            KEY("GpuPower", gpu_power_w)
            KEY("GpuCoreOffset", gpu_core_mhz)
            KEY("GpuMemOffset", gpu_mem_mhz)
            KEY("GpuMaxMhz", gpu_max_mhz)
#undef KEY
        }
        fclose(f);
    }
    /* not further than Intel's own tools go */
    if (s.cpu_core_mv > 150 || s.cpu_core_mv < -250)
        s.cpu_core_mv = 0;
    if (s.cpu_cache_mv > 150 || s.cpu_cache_mv < -250)
        s.cpu_cache_mv = 0;
    /* a target the chip throttles at anyway, or one it never gets below, is no target */
    if (s.cpu_temp_target && (s.cpu_temp_target < 60 || s.cpu_temp_target > 98))
        s.cpu_temp_target = 0;
    games = s;
}

/**********************************************************************
 *          The processor
 */

static struct {
    int intel;
    char name[96];
    int msr;                  /* /dev/cpu/0/msr */
    int undervolt;            /* MSR 0x150 can be written */
    int core_mv, cache_mv;    /* now */
    int rapl;                 /* power limits can be set */
    char rapl_dirs[2][PATH_MAX]; /* intel-rapl:0, intel-rapl-mmio:0 */
    long long pl1_default_uw, pl2_default_uw, pl1_max_uw;
    long long min_khz, max_khz; /* the hardware's */
    int turbo;                /* the turbo ratios can be set (PLATFORM_INFO bit 28) */
    uint64_t turbo_default;   /* MSR 0x1AD as the firmware left it: 1 to 8 cores busy */
    int turbo_base, turbo_max, oc_bins; /* the ratios: without turbo, the most the chip allows */
    int voltage;              /* MSR 0x198 says the cores' voltage */
} cpu;

static int msr_read(unsigned int reg, uint64_t *value)
{
    int fd = open("/dev/cpu/0/msr", O_RDONLY | O_CLOEXEC), ok;

    if (fd < 0)
        return 0;
    ok = pread(fd, value, 8, reg) == 8;
    close(fd);
    return ok;
}

static int msr_write(unsigned int reg, uint64_t value)
{
    int fd = open("/dev/cpu/0/msr", O_WRONLY | O_CLOEXEC), ok;

    if (fd < 0)
        return 0;
    ok = pwrite(fd, &value, 8, reg) == 8;
    close(fd);
    return ok;
}

/* the voltage offset of a plane (0 the cores, 2 the cache), in mV */
static int offset_read(int plane, int *mv)
{
    uint64_t v;
    int units;

    if (!msr_write(0x150, 0x8000001000000000ull | ((uint64_t)plane << 40)) || !msr_read(0x150, &v))
        return 0;
    units = (int)((v >> 21) & 0x7ff);
    if (units & 0x400)
        units -= 0x800;
    *mv = (units * 1000 + (units < 0 ? -512 : 512)) / 1024; /* 1/1024 V */
    return 1;
}

static int offset_write(int plane, int mv)
{
    int units = (mv * 1024 + (mv < 0 ? -500 : 500)) / 1000;
    uint64_t v = 0x8000001100000000ull | ((uint64_t)plane << 40) | ((uint64_t)(units & 0x7ff) << 21);
    int now;

    if (!msr_write(0x150, v))
        return 0;
    /* microcode that refuses undervolting takes the write and keeps 0 */
    return offset_read(plane, &now) && abs(now - mv) <= 1;
}

static void cpu_detect(void)
{
    char line[256];
    uint64_t v;
    FILE *f;

    if ((f = fopen("/proc/cpuinfo", "re"))) {
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "vendor_id", 9))
                cpu.intel = strstr(line, "GenuineIntel") != NULL;
            else if (!strncmp(line, "model name", 10) && strchr(line, ':')) {
                snprintf(cpu.name, sizeof(cpu.name), "%s", strchr(line, ':') + 2);
                cpu.name[strcspn(cpu.name, "\n")] = 0;
                break;
            }
        }
        fclose(f);
    }
    cpu.msr = msr_read(0x194, &v);
    if (cpu.intel && cpu.msr && !((v >> 20) & 1) && offset_read(0, &cpu.core_mv) && offset_read(2, &cpu.cache_mv))
        cpu.undervolt = 1;

    /* the package's limits: the MSR copy, and the MMIO one many laptops keep too */
    for (int i = 0; i < 2; i++) {
        const char *dir = i ? "/sys/class/powercap/intel-rapl-mmio:0" : "/sys/class/powercap/intel-rapl:0";
        char path[PATH_MAX];

        snprintf(path, sizeof(path), "%s/constraint_0_power_limit_uw", dir);
        if (access(path, F_OK))
            continue;
        snprintf(cpu.rapl_dirs[i], sizeof(cpu.rapl_dirs[i]), "%s", dir);
        if (i)
            continue;
        cpu.pl1_default_uw = read_ll(path);
        snprintf(path, sizeof(path), "%s/constraint_1_power_limit_uw", dir);
        cpu.pl2_default_uw = read_ll(path);
        snprintf(path, sizeof(path), "%s/constraint_0_max_power_uw", dir);
        cpu.pl1_max_uw = read_ll(path);
        cpu.rapl = cpu.pl1_default_uw > 0;
    }
    if (cpu.rapl && cpu.intel && cpu.msr && msr_read(0x610, &v) && (v >> 63))
        cpu.rapl = 0; /* locked until the next reset */

    cpu.min_khz = read_ll("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_min_freq");
    cpu.max_khz = read_ll("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");

    /* overclocking by the multiplier: the turbo ratios, as high as the
     * overclocking bins of FLEX_RATIO let them (7: unlimited, an unlocked
     * chip); a locked chip's bins are 0, and the ratios only go down */
    if (cpu.intel && cpu.msr && msr_read(0xce, &v) && ((v >> 28) & 1) && msr_read(0x1ad, &cpu.turbo_default)) {
        uint64_t flex = 0;

        cpu.turbo_base = (int)((v >> 8) & 0xff);
        msr_read(0x194, &flex);
        cpu.oc_bins = (int)((flex >> 17) & 7);
        cpu.turbo_max = (int)(cpu.turbo_default & 0xff) + (cpu.oc_bins == 7 ? 20 : cpu.oc_bins);
        cpu.turbo = cpu.turbo_base > 0 && cpu.turbo_max > cpu.turbo_base;
    }
    cpu.voltage = cpu.intel && cpu.msr && msr_read(0x198, &v) && ((v >> 32) & 0xffff);
}

/* every busy-core count gets the ratio, none above what it had unless raised
 * for all; 0 is the firmware's */
static int set_turbo(int ratio)
{
    uint64_t value = 0;

    if (!ratio)
        return msr_write(0x1ad, cpu.turbo_default);
    for (int i = 0; i < 8; i++) {
        uint64_t byte = (cpu.turbo_default >> (8 * i)) & 0xff;

        if (!byte)
            continue;
        if (ratio > (int)(cpu.turbo_default & 0xff) || (int)byte > ratio)
            byte = ratio;
        value |= byte << (8 * i);
    }
    return msr_write(0x1ad, value);
}

static int set_power_limits(long long pl1_uw, long long pl2_uw)
{
    int ok = 1;

    for (int i = 0; i < 2; i++) {
        char path[PATH_MAX];

        if (!cpu.rapl_dirs[i][0])
            continue;
        snprintf(path, sizeof(path), "%s/constraint_0_power_limit_uw", cpu.rapl_dirs[i]);
        if (pl1_uw > 0 && !write_ll(path, pl1_uw) && !i)
            ok = 0;
        snprintf(path, sizeof(path), "%s/constraint_1_power_limit_uw", cpu.rapl_dirs[i]);
        if (pl2_uw > 0 && !access(path, F_OK) && !write_ll(path, pl2_uw) && !i)
            ok = 0;
    }
    return ok;
}

static int set_max_khz(long long khz)
{
    glob_t g;
    int ok = 1;

    if (glob("/sys/devices/system/cpu/cpufreq/policy*/scaling_max_freq", 0, NULL, &g))
        return 0;
    for (size_t i = 0; i < g.gl_pathc; i++)
        ok &= write_ll(g.gl_pathv[i], khz);
    globfree(&g);
    return ok;
}

/**********************************************************************
 *          The graphics card games run on
 */

typedef void *nvml_device;
static struct {
    int loaded;
    int (*init)(void);
    int (*shutdown)(void);
    int (*by_pci)(const char *bus, nvml_device *device);
    int (*name)(nvml_device, char *, unsigned int);
    int (*limit_constraints)(nvml_device, unsigned int *, unsigned int *);
    int (*limit_default)(nvml_device, unsigned int *);
    int (*limit)(nvml_device, unsigned int *);
    int (*set_limit)(nvml_device, unsigned int);
    int (*gpc_range)(nvml_device, int *, int *);
    int (*gpc_offset)(nvml_device, int *);
    int (*set_gpc_offset)(nvml_device, int);
    int (*mem_range)(nvml_device, int *, int *);
    int (*mem_offset)(nvml_device, int *);
    int (*set_mem_offset)(nvml_device, int);
    int (*max_clock)(nvml_device, int, unsigned int *);
    int (*lock_clocks)(nvml_device, unsigned int, unsigned int);
    int (*reset_clocks)(nvml_device);
} nvml;

static struct {
    int vendor;               /* 0 none, 1 NVIDIA, 2 AMD, 3 Intel */
    char pci[32];
    char name[96];
    char hwmon[PATH_MAX];     /* AMD */
    char card[PATH_MAX];      /* Intel: /sys/class/drm/cardN */
    int power;                /* the limit can be set */
    unsigned int power_min_w, power_max_w, power_default_w, power_w;
    int offsets;              /* clock offsets */
    int core_min, core_max, mem_min, mem_max, core_now, mem_now;
    int max_clock;            /* the top clock can be capped */
    unsigned int hw_max_mhz;
} gpu;

static int nvml_open(void)
{
    void *lib;

    if (!nvml.loaded) {
        nvml.loaded = -1;
        if (!(lib = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL)))
            return 0;
#define SYM(field, name) nvml.field = dlsym(lib, name)
        SYM(init, "nvmlInit_v2");
        SYM(shutdown, "nvmlShutdown");
        SYM(by_pci, "nvmlDeviceGetHandleByPciBusId_v2");
        SYM(name, "nvmlDeviceGetName");
        SYM(limit_constraints, "nvmlDeviceGetPowerManagementLimitConstraints");
        SYM(limit_default, "nvmlDeviceGetPowerManagementDefaultLimit");
        SYM(limit, "nvmlDeviceGetPowerManagementLimit");
        SYM(set_limit, "nvmlDeviceSetPowerManagementLimit");
        SYM(gpc_range, "nvmlDeviceGetGpcClkMinMaxVfOffset");
        SYM(gpc_offset, "nvmlDeviceGetGpcClkVfOffset");
        SYM(set_gpc_offset, "nvmlDeviceSetGpcClkVfOffset");
        SYM(mem_range, "nvmlDeviceGetMemClkMinMaxVfOffset");
        SYM(mem_offset, "nvmlDeviceGetMemClkVfOffset");
        SYM(set_mem_offset, "nvmlDeviceSetMemClkVfOffset");
        SYM(max_clock, "nvmlDeviceGetMaxClockInfo");
        SYM(lock_clocks, "nvmlDeviceSetGpuLockedClocks");
        SYM(reset_clocks, "nvmlDeviceResetGpuLockedClocks");
#undef SYM
        if (!nvml.init || !nvml.shutdown || !nvml.by_pci)
            return 0;
        nvml.loaded = 1;
    }
    return nvml.loaded == 1 && !nvml.init();
}

/* the dedicated card if there is one, else the one the panel hangs on */
static void gpu_detect(void)
{
    glob_t g;

    if (glob("/sys/class/drm/card[0-9]*/device/vendor", 0, NULL, &g))
        return;
    for (size_t i = 0; i < g.gl_pathc; i++) {
        char dir[PATH_MAX], link[PATH_MAX], target[PATH_MAX], *base;
        long long vendor;
        int boot, kind;
        ssize_t n;

        if (strchr(g.gl_pathv[i] + 15, '-'))
            continue; /* card0-eDP-1 */
        snprintf(dir, sizeof(dir), "%s", g.gl_pathv[i]);
        *strrchr(dir, '/') = 0; /* .../device */
        {
            char buf[16];
            read_text(g.gl_pathv[i], buf, sizeof(buf));
            vendor = strtoll(buf, NULL, 16);
        }
        kind = vendor == 0x10de ? 1 : vendor == 0x1002 ? 2 : vendor == 0x8086 ? 3 : 0;
        if (!kind)
            continue;
        snprintf(link, sizeof(link), "%s/boot_vga", dir);
        boot = read_ll(link) == 1;
        if (gpu.vendor && (boot || kind == 3))
            continue; /* a dedicated one found already */
        if ((n = readlink(dir, target, sizeof(target) - 1)) <= 0)
            continue;
        target[n] = 0;
        base = strrchr(target, '/');
        memset(&gpu, 0, sizeof(gpu));
        gpu.vendor = kind;
        snprintf(gpu.pci, sizeof(gpu.pci), "%s", base ? base + 1 : target);
        snprintf(gpu.card, sizeof(gpu.card), "%s", dir);
        *strrchr(gpu.card, '/') = 0; /* /sys/class/drm/cardN */
        if (!boot && kind != 3)
            break;
    }
    globfree(&g);

    if (gpu.vendor == 1 && nvml_open()) {
        nvml_device dev;
        unsigned int lo, hi, mw;

        if (!nvml.by_pci(gpu.pci, &dev)) {
            if (nvml.name)
                nvml.name(dev, gpu.name, sizeof(gpu.name));
            if (nvml.limit_constraints && nvml.set_limit && !nvml.limit_constraints(dev, &lo, &hi) && hi > lo) {
                gpu.power = 1;
                gpu.power_min_w = lo / 1000;
                gpu.power_max_w = hi / 1000;
                if (nvml.limit_default && !nvml.limit_default(dev, &mw))
                    gpu.power_default_w = mw / 1000;
                if (nvml.limit && !nvml.limit(dev, &mw))
                    gpu.power_w = mw / 1000;
            }
            if (nvml.gpc_range && nvml.set_gpc_offset && !nvml.gpc_range(dev, &gpu.core_min, &gpu.core_max) &&
                gpu.core_max > gpu.core_min) {
                gpu.offsets = 1;
                if (nvml.gpc_offset)
                    nvml.gpc_offset(dev, &gpu.core_now);
                if (nvml.mem_range && !nvml.mem_range(dev, &gpu.mem_min, &gpu.mem_max) && nvml.mem_offset)
                    nvml.mem_offset(dev, &gpu.mem_now);
            }
            if (nvml.max_clock && !nvml.max_clock(dev, 0, &gpu.hw_max_mhz) && nvml.reset_clocks &&
                nvml.lock_clocks && !nvml.reset_clocks(dev))
                gpu.max_clock = 1; /* Volta and newer; the reset is the state it is in anyway */
        }
        nvml.shutdown();
    } else if (gpu.vendor == 2) {
        glob_t h;
        char pattern[PATH_MAX + 32];

        snprintf(pattern, sizeof(pattern), "/sys/bus/pci/devices/%s/hwmon/hwmon*/power1_cap", gpu.pci);
        if (!glob(pattern, 0, NULL, &h)) {
            char path[PATH_MAX + 16];
            long long lo, hi, def, now;

            snprintf(gpu.hwmon, sizeof(gpu.hwmon), "%s", h.gl_pathv[0]);
            *strrchr(gpu.hwmon, '/') = 0;
            snprintf(path, sizeof(path), "%s/power1_cap_min", gpu.hwmon);
            lo = read_ll(path);
            snprintf(path, sizeof(path), "%s/power1_cap_max", gpu.hwmon);
            hi = read_ll(path);
            snprintf(path, sizeof(path), "%s/power1_cap_default", gpu.hwmon);
            def = read_ll(path);
            snprintf(path, sizeof(path), "%s/power1_cap", gpu.hwmon);
            now = read_ll(path);
            if (hi > lo && lo >= 0 && now > 0) {
                gpu.power = 1;
                gpu.power_min_w = lo / 1000000;
                gpu.power_max_w = hi / 1000000;
                gpu.power_default_w = (def > 0 ? def : now) / 1000000;
                gpu.power_w = now / 1000000;
            }
            globfree(&h);
        }
    } else if (gpu.vendor == 3) {
        char path[PATH_MAX + 32];
        long long rp0;

        snprintf(path, sizeof(path), "%s/gt_RP0_freq_mhz", gpu.card);
        if ((rp0 = read_ll(path)) > 0) {
            gpu.hw_max_mhz = rp0;
            gpu.max_clock = 1;
        }
    }
}

/* what fails, by name, for the panel */
static void gpu_apply(char *failed, size_t size)
{
    if (gpu.vendor == 1 && (gpu.power || gpu.offsets || gpu.max_clock) && nvml_open()) {
        nvml_device dev;

        if (!nvml.by_pci(gpu.pci, &dev)) {
            if (gpu.power) {
                unsigned int w = games.gpu_power_w ? games.gpu_power_w : gpu.power_default_w;

                if (w < gpu.power_min_w) w = gpu.power_min_w;
                if (w > gpu.power_max_w) w = gpu.power_max_w;
                if (w != gpu.power_w) {
                    if (nvml.set_limit(dev, w * 1000))
                        strncat(failed, " gpu-power", size - strlen(failed) - 1);
                    else
                        gpu.power_w = w;
                }
            }
            if (gpu.offsets) {
                int core = games.gpu_core_mhz, mem = games.gpu_mem_mhz;

                core = core < gpu.core_min ? gpu.core_min : core > gpu.core_max ? gpu.core_max : core;
                mem = mem < gpu.mem_min ? gpu.mem_min : mem > gpu.mem_max ? gpu.mem_max : mem;
                if (core != gpu.core_now) {
                    if (nvml.set_gpc_offset(dev, core))
                        strncat(failed, " gpu-core", size - strlen(failed) - 1);
                    else
                        gpu.core_now = core;
                }
                if (mem != gpu.mem_now && nvml.set_mem_offset) {
                    if (nvml.set_mem_offset(dev, mem))
                        strncat(failed, " gpu-mem", size - strlen(failed) - 1);
                    else
                        gpu.mem_now = mem;
                }
            }
            if (gpu.max_clock) {
                if (games.gpu_max_mhz > 0 ? nvml.lock_clocks(dev, 0, games.gpu_max_mhz) : nvml.reset_clocks(dev))
                    strncat(failed, " gpu-max", size - strlen(failed) - 1);
            }
        }
        nvml.shutdown();
    } else if (gpu.vendor == 2 && gpu.power) {
        char path[PATH_MAX + 16];
        unsigned int w = games.gpu_power_w ? games.gpu_power_w : gpu.power_default_w;

        if (w < gpu.power_min_w) w = gpu.power_min_w;
        if (w > gpu.power_max_w) w = gpu.power_max_w;
        snprintf(path, sizeof(path), "%s/power1_cap", gpu.hwmon);
        if (w != gpu.power_w) {
            if (!write_ll(path, (long long)w * 1000000))
                strncat(failed, " gpu-power", size - strlen(failed) - 1);
            else
                gpu.power_w = w;
        }
    } else if (gpu.vendor == 3 && gpu.max_clock) {
        char path[PATH_MAX + 32];
        long long mhz = games.gpu_max_mhz > 0 && games.gpu_max_mhz < (int)gpu.hw_max_mhz ? games.gpu_max_mhz
                                                                                        : gpu.hw_max_mhz;

        snprintf(path, sizeof(path), "%s/gt_max_freq_mhz", gpu.card);
        if (!write_ll(path, mhz))
            strncat(failed, " gpu-max", size - strlen(failed) - 1);
    }
}

/**********************************************************************
 *          Applying, and what the panel reads
 */

static void write_info(void)
{
    char tmp[] = TUNING_INFO ".new";
    FILE *f;

    mkdir(PROGRAM_DATA, 0755);
    if (!(f = fopen(tmp, "we")))
        return;
    fprintf(f, "[Cpu]\r\nName=%s\r\nIntel=%d\r\n", cpu.name, cpu.intel);
    fprintf(f, "Undervolt=%d\r\nCoreOffset=%d\r\nCacheOffset=%d\r\n", cpu.undervolt, cpu.core_mv, cpu.cache_mv);
    fprintf(f, "PowerLimits=%d\r\n", cpu.rapl);
    if (cpu.rapl) {
        char path[PATH_MAX];

        snprintf(path, sizeof(path), "%s/constraint_0_power_limit_uw", cpu.rapl_dirs[0]);
        fprintf(f, "Pl1=%lld\r\n", read_ll(path) / 1000000);
        snprintf(path, sizeof(path), "%s/constraint_1_power_limit_uw", cpu.rapl_dirs[0]);
        fprintf(f, "Pl2=%lld\r\n", read_ll(path) / 1000000);
        fprintf(f, "Pl1Default=%lld\r\nPl2Default=%lld\r\nPlMax=%lld\r\n", cpu.pl1_default_uw / 1000000,
                cpu.pl2_default_uw / 1000000, cpu.pl1_max_uw > 0 ? cpu.pl1_max_uw / 1000000 : 0);
    }
    fprintf(f, "MinMhz=%lld\r\nMaxMhz=%lld\r\n", cpu.min_khz / 1000, cpu.max_khz / 1000);
    fprintf(f, "Turbo=%d\r\nTurboBase=%d\r\nTurboDefault=%d\r\nTurboMax=%d\r\nOcBins=%d\r\nVoltage=%d\r\n", cpu.turbo,
            cpu.turbo_base, (int)(cpu.turbo_default & 0xff), cpu.turbo_max, cpu.oc_bins, cpu.voltage);
    fprintf(f, "[Gpu]\r\nVendor=%d\r\nPci=%s\r\nName=%s\r\n", gpu.vendor, gpu.pci, gpu.name);
    fprintf(f, "PowerLimit=%d\r\nPowerMin=%u\r\nPowerMax=%u\r\nPowerDefault=%u\r\nPower=%u\r\n", gpu.power,
            gpu.power_min_w, gpu.power_max_w, gpu.power_default_w, gpu.power_w);
    fprintf(f, "ClockOffsets=%d\r\nCoreMin=%d\r\nCoreMax=%d\r\nMemMin=%d\r\nMemMax=%d\r\nCore=%d\r\nMem=%d\r\n",
            gpu.offsets, gpu.core_min, gpu.core_max, gpu.mem_min, gpu.mem_max, gpu.core_now, gpu.mem_now);
    fprintf(f, "MaxClock=%d\r\nMaxClockMhz=%u\r\n", gpu.max_clock, gpu.hw_max_mhz);
    fprintf(f, "[State]\r\nStatus=%s\r\n", status);
    fclose(f);
    chown(tmp, owner_uid, owner_gid);
    rename(tmp, TUNING_INFO);
}

static int anything_set(void)
{
    return games.cpu_core_mv || games.cpu_cache_mv || games.cpu_pl1_w || games.cpu_pl2_w || games.cpu_max_mhz ||
           games.cpu_turbo_ratio || games.cpu_temp_target ||
           games.gpu_power_w || games.gpu_core_mhz || games.gpu_mem_mhz || games.gpu_max_mhz;
}

/* the top frequency written last, and the one the temperature target holds
 * now (0: not holding one) */
static long long applied_max_khz, thermal_khz;

/* the top frequency asked for: the panel's, or the chip's own */
static long long top_khz(void)
{
    long long khz = games.cpu_max_mhz > 0 ? games.cpu_max_mhz * 1000ll : cpu.max_khz;

    if (khz < cpu.min_khz)
        khz = cpu.min_khz;
    if (khz > cpu.max_khz)
        khz = cpu.max_khz;
    return khz;
}

static void apply(void)
{
    static int applied_limits;
    char failed[96] = "";
    int fd;

    /* a mark on C: until the machine has run with it two minutes */
    if (keeps_state && anything_set() && (fd = open(TRIAL, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)) >= 0) {
        fsync(fd);
        close(fd);
        trial_until = uptime() + TRIAL_SECONDS;
    }

    if (cpu.undervolt) {
        if (games.cpu_core_mv != cpu.core_mv) {
            if (offset_write(0, games.cpu_core_mv))
                cpu.core_mv = games.cpu_core_mv;
            else
                strcat(failed, " core");
        }
        if (games.cpu_cache_mv != cpu.cache_mv) {
            if (offset_write(2, games.cpu_cache_mv))
                cpu.cache_mv = games.cpu_cache_mv;
            else
                strcat(failed, " cache");
        }
    }
    /* the firmware may move the limits itself (a fan or performance button):
     * left alone unless set here */
    if (cpu.rapl && (games.cpu_pl1_w > 0 || games.cpu_pl2_w > 0 || applied_limits)) {
        long long pl1 = games.cpu_pl1_w > 0 ? games.cpu_pl1_w * 1000000ll : cpu.pl1_default_uw;
        long long pl2 = games.cpu_pl2_w > 0 ? games.cpu_pl2_w * 1000000ll : cpu.pl2_default_uw;

        if (pl2 > 0 && pl2 < pl1)
            pl2 = pl1;
        if (!set_power_limits(pl1, pl2))
            strcat(failed, " power");
        else
            applied_limits = games.cpu_pl1_w > 0 || games.cpu_pl2_w > 0;
    }
    if (cpu.max_khz > 0) {
        long long khz = top_khz();

        /* the power plan sets the limit too: only a change is written */
        if (khz != (applied_max_khz ? applied_max_khz : cpu.max_khz)) {
            if (set_max_khz(khz))
                applied_max_khz = khz;
            else
                strcat(failed, " max");
        }
        /* the temperature target starts again from the top */
        thermal_khz = 0;
    }
    if (cpu.turbo) {
        static int applied_turbo;
        int ratio = games.cpu_turbo_ratio;

        if (ratio && (ratio < cpu.turbo_base || ratio > cpu.turbo_max))
            ratio = 0;
        if (ratio != applied_turbo) {
            if (set_turbo(ratio))
                applied_turbo = ratio;
            else
                strcat(failed, " turbo");
        }
    }
    gpu_apply(failed, sizeof(failed));

    if (failed[0]) {
        snprintf(status, sizeof(status), "failed%s", failed);
        LOG("tuning: not applied:%s", failed);
    } else {
        snprintf(status, sizeof(status), "%s", anything_set() ? "applied" : "none");
        if (anything_set())
            LOG("tuning: applied (core %d mV, cache %d mV, PL1 %d W, PL2 %d W, max %d MHz, held at %d C, GPU %d W %+d/%+d MHz max %d)",
                games.cpu_core_mv, games.cpu_cache_mv, games.cpu_pl1_w, games.cpu_pl2_w, games.cpu_max_mhz, games.cpu_temp_target,
                games.gpu_power_w, games.gpu_core_mhz, games.gpu_mem_mhz, games.gpu_max_mhz);
    }
    write_info();
}

void tuning_boot(int keeps, uid_t uid, gid_t gid)
{
    keeps_state = keeps;
    owner_uid = uid;
    owner_gid = gid;
    games_load();
    cpu_detect();
    gpu_detect();
    LOG("tuning: %s: undervolt %s, power limits %s; graphics %s: power %s, offsets %s, top clock %s", cpu.name,
        cpu.undervolt ? "yes" : "no", cpu.rapl ? "yes" : "no", gpu.pci[0] ? gpu.pci : "none",
        gpu.power ? "yes" : "no", gpu.offsets ? "yes" : "no", gpu.max_clock ? "yes" : "no");

    if (keeps_state && !access(TRIAL, F_OK)) {
        /* the last start with it did not last two minutes */
        rename(TRIAL, HELD);
        LOG("tuning: the last start with it applied did not last; held back until applied again");
    }
    if (games.apply_at_boot && anything_set()) {
        if (!access(HELD, F_OK)) {
            snprintf(status, sizeof(status), "held");
            write_info();
            return;
        }
        apply();
        return;
    }
    write_info();
}

void tuning_apply(void)
{
    games_load();
    unlink(HELD);
    /* the card again: its driver may have come after the start's look */
    if (!gpu.power && !gpu.offsets && !gpu.max_clock)
        gpu_detect();
    apply();
}

void tuning_game_started(void)
{
    char failed[64] = "";

    if (!games.gpu_power_w && !games.gpu_core_mhz && !games.gpu_mem_mhz && !games.gpu_max_mhz)
        return;
    if (!games.apply_at_boot && strcmp(status, "applied"))
        return;
    /* a card that slept between games may have lost them: written again */
    gpu.power_w = 0;
    gpu.core_now = gpu.mem_now = 0x7fffffff;
    gpu_apply(failed, sizeof(failed));
    if (failed[0])
        LOG("tuning: the graphics card's settings for the game:%s", failed);
}

/* the cores' voltage for the panel and the overlay, which cannot read the
 * processor's registers: only while they read (powrprof touches the file
 * it asks with), twice a second */
static void serve_voltage(void)
{
    static double last;
    struct stat st;
    struct timespec now;
    uint64_t v;
    unsigned int mv;
    char text[32];
    int fd;

    if (!cpu.voltage || uptime() - last < 0.5)
        return;
    last = uptime();
    clock_gettime(CLOCK_REALTIME, &now);
    if (stat(VOLTAGE_WANTED, &st) || now.tv_sec - st.st_mtime > 3)
        return;
    if (!msr_read(0x198, &v))
        return;
    mv = (unsigned int)(((v >> 32) & 0xffff) * 1000 / 8192);
    if (mv < 400 || mv > 2000)
        return; /* not a voltage: a virtual machine's register, an older chip's other field */
    snprintf(text, sizeof(text), "%u\n", mv);
    if ((fd = open(VOLTAGE_FILE ".new", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)) < 0)
        return;
    if (write(fd, text, strlen(text)) < 0)
        LOG("tuning: voltage: %s", strerror(errno));
    close(fd);
    rename(VOLTAGE_FILE ".new", VOLTAGE_FILE);
}

/* the hottest of the processor's sensors (coretemp: the package and each
 * core), in °C; -1 without them */
static int hottest_core(void)
{
    static char dir[PATH_MAX];
    static int searched;
    char path[PATH_MAX + 32];
    int hottest = -1;

    if (!searched) {
        DIR *d = opendir("/sys/class/hwmon");
        struct dirent *de;
        char name[32];

        searched = 1;
        while (d && (de = readdir(d))) {
            int fd;
            ssize_t n;

            if (de->d_name[0] == '.')
                continue;
            snprintf(path, sizeof(path), "/sys/class/hwmon/%s/name", de->d_name);
            if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
                continue;
            n = read(fd, name, sizeof(name) - 1);
            close(fd);
            name[n > 0 ? n : 0] = 0;
            if (!strncmp(name, "coretemp", 8) || !strncmp(name, "k10temp", 7) || !strncmp(name, "zenpower", 8)) {
                snprintf(dir, sizeof(dir), "/sys/class/hwmon/%s", de->d_name);
                break;
            }
        }
        if (d)
            closedir(d);
    }
    if (!dir[0])
        return -1;
    for (int i = 1; i <= 64; i++) {
        long long mc;

        snprintf(path, sizeof(path), "%s/temp%d_input", dir, i);
        if (access(path, F_OK))
            continue;
        if ((mc = read_ll(path)) / 1000 > hottest)
            hottest = (int)(mc / 1000);
    }
    return hottest;
}

/* The temperature target (games.ini CpuTempTarget): the hottest core is held
 * near it by the top frequency, before the chip throttles itself. Intel's own
 * throttle at its limit (95 °C on this laptop) drops the clock all at once,
 * and a heavy frame of a game falls into it: in CS2 on a GTX 1050 laptop that
 * shares its heat pipes, 91 % of the seconds with a hitch were throttled ones,
 * and below 92 °C there were nearly none (2026-11-23). A fixed top frequency
 * is either too hot or too slow, as the card's heat and the game's load
 * change: here it follows the temperature, down quickly, up slowly. */
static void hold_temperature(void)
{
    static double last_up;
    long long top, khz, step = 0;
    int t;

    if (!games.cpu_temp_target || cpu.max_khz <= 0 || (t = hottest_core()) < 0) {
        if (thermal_khz) {
            /* turned off: the panel's top frequency again */
            if (set_max_khz(top_khz()))
                applied_max_khz = top_khz();
            thermal_khz = 0;
        }
        return;
    }
    top = top_khz();
    khz = thermal_khz ? thermal_khz : top;
    if (t >= games.cpu_temp_target + 2)
        step = -200000;
    else if (t >= games.cpu_temp_target)
        step = -100000;
    else if (t <= games.cpu_temp_target - 4 && uptime() - last_up >= 1.0) {
        step = 100000;
        last_up = uptime();
    }
    khz += step;
    if (khz > top)
        khz = top;
    if (khz < 1600000 && khz < top)
        khz = top < 1600000 ? top : 1600000;
    if (khz < cpu.min_khz)
        khz = cpu.min_khz;
    if (khz != (applied_max_khz ? applied_max_khz : cpu.max_khz)) {
        if (set_max_khz(khz))
            applied_max_khz = khz;
    }
    thermal_khz = khz;
}

void tuning_tick(void)
{
    if (trial_until && uptime() >= trial_until) {
        unlink(TRIAL);
        trial_until = 0;
    }
    serve_voltage();
    hold_temperature();
}
