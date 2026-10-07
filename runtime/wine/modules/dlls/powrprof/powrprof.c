/*
 * Power management (powrprof.dll)
 *
 * Power schemes as Windows keeps them (docs/power.md):
 *   HKLM\SYSTEM\CurrentControlSet\Control\Power\PowerSettings\{subgroup}\{setting}
 *     what a setting is: its names, its range or its possible values (keys
 *     0, 1...), and DefaultPowerSchemeValues\{scheme} AcSettingIndex /
 *     DcSettingIndex (an overlay of the power slider: ProvAc/ProvDc...);
 *   HKLM\...\Power\User\PowerSchemes\{scheme}
 *     the schemes, ActivePowerScheme and the slider's overlays
 *     (ActiveOverlayAcPowerScheme, ActiveOverlayDcPowerScheme); a value the
 *     user changed: {scheme}\{subgroup}\{setting} ACSettingIndex/DCSettingIndex.
 * runtime/registry/power.reg has those of Windows 10 (tools/power/win10-power.py),
 * their names are this DLL's strings (strings.rc). A scheme the user made
 * keeps the scheme it was made from in ArcticBaseScheme and falls back on it.
 *
 * The power policy (winlogon) watches these keys and applies the active
 * scheme to the hardware; the hardware's state comes from the unix side
 * (unix.c) for everything of Arctic that shows it.
 *
 * Copyright (C) 2005 Benjamin Cutler
 * Copyright (C) 2008 Stefan Leichter
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winreg.h"
#include "winternl.h"
#include "winuser.h"
#include "rpc.h"
#include "powrprof.h"
#include "powersetting.h"
#include "initguid.h"
#include "unixlib.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(powrprof);

#ifndef POWER_PLATFORM_ROLE_V1
#define POWER_PLATFORM_ROLE_V1 1
#define POWER_PLATFORM_ROLE_V2 2
#endif

POWER_PLATFORM_ROLE WINAPI PowerDeterminePlatformRoleEx( ULONG version );

static HINSTANCE powrprof_instance;
static const GUID null_guid;
static BOOL unix_ready;

static const WCHAR settings_path[] = L"SYSTEM\\CurrentControlSet\\Control\\Power\\PowerSettings";
static const WCHAR schemes_path[] = L"SYSTEM\\CurrentControlSet\\Control\\Power\\User\\PowerSchemes";

/* settings that belong to no subgroup sit right under PowerSettings */
DEFINE_GUID( NO_SUBGROUP, 0xfea3413e, 0x7e05, 0x4911, 0x9a, 0x71, 0x70, 0x03, 0x31, 0xf1, 0xc2, 0x94 );
DEFINE_GUID( SCHEME_BALANCED, 0x381b4222, 0xf694, 0x41f0, 0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e );
DEFINE_GUID( SCHEME_MAX, 0x8c5e7fda, 0xe8bf, 0x4a96, 0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c );
DEFINE_GUID( SCHEME_MIN, 0xa1841308, 0x3541, 0x4fab, 0xbc, 0x81, 0xf7, 0x15, 0x56, 0xf2, 0x0b, 0x4a );

/* the power slider's overlays: not schemes of their own in any list */
static const WCHAR *const overlays[] =
{
    L"961cc777-2547-4f9d-8174-7d86181b8a7a",  /* better battery */
    L"3af9b8d9-7c97-431d-ad78-34a8bfea439f",  /* high performance */
    L"ded574b5-45a0-4f42-8737-46345c09c238",  /* max performance */
};

static const WCHAR *const builtin_schemes[] =
{
    L"381b4222-f694-41f0-9685-ff5bb260df2e",
    L"8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c",
    L"a1841308-3541-4fab-bc81-f71556f20b4a",
};

/**********************************************************************
 *          Helpers
 */

static void guid_str( const GUID *guid, WCHAR *buf )
{
    swprintf( buf, 37, L"%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", guid->Data1, guid->Data2, guid->Data3,
              guid->Data4[0], guid->Data4[1], guid->Data4[2], guid->Data4[3], guid->Data4[4], guid->Data4[5],
              guid->Data4[6], guid->Data4[7] );
}

static BOOL parse_guid( const WCHAR *str, GUID *guid )
{
    unsigned int d[11];

    if (*str == '{') str++;
    if (swscanf( str, L"%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6],
                 &d[7], &d[8], &d[9], &d[10] ) != 11)
        return FALSE;
    guid->Data1 = d[0];
    guid->Data2 = d[1];
    guid->Data3 = d[2];
    for (int i = 0; i < 8; i++) guid->Data4[i] = d[3 + i];
    return TRUE;
}

static BOOL is_overlay( const WCHAR *name )
{
    for (UINT i = 0; i < ARRAY_SIZE(overlays); i++) if (!wcsicmp( name, overlays[i] )) return TRUE;
    return FALSE;
}

static BOOL is_builtin( const WCHAR *name )
{
    for (UINT i = 0; i < ARRAY_SIZE(builtin_schemes); i++) if (!wcsicmp( name, builtin_schemes[i] )) return TRUE;
    return FALSE;
}

static BOOL no_subgroup( const GUID *subgroup )
{
    return !subgroup || IsEqualGUID( subgroup, &null_guid ) || IsEqualGUID( subgroup, &NO_SUBGROUP );
}

/* PowerSettings\{subgroup}[\{setting}] */
static LONG open_setting( const GUID *subgroup, const GUID *setting, REGSAM access, HKEY *key )
{
    WCHAR path[256], sub[40], set[40];

    if (setting) guid_str( setting, set );
    if (!no_subgroup( subgroup )) guid_str( subgroup, sub );
    if (!setting && no_subgroup( subgroup )) return RegOpenKeyExW( HKEY_LOCAL_MACHINE, settings_path, 0, access, key );
    if (!setting) swprintf( path, ARRAY_SIZE(path), L"%s\\%s", settings_path, sub );
    else if (no_subgroup( subgroup )) swprintf( path, ARRAY_SIZE(path), L"%s\\%s", settings_path, set );
    else swprintf( path, ARRAY_SIZE(path), L"%s\\%s\\%s", settings_path, sub, set );
    return RegOpenKeyExW( HKEY_LOCAL_MACHINE, path, 0, access, key );
}

/* User\PowerSchemes\{scheme}[\{subgroup}[\{setting}]] */
static void scheme_path( WCHAR *path, size_t size, const WCHAR *scheme, const GUID *subgroup, const GUID *setting )
{
    WCHAR sub[40], set[40];

    if (setting) guid_str( setting, set );
    if (!no_subgroup( subgroup )) guid_str( subgroup, sub );
    if (!setting && no_subgroup( subgroup )) swprintf( path, size, L"%s\\%s", schemes_path, scheme );
    else if (!setting) swprintf( path, size, L"%s\\%s\\%s", schemes_path, scheme, sub );
    else if (no_subgroup( subgroup )) swprintf( path, size, L"%s\\%s\\%s", schemes_path, scheme, set );
    else swprintf( path, size, L"%s\\%s\\%s\\%s", schemes_path, scheme, sub, set );
}

static BOOL scheme_exists( const WCHAR *scheme )
{
    WCHAR path[256];
    HKEY key;

    swprintf( path, ARRAY_SIZE(path), L"%s\\%s", schemes_path, scheme );
    if (RegOpenKeyExW( HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &key )) return FALSE;
    RegCloseKey( key );
    return TRUE;
}

static LONG get_dword( HKEY root, const WCHAR *path, const WCHAR *name, DWORD *value )
{
    DWORD size = sizeof(*value), type;
    LONG ret = RegGetValueW( root, path, name, RRF_RT_REG_DWORD, &type, value, &size );
    return ret;
}

/* the value a scheme gives a setting: the user's, the scheme's default,
 * or the default of the scheme it was made from */
static DWORD read_index( const WCHAR *scheme, const GUID *subgroup, const GUID *setting, BOOL ac, DWORD *index,
                         int depth )
{
    WCHAR path[512], base[40], set[40], sub[40];
    DWORD size;

    if (depth > 4) return ERROR_FILE_NOT_FOUND;
    scheme_path( path, ARRAY_SIZE(path), scheme, subgroup, setting );
    if (!get_dword( HKEY_LOCAL_MACHINE, path, ac ? L"ACSettingIndex" : L"DCSettingIndex", index )) return ERROR_SUCCESS;

    guid_str( setting, set );
    if (no_subgroup( subgroup ))
        swprintf( path, ARRAY_SIZE(path), L"%s\\%s\\DefaultPowerSchemeValues\\%s", settings_path, set, scheme );
    else
    {
        guid_str( subgroup, sub );
        swprintf( path, ARRAY_SIZE(path), L"%s\\%s\\%s\\DefaultPowerSchemeValues\\%s", settings_path, sub, set, scheme );
    }
    if (!get_dword( HKEY_LOCAL_MACHINE, path, ac ? L"AcSettingIndex" : L"DcSettingIndex", index )) return ERROR_SUCCESS;
    if (is_overlay( scheme ) &&
        !get_dword( HKEY_LOCAL_MACHINE, path, ac ? L"ProvAcSettingIndex" : L"ProvDcSettingIndex", index ))
        return ERROR_SUCCESS;

    swprintf( path, ARRAY_SIZE(path), L"%s\\%s", schemes_path, scheme );
    size = sizeof(base);
    if (!RegGetValueW( HKEY_LOCAL_MACHINE, path, L"ArcticBaseScheme", RRF_RT_REG_SZ, NULL, base, &size ))
        return read_index( base, subgroup, setting, ac, index, depth + 1 );
    return ERROR_FILE_NOT_FOUND;
}

static DWORD write_index( const WCHAR *scheme, const GUID *subgroup, const GUID *setting, BOOL ac, DWORD index )
{
    WCHAR path[512];
    HKEY key;
    LONG ret;

    if (!scheme_exists( scheme )) return ERROR_FILE_NOT_FOUND;
    scheme_path( path, ARRAY_SIZE(path), scheme, subgroup, setting );
    if ((ret = RegCreateKeyExW( HKEY_LOCAL_MACHINE, path, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL ))) return ret;
    ret = RegSetValueExW( key, ac ? L"ACSettingIndex" : L"DCSettingIndex", 0, REG_DWORD, (BYTE *)&index, sizeof(index) );
    RegCloseKey( key );
    return ret;
}

/* "@%SystemRoot%\system32\powrprof.dll,-15,Balanced (recommended)": the
 * string of the DLL, or the text after it when the DLL has none */
static void resolve_string( const WCHAR *value, WCHAR *out, DWORD count )
{
    const WCHAR *comma, *fallback;
    WCHAR file[MAX_PATH], expanded[MAX_PATH];
    int id;

    out[0] = 0;
    if (value[0] != '@')
    {
        lstrcpynW( out, value, count );
        return;
    }
    if (!(comma = wcschr( value, ',' )) || comma[1] != '-')
    {
        lstrcpynW( out, value + 1, count );
        return;
    }
    id = wcstol( comma + 2, NULL, 10 );
    fallback = wcspbrk( comma + 2, L",;" );
    lstrcpynW( file, value + 1, min( (size_t)(comma - value), ARRAY_SIZE(file) ) );
    ExpandEnvironmentStringsW( file, expanded, ARRAY_SIZE(expanded) );
    if (wcsstr( wcslwr( expanded ), L"powrprof.dll" ))
    {
        if (LoadStringW( powrprof_instance, id, out, count )) return;
    }
    else
    {
        HMODULE module = LoadLibraryExW( expanded, NULL, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE );
        if (module)
        {
            int n = LoadStringW( module, id, out, count );
            FreeLibrary( module );
            if (n) return;
        }
    }
    lstrcpynW( out, fallback && fallback[0] == ',' ? fallback + 1 : L"", count );
}

/* a string value, resolved, into a buffer of bytes as Power*Read* give it */
static DWORD read_string( HKEY key, const WCHAR *name, UCHAR *buffer, DWORD *size )
{
    WCHAR raw[1024], text[1024];
    DWORD raw_size = sizeof(raw), need;
    LONG ret;

    if (!size) return ERROR_INVALID_PARAMETER;
    if ((ret = RegQueryValueExW( key, name, NULL, NULL, (BYTE *)raw, &raw_size ))) return ret;
    raw[min( raw_size / sizeof(WCHAR), ARRAY_SIZE(raw) - 1 )] = 0;
    resolve_string( raw, text, ARRAY_SIZE(text) );
    need = (wcslen( text ) + 1) * sizeof(WCHAR);
    if (!buffer)
    {
        *size = need;
        return ERROR_SUCCESS;
    }
    if (*size < need)
    {
        *size = need;
        return ERROR_MORE_DATA;
    }
    memcpy( buffer, text, need );
    *size = need;
    return ERROR_SUCCESS;
}

static DWORD write_string( HKEY key, const WCHAR *name, const UCHAR *buffer, DWORD size )
{
    if (!buffer) return ERROR_INVALID_PARAMETER;
    return RegSetValueExW( key, name, 0, REG_SZ, buffer, size );
}

static DWORD copy_guid( const GUID *guid, UCHAR *buffer, DWORD *size )
{
    if (!size) return ERROR_INVALID_PARAMETER;
    if (!buffer)
    {
        *size = sizeof(GUID);
        return ERROR_SUCCESS;
    }
    if (*size < sizeof(GUID))
    {
        *size = sizeof(GUID);
        return ERROR_MORE_DATA;
    }
    memcpy( buffer, guid, sizeof(GUID) );
    *size = sizeof(GUID);
    return ERROR_SUCCESS;
}

/* a key of PowerSettings that is one setting, not a subgroup: it has
 * defaults for the schemes, a range or a list of values */
static BOOL is_setting_key( HKEY parent, const WCHAR *name )
{
    HKEY key, sub;
    BOOL setting = FALSE;
    DWORD value;

    if (RegOpenKeyExW( parent, name, 0, KEY_READ, &key )) return FALSE;
    if (!RegOpenKeyExW( key, L"DefaultPowerSchemeValues", 0, KEY_READ, &sub ) ||
        !RegOpenKeyExW( key, L"0", 0, KEY_READ, &sub ))
    {
        RegCloseKey( sub );
        setting = TRUE;
    }
    else if (!get_dword( key, NULL, L"ValueMax", &value )) setting = TRUE;
    RegCloseKey( key );
    return setting;
}

/**********************************************************************
 *          The hardware (unix side)
 */

static BOOL init_unix(void)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    BOOL pending;

    /* the first caller loads the unix side, the others wait for it */
    if (InitOnceBeginInitialize( &once, 0, &pending, NULL ) && pending)
    {
        unix_ready = !__wine_init_unix_call();
        InitOnceComplete( &once, 0, NULL );
    }
    return unix_ready;
}

BOOL WINAPI ArcticPowerStatus( struct arctic_power_status *status )
{
    if (!init_unix() || WINE_UNIX_CALL( unix_power_status, status )) return FALSE;
    return TRUE;
}

BOOL WINAPI ArcticGpuList( struct arctic_gpu_list *list, BOOL with_users )
{
    struct gpu_list_params params = { list, with_users };

    if (!init_unix() || WINE_UNIX_CALL( unix_gpu_list, &params )) return FALSE;
    return TRUE;
}

BOOL WINAPI ArcticCpuInfo( struct arctic_cpu_info *info )
{
    if (!init_unix() || WINE_UNIX_CALL( unix_cpu_info, info )) return FALSE;
    return TRUE;
}

BOOL WINAPI ArcticPowerApply( struct arctic_power_apply *apply )
{
    if (!init_unix() || WINE_UNIX_CALL( unix_power_apply, apply )) return FALSE;
    return TRUE;
}

/* The host does what takes the whole machine (docs/hibernation.md,
 * docs/power.md): the request goes through /run/arctic/power, the caller
 * freezes with everything else while the machine sleeps, and the answer is
 * there when it wakes: "awake", or "refused" with why. */
BOOL WINAPI ArcticPowerRequest( const char *request )
{
    static const WCHAR request_file[] = L"\\\\?\\unix\\run\\arctic\\power\\request";
    static const WCHAR result_file[] = L"\\\\?\\unix\\run\\arctic\\power\\result";
    static SRWLOCK lock = SRWLOCK_INIT;
    char answer[256], line[64];
    DWORD done, i;
    HANDLE file;
    BOOL ok = FALSE;

    AcquireSRWLockExclusive( &lock );
    DeleteFileW( result_file );
    file = CreateFileW( request_file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL );
    if (file == INVALID_HANDLE_VALUE)
    {
        ReleaseSRWLockExclusive( &lock );
        return FALSE;
    }
    snprintf( line, sizeof(line), "%s\n", request );
    WriteFile( file, line, strlen( line ), &done, NULL );
    CloseHandle( file );
    SetLastError( ERROR_TIMEOUT );
    for (i = 0; i < 1800; i++)
    {
        Sleep( 100 );
        file = CreateFileW( result_file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL );
        if (file == INVALID_HANDLE_VALUE) continue;
        done = 0;
        ReadFile( file, answer, sizeof(answer) - 1, &done, NULL );
        CloseHandle( file );
        answer[done] = 0;
        DeleteFileW( result_file );
        TRACE( "%s: %s", request, answer );
        if (!strncmp( answer, "awake", 5 ) || !strncmp( answer, "done", 4 )) ok = TRUE;
        else SetLastError( ERROR_BUSY );
        break;
    }
    ReleaseSRWLockExclusive( &lock );
    return ok;
}

/**********************************************************************
 *          Legacy power policies (Windows XP)
 */

NTSTATUS WINAPI CallNtPowerInformation( POWER_INFORMATION_LEVEL level, void *input, ULONG input_size,
                                        void *output, ULONG output_size )
{
    return NtPowerInformation( level, input, input_size, output, output_size );
}

BOOLEAN WINAPI CanUserWritePwrScheme(void)
{
    return TRUE;
}

BOOLEAN WINAPI DeletePwrScheme( UINT index )
{
    FIXME( "(%d) stub!\n", index );
    SetLastError( ERROR_CALL_NOT_IMPLEMENTED );
    return FALSE;
}

BOOLEAN WINAPI EnumPwrSchemes( PWRSCHEMESENUMPROC proc, LPARAM param )
{
    FIXME( "(%p, %Id) stub!\n", proc, param );
    SetLastError( ERROR_CALL_NOT_IMPLEMENTED );
    return FALSE;
}

BOOLEAN WINAPI GetActivePwrScheme( PUINT id )
{
    *id = 0;
    return TRUE;
}

BOOLEAN WINAPI GetCurrentPowerPolicies( PGLOBAL_POWER_POLICY global, PPOWER_POLICY policy )
{
    FIXME( "(%p, %p) stub!\n", global, policy );
    return FALSE;
}

BOOLEAN WINAPI GetPwrCapabilities( PSYSTEM_POWER_CAPABILITIES caps )
{
    struct arctic_power_status status;
    DWORD hibernate = 0;

    if (!caps)
    {
        SetLastError( ERROR_INVALID_PARAMETER );
        return FALSE;
    }
    memset( caps, 0, sizeof(*caps) );
    caps->PowerButtonPresent = TRUE;
    caps->SystemS5 = TRUE;
    caps->ProcessorThrottle = TRUE;
    caps->ProcessorMinThrottle = 5;
    caps->ProcessorMaxThrottle = 100;
    caps->VideoDimPresent = TRUE;
    caps->ApmPresent = FALSE;
    get_dword( HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Power", L"HibernateEnabled", &hibernate );
    caps->SystemS4 = TRUE;
    caps->HiberFilePresent = !!hibernate;
    if (ArcticPowerStatus( &status ))
    {
        caps->SystemBatteriesPresent = status.battery_count > 0;
        caps->BatteriesAreShortTerm = FALSE;
        caps->LidPresent = status.lid_present;
        caps->SystemS3 = status.sleep_deep;
        if (status.battery_count)
        {
            caps->BatteryScale[0].Granularity = 1;
            caps->BatteryScale[0].Capacity = 100;
        }
    }
    caps->RtcWake = PowerSystemSleeping3;
    caps->MinDeviceWakeState = PowerSystemSleeping1;
    caps->DefaultLowLatencyWake = PowerSystemUnspecified;
    return TRUE;
}

BOOLEAN WINAPI GetPwrDiskSpindownRange( PUINT max, PUINT min )
{
    if (!max || !min)
    {
        SetLastError( ERROR_INVALID_PARAMETER );
        return FALSE;
    }
    *max = 3600 * 5;
    *min = 3;
    return TRUE;
}

BOOLEAN WINAPI IsAdminOverrideActive( PADMINISTRATOR_POWER_POLICY policy )
{
    return FALSE;
}

BOOLEAN WINAPI IsPwrHibernateAllowed(void)
{
    DWORD enabled = 0;

    get_dword( HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Power", L"HibernateEnabled", &enabled );
    return !!enabled;
}

BOOLEAN WINAPI IsPwrShutdownAllowed(void)
{
    return TRUE;
}

BOOLEAN WINAPI IsPwrSuspendAllowed(void)
{
    struct arctic_power_status status;

    return ArcticPowerStatus( &status ) && status.sleep_supported;
}

BOOLEAN WINAPI ReadGlobalPwrPolicy( PGLOBAL_POWER_POLICY policy )
{
    FIXME( "(%p) stub!\n", policy );
    return FALSE;
}

BOOLEAN WINAPI ReadProcessorPwrScheme( UINT id, PMACHINE_PROCESSOR_POWER_POLICY policy )
{
    FIXME( "(%d, %p) stub!\n", id, policy );
    SetLastError( ERROR_FILE_NOT_FOUND );
    return FALSE;
}

BOOLEAN WINAPI ReadPwrScheme( UINT id, PPOWER_POLICY policy )
{
    FIXME( "(%d, %p) stub!\n", id, policy );
    return FALSE;
}

BOOLEAN WINAPI SetActivePwrScheme( UINT id, PGLOBAL_POWER_POLICY global, PPOWER_POLICY policy )
{
    FIXME( "(%d, %p, %p) stub!\n", id, global, policy );
    return FALSE;
}

/* Sleep and hibernation are the host's. Programs hear of it as from Windows:
 * PBT_APMSUSPEND before, PBT_APMRESUMESUSPEND / PBT_APMRESUMEAUTOMATIC after. */
BOOLEAN WINAPI SetSuspendState( BOOLEAN hibernate, BOOLEAN force, BOOLEAN disable_wake )
{
    DWORD_PTR result;
    BOOL ok;

    TRACE( "(%d, %d, %d)\n", hibernate, force, disable_wake );
    if (!hibernate && !IsPwrSuspendAllowed())
    {
        SetLastError( ERROR_NOT_SUPPORTED );
        return FALSE;
    }
    SendMessageTimeoutW( HWND_BROADCAST, WM_POWERBROADCAST, PBT_APMSUSPEND, 0, SMTO_ABORTIFHUNG, 2000, &result );
    ok = ArcticPowerRequest( hibernate ? "hibernate" : "sleep" );
    PostMessageW( HWND_BROADCAST, WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0 );
    PostMessageW( HWND_BROADCAST, WM_POWERBROADCAST, PBT_APMRESUMESUSPEND, 0 );
    return ok;
}

BOOLEAN WINAPI WriteGlobalPwrPolicy( PGLOBAL_POWER_POLICY policy )
{
    FIXME( "(%p) stub!\n", policy );
    return TRUE;
}

BOOLEAN WINAPI WriteProcessorPwrScheme( UINT id, PMACHINE_PROCESSOR_POWER_POLICY policy )
{
    FIXME( "(%d, %p) stub!\n", id, policy );
    return TRUE;
}

BOOLEAN WINAPI WritePwrScheme( PUINT id, LPWSTR name, LPWSTR description, PPOWER_POLICY policy )
{
    FIXME( "(%p, %s, %s, %p) stub!\n", id, debugstr_w(name), debugstr_w(description), policy );
    return TRUE;
}

/**********************************************************************
 *          Power schemes (Windows Vista and later)
 */

DWORD WINAPI PowerGetActiveScheme( HKEY root, GUID **scheme )
{
    WCHAR value[64];
    DWORD size = sizeof(value);
    GUID guid = SCHEME_BALANCED;

    TRACE( "(%p, %p)\n", root, scheme );
    if (!scheme) return ERROR_INVALID_PARAMETER;
    if (!RegGetValueW( HKEY_LOCAL_MACHINE, schemes_path, L"ActivePowerScheme", RRF_RT_REG_SZ, NULL, value, &size ))
    {
        GUID read;
        if (parse_guid( value, &read ) && scheme_exists( value )) guid = read;
    }
    if (!(*scheme = LocalAlloc( 0, sizeof(GUID) ))) return ERROR_NOT_ENOUGH_MEMORY;
    **scheme = guid;
    return ERROR_SUCCESS;
}

/* the power policy (winlogon) watches the keys and tells the programs
 * what they asked to hear */
static void notify_change(void)
{
}

DWORD WINAPI PowerSetActiveScheme( HKEY root, GUID *scheme )
{
    WCHAR name[40];
    LONG ret;
    HKEY key;

    TRACE( "(%p, %s)\n", root, debugstr_guid(scheme) );
    if (!scheme) return ERROR_INVALID_PARAMETER;
    guid_str( scheme, name );
    if (!scheme_exists( name ) || is_overlay( name )) return ERROR_FILE_NOT_FOUND;
    if ((ret = RegCreateKeyExW( HKEY_LOCAL_MACHINE, schemes_path, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL ))) return ret;
    ret = RegSetValueExW( key, L"ActivePowerScheme", 0, REG_SZ, (BYTE *)name, (wcslen( name ) + 1) * sizeof(WCHAR) );
    RegCloseKey( key );
    notify_change();
    return ret;
}

static DWORD read_value_index( HKEY root, const GUID *scheme, const GUID *subgroup, const GUID *setting, BOOL ac,
                               DWORD *index )
{
    WCHAR name[40];

    if (!scheme || !setting || !index) return ERROR_INVALID_PARAMETER;
    guid_str( scheme, name );
    return read_index( name, subgroup, setting, ac, index, 0 );
}

DWORD WINAPI PowerReadACValueIndex( HKEY root, const GUID *scheme, const GUID *subgroup, const GUID *setting, DWORD *index )
{
    TRACE( "(%p, %s, %s, %s, %p)\n", root, debugstr_guid(scheme), debugstr_guid(subgroup), debugstr_guid(setting), index );
    return read_value_index( root, scheme, subgroup, setting, TRUE, index );
}

DWORD WINAPI PowerReadDCValueIndex( HKEY root, const GUID *scheme, const GUID *subgroup, const GUID *setting, DWORD *index )
{
    TRACE( "(%p, %s, %s, %s, %p)\n", root, debugstr_guid(scheme), debugstr_guid(subgroup), debugstr_guid(setting), index );
    return read_value_index( root, scheme, subgroup, setting, FALSE, index );
}

static DWORD read_value( const GUID *scheme, const GUID *subgroup, const GUID *setting, BOOL ac, ULONG *type,
                         UCHAR *buffer, DWORD *size )
{
    DWORD index, ret;

    if (!size) return ERROR_INVALID_PARAMETER;
    if ((ret = read_value_index( NULL, scheme, subgroup, setting, ac, &index ))) return ret;
    if (type) *type = REG_DWORD;
    if (!buffer)
    {
        *size = sizeof(DWORD);
        return ERROR_SUCCESS;
    }
    if (*size < sizeof(DWORD))
    {
        *size = sizeof(DWORD);
        return ERROR_MORE_DATA;
    }
    memcpy( buffer, &index, sizeof(index) );
    *size = sizeof(DWORD);
    return ERROR_SUCCESS;
}

DWORD WINAPI PowerReadACValue( HKEY root, const GUID *scheme, const GUID *subgroup, const GUID *setting, ULONG *type,
                               UCHAR *buffer, DWORD *size )
{
    return read_value( scheme, subgroup, setting, TRUE, type, buffer, size );
}

DWORD WINAPI PowerReadDCValue( HKEY root, const GUID *scheme, const GUID *subgroup, const GUID *setting, ULONG *type,
                               UCHAR *buffer, DWORD *size )
{
    return read_value( scheme, subgroup, setting, FALSE, type, buffer, size );
}

DWORD WINAPI PowerWriteACValueIndex( HKEY root, const GUID *scheme, const GUID *subgroup, const GUID *setting, DWORD index )
{
    WCHAR name[40];
    DWORD ret;

    TRACE( "(%p, %s, %s, %s, %lu)\n", root, debugstr_guid(scheme), debugstr_guid(subgroup), debugstr_guid(setting), index );
    if (!scheme || !setting) return ERROR_INVALID_PARAMETER;
    guid_str( scheme, name );
    if (!(ret = write_index( name, subgroup, setting, TRUE, index ))) notify_change();
    return ret;
}

DWORD WINAPI PowerWriteDCValueIndex( HKEY root, const GUID *scheme, const GUID *subgroup, const GUID *setting, DWORD index )
{
    WCHAR name[40];
    DWORD ret;

    TRACE( "(%p, %s, %s, %s, %lu)\n", root, debugstr_guid(scheme), debugstr_guid(subgroup), debugstr_guid(setting), index );
    if (!scheme || !setting) return ERROR_INVALID_PARAMETER;
    guid_str( scheme, name );
    if (!(ret = write_index( name, subgroup, setting, FALSE, index ))) notify_change();
    return ret;
}

static DWORD read_default( const GUID *scheme, const GUID *subgroup, const GUID *setting, BOOL ac, DWORD *index )
{
    WCHAR path[512], name[40], set[40], sub[40], base[40];
    DWORD size;

    if (!scheme || !setting || !index) return ERROR_INVALID_PARAMETER;
    guid_str( scheme, name );
    guid_str( setting, set );
    for (int depth = 0; depth < 4; depth++)
    {
        if (no_subgroup( subgroup ))
            swprintf( path, ARRAY_SIZE(path), L"%s\\%s\\DefaultPowerSchemeValues\\%s", settings_path, set, name );
        else
        {
            guid_str( subgroup, sub );
            swprintf( path, ARRAY_SIZE(path), L"%s\\%s\\%s\\DefaultPowerSchemeValues\\%s", settings_path, sub, set, name );
        }
        if (!get_dword( HKEY_LOCAL_MACHINE, path, ac ? L"AcSettingIndex" : L"DcSettingIndex", index )) return ERROR_SUCCESS;
        swprintf( path, ARRAY_SIZE(path), L"%s\\%s", schemes_path, name );
        size = sizeof(base);
        if (RegGetValueW( HKEY_LOCAL_MACHINE, path, L"ArcticBaseScheme", RRF_RT_REG_SZ, NULL, base, &size )) break;
        lstrcpyW( name, base );
    }
    return ERROR_FILE_NOT_FOUND;
}

DWORD WINAPI PowerReadACDefaultIndex( HKEY root, const GUID *scheme, const GUID *subgroup, const GUID *setting, DWORD *index )
{
    return read_default( scheme, subgroup, setting, TRUE, index );
}

DWORD WINAPI PowerReadDCDefaultIndex( HKEY root, const GUID *scheme, const GUID *subgroup, const GUID *setting, DWORD *index )
{
    return read_default( scheme, subgroup, setting, FALSE, index );
}

/* names: of a scheme (scheme only), of a subgroup (subgroup only), of a
 * setting (setting, with or without its subgroup) */
static DWORD open_named( const GUID *scheme, const GUID *subgroup, const GUID *setting, REGSAM access, HKEY *key )
{
    WCHAR path[256], name[40];

    if (setting) return open_setting( subgroup, setting, access, key );
    if (!no_subgroup( subgroup )) return open_setting( subgroup, NULL, access, key );
    if (!scheme) return ERROR_INVALID_PARAMETER;
    guid_str( scheme, name );
    swprintf( path, ARRAY_SIZE(path), L"%s\\%s", schemes_path, name );
    return RegOpenKeyExW( HKEY_LOCAL_MACHINE, path, 0, access, key );
}

static DWORD read_named( const GUID *scheme, const GUID *subgroup, const GUID *setting, const WCHAR *value,
                         UCHAR *buffer, DWORD *size )
{
    HKEY key;
    DWORD ret;

    if ((ret = open_named( scheme, subgroup, setting, KEY_READ, &key ))) return ret;
    ret = read_string( key, value, buffer, size );
    RegCloseKey( key );
    return ret;
}

static DWORD write_named( const GUID *scheme, const GUID *subgroup, const GUID *setting, const WCHAR *value,
                          const UCHAR *buffer, DWORD size )
{
    HKEY key;
    DWORD ret;

    if ((ret = open_named( scheme, subgroup, setting, KEY_WRITE, &key ))) return ret;
    ret = write_string( key, value, buffer, size );
    RegCloseKey( key );
    return ret;
}

DWORD WINAPI PowerReadFriendlyName( HKEY root, const GUID *scheme, const GUID *subgroup, const GUID *setting,
                                    UCHAR *buffer, DWORD *size )
{
    TRACE( "(%p, %s, %s, %s, %p, %p)\n", root, debugstr_guid(scheme), debugstr_guid(subgroup), debugstr_guid(setting),
           buffer, size );
    return read_named( scheme, subgroup, setting, L"FriendlyName", buffer, size );
}

DWORD WINAPI PowerReadDescription( HKEY root, const GUID *scheme, const GUID *subgroup, const GUID *setting,
                                   UCHAR *buffer, DWORD *size )
{
    return read_named( scheme, subgroup, setting, L"Description", buffer, size );
}

DWORD WINAPI PowerWriteFriendlyName( HKEY root, const GUID *scheme, const GUID *subgroup, const GUID *setting,
                                     UCHAR *buffer, DWORD size )
{
    DWORD ret = write_named( scheme, subgroup, setting, L"FriendlyName", buffer, size );
    if (!ret) notify_change();
    return ret;
}

DWORD WINAPI PowerWriteDescription( HKEY root, const GUID *scheme, const GUID *subgroup, const GUID *setting,
                                    UCHAR *buffer, DWORD size )
{
    return write_named( scheme, subgroup, setting, L"Description", buffer, size );
}

static DWORD read_setting_dword( const GUID *subgroup, const GUID *setting, const WCHAR *name, DWORD *value )
{
    HKEY key;
    DWORD ret;

    if (!value) return ERROR_INVALID_PARAMETER;
    if ((ret = open_setting( subgroup, setting, KEY_READ, &key ))) return ret;
    ret = get_dword( key, NULL, name, value );
    RegCloseKey( key );
    return ret;
}

DWORD WINAPI PowerReadValueMin( HKEY root, const GUID *subgroup, const GUID *setting, DWORD *value )
{
    return read_setting_dword( subgroup, setting, L"ValueMin", value );
}

DWORD WINAPI PowerReadValueMax( HKEY root, const GUID *subgroup, const GUID *setting, DWORD *value )
{
    return read_setting_dword( subgroup, setting, L"ValueMax", value );
}

DWORD WINAPI PowerReadValueIncrement( HKEY root, const GUID *subgroup, const GUID *setting, DWORD *value )
{
    return read_setting_dword( subgroup, setting, L"ValueIncrement", value );
}

DWORD WINAPI PowerReadValueUnitsSpecifier( HKEY root, const GUID *subgroup, const GUID *setting, UCHAR *buffer,
                                           DWORD *size )
{
    HKEY key;
    DWORD ret;

    if ((ret = open_setting( subgroup, setting, KEY_READ, &key ))) return ret;
    ret = read_string( key, L"ValueUnits", buffer, size );
    RegCloseKey( key );
    return ret;
}

DWORD WINAPI PowerReadSettingAttributes( const GUID *subgroup, const GUID *setting )
{
    DWORD attributes = 0;
    HKEY key;

    if (setting) read_setting_dword( subgroup, setting, L"Attributes", &attributes );
    else if (!open_setting( subgroup, NULL, KEY_READ, &key ))
    {
        get_dword( key, NULL, L"Attributes", &attributes );
        RegCloseKey( key );
    }
    return attributes;
}

DWORD WINAPI PowerWriteSettingAttributes( const GUID *subgroup, const GUID *setting, DWORD attributes )
{
    HKEY key;
    DWORD ret;

    if ((ret = open_setting( subgroup, setting, KEY_WRITE, &key ))) return ret;
    ret = RegSetValueExW( key, L"Attributes", 0, REG_DWORD, (BYTE *)&attributes, sizeof(attributes) );
    RegCloseKey( key );
    return ret;
}

/* the possible values of a setting with a list: keys 0, 1, 2... */
static DWORD open_possible( const GUID *subgroup, const GUID *setting, ULONG index, HKEY *key )
{
    WCHAR name[16];
    HKEY parent;
    DWORD ret;

    if ((ret = open_setting( subgroup, setting, KEY_READ, &parent ))) return ret;
    swprintf( name, ARRAY_SIZE(name), L"%lu", index );
    ret = RegOpenKeyExW( parent, name, 0, KEY_READ, key );
    RegCloseKey( parent );
    return ret;
}

DWORD WINAPI PowerReadPossibleValue( HKEY root, const GUID *subgroup, const GUID *setting, ULONG *type, ULONG index,
                                     UCHAR *buffer, DWORD *size )
{
    HKEY key;
    DWORD ret, value;

    if (!size) return ERROR_INVALID_PARAMETER;
    if ((ret = open_possible( subgroup, setting, index, &key ))) return ret;
    ret = get_dword( key, NULL, L"SettingValue", &value );
    RegCloseKey( key );
    if (ret) return ret;
    if (type) *type = REG_DWORD;
    if (!buffer)
    {
        *size = sizeof(DWORD);
        return ERROR_SUCCESS;
    }
    if (*size < sizeof(DWORD))
    {
        *size = sizeof(DWORD);
        return ERROR_MORE_DATA;
    }
    memcpy( buffer, &value, sizeof(value) );
    *size = sizeof(DWORD);
    return ERROR_SUCCESS;
}

DWORD WINAPI PowerReadPossibleFriendlyName( HKEY root, const GUID *subgroup, const GUID *setting, ULONG index,
                                            UCHAR *buffer, DWORD *size )
{
    HKEY key;
    DWORD ret;

    if ((ret = open_possible( subgroup, setting, index, &key ))) return ret;
    ret = read_string( key, L"FriendlyName", buffer, size );
    RegCloseKey( key );
    return ret;
}

DWORD WINAPI PowerReadPossibleDescription( HKEY root, const GUID *subgroup, const GUID *setting, ULONG index,
                                           UCHAR *buffer, DWORD *size )
{
    HKEY key;
    DWORD ret;

    if ((ret = open_possible( subgroup, setting, index, &key ))) return ret;
    ret = read_string( key, L"Description", buffer, size );
    RegCloseKey( key );
    return ret;
}

DWORD WINAPI PowerEnumerate( HKEY root, const GUID *scheme, const GUID *subgroup, POWER_DATA_ACCESSOR flags,
                             ULONG index, UCHAR *buffer, DWORD *size )
{
    WCHAR name[64];
    DWORD len, found = 0, ret = ERROR_NO_MORE_ITEMS;
    HKEY key;
    GUID guid;

    TRACE( "(%p, %s, %s, %d, %lu, %p, %p)\n", root, debugstr_guid(scheme), debugstr_guid(subgroup), flags, index,
           buffer, size );
    if (!size) return ERROR_INVALID_PARAMETER;

    switch (flags)
    {
    case ACCESS_SCHEME:
        if (RegOpenKeyExW( HKEY_LOCAL_MACHINE, schemes_path, 0, KEY_READ, &key )) return ERROR_NO_MORE_ITEMS;
        for (DWORD i = 0; len = ARRAY_SIZE(name), !RegEnumKeyExW( key, i, name, &len, NULL, NULL, NULL, NULL ); i++)
        {
            if (is_overlay( name ) || !parse_guid( name, &guid )) continue;
            if (found++ == index)
            {
                ret = copy_guid( &guid, buffer, size );
                break;
            }
        }
        RegCloseKey( key );
        return ret;

    case ACCESS_SUBGROUP:
        if (RegOpenKeyExW( HKEY_LOCAL_MACHINE, settings_path, 0, KEY_READ, &key )) return ERROR_NO_MORE_ITEMS;
        for (DWORD i = 0; len = ARRAY_SIZE(name), !RegEnumKeyExW( key, i, name, &len, NULL, NULL, NULL, NULL ); i++)
        {
            if (!parse_guid( name, &guid ) || is_setting_key( key, name )) continue;
            if (found++ == index)
            {
                ret = copy_guid( &guid, buffer, size );
                break;
            }
        }
        RegCloseKey( key );
        return ret;

    case ACCESS_INDIVIDUAL_SETTING:
        if (open_setting( subgroup, NULL, KEY_READ, &key )) return ERROR_NO_MORE_ITEMS;
        for (DWORD i = 0; len = ARRAY_SIZE(name), !RegEnumKeyExW( key, i, name, &len, NULL, NULL, NULL, NULL ); i++)
        {
            if (!parse_guid( name, &guid )) continue;
            /* right under PowerSettings: only the settings, not the subgroups */
            if (no_subgroup( subgroup ) && !is_setting_key( key, name )) continue;
            if (found++ == index)
            {
                ret = copy_guid( &guid, buffer, size );
                break;
            }
        }
        RegCloseKey( key );
        return ret;

    default:
        FIXME( "flags %d not supported\n", flags );
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

/* a copy of a scheme: its names and a link to it, so it follows its
 * defaults; the values the user had changed in it are copied */
static void copy_tree( HKEY from, HKEY to )
{
    WCHAR name[256];
    BYTE data[2048];
    DWORD len, size, type;

    for (DWORD i = 0; len = ARRAY_SIZE(name), size = sizeof(data),
         !RegEnumValueW( from, i, name, &len, NULL, &type, data, &size ); i++)
        RegSetValueExW( to, name, 0, type, data, size );
    for (DWORD i = 0; len = ARRAY_SIZE(name), !RegEnumKeyExW( from, i, name, &len, NULL, NULL, NULL, NULL ); i++)
    {
        HKEY src, dst;

        if (RegOpenKeyExW( from, name, 0, KEY_READ, &src )) continue;
        if (!RegCreateKeyExW( to, name, 0, NULL, 0, KEY_WRITE | KEY_READ, NULL, &dst, NULL ))
        {
            copy_tree( src, dst );
            RegCloseKey( dst );
        }
        RegCloseKey( src );
    }
}

DWORD WINAPI PowerDuplicateScheme( HKEY root, const GUID *source, GUID **destination )
{
    WCHAR from_name[40], to_name[40], path[256];
    HKEY from, to;
    GUID guid;
    DWORD ret;

    TRACE( "(%p, %s, %p)\n", root, debugstr_guid(source), destination );
    if (!source || !destination) return ERROR_INVALID_PARAMETER;
    guid_str( source, from_name );
    swprintf( path, ARRAY_SIZE(path), L"%s\\%s", schemes_path, from_name );
    if ((ret = RegOpenKeyExW( HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &from ))) return ret;
    if (*destination) guid = **destination;
    else
    {
        UuidCreate( &guid );
        if (!(*destination = LocalAlloc( 0, sizeof(GUID) )))
        {
            RegCloseKey( from );
            return ERROR_NOT_ENOUGH_MEMORY;
        }
        **destination = guid;
    }
    guid_str( &guid, to_name );
    swprintf( path, ARRAY_SIZE(path), L"%s\\%s", schemes_path, to_name );
    if (!(ret = RegCreateKeyExW( HKEY_LOCAL_MACHINE, path, 0, NULL, 0, KEY_WRITE | KEY_READ, NULL, &to, NULL )))
    {
        copy_tree( from, to );
        RegSetValueExW( to, L"ArcticBaseScheme", 0, REG_SZ, (BYTE *)from_name, (wcslen( from_name ) + 1) * sizeof(WCHAR) );
        RegCloseKey( to );
    }
    RegCloseKey( from );
    notify_change();
    return ret;
}

DWORD WINAPI PowerDeleteScheme( HKEY root, const GUID *scheme )
{
    WCHAR name[40], path[256], active[40];
    DWORD size = sizeof(active);

    if (!scheme) return ERROR_INVALID_PARAMETER;
    guid_str( scheme, name );
    /* neither the active scheme nor one of Windows' own */
    if (!RegGetValueW( HKEY_LOCAL_MACHINE, schemes_path, L"ActivePowerScheme", RRF_RT_REG_SZ, NULL, active, &size ) &&
        !wcsicmp( active, name ))
        return ERROR_ACCESS_DENIED;
    if (is_builtin( name ) || is_overlay( name )) return ERROR_ACCESS_DENIED;
    swprintf( path, ARRAY_SIZE(path), L"%s\\%s", schemes_path, name );
    notify_change();
    return RegDeleteTreeW( HKEY_LOCAL_MACHINE, path ) ? ERROR_FILE_NOT_FOUND : ERROR_SUCCESS;
}

/* back to what Windows' scheme is made with: the user's values go */
static void clear_values( const WCHAR *scheme )
{
    WCHAR path[256], name[64];
    BOOL found;
    DWORD len;
    HKEY key;

    swprintf( path, ARRAY_SIZE(path), L"%s\\%s", schemes_path, scheme );
    if (RegOpenKeyExW( HKEY_LOCAL_MACHINE, path, 0, KEY_READ | KEY_WRITE, &key )) return;
    do
    {
        GUID guid;

        found = FALSE;
        for (DWORD i = 0; len = ARRAY_SIZE(name), !RegEnumKeyExW( key, i, name, &len, NULL, NULL, NULL, NULL ); i++)
            if (parse_guid( name, &guid ))
            {
                found = !RegDeleteTreeW( key, name );
                break;
            }
    } while (found);
    RegCloseKey( key );
}

DWORD WINAPI PowerCanRestoreIndividualDefaultPowerScheme( const GUID *scheme )
{
    WCHAR name[40];

    if (!scheme) return ERROR_INVALID_PARAMETER;
    guid_str( scheme, name );
    return is_builtin( name ) ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND;
}

DWORD WINAPI PowerRestoreIndividualDefaultPowerScheme( const GUID *scheme )
{
    WCHAR name[40];

    if (!scheme) return ERROR_INVALID_PARAMETER;
    guid_str( scheme, name );
    if (!scheme_exists( name )) return ERROR_FILE_NOT_FOUND;
    /* a scheme the user made goes back to the one it came from */
    clear_values( name );
    notify_change();
    return ERROR_SUCCESS;
}

DWORD WINAPI PowerRestoreDefaultPowerSchemes(void)
{
    WCHAR name[64];
    DWORD len;
    HKEY key;

    if (RegOpenKeyExW( HKEY_LOCAL_MACHINE, schemes_path, 0, KEY_READ | KEY_WRITE, &key )) return ERROR_FILE_NOT_FOUND;
    for (DWORD i = 0; len = ARRAY_SIZE(name), !RegEnumKeyExW( key, i, name, &len, NULL, NULL, NULL, NULL );)
    {
        if (!is_builtin( name ) && !is_overlay( name ) && !RegDeleteTreeW( key, name )) continue;
        clear_values( name );
        i++;
    }
    RegSetValueExW( key, L"ActivePowerScheme", 0, REG_SZ, (BYTE *)builtin_schemes[0],
                    (wcslen( builtin_schemes[0] ) + 1) * sizeof(WCHAR) );
    RegDeleteValueW( key, L"ActiveOverlayAcPowerScheme" );
    RegDeleteValueW( key, L"ActiveOverlayDcPowerScheme" );
    RegCloseKey( key );
    notify_change();
    return ERROR_SUCCESS;
}

DWORD WINAPI PowerSettingAccessCheck( POWER_DATA_ACCESSOR flags, const GUID *guid )
{
    /* one user, who owns the machine */
    return ERROR_SUCCESS;
}

DWORD WINAPI PowerSettingAccessCheckEx( POWER_DATA_ACCESSOR flags, const GUID *guid, REGSAM access )
{
    return ERROR_SUCCESS;
}

/* The power slider: an overlay over the Balanced scheme for the power
 * source now (ActiveOverlayAcPowerScheme / ActiveOverlayDcPowerScheme);
 * none (GUID_NULL) is Windows' recommended middle. */
static BOOL on_mains(void)
{
    SYSTEM_POWER_STATUS status;

    return !GetSystemPowerStatus( &status ) || status.ACLineStatus != 0;
}

static DWORD get_overlay( BOOL ac, GUID *guid )
{
    WCHAR value[64];
    DWORD size = sizeof(value);

    *guid = null_guid;
    if (!RegGetValueW( HKEY_LOCAL_MACHINE, schemes_path, ac ? L"ActiveOverlayAcPowerScheme" : L"ActiveOverlayDcPowerScheme",
                       RRF_RT_REG_SZ, NULL, value, &size ))
        parse_guid( value, guid );
    return ERROR_SUCCESS;
}

DWORD WINAPI PowerGetActualOverlayScheme( GUID *guid )
{
    if (!guid) return ERROR_INVALID_PARAMETER;
    return get_overlay( on_mains(), guid );
}

DWORD WINAPI PowerGetEffectiveOverlayScheme( GUID *guid )
{
    GUID *active;

    if (!guid) return ERROR_INVALID_PARAMETER;
    /* the slider works over Balanced only, as in Windows */
    if (!PowerGetActiveScheme( NULL, &active ))
    {
        BOOL balanced = IsEqualGUID( active, &SCHEME_BALANCED );
        LocalFree( active );
        if (!balanced)
        {
            *guid = null_guid;
            return ERROR_SUCCESS;
        }
    }
    return get_overlay( on_mains(), guid );
}

DWORD WINAPI PowerSetActiveOverlayScheme( GUID *guid )
{
    WCHAR name[40];
    const WCHAR *value = on_mains() ? L"ActiveOverlayAcPowerScheme" : L"ActiveOverlayDcPowerScheme";
    HKEY key;
    LONG ret;

    if (!guid) return ERROR_INVALID_PARAMETER;
    guid_str( guid, name );
    if (!IsEqualGUID( guid, &null_guid ) && !is_overlay( name )) return ERROR_INVALID_PARAMETER;
    if ((ret = RegCreateKeyExW( HKEY_LOCAL_MACHINE, schemes_path, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL ))) return ret;
    ret = RegSetValueExW( key, value, 0, REG_SZ, (BYTE *)name, (wcslen( name ) + 1) * sizeof(WCHAR) );
    RegCloseKey( key );
    notify_change();
    return ret;
}

DWORD WINAPI PowerGetOverlaySchemes( GUID **overlays_out, DWORD *count, BYTE explicit_only )
{
    if (!overlays_out || !count) return ERROR_INVALID_PARAMETER;
    if (!(*overlays_out = LocalAlloc( 0, ARRAY_SIZE(overlays) * sizeof(GUID) ))) return ERROR_NOT_ENOUGH_MEMORY;
    for (UINT i = 0; i < ARRAY_SIZE(overlays); i++) parse_guid( overlays[i], &(*overlays_out)[i] );
    *count = ARRAY_SIZE(overlays);
    return ERROR_SUCCESS;
}

POWER_PLATFORM_ROLE WINAPI PowerDeterminePlatformRole(void)
{
    return PowerDeterminePlatformRoleEx( POWER_PLATFORM_ROLE_V1 );
}

POWER_PLATFORM_ROLE WINAPI PowerDeterminePlatformRoleEx( ULONG version )
{
    struct arctic_power_status status;

    TRACE( "(%lu)\n", version );
    if (version < POWER_PLATFORM_ROLE_V1 || version > POWER_PLATFORM_ROLE_V2) return PlatformRoleUnspecified;
    return ArcticPowerStatus( &status ) && status.battery_count ? PlatformRoleMobile : PlatformRoleDesktop;
}

DWORD WINAPI PowerRegisterSuspendResumeNotification( DWORD flags, HANDLE recipient, PHPOWERNOTIFY handle )
{
    FIXME( "(0x%08lx, %p, %p) stub!\n", flags, recipient, handle );
    *handle = (HPOWERNOTIFY)0xdeadbeef;
    return ERROR_SUCCESS;
}

DWORD WINAPI PowerUnregisterSuspendResumeNotification( HPOWERNOTIFY handle )
{
    FIXME( "(%p) stub!\n", handle );
    return ERROR_SUCCESS;
}

DWORD WINAPI PowerSettingRegisterNotification( const GUID *setting, DWORD flags, HANDLE recipient, PHPOWERNOTIFY handle )
{
    FIXME( "(%s,0x%08lx,%p,%p) stub!\n", debugstr_guid(setting), flags, recipient, handle );
    *handle = (PHPOWERNOTIFY)0xdeadbeef;
    return ERROR_SUCCESS;
}

DWORD WINAPI PowerSettingUnregisterNotification( HPOWERNOTIFY handle )
{
    FIXME( "(%p) stub!\n", handle );
    return ERROR_SUCCESS;
}

HRESULT WINAPI PowerRegisterForEffectivePowerModeNotifications( ULONG version, EFFECTIVE_POWER_MODE_CALLBACK *callback,
                                                                void *context, void **handle )
{
    GUID overlay;
    EFFECTIVE_POWER_MODE mode = EffectivePowerModeBalanced;
    DWORD saver = 0;

    TRACE( "(%lu, %p, %p, %p)\n", version, callback, context, handle );
    get_dword( HKEY_LOCAL_MACHINE, ARCTIC_POWER_KEY, ARCTIC_ENERGY_SAVER_ON, &saver );
    PowerGetEffectiveOverlayScheme( &overlay );
    if (saver) mode = EffectivePowerModeBatterySaver;
    else if (!memcmp( &overlay, &(GUID){ 0x961cc777, 0x2547, 0x4f9d, { 0x81, 0x74, 0x7d, 0x86, 0x18, 0x1b, 0x8a, 0x7a } }, sizeof(GUID) ))
        mode = EffectivePowerModeBetterBattery;
    else if (!memcmp( &overlay, &(GUID){ 0xded574b5, 0x45a0, 0x4f42, { 0x87, 0x37, 0x46, 0x34, 0x5c, 0x09, 0xc2, 0x38 } }, sizeof(GUID) ))
        mode = EffectivePowerModeMaxPerformance;
    else if (!memcmp( &overlay, &(GUID){ 0x3af9b8d9, 0x7c97, 0x431d, { 0xad, 0x78, 0x34, 0xa8, 0xbf, 0xea, 0x43, 0x9f } }, sizeof(GUID) ))
        mode = EffectivePowerModeHighPerformance;
    if (callback) callback( mode, context );
    *handle = (void *)0xdeadbeef;
    return S_OK;
}

HRESULT WINAPI PowerUnregisterFromEffectivePowerModeNotifications( void *handle )
{
    return S_OK;
}

BOOL WINAPI DllMain( HINSTANCE instance, DWORD reason, void *reserved )
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        powrprof_instance = instance;
        DisableThreadLibraryCalls( instance );
    }
    return TRUE;
}
