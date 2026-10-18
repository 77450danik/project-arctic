/*
 * Task Manager: the Processes tab
 *
 * Programs are the processes with a window on the taskbar, together with
 * every other process of the same program (Chrome's renderers under
 * "Google Chrome (12)"); Windows processes are the system's own; the rest
 * run in the background. CPU, memory and disk warm their cells with their
 * value, and the header carries each column's total.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <math.h>
#include "taskmgr.h"
#include "shellapi.h"
#include "shlobj.h"

#define APP_FLAG       0x40000000  /* the param of a program's row: a hash of its path */

static HWND page;
static struct grid *grid;
static LPARAM *expanded;           /* programs opened into their processes */
static UINT expanded_count;
static BOOL group_by_type = TRUE;

static LPARAM app_key( const struct proc *p )
{
    const WCHAR *s = p->path[0] ? p->path : p->exe;
    DWORD h = 5381;
    for (; *s; s++) h = h * 33 + towlower( *s );
    return APP_FLAG | (h & 0x3fffffff);
}

static BOOL is_expanded( LPARAM key )
{
    for (UINT i = 0; i < expanded_count; i++) if (expanded[i] == key) return TRUE;
    return FALSE;
}

static void toggle_expanded( LPARAM key )
{
    for (UINT i = 0; i < expanded_count; i++)
        if (expanded[i] == key) { expanded[i] = expanded[--expanded_count]; return; }
    if ((expanded = realloc( expanded, (expanded_count + 1) * sizeof(*expanded) )))
        expanded[expanded_count++] = key;
}

static double heat_cpu( double pct ) { return sqrt( min( 1.0, pct / 100.0 ) ); }
static double heat_mem( double bytes ) { return sqrt( min( 1.0, bytes / (2.0 * 1024 * 1024 * 1024) ) ); }
static double heat_disk( double bps ) { return sqrt( min( 1.0, bps / (50.0 * 1024 * 1024) ) ); }

static void fill_cells( struct grid_row *row, double cpu, double mem, double disk )
{
    WCHAR buf[64];

    format_percent( buf, ARRAY_SIZE(buf), cpu );
    grid_set_cell( row, 2, buf );
    format_bytes( buf, ARRAY_SIZE(buf), mem, FALSE );
    grid_set_cell( row, 3, buf );
    if (disk < 0.05 * 1024 * 1024) swprintf( buf, ARRAY_SIZE(buf), load_string( IDS_UNIT_PER_S ), L"0 МБ" );
    else format_bytes( buf, ARRAY_SIZE(buf), disk, TRUE );
    grid_set_cell( row, 4, buf );
    swprintf( buf, ARRAY_SIZE(buf), L"0 %s", load_string( IDS_UNIT_MBPS ) );
    grid_set_cell( row, 5, buf );
    row->sort[2] = cpu;
    row->sort[3] = mem;
    row->sort[4] = disk;
    row->heat[2] = heat_cpu( cpu );
    row->heat[3] = heat_mem( mem );
    row->heat[4] = heat_disk( disk );
    row->heat[5] = 0;
}

static void add_process_row( const struct proc *p, int group, int indent )
{
    struct grid_row *row = grid_add_row( grid );

    if (!row) return;
    grid_set_cell( row, 0, indent && p->window_title[0] ? p->window_title : p->title );
    grid_set_cell( row, 1, p->suspended ? load_string( IDS_SUSPENDED ) : L"" );
    fill_cells( row, p->cpu, p->private_ws, p->disk );
    row->icon = p->icon;
    row->indent = indent;
    row->param = p->pid;
    row->group_id = group;
}

static void add_group_title( UINT title, UINT count, int group )
{
    struct grid_row *row = grid_add_row( grid );
    WCHAR buf[128];

    if (!row) return;
    swprintf( buf, ARRAY_SIZE(buf), L"%s (%u)", load_string( title ), count );
    grid_set_cell( row, 0, buf );
    row->group = TRUE;
    row->group_id = group;
}

static void update_totals(void)
{
    const struct sample *s = data_latest();
    double mem_pct = sysinfo.mem_total ? s->mem_used * 100.0 / sysinfo.mem_total : 0;

    swprintf( grid->cols[2].total, ARRAY_SIZE(grid->cols[2].total), L"%.0f%%", s->cpu );
    swprintf( grid->cols[3].total, ARRAY_SIZE(grid->cols[3].total), L"%.0f%%", mem_pct );
    swprintf( grid->cols[4].total, ARRAY_SIZE(grid->cols[4].total), L"%.0f%%", s->disk_active );
    swprintf( grid->cols[5].total, ARRAY_SIZE(grid->cols[5].total), L"0%%" );
}

void processes_refresh(void)
{
    UINT counts[3] = { 0 };
    BOOL *done;

    if (!grid) return;
    if (!(done = calloc( proc_count + 1, sizeof(*done) ))) return;
    grid_begin( grid );

    /* programs: every process of a program that has a window */
    for (UINT i = 0; i < proc_count; i++)
    {
        struct grid_row *row;
        LPARAM key;
        double cpu = 0, mem = 0, disk = 0;
        UINT n = 0;

        if (done[i] || procs[i].kind != KIND_APP) continue;
        key = app_key( &procs[i] );
        for (UINT j = 0; j < proc_count; j++)
            if (!done[j] && app_key( &procs[j] ) == key)
            {
                cpu += procs[j].cpu;
                mem += procs[j].private_ws;
                disk += procs[j].disk;
                n++;
            }
        if (!(row = grid_add_row( grid ))) break;
        if (n > 1)
        {
            WCHAR buf[300];
            swprintf( buf, ARRAY_SIZE(buf), L"%s (%u)", procs[i].title, n );
            grid_set_cell( row, 0, buf );
        }
        else grid_set_cell( row, 0, procs[i].title );
        fill_cells( row, min( cpu, 100 ), mem, disk );
        row->icon = procs[i].icon;
        row->param = key;
        row->group_id = 0;
        row->expandable = TRUE;
        row->expanded = is_expanded( key );
        counts[0]++;
        for (UINT j = 0; j < proc_count; j++)
        {
            if (done[j] || app_key( &procs[j] ) != key) continue;
            done[j] = TRUE;
            /* opened: each process, by its window's title if it has one */
            if (row->expanded) add_process_row( &procs[j], 0, 1 );
        }
    }
    for (UINT i = 0; i < proc_count; i++)
    {
        int group;
        if (done[i]) continue;
        group = !group_by_type ? 0 : procs[i].kind == KIND_WINDOWS ? 2 : 1;
        add_process_row( &procs[i], group, 0 );
        counts[group]++;
    }
    if (group_by_type)
    {
        if (counts[0]) add_group_title( IDS_GROUP_APPS, counts[0], 0 );
        if (counts[1]) add_group_title( IDS_GROUP_BACKGROUND, counts[1], 1 );
        if (counts[2]) add_group_title( IDS_GROUP_WINDOWS, counts[2], 2 );
    }
    free( done );
    update_totals();
    grid_end( grid );
}

/* the processes a row stands for: a program's are all of its own */
static UINT selected_pids( DWORD *pids, UINT max_pids, HWND *window )
{
    struct grid_row *row = grid_selected( grid );
    UINT n = 0;

    *window = NULL;
    if (!row) return 0;
    for (UINT i = 0; i < proc_count && n < max_pids; i++)
    {
        BOOL match = (row->param & APP_FLAG) ? app_key( &procs[i] ) == row->param : procs[i].pid == (DWORD)row->param;
        if (!match) continue;
        pids[n++] = procs[i].pid;
        if (!*window && procs[i].window) *window = procs[i].window;
    }
    return n;
}

BOOL processes_can_end(void)
{
    struct grid_row *row = grid ? grid_selected( grid ) : NULL;
    return row && !row->group;
}

static BOOL is_system_pid( DWORD pid )
{
    for (UINT i = 0; i < proc_count; i++) if (procs[i].pid == pid) return procs[i].kind == KIND_WINDOWS;
    return FALSE;
}

void processes_end_task(void)
{
    DWORD pids[256];
    HWND window;
    UINT n = selected_pids( pids, ARRAY_SIZE(pids), &window );
    struct grid_row *row = grid_selected( grid );

    if (!n) return;
    /* a program that has a window is asked to close first, as Windows does */
    if (window && (row->param & APP_FLAG) && n == 1)
    {
        data_end_window( window, pids[0] );
        return;
    }
    if (is_system_pid( pids[0] ) &&
        MessageBoxW( main_window, load_string( IDS_CONFIRM_END_SYSTEM ), load_string( IDS_APP_TITLE ),
                     MB_ICONWARNING | MB_OKCANCEL | MB_DEFBUTTON2 ) != IDOK) return;
    for (UINT i = 0; i < n; i++) data_end_process( pids[i], FALSE );
}

static void open_location( const WCHAR *path )
{
    WCHAR args[MAX_PATH + 16];
    if (!path[0]) return;
    swprintf( args, ARRAY_SIZE(args), L"/select,\"%s\"", path );
    ShellExecuteW( main_window, NULL, L"explorer.exe", args, NULL, SW_SHOWNORMAL );
}

static const struct proc *first_selected(void)
{
    DWORD pids[1];
    HWND window;
    if (!selected_pids( pids, 1, &window )) return NULL;
    for (UINT i = 0; i < proc_count; i++) if (procs[i].pid == pids[0]) return &procs[i];
    return NULL;
}

static void context_menu(void)
{
    HMENU popup = LoadMenuW( instance, MAKEINTRESOURCEW(IDR_POPUP) ), sub = GetSubMenu( popup, 0 );
    struct grid_row *row = grid_selected( grid );
    POINT pt;
    UINT cmd;
    HWND window;
    DWORD pids[256];
    const struct proc *p;

    GetCursorPos( &pt );
    if (!row || !(row->param & APP_FLAG)) DeleteMenu( sub, IDM_SWITCH_TO, MF_BYCOMMAND );
    if (!row || !row->expandable) DeleteMenu( sub, IDM_EXPAND_ALL, MF_BYCOMMAND );
    else ModifyMenuW( sub, IDM_EXPAND_ALL, MF_BYCOMMAND | MF_STRING, IDM_EXPAND_ALL,
                      load_string( row->expanded ? IDS_COLLAPSE : IDS_EXPAND ) );
    cmd = TrackPopupMenu( sub, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, main_window, NULL );
    DestroyMenu( popup );
    selected_pids( pids, ARRAY_SIZE(pids), &window );
    p = first_selected();
    switch (cmd)
    {
    case IDM_EXPAND_ALL:
        toggle_expanded( row->param );
        processes_refresh();
        break;
    case IDM_SWITCH_TO:
        if (window)
        {
            if (IsIconic( window )) ShowWindow( window, SW_RESTORE );
            SetForegroundWindow( window );
        }
        break;
    case IDM_END_TASK:
        processes_end_task();
        break;
    case IDM_OPEN_LOCATION:
        if (p) open_location( p->path );
        break;
    case IDM_SEARCH_ONLINE:
        if (p)
        {
            WCHAR url[512];
            swprintf( url, ARRAY_SIZE(url), L"https://www.google.com/search?q=%s", p->exe );
            ShellExecuteW( main_window, NULL, url, NULL, NULL, SW_SHOWNORMAL );
        }
        break;
    case IDM_PROPERTIES:
        if (p && p->path[0])
        {
            SHELLEXECUTEINFOW sei = { sizeof(sei), SEE_MASK_INVOKEIDLIST };
            sei.lpVerb = L"properties";
            sei.lpFile = p->path;
            sei.nShow = SW_SHOW;
            ShellExecuteExW( &sei );
        }
        break;
    case IDM_GO_TO_DETAILS:
        if (p) show_details_of( p->pid );
        break;
    }
}

static LRESULT CALLBACK page_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    switch (msg)
    {
    case WM_COMMAND:
        if (LOWORD(wparam) == IDC_GRID)
        {
            struct grid_row *row = grid_selected( grid );
            switch (HIWORD(wparam))
            {
            case GN_SELCHANGE: update_end_button(); break;
            case GN_EXPAND:
                if (row && row->expandable) { toggle_expanded( row->param ); processes_refresh(); }
                break;
            case GN_DBLCLK:
                if (row && (row->param & APP_FLAG)) { toggle_expanded( row->param ); processes_refresh(); }
                break;
            case GN_CONTEXT: context_menu(); break;
            case GN_KEYDELETE: processes_end_task(); break;
            }
            return 0;
        }
        switch (LOWORD(wparam))
        {
        case IDM_GROUP_BY_TYPE: group_by_type = !group_by_type; processes_refresh(); return 0;
        case IDM_EXPAND_ALL:
            for (UINT i = 0; i < grid->row_count; i++)
                if (grid->rows[i].expandable && !grid->rows[i].expanded) toggle_expanded( grid->rows[i].param );
            processes_refresh();
            return 0;
        case IDM_COLLAPSE_ALL:
            expanded_count = 0;
            processes_refresh();
            return 0;
        }
        break;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

void processes_create( HWND parent )
{
    WNDCLASSW wc = { 0, page_proc, 0, 0, instance };

    wc.hbrBackground = GetStockObject( WHITE_BRUSH );
    wc.lpszClassName = L"ArcticTaskProcesses";
    RegisterClassW( &wc );
    page = CreateWindowExW( 0, wc.lpszClassName, NULL, WS_CHILD | WS_CLIPCHILDREN, 0, 0, 0, 0, parent, NULL, instance, NULL );
    grid = grid_create( page, IDC_GRID, TRUE );
    grid->groups = TRUE;
    grid->icons = TRUE;
    grid->tree = TRUE;
    grid_add_col( grid, IDS_COL_NAME, 260, FALSE, FALSE );
    grid_add_col( grid, IDS_COL_STATUS, 90, FALSE, FALSE );
    grid_add_col( grid, IDS_COL_CPU, 70, TRUE, TRUE );
    grid_add_col( grid, IDS_COL_MEMORY, 90, TRUE, TRUE );
    grid_add_col( grid, IDS_COL_DISK, 80, TRUE, TRUE );
    grid_add_col( grid, IDS_COL_NETWORK, 80, TRUE, TRUE );
    /* Windows' default order: the programs first, by name */
    grid->sort_col = 0;
    set_page_hwnd( IDS_TAB_PROCESSES, page );
}

void processes_resize( int w, int h )
{
    if (grid) SetWindowPos( grid->hwnd, NULL, 0, 0, w, h, SWP_NOZORDER );
}
