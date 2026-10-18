/*
 * Task Manager: the Users and App history tabs
 *
 * Users: each signed-in user with the sum of their processes, opening into
 * them. App history: the processor time each program with a window has
 * used since Task Manager started watching, the ones that have ended
 * included.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <math.h>
#include "taskmgr.h"

/* ------------------------------------------------------------------ users */

static HWND users_page;
static struct grid *users_grid;
static BOOL users_open = TRUE;

static double heat_of( double v, double full ) { return sqrt( min( 1.0, v / full ) ); }

void users_refresh(void)
{
    WCHAR names[16][64], buf[64];
    UINT name_count = 0;

    if (!users_grid) return;
    for (UINT i = 0; i < proc_count; i++)
    {
        UINT n;
        if (!procs[i].user[0]) continue;
        for (n = 0; n < name_count; n++) if (!wcsicmp( names[n], procs[i].user )) break;
        if (n == name_count && name_count < ARRAY_SIZE(names)) lstrcpynW( names[name_count++], procs[i].user, 64 );
    }
    grid_begin( users_grid );
    for (UINT n = 0; n < name_count; n++)
    {
        struct grid_row *row = grid_add_row( users_grid );
        double cpu = 0, mem = 0, disk = 0;
        UINT count = 0;

        if (!row) break;
        for (UINT i = 0; i < proc_count; i++)
            if (!wcsicmp( procs[i].user, names[n] )) { cpu += procs[i].cpu; mem += procs[i].private_ws; disk += procs[i].disk; count++; }
        swprintf( buf, ARRAY_SIZE(buf), L"%s (%u)", names[n], count );
        grid_set_cell( row, 0, buf );
        grid_set_cell( row, 1, L"" );
        format_percent( buf, ARRAY_SIZE(buf), min( cpu, 100 ) );
        grid_set_cell( row, 2, buf );
        format_bytes( buf, ARRAY_SIZE(buf), mem, FALSE );
        grid_set_cell( row, 3, buf );
        format_bytes( buf, ARRAY_SIZE(buf), disk, TRUE );
        grid_set_cell( row, 4, buf );
        row->heat[2] = heat_of( cpu, 100 );
        row->heat[3] = heat_of( mem, 8.0 * 1024 * 1024 * 1024 );
        row->heat[4] = heat_of( disk, 50.0 * 1024 * 1024 );
        row->sort[2] = cpu; row->sort[3] = mem; row->sort[4] = disk;
        row->param = 0x40000000 | n;
        row->expandable = TRUE;
        row->expanded = users_open;
        if (!users_open) continue;
        for (UINT i = 0; i < proc_count; i++)
        {
            struct grid_row *child;
            if (wcsicmp( procs[i].user, names[n] ) || !(child = grid_add_row( users_grid ))) continue;
            grid_set_cell( child, 0, procs[i].title );
            format_percent( buf, ARRAY_SIZE(buf), procs[i].cpu );
            grid_set_cell( child, 2, buf );
            format_bytes( buf, ARRAY_SIZE(buf), procs[i].private_ws, FALSE );
            grid_set_cell( child, 3, buf );
            format_bytes( buf, ARRAY_SIZE(buf), procs[i].disk, TRUE );
            grid_set_cell( child, 4, buf );
            child->heat[2] = heat_of( procs[i].cpu, 100 );
            child->heat[3] = heat_of( procs[i].private_ws, 2.0 * 1024 * 1024 * 1024 );
            child->heat[4] = heat_of( procs[i].disk, 50.0 * 1024 * 1024 );
            child->sort[2] = procs[i].cpu; child->sort[3] = procs[i].private_ws; child->sort[4] = procs[i].disk;
            child->icon = procs[i].icon;
            child->indent = 1;
            child->param = procs[i].pid;
        }
    }
    {
        const struct sample *s = data_latest();
        swprintf( users_grid->cols[2].total, 32, L"%.0f%%", s->cpu );
        swprintf( users_grid->cols[3].total, 32, L"%.0f%%", sysinfo.mem_total ? s->mem_used * 100.0 / sysinfo.mem_total : 0 );
        swprintf( users_grid->cols[4].total, 32, L"%.0f%%", s->disk_active );
    }
    grid_end( users_grid );
}

static LRESULT CALLBACK users_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    if (msg == WM_COMMAND && LOWORD(wparam) == IDC_GRID)
    {
        if (HIWORD(wparam) == GN_EXPAND || HIWORD(wparam) == GN_DBLCLK)
        {
            users_open = !users_open;
            users_refresh();
        }
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

void users_create( HWND parent )
{
    WNDCLASSW wc = { 0, users_proc, 0, 0, instance };

    wc.hbrBackground = GetStockObject( WHITE_BRUSH );
    wc.lpszClassName = L"ArcticTaskUsers";
    RegisterClassW( &wc );
    users_page = CreateWindowExW( 0, wc.lpszClassName, NULL, WS_CHILD | WS_CLIPCHILDREN, 0, 0, 0, 0, parent, NULL, instance, NULL );
    users_grid = grid_create( users_page, IDC_GRID, TRUE );
    users_grid->icons = TRUE;
    users_grid->tree = TRUE;
    grid_add_col( users_grid, IDS_COL_USER, 260, FALSE, FALSE );
    grid_add_col( users_grid, IDS_COL_STATUS, 90, FALSE, FALSE );
    grid_add_col( users_grid, IDS_COL_CPU, 70, TRUE, TRUE );
    grid_add_col( users_grid, IDS_COL_MEMORY, 90, TRUE, TRUE );
    grid_add_col( users_grid, IDS_COL_DISK, 80, TRUE, TRUE );
    set_page_hwnd( IDS_TAB_USERS, users_page );
}

void users_resize( int w, int h )
{
    if (users_grid) SetWindowPos( users_grid->hwnd, NULL, 0, 0, w, h, SWP_NOZORDER );
}

/* ------------------------------------------------------------------ App history */

struct app_time
{
    WCHAR     path[MAX_PATH], title[256];
    int       icon;
    ULONGLONG retired;             /* the time of its processes that have ended */
};
struct pid_time { DWORD pid; UINT app; ULONGLONG cpu; BOOL seen; };

static HWND history_page;
static struct grid *history_grid;
static struct app_time *apps;
static UINT app_count;
static struct pid_time *pids;
static UINT pid_count;

static UINT app_index( const struct proc *p )
{
    const WCHAR *path = p->path[0] ? p->path : p->exe;

    for (UINT i = 0; i < app_count; i++) if (!wcsicmp( apps[i].path, path )) return i;
    if (!(apps = realloc( apps, (app_count + 1) * sizeof(*apps) ))) { app_count = 0; return 0; }
    memset( &apps[app_count], 0, sizeof(*apps) );
    lstrcpynW( apps[app_count].path, path, MAX_PATH );
    lstrcpynW( apps[app_count].title, p->title, 256 );
    apps[app_count].icon = p->icon;
    return app_count++;
}

static void track(void)
{
    for (UINT j = 0; j < pid_count; j++) pids[j].seen = FALSE;
    for (UINT i = 0; i < proc_count; i++)
    {
        UINT j;
        if (procs[i].kind != KIND_APP) continue;
        for (j = 0; j < pid_count; j++) if (pids[j].pid == procs[i].pid) break;
        if (j == pid_count)
        {
            if (!(pids = realloc( pids, (pid_count + 1) * sizeof(*pids) ))) { pid_count = 0; return; }
            pids[pid_count].pid = procs[i].pid;
            pids[pid_count].app = app_index( &procs[i] );
            pid_count++;
        }
        pids[j].cpu = procs[i].cpu_time;
        pids[j].seen = TRUE;
    }
    /* a process that ended leaves its time to its program */
    for (UINT j = 0; j < pid_count; )
    {
        if (!pids[j].seen && pids[j].app < app_count)
        {
            apps[pids[j].app].retired += pids[j].cpu;
            pids[j] = pids[--pid_count];
        }
        else j++;
    }
}

void history_refresh(void)
{
    WCHAR buf[64];

    if (!history_grid) return;
    grid_begin( history_grid );
    for (UINT a = 0; a < app_count; a++)
    {
        struct grid_row *row = grid_add_row( history_grid );
        ULONGLONG total = apps[a].retired, s;

        if (!row) break;
        for (UINT j = 0; j < pid_count; j++) if (pids[j].app == a) total += pids[j].cpu;
        s = total / 10000000;
        swprintf( buf, ARRAY_SIZE(buf), L"%u:%02u:%02u", (UINT)(s / 3600), (UINT)(s / 60 % 60), (UINT)(s % 60) );
        grid_set_cell( row, 0, apps[a].title );
        grid_set_cell( row, 1, buf );
        grid_set_cell( row, 2, L"0 МБ" );
        row->sort[1] = (double)total;
        row->icon = apps[a].icon;
        row->param = a + 1;
    }
    grid_end( history_grid );
}

/* the history is kept whichever tab is in front */
void history_track(void) { track(); }

void history_create( HWND parent )
{
    WNDCLASSW wc = { 0, DefWindowProcW, 0, 0, instance };

    wc.hbrBackground = GetStockObject( WHITE_BRUSH );
    wc.lpszClassName = L"ArcticTaskHistory";
    RegisterClassW( &wc );
    history_page = CreateWindowExW( 0, wc.lpszClassName, NULL, WS_CHILD | WS_CLIPCHILDREN, 0, 0, 0, 0, parent, NULL, instance, NULL );
    history_grid = grid_create( history_page, IDC_GRID, FALSE );
    history_grid->icons = TRUE;
    grid_add_col( history_grid, IDS_COL_NAME, 260, FALSE, FALSE );
    grid_add_col( history_grid, IDS_COL_CPU_TIME, 120, TRUE, FALSE );
    grid_add_col( history_grid, IDS_COL_NETWORK, 120, TRUE, FALSE );
    history_grid->sort_col = 1;
    history_grid->sort_down = TRUE;
    set_page_hwnd( IDS_TAB_HISTORY, history_page );
}

void history_resize( int w, int h )
{
    if (history_grid) SetWindowPos( history_grid->hwnd, NULL, 0, 0, w, h, SWP_NOZORDER );
}
