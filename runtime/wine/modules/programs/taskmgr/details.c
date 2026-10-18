/*
 * Task Manager: the Details tab, every process by its image name
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "taskmgr.h"
#include "shellapi.h"

static HWND page;
static struct grid *grid;

static UINT priority_name( LONG base )
{
    if (base >= 24) return IDS_PRIO_REALTIME;
    if (base >= 13) return IDS_PRIO_HIGH;
    if (base >= 10) return IDS_PRIO_ABOVE;
    if (base >= 8) return IDS_PRIO_NORMAL;
    if (base >= 6) return IDS_PRIO_BELOW;
    return IDS_PRIO_IDLE;
}

void details_refresh(void)
{
    WCHAR buf[64];

    if (!grid) return;
    grid_begin( grid );
    for (UINT i = 0; i < proc_count; i++)
    {
        const struct proc *p = &procs[i];
        struct grid_row *row = grid_add_row( grid );

        if (!row) break;
        grid_set_cell( row, 0, p->exe );
        swprintf( buf, ARRAY_SIZE(buf), L"%u", p->pid );
        grid_set_cell( row, 1, buf );
        row->sort[1] = p->pid;
        grid_set_cell( row, 2, load_string( p->suspended ? IDS_SUSPENDED : IDS_RUNNING ) );
        grid_set_cell( row, 3, p->user );
        swprintf( buf, ARRAY_SIZE(buf), L"%02.0f", p->cpu );
        grid_set_cell( row, 4, buf );
        row->sort[4] = p->cpu;
        swprintf( buf, ARRAY_SIZE(buf), L"%I64u K", (ULONGLONG)p->private_ws / 1024 );
        grid_set_cell( row, 5, buf );
        row->sort[5] = (double)p->private_ws;
        swprintf( buf, ARRAY_SIZE(buf), L"%u", p->threads );
        grid_set_cell( row, 6, buf );
        row->sort[6] = p->threads;
        swprintf( buf, ARRAY_SIZE(buf), L"%u", p->handles );
        grid_set_cell( row, 7, buf );
        row->sort[7] = p->handles;
        grid_set_cell( row, 8, load_string( priority_name( p->priority ) ) );
        grid_set_cell( row, 9, wcsicmp( p->title, p->exe ) ? p->title : L"" );
        row->icon = p->icon;
        row->param = p->pid;
    }
    grid_end( grid );
}

static const struct proc *selected(void)
{
    struct grid_row *row = grid ? grid_selected( grid ) : NULL;
    if (!row) return NULL;
    for (UINT i = 0; i < proc_count; i++) if (procs[i].pid == (DWORD)row->param) return &procs[i];
    return NULL;
}

BOOL details_can_end(void) { return selected() != NULL; }

static void end_selected( BOOL tree )
{
    const struct proc *p = selected();
    WCHAR text[512];

    if (!p) return;
    swprintf( text, ARRAY_SIZE(text), load_string( IDS_CONFIRM_END_TEXT ), p->exe );
    if (MessageBoxW( main_window, text, load_string( IDS_CONFIRM_END_TITLE ),
                     MB_ICONWARNING | MB_OKCANCEL | MB_DEFBUTTON2 ) != IDOK) return;
    if (!data_end_process( p->pid, tree ))
        MessageBoxW( main_window, load_string( IDS_END_FAILED ), load_string( IDS_APP_TITLE ), MB_ICONERROR );
}

void details_end_task(void) { end_selected( FALSE ); }

void details_select( DWORD pid )
{
    if (!grid) return;
    grid->selected_param = pid;
    details_refresh();
    SetFocus( grid->hwnd );
}

static void context_menu(void)
{
    HMENU popup = LoadMenuW( instance, MAKEINTRESOURCEW(IDR_POPUP) ), sub = GetSubMenu( popup, 1 );
    const struct proc *p = selected();
    static const DWORD classes[] = { REALTIME_PRIORITY_CLASS, HIGH_PRIORITY_CLASS, ABOVE_NORMAL_PRIORITY_CLASS,
                                     NORMAL_PRIORITY_CLASS, BELOW_NORMAL_PRIORITY_CLASS, IDLE_PRIORITY_CLASS };
    POINT pt;
    UINT cmd;

    if (!p) { DestroyMenu( popup ); return; }
    CheckMenuRadioItem( sub, IDM_PRIO_REALTIME, IDM_PRIO_IDLE, IDM_PRIO_REALTIME + priority_name( p->priority ) - IDS_PRIO_REALTIME,
                        MF_BYCOMMAND );
    GetCursorPos( &pt );
    cmd = TrackPopupMenu( sub, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, main_window, NULL );
    DestroyMenu( popup );
    switch (cmd)
    {
    case IDM_END_TASK: end_selected( FALSE ); break;
    case IDM_END_TREE: end_selected( TRUE ); break;
    case IDM_PRIO_REALTIME: case IDM_PRIO_HIGH: case IDM_PRIO_ABOVE: case IDM_PRIO_NORMAL:
    case IDM_PRIO_BELOW: case IDM_PRIO_IDLE:
        data_set_priority( p->pid, classes[cmd - IDM_PRIO_REALTIME] );
        break;
    case IDM_OPEN_LOCATION:
        if (p->path[0])
        {
            WCHAR args[MAX_PATH + 16];
            swprintf( args, ARRAY_SIZE(args), L"/select,\"%s\"", p->path );
            ShellExecuteW( main_window, NULL, L"explorer.exe", args, NULL, SW_SHOWNORMAL );
        }
        break;
    case IDM_SEARCH_ONLINE:
    {
        WCHAR url[512];
        swprintf( url, ARRAY_SIZE(url), L"https://www.google.com/search?q=%s", p->exe );
        ShellExecuteW( main_window, NULL, url, NULL, NULL, SW_SHOWNORMAL );
        break;
    }
    case IDM_PROPERTIES:
        if (p->path[0])
        {
            SHELLEXECUTEINFOW sei = { sizeof(sei), SEE_MASK_INVOKEIDLIST };
            sei.lpVerb = L"properties";
            sei.lpFile = p->path;
            sei.nShow = SW_SHOW;
            ShellExecuteExW( &sei );
        }
        break;
    }
}

static LRESULT CALLBACK page_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    if (msg == WM_COMMAND && LOWORD(wparam) == IDC_GRID)
    {
        switch (HIWORD(wparam))
        {
        case GN_SELCHANGE: update_end_button(); break;
        case GN_CONTEXT: context_menu(); break;
        case GN_KEYDELETE: end_selected( FALSE ); break;
        }
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

void details_create( HWND parent )
{
    WNDCLASSW wc = { 0, page_proc, 0, 0, instance };

    wc.hbrBackground = GetStockObject( WHITE_BRUSH );
    wc.lpszClassName = L"ArcticTaskDetails";
    RegisterClassW( &wc );
    page = CreateWindowExW( 0, wc.lpszClassName, NULL, WS_CHILD | WS_CLIPCHILDREN, 0, 0, 0, 0, parent, NULL, instance, NULL );
    grid = grid_create( page, IDC_GRID, FALSE );
    grid->icons = TRUE;
    grid_add_col( grid, IDS_COL_NAME, 180, FALSE, FALSE );
    grid_add_col( grid, IDS_COL_PID, 60, TRUE, FALSE );
    grid_add_col( grid, IDS_COL_STATUS, 90, FALSE, FALSE );
    grid_add_col( grid, IDS_COL_USER, 100, FALSE, FALSE );
    grid_add_col( grid, IDS_COL_CPU, 50, TRUE, FALSE );
    grid_add_col( grid, IDS_COL_MEMORY, 110, TRUE, FALSE );
    grid_add_col( grid, IDS_COL_THREADS, 60, TRUE, FALSE );
    grid_add_col( grid, IDS_COL_HANDLES, 80, TRUE, FALSE );
    grid_add_col( grid, IDS_COL_PRIORITY, 100, FALSE, FALSE );
    grid_add_col( grid, IDS_COL_DESCRIPTION, 220, FALSE, FALSE );
    grid->sort_col = 0;
    set_page_hwnd( IDS_TAB_DETAILS, page );
}

void details_resize( int w, int h )
{
    if (grid) SetWindowPos( grid->hwnd, NULL, 0, 0, w, h, SWP_NOZORDER );
}
