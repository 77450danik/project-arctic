/*
 * Arctic: the machine's power, as powrprof.dll reads and sets it
 * (docs/power.md)
 *
 * The batteries, the power supply, the graphics cards and the processor
 * come from the kernel (sysfs, NVML for NVIDIA's cards) through powrprof's
 * unix side, whatever the laptop has: the notification area (batmeter.dll),
 * the Control Panel (powercpl.dll) and the power policy (winlogon) all read
 * them here. Every field is 32 bits wide, so 32-bit programs see the same.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_ARCTIC_POWER_H
#define __WINE_ARCTIC_POWER_H

#define ARCTIC_MAX_BATTERIES  4
#define ARCTIC_MAX_GPUS       4
#define ARCTIC_MAX_GPU_USERS  24

#define ARCTIC_UNKNOWN        0xffffffffu

enum arctic_battery_state
{
    ARCTIC_BATTERY_UNKNOWN,
    ARCTIC_BATTERY_DISCHARGING,
    ARCTIC_BATTERY_CHARGING,
    ARCTIC_BATTERY_NOT_CHARGING,   /* on the mains, held below full (a charge limit) */
    ARCTIC_BATTERY_FULL,
};

struct arctic_battery
{
    WCHAR  name[32];               /* the kernel's: BAT0, BAT1, CMB0... */
    WCHAR  manufacturer[32];
    WCHAR  model[48];
    WCHAR  chemistry[16];          /* Li-ion, LiP... */
    UINT32 state;                  /* enum arctic_battery_state */
    UINT32 percent;                /* 0-100 */
    UINT32 remaining_mwh;          /* ARCTIC_UNKNOWN when the battery does not say */
    UINT32 full_mwh;
    UINT32 design_mwh;
    UINT32 rate_mw;                /* charging or discharging, ARCTIC_UNKNOWN */
    UINT32 voltage_mv;
    UINT32 cycles;                 /* ARCTIC_UNKNOWN (many say 0: not counted) */
};

struct arctic_power_status
{
    UINT32 ac_online;              /* on the mains (or USB-C power) */
    UINT32 ac_present;             /* a power supply the machine reports at all */
    UINT32 battery_count;
    UINT32 percent;                /* all batteries as one, ARCTIC_UNKNOWN without */
    UINT32 state;                  /* all of them: enum arctic_battery_state */
    UINT32 rate_mw;                /* what the machine draws from the batteries or puts in */
    UINT32 seconds_left;           /* to empty, or to full while charging; ARCTIC_UNKNOWN */
    UINT32 lid_present;
    UINT32 lid_closed;
    UINT32 sleep_supported;        /* suspend to memory: deep or s2idle */
    UINT32 sleep_deep;             /* S3 (deep), not only s2idle */
    struct arctic_battery batteries[ARCTIC_MAX_BATTERIES];
};

enum arctic_gpu_vendor { ARCTIC_GPU_OTHER, ARCTIC_GPU_INTEL, ARCTIC_GPU_AMD, ARCTIC_GPU_NVIDIA };

struct arctic_gpu_user
{
    UINT32 pid;                    /* unix */
    WCHAR  image[64];              /* chrome.exe */
    WCHAR  path[200];              /* C:\Program Files\...\chrome.exe, when known */
};

struct arctic_gpu
{
    WCHAR  pci[16];                /* 0000:01:00.0 */
    WCHAR  name[128];              /* as Windows names it: the adapter's DeviceDesc, else the PCI id database's */
    WCHAR  driver[16];             /* i915, xe, amdgpu, nvidia, nouveau... */
    UINT32 vendor_id, device_id, subsys_vendor_id, subsys_id;
    UINT32 vendor;                 /* enum arctic_gpu_vendor */
    UINT32 integrated;             /* the one the firmware showed the boot on, and the laptop's panel */
    UINT32 active;                 /* awake (runtime PM active / D0); 0: asleep */
    UINT32 display;                /* a monitor is connected to it */
    UINT32 power_mw;               /* ARCTIC_UNKNOWN */
    UINT32 temperature_mc;         /* millidegrees, ARCTIC_UNKNOWN */
    UINT32 busy_percent;           /* ARCTIC_UNKNOWN */
    UINT32 memory_used_mb, memory_total_mb; /* ARCTIC_UNKNOWN */
    UINT32 user_count;
    struct arctic_gpu_user users[ARCTIC_MAX_GPU_USERS];
};

struct arctic_gpu_list
{
    UINT32 count;
    struct arctic_gpu gpus[ARCTIC_MAX_GPUS];
};

enum arctic_cpu_driver { ARCTIC_CPU_NONE, ARCTIC_CPU_INTEL_PSTATE, ARCTIC_CPU_AMD_PSTATE, ARCTIC_CPU_OTHER };

struct arctic_cpu_info
{
    UINT32 driver;                 /* enum arctic_cpu_driver */
    UINT32 epp;                    /* energy_performance_preference can be set */
    UINT32 boost;                  /* turbo can be switched */
    UINT32 platform_profile;       /* /sys/firmware/acpi/platform_profile is there */
    WCHAR  profiles[64];           /* its choices, as the firmware lists them */
    UINT32 backlight;              /* a panel whose brightness can be set */
    UINT32 brightness_percent;
    UINT32 package_power_mw;       /* the processor package (RAPL), ARCTIC_UNKNOWN */
};

/* what the power policy puts on the hardware; a field at ARCTIC_UNKNOWN stays as it is */
struct arctic_power_apply
{
    UINT32 epp;                    /* 0 (performance) - 100 (power saving), Windows' scale */
    UINT32 max_percent;            /* processor maximum state */
    UINT32 min_percent;            /* processor minimum state */
    UINT32 boost;                  /* 0 off, 1 on */
    UINT32 profile;                /* 0 low-power, 1 balanced, 2 performance */
    UINT32 aspm;                   /* 0 off (performance), 1 moderate, 2 maximum */
    UINT32 usb_autosuspend;        /* 0 off, 1 on */
    UINT32 brightness_percent;
    UINT32 failed;                 /* out: the fields that could not be set, one bit each in order */
};

/* powrprof.dll's power scheme API that Wine's powrprof.h does not declare */
DWORD WINAPI PowerGetActiveScheme( HKEY root, GUID **scheme );
DWORD WINAPI PowerSetActiveScheme( HKEY root, GUID *scheme );
DWORD WINAPI PowerReadACValueIndex( HKEY root, const GUID *scheme, const GUID *sub, const GUID *setting, DWORD *index );
DWORD WINAPI PowerReadDCValueIndex( HKEY root, const GUID *scheme, const GUID *sub, const GUID *setting, DWORD *index );
DWORD WINAPI PowerWriteDCValueIndex( HKEY root, const GUID *scheme, const GUID *sub, const GUID *setting, DWORD index );
DWORD WINAPI PowerReadACDefaultIndex( HKEY root, const GUID *scheme, const GUID *sub, const GUID *setting, DWORD *index );
DWORD WINAPI PowerReadDCDefaultIndex( HKEY root, const GUID *scheme, const GUID *sub, const GUID *setting, DWORD *index );
DWORD WINAPI PowerReadFriendlyName( HKEY root, const GUID *scheme, const GUID *sub, const GUID *setting, UCHAR *buffer, DWORD *size );
DWORD WINAPI PowerWriteFriendlyName( HKEY root, const GUID *scheme, const GUID *sub, const GUID *setting, UCHAR *buffer, DWORD size );
DWORD WINAPI PowerReadDescription( HKEY root, const GUID *scheme, const GUID *sub, const GUID *setting, UCHAR *buffer, DWORD *size );
DWORD WINAPI PowerReadPossibleValue( HKEY root, const GUID *sub, const GUID *setting, ULONG *type, ULONG index, UCHAR *buffer, DWORD *size );
DWORD WINAPI PowerReadPossibleFriendlyName( HKEY root, const GUID *sub, const GUID *setting, ULONG index, UCHAR *buffer, DWORD *size );
DWORD WINAPI PowerReadPossibleDescription( HKEY root, const GUID *sub, const GUID *setting, ULONG index, UCHAR *buffer, DWORD *size );
DWORD WINAPI PowerReadValueMin( HKEY root, const GUID *sub, const GUID *setting, DWORD *value );
DWORD WINAPI PowerReadValueMax( HKEY root, const GUID *sub, const GUID *setting, DWORD *value );
DWORD WINAPI PowerReadValueIncrement( HKEY root, const GUID *sub, const GUID *setting, DWORD *value );
DWORD WINAPI PowerReadValueUnitsSpecifier( HKEY root, const GUID *sub, const GUID *setting, UCHAR *buffer, DWORD *size );
DWORD WINAPI PowerReadSettingAttributes( const GUID *sub, const GUID *setting );
DWORD WINAPI PowerDuplicateScheme( HKEY root, const GUID *source, GUID **destination );
DWORD WINAPI PowerDeleteScheme( HKEY root, const GUID *scheme );
DWORD WINAPI PowerRestoreIndividualDefaultPowerScheme( const GUID *scheme );
DWORD WINAPI PowerCanRestoreIndividualDefaultPowerScheme( const GUID *scheme );
DWORD WINAPI PowerGetEffectiveOverlayScheme( GUID *overlay );
DWORD WINAPI PowerSetActiveOverlayScheme( GUID *overlay );

/* exports of powrprof.dll */
BOOL WINAPI ArcticPowerStatus( struct arctic_power_status *status );
BOOL WINAPI ArcticGpuList( struct arctic_gpu_list *list, BOOL with_users );
/* what each graphics card a program can be given needs in its environment (ARCTIC_GPU_KEY) */
BOOL WINAPI ArcticGpuPublish(void);
BOOL WINAPI ArcticCpuInfo( struct arctic_cpu_info *info );
BOOL WINAPI ArcticPowerApply( struct arctic_power_apply *apply );
/* "sleep", "hibernate" or "wifi-powersave on|off": asks the host, waits for its answer */
BOOL WINAPI ArcticPowerRequest( const char *request );

/* HKLM\SYSTEM\CurrentControlSet\Control\Power: what the power policy keeps */
#define ARCTIC_POWER_KEY         L"SYSTEM\\CurrentControlSet\\Control\\Power"
#define ARCTIC_ENERGY_SAVER_ON   L"ArcticEnergySaverOn"      /* DWORD: battery saver is on now */
#define ARCTIC_ENERGY_SAVER_HAND L"ArcticEnergySaverManual"  /* DWORD: turned on by hand (stays until charging) */

/* What programs asked of the power policy: SetThreadExecutionState and power
 * requests (a video keeps the display on, a download keeps the machine
 * awake). kernelbase keeps one slot a process here; the policy reads them. */
#define ARCTIC_EXECUTION_STATE_NAME  L"Local\\ArcticExecutionState"
#define ARCTIC_EXECUTION_STATE_SLOTS 128

struct arctic_execution_state
{
    struct
    {
        LONG   pid;                /* 0: free */
        LONG   display;            /* the display stays on: ES_CONTINUOUS and power requests */
        LONG   system;             /* the machine stays awake */
        UINT32 ping;               /* GetTickCount of a call without ES_CONTINUOUS: inactivity starts again */
    } slots[ARCTIC_EXECUTION_STATE_SLOTS];
};

/* The graphics card a program runs on (Windows' "Graphics settings",
 * HKCU\Software\Microsoft\DirectX\UserGpuPreferences: "GpuPreference=N;",
 * 0 let Windows decide, 1 power saving, 2 high performance). The power policy
 * keeps here, volatile, what each choice puts in a program's environment
 * (REG_MULTI_SZ of NAME=VALUE; NAME= removes it) and the cards' names;
 * nothing when the machine has one card. kernelbase's CreateProcess adds it:
 * "Default" (the card the monitors hang on) for Windows' programs and the
 * others, the fast card for a game when Windows decides. */
#define ARCTIC_GPU_KEY            L"SYSTEM\\CurrentControlSet\\Control\\Power\\ArcticGpu"
#define ARCTIC_GPU_DEFAULT        L"Default"
#define ARCTIC_GPU_POWER_SAVING   L"PowerSaving"
#define ARCTIC_GPU_HIGH_PERF      L"HighPerformance"
#define ARCTIC_GPU_POWER_SAVING_NAME L"PowerSavingName"
#define ARCTIC_GPU_HIGH_PERF_NAME L"HighPerformanceName"

/* winsrv signals it on every input from the user's devices: the power
 * policy's inactivity, as win32k tells Windows' power manager */
#define ARCTIC_USER_INPUT_EVENT     L"Global\\ArcticUserInput"

/* registered messages between the parts */
#define ARCTIC_POWER_POLICY_CLASS   L"ArcticPowerPolicy"
/* winsrv -> policy: the buttons, as events (the policy's window is another
 * process's, which FindWindow does not see) */
#define ARCTIC_POWER_BUTTON_EVENT   L"Global\\ArcticPowerButton"
#define ARCTIC_SLEEP_BUTTON_EVENT   L"Global\\ArcticSleepButton"

/* the shared history of what the batteries gave, for the Control Panel's graph */
#define ARCTIC_POWER_HISTORY_NAME L"Local\\ArcticPowerHistory"
#define ARCTIC_POWER_HISTORY_SIZE 360     /* one sample each 10 s: an hour */

struct arctic_power_history
{
    UINT32 next;                   /* where the next sample goes */
    UINT32 count;
    LONG   warning_serial;         /* one more for every battery warning (batmeter shows it) */
    UINT32 warning_level;          /* 1 low, 2 reserve */
    struct
    {
        UINT32 tick;               /* GetTickCount of the sample */
        UINT32 rate_mw;            /* ARCTIC_UNKNOWN */
        UINT32 percent;
        UINT32 ac_online;
    } samples[ARCTIC_POWER_HISTORY_SIZE];
};

#endif
