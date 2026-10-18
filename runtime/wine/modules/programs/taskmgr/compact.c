/*
 * Task Manager: "Fewer details", the programs that are open and End task
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "taskmgr.h"

static HWND page;
static struct grid *grid;

void compact_refresh(void)
{
    if (!grid) return;
    grid_begin( grid );
    for (UINT i = 0; i < proc_count; i++)
    {
        struct grid_row *row;
        if (procs[i].kind != KIND_APP || !procs[i].window) continue;
        if (!(row = grid_add_row( grid ))) break;
        grid_set_cell( row, 0, procs[i].title );
        row->icon = procs[i].icon;
        row->param = procs[i].pid;
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

BOOL compact_can_end(void) { return selected() != NULL; }

void compact_end_task(void)
{
    const struct proc *p = selected();
    if (!p) return;
    if (p->window) data_end_window( p->window, p->pid );
    else data_end_process( p->pid, FALSE );
}

static LRESULT CALLBACK page_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    if (msg == WM_COMMAND && LOWORD(wparam) == IDC_GRID)
    {
        const struct proc *p = selected();
        switch (HIWORD(wparam))
        {
        case GN_SELCHANGE: update_end_button(); break;
        case GN_DBLCLK:
            /* a double click switches to the program, as Windows' compact view does */
            if (p && p->window)
            {
                if (IsIconic( p->window )) ShowWindow( p->window, SW_RESTORE );
                SetForegroundWindow( p->window );
            }
            break;
        case GN_KEYDELETE: compact_end_task(); break;
        }
        return 0;
    }
    if (msg == WM_PAINT && grid && !grid->row_count)
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint( hwnd, &ps );
        RECT rc;
        GetClientRect( hwnd, &rc );
        FillRect( hdc, &rc, GetStockObject( WHITE_BRUSH ) );
        EndPaint( hwnd, &ps );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

void compact_create( HWND parent )
{
    WNDCLASSW wc = { 0, page_proc, 0, 0, instance };

    wc.hbrBackground = GetStockObject( WHITE_BRUSH );
    wc.lpszClassName = L"ArcticTaskCompact";
    RegisterClassW( &wc );
    page = CreateWindowExW( 0, wc.lpszClassName, NULL, WS_CHILD | WS_CLIPCHILDREN, 0, 0, 0, 0, parent, NULL, instance, NULL );
    grid = grid_create( page, IDC_GRID, FALSE );
    grid->no_header = TRUE;
    grid->icons = TRUE;
    grid_add_col( grid, IDS_COL_NAME, 300, FALSE, FALSE );
    grid->sort_col = 0;
    set_compact_hwnd( page );
}

void compact_resize( int w, int h )
{
    if (grid) SetWindowPos( grid->hwnd, NULL, 0, 0, w, h, SWP_NOZORDER );
}
