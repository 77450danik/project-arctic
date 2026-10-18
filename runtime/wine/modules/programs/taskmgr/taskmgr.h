/*
 * Task Manager (taskmgr.exe), Windows 10's
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __TASKMGR_H
#define __TASKMGR_H

#include <stdarg.h>
#include <stdlib.h>
#include <wchar.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "winreg.h"
#include "commctrl.h"
#include "resource.h"

/* windowsx.h is not for Wine's own sources */
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#ifndef GET_WHEEL_DELTA_WPARAM
#define GET_WHEEL_DELTA_WPARAM(wp) ((short)HIWORD(wp))
#endif

/* ------------------------------------------------------------------ data */

#define HISTORY        60          /* samples the graphs keep: a minute at the normal speed */
#define MAX_CPUS       256
#define MAX_DISKS      8
#define MAX_GPUS       4
#define NO_VALUE       (-1.0)      /* a figure the hardware does not give */

struct proc
{
    DWORD    pid, ppid;
    WCHAR    exe[MAX_PATH];        /* image name: chrome.exe */
    WCHAR    path[MAX_PATH];       /* full path, empty if it could not be read */
    WCHAR    title[256];           /* the description of the program, else the image name */
    WCHAR    user[64];
    DWORD    threads, handles, session;
    ULONGLONG cpu_time;            /* 100 ns, kernel + user */
    double   cpu;                  /* % of the whole machine over the last interval */
    SIZE_T   private_ws;           /* bytes: the Memory column (private working set) */
    SIZE_T   working_set, commit;
    ULONGLONG io_bytes;            /* read + written so far */
    double   disk;                 /* bytes a second */
    HWND     window;               /* its main window on the taskbar, if it has one */
    WCHAR    window_title[256];
    int      kind;                 /* KIND_APP, KIND_BACKGROUND, KIND_WINDOWS */
    BOOL     suspended, seen;
    int      icon;                 /* index in the small image list, -1 for the default */
    LONG     priority;
};

enum { KIND_APP, KIND_BACKGROUND, KIND_WINDOWS };

struct sample
{
    double cpu;                    /* % */
    double cpu_each[MAX_CPUS];
    double mem_used;               /* bytes */
    double disk_active;            /* % of the busiest disk */
    double disk_read, disk_write;  /* bytes a second, all disks */
    double net_send, net_recv;     /* bits a second */
    struct
    {
        double active;             /* % */
        double read, write;        /* bytes a second */
        double response;           /* ms a request took */
    } disks[MAX_DISKS];            /* by the disk's slot in sysinfo.disks */
    struct
    {
        double util;               /* % */
        double mem_used;           /* bytes, NO_VALUE */
        double temperature;        /* degrees Celsius, NO_VALUE */
    } gpus[MAX_GPUS];
};

/* a physical disk and the drive letters on it */
struct disk_info
{
    BOOL     present;
    DWORD    number;               /* its PhysicalDrive number: "Диск 0" */
    WCHAR    letters[64];          /* "C: D:" */
    WCHAR    device[8];            /* \\.\C: : one of its drives, to ask it */
    WCHAR    model[128];
    BOOL     ssd, removable, system;
    ULONGLONG capacity, formatted;
    BOOL     counters;             /* the disk answers IOCTL_DISK_PERFORMANCE */
    ULONGLONG last_read, last_written, last_idle, last_query, last_rw_time, last_requests;
    BOOL     have_last;
};

struct gpu_info
{
    WCHAR    name[128];
    WCHAR    driver[16];
    WCHAR    pci[16];              /* 0000:01:00.0 */
    BOOL     integrated;
    double   mem_total;            /* bytes, NO_VALUE */
};

struct system_info
{
    WCHAR    cpu_name[128];
    DWORD    cpu_mhz_base, cpu_mhz_now;
    DWORD    sockets, cores, logical;
    DWORD    l1, l2, l3;           /* bytes */
    BOOL     virtualization;
    ULONGLONG mem_total, mem_avail, mem_commit, mem_commit_limit;
    ULONGLONG mem_cached, mem_paged, mem_nonpaged, mem_hw_reserved;
    DWORD    mem_speed, mem_slots, mem_slots_used;
    WCHAR    mem_form[32];
    DWORD    processes, threads, handles;
    ULONGLONG uptime_ms;
    struct disk_info disks[MAX_DISKS];
    UINT     disk_count;           /* slots used, present or not */
    struct gpu_info gpus[MAX_GPUS];
    UINT     gpu_count;
    WCHAR    net_name[128], net_kind[32], net_ipv4[64], net_ipv6[64];
    BOOL     have_net, wifi;
};

extern struct proc *procs;
extern UINT proc_count;
extern struct sample history[HISTORY];
extern UINT history_count;         /* samples taken, up to HISTORY */
extern struct system_info sysinfo;
extern HIMAGELIST small_icons;
extern UINT cpu_count;

void data_init(void);
void data_sample(void);            /* one interval: processes, CPU, memory, disk, network */
const struct sample *data_latest(void);
const struct sample *data_back( UINT ago );   /* ago samples before the latest, NULL if none */
BOOL data_end_process( DWORD pid, BOOL tree );
void data_set_priority( DWORD pid, DWORD priority_class );
void data_end_window( HWND hwnd, DWORD pid );
UINT data_disk_order( UINT *slots );   /* the present disks' slots by number */
/* the graphics cards, asked on a thread of their own: NVML takes its time */
void gpu_init(void);
void gpu_sample( struct sample *s );

/* ------------------------------------------------------------------ the grid */

#define GRID_MAX_COLS  16

struct grid_col
{
    UINT     title;                /* string id */
    int      width;                /* at 96 DPI */
    BOOL     right;                /* numbers: right aligned */
    BOOL     heat;                 /* the cell's background follows its value */
    BOOL     hidden;
    WCHAR    total[32];            /* over the title: "12%" */
};

struct grid_row
{
    WCHAR   *cells[GRID_MAX_COLS];
    double   heat[GRID_MAX_COLS];  /* 0..1 */
    double   sort[GRID_MAX_COLS];  /* for numeric columns */
    int      icon;                 /* in small_icons, -1 none */
    int      indent;               /* 0, 1 for a child */
    BOOL     group;                /* a group's title: "Програми (3)" */
    BOOL     expandable, expanded;
    LPARAM   param;                /* the pid, the service row... */
    int      group_id;             /* rows sort within their group */
};

struct grid
{
    HWND     hwnd, header;
    struct grid_col cols[GRID_MAX_COLS];
    UINT     col_count;
    struct grid_row *rows;
    UINT     row_count, row_alloc;
    int      selected;             /* row index, -1 none */
    LPARAM   selected_param;       /* kept across a refresh */
    int      scroll, hscroll;      /* first visible row, pixels */
    int      sort_col;
    BOOL     sort_down;
    BOOL     big_header;           /* two lines: the total over the title */
    BOOL     groups;               /* rows are grouped: no sorting across groups */
    BOOL     icons;                /* the first column has the programs' icons */
    BOOL     tree;                 /* rows open into children: room for the chevron */
    BOOL     no_header;            /* a plain list: the compact view */
    UINT     notify_id;            /* WM_COMMAND id to the parent */
};

#define GRID_CLASS     L"ArcticTaskGrid"
#define GN_SELCHANGE   1
#define GN_DBLCLK      2
#define GN_EXPAND      3
#define GN_CONTEXT     4
#define GN_KEYDELETE   5

void grid_register( HINSTANCE inst );
struct grid *grid_create( HWND parent, UINT id, BOOL big_header );
void grid_add_col( struct grid *g, UINT title, int width, BOOL right, BOOL heat );
void grid_begin( struct grid *g );
struct grid_row *grid_add_row( struct grid *g );
void grid_set_cell( struct grid_row *row, UINT col, const WCHAR *text );
void grid_end( struct grid *g );    /* sorts, keeps the selection, repaints */
void grid_update_header( struct grid *g );
struct grid_row *grid_selected( struct grid *g );
void grid_destroy( struct grid *g );

/* ------------------------------------------------------------------ the pages */

struct page
{
    UINT     title;
    HWND     hwnd;
    void   (*create)( HWND parent );
    void   (*refresh)(void);
    void   (*resize)( int width, int height );
    void   (*end_task)(void);      /* the button at the bottom, NULL if none */
    BOOL   (*can_end)(void);
};

extern HINSTANCE instance;
extern HWND main_window;
extern HFONT ui_font, ui_bold, big_font, title_font, small_font;
extern UINT dpi;

int scale( int px );               /* 96-DPI pixels at the window's DPI */
const WCHAR *load_string( UINT id );
void format_bytes( WCHAR *buf, size_t len, double bytes, BOOL per_second );
void format_percent( WCHAR *buf, size_t len, double pct );
void update_end_button(void);
void set_page_hwnd( UINT title, HWND hwnd );
void set_compact_hwnd( HWND hwnd );
void minimize_after_use(void);
void show_details_of( DWORD pid );  /* "Go to details" */

void processes_create( HWND parent );
void processes_refresh(void);
void processes_resize( int w, int h );
void processes_end_task(void);
BOOL processes_can_end(void);

void perf_create( HWND parent );
void perf_refresh(void);
void perf_resize( int w, int h );

void history_create( HWND parent );
void history_refresh(void);
void history_resize( int w, int h );
void history_track(void);          /* each sample, whichever tab is in front */

void startup_create( HWND parent );
void startup_refresh(void);
void startup_resize( int w, int h );
void startup_toggle(void);
int  startup_can_toggle(void);     /* 0 nothing selected, 1 it is on, 2 it is off */
BOOL startup_can_end(void);

void users_create( HWND parent );
void users_refresh(void);
void users_resize( int w, int h );

void details_create( HWND parent );
void details_refresh(void);
void details_resize( int w, int h );
void details_end_task(void);
BOOL details_can_end(void);
void details_select( DWORD pid );

void services_create( HWND parent );
void services_refresh(void);
void services_resize( int w, int h );

/* the compact view: the programs and End task */
void compact_create( HWND parent );
void compact_refresh(void);
void compact_resize( int w, int h );
void compact_end_task(void);
BOOL compact_can_end(void);

#endif
