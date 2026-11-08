/*
 * Date and Time of the Control Panel: the page
 *
 * As the pages of Power Options are laid out: the task pane on the left with
 * the Control Panel's home, "Синхронізувати зараз" and, under "Див. також",
 * Notifications and System; on the right the time as it runs and today's
 * date, then three groups, each with a short line of what it does:
 *
 * - Час з інтернету: "Установлювати час автоматично", the time server (Windows'
 *   time.windows.com or another), "Синхронізувати зараз" and when it last was;
 * - Дата й час вручну: a date and a time, "Змінити", while the time is not
 *   set automatically;
 * - Часовий пояс: "Установлювати часовий пояс автоматично", every zone of
 *   the registry as Windows lists them (by their offset), and when the clock
 *   goes to summer or winter time next.
 *
 * Programs may not set the clock (NtSetSystemTime) and Wine has no zone of
 * its own: the host does it (arctic-init's serve_time, /run/arctic/time),
 * and a request that waits for the network runs on a thread of its own. The
 * zone goes to the host by its name in the tz database, which the table ntdll
 * names zones by (Arctic\Runtime\Time Zones\TZ Mapping) gives for the zone
 * chosen; every program follows /etc/localtime at once. What Windows keeps of
 * it is written too: TimeZoneInformation, W32Time's Type and NtpServer.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdio.h>
#include <stdlib.h>

#include "timedate.h"
#include "commctrl.h"
#include "shlwapi.h"

#define INDENT       px(20)
#define ROW_GAP      px(8)

#define ZONES_KEY    L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones"
#define MAPPING_KEY  L"Software\\Arctic\\Runtime\\Time Zones\\TZ Mapping"
#define W32TIME_KEY  L"SYSTEM\\CurrentControlSet\\Services\\W32Time\\Parameters"
#define TZINFO_KEY   L"SYSTEM\\CurrentControlSet\\Control\\TimeZoneInformation"

static const WCHAR *servers[] = { L"time.windows.com", L"time.nist.gov", L"pool.ntp.org", L"time.google.com" };

/**********************************************************************
 *          The host
 */

/* one request to arctic-init (serve_time) and its answer; TRUE for "done" */
static BOOL host_request( const char *request, char *answer, DWORD size, DWORD timeout )
{
    static const WCHAR request_file[] = L"\\\\?\\unix\\run\\arctic\\time\\request";
    static const WCHAR new_file[] = L"\\\\?\\unix\\run\\arctic\\time\\.request";
    static const WCHAR result_file[] = L"\\\\?\\unix\\run\\arctic\\time\\result";
    static SRWLOCK lock = SRWLOCK_INIT;
    char line[256];
    DWORD done;
    HANDLE file;
    BOOL ok = FALSE;

    answer[0] = 0;
    AcquireSRWLockExclusive( &lock );
    DeleteFileW( result_file );
    /* written whole before the host sees it */
    file = CreateFileW( new_file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL );
    if (file != INVALID_HANDLE_VALUE)
    {
        snprintf( line, sizeof(line), "%s\n", request );
        WriteFile( file, line, strlen( line ), &done, NULL );
        CloseHandle( file );
        if (!MoveFileExW( new_file, request_file, MOVEFILE_REPLACE_EXISTING )) timeout = 0;
        for (DWORD waited = 0; waited < timeout; waited += 50)
        {
            Sleep( 50 );
            file = CreateFileW( result_file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
                                OPEN_EXISTING, 0, NULL );
            if (file == INVALID_HANDLE_VALUE) continue;
            done = 0;
            ReadFile( file, answer, size - 1, &done, NULL );
            CloseHandle( file );
            answer[done] = 0;
            DeleteFileW( result_file );
            ok = !strncmp( answer, "done", 4 );
            break;
        }
    }
    ReleaseSRWLockExclusive( &lock );
    return ok;
}

/* what the host keeps */
static struct
{
    WCHAR server[128];       /* the time server, when the time is set automatically */
    BOOL auto_time;
    BOOL auto_zone;
    char zone[96];           /* the zone's name in the tz database */
    LONGLONG synced;         /* when the clock was last set from a server, 0 never */
    WCHAR synced_server[128];
} host;

static void load_host_state(void)
{
    char answer[512], *line, *next;

    memset( &host, 0, sizeof(host) );
    lstrcpyW( host.server, servers[0] );
    host.auto_time = TRUE;
    if (!host_request( "state", answer, sizeof(answer), 3000 )) return;
    for (line = answer + 5; line && *line; line = next)
    {
        char *value;

        if ((next = strchr( line, '\n' ))) *next++ = 0;
        if (!(value = strchr( line, '=' ))) continue;
        *value++ = 0;
        if (!strcmp( line, "server" ))
        {
            host.auto_time = strcmp( value, "off" ) != 0;
            if (host.auto_time && value[0])
                MultiByteToWideChar( CP_UTF8, 0, value, -1, host.server, ARRAY_SIZE(host.server) );
        }
        else if (!strcmp( line, "zone" )) lstrcpynA( host.zone, value, sizeof(host.zone) );
        else if (!strcmp( line, "zone-auto" )) host.auto_zone = !strcmp( value, "on" );
        else if (!strcmp( line, "synced" ) && value[0])
        {
            char *space;

            host.synced = _strtoi64( value, &space, 10 );
            if (*space == ' ') MultiByteToWideChar( CP_UTF8, 0, space + 1, -1, host.synced_server,
                                                    ARRAY_SIZE(host.synced_server) );
        }
    }
}

/**********************************************************************
 *          Requests that wait: on a thread of their own
 */

enum job_kind { JOB_NONE, JOB_SYNC, JOB_SET, JOB_ZONE, JOB_AUTO_TIME, JOB_AUTO_ZONE };

static struct
{
    volatile LONG busy;      /* a thread runs it */
    volatile LONG finished;  /* ... and is done, the page has not seen it yet */
    enum job_kind kind;
    char requests[3][160];
    int count;
    BOOL ok;
    char answer[256];
} job;

static DWORD WINAPI job_thread( void *arg )
{
    job.ok = TRUE;
    for (int i = 0; i < job.count && job.ok; i++)
        job.ok = host_request( job.requests[i], job.answer, sizeof(job.answer), 20000 );
    /* the clock or the zone moved: every program's clock with them */
    if (job.ok && job.kind != JOB_AUTO_TIME)
        SendMessageTimeoutW( HWND_BROADCAST, WM_TIMECHANGE, 0, 0, SMTO_ABORTIFHUNG, 2000, NULL );
    InterlockedExchange( &job.finished, 1 );
    return 0;
}

static BOOL start_job( enum job_kind kind, const char *first, const char *second )
{
    HANDLE thread;

    if (InterlockedCompareExchange( &job.busy, 1, 0 )) return FALSE;
    job.kind = kind;
    job.count = 0;
    lstrcpynA( job.requests[job.count++], first, sizeof(job.requests[0]) );
    if (second) lstrcpynA( job.requests[job.count++], second, sizeof(job.requests[0]) );
    job.finished = 0;
    if (!(thread = CreateThread( NULL, 0, job_thread, NULL, 0, NULL )))
    {
        job.busy = 0;
        return FALSE;
    }
    CloseHandle( thread );
    return TRUE;
}

/**********************************************************************
 *          The zones
 */

struct tzi_reg
{
    LONG bias, standard_bias, daylight_bias;
    SYSTEMTIME standard_date, daylight_date;
};

struct zone
{
    WCHAR key[128];
    WCHAR display[256];
    WCHAR std[64], dlt[64];
    struct tzi_reg tzi;
};

static struct zone *zones;
static UINT zone_count;
static WCHAR current_zone[128];

static int compare_zones( const void *a, const void *b )
{
    const struct zone *za = a, *zb = b;

    /* as Windows lists them: from UTC-12 to UTC+14, then by name */
    if (za->tzi.bias != zb->tzi.bias) return za->tzi.bias > zb->tzi.bias ? -1 : 1;
    return lstrcmpiW( za->display, zb->display );
}

static void read_string( HKEY key, const WCHAR *mui, const WCHAR *name, WCHAR *value, DWORD count )
{
    DWORD size = count * sizeof(WCHAR);

    value[0] = 0;
    if (mui && !RegLoadMUIStringW( key, mui, value, size, NULL, 0, NULL ) && value[0]) return;
    if (RegGetValueW( key, NULL, name, RRF_RT_REG_SZ, NULL, value, &size )) value[0] = 0;
}

static void load_zones(void)
{
    WCHAR name[128];
    HKEY root, key;
    DWORD size;

    if (zones) return;
    if (RegOpenKeyExW( HKEY_LOCAL_MACHINE, ZONES_KEY, 0, KEY_READ, &root )) return;
    for (DWORD i = 0, len = ARRAY_SIZE(name); !RegEnumKeyExW( root, i, name, &len, NULL, NULL, NULL, NULL );
         i++, len = ARRAY_SIZE(name))
    {
        struct zone *zone, *grown;

        if (RegOpenKeyExW( root, name, 0, KEY_READ, &key )) continue;
        if (!(grown = realloc( zones, (zone_count + 1) * sizeof(*zones) )))
        {
            RegCloseKey( key );
            break;
        }
        zones = grown;
        zone = &zones[zone_count];
        memset( zone, 0, sizeof(*zone) );
        lstrcpynW( zone->key, name, ARRAY_SIZE(zone->key) );
        read_string( key, L"MUI_Display", L"Display", zone->display, ARRAY_SIZE(zone->display) );
        read_string( key, L"MUI_Std", L"Std", zone->std, ARRAY_SIZE(zone->std) );
        read_string( key, L"MUI_Dlt", L"Dlt", zone->dlt, ARRAY_SIZE(zone->dlt) );
        size = sizeof(zone->tzi);
        if (!RegGetValueW( key, NULL, L"TZI", RRF_RT_REG_BINARY, NULL, &zone->tzi, &size ) && zone->display[0])
            zone_count++;
        RegCloseKey( key );
    }
    RegCloseKey( root );
    qsort( zones, zone_count, sizeof(*zones), compare_zones );
}

static struct zone *find_zone( const WCHAR *key )
{
    for (UINT i = 0; i < zone_count; i++) if (!lstrcmpiW( zones[i].key, key )) return &zones[i];
    return NULL;
}

/* the Windows zone a name of the tz database is */
static BOOL mapped_zone( const char *unix_name, WCHAR *key, DWORD count )
{
    WCHAR name[96];
    DWORD size = count * sizeof(WCHAR);

    MultiByteToWideChar( CP_UTF8, 0, unix_name, -1, name, ARRAY_SIZE(name) );
    return !RegGetValueW( HKEY_LOCAL_MACHINE, MAPPING_KEY, name, RRF_RT_REG_SZ, NULL, key, &size );
}

/* the zone of the tz database for a Windows zone: the host's own when it is
 * that zone, the one CLDR names first for it ("001"), or the first in the
 * table under a continent's name */
static BOOL unix_zone( const WCHAR *key, char *unix_name, DWORD size )
{
    static const struct { const WCHAR *key; const char *names; } golden[] =
    {
        { L"FLE Standard Time", "Europe/Kyiv Europe/Kiev" },
        { L"Central European Standard Time", "Europe/Warsaw" },
        { L"Central Europe Standard Time", "Europe/Budapest" },
        { L"W. Europe Standard Time", "Europe/Berlin" },
        { L"Romance Standard Time", "Europe/Paris" },
        { L"GMT Standard Time", "Europe/London" },
        { L"Greenwich Standard Time", "Atlantic/Reykjavik" },
        { L"GTB Standard Time", "Europe/Bucharest" },
        { L"E. Europe Standard Time", "Europe/Chisinau" },
        { L"Russian Standard Time", "Europe/Moscow" },
        { L"Belarus Standard Time", "Europe/Minsk" },
        { L"Turkey Standard Time", "Europe/Istanbul" },
        { L"Kaliningrad Standard Time", "Europe/Kaliningrad" },
        { L"Israel Standard Time", "Asia/Jerusalem" },
        { L"Egypt Standard Time", "Africa/Cairo" },
        { L"South Africa Standard Time", "Africa/Johannesburg" },
        { L"W. Central Africa Standard Time", "Africa/Lagos" },
        { L"E. Africa Standard Time", "Africa/Nairobi" },
        { L"Morocco Standard Time", "Africa/Casablanca" },
        { L"Arabic Standard Time", "Asia/Baghdad" },
        { L"Arab Standard Time", "Asia/Riyadh" },
        { L"Arabian Standard Time", "Asia/Dubai" },
        { L"Iran Standard Time", "Asia/Tehran" },
        { L"Georgian Standard Time", "Asia/Tbilisi" },
        { L"Caucasus Standard Time", "Asia/Yerevan" },
        { L"Azerbaijan Standard Time", "Asia/Baku" },
        { L"Afghanistan Standard Time", "Asia/Kabul" },
        { L"West Asia Standard Time", "Asia/Tashkent" },
        { L"Pakistan Standard Time", "Asia/Karachi" },
        { L"India Standard Time", "Asia/Kolkata Asia/Calcutta" },
        { L"Nepal Standard Time", "Asia/Kathmandu Asia/Katmandu" },
        { L"Bangladesh Standard Time", "Asia/Dhaka" },
        { L"SE Asia Standard Time", "Asia/Bangkok" },
        { L"China Standard Time", "Asia/Shanghai" },
        { L"Singapore Standard Time", "Asia/Singapore" },
        { L"Taipei Standard Time", "Asia/Taipei" },
        { L"Tokyo Standard Time", "Asia/Tokyo" },
        { L"Korea Standard Time", "Asia/Seoul" },
        { L"Ekaterinburg Standard Time", "Asia/Yekaterinburg" },
        { L"N. Central Asia Standard Time", "Asia/Novosibirsk" },
        { L"North Asia Standard Time", "Asia/Krasnoyarsk" },
        { L"Vladivostok Standard Time", "Asia/Vladivostok" },
        { L"AUS Eastern Standard Time", "Australia/Sydney" },
        { L"E. Australia Standard Time", "Australia/Brisbane" },
        { L"Cen. Australia Standard Time", "Australia/Adelaide" },
        { L"AUS Central Standard Time", "Australia/Darwin" },
        { L"W. Australia Standard Time", "Australia/Perth" },
        { L"New Zealand Standard Time", "Pacific/Auckland" },
        { L"Hawaiian Standard Time", "Pacific/Honolulu" },
        { L"Alaskan Standard Time", "America/Anchorage" },
        { L"Pacific Standard Time", "America/Los_Angeles" },
        { L"Mountain Standard Time", "America/Denver" },
        { L"US Mountain Standard Time", "America/Phoenix" },
        { L"Central Standard Time", "America/Chicago" },
        { L"Eastern Standard Time", "America/New_York" },
        { L"Atlantic Standard Time", "America/Halifax" },
        { L"Newfoundland Standard Time", "America/St_Johns" },
        { L"Central Standard Time (Mexico)", "America/Mexico_City" },
        { L"SA Pacific Standard Time", "America/Bogota" },
        { L"Argentina Standard Time", "America/Argentina/Buenos_Aires America/Buenos_Aires" },
        { L"E. South America Standard Time", "America/Sao_Paulo" },
        { L"UTC", "Etc/UTC UTC" },
    };
    static const char *continents[] = { "Europe/", "America/", "Asia/", "Africa/", "Australia/", "Pacific/",
                                        "Atlantic/", "Indian/", "Antarctica/" };
    WCHAR mapped[128], name[96];
    char candidate[96], best[96] = "";
    HKEY hkey;

    /* the host's own */
    if (host.zone[0] && mapped_zone( host.zone, mapped, ARRAY_SIZE(mapped) ) && !lstrcmpiW( mapped, key ))
    {
        lstrcpynA( unix_name, host.zone, size );
        return TRUE;
    }
    /* CLDR's first */
    for (UINT i = 0; i < ARRAY_SIZE(golden); i++)
    {
        const char *p = golden[i].names;

        if (lstrcmpiW( golden[i].key, key )) continue;
        while (*p)
        {
            int len = strcspn( p, " " );

            snprintf( candidate, sizeof(candidate), "%.*s", len, p );
            p += len + (p[len] == ' ');
            if (mapped_zone( candidate, mapped, ARRAY_SIZE(mapped) ) && !lstrcmpiW( mapped, key ))
            {
                lstrcpynA( unix_name, candidate, size );
                return TRUE;
            }
            if (!best[0]) lstrcpynA( best, candidate, sizeof(best) );
        }
    }
    /* any in the table, a continent's before the others */
    if (!RegOpenKeyExW( HKEY_LOCAL_MACHINE, MAPPING_KEY, 0, KEY_READ, &hkey ))
    {
        BOOL best_continent = FALSE;

        for (DWORD i = 0, len = ARRAY_SIZE(name), msize = sizeof(mapped);
             !RegEnumValueW( hkey, i, name, &len, NULL, NULL, (BYTE *)mapped, &msize );
             i++, len = ARRAY_SIZE(name), msize = sizeof(mapped))
        {
            BOOL continent = FALSE;

            if (lstrcmpiW( mapped, key )) continue;
            WideCharToMultiByte( CP_UTF8, 0, name, -1, candidate, sizeof(candidate), NULL, NULL );
            for (UINT c = 0; c < ARRAY_SIZE(continents); c++)
                if (!strncmp( candidate, continents[c], strlen( continents[c] ) )) continent = TRUE;
            if (!best[0] || (continent && !best_continent))
            {
                lstrcpynA( best, candidate, sizeof(best) );
                best_continent = continent;
            }
        }
        RegCloseKey( hkey );
    }
    if (!best[0]) return FALSE;
    lstrcpynA( unix_name, best, size );
    return TRUE;
}

/* which of the zones is the system's now */
static void find_current_zone(void)
{
    DYNAMIC_TIME_ZONE_INFORMATION info;

    current_zone[0] = 0;
    if (host.zone[0] && mapped_zone( host.zone, current_zone, ARRAY_SIZE(current_zone) ) && find_zone( current_zone ))
        return;
    if (GetDynamicTimeZoneInformation( &info ) != TIME_ZONE_ID_INVALID)
        lstrcpynW( current_zone, info.TimeZoneKeyName, ARRAY_SIZE(current_zone) );
}

/* what Windows keeps of the zone, for programs that read it */
static void write_zone_info( const struct zone *zone )
{
    DWORD zero = 0;
    HKEY key;

    if (RegCreateKeyExW( HKEY_LOCAL_MACHINE, TZINFO_KEY, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL )) return;
    RegSetValueExW( key, L"Bias", 0, REG_DWORD, (const BYTE *)&zone->tzi.bias, sizeof(DWORD) );
    RegSetValueExW( key, L"StandardBias", 0, REG_DWORD, (const BYTE *)&zone->tzi.standard_bias, sizeof(DWORD) );
    RegSetValueExW( key, L"DaylightBias", 0, REG_DWORD, (const BYTE *)&zone->tzi.daylight_bias, sizeof(DWORD) );
    RegSetValueExW( key, L"StandardStart", 0, REG_BINARY, (const BYTE *)&zone->tzi.standard_date, sizeof(SYSTEMTIME) );
    RegSetValueExW( key, L"DaylightStart", 0, REG_BINARY, (const BYTE *)&zone->tzi.daylight_date, sizeof(SYSTEMTIME) );
    RegSetValueExW( key, L"StandardName", 0, REG_SZ, (const BYTE *)zone->std, (lstrlenW( zone->std ) + 1) * sizeof(WCHAR) );
    RegSetValueExW( key, L"DaylightName", 0, REG_SZ, (const BYTE *)zone->dlt, (lstrlenW( zone->dlt ) + 1) * sizeof(WCHAR) );
    RegSetValueExW( key, L"TimeZoneKeyName", 0, REG_SZ, (const BYTE *)zone->key, (lstrlenW( zone->key ) + 1) * sizeof(WCHAR) );
    RegSetValueExW( key, L"DynamicDaylightTimeDisabled", 0, REG_DWORD, (const BYTE *)&zero, sizeof(zero) );
    RegCloseKey( key );
}

/* W32Time's own settings, as Windows' Date and Time writes them */
static void write_w32time( BOOL automatic, const WCHAR *server )
{
    WCHAR value[160];
    HKEY key;

    if (RegCreateKeyExW( HKEY_LOCAL_MACHINE, W32TIME_KEY, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL )) return;
    lstrcpyW( value, automatic ? L"NTP" : L"NoSync" );
    RegSetValueExW( key, L"Type", 0, REG_SZ, (const BYTE *)value, (lstrlenW( value ) + 1) * sizeof(WCHAR) );
    swprintf( value, ARRAY_SIZE(value), L"%s,0x9", server );
    RegSetValueExW( key, L"NtpServer", 0, REG_SZ, (const BYTE *)value, (lstrlenW( value ) + 1) * sizeof(WCHAR) );
    RegCloseKey( key );
}

/**********************************************************************
 *          Summer and winter time
 */

static int month_days( int year, int month )
{
    static const int days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return month == 2 && ((year % 4 == 0 && year % 100) || year % 400 == 0) ? 29 : days[month - 1];
}

static int week_day( int year, int month, int day )
{
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (month < 3) year--;
    return (year + year / 4 - year / 100 + year / 400 + t[month - 1] + day) % 7;
}

/* when a rule of the zone falls in a year: "the last Sunday of March" */
static void rule_date( const SYSTEMTIME *rule, int year, SYSTEMTIME *out )
{
    *out = *rule;
    if (rule->wYear) return;
    out->wYear = year;
    out->wDay = 1 + (rule->wDayOfWeek - week_day( year, rule->wMonth, 1 ) + 7) % 7 + (rule->wDay - 1) * 7;
    while (out->wDay > month_days( year, rule->wMonth )) out->wDay -= 7;
}

static ULONGLONG ticks( const SYSTEMTIME *st )
{
    FILETIME ft;

    if (!SystemTimeToFileTime( st, &ft )) return 0;
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

/* the next change of the clock in the zone, in its local time */
static void dst_text( const struct zone *zone, WCHAR *text, DWORD count )
{
    SYSTEMTIME now, best = { 0 }, when;
    ULONGLONG now_ticks, best_ticks = 0;
    BOOL to_summer = FALSE;
    WCHAR date[96], time[32];

    if (!zone->tzi.daylight_date.wMonth || !zone->tzi.standard_date.wMonth)
    {
        lstrcpynW( text, load_string( IDS_NO_DST ), count );
        return;
    }
    GetLocalTime( &now );
    now_ticks = ticks( &now );
    for (int year = now.wYear; year <= now.wYear + 1; year++)
    {
        for (int summer = 0; summer < 2; summer++)
        {
            ULONGLONG t;

            rule_date( summer ? &zone->tzi.daylight_date : &zone->tzi.standard_date, year, &when );
            t = ticks( &when );
            if (t > now_ticks && (!best_ticks || t < best_ticks))
            {
                best_ticks = t;
                best = when;
                to_summer = summer;
            }
        }
    }
    if (!best_ticks)
    {
        lstrcpynW( text, load_string( IDS_NO_DST ), count );
        return;
    }
    if (!GetDateFormatEx( LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, &best, NULL, date, ARRAY_SIZE(date), NULL ))
        date[0] = 0;
    if (!GetTimeFormatEx( LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &best, NULL, time, ARRAY_SIZE(time) ))
        time[0] = 0;
    lstrcpynW( text, format_string( to_summer ? IDS_DST_START : IDS_DST_END, date, time ), count );
}

/**********************************************************************
 *          What the page shows
 */

static WCHAR status[256];     /* the line under the time server */
static BOOL manual_touched;   /* the date or time picked by hand since the page came */
static HFONT clock_font;

static void sync_status(void)
{
    if (!host.auto_time && !host.synced)
    {
        lstrcpynW( status, load_string( IDS_SYNC_OFF ), ARRAY_SIZE(status) );
        return;
    }
    if (!host.synced)
    {
        lstrcpynW( status, load_string( IDS_NEVER_SYNCED ), ARRAY_SIZE(status) );
        return;
    }
    {
        LONGLONG ft = host.synced * 10000000 + 116444736000000000LL;
        FILETIME utc = { (DWORD)ft, (DWORD)(ft >> 32) };
        SYSTEMTIME st_utc, local;
        WCHAR date[96], time[32], when[160];

        FileTimeToSystemTime( &utc, &st_utc );
        SystemTimeToTzSpecificLocalTime( NULL, &st_utc, &local );
        if (!GetDateFormatEx( LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, &local, NULL, date, ARRAY_SIZE(date), NULL ))
            date[0] = 0;
        if (!GetTimeFormatEx( LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &local, NULL, time, ARRAY_SIZE(time) ))
            time[0] = 0;
        swprintf( when, ARRAY_SIZE(when), L"%s, %s", date, time );
        lstrcpynW( status, format_string( IDS_SYNCED, host.synced_server[0] ? host.synced_server : host.server, when ),
                   ARRAY_SIZE(status) );
    }
}

/* the time as it runs, and today */
static void paint_clock( struct view *view, HDC hdc, const RECT *rect, UINT_PTR param )
{
    SYSTEMTIME now;
    WCHAR time[32], date[96];
    RECT line = *rect;
    HGDIOBJ old;

    if (!clock_font)
    {
        NONCLIENTMETRICSW metrics = { sizeof(metrics) };

        SystemParametersInfoW( SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0 );
        metrics.lfMessageFont.lfHeight = -px( 36 );
        metrics.lfMessageFont.lfWeight = FW_LIGHT;
        metrics.lfMessageFont.lfQuality = CLEARTYPE_QUALITY;
        clock_font = CreateFontIndirectW( &metrics.lfMessageFont );
    }
    GetLocalTime( &now );
    if (!GetTimeFormatEx( LOCALE_NAME_USER_DEFAULT, 0, &now, NULL, time, ARRAY_SIZE(time) )) time[0] = 0;
    if (!GetDateFormatEx( LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, &now, NULL, date, ARRAY_SIZE(date), NULL ))
        date[0] = 0;
    SetBkMode( hdc, TRANSPARENT );
    line.left += INDENT;
    old = SelectObject( hdc, clock_font );
    SetTextColor( hdc, COLOR_TEXT );
    DrawTextW( hdc, time, -1, &line, DT_SINGLELINE | DT_NOPREFIX | DT_TOP );
    line.top += px( 46 );
    SelectObject( hdc, view_font( STYLE_BODY ) );
    SetTextColor( hdc, COLOR_GRAY );
    DrawTextW( hdc, date, -1, &line, DT_SINGLELINE | DT_NOPREFIX | DT_TOP );
    SelectObject( hdc, old );
}

static void paint_status( struct view *view, HDC hdc, const RECT *rect, UINT_PTR param )
{
    RECT line = *rect;
    HGDIOBJ old = SelectObject( hdc, view_font( STYLE_BODY ) );

    line.left += INDENT;
    SetBkMode( hdc, TRANSPARENT );
    SetTextColor( hdc, COLOR_GRAY );
    DrawTextW( hdc, status, -1, &line, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS | DT_VCENTER );
    SelectObject( hdc, old );
}

static void server_text( struct view *view, WCHAR *server, DWORD count )
{
    HWND combo = GetDlgItem( view_window( view ), IDC_SERVER );
    WCHAR *p;

    server[0] = 0;
    if (combo) GetWindowTextW( combo, server, count );
    StrTrimW( server, L" \t" );
    for (p = server; *p; p++)
        if (!(*p >= 'a' && *p <= 'z') && !(*p >= 'A' && *p <= 'Z') && !(*p >= '0' && *p <= '9') &&
            *p != '.' && *p != '-')
            break;
    if (*p || !server[0]) lstrcpynW( server, host.server, count );
}

static void sync_now( struct view *view, UINT_PTR param )
{
    WCHAR server[128];
    char request[160];

    server_text( view, server, ARRAY_SIZE(server) );
    snprintf( request, sizeof(request), "sync %ls", server );
    if (start_job( JOB_SYNC, request, NULL ))
    {
        lstrcpynW( status, load_string( IDS_SYNCING ), ARRAY_SIZE(status) );
        InvalidateRect( view_window( view ), NULL, FALSE );
    }
}

static void open_notifications( struct view *view, UINT_PTR param )
{
    cp_run( L"control.exe", L"notifications.cpl" );
}

static void open_system( struct view *view, UINT_PTR param )
{
    cp_run( L"control.exe", L"/name Microsoft.System" );
}

/* the date and time pickers at the time now, unless picked by hand */
static void set_pickers( struct view *view )
{
    HWND content = view_window( view ), focus = GetFocus();
    HWND date = GetDlgItem( content, IDC_DATE ), time = GetDlgItem( content, IDC_TIME );
    SYSTEMTIME now;

    if (manual_touched || !date || !time || focus == date || focus == time || IsChild( date, focus ) ||
        IsChild( time, focus ))
        return;
    GetLocalTime( &now );
    SendMessageW( date, DTM_SETSYSTEMTIME, GDT_VALID, (LPARAM)&now );
    SendMessageW( time, DTM_SETSYSTEMTIME, GDT_VALID, (LPARAM)&now );
}

void datetime_build( struct view *view )
{
    static BOOL loaded;
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_DATE_CLASSES };
    /* the page has no width yet at its first build: the controls take their own */
    int width = px( 460 );
    WCHAR text[256];
    HWND hwnd;

    InitCommonControlsEx( &icc );
    if (!loaded || !job.busy)
    {
        load_host_state();
        load_zones();
        find_current_zone();
        if (!job.busy) sync_status();
        loaded = TRUE;
    }

    view_nav_home( view );
    view_nav_link( view, load_string( IDS_SYNC_NOW ), sync_now, 0 );
    view_nav_see_also( view, load_string( IDS_NOTIFICATIONS ), open_notifications, 0 );
    view_nav_see_also( view, load_string( IDS_SYSTEM ), open_system, 0 );

    view_text( view, STYLE_TITLE, load_string( IDS_TITLE ) );
    view_space( view, px( 8 ) );
    view_paint_area( view, px( 72 ), paint_clock, 0 );
    view_space( view, px( 10 ) );

    /* Час з інтернету */
    view_group( view, load_string( IDS_TIME_GROUP ) );
    hwnd = view_control( view, L"Button", load_string( IDS_AUTO_TIME ), BS_AUTOCHECKBOX | WS_TABSTOP, px( 520 ), px( 20 ),
                         INDENT, IDC_AUTO_TIME );
    SendMessageW( hwnd, BM_SETCHECK, host.auto_time ? BST_CHECKED : BST_UNCHECKED, 0 );
    view_text_at( view, STYLE_GRAY, load_string( IDS_AUTO_TIME_HINT ), INDENT + px( 18 ) );
    view_space( view, ROW_GAP );
    view_row_begin( view );
    view_text_at( view, STYLE_BODY, load_string( IDS_SERVER ), INDENT );
    hwnd = view_control_beside( view, L"ComboBox", L"", CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP,
                                INDENT + px( 64 ), px( 200 ), px( 160 ), IDC_SERVER );
    for (UINT i = 0; i < ARRAY_SIZE(servers); i++) SendMessageW( hwnd, CB_ADDSTRING, 0, (LPARAM)servers[i] );
    SetWindowTextW( hwnd, host.server );
    view_control_beside( view, L"Button", load_string( IDS_SYNC_NOW ), BS_PUSHBUTTON | WS_TABSTOP,
                         INDENT + px( 274 ), px( 170 ), px( 25 ), IDC_SYNC );
    view_row_end( view );
    view_paint_area( view, px( 24 ), paint_status, 0 );
    view_space( view, px( 12 ) );

    /* Дата й час вручну */
    view_group( view, load_string( IDS_MANUAL_GROUP ) );
    view_row_begin( view );
    hwnd = view_control_beside( view, DATETIMEPICK_CLASSW, L"", DTS_SHORTDATEFORMAT | WS_TABSTOP, INDENT, px( 150 ),
                                px( 23 ), IDC_DATE );
    EnableWindow( hwnd, !host.auto_time );
    hwnd = view_control_beside( view, DATETIMEPICK_CLASSW, L"", DTS_TIMEFORMAT | DTS_UPDOWN | WS_TABSTOP,
                                INDENT + px( 160 ), px( 110 ), px( 23 ), IDC_TIME );
    EnableWindow( hwnd, !host.auto_time );
    hwnd = view_control_beside( view, L"Button", load_string( IDS_CHANGE ), BS_PUSHBUTTON | WS_TABSTOP,
                                INDENT + px( 280 ), px( 100 ), px( 25 ), IDC_SET );
    EnableWindow( hwnd, !host.auto_time );
    view_row_end( view );
    manual_touched = FALSE;
    set_pickers( view );
    if (host.auto_time)
    {
        view_space( view, px( 4 ) );
        view_text_at( view, STYLE_GRAY, load_string( IDS_MANUAL_HINT ), INDENT );
    }
    view_space( view, px( 16 ) );

    /* Часовий пояс */
    view_group( view, load_string( IDS_ZONE_GROUP ) );
    hwnd = view_control( view, L"Button", load_string( IDS_AUTO_ZONE ), BS_AUTOCHECKBOX | WS_TABSTOP, px( 520 ), px( 20 ),
                         INDENT, IDC_AUTO_ZONE );
    SendMessageW( hwnd, BM_SETCHECK, host.auto_zone ? BST_CHECKED : BST_UNCHECKED, 0 );
    view_text_at( view, STYLE_GRAY, load_string( IDS_AUTO_ZONE_HINT ), INDENT + px( 18 ) );
    view_space( view, ROW_GAP );
    hwnd = view_control( view, L"ComboBox", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, width, px( 320 ),
                         INDENT, IDC_ZONE );
    for (UINT i = 0; i < zone_count; i++)
    {
        SendMessageW( hwnd, CB_ADDSTRING, 0, (LPARAM)zones[i].display );
        if (!lstrcmpiW( zones[i].key, current_zone )) SendMessageW( hwnd, CB_SETCURSEL, i, 0 );
    }
    EnableWindow( hwnd, !host.auto_zone );
    view_space( view, px( 6 ) );
    {
        struct zone *zone = find_zone( current_zone );

        if (zone)
        {
            dst_text( zone, text, ARRAY_SIZE(text) );
            view_text_at( view, STYLE_GRAY, text, INDENT );
        }
    }
    view_space( view, px( 12 ) );

    view_set_timer( view, 1000 );
}

static void enable( struct view *view, UINT id, BOOL on )
{
    HWND hwnd = GetDlgItem( view_window( view ), id );

    if (hwnd) EnableWindow( hwnd, on );
}

BOOL datetime_command( struct view *view, UINT id, UINT code, HWND control )
{
    WCHAR server[128];
    char request[160], second[160];

    switch (id)
    {
    case IDC_AUTO_TIME:
        if (code != BN_CLICKED) return FALSE;
        server_text( view, server, ARRAY_SIZE(server) );
        host.auto_time = SendMessageW( control, BM_GETCHECK, 0, 0 ) == BST_CHECKED;
        write_w32time( host.auto_time, server );
        if (host.auto_time)
        {
            snprintf( request, sizeof(request), "auto %ls", server );
            snprintf( second, sizeof(second), "sync %ls", server );
            if (start_job( JOB_AUTO_TIME, request, second ))
                lstrcpynW( status, load_string( IDS_SYNCING ), ARRAY_SIZE(status) );
        }
        else start_job( JOB_AUTO_TIME, "auto off", NULL );
        enable( view, IDC_DATE, !host.auto_time );
        enable( view, IDC_TIME, !host.auto_time );
        enable( view, IDC_SET, !host.auto_time );
        return TRUE;

    case IDC_SERVER:
        if (code != CBN_KILLFOCUS && code != CBN_SELENDOK) return FALSE;
        if (code == CBN_SELENDOK)
        {
            int sel = SendMessageW( control, CB_GETCURSEL, 0, 0 );
            if (sel >= 0 && sel < (int)ARRAY_SIZE(servers)) SetWindowTextW( control, servers[sel] );
        }
        server_text( view, server, ARRAY_SIZE(server) );
        if (!lstrcmpiW( server, host.server )) return TRUE;
        lstrcpynW( host.server, server, ARRAY_SIZE(host.server) );
        write_w32time( host.auto_time, server );
        if (host.auto_time)
        {
            snprintf( request, sizeof(request), "auto %ls", server );
            start_job( JOB_AUTO_TIME, request, NULL );
        }
        return TRUE;

    case IDC_SYNC:
        if (code != BN_CLICKED) return FALSE;
        sync_now( view, 0 );
        return TRUE;

    case IDC_DATE:
    case IDC_TIME:
        if (code == DTN_DATETIMECHANGE) manual_touched = TRUE;
        return FALSE;

    case IDC_SET:
    {
        SYSTEMTIME date, time, local, utc;
        FILETIME ft;
        LONGLONG seconds;

        if (code != BN_CLICKED) return FALSE;
        if (SendMessageW( GetDlgItem( view_window( view ), IDC_DATE ), DTM_GETSYSTEMTIME, 0, (LPARAM)&date ) != GDT_VALID ||
            SendMessageW( GetDlgItem( view_window( view ), IDC_TIME ), DTM_GETSYSTEMTIME, 0, (LPARAM)&time ) != GDT_VALID)
            return TRUE;
        local = date;
        local.wHour = time.wHour;
        local.wMinute = time.wMinute;
        local.wSecond = time.wSecond;
        local.wMilliseconds = 0;
        if (!TzSpecificLocalTimeToSystemTime( NULL, &local, &utc ) || !SystemTimeToFileTime( &utc, &ft )) return TRUE;
        seconds = ((((LONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - 116444736000000000LL) / 10000000;
        snprintf( request, sizeof(request), "set %I64d", seconds );
        start_job( JOB_SET, request, NULL );
        return TRUE;
    }

    case IDC_AUTO_ZONE:
        if (code != BN_CLICKED) return FALSE;
        host.auto_zone = SendMessageW( control, BM_GETCHECK, 0, 0 ) == BST_CHECKED;
        enable( view, IDC_ZONE, !host.auto_zone );
        if (host.auto_zone) start_job( JOB_AUTO_ZONE, "zone-auto on", "zone-detect" );
        else start_job( JOB_AUTO_ZONE, "zone-auto off", NULL );
        return TRUE;

    case IDC_ZONE:
    {
        int sel;
        char name[96];

        if (code != CBN_SELENDOK) return FALSE;
        sel = SendMessageW( control, CB_GETCURSEL, 0, 0 );
        if (sel < 0 || sel >= (int)zone_count || !lstrcmpiW( zones[sel].key, current_zone )) return TRUE;
        if (!unix_zone( zones[sel].key, name, sizeof(name) ))
        {
            MessageBoxW( GetAncestor( view_window( view ), GA_ROOT ), load_string( IDS_ZONE_FAILED ),
                         load_string( IDS_NAME ), MB_ICONWARNING );
            return TRUE;
        }
        snprintf( request, sizeof(request), "zone %s", name );
        if (start_job( JOB_ZONE, request, NULL ))
        {
            lstrcpynW( current_zone, zones[sel].key, ARRAY_SIZE(current_zone) );
            lstrcpynA( host.zone, name, sizeof(host.zone) );
            write_zone_info( &zones[sel] );
        }
        return TRUE;
    }
    }
    return FALSE;
}

void datetime_timer( struct view *view )
{
    set_pickers( view );
    if (job.finished)
    {
        enum job_kind kind = job.kind;
        BOOL ok = job.ok;

        job.finished = 0;
        job.busy = 0;
        switch (kind)
        {
        case JOB_SYNC:
        case JOB_AUTO_TIME:
        {
            WCHAR server[128];

            lstrcpynW( server, host.server, ARRAY_SIZE(server) );
            load_host_state();
            if (ok) sync_status();
            else lstrcpynW( status, format_string( IDS_SYNC_FAILED, server ), ARRAY_SIZE(status) );
            if (kind == JOB_AUTO_TIME) view_rebuild( view );
            break;
        }
        case JOB_SET:
            lstrcpynW( status, load_string( ok ? IDS_TIME_SET : IDS_TIME_FAILED ), ARRAY_SIZE(status) );
            manual_touched = FALSE;
            break;
        case JOB_ZONE:
        case JOB_AUTO_ZONE:
        {
            struct zone *zone;

            if (!ok && kind == JOB_ZONE)
                MessageBoxW( GetAncestor( view_window( view ), GA_ROOT ), load_string( IDS_ZONE_FAILED ),
                             load_string( IDS_NAME ), MB_ICONWARNING );
            load_host_state();
            find_current_zone();
            if ((zone = find_zone( current_zone ))) write_zone_info( zone );
            sync_status();
            view_rebuild( view );
            break;
        }
        default:
            break;
        }
    }
    InvalidateRect( view_window( view ), NULL, FALSE );
}
