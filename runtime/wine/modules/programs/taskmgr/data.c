/*
 * Task Manager: what the machine is doing
 *
 * Everything comes through Windows' own interfaces, as Windows' Task
 * Manager reads it: the processes and the processors from
 * NtQuerySystemInformation, memory from GlobalMemoryStatusEx and
 * GetPerformanceInfo, each disk from its drives' IOCTL_DISK_PERFORMANCE
 * (Wine 0095; SystemPerformanceInformation, which Wine 0094 fills, when a
 * disk does not answer), the network from GetIfTable2, memory modules
 * from the SMBIOS table, the graphics cards from powrprof (gpu.c).
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "taskmgr.h"
#include "winternl.h"
#include "psapi.h"
#include "shellapi.h"
#include "winver.h"
#include "winsock2.h"
#include "ws2tcpip.h"
#include "iphlpapi.h"
#include "netioapi.h"
#include "powrprof.h"
#include "sysinfoapi.h"
#include "winioctl.h"
#include "ntddstor.h"

struct proc *procs;
UINT proc_count;
struct sample history[HISTORY];
UINT history_count;
struct system_info sysinfo;
HIMAGELIST small_icons;
UINT cpu_count = 1;

#define RSMB 0x52534d42   /* 'RSMB', the raw SMBIOS table */

static UINT history_pos;          /* where the next sample goes */
static ULONGLONG last_tick;       /* GetTickCount64 of the last sample */
static SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION last_cpu[MAX_CPUS];
static ULONGLONG last_disk_read, last_disk_write, last_disk_busy;   /* all disks, the fallback */
static ULONGLONG last_net_in, last_net_out;
static BOOL have_last;

/* what is known of each process once, by pid and start time */
struct known
{
    DWORD     pid;
    ULONGLONG created;
    ULONGLONG cpu_time, io_bytes;
    WCHAR     path[MAX_PATH], title[256], user[64];
    int       icon;
    BOOL      alive;
};
static struct known *known;
static UINT known_count, known_alloc;

/* image paths and their icon in small_icons */
struct icon_cache { WCHAR path[MAX_PATH]; int icon; };
static struct icon_cache *icons;
static UINT icon_count;

/* the processes of the system itself, which Windows lists as its own */
static const WCHAR *windows_processes[] =
{
    L"system", L"wininit.exe", L"csrss.exe", L"dwm.exe", L"winlogon.exe", L"services.exe",
    L"svchost.exe", L"lsass.exe", L"winedevice.exe", L"plugplay.exe", L"rpcss.exe", L"smss.exe",
    L"explorer.exe", L"audiodg.exe", L"userinit.exe", L"fontdrvhost.exe", L"sihost.exe",
    L"spoolsv.exe", L"taskhostw.exe", L"conhost.exe", L"wineboot.exe", L"start.exe",
};

static BOOL is_windows_process( const WCHAR *exe )
{
    for (UINT i = 0; i < ARRAY_SIZE(windows_processes); i++)
        if (!wcsicmp( exe, windows_processes[i] )) return TRUE;
    return FALSE;
}

/* ------------------------------------------------------------------ helpers */

static ULONGLONG ft64( const LARGE_INTEGER *li ) { return li->QuadPart; }

static int default_icon(void)
{
    static int index = -2;
    if (index == -2)
    {
        HICON icon = NULL;
        ExtractIconExW( L"shell32.dll", 2, NULL, &icon, 1 );
        if (!icon) icon = LoadIconW( NULL, (LPCWSTR)IDI_APPLICATION );
        index = icon ? ImageList_AddIcon( small_icons, icon ) : -1;
    }
    return index;
}

static int icon_for( const WCHAR *path )
{
    HICON icon = NULL;

    if (!path[0]) return -1;
    for (UINT i = 0; i < icon_count; i++)
        if (!wcsicmp( icons[i].path, path )) return icons[i].icon;
    if (!(icons = realloc( icons, (icon_count + 1) * sizeof(*icons) ))) return -1;
    wcscpy( icons[icon_count].path, path );
    icons[icon_count].icon = -1;
    if (ExtractIconExW( path, 0, NULL, &icon, 1 ) && icon)
    {
        icons[icon_count].icon = ImageList_AddIcon( small_icons, icon );
        DestroyIcon( icon );
    }
    return icons[icon_count++].icon;
}

/* the program's own name for itself, as Windows lists processes: its
 * version resource's FileDescription, else the image name */
static void describe( const WCHAR *path, const WCHAR *exe, WCHAR *title, size_t len )
{
    DWORD handle, size;
    void *data;
    struct { WORD lang, cp; } *tr;
    UINT trlen;
    WCHAR key[64], *value;
    UINT vlen;

    lstrcpynW( title, exe, len );
    if (!path[0] || !(size = GetFileVersionInfoSizeW( path, &handle ))) return;
    if (!(data = malloc( size ))) return;
    if (GetFileVersionInfoW( path, 0, size, data ) &&
        VerQueryValueW( data, L"\\VarFileInfo\\Translation", (void **)&tr, &trlen ) && trlen >= sizeof(*tr))
    {
        swprintf( key, ARRAY_SIZE(key), L"\\StringFileInfo\\%04x%04x\\FileDescription", tr->lang, tr->cp );
        if (VerQueryValueW( data, key, (void **)&value, &vlen ) && vlen > 1 && value[0])
            lstrcpynW( title, value, len );
    }
    free( data );
    /* Windows' own name for the shell */
    if (!wcsicmp( exe, L"explorer.exe" )) lstrcpynW( title, load_string( IDS_EXPLORER ), len );
}

static void user_of( HANDLE process, WCHAR *user, size_t len )
{
    HANDLE token;
    char buf[256];
    TOKEN_USER *tu = (TOKEN_USER *)buf;
    DWORD size, name_len = len, domain_len = 64;
    WCHAR domain[64];
    SID_NAME_USE use;

    user[0] = 0;
    if (!OpenProcessToken( process, TOKEN_QUERY, &token )) return;
    if (GetTokenInformation( token, TokenUser, buf, sizeof(buf), &size ))
        LookupAccountSidW( NULL, tu->User.Sid, user, &name_len, domain, &domain_len, &use );
    CloseHandle( token );
}

static struct known *find_known( DWORD pid, ULONGLONG created )
{
    for (UINT i = 0; i < known_count; i++)
        if (known[i].pid == pid && known[i].created == created) return &known[i];
    return NULL;
}

static struct known *add_known( DWORD pid, ULONGLONG created, const WCHAR *exe )
{
    struct known *k;
    HANDLE process;
    DWORD len;

    if (known_count == known_alloc)
    {
        UINT n = known_alloc ? known_alloc * 2 : 128;
        struct known *p = realloc( known, n * sizeof(*known) );
        if (!p) return NULL;
        known = p;
        known_alloc = n;
    }
    k = &known[known_count++];
    memset( k, 0, sizeof(*k) );
    k->pid = pid;
    k->created = created;
    k->icon = -1;
    if ((process = OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid )))
    {
        len = ARRAY_SIZE(k->path);
        if (!QueryFullProcessImageNameW( process, 0, k->path, &len )) k->path[0] = 0;
        user_of( process, k->user, ARRAY_SIZE(k->user) );
        CloseHandle( process );
    }
    describe( k->path, exe, k->title, ARRAY_SIZE(k->title) );
    /* Task Manager itself, by its name in the language of the system */
    if (pid == GetCurrentProcessId()) lstrcpynW( k->title, load_string( IDS_APP_TITLE ), ARRAY_SIZE(k->title) );
    k->icon = icon_for( k->path );
    /* a program without an icon of its own has Windows' default one */
    if (k->icon < 0) k->icon = default_icon();
    return k;
}

/* the window Windows would show the program by on the taskbar */
struct find_window { DWORD pid; HWND found; };

static BOOL CALLBACK find_main_window( HWND hwnd, LPARAM lparam )
{
    struct find_window *fw = (struct find_window *)lparam;
    DWORD pid;
    WCHAR cls[64];

    if (!IsWindowVisible( hwnd ) || GetWindow( hwnd, GW_OWNER )) return TRUE;
    if (GetWindowLongW( hwnd, GWL_EXSTYLE ) & WS_EX_TOOLWINDOW) return TRUE;
    if (!GetWindowTextLengthW( hwnd )) return TRUE;
    GetWindowThreadProcessId( hwnd, &pid );
    if (pid != fw->pid) return TRUE;
    GetClassNameW( hwnd, cls, ARRAY_SIZE(cls) );
    /* the shell's desktop and taskbar are not windows of a program */
    if (!wcscmp( cls, L"Progman" ) || !wcscmp( cls, L"Shell_TrayWnd" ) || !wcscmp( cls, L"WorkerW" ) ||
        !wcscmp( cls, L"Shell_SecondaryTrayWnd" )) return TRUE;
    fw->found = hwnd;
    return FALSE;
}

/* ------------------------------------------------------------------ init */

static void read_cpu_info(void)
{
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION *info = NULL;
    DWORD len = 0, count, mhz = 0, size;
    HKEY key;
    SYSTEM_INFO si;

    GetSystemInfo( &si );
    cpu_count = min( si.dwNumberOfProcessors, MAX_CPUS );
    sysinfo.logical = si.dwNumberOfProcessors;

    if (!RegOpenKeyExW( HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &key ))
    {
        size = sizeof(sysinfo.cpu_name);
        if (RegQueryValueExW( key, L"ProcessorNameString", NULL, NULL, (BYTE *)sysinfo.cpu_name, &size ))
            sysinfo.cpu_name[0] = 0;
        size = sizeof(mhz);
        RegQueryValueExW( key, L"~MHz", NULL, NULL, (BYTE *)&mhz, &size );
        RegCloseKey( key );
    }
    /* "Intel(R) Core(TM) i5-7300HQ CPU @ 2.50GHz": the trailing spaces go */
    for (WCHAR *p = sysinfo.cpu_name + wcslen( sysinfo.cpu_name ); p > sysinfo.cpu_name && p[-1] == ' '; ) *--p = 0;
    sysinfo.cpu_mhz_base = mhz;

    GetLogicalProcessorInformation( NULL, &len );
    if (len && (info = malloc( len )) && GetLogicalProcessorInformation( info, &len ))
    {
        count = len / sizeof(*info);
        for (DWORD i = 0; i < count; i++)
        {
            switch (info[i].Relationship)
            {
            case RelationProcessorCore: sysinfo.cores++; break;
            case RelationProcessorPackage: sysinfo.sockets++; break;
            case RelationCache:
                if (info[i].Cache.Level == 1 && info[i].Cache.Type != CacheInstruction) sysinfo.l1 += info[i].Cache.Size;
                if (info[i].Cache.Level == 1 && info[i].Cache.Type == CacheInstruction) sysinfo.l1 += info[i].Cache.Size;
                if (info[i].Cache.Level == 2) sysinfo.l2 += info[i].Cache.Size;
                if (info[i].Cache.Level == 3) sysinfo.l3 += info[i].Cache.Size;
                break;
            default: break;
            }
        }
    }
    free( info );
    if (!sysinfo.cores) sysinfo.cores = sysinfo.logical;
    if (!sysinfo.sockets) sysinfo.sockets = 1;
    /* the Windows Hypervisor Platform is there (docs/hypervisor.md) */
    sysinfo.virtualization = IsProcessorFeaturePresent( PF_VIRT_FIRMWARE_ENABLED ) ||
                             GetFileAttributesW( L"C:\\Windows\\System32\\WinHvPlatform.dll" ) != INVALID_FILE_ATTRIBUTES;
}

/* the memory modules: SMBIOS structures 16 (the array) and 17 (each device) */
static void read_memory_modules(void)
{
    DWORD size = GetSystemFirmwareTable( RSMB, 0, NULL, 0 );
    BYTE *buf, *p, *end;
    ULONGLONG installed = 0;
    BYTE form = 0;

    if (!size || !(buf = malloc( size ))) return;
    if (GetSystemFirmwareTable( RSMB, 0, buf, size ) != size) { free( buf ); return; }
    /* RawSMBIOSData: 8 bytes of header, then the table */
    p = buf + 8;
    end = buf + min( size, 8 + *(DWORD *)(buf + 4) );
    while (p + 4 <= end)
    {
        BYTE type = p[0], len = p[1];
        BYTE *next = p + len;

        if (len < 4 || next > end) break;
        if (type == 16 && len >= 0x0f)
            sysinfo.mem_slots += *(WORD *)(p + 0x0d);
        else if (type == 17 && len >= 0x15)
        {
            WORD mb = *(WORD *)(p + 0x0c);
            if (mb && mb != 0xffff)
            {
                ULONGLONG bytes = (mb & 0x8000) ? (ULONGLONG)(mb & 0x7fff) * 1024 : (ULONGLONG)mb * 1024 * 1024;
                if (mb == 0x7fff && len >= 0x20) bytes = (ULONGLONG)*(DWORD *)(p + 0x1c) * 1024 * 1024;
                installed += bytes;
                sysinfo.mem_slots_used++;
                form = p[0x0e];
                if (len >= 0x17 && *(WORD *)(p + 0x15)) sysinfo.mem_speed = *(WORD *)(p + 0x15);
                if (len >= 0x22 && *(WORD *)(p + 0x20)) sysinfo.mem_speed = *(WORD *)(p + 0x20);
            }
        }
        if (type == 127) break;
        /* the strings after the structure end with two zeros */
        while (next + 1 < end && (next[0] || next[1])) next++;
        p = next + 2;
    }
    free( buf );
    if (form == 0x09) wcscpy( sysinfo.mem_form, L"DIMM" );
    else if (form == 0x0d) wcscpy( sysinfo.mem_form, L"SODIMM" );
    else if (form) lstrcpynW( sysinfo.mem_form, load_string( IDS_FORM_OTHER ), ARRAY_SIZE(sysinfo.mem_form) );
    if (installed > sysinfo.mem_total) sysinfo.mem_hw_reserved = installed - sysinfo.mem_total;
    if (sysinfo.mem_slots < sysinfo.mem_slots_used) sysinfo.mem_slots = sysinfo.mem_slots_used;
}

/* ------------------------------------------------------------------ disks */

static HANDLE open_drive( const WCHAR *device )
{
    return CreateFileW( device, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL );
}

static struct disk_info *disk_by_number( DWORD number )
{
    for (UINT i = 0; i < sysinfo.disk_count; i++)
        if (sysinfo.disks[i].number == number) return &sysinfo.disks[i];
    return NULL;
}

/* what the disk is: its model, its size, a hard disk or an SSD */
static void describe_disk( struct disk_info *d, HANDLE h )
{
    STORAGE_PROPERTY_QUERY query = { StorageDeviceProperty, PropertyStandardQuery };
    BYTE buf[1024];
    DISK_GEOMETRY_EX geometry;
    DEVICE_SEEK_PENALTY_DESCRIPTOR seek;
    DWORD len;

    memset( buf, 0, sizeof(buf) );
    if (DeviceIoControl( h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buf, sizeof(buf), &len, NULL ) &&
        len >= sizeof(STORAGE_DEVICE_DESCRIPTOR))
    {
        STORAGE_DEVICE_DESCRIPTOR *desc = (STORAGE_DEVICE_DESCRIPTOR *)buf;

        d->removable = desc->RemovableMedia || desc->BusType == BusTypeUsb;
        if (desc->ProductIdOffset && desc->ProductIdOffset < len)
        {
            buf[sizeof(buf) - 1] = 0;
            MultiByteToWideChar( CP_UTF8, 0, (char *)buf + desc->ProductIdOffset, -1, d->model, ARRAY_SIZE(d->model) );
            /* "Samsung SSD 860   " */
            for (int n = wcslen( d->model ); n && d->model[n - 1] == ' '; n--) d->model[n - 1] = 0;
        }
    }
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    memset( &seek, 0, sizeof(seek) );
    d->ssd = !DeviceIoControl( h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &seek, sizeof(seek), &len, NULL ) ||
             !seek.IncursSeekPenalty;
    if (DeviceIoControl( h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0, &geometry, sizeof(geometry), &len, NULL ))
        d->capacity = geometry.DiskSize.QuadPart;
}

/* the disks behind the drive letters, each once with all its letters, as
 * Windows' Task Manager lists them; a disk keeps its slot (and its graph)
 * while it is there */
static void read_disks(void)
{
    WCHAR system_drive = L'C';
    WCHAR windir[MAX_PATH];
    DWORD drives = GetLogicalDrives();
    BOOL before[MAX_DISKS] = { 0 };

    if (GetWindowsDirectoryW( windir, ARRAY_SIZE(windir) )) system_drive = towupper( windir[0] );
    for (UINT i = 0; i < sysinfo.disk_count; i++)
    {
        before[i] = sysinfo.disks[i].present;
        sysinfo.disks[i].present = FALSE;
        sysinfo.disks[i].letters[0] = 0;
        sysinfo.disks[i].formatted = 0;
        sysinfo.disks[i].system = FALSE;
    }

    for (int i = 2; i < 26; i++)
    {
        WCHAR root[] = L"A:\\", device[] = L"\\\\.\\A:";
        STORAGE_DEVICE_NUMBER devnum;
        ULARGE_INTEGER avail, total, freeb;
        struct disk_info *d;
        UINT type;
        DWORD len, number;
        HANDLE h;

        if (!(drives & (1u << i))) continue;
        root[0] = device[4] = 'A' + i;
        type = GetDriveTypeW( root );
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
        if ((h = open_drive( device )) == INVALID_HANDLE_VALUE) continue;
        if (DeviceIoControl( h, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, &devnum, sizeof(devnum), &len, NULL ))
            number = devnum.DeviceNumber;
        else number = 1000 + i;   /* a drive of its own */

        if (!(d = disk_by_number( number )))
        {
            UINT slot;

            /* a slot never used, else one whose disk went away before */
            if (sysinfo.disk_count < MAX_DISKS) slot = sysinfo.disk_count++;
            else
            {
                for (slot = 0; slot < MAX_DISKS; slot++) if (!before[slot] && !sysinfo.disks[slot].present) break;
                if (slot == MAX_DISKS) { CloseHandle( h ); continue; }
            }
            d = &sysinfo.disks[slot];
            memset( d, 0, sizeof(*d) );
            d->number = number;
            describe_disk( d, h );
        }
        if (!d->present)
        {
            DISK_PERFORMANCE perf;

            d->present = TRUE;
            lstrcpyW( d->device, device );
            d->counters = DeviceIoControl( h, IOCTL_DISK_PERFORMANCE, NULL, 0, &perf, sizeof(perf), &len, NULL );
        }
        CloseHandle( h );

        if (d->letters[0]) lstrcatW( d->letters, L" " );
        root[2] = 0;
        if (wcslen( d->letters ) + 3 < ARRAY_SIZE(d->letters)) lstrcatW( d->letters, root );
        root[2] = '\\';
        if (GetDiskFreeSpaceExW( root, &avail, &total, &freeb )) d->formatted += total.QuadPart;
        if (towupper( root[0] ) == system_drive) d->system = TRUE;
        if (d->capacity < d->formatted) d->capacity = d->formatted;
    }
}

/* the slots in the order of the tiles: by the disks' numbers */
UINT data_disk_order( UINT *slots )
{
    UINT n = 0;

    for (UINT i = 0; i < sysinfo.disk_count; i++) if (sysinfo.disks[i].present) slots[n++] = i;
    for (UINT i = 1; i < n; i++)
        for (UINT j = i; j > 0 && sysinfo.disks[slots[j]].number < sysinfo.disks[slots[j - 1]].number; j--)
        {
            UINT t = slots[j]; slots[j] = slots[j - 1]; slots[j - 1] = t;
        }
    return n;
}

void data_init(void)
{
    MEMORYSTATUSEX ms = { sizeof(ms) };
    int cx = GetSystemMetrics( SM_CXSMICON );

    small_icons = ImageList_Create( cx, cx, ILC_COLOR32 | ILC_MASK, 64, 64 );
    GlobalMemoryStatusEx( &ms );
    sysinfo.mem_total = ms.ullTotalPhys;
    read_cpu_info();
    read_memory_modules();
    read_disks();
    gpu_init();
}

/* ------------------------------------------------------------------ sampling */

static void sample_processes( double elapsed_100ns )
{
    ULONG size = 512 * 1024, len;
    BYTE *buf = NULL;
    NTSTATUS status;
    SYSTEM_PROCESS_INFORMATION *spi;
    UINT count = 0, threads = 0, handles = 0;

    for (;;)
    {
        free( buf );
        if (!(buf = malloc( size ))) return;
        status = NtQuerySystemInformation( SystemProcessInformation, buf, size, &len );
        if (status != STATUS_INFO_LENGTH_MISMATCH) break;
        size *= 2;
    }
    if (status) { free( buf ); return; }

    for (spi = (SYSTEM_PROCESS_INFORMATION *)buf;; spi = (SYSTEM_PROCESS_INFORMATION *)((BYTE *)spi + spi->NextEntryOffset))
    {
        count++;
        if (!spi->NextEntryOffset) break;
    }
    free( procs );
    if (!(procs = calloc( count, sizeof(*procs) ))) { free( buf ); proc_count = 0; return; }
    for (UINT i = 0; i < known_count; i++) known[i].alive = FALSE;

    proc_count = 0;
    for (spi = (SYSTEM_PROCESS_INFORMATION *)buf;; spi = (SYSTEM_PROCESS_INFORMATION *)((BYTE *)spi + spi->NextEntryOffset))
    {
        struct proc *p = &procs[proc_count];
        DWORD pid = HandleToUlong( spi->UniqueProcessId );
        ULONGLONG created = ft64( &spi->CreationTime );
        ULONGLONG cpu = ft64( &spi->KernelTime ) + ft64( &spi->UserTime );
        ULONGLONG io = spi->ioCounters.ReadTransferCount + spi->ioCounters.WriteTransferCount;
        struct known *k;

        if (pid)
        {
            p->pid = pid;
            p->ppid = HandleToUlong( spi->ParentProcessId );
            if (spi->ProcessName.Buffer)
                lstrcpynW( p->exe, spi->ProcessName.Buffer, min( ARRAY_SIZE(p->exe), spi->ProcessName.Length / sizeof(WCHAR) + 1 ) );
            p->threads = spi->dwThreadCount;
            p->handles = spi->HandleCount;
            p->session = spi->SessionId;
            p->cpu_time = cpu;
            p->priority = spi->dwBasePriority;
            {
                /* the base priority of its class, as Windows lists it */
                HANDLE process = OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid );
                if (process)
                {
                    switch (GetPriorityClass( process ))
                    {
                    case IDLE_PRIORITY_CLASS: p->priority = 4; break;
                    case BELOW_NORMAL_PRIORITY_CLASS: p->priority = 6; break;
                    case ABOVE_NORMAL_PRIORITY_CLASS: p->priority = 10; break;
                    case HIGH_PRIORITY_CLASS: p->priority = 13; break;
                    case REALTIME_PRIORITY_CLASS: p->priority = 24; break;
                    default: p->priority = 8; break;
                    }
                    CloseHandle( process );
                }
            }
            p->working_set = spi->vmCounters.WorkingSetSize;
            p->private_ws = spi->vmCounters.PagefileUsage ? spi->vmCounters.PagefileUsage : spi->vmCounters.PrivateUsage;
            p->commit = spi->vmCounters.PrivateUsage ? spi->vmCounters.PrivateUsage : p->private_ws;
            p->io_bytes = io;
            /* every thread waiting for a resume: suspended, as Windows says */
            p->suspended = FALSE;

            if (!(k = find_known( pid, created ))) k = add_known( pid, created, p->exe );
            if (k)
            {
                if (have_last && k->cpu_time && elapsed_100ns > 0)
                {
                    p->cpu = (double)(cpu - min( cpu, k->cpu_time )) * 100.0 / (elapsed_100ns * cpu_count);
                    p->disk = (double)(io - min( io, k->io_bytes )) * 1e7 / elapsed_100ns;
                }
                k->cpu_time = cpu ? cpu : 1;
                k->io_bytes = io;
                k->alive = TRUE;
                wcscpy( p->path, k->path );
                wcscpy( p->title, k->title );
                wcscpy( p->user, k->user );
                p->icon = k->icon;
            }
            else
            {
                wcscpy( p->title, p->exe );
                p->icon = -1;
            }
            if (p->cpu > 100) p->cpu = 100;
            threads += p->threads;
            handles += p->handles;
            proc_count++;
        }
        if (!spi->NextEntryOffset) break;
    }
    free( buf );

    /* forget the processes that ended */
    for (UINT i = 0; i < known_count; )
    {
        if (!known[i].alive) known[i] = known[--known_count];
        else i++;
    }

    /* programs: a window on the taskbar; the system's own; everything else in the background */
    for (UINT i = 0; i < proc_count; i++)
    {
        struct find_window fw = { procs[i].pid, NULL };

        EnumWindows( find_main_window, (LPARAM)&fw );
        procs[i].window = fw.found;
        if (fw.found) GetWindowTextW( fw.found, procs[i].window_title, ARRAY_SIZE(procs[i].window_title) );
        if (fw.found) procs[i].kind = KIND_APP;
        else if (is_windows_process( procs[i].exe )) procs[i].kind = KIND_WINDOWS;
        else procs[i].kind = KIND_BACKGROUND;
    }

    sysinfo.processes = proc_count;
    sysinfo.threads = threads;
    sysinfo.handles = handles;
}

static double sample_cpu( struct sample *s, double elapsed_100ns )
{
    SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION now[MAX_CPUS];
    ULONG len;
    double busy_all = 0, total_all = 0;

    memset( now, 0, sizeof(now) );
    if (NtQuerySystemInformation( SystemProcessorPerformanceInformation, now, sizeof(now[0]) * cpu_count, &len ))
        return 0;
    for (UINT i = 0; i < cpu_count; i++)
    {
        double idle = (double)(now[i].IdleTime.QuadPart - last_cpu[i].IdleTime.QuadPart);
        double total = (double)(now[i].KernelTime.QuadPart - last_cpu[i].KernelTime.QuadPart) +
                       (double)(now[i].UserTime.QuadPart - last_cpu[i].UserTime.QuadPart);
        double pct = 0;

        if (have_last && total > 0)
        {
            pct = (total - idle) * 100.0 / total;
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            busy_all += total - idle;
            total_all += total;
        }
        s->cpu_each[i] = pct;
        last_cpu[i] = now[i];
    }
    return total_all > 0 ? busy_all * 100.0 / total_all : 0;
}

static void sample_memory( struct sample *s )
{
    MEMORYSTATUSEX ms = { sizeof(ms) };
    PERFORMANCE_INFORMATION pi = { sizeof(pi) };

    GlobalMemoryStatusEx( &ms );
    sysinfo.mem_total = ms.ullTotalPhys;
    sysinfo.mem_avail = ms.ullAvailPhys;
    sysinfo.mem_commit = ms.ullTotalPageFile - ms.ullAvailPageFile;
    sysinfo.mem_commit_limit = ms.ullTotalPageFile;
    if (GetPerformanceInfo( &pi, sizeof(pi) ))
    {
        sysinfo.mem_cached = (ULONGLONG)pi.SystemCache * pi.PageSize;
        sysinfo.mem_paged = (ULONGLONG)pi.KernelPaged * pi.PageSize;
        sysinfo.mem_nonpaged = (ULONGLONG)pi.KernelNonpaged * pi.PageSize;
    }
    s->mem_used = (double)(ms.ullTotalPhys - ms.ullAvailPhys);
}

/* a disk's counters since the last sample, as Windows' "% Disk Time" and
 * "Avg. Disk sec/Transfer" are made from DISK_PERFORMANCE */
static void sample_one_disk( struct disk_info *d, struct sample *s, UINT slot )
{
    DISK_PERFORMANCE perf;
    ULONGLONG rd, wr, idle, query, rw_time, requests;
    DWORD len;
    HANDLE h;

    if ((h = open_drive( d->device )) == INVALID_HANDLE_VALUE) return;
    d->counters = DeviceIoControl( h, IOCTL_DISK_PERFORMANCE, NULL, 0, &perf, sizeof(perf), &len, NULL );
    CloseHandle( h );
    if (!d->counters) return;

    rd = perf.BytesRead.QuadPart;
    wr = perf.BytesWritten.QuadPart;
    idle = perf.IdleTime.QuadPart;
    query = perf.QueryTime.QuadPart;
    rw_time = perf.ReadTime.QuadPart + perf.WriteTime.QuadPart;
    requests = (ULONGLONG)perf.ReadCount + perf.WriteCount;
    if (d->have_last && query > d->last_query)
    {
        double span = (double)(query - d->last_query);   /* 100 ns */
        double idle_part = (double)(idle - min( idle, d->last_idle )) / span;

        s->disks[slot].read = (double)(rd - min( rd, d->last_read )) * 1e7 / span;
        s->disks[slot].write = (double)(wr - min( wr, d->last_written )) * 1e7 / span;
        s->disks[slot].active = idle_part >= 1 ? 0 : (1 - idle_part) * 100;
        if (requests > d->last_requests)
            s->disks[slot].response = (double)(rw_time - min( rw_time, d->last_rw_time )) / 1e4 / (requests - d->last_requests);
    }
    d->last_read = rd;
    d->last_written = wr;
    d->last_idle = idle;
    d->last_query = query;
    d->last_rw_time = rw_time;
    d->last_requests = requests;
    d->have_last = TRUE;
}

static void sample_disk( struct sample *s, double elapsed_ms )
{
    SYSTEM_PERFORMANCE_INFORMATION spi;
    ULONG len;
    ULONGLONG rd, wr, busy;
    double all_read = 0, all_write = 0, all_active = 0;
    BOOL fallback = FALSE;

    for (UINT i = 0; i < sysinfo.disk_count; i++)
    {
        struct disk_info *d = &sysinfo.disks[i];

        if (!d->present) continue;
        sample_one_disk( d, s, i );
        if (!d->counters) { fallback = TRUE; continue; }
        all_read += s->disks[i].read;
        all_write += s->disks[i].write;
        all_active = max( all_active, s->disks[i].active );
    }

    /* the sum of the disks, which Wine 0094 gives, for a disk without its own */
    memset( &spi, 0, sizeof(spi) );
    if (NtQuerySystemInformation( SystemPerformanceInformation, &spi, sizeof(spi), &len )) return;
    rd = spi.ReadTransferCount.QuadPart;
    wr = spi.WriteTransferCount.QuadPart;
    busy = spi.OtherOperationCount;   /* Wine 0094: ms the busiest disk was busy */
    if (have_last && elapsed_ms > 0 && (fallback || !sysinfo.disk_count))
    {
        double read = (double)(rd - min( rd, last_disk_read )) * 1000.0 / elapsed_ms;
        double write = (double)(wr - min( wr, last_disk_write )) * 1000.0 / elapsed_ms;
        double active = min( 100.0, (double)(busy - min( busy, last_disk_busy )) * 100.0 / elapsed_ms );

        for (UINT i = 0; i < sysinfo.disk_count; i++)
        {
            if (!sysinfo.disks[i].present || sysinfo.disks[i].counters || !sysinfo.disks[i].system) continue;
            s->disks[i].read = read;
            s->disks[i].write = write;
            s->disks[i].active = active;
        }
        all_read = max( all_read, read );
        all_write = max( all_write, write );
        all_active = max( all_active, active );
    }
    last_disk_read = rd;
    last_disk_write = wr;
    last_disk_busy = busy;
    s->disk_read = all_read;
    s->disk_write = all_write;
    s->disk_active = all_active;
}

static void sample_network( struct sample *s, double elapsed_ms )
{
    MIB_IF_TABLE2 *table = NULL;
    ULONGLONG in = 0, out = 0, best = 0;

    if (GetIfTable2( &table )) return;
    sysinfo.have_net = FALSE;
    for (ULONG i = 0; i < table->NumEntries; i++)
    {
        MIB_IF_ROW2 *row = &table->Table[i];

        if (row->Type == IF_TYPE_SOFTWARE_LOOPBACK || row->OperStatus != IfOperStatusUp) continue;
        if (row->Type != IF_TYPE_ETHERNET_CSMACD && row->Type != IF_TYPE_IEEE80211) continue;
        in += row->InOctets;
        out += row->OutOctets;
        /* the adapter that carried the most is the one the page shows */
        if (!sysinfo.have_net || row->InOctets + row->OutOctets > best)
        {
            best = row->InOctets + row->OutOctets;
            sysinfo.have_net = TRUE;
            sysinfo.wifi = row->Type == IF_TYPE_IEEE80211;
            lstrcpynW( sysinfo.net_name, row->Description, ARRAY_SIZE(sysinfo.net_name) );
        }
    }
    FreeMibTable( table );
    if (have_last && elapsed_ms > 0)
    {
        s->net_recv = (double)(in - min( in, last_net_in )) * 8000.0 / elapsed_ms;
        s->net_send = (double)(out - min( out, last_net_out )) * 8000.0 / elapsed_ms;
    }
    last_net_in = in;
    last_net_out = out;
}

static void read_addresses(void)
{
    ULONG size = 16384;
    IP_ADAPTER_ADDRESSES *aa = malloc( size ), *a;

    sysinfo.net_ipv4[0] = sysinfo.net_ipv6[0] = 0;
    if (!aa) return;
    if (GetAdaptersAddresses( AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                              NULL, aa, &size ) == NO_ERROR)
    {
        for (a = aa; a; a = a->Next)
        {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next)
            {
                WCHAR text[64];
                DWORD tlen = ARRAY_SIZE(text);

                if (WSAAddressToStringW( u->Address.lpSockaddr, u->Address.iSockaddrLength, NULL, text, &tlen )) continue;
                if (u->Address.lpSockaddr->sa_family == AF_INET && !sysinfo.net_ipv4[0])
                    lstrcpynW( sysinfo.net_ipv4, text, ARRAY_SIZE(sysinfo.net_ipv4) );
                else if (u->Address.lpSockaddr->sa_family == AF_INET6 && !sysinfo.net_ipv6[0])
                    lstrcpynW( sysinfo.net_ipv6, text, ARRAY_SIZE(sysinfo.net_ipv6) );
            }
            if (sysinfo.net_ipv4[0]) break;
        }
    }
    free( aa );
}

static void read_cpu_speed(void)
{
    PROCESSOR_POWER_INFORMATION ppi[MAX_CPUS];
    ULONG best = 0;

    memset( ppi, 0, sizeof(ppi) );
    if (!CallNtPowerInformation( ProcessorInformation, NULL, 0, ppi, sizeof(ppi[0]) * cpu_count ))
    {
        for (UINT i = 0; i < cpu_count; i++) if (ppi[i].CurrentMhz > best) best = ppi[i].CurrentMhz;
        if (!sysinfo.cpu_mhz_base) sysinfo.cpu_mhz_base = ppi[0].MaxMhz;
    }
    sysinfo.cpu_mhz_now = best ? best : sysinfo.cpu_mhz_base;
}

void data_sample(void)
{
    ULONGLONG now = GetTickCount64();
    double elapsed_ms = last_tick ? (double)(now - last_tick) : 0;
    struct sample *s = &history[history_pos];
    static UINT addresses_every;

    memset( s, 0, sizeof(*s) );
    sample_processes( elapsed_ms * 10000.0 );
    s->cpu = sample_cpu( s, elapsed_ms * 10000.0 );
    sample_memory( s );
    /* a disk plugged in or taken out shows within a few seconds */
    if (addresses_every && !(addresses_every % 5)) read_disks();
    sample_disk( s, elapsed_ms );
    sample_network( s, elapsed_ms );
    gpu_sample( s );
    if (!(addresses_every++ % 10)) read_addresses();
    read_cpu_speed();
    sysinfo.uptime_ms = now;

    last_tick = now;
    have_last = TRUE;
    history_pos = (history_pos + 1) % HISTORY;
    if (history_count < HISTORY) history_count++;
}

const struct sample *data_back( UINT ago )
{
    if (ago >= history_count) return NULL;
    return &history[(history_pos + HISTORY - 1 - ago) % HISTORY];
}

const struct sample *data_latest(void)
{
    static const struct sample empty;
    const struct sample *s = data_back( 0 );
    return s ? s : &empty;
}

/* ------------------------------------------------------------------ actions */

static void end_children( DWORD pid )
{
    for (UINT i = 0; i < proc_count; i++)
    {
        if (procs[i].ppid != pid || procs[i].pid == pid) continue;
        end_children( procs[i].pid );
        data_end_process( procs[i].pid, FALSE );
    }
}

BOOL data_end_process( DWORD pid, BOOL tree )
{
    HANDLE process;
    BOOL ret;

    if (tree) end_children( pid );
    if (!(process = OpenProcess( PROCESS_TERMINATE, FALSE, pid ))) return FALSE;
    ret = TerminateProcess( process, 1 );
    CloseHandle( process );
    return ret;
}

/* End task on a program, as Windows does it: its window is asked to close,
 * and a program that does not answer is ended */
void data_end_window( HWND hwnd, DWORD pid )
{
    DWORD_PTR result;

    if (!SendMessageTimeoutW( hwnd, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG, 3000, &result ))
        data_end_process( pid, FALSE );
}

void data_set_priority( DWORD pid, DWORD priority_class )
{
    HANDLE process = OpenProcess( PROCESS_SET_INFORMATION, FALSE, pid );

    if (!process) return;
    SetPriorityClass( process, priority_class );
    CloseHandle( process );
}
