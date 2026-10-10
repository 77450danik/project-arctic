/*
 * Power management: the hardware (unix side)
 *
 * Whatever the laptop has, read from the kernel as it is found: batteries
 * and power supplies (/sys/class/power_supply: energy_* or charge_* times
 * the voltage, power_now or current_now times the voltage), graphics cards
 * (/sys/class/drm, hwmon, RAPL for an Intel processor's own graphics, NVML
 * for NVIDIA's driver), the processor's frequency driver (intel_pstate,
 * amd-pstate or another cpufreq driver), the ACPI platform profile and the
 * panel's backlight. What the machine lacks is reported missing; nothing
 * here is one laptop's.
 *
 * The settings it writes (brightness, the processor's energy preference and
 * limits, the platform profile, PCI Express and USB power saving) are files
 * the host gives the NT user at start (arctic-init, udev).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"

#include "unixlib.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(powrprof);

/**********************************************************************
 *          sysfs
 */

static BOOL read_str( const char *dir, const char *name, char *buf, size_t size )
{
    char path[PATH_MAX];
    ssize_t n;
    int fd;

    if (snprintf( path, sizeof(path), "%s/%s", dir, name ) >= (int)sizeof(path)) return FALSE;
    if ((fd = open( path, O_RDONLY | O_CLOEXEC )) < 0) return FALSE;
    n = read( fd, buf, size - 1 );
    close( fd );
    if (n < 0) return FALSE;
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == ' ')) n--;
    buf[n] = 0;
    return TRUE;
}

/* a number, or -1 when the file is not there or says nothing */
static long long read_num( const char *dir, const char *name )
{
    char buf[64], *end;
    long long value;

    if (!read_str( dir, name, buf, sizeof(buf) ) || !buf[0]) return -1;
    value = strtoll( buf, &end, 10 );
    return end == buf ? -1 : value;
}

static BOOL write_str( const char *dir, const char *name, const char *value )
{
    char path[PATH_MAX];
    size_t len = strlen( value );
    BOOL ok;
    int fd;

    if (snprintf( path, sizeof(path), "%s/%s", dir, name ) >= (int)sizeof(path)) return FALSE;
    if ((fd = open( path, O_WRONLY | O_CLOEXEC )) < 0) return FALSE;
    ok = write( fd, value, len ) == (ssize_t)len;
    if (!ok) WARN( "%s: %s\n", path, strerror( errno ) );
    close( fd );
    return ok;
}

static BOOL write_num( const char *dir, const char *name, long long value )
{
    char buf[32];

    snprintf( buf, sizeof(buf), "%lld", value );
    return write_str( dir, name, buf );
}

static void to_wide( WCHAR *dst, size_t count, const char *src )
{
    size_t i;

    /* the kernel's names are ASCII; anything else is not shown wrong */
    for (i = 0; i + 1 < count && src[i]; i++) dst[i] = (unsigned char)src[i] < 0x80 ? src[i] : '?';
    dst[i] = 0;
}

/* back to ASCII: the names it holds came from the kernel as such */
static void to_narrow( char *dst, size_t count, const WCHAR *src )
{
    size_t i;

    for (i = 0; i + 1 < count && src[i]; i++) dst[i] = src[i] < 0x80 ? src[i] : '?';
    dst[i] = 0;
}

static int wide_cmp( const WCHAR *a, const WCHAR *b )
{
    while (*a && *a == *b) a++, b++;
    return *a - *b;
}

static UINT32 clamp_u32( long long value )
{
    if (value < 0) return ARCTIC_UNKNOWN;
    if (value >= ARCTIC_UNKNOWN) return ARCTIC_UNKNOWN - 1;
    return value;
}

static double now_seconds(void)
{
    struct timespec ts;

    clock_gettime( CLOCK_MONOTONIC, &ts );
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/**********************************************************************
 *          Batteries and the mains
 */

#define POWER_SUPPLY "/sys/class/power_supply"

static void read_battery( const char *dir, struct arctic_battery *bat, const char *name )
{
    long long voltage = read_num( dir, "voltage_now" ), design_voltage = read_num( dir, "voltage_min_design" );
    long long now, full, design, power, current, capacity;
    char buf[64];

    to_wide( bat->name, ARRAY_SIZE(bat->name), name );
    if (read_str( dir, "manufacturer", buf, sizeof(buf) )) to_wide( bat->manufacturer, ARRAY_SIZE(bat->manufacturer), buf );
    if (read_str( dir, "model_name", buf, sizeof(buf) )) to_wide( bat->model, ARRAY_SIZE(bat->model), buf );
    if (read_str( dir, "technology", buf, sizeof(buf) )) to_wide( bat->chemistry, ARRAY_SIZE(bat->chemistry), buf );

    bat->state = ARCTIC_BATTERY_UNKNOWN;
    if (read_str( dir, "status", buf, sizeof(buf) ))
    {
        if (!strcmp( buf, "Discharging" )) bat->state = ARCTIC_BATTERY_DISCHARGING;
        else if (!strcmp( buf, "Charging" )) bat->state = ARCTIC_BATTERY_CHARGING;
        else if (!strcmp( buf, "Not charging" )) bat->state = ARCTIC_BATTERY_NOT_CHARGING;
        else if (!strcmp( buf, "Full" )) bat->state = ARCTIC_BATTERY_FULL;
    }

    /* energy in µWh; or charge in µAh, which the design voltage turns into
     * energy (as UPower does: the present voltage would make a full battery
     * look bigger than an empty one) */
    if ((now = read_num( dir, "energy_now" )) >= 0)
    {
        full = read_num( dir, "energy_full" );
        design = read_num( dir, "energy_full_design" );
    }
    else
    {
        long long volts = design_voltage > 0 ? design_voltage : voltage;

        now = read_num( dir, "charge_now" );
        full = read_num( dir, "charge_full" );
        design = read_num( dir, "charge_full_design" );
        if (volts <= 0) now = full = design = -1;
        else
        {
            if (now >= 0) now = now * volts / 1000000;
            if (full >= 0) full = full * volts / 1000000;
            if (design >= 0) design = design * volts / 1000000;
        }
    }
    bat->remaining_mwh = now >= 0 ? clamp_u32( now / 1000 ) : ARCTIC_UNKNOWN;
    bat->full_mwh = full > 0 ? clamp_u32( full / 1000 ) : ARCTIC_UNKNOWN;
    bat->design_mwh = design > 0 ? clamp_u32( design / 1000 ) : ARCTIC_UNKNOWN;

    /* power in µW; or current in µA times the present voltage. Some report
     * a negative current while discharging. */
    if ((power = read_num( dir, "power_now" )) < 0 && (current = read_num( dir, "current_now" )) != -1 && voltage > 0)
        power = llabs( current ) * voltage / 1000000;
    bat->rate_mw = power > 0 ? clamp_u32( llabs( power ) / 1000 ) : power == 0 ? 0 : ARCTIC_UNKNOWN;
    bat->voltage_mv = voltage > 0 ? clamp_u32( voltage / 1000 ) : ARCTIC_UNKNOWN;
    bat->cycles = clamp_u32( read_num( dir, "cycle_count" ) );

    if ((capacity = read_num( dir, "capacity" )) >= 0) bat->percent = min( capacity, 100 );
    else if (now >= 0 && full > 0) bat->percent = min( now * 100 / full, 100 );
    else bat->percent = 0;
}

static BOOL is_mains( const char *type )
{
    /* USB-C and USB chargers power the laptop as its adapter does */
    return !strcmp( type, "Mains" ) || !strncmp( type, "USB", 3 ) || !strcmp( type, "Wireless" );
}

/* the time left, from a rate smoothed over about a minute: what the battery
 * says right now jumps with every burst of work */
static double smoothed_rate;
static double smoothed_at;

static NTSTATUS power_status( void *args )
{
    struct arctic_power_status *status = args;
    long long remaining = 0, full = 0;
    UINT32 percent_sum = 0, charging = 0, discharging = 0, not_charging = 0, full_count = 0;
    BOOL energy_known = TRUE, rate_known = TRUE;
    UINT32 rate = 0;
    struct dirent *de;
    char dir[PATH_MAX], buf[64];
    DIR *d;

    memset( status, 0, sizeof(*status) );
    status->percent = status->rate_mw = status->seconds_left = ARCTIC_UNKNOWN;

    if ((d = opendir( POWER_SUPPLY )))
    {
        while ((de = readdir( d )))
        {
            if (de->d_name[0] == '.') continue;
            snprintf( dir, sizeof(dir), POWER_SUPPLY "/%s", de->d_name );
            /* a mouse's or a headset's battery is the device's, not the machine's */
            if (read_str( dir, "scope", buf, sizeof(buf) ) && !strcmp( buf, "Device" )) continue;
            if (!read_str( dir, "type", buf, sizeof(buf) )) continue;
            if (is_mains( buf ))
            {
                status->ac_present = TRUE;
                if (read_num( dir, "online" ) > 0) status->ac_online = TRUE;
            }
            else if (!strcmp( buf, "Battery" ) && status->battery_count < ARCTIC_MAX_BATTERIES)
            {
                struct arctic_battery *bat = &status->batteries[status->battery_count];

                if (!read_num( dir, "present" )) continue;
                read_battery( dir, bat, de->d_name );
                status->battery_count++;
            }
        }
        closedir( d );
    }

    /* a stable order: BAT0 before BAT1, as Windows numbers them */
    for (UINT32 i = 1; i < status->battery_count; i++)
        for (UINT32 j = i; j > 0 && wide_cmp( status->batteries[j - 1].name, status->batteries[j].name ) > 0; j--)
        {
            struct arctic_battery tmp = status->batteries[j];
            status->batteries[j] = status->batteries[j - 1];
            status->batteries[j - 1] = tmp;
        }

    for (UINT32 i = 0; i < status->battery_count; i++)
    {
        const struct arctic_battery *bat = &status->batteries[i];

        if (bat->remaining_mwh == ARCTIC_UNKNOWN || bat->full_mwh == ARCTIC_UNKNOWN) energy_known = FALSE;
        else
        {
            remaining += bat->remaining_mwh;
            full += bat->full_mwh;
        }
        if (bat->rate_mw == ARCTIC_UNKNOWN) rate_known = FALSE;
        else rate += bat->rate_mw;
        percent_sum += bat->percent;
        switch (bat->state)
        {
        case ARCTIC_BATTERY_CHARGING: charging++; break;
        case ARCTIC_BATTERY_DISCHARGING: discharging++; break;
        case ARCTIC_BATTERY_NOT_CHARGING: not_charging++; break;
        case ARCTIC_BATTERY_FULL: full_count++; break;
        }
    }

    if (status->battery_count)
    {
        /* the batteries as one, as Windows' composite battery */
        status->percent = energy_known && full ? (UINT32)min( (remaining * 100 + full / 2) / full, 100 )
                                               : percent_sum / status->battery_count;
        if (charging) status->state = ARCTIC_BATTERY_CHARGING;
        else if (discharging) status->state = ARCTIC_BATTERY_DISCHARGING;
        else if (full_count == status->battery_count) status->state = ARCTIC_BATTERY_FULL;
        else if (not_charging) status->state = ARCTIC_BATTERY_NOT_CHARGING;
        else status->state = status->ac_online ? ARCTIC_BATTERY_NOT_CHARGING : ARCTIC_BATTERY_DISCHARGING;
        /* without a power supply in sysfs, the batteries say where the power comes from */
        if (!status->ac_present) status->ac_online = !discharging;
        if (rate_known) status->rate_mw = rate;

        if (rate_known && rate && energy_known && full &&
            (status->state == ARCTIC_BATTERY_DISCHARGING || status->state == ARCTIC_BATTERY_CHARGING))
        {
            double t = now_seconds(), weight;
            long long energy = status->state == ARCTIC_BATTERY_DISCHARGING ? remaining : full - remaining;

            if (!smoothed_at || t - smoothed_at > 300) smoothed_rate = rate;
            else
            {
                weight = min( (t - smoothed_at) / 60.0, 1.0 );
                smoothed_rate += (rate - smoothed_rate) * weight;
            }
            smoothed_at = t;
            if (energy > 0 && smoothed_rate > 0) status->seconds_left = clamp_u32( energy * 3600.0 / smoothed_rate );
        }
        else smoothed_at = 0;
    }
    else status->ac_online = TRUE;

    /* the lid: ACPI's button */
    if ((d = opendir( "/proc/acpi/button/lid" )))
    {
        while ((de = readdir( d )))
        {
            if (de->d_name[0] == '.') continue;
            snprintf( dir, sizeof(dir), "/proc/acpi/button/lid/%s", de->d_name );
            if (read_str( dir, "state", buf, sizeof(buf) ))
            {
                status->lid_present = TRUE;
                if (strstr( buf, "closed" )) status->lid_closed = TRUE;
            }
        }
        closedir( d );
    }

    if (read_str( "/sys/power", "state", buf, sizeof(buf) ) && strstr( buf, "mem" ))
    {
        status->sleep_supported = TRUE;
        status->sleep_deep = read_str( "/sys/power", "mem_sleep", buf, sizeof(buf) ) && strstr( buf, "deep" );
    }
    return STATUS_SUCCESS;
}

/**********************************************************************
 *          Graphics cards
 */

/* NVIDIA's management library, from its driver, when that driver is there.
 * Only the page of the batteries and cards asks it, and lets it go after
 * each look: a process that keeps it open keeps the card awake. */
typedef int nvml_return;
typedef void *nvml_device;
struct nvml_utilization { unsigned int gpu, memory; };
struct nvml_memory { unsigned long long total, free, used; };
struct nvml_process { unsigned int pid; unsigned long long used_memory; unsigned int gpu_instance, compute_instance; };

static struct
{
    BOOL tried, ready;
    nvml_return (*init)(void);
    nvml_return (*shutdown)(void);
    nvml_return (*by_pci)( const char *bus, nvml_device *device );
    nvml_return (*power)( nvml_device device, unsigned int *mw );
    nvml_return (*temperature)( nvml_device device, int sensor, unsigned int *celsius );
    nvml_return (*utilization)( nvml_device device, struct nvml_utilization *util );
    nvml_return (*memory)( nvml_device device, struct nvml_memory *memory );
    nvml_return (*processes)( nvml_device device, unsigned int *count, struct nvml_process *infos );
} nvml;

static BOOL nvml_load(void)
{
    void *lib;

    if (nvml.tried) return nvml.ready && !nvml.init();
    nvml.tried = TRUE;
    if (!(lib = dlopen( "libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL ))) return FALSE;
    nvml.init = dlsym( lib, "nvmlInit_v2" );
    nvml.shutdown = dlsym( lib, "nvmlShutdown" );
    nvml.by_pci = dlsym( lib, "nvmlDeviceGetHandleByPciBusId_v2" );
    nvml.power = dlsym( lib, "nvmlDeviceGetPowerUsage" );
    nvml.temperature = dlsym( lib, "nvmlDeviceGetTemperature" );
    nvml.utilization = dlsym( lib, "nvmlDeviceGetUtilizationRates" );
    nvml.memory = dlsym( lib, "nvmlDeviceGetMemoryInfo" );
    nvml.processes = dlsym( lib, "nvmlDeviceGetGraphicsRunningProcesses_v3" );
    if (!nvml.init || !nvml.shutdown || !nvml.by_pci || nvml.init()) return FALSE;
    nvml.ready = TRUE;
    return TRUE;
}

/* The card's model from the PCI id database, the way Linux distributions
 * ship it ("GP107M [GeForce GTX 1050 Mobile]" is a GeForce GTX 1050
 * Mobile); AMD's by chip and revision from libdrm's table, as its Windows
 * driver names them. The same as dwm's names of the display adapter. */
static BOOL pci_ids_name( unsigned int vendor, unsigned int device, char *name, size_t size )
{
    char line[256], *open, *close;
    BOOL in_vendor = FALSE;
    FILE *f;

    if (!(f = fopen( "/usr/share/hwdata/pci.ids", "re" ))) return FALSE;
    while (fgets( line, sizeof(line), f ))
    {
        if (line[0] == '#' || line[0] == '\n') continue;
        if (line[0] != '\t')
        {
            in_vendor = strtoul( line, NULL, 16 ) == vendor;
            continue;
        }
        if (!in_vendor || line[1] == '\t' || strtoul( line + 1, NULL, 16 ) != device) continue;
        if (!(open = strchr( line, ' ' ))) break;
        while (*open == ' ') open++;
        if ((close = strrchr( open, '\n' ))) *close = 0;
        if ((close = strchr( open, '[' )) && strrchr( close, ']' ))
        {
            open = close + 1;
            *strrchr( open, ']' ) = 0;
        }
        snprintf( name, size, "%s", open );
        fclose( f );
        return TRUE;
    }
    fclose( f );
    return FALSE;
}

static BOOL amdgpu_ids_name( unsigned int device, unsigned int revision, char *name, size_t size )
{
    char line[256], *end, *text;
    FILE *f;

    if (!(f = fopen( "/usr/share/libdrm/amdgpu.ids", "re" ))) return FALSE;
    while (fgets( line, sizeof(line), f ))
    {
        if (line[0] == '#' || strtoul( line, &end, 16 ) != device || *end != ',') continue;
        if (strtoul( end + 1, &end, 16 ) != revision || *end != ',') continue;
        for (text = end + 1; *text == ' ' || *text == '\t'; text++) ;
        text[strcspn( text, "\r\n" )] = 0;
        if (!*text) break;
        snprintf( name, size, "%s", text );
        fclose( f );
        return TRUE;
    }
    fclose( f );
    return FALSE;
}

static void gpu_model( const char *device, struct arctic_gpu *gpu )
{
    char model[96], name[128], buf[16];
    unsigned int revision = read_str( device, "revision", buf, sizeof(buf) ) ? strtoul( buf, NULL, 16 ) : 0;
    const char *maker = gpu->vendor == ARCTIC_GPU_NVIDIA ? "NVIDIA " : gpu->vendor == ARCTIC_GPU_INTEL ? "Intel(R) " :
                        gpu->vendor == ARCTIC_GPU_AMD ? "AMD " : "";

    if (gpu->vendor == ARCTIC_GPU_AMD && amdgpu_ids_name( gpu->device_id, revision, model, sizeof(model) ))
        snprintf( name, sizeof(name), "%s", model );
    else if (pci_ids_name( gpu->vendor_id, gpu->device_id, model, sizeof(model) ))
        snprintf( name, sizeof(name), "%s%s", maker, model );
    else
        snprintf( name, sizeof(name), "%04X:%04X", gpu->vendor_id, gpu->device_id );
    to_wide( gpu->name, ARRAY_SIZE(gpu->name), name );
}

/* a monitor is connected to the card: its connectors, card0-eDP-1 and the like */
static BOOL drm_card_has_monitor( const char *card )
{
    char dir[PATH_MAX], status[32];
    struct dirent *de;
    BOOL ret = FALSE;
    DIR *d;

    snprintf( dir, sizeof(dir), "/sys/class/drm/%s", card );
    if (!(d = opendir( dir ))) return FALSE;
    while (!ret && (de = readdir( d )))
    {
        char path[PATH_MAX];

        if (strncmp( de->d_name, card, strlen( card ) ) || de->d_name[strlen( card )] != '-') continue;
        snprintf( path, sizeof(path), "%s/%s", dir, de->d_name );
        ret = read_str( path, "status", status, sizeof(status) ) && !strcmp( status, "connected" );
    }
    closedir( d );
    return ret;
}

/* energy counters (µJ) turned into power between two readings */
struct energy_meter
{
    char path[PATH_MAX];
    long long last_uj;
    double last_at;
    UINT32 last_mw;
};

static struct energy_meter rapl_package, rapl_uncore;
static BOOL rapl_found;

static UINT32 meter_read( struct energy_meter *meter )
{
    long long uj, range;
    double t;

    if (!meter->path[0] || (uj = read_num( meter->path, "energy_uj" )) < 0) return ARCTIC_UNKNOWN;
    t = now_seconds();
    if (meter->last_at && t - meter->last_at >= 0.2)
    {
        long long delta = uj - meter->last_uj;

        if (delta < 0 && (range = read_num( meter->path, "max_energy_range_uj" )) > 0) delta += range;
        if (delta >= 0) meter->last_mw = clamp_u32( delta / ((t - meter->last_at) * 1000) );
    }
    else if (meter->last_at) return meter->last_mw ? meter->last_mw : ARCTIC_UNKNOWN;
    meter->last_uj = uj;
    meter->last_at = t;
    return meter->last_at && meter->last_mw ? meter->last_mw : ARCTIC_UNKNOWN;
}

/* intel-rapl:0 is the package, its "uncore" child the processor's own graphics */
static void rapl_find(void)
{
    char dir[PATH_MAX], name[32];
    struct dirent *de;
    DIR *d;

    if (rapl_found) return;
    rapl_found = TRUE;
    if (!(d = opendir( "/sys/class/powercap" ))) return;
    while ((de = readdir( d )))
    {
        if (strncmp( de->d_name, "intel-rapl:", 11 )) continue;
        snprintf( dir, sizeof(dir), "/sys/class/powercap/%s", de->d_name );
        if (!read_str( dir, "name", name, sizeof(name) )) continue;
        if (!strncmp( name, "package", 7 ) && !rapl_package.path[0]) strcpy( rapl_package.path, dir );
        else if (!strcmp( name, "uncore" ) && !rapl_uncore.path[0]) strcpy( rapl_uncore.path, dir );
    }
    closedir( d );
}

/* the hwmon of a card: amdgpu, nouveau, radeon have one */
static void read_hwmon( const char *device, struct arctic_gpu *gpu )
{
    char dir[PATH_MAX], hw[PATH_MAX];
    struct dirent *de;
    long long value;
    DIR *d;

    snprintf( dir, sizeof(dir), "%s/hwmon", device );
    if (!(d = opendir( dir ))) return;
    while ((de = readdir( d )))
    {
        if (strncmp( de->d_name, "hwmon", 5 )) continue;
        snprintf( hw, sizeof(hw), "%s/%s", dir, de->d_name );
        if ((value = read_num( hw, "power1_average" )) < 0) value = read_num( hw, "power1_input" );
        if (value >= 0) gpu->power_mw = clamp_u32( value / 1000 );
        if ((value = read_num( hw, "temp1_input" )) >= 0) gpu->temperature_mc = clamp_u32( value );
        break;
    }
    closedir( d );
}

static int pci_of( const char *link, char *pci, size_t size )
{
    char target[PATH_MAX], *base;
    ssize_t n = readlink( link, target, sizeof(target) - 1 );

    if (n <= 0) return 0;
    target[n] = 0;
    base = strrchr( target, '/' );
    snprintf( pci, size, "%s", base ? base + 1 : target );
    return 1;
}

/* NVIDIA's device minor of a card, for /dev/nvidiaN */
static int nvidia_minor( const char *pci )
{
    char dir[PATH_MAX], info[2048], *p;

    snprintf( dir, sizeof(dir), "/proc/driver/nvidia/gpus/%s", pci );
    if (!read_str( dir, "information", info, sizeof(info) )) return -1;
    if (!(p = strstr( info, "Device Minor:" ))) return -1;
    return atoi( p + 13 );
}

static void add_user( struct arctic_gpu *gpu, unsigned int pid )
{
    char dir[64], cmd[512];
    const char *name;
    ssize_t n;
    int fd;

    for (UINT32 i = 0; i < gpu->user_count; i++) if (gpu->users[i].pid == pid) return;
    if (gpu->user_count >= ARCTIC_MAX_GPU_USERS) return;
    snprintf( dir, sizeof(dir), "/proc/%u/cmdline", pid );
    if ((fd = open( dir, O_RDONLY | O_CLOEXEC )) < 0) return;
    n = read( fd, cmd, sizeof(cmd) - 1 );
    close( fd );
    if (n <= 0) return;
    cmd[n] = 0;
    /* Wine shows a process by its Windows path */
    name = strrchr( cmd, '\\' );
    if (!name) name = strrchr( cmd, '/' );
    name = name ? name + 1 : cmd;
    gpu->users[gpu->user_count].pid = pid;
    to_wide( gpu->users[gpu->user_count].image, ARRAY_SIZE(gpu->users[0].image), name );
    if (cmd[1] == ':') to_wide( gpu->users[gpu->user_count].path, ARRAY_SIZE(gpu->users[0].path), cmd );
    else gpu->users[gpu->user_count].path[0] = 0;
    gpu->user_count++;
}

/* which card a DRM node is: renderD128 -> 0000:00:02.0 */
static int drm_node_pci( const char *node, char *pci, size_t size )
{
    char link[PATH_MAX];

    snprintf( link, sizeof(link), "/sys/class/drm/%s/device", node );
    return pci_of( link, pci, size );
}

/* A process uses a card when one of its DRM files did work there (fdinfo:
 * the engines' time, or memory it holds): Mesa's drivers open every card's
 * node to list them, and only that is no use. NVIDIA's, when NVML cannot
 * say, by its device file. Engine time in the last interval gives how busy
 * a card is, as Task Manager counts it. */
struct engine_sample { unsigned int pid; int fd; unsigned long long ns; };
static struct engine_sample *last_samples;
static size_t last_sample_count;
static double last_scan_at;

static void scan_users( struct arctic_gpu_list *list, BOOL nvml_users[] )
{
    struct engine_sample *samples = NULL;
    size_t sample_count = 0, sample_max = 0;
    unsigned long long busy_ns[ARCTIC_MAX_GPUS] = { 0 };
    double t = now_seconds();
    struct dirent *de, *fe;
    DIR *proc, *fds;

    if (!(proc = opendir( "/proc" ))) return;
    while ((de = readdir( proc )))
    {
        char fd_dir[64], link[PATH_MAX], target[256], info_path[96], info[1024];
        unsigned int pid;
        char *end;

        pid = strtoul( de->d_name, &end, 10 );
        if (*end || !pid) continue;
        snprintf( fd_dir, sizeof(fd_dir), "/proc/%u/fd", pid );
        if (!(fds = opendir( fd_dir ))) continue;
        while ((fe = readdir( fds )))
        {
            char pci[32] = "";
            int node_minor, card = -1, fd;
            ssize_t n;

            if (fe->d_name[0] == '.') continue;
            snprintf( link, sizeof(link), "%s/%s", fd_dir, fe->d_name );
            if ((n = readlink( link, target, sizeof(target) - 1 )) <= 0) continue;
            target[n] = 0;
            fd = atoi( fe->d_name );

            if (!strncmp( target, "/dev/nvidia", 11 ) && sscanf( target + 11, "%d", &node_minor ) == 1)
            {
                for (UINT32 i = 0; i < list->count; i++)
                {
                    char gpu_pci[32];

                    if (list->gpus[i].vendor != ARCTIC_GPU_NVIDIA || nvml_users[i]) continue;
                    to_narrow( gpu_pci, sizeof(gpu_pci), list->gpus[i].pci );
                    if (nvidia_minor( gpu_pci ) == node_minor) add_user( &list->gpus[i], pid );
                }
                continue;
            }
            if (strncmp( target, "/dev/dri/", 9 ) || !drm_node_pci( target + 9, pci, sizeof(pci) )) continue;
            for (UINT32 i = 0; i < list->count; i++)
            {
                char gpu_pci[32];

                to_narrow( gpu_pci, sizeof(gpu_pci), list->gpus[i].pci );
                if (!strcmp( gpu_pci, pci )) card = i;
            }
            if (card < 0 || list->gpus[card].vendor == ARCTIC_GPU_NVIDIA) continue;

            snprintf( info_path, sizeof(info_path), "/proc/%u/fdinfo", pid );
            if (read_str( info_path, fe->d_name, info, sizeof(info) ))
            {
                unsigned long long ns = 0, value;
                BOOL used = FALSE;
                char *p = info;

                while ((p = strstr( p, "drm-" )))
                {
                    char key[48];
                    if (sscanf( p, "drm-%47[^:]: %llu", key, &value ) == 2)
                    {
                        if (!strncmp( key, "engine-", 7 ) && strncmp( key, "engine-capacity", 15 )) ns += value;
                        if ((!strncmp( key, "engine-", 7 ) || !strncmp( key, "resident", 8 ) ||
                             !strncmp( key, "total", 5 )) && value) used = TRUE;
                    }
                    p += 4;
                }
                if (!used) continue;
                add_user( &list->gpus[card], pid );

                /* engine time since the last scan */
                if (sample_count == sample_max)
                {
                    sample_max = sample_max ? sample_max * 2 : 64;
                    samples = realloc( samples, sample_max * sizeof(*samples) );
                }
                samples[sample_count].pid = pid;
                samples[sample_count].fd = fd;
                samples[sample_count].ns = ns;
                for (size_t k = 0; k < last_sample_count; k++)
                    if (last_samples[k].pid == pid && last_samples[k].fd == fd && ns >= last_samples[k].ns)
                        busy_ns[card] += ns - last_samples[k].ns;
                sample_count++;
            }
        }
        closedir( fds );
    }
    closedir( proc );

    if (last_scan_at && t > last_scan_at)
        for (UINT32 i = 0; i < list->count; i++)
            if (list->gpus[i].vendor != ARCTIC_GPU_NVIDIA)
                list->gpus[i].busy_percent = min( 100, (UINT32)(busy_ns[i] / ((t - last_scan_at) * 1e7)) );
    free( last_samples );
    last_samples = samples;
    last_sample_count = sample_count;
    last_scan_at = t;
}

static NTSTATUS gpu_list( void *args )
{
    struct gpu_list_params *params = args;
    struct arctic_gpu_list *list = params->list;
    BOOL nvml_users[ARCTIC_MAX_GPUS] = { 0 };
    struct dirent *de;
    DIR *d;

    memset( list, 0, sizeof(*list) );
    if (!(d = opendir( "/sys/class/drm" ))) return STATUS_SUCCESS;
    while ((de = readdir( d )) && list->count < ARCTIC_MAX_GPUS)
    {
        char device[PATH_MAX], link[PATH_MAX], buf[64], pci[32];
        struct arctic_gpu *gpu = &list->gpus[list->count];
        const char *p;
        long long value;

        /* card0, not card0-eDP-1: one entry a card */
        if (strncmp( de->d_name, "card", 4 )) continue;
        for (p = de->d_name + 4; *p && *p >= '0' && *p <= '9'; p++) ;
        if (*p || p == de->d_name + 4) continue;

        snprintf( device, sizeof(device), "/sys/class/drm/%s/device", de->d_name );
        /* simpledrm and other cards without a PCI device are no graphics card */
        if ((value = read_num( device, "vendor" )) < 0) continue;
        snprintf( link, sizeof(link), "/sys/class/drm/%s/device", de->d_name );
        if (!pci_of( link, pci, sizeof(pci) ) || !strchr( pci, ':' )) continue;

        memset( gpu, 0, sizeof(*gpu) );
        to_wide( gpu->pci, ARRAY_SIZE(gpu->pci), pci );
        if (read_str( device, "vendor", buf, sizeof(buf) )) gpu->vendor_id = strtoul( buf, NULL, 16 );
        if (read_str( device, "device", buf, sizeof(buf) )) gpu->device_id = strtoul( buf, NULL, 16 );
        if (read_str( device, "subsystem_vendor", buf, sizeof(buf) )) gpu->subsys_vendor_id = strtoul( buf, NULL, 16 );
        if (read_str( device, "subsystem_device", buf, sizeof(buf) )) gpu->subsys_id = strtoul( buf, NULL, 16 );
        snprintf( link, sizeof(link), "%s/driver", device );
        if (pci_of( link, buf, sizeof(buf) )) to_wide( gpu->driver, ARRAY_SIZE(gpu->driver), buf );
        gpu->vendor = gpu->vendor_id == 0x8086 ? ARCTIC_GPU_INTEL : gpu->vendor_id == 0x1002 ? ARCTIC_GPU_AMD
                    : gpu->vendor_id == 0x10de ? ARCTIC_GPU_NVIDIA : ARCTIC_GPU_OTHER;
        gpu->integrated = read_num( device, "boot_vga" ) == 1;
        gpu->display = drm_card_has_monitor( de->d_name );
        gpu_model( device, gpu );
        gpu->power_mw = gpu->temperature_mc = gpu->busy_percent = ARCTIC_UNKNOWN;
        gpu->memory_used_mb = gpu->memory_total_mb = ARCTIC_UNKNOWN;

        /* asleep: runtime power management suspended it, or it is in D3 */
        gpu->active = TRUE;
        if (read_str( device, "power/runtime_status", buf, sizeof(buf) ) && !strcmp( buf, "suspended" )) gpu->active = FALSE;
        if (read_str( device, "power_state", buf, sizeof(buf) ) && !strncmp( buf, "D3", 2 )) gpu->active = FALSE;

        if (gpu->active) read_hwmon( device, gpu );
        if ((value = read_num( device, "gpu_busy_percent" )) >= 0) gpu->busy_percent = min( value, 100 );
        if ((value = read_num( device, "mem_info_vram_used" )) >= 0) gpu->memory_used_mb = clamp_u32( value >> 20 );
        if ((value = read_num( device, "mem_info_vram_total" )) >= 0) gpu->memory_total_mb = clamp_u32( value >> 20 );

        /* a card asleep is not woken to be asked: NVML would wake it */
        if (gpu->vendor == ARCTIC_GPU_NVIDIA && gpu->active && params->with_users && nvml_load())
        {
            char bus[32];
            nvml_device handle;
            unsigned int mw, celsius;
            struct nvml_utilization util;
            struct nvml_memory mem;

            snprintf( bus, sizeof(bus), "%s", pci );
            if (!nvml.by_pci( bus, &handle ))
            {
                if (nvml.power && !nvml.power( handle, &mw )) gpu->power_mw = mw;
                if (nvml.temperature && !nvml.temperature( handle, 0, &celsius )) gpu->temperature_mc = celsius * 1000;
                if (nvml.utilization && !nvml.utilization( handle, &util )) gpu->busy_percent = util.gpu;
                if (nvml.memory && !nvml.memory( handle, &mem ))
                {
                    gpu->memory_used_mb = mem.used >> 20;
                    gpu->memory_total_mb = mem.total >> 20;
                }
                if (params->with_users && nvml.processes)
                {
                    struct nvml_process procs[ARCTIC_MAX_GPU_USERS];
                    unsigned int count = ARCTIC_MAX_GPU_USERS;

                    if (!nvml.processes( handle, &count, procs ))
                    {
                        for (unsigned int i = 0; i < count; i++) add_user( gpu, procs[i].pid );
                        nvml_users[list->count] = TRUE;
                    }
                }
            }
            nvml.shutdown();
        }
        /* the processor's own graphics: its share of the package (RAPL "uncore") */
        if (gpu->vendor == ARCTIC_GPU_INTEL && gpu->integrated && gpu->power_mw == ARCTIC_UNKNOWN)
        {
            rapl_find();
            gpu->power_mw = meter_read( &rapl_uncore );
        }
        list->count++;
    }
    closedir( d );

    /* the card a laptop's panel hangs on: when none says boot_vga, the
     * Intel or AMD one beside a dedicated card */
    {
        BOOL any = FALSE;
        for (UINT32 i = 0; i < list->count; i++) any |= list->gpus[i].integrated;
        if (!any && list->count > 1)
            for (UINT32 i = 0; i < list->count; i++)
                if (list->gpus[i].vendor == ARCTIC_GPU_INTEL) { list->gpus[i].integrated = TRUE; break; }
    }

    if (params->with_users) scan_users( list, nvml_users );
    return STATUS_SUCCESS;
}

/**********************************************************************
 *          The processor, the profile, the panel
 */

#define CPU_DIR "/sys/devices/system/cpu"

static void scaling_driver( char *buf, size_t size )
{
    if (!read_str( CPU_DIR "/cpu0/cpufreq", "scaling_driver", buf, size )) buf[0] = 0;
}

/* the backlight that really moves the panel: firmware, then platform, then raw */
static BOOL find_backlight( char *dir, size_t size )
{
    static const char *order[] = { "firmware", "platform", "raw" };
    char path[PATH_MAX], type[32];
    struct dirent *de;
    DIR *d;

    for (int k = 0; k < 3; k++)
    {
        if (!(d = opendir( "/sys/class/backlight" ))) return FALSE;
        while ((de = readdir( d )))
        {
            if (de->d_name[0] == '.') continue;
            snprintf( path, sizeof(path), "/sys/class/backlight/%s", de->d_name );
            if (read_str( path, "type", type, sizeof(type) ) && !strcmp( type, order[k] ) &&
                read_num( path, "max_brightness" ) > 0)
            {
                snprintf( dir, size, "%s", path );
                closedir( d );
                return TRUE;
            }
        }
        closedir( d );
    }
    return FALSE;
}

static NTSTATUS cpu_info( void *args )
{
    struct arctic_cpu_info *info = args;
    char driver[32], buf[256], dir[PATH_MAX];

    memset( info, 0, sizeof(*info) );
    scaling_driver( driver, sizeof(driver) );
    if (!strcmp( driver, "intel_pstate" ) || !strcmp( driver, "intel_cpufreq" )) info->driver = ARCTIC_CPU_INTEL_PSTATE;
    else if (!strncmp( driver, "amd-pstate", 10 )) info->driver = ARCTIC_CPU_AMD_PSTATE;
    else if (driver[0]) info->driver = ARCTIC_CPU_OTHER;
    info->epp = !access( CPU_DIR "/cpu0/cpufreq/energy_performance_preference", W_OK );
    info->boost = !access( CPU_DIR "/intel_pstate/no_turbo", W_OK ) || !access( CPU_DIR "/cpufreq/boost", W_OK );
    if (read_str( "/sys/firmware/acpi", "platform_profile_choices", buf, sizeof(buf) ))
    {
        info->platform_profile = !access( "/sys/firmware/acpi/platform_profile", W_OK );
        to_wide( info->profiles, ARRAY_SIZE(info->profiles), buf );
    }
    if (find_backlight( dir, sizeof(dir) ))
    {
        long long max = read_num( dir, "max_brightness" ), cur = read_num( dir, "brightness" );

        info->backlight = TRUE;
        if (max > 0 && cur >= 0) info->brightness_percent = (cur * 100 + max / 2) / max;
    }
    rapl_find();
    info->package_power_mw = meter_read( &rapl_package );
    return STATUS_SUCCESS;
}

/* Windows' energy preference, 0 (performance) to 100 (power saving), as
 * the kernel takes it: intel_pstate takes the hardware's own 0-255, the
 * others a name */
static BOOL set_epp( UINT32 epp, const char *driver )
{
    char policy[PATH_MAX], value[32];
    struct dirent *de;
    BOOL ok = TRUE, any = FALSE;
    DIR *d;

    if (!strcmp( driver, "intel_pstate" )) snprintf( value, sizeof(value), "%u", min( epp, 100 ) * 255 / 100 );
    else snprintf( value, sizeof(value), "%s", epp <= 10 ? "performance" : epp <= 40 ? "balance_performance"
                                               : epp <= 75 ? "balance_power" : "power" );
    if (!(d = opendir( CPU_DIR "/cpufreq" ))) return FALSE;
    while ((de = readdir( d )))
    {
        if (strncmp( de->d_name, "policy", 6 )) continue;
        snprintf( policy, sizeof(policy), CPU_DIR "/cpufreq/%s", de->d_name );
        if (access( policy, F_OK )) continue;
        any = TRUE;
        /* the "performance" governor pins the preference at performance */
        if (!strcmp( driver, "intel_pstate" ) || !strncmp( driver, "amd-pstate", 10 ))
        {
            char governor[32];
            if (read_str( policy, "scaling_governor", governor, sizeof(governor) ) && strcmp( governor, "powersave" ))
                write_str( policy, "scaling_governor", "powersave" );
        }
        if (!write_str( policy, "energy_performance_preference", value ) && strcmp( driver, "intel_pstate" ) == 0)
        {
            /* an intel_pstate without numbers: the name */
            const char *name = epp <= 10 ? "performance" : epp <= 40 ? "balance_performance" : epp <= 75 ? "balance_power" : "power";
            ok &= write_str( policy, "energy_performance_preference", name );
        }
    }
    closedir( d );
    return ok && any;
}

/* without an energy preference: the governor */
static BOOL set_governor( UINT32 epp )
{
    char policy[PATH_MAX], available[256];
    struct dirent *de;
    BOOL ok = TRUE;
    DIR *d;

    if (!(d = opendir( CPU_DIR "/cpufreq" ))) return FALSE;
    while ((de = readdir( d )))
    {
        const char *want;

        if (strncmp( de->d_name, "policy", 6 )) continue;
        snprintf( policy, sizeof(policy), CPU_DIR "/cpufreq/%s", de->d_name );
        if (!read_str( policy, "scaling_available_governors", available, sizeof(available) )) continue;
        if (epp <= 10 && strstr( available, "performance" )) want = "performance";
        else if (epp >= 90 && strstr( available, "powersave" )) want = "powersave";
        else if (strstr( available, "schedutil" )) want = "schedutil";
        else if (strstr( available, "ondemand" )) want = "ondemand";
        else continue;
        ok &= write_str( policy, "scaling_governor", want );
    }
    closedir( d );
    return ok;
}

static BOOL set_limit( UINT32 percent, BOOL maximum, const char *driver )
{
    char policy[PATH_MAX];
    struct dirent *de;
    BOOL ok = TRUE;
    DIR *d;

    percent = min( max( percent, 1 ), 100 );
    if (!access( CPU_DIR "/intel_pstate", F_OK ) && !strncmp( driver, "intel_", 6 ))
        return write_num( CPU_DIR "/intel_pstate", maximum ? "max_perf_pct" : "min_perf_pct", percent );

    if (!(d = opendir( CPU_DIR "/cpufreq" ))) return FALSE;
    while ((de = readdir( d )))
    {
        long long lo, hi, freq;

        if (strncmp( de->d_name, "policy", 6 )) continue;
        snprintf( policy, sizeof(policy), CPU_DIR "/cpufreq/%s", de->d_name );
        lo = read_num( policy, "cpuinfo_min_freq" );
        hi = read_num( policy, "cpuinfo_max_freq" );
        if (lo <= 0 || hi <= 0) continue;
        freq = max( lo, hi * percent / 100 );
        ok &= write_num( policy, maximum ? "scaling_max_freq" : "scaling_min_freq", freq );
    }
    closedir( d );
    return ok;
}

static BOOL set_profile( UINT32 profile )
{
    static const char *wanted[3][3] =
    {
        { "low-power", "quiet", "cool" },
        { "balanced", "balanced-performance", NULL },
        { "performance", "balanced-performance", NULL },
    };
    char choices[256];

    if (!read_str( "/sys/firmware/acpi", "platform_profile_choices", choices, sizeof(choices) )) return TRUE;
    for (int i = 0; i < 3 && wanted[min( profile, 2 )][i]; i++)
    {
        const char *name = wanted[min( profile, 2 )][i];
        char *p = strstr( choices, name );
        size_t len = strlen( name );

        if (p && (p == choices || p[-1] == ' ') && (!p[len] || p[len] == ' '))
            return write_str( "/sys/firmware/acpi", "platform_profile", name );
    }
    return TRUE;
}

/* USB selective suspend: every device but those people touch (HID), as
 * Windows suspends only what its driver allows */
static BOOL set_usb_autosuspend( BOOL on )
{
    char dev[PATH_MAX], intf[PATH_MAX], cls[16];
    struct dirent *de, *ie;
    BOOL ok = TRUE;
    DIR *d, *id;

    if (!(d = opendir( "/sys/bus/usb/devices" ))) return FALSE;
    while ((de = readdir( d )))
    {
        BOOL hid = FALSE;

        if (de->d_name[0] == '.' || strchr( de->d_name, ':' )) continue;
        snprintf( dev, sizeof(dev), "/sys/bus/usb/devices/%s", de->d_name );
        if (access( dev, F_OK ) || read_num( dev, "bDeviceClass" ) == 9) continue;   /* hubs keep theirs */
        if ((id = opendir( dev )))
        {
            while ((ie = readdir( id )))
            {
                if (!strchr( ie->d_name, ':' )) continue;
                snprintf( intf, sizeof(intf), "%s/%s", dev, ie->d_name );
                if (read_str( intf, "bInterfaceClass", cls, sizeof(cls) ) && !strcmp( cls, "03" )) hid = TRUE;
            }
            closedir( id );
        }
        if (hid) continue;
        if (access( dev, F_OK ) == 0)
        {
            char control[PATH_MAX];
            snprintf( control, sizeof(control), "%s/power", dev );
            if (!access( control, F_OK )) ok &= write_str( control, "control", on ? "auto" : "on" );
        }
    }
    closedir( d );
    return ok;
}

static NTSTATUS power_apply( void *args )
{
    struct arctic_power_apply *apply = args;
    char driver[32], dir[PATH_MAX];

    apply->failed = 0;
    scaling_driver( driver, sizeof(driver) );

    if (apply->epp != ARCTIC_UNKNOWN)
    {
        if (!access( CPU_DIR "/cpu0/cpufreq/energy_performance_preference", F_OK ))
        {
            if (!set_epp( apply->epp, driver )) apply->failed |= 1 << 0;
        }
        else if (driver[0] && !set_governor( apply->epp )) apply->failed |= 1 << 0;
    }
    if (apply->max_percent != ARCTIC_UNKNOWN && driver[0] && !set_limit( apply->max_percent, TRUE, driver ))
        apply->failed |= 1 << 1;
    if (apply->min_percent != ARCTIC_UNKNOWN && driver[0] && !set_limit( apply->min_percent, FALSE, driver ))
        apply->failed |= 1 << 2;
    if (apply->boost != ARCTIC_UNKNOWN)
    {
        BOOL ok = TRUE;
        if (!access( CPU_DIR "/intel_pstate/no_turbo", F_OK )) ok = write_num( CPU_DIR "/intel_pstate", "no_turbo", !apply->boost );
        else if (!access( CPU_DIR "/cpufreq/boost", F_OK )) ok = write_num( CPU_DIR "/cpufreq", "boost", !!apply->boost );
        if (!ok) apply->failed |= 1 << 3;
    }
    if (apply->profile != ARCTIC_UNKNOWN && !set_profile( apply->profile )) apply->failed |= 1 << 4;
    if (apply->aspm != ARCTIC_UNKNOWN && !access( "/sys/module/pcie_aspm/parameters/policy", F_OK ))
    {
        static const char *policies[] = { "performance", "powersave", "powersupersave" };
        /* the firmware may keep ASPM for itself: then nothing can change it */
        if (!write_str( "/sys/module/pcie_aspm/parameters", "policy", policies[min( apply->aspm, 2 )] ))
            apply->failed |= 1 << 5;
    }
    if (apply->usb_autosuspend != ARCTIC_UNKNOWN && !set_usb_autosuspend( apply->usb_autosuspend ))
        apply->failed |= 1 << 6;
    if (apply->brightness_percent != ARCTIC_UNKNOWN && find_backlight( dir, sizeof(dir) ))
    {
        long long max = read_num( dir, "max_brightness" );
        long long value = (max * min( apply->brightness_percent, 100 ) + 50) / 100;

        /* 0% is the dimmest Windows goes, not a dark panel */
        if (!write_num( dir, "brightness", max( value, 1 ) )) apply->failed |= 1 << 7;
    }
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    power_status,
    gpu_list,
    cpu_info,
    power_apply,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_funcs) == unix_funcs_count );

#ifdef _WIN64

static NTSTATUS wow64_gpu_list( void *args )
{
    struct { ULONG list; UINT32 with_users; } const *params32 = args;
    struct gpu_list_params params = { ULongToPtr( params32->list ), params32->with_users };

    return gpu_list( &params );
}

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    power_status,
    wow64_gpu_list,
    cpu_info,
    power_apply,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_wow64_funcs) == unix_funcs_count );

#endif
