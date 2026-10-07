/*
 * Windows Logon Application: the power policy
 *
 * What Windows' power manager does for the session (docs/power.md), on a
 * thread of winlogon, which lives as long as the session:
 * - the active power scheme, with the power slider's overlay over Balanced,
 *   goes to the hardware for the power source now (the processor's energy
 *   preference and limits, turbo, the platform profile, PCI Express and USB
 *   power saving, the Wi-Fi adapter's, the panel's brightness), again
 *   whenever the scheme, the source or battery saver changes;
 * - inactivity dims the display, turns it off (dwm.exe), puts the machine
 *   to sleep or into hibernation, unless a program asked to keep them on;
 * - the lid, the power and the sleep button do what the scheme says;
 * - the low and the reserve battery levels warn, the critical one acts;
 * - battery saver turns on at its level on battery and off on the mains;
 * - programs hear of it all through WM_POWERBROADCAST;
 * - what the batteries gave over the last hour stays for the Control Panel.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdlib.h>

#include <windows.h>
#include <winternl.h>
#include <rpc.h>
#include <powrprof.h>

#include "wine/arctic_power.h"
#include "wine/debug.h"

#include "winlogon.h"

WINE_DEFAULT_DEBUG_CHANNEL(winlogon);

#define TIMER_TICK      1
#define TIMER_IDLE      2
#define TICK_MS         1000
#define IDLE_MS         250
#define HISTORY_EVERY   10000

static const GUID null_guid;
static const GUID scheme_balanced = { 0x381b4222, 0xf694, 0x41f0, { 0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e } };

/* subgroups */
static const GUID sub_video    = { 0x7516b95f, 0xf776, 0x4464, { 0x8c, 0x53, 0x06, 0x16, 0x7f, 0x40, 0xcc, 0x99 } };
static const GUID sub_sleep    = { 0x238c9fa8, 0x0aad, 0x41ed, { 0x83, 0xf4, 0x97, 0xbe, 0x24, 0x2c, 0x8f, 0x20 } };
static const GUID sub_buttons  = { 0x4f971e89, 0xeebd, 0x4455, { 0xa8, 0xde, 0x9e, 0x59, 0x04, 0x0e, 0x73, 0x47 } };
static const GUID sub_battery  = { 0xe73a048d, 0xbf27, 0x4f12, { 0x97, 0x31, 0x8b, 0x20, 0x76, 0xe8, 0x89, 0x1f } };
static const GUID sub_cpu      = { 0x54533251, 0x82be, 0x4824, { 0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00 } };
static const GUID sub_pcie     = { 0x501a4d13, 0x42af, 0x4429, { 0x9f, 0xd1, 0xa8, 0x21, 0x8c, 0x26, 0x8e, 0x20 } };
static const GUID sub_usb      = { 0x2a737441, 0x1930, 0x4402, { 0x8d, 0x77, 0xb2, 0xbe, 0xbb, 0xa3, 0x08, 0xa3 } };
static const GUID sub_wifi     = { 0x19cbb8fa, 0x5279, 0x450e, { 0x9f, 0xac, 0x8a, 0x3d, 0x5f, 0xed, 0xd0, 0xc1 } };
static const GUID sub_saver    = { 0xde830923, 0xa562, 0x41af, { 0xa0, 0x86, 0xe3, 0xa2, 0xc6, 0xba, 0xd2, 0xda } };

/* settings */
static const GUID set_video_idle  = { 0x3c0bc021, 0xc8a8, 0x4e07, { 0xa9, 0x73, 0x6b, 0x14, 0xcb, 0xcb, 0x2b, 0x7e } };
static const GUID set_video_dim   = { 0x17aaa29b, 0x8b43, 0x4b94, { 0xaa, 0xfe, 0x35, 0xf6, 0x4d, 0xaa, 0xf1, 0xee } };
static const GUID set_brightness  = { 0xaded5e82, 0xb909, 0x4619, { 0x99, 0x49, 0xf5, 0xd7, 0x1d, 0xac, 0x0b, 0xcb } };
static const GUID set_dim_level   = { 0xf1fbfde2, 0xa960, 0x4165, { 0x9f, 0x88, 0x50, 0x66, 0x79, 0x11, 0xce, 0x96 } };
static const GUID set_standby     = { 0x29f6c1db, 0x86da, 0x48c5, { 0x9f, 0xdb, 0xf2, 0xb6, 0x7b, 0x1f, 0x44, 0xda } };
static const GUID set_hibernate   = { 0x9d7815a6, 0x7ee4, 0x497e, { 0x88, 0x88, 0x51, 0x5a, 0x05, 0xf0, 0x23, 0x64 } };
static const GUID set_lid         = { 0x5ca83367, 0x6e45, 0x459f, { 0xa2, 0x7b, 0x47, 0x6b, 0x1d, 0x01, 0xc9, 0x36 } };
static const GUID set_pbutton     = { 0x7648efa3, 0xdd9c, 0x4e3e, { 0xb5, 0x66, 0x50, 0xf9, 0x29, 0x38, 0x62, 0x80 } };
static const GUID set_sbutton     = { 0x96996bc0, 0xad50, 0x47ec, { 0x92, 0x3b, 0x6f, 0x41, 0x87, 0x4d, 0xd9, 0xeb } };
static const GUID set_crit_action = { 0x637ea02f, 0xbbcb, 0x4015, { 0x8e, 0x2c, 0xa1, 0xc7, 0xb9, 0xc0, 0xb5, 0x46 } };
static const GUID set_crit_level  = { 0x9a66d8d7, 0x4ff7, 0x4ef9, { 0xb5, 0xa2, 0x5a, 0x32, 0x6c, 0xa2, 0xa4, 0x69 } };
static const GUID set_low_action  = { 0xd8742dcb, 0x3e6a, 0x4b3c, { 0xb3, 0xfe, 0x37, 0x46, 0x23, 0xcd, 0xcf, 0x06 } };
static const GUID set_low_level   = { 0x8183ba9a, 0xe910, 0x48da, { 0x87, 0x69, 0x14, 0xae, 0x6d, 0xc1, 0x17, 0x0a } };
static const GUID set_low_notify  = { 0xbcded951, 0x187b, 0x4d05, { 0xbc, 0xcc, 0xf7, 0xe5, 0x19, 0x60, 0xc2, 0x58 } };
static const GUID set_reserve     = { 0xf3c5027d, 0xcd16, 0x4930, { 0xaa, 0x6b, 0x90, 0xdb, 0x84, 0x4a, 0x8f, 0x00 } };
static const GUID set_proc_min    = { 0x893dee8e, 0x2bef, 0x41e0, { 0x89, 0xc6, 0xb5, 0x5d, 0x09, 0x29, 0x96, 0x4c } };
static const GUID set_proc_max    = { 0xbc5038f7, 0x23e0, 0x4960, { 0x96, 0xda, 0x33, 0xab, 0xaf, 0x59, 0x35, 0xec } };
static const GUID set_epp         = { 0x36687f9e, 0xe3a5, 0x4dbf, { 0xb1, 0xdc, 0x15, 0xeb, 0x38, 0x1c, 0x68, 0x63 } };
static const GUID set_boost       = { 0xbe337238, 0x0d82, 0x4146, { 0xa9, 0x60, 0x4f, 0x37, 0x49, 0xd4, 0x70, 0xc7 } };
static const GUID set_cooling     = { 0x94d3a615, 0xa899, 0x4ac5, { 0xae, 0x2b, 0xe4, 0xd8, 0xf6, 0x34, 0x36, 0x7f } };
static const GUID set_aspm        = { 0xee12f906, 0xd277, 0x404b, { 0xb6, 0xda, 0xe5, 0xfa, 0x1a, 0x57, 0x6d, 0xf5 } };
static const GUID set_usb_suspend = { 0x48e6b7a6, 0x50f5, 0x4782, { 0xa5, 0xd4, 0x53, 0xbb, 0x8f, 0x07, 0xe2, 0x26 } };
static const GUID set_wifi        = { 0x12bbebe6, 0x58d6, 0x4636, { 0x95, 0xbb, 0x32, 0x17, 0xef, 0x86, 0x7c, 0x1a } };
static const GUID set_es_level    = { 0xe69653ca, 0xcf7f, 0x4f05, { 0xaa, 0x73, 0xcb, 0x83, 0x3f, 0xa9, 0x0a, 0xd4 } };
static const GUID set_es_bright   = { 0x13d09884, 0xf74e, 0x474a, { 0xa8, 0x52, 0xb6, 0xbd, 0xe8, 0xad, 0x03, 0xa8 } };

static void publish_gpus(void);

enum action { ACTION_NOTHING, ACTION_SLEEP, ACTION_HIBERNATE, ACTION_SHUT_DOWN, ACTION_DISPLAY_OFF };

/* the scheme's values for the power source now */
struct effective
{
    DWORD video_idle, video_dim, brightness, dim_level, standby, hibernate;
    DWORD lid, pbutton, sbutton;
    DWORD crit_action, crit_level, low_action, low_level, low_notify, reserve;
    DWORD proc_min, proc_max, epp, boost, cooling, aspm, usb_suspend, wifi;
    DWORD es_level, es_bright;
};

static HWND policy_window;
static HKEY power_key, display_key;
static HANDLE power_changed;
static struct arctic_power_status status, last;
static BOOL have_last;
static struct effective eff;
static BOOL saver_on, saver_declined;
static BOOL dimmed, display_off, acting;
static DWORD idle_base, applied_brightness = ARCTIC_UNKNOWN, applied_wifi = ARCTIC_UNKNOWN;
static DWORD base_brightness = ARCTIC_UNKNOWN;
static struct arctic_power_history *history;
static struct arctic_execution_state *execution;
static DWORD last_sample;
static HANDLE user_input, power_button, sleep_button;
static DWORD last_user_input;

/**********************************************************************
 *          The scheme
 */

static BOOL scheme_value( const GUID *scheme, const GUID *overlay, const GUID *sub, const GUID *set, BOOL ac, DWORD *value )
{
    DWORD (WINAPI *read)( HKEY, const GUID *, const GUID *, const GUID *, DWORD * ) =
        ac ? PowerReadACValueIndex : PowerReadDCValueIndex;

    if (overlay && !IsEqualGUID( overlay, &null_guid ) && !read( NULL, overlay, sub, set, value )) return TRUE;
    return !read( NULL, scheme, sub, set, value );
}

static void overlay_for( const GUID *scheme, BOOL ac, GUID *overlay )
{
    WCHAR value[64];
    DWORD size = sizeof(value);

    *overlay = null_guid;
    if (!IsEqualGUID( scheme, &scheme_balanced )) return;
    if (RegGetValueW( HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Power\\User\\PowerSchemes",
                      ac ? L"ActiveOverlayAcPowerScheme" : L"ActiveOverlayDcPowerScheme", RRF_RT_REG_SZ, NULL, value,
                      &size ))
        return;
    UuidFromStringW( value, overlay );
}

static void read_effective( BOOL ac )
{
    struct { const GUID *sub, *set; DWORD *value, fallback; } table[] =
    {
        { &sub_video,   &set_video_idle,  &eff.video_idle,  0 },
        { &sub_video,   &set_video_dim,   &eff.video_dim,   0 },
        { &sub_video,   &set_brightness,  &eff.brightness,  ARCTIC_UNKNOWN },
        { &sub_video,   &set_dim_level,   &eff.dim_level,   50 },
        { &sub_sleep,   &set_standby,     &eff.standby,     0 },
        { &sub_sleep,   &set_hibernate,   &eff.hibernate,   0 },
        { &sub_buttons, &set_lid,         &eff.lid,         ACTION_SLEEP },
        { &sub_buttons, &set_pbutton,     &eff.pbutton,     ACTION_SHUT_DOWN },
        { &sub_buttons, &set_sbutton,     &eff.sbutton,     ACTION_SLEEP },
        { &sub_battery, &set_crit_action, &eff.crit_action, ACTION_HIBERNATE },
        { &sub_battery, &set_crit_level,  &eff.crit_level,  5 },
        { &sub_battery, &set_low_action,  &eff.low_action,  ACTION_NOTHING },
        { &sub_battery, &set_low_level,   &eff.low_level,   10 },
        { &sub_battery, &set_low_notify,  &eff.low_notify,  1 },
        { &sub_battery, &set_reserve,     &eff.reserve,     7 },
        { &sub_cpu,     &set_proc_min,    &eff.proc_min,    5 },
        { &sub_cpu,     &set_proc_max,    &eff.proc_max,    100 },
        { &sub_cpu,     &set_epp,         &eff.epp,         ARCTIC_UNKNOWN },
        { &sub_cpu,     &set_boost,       &eff.boost,       2 },
        { &sub_cpu,     &set_cooling,     &eff.cooling,     1 },
        { &sub_pcie,    &set_aspm,        &eff.aspm,        ARCTIC_UNKNOWN },
        { &sub_usb,     &set_usb_suspend, &eff.usb_suspend, ARCTIC_UNKNOWN },
        { &sub_wifi,    &set_wifi,        &eff.wifi,        ARCTIC_UNKNOWN },
        { &sub_saver,   &set_es_level,    &eff.es_level,    20 },
        { &sub_saver,   &set_es_bright,   &eff.es_bright,   70 },
    };
    GUID *scheme, overlay;

    if (PowerGetActiveScheme( NULL, &scheme ))
    {
        for (UINT i = 0; i < ARRAY_SIZE(table); i++) *table[i].value = table[i].fallback;
        return;
    }
    overlay_for( scheme, ac, &overlay );
    for (UINT i = 0; i < ARRAY_SIZE(table); i++)
        if (!scheme_value( scheme, &overlay, table[i].sub, table[i].set, ac, table[i].value ))
            *table[i].value = table[i].fallback;
    LocalFree( scheme );
}

/**********************************************************************
 *          The hardware
 */

static BOOL on_mains(void)
{
    return status.ac_online || !status.battery_count;
}

static DWORD wanted_brightness(void)
{
    DWORD level = eff.brightness != ARCTIC_UNKNOWN ? eff.brightness : base_brightness;

    if (level == ARCTIC_UNKNOWN) return ARCTIC_UNKNOWN;
    if (dimmed) level = min( level, eff.dim_level );
    /* battery saver's "lower screen brightness": a share of it */
    if (saver_on && eff.es_bright < 100) level = level * eff.es_bright / 100;
    return min( level, 100 );
}

static void apply_brightness(void)
{
    struct arctic_power_apply apply;
    DWORD level = wanted_brightness();

    if (level == ARCTIC_UNKNOWN || level == applied_brightness) return;
    memset( &apply, 0xff, sizeof(apply) );
    apply.brightness_percent = level;
    if (ArcticPowerApply( &apply ) && !(apply.failed & (1 << 7))) applied_brightness = level;
}

static void apply_scheme(void)
{
    struct arctic_power_apply apply;
    struct arctic_cpu_info cpu;
    DWORD epp, wifi;

    read_effective( on_mains() );
    if (ArcticCpuInfo( &cpu ) && cpu.backlight && base_brightness == ARCTIC_UNKNOWN)
        base_brightness = cpu.brightness_percent;

    memset( &apply, 0xff, sizeof(apply) );
    /* Windows' own default energy preference when the scheme has none: balanced */
    epp = eff.epp != ARCTIC_UNKNOWN ? eff.epp : 50;
    if (saver_on) epp = 100;
    apply.epp = epp;
    apply.max_percent = saver_on ? min( eff.proc_max, 70 ) : eff.proc_max;
    apply.min_percent = eff.proc_min;
    apply.boost = saver_on ? 0 : eff.boost != 0;
    /* the firmware's profile follows the energy preference, and passive
     * cooling keeps the fans down as Windows' "Passive" does */
    apply.profile = saver_on || epp >= 60 || !eff.cooling ? 0 : epp <= 10 ? 2 : 1;
    if (eff.aspm != ARCTIC_UNKNOWN) apply.aspm = min( eff.aspm, 2 );
    if (eff.usb_suspend != ARCTIC_UNKNOWN) apply.usb_autosuspend = eff.usb_suspend != 0;
    ArcticPowerApply( &apply );
    if (apply.failed) WARN( "power: not set: %#x\n", apply.failed );
    TRACE( "power: %s, epp %lu, %lu-%lu%%, boost %u, profile %u%s\n", on_mains() ? "mains" : "battery", epp,
           eff.proc_min, eff.proc_max, apply.boost, apply.profile, saver_on ? ", battery saver" : "" );

    applied_brightness = ARCTIC_UNKNOWN;
    apply_brightness();

    wifi = eff.wifi != ARCTIC_UNKNOWN ? eff.wifi != 0 : 0;
    if (saver_on) wifi = 1;
    if (wifi != applied_wifi)
    {
        applied_wifi = wifi;
        ArcticPowerRequest( wifi ? "wifi-powersave on" : "wifi-powersave off" );
    }
}

/* brightness keys and the Control Panel's slider change the scheme's
 * brightness for the power source now, as in Windows */
static void follow_brightness(void)
{
    struct arctic_cpu_info cpu;
    GUID *scheme;

    if (dimmed || display_off || saver_on || !ArcticCpuInfo( &cpu ) || !cpu.backlight) return;
    if (applied_brightness != ARCTIC_UNKNOWN && abs( (int)cpu.brightness_percent - (int)applied_brightness ) <= 1) return;
    if (applied_brightness == ARCTIC_UNKNOWN && eff.brightness == ARCTIC_UNKNOWN)
    {
        base_brightness = applied_brightness = cpu.brightness_percent;
        return;
    }
    base_brightness = applied_brightness = cpu.brightness_percent;
    if (PowerGetActiveScheme( NULL, &scheme )) return;
    if (on_mains()) PowerWriteACValueIndex( NULL, scheme, &sub_video, &set_brightness, cpu.brightness_percent );
    else PowerWriteDCValueIndex( NULL, scheme, &sub_video, &set_brightness, cpu.brightness_percent );
    eff.brightness = cpu.brightness_percent;
    LocalFree( scheme );
}

static void set_display( BOOL off )
{
    DWORD value = off;

    if (display_off == off) return;
    display_off = off;
    TRACE( "power: display %s\n", off ? "off" : "on" );
    if (!display_key)
        RegCreateKeyExW( HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Power\\ArcticDisplay", 0, NULL,
                         REG_OPTION_VOLATILE, KEY_SET_VALUE, NULL, &display_key, NULL );
    if (display_key) RegSetValueExW( display_key, L"MonitorsOff", 0, REG_DWORD, (BYTE *)&value, sizeof(value) );
}

static void set_saver( BOOL on, BOOL by_hand )
{
    DWORD value = on;

    if (saver_on == on) return;
    saver_on = on;
    TRACE( "power: battery saver %s\n", on ? "on" : "off" );
    RegSetValueExW( power_key, ARCTIC_ENERGY_SAVER_ON, 0, REG_DWORD, (BYTE *)&value, sizeof(value) );
    value = on && by_hand;
    RegSetValueExW( power_key, ARCTIC_ENERGY_SAVER_HAND, 0, REG_DWORD, (BYTE *)&value, sizeof(value) );
    PostMessageW( HWND_BROADCAST, WM_POWERBROADCAST, PBT_APMPOWERSTATUSCHANGE, 0 );
}

/**********************************************************************
 *          Actions
 */

static DWORD WINAPI action_thread( void *arg )
{
    enum action action = (UINT_PTR)arg;
    BOOL ok = TRUE;

    switch (action)
    {
    case ACTION_SLEEP:
        if (!IsPwrSuspendAllowed()) break;
        TRACE( "power: sleep\n" );
        ok = SetSuspendState( FALSE, FALSE, FALSE );
        break;
    case ACTION_HIBERNATE:
        TRACE( "power: hibernation\n" );
        if (IsPwrHibernateAllowed()) ok = SetSuspendState( TRUE, FALSE, FALSE );
        else if (IsPwrSuspendAllowed()) ok = SetSuspendState( FALSE, FALSE, FALSE );
        break;
    case ACTION_SHUT_DOWN:
        TRACE( "power: shut down\n" );
        ExitWindowsEx( EWX_SHUTDOWN | EWX_POWEROFF, SHTDN_REASON_MAJOR_POWER | SHTDN_REASON_FLAG_PLANNED );
        break;
    default:
        break;
    }
    if (!ok) WARN( "power: action %u refused: %lu\n", action, GetLastError() );
    /* awake again: inactivity counts from now */
    PostMessageW( policy_window, WM_APP, 0, 0 );
    return 0;
}

static void act( DWORD action )
{
    HANDLE thread;

    if (action == ACTION_NOTHING || acting) return;
    if (action == ACTION_DISPLAY_OFF)
    {
        set_display( TRUE );
        return;
    }
    acting = TRUE;
    if ((thread = CreateThread( NULL, 0, action_thread, (void *)(UINT_PTR)action, 0, NULL ))) CloseHandle( thread );
    else acting = FALSE;
}

/**********************************************************************
 *          Inactivity
 */

static void requested( BOOL *display, BOOL *system )
{
    *display = *system = FALSE;
    if (!execution) return;
    for (UINT i = 0; i < ARCTIC_EXECUTION_STATE_SLOTS; i++)
    {
        LONG pid = execution->slots[i].pid;
        HANDLE process;
        DWORD code;

        if (!pid || (execution->slots[i].display <= 0 && execution->slots[i].system <= 0)) continue;
        /* a process that ended without clearing its slot */
        if (!(process = OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid )))
        {
            InterlockedCompareExchange( &execution->slots[i].pid, 0, pid );
            continue;
        }
        if (GetExitCodeProcess( process, &code ) && code != STILL_ACTIVE)
            InterlockedCompareExchange( &execution->slots[i].pid, 0, pid );
        else
        {
            *display |= execution->slots[i].display > 0;
            *system |= execution->slots[i].system > 0 || execution->slots[i].display > 0;
        }
        CloseHandle( process );
    }
}

/* inactivity since the last input, the last wake, or the last program that
 * said it is busy (SetThreadExecutionState without ES_CONTINUOUS) */
static DWORD idle_ms(void)
{
    LASTINPUTINFO info = { sizeof(info) };
    DWORD now = GetTickCount(), since;

    if (!GetLastInputInfo( &info )) info.dwTime = now;
    /* what winsrv saw of the keyboard, the mouse, the touchpad */
    if (user_input && !WaitForSingleObject( user_input, 0 )) last_user_input = now;
    since = (int)(info.dwTime - idle_base) > 0 ? info.dwTime : idle_base;
    if (last_user_input && (int)(last_user_input - since) > 0) since = last_user_input;
    if (execution)
        for (UINT i = 0; i < ARCTIC_EXECUTION_STATE_SLOTS; i++)
            if (execution->slots[i].pid && (int)(execution->slots[i].ping - since) > 0 &&
                (int)(now - execution->slots[i].ping) >= 0)
                since = execution->slots[i].ping;
    return now - since;
}

static void check_idle(void)
{
    DWORD idle = idle_ms();
    BOOL keep_display, keep_system;

    if (acting) return;
    requested( &keep_display, &keep_system );

    /* any input lights the display again */
    if (idle < IDLE_MS * 2 && (dimmed || display_off))
    {
        set_display( FALSE );
        if (dimmed)
        {
            dimmed = FALSE;
            apply_brightness();
        }
        KillTimer( policy_window, TIMER_IDLE );
        return;
    }
    if (!keep_display)
    {
        if (eff.video_dim && idle >= eff.video_dim * 1000 && !dimmed && !display_off)
        {
            dimmed = TRUE;
            apply_brightness();
            SetTimer( policy_window, TIMER_IDLE, IDLE_MS, NULL );
        }
        if (eff.video_idle && idle >= eff.video_idle * 1000 && !display_off)
        {
            set_display( TRUE );
            SetTimer( policy_window, TIMER_IDLE, IDLE_MS, NULL );
        }
    }
    if (!keep_system)
    {
        if (eff.standby && idle >= eff.standby * 1000 && IsPwrSuspendAllowed()) act( ACTION_SLEEP );
        else if (eff.hibernate && idle >= eff.hibernate * 1000 && IsPwrHibernateAllowed()) act( ACTION_HIBERNATE );
    }
}

/**********************************************************************
 *          The batteries
 */

static void sample_history(void)
{
    DWORD now = GetTickCount();
    UINT32 at;

    if (!history || (history->count && now - last_sample < HISTORY_EVERY)) return;
    last_sample = now;
    at = history->next;
    history->samples[at].tick = now;
    history->samples[at].rate_mw = status.rate_mw;
    history->samples[at].percent = status.percent;
    history->samples[at].ac_online = status.ac_online;
    history->next = (at + 1) % ARCTIC_POWER_HISTORY_SIZE;
    if (history->count < ARCTIC_POWER_HISTORY_SIZE) history->count++;
}

/* the battery icon (batmeter.dll) shows Windows' notification: it watches
 * the shared history for it */
static void warn_battery( UINT level )
{
    if (!history) return;
    history->warning_level = level;
    InterlockedIncrement( &history->warning_serial );
}

static BOOL crossed( DWORD level )
{
    return have_last && last.percent != ARCTIC_UNKNOWN && last.percent > level && status.percent <= level;
}

static void tick(void)
{
    struct arctic_power_status before = status;
    BOOL changed;

    if (!ArcticPowerStatus( &status )) return;
    changed = !have_last || before.ac_online != status.ac_online || before.battery_count != status.battery_count;

    if (changed)
    {
        /* the mains end battery saver; battery saver may start on battery */
        if (on_mains())
        {
            set_saver( FALSE, FALSE );
            saver_declined = FALSE;
        }
        apply_scheme();
        if (have_last) PostMessageW( HWND_BROADCAST, WM_POWERBROADCAST, PBT_APMPOWERSTATUSCHANGE, 0 );
    }
    else if (before.percent != status.percent || before.state != status.state)
        PostMessageW( HWND_BROADCAST, WM_POWERBROADCAST, PBT_APMPOWERSTATUSCHANGE, 0 );

    if (!on_mains() && status.percent != ARCTIC_UNKNOWN)
    {
        if (eff.es_level && status.percent <= eff.es_level && !saver_on && !saver_declined &&
            (!have_last || crossed( eff.es_level ) || changed))
        {
            set_saver( TRUE, FALSE );
            apply_scheme();
        }
        if (crossed( eff.reserve ))
        {
            MESSAGE( "winlogon: the battery is at its reserve level (%u%%)\n", status.percent );
            warn_battery( 2 );
        }
        else if (crossed( eff.low_level ))
        {
            MESSAGE( "winlogon: the battery is low (%u%%)\n", status.percent );
            if (eff.low_notify) warn_battery( 1 );
            PostMessageW( HWND_BROADCAST, WM_POWERBROADCAST, PBT_APMBATTERYLOW, 0 );
            act( eff.low_action );
        }
        if (crossed( eff.crit_level ))
        {
            WARN( "power: the battery is at its critical level (%u%%)\n", status.percent );
            act( eff.crit_action == ACTION_HIBERNATE && !IsPwrHibernateAllowed() ? ACTION_SHUT_DOWN : eff.crit_action );
        }
    }

    if (status.lid_present && have_last && before.lid_closed != status.lid_closed && status.lid_closed)
        act( eff.lid );

    follow_brightness();
    sample_history();
    /* a card that came late (its driver loads in the background) */
    if (GetTickCount() - idle_base < 120000 || !(GetTickCount() / 1000 % 60)) publish_gpus();
    check_idle();
    /* the next tick compares with this one */
    last = status;
    have_last = TRUE;
}

/* someone changed the scheme, the slider or battery saver */
static void reread(void)
{
    DWORD saver = 0, size = sizeof(saver);

    RegGetValueW( power_key, NULL, ARCTIC_ENERGY_SAVER_ON, RRF_RT_REG_DWORD, NULL, &saver, &size );
    if (!!saver != saver_on)
    {
        /* turned off by hand under its level: not again until the next charge */
        if (!saver && !on_mains()) saver_declined = TRUE;
        saver_on = !!saver;
        PostMessageW( HWND_BROADCAST, WM_POWERBROADCAST, PBT_APMPOWERSTATUSCHANGE, 0 );
    }
    apply_scheme();
    RegNotifyChangeKeyValue( power_key, TRUE, REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_CHANGE_NAME, power_changed, TRUE );
}

/**********************************************************************
 *          The graphics cards
 */

/* the card's name as Windows shows it: the display adapter's DeviceDesc */
static void gpu_name( const struct arctic_gpu *gpu, WCHAR *name, DWORD count )
{
    WCHAR path[128], sub[64], desc[256];
    DWORD len, size;
    HKEY key, dev;

    swprintf( name, count, L"%04X:%04X", gpu->vendor_id, gpu->device_id );
    swprintf( path, ARRAY_SIZE(path), L"SYSTEM\\CurrentControlSet\\Enum\\PCI" );
    if (RegOpenKeyExW( HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &key )) return;
    for (DWORD i = 0; len = ARRAY_SIZE(sub), !RegEnumKeyExW( key, i, sub, &len, NULL, NULL, NULL, NULL ); i++)
    {
        WCHAR want[32];

        swprintf( want, ARRAY_SIZE(want), L"VEN_%04X&DEV_%04X", gpu->vendor_id, gpu->device_id );
        if (wcsnicmp( sub, want, wcslen( want ) )) continue;
        if (RegOpenKeyExW( key, sub, 0, KEY_READ, &dev )) continue;
        for (DWORD k = 0; len = ARRAY_SIZE(path), !RegEnumKeyExW( dev, k, path, &len, NULL, NULL, NULL, NULL ); k++)
        {
            size = sizeof(desc);
            if (!RegGetValueW( dev, path, L"DeviceDesc", RRF_RT_REG_SZ, NULL, desc, &size ))
            {
                /* "@oem.inf,%name%;Intel(R) HD Graphics 630": the text after ';' */
                WCHAR *semi = wcsrchr( desc, ';' );
                lstrcpynW( name, semi ? semi + 1 : desc, count );
                break;
            }
        }
        RegCloseKey( dev );
        break;
    }
    RegCloseKey( key );
}

static DWORD multi_size( const WCHAR *list )
{
    const WCHAR *p = list;

    while (*p) p += wcslen( p ) + 1;
    return (p - list + 1) * sizeof(WCHAR);
}

static void add_var( WCHAR *list, size_t *len, size_t max, const WCHAR *name, const WCHAR *value )
{
    int n = swprintf( list + *len, max - *len - 1, L"%s=%s", name, value );

    if (n > 0) *len += n + 1;
    list[*len] = 0;
}

/* what a program running on gpu needs in its environment, whatever the cards */
static void gpu_vars( const struct arctic_gpu_list *list, const struct arctic_gpu *gpu, const WCHAR *name, BOOL gl,
                      WCHAR *vars, size_t max )
{
    BOOL nvidia_here = FALSE;
    WCHAR value[64];
    size_t len = 0;

    vars[0] = vars[1] = 0;
    for (UINT i = 0; i < list->count; i++) nvidia_here |= list->gpus[i].vendor == ARCTIC_GPU_NVIDIA;

    /* DXVK and vkd3d-proton (Direct3D) take the card by its name */
    add_var( vars, &len, max, L"DXVK_FILTER_DEVICE_NAME", name );
    add_var( vars, &len, max, L"VKD3D_FILTER_DEVICE_NAME", name );
    if (gpu->vendor == ARCTIC_GPU_NVIDIA)
    {
        /* NVIDIA's PRIME render offload: its Vulkan layer shows only its card */
        add_var( vars, &len, max, L"__NV_PRIME_RENDER_OFFLOAD", L"1" );
        add_var( vars, &len, max, L"__VK_LAYER_NV_optimus", L"NVIDIA_only" );
        add_var( vars, &len, max, L"MESA_VK_DEVICE_SELECT", L"" );
        add_var( vars, &len, max, L"DRI_PRIME", L"" );
        if (gl)
        {
            add_var( vars, &len, max, L"__GLX_VENDOR_LIBRARY_NAME", L"nvidia" );
            add_var( vars, &len, max, L"__EGL_VENDOR_LIBRARY_FILENAMES", L"/usr/share/glvnd/egl_vendor.d/10_nvidia.json" );
        }
    }
    else
    {
        add_var( vars, &len, max, L"__NV_PRIME_RENDER_OFFLOAD", L"" );
        add_var( vars, &len, max, L"__VK_LAYER_NV_optimus", nvidia_here ? L"non_NVIDIA_only" : L"" );
        /* Mesa's device selection; DRI_PRIME moves OpenGL too */
        swprintf( value, ARRAY_SIZE(value), L"%04x:%04x", gpu->vendor_id, gpu->device_id );
        add_var( vars, &len, max, L"MESA_VK_DEVICE_SELECT", value );
        if (!gpu->integrated)
        {
            WCHAR pci[32];
            size_t k;

            lstrcpynW( pci, gpu->pci, ARRAY_SIZE(pci) );
            for (k = 0; pci[k]; k++) if (pci[k] == ':' || pci[k] == '.') pci[k] = '_';
            swprintf( value, ARRAY_SIZE(value), L"pci-%s", pci );
            add_var( vars, &len, max, L"DRI_PRIME", value );
        }
        else add_var( vars, &len, max, L"DRI_PRIME", L"" );
        add_var( vars, &len, max, L"__GLX_VENDOR_LIBRARY_NAME", L"" );
        add_var( vars, &len, max, L"__EGL_VENDOR_LIBRARY_FILENAMES", L"" );
    }
}

static UINT published_gpus = ~0u;

static void publish_gpus(void)
{
    struct arctic_gpu_list list;
    const struct arctic_gpu *saving = NULL, *fast = NULL;
    WCHAR saving_name[256], fast_name[256], vars[2048];
    HKEY key;

    if (!ArcticGpuList( &list, FALSE ) || list.count == published_gpus) return;
    published_gpus = list.count;
    if (list.count < 2)
    {
        RegDeleteKeyW( HKEY_LOCAL_MACHINE, ARCTIC_GPU_KEY );
        return;
    }
    /* the power saving card is the one the panel hangs on; the other is the fast one */
    for (UINT i = 0; i < list.count; i++) if (list.gpus[i].integrated && !saving) saving = &list.gpus[i];
    if (!saving) saving = &list.gpus[0];
    for (UINT i = 0; i < list.count; i++)
        if (&list.gpus[i] != saving && (!fast || list.gpus[i].vendor == ARCTIC_GPU_NVIDIA)) fast = &list.gpus[i];
    if (!fast) return;

    if (RegCreateKeyExW( HKEY_LOCAL_MACHINE, ARCTIC_GPU_KEY, 0, NULL, REG_OPTION_VOLATILE, KEY_SET_VALUE, NULL, &key, NULL ))
        return;
    gpu_name( saving, saving_name, ARRAY_SIZE(saving_name) );
    gpu_name( fast, fast_name, ARRAY_SIZE(fast_name) );
    RegSetValueExW( key, ARCTIC_GPU_POWER_SAVING_NAME, 0, REG_SZ, (BYTE *)saving_name, (wcslen( saving_name ) + 1) * sizeof(WCHAR) );
    RegSetValueExW( key, ARCTIC_GPU_HIGH_PERF_NAME, 0, REG_SZ, (BYTE *)fast_name, (wcslen( fast_name ) + 1) * sizeof(WCHAR) );
    gpu_vars( &list, saving, saving_name, TRUE, vars, ARRAY_SIZE(vars) );
    RegSetValueExW( key, ARCTIC_GPU_POWER_SAVING, 0, REG_MULTI_SZ, (BYTE *)vars, multi_size( vars ) );
    gpu_vars( &list, fast, fast_name, FALSE, vars, ARRAY_SIZE(vars) );
    RegSetValueExW( key, ARCTIC_GPU_HIGH_PERF, 0, REG_MULTI_SZ, (BYTE *)vars, multi_size( vars ) );
    gpu_vars( &list, fast, fast_name, TRUE, vars, ARRAY_SIZE(vars) );
    RegSetValueExW( key, ARCTIC_GPU_HIGH_PERF_GL, 0, REG_MULTI_SZ, (BYTE *)vars, multi_size( vars ) );
    RegCloseKey( key );
    TRACE( "power: graphics cards %s (power saving), %s (high performance)\n", debugstr_w(saving_name),
           debugstr_w(fast_name) );
}

/**********************************************************************
 *          The thread
 */

static void *shared( const WCHAR *name, SIZE_T size )
{
    HANDLE map = CreateFileMappingW( INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, size, name );
    void *view;

    if (!map) return NULL;
    view = MapViewOfFile( map, FILE_MAP_ALL_ACCESS, 0, 0, size );
    /* the mapping stays as long as winlogon */
    return view;
}

static LRESULT WINAPI policy_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    switch (msg)
    {
    case WM_TIMER:
        if (wp == TIMER_TICK) tick();
        else if (wp == TIMER_IDLE) check_idle();
        return 0;
    case WM_APP:
        /* an action is done: the machine woke, or did not sleep */
        acting = FALSE;
        idle_base = GetTickCount();
        dimmed = FALSE;
        set_display( FALSE );
        have_last = FALSE;
        applied_wifi = ARCTIC_UNKNOWN;
        tick();
        apply_scheme();
        PostMessageW( HWND_BROADCAST, WM_POWERBROADCAST, PBT_APMPOWERSTATUSCHANGE, 0 );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wp, lp );
}

static DWORD WINAPI policy_thread( void *arg )
{
    WNDCLASSW cls = { .lpfnWndProc = policy_proc, .hInstance = GetModuleHandleW( NULL ),
                      .lpszClassName = ARCTIC_POWER_POLICY_CLASS };
    DWORD saver = 0, size = sizeof(saver);
    MSG msg;

    SetThreadDescription( GetCurrentThread(), L"PowerPolicy" );
    RegCreateKeyExW( HKEY_LOCAL_MACHINE, ARCTIC_POWER_KEY, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &power_key, NULL );
    if (!power_key) return 1;
    /* battery saver turned on by hand stays on across a restart, as in Windows */
    RegGetValueW( power_key, NULL, ARCTIC_ENERGY_SAVER_HAND, RRF_RT_REG_DWORD, NULL, &saver, &size );
    saver_on = !!saver;
    saver = saver_on;
    RegSetValueExW( power_key, ARCTIC_ENERGY_SAVER_ON, 0, REG_DWORD, (BYTE *)&saver, sizeof(saver) );

    history = shared( ARCTIC_POWER_HISTORY_NAME, sizeof(*history) );
    execution = shared( ARCTIC_EXECUTION_STATE_NAME, sizeof(*execution) );
    power_button = CreateEventW( NULL, FALSE, FALSE, ARCTIC_POWER_BUTTON_EVENT );
    sleep_button = CreateEventW( NULL, FALSE, FALSE, ARCTIC_SLEEP_BUTTON_EVENT );
    power_changed = CreateEventW( NULL, FALSE, FALSE, NULL );
    user_input = CreateEventW( NULL, FALSE, FALSE, ARCTIC_USER_INPUT_EVENT );
    idle_base = GetTickCount();

    RegisterClassW( &cls );
    policy_window = CreateWindowW( cls.lpszClassName, NULL, WS_POPUP, 0, 0, 0, 0, NULL, NULL, cls.hInstance, NULL );
    tick();
    reread();
    publish_gpus();
    SetTimer( policy_window, TIMER_TICK, TICK_MS, NULL );

    for (;;)
    {
        HANDLE handles[3] = { power_changed, power_button, sleep_button };
        DWORD ret = MsgWaitForMultipleObjects( 3, handles, FALSE, INFINITE, QS_ALLINPUT );

        if (ret == WAIT_OBJECT_0) reread();
        else if (ret == WAIT_OBJECT_0 + 1 || ret == WAIT_OBJECT_0 + 2)
        {
            DWORD action = ret == WAIT_OBJECT_0 + 2 ? eff.sbutton : eff.pbutton;

            MESSAGE( "winlogon: %s button: %lu\n", ret == WAIT_OBJECT_0 + 2 ? "sleep" : "power", action );
            /* the press wakes the display too */
            idle_base = GetTickCount();
            act( action );
        }
        while (PeekMessageW( &msg, NULL, 0, 0, PM_REMOVE ))
        {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage( &msg );
            DispatchMessageW( &msg );
        }
    }
}

void start_power_policy(void)
{
    HANDLE thread = CreateThread( NULL, 0, policy_thread, NULL, 0, NULL );

    if (thread) CloseHandle( thread );
}
