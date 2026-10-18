/*
 * Task Manager: the Services tab
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "taskmgr.h"
#include "winsvc.h"
#include "shellapi.h"

static HWND page;
static struct grid *grid;
static ENUM_SERVICE_STATUS_PROCESSW *services;
static DWORD service_count;

static UINT state_name( DWORD state )
{
    switch (state)
    {
    case SERVICE_RUNNING: return IDS_RUNNING;
    case SERVICE_START_PENDING: return IDS_STARTING;
    case SERVICE_STOP_PENDING: return IDS_STOPPING;
    default: return IDS_STOPPED;
    }
}

static void read_services(void)
{
    SC_HANDLE scm = OpenSCManagerW( NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE );
    DWORD needed = 0, resume = 0;

    free( services );
    services = NULL;
    service_count = 0;
    if (!scm) return;
    EnumServicesStatusExW( scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, NULL, 0, &needed,
                           &service_count, &resume, NULL );
    if (needed && (services = malloc( needed )))
    {
        resume = 0;
        if (!EnumServicesStatusExW( scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, (BYTE *)services,
                                    needed, &needed, &service_count, &resume, NULL ))
            service_count = 0;
    }
    CloseServiceHandle( scm );
}

void services_refresh(void)
{
    WCHAR buf[32];

    if (!grid) return;
    read_services();
    grid_begin( grid );
    for (DWORD i = 0; i < service_count; i++)
    {
        const ENUM_SERVICE_STATUS_PROCESSW *s = &services[i];
        struct grid_row *row = grid_add_row( grid );

        if (!row) break;
        grid_set_cell( row, 0, s->lpServiceName );
        if (s->ServiceStatusProcess.dwProcessId)
        {
            swprintf( buf, ARRAY_SIZE(buf), L"%u", s->ServiceStatusProcess.dwProcessId );
            grid_set_cell( row, 1, buf );
        }
        row->sort[1] = s->ServiceStatusProcess.dwProcessId;
        grid_set_cell( row, 2, s->lpDisplayName );
        grid_set_cell( row, 3, load_string( state_name( s->ServiceStatusProcess.dwCurrentState ) ) );
        /* params of services: their index, plus one so that none is zero */
        row->param = (LPARAM)i + 1;
    }
    grid_end( grid );
}

static const ENUM_SERVICE_STATUS_PROCESSW *selected(void)
{
    struct grid_row *row = grid ? grid_selected( grid ) : NULL;
    if (!row || row->param < 1 || (DWORD)row->param > service_count) return NULL;
    return &services[row->param - 1];
}

static void control( const WCHAR *name, UINT cmd )
{
    SC_HANDLE scm = OpenSCManagerW( NULL, NULL, SC_MANAGER_CONNECT ), svc;
    SERVICE_STATUS status;

    if (!scm) return;
    if ((svc = OpenServiceW( scm, name, SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS )))
    {
        if (cmd == IDM_SERVICE_STOP || cmd == IDM_SERVICE_RESTART)
        {
            ControlService( svc, SERVICE_CONTROL_STOP, &status );
            for (int i = 0; i < 50 && QueryServiceStatus( svc, &status ) && status.dwCurrentState != SERVICE_STOPPED; i++)
                Sleep( 100 );
        }
        if (cmd == IDM_SERVICE_START || cmd == IDM_SERVICE_RESTART) StartServiceW( svc, 0, NULL );
        CloseServiceHandle( svc );
    }
    CloseServiceHandle( scm );
    services_refresh();
}

static void context_menu(void)
{
    HMENU popup = LoadMenuW( instance, MAKEINTRESOURCEW(IDR_POPUP) ), sub = GetSubMenu( popup, 2 );
    const ENUM_SERVICE_STATUS_PROCESSW *s = selected();
    WCHAR name[256];
    DWORD pid;
    POINT pt;
    UINT cmd;

    if (!s) { DestroyMenu( popup ); return; }
    lstrcpynW( name, s->lpServiceName, ARRAY_SIZE(name) );
    pid = s->ServiceStatusProcess.dwProcessId;
    if (s->ServiceStatusProcess.dwCurrentState == SERVICE_RUNNING) EnableMenuItem( sub, IDM_SERVICE_START, MF_GRAYED );
    else
    {
        EnableMenuItem( sub, IDM_SERVICE_STOP, MF_GRAYED );
        EnableMenuItem( sub, IDM_SERVICE_RESTART, MF_GRAYED );
    }
    if (!pid) EnableMenuItem( sub, IDM_GO_TO_DETAILS, MF_GRAYED );
    GetCursorPos( &pt );
    cmd = TrackPopupMenu( sub, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, main_window, NULL );
    DestroyMenu( popup );
    switch (cmd)
    {
    case IDM_SERVICE_START: case IDM_SERVICE_STOP: case IDM_SERVICE_RESTART:
        control( name, cmd );
        break;
    case IDM_OPEN_SERVICES:
        ShellExecuteW( main_window, NULL, L"services.msc", NULL, NULL, SW_SHOWNORMAL );
        break;
    case IDM_SEARCH_ONLINE:
    {
        WCHAR url[512];
        swprintf( url, ARRAY_SIZE(url), L"https://www.google.com/search?q=%s", name );
        ShellExecuteW( main_window, NULL, url, NULL, NULL, SW_SHOWNORMAL );
        break;
    }
    case IDM_GO_TO_DETAILS:
        show_details_of( pid );
        break;
    }
}

static LRESULT CALLBACK page_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    if (msg == WM_COMMAND && LOWORD(wparam) == IDC_GRID)
    {
        if (HIWORD(wparam) == GN_CONTEXT) context_menu();
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

void services_create( HWND parent )
{
    WNDCLASSW wc = { 0, page_proc, 0, 0, instance };

    wc.hbrBackground = GetStockObject( WHITE_BRUSH );
    wc.lpszClassName = L"ArcticTaskServices";
    RegisterClassW( &wc );
    page = CreateWindowExW( 0, wc.lpszClassName, NULL, WS_CHILD | WS_CLIPCHILDREN, 0, 0, 0, 0, parent, NULL, instance, NULL );
    grid = grid_create( page, IDC_GRID, FALSE );
    grid_add_col( grid, IDS_COL_NAME, 160, FALSE, FALSE );
    grid_add_col( grid, IDS_COL_PID, 60, TRUE, FALSE );
    grid_add_col( grid, IDS_COL_DESCRIPTION, 300, FALSE, FALSE );
    grid_add_col( grid, IDS_COL_STATUS, 100, FALSE, FALSE );
    grid->sort_col = 0;
    set_page_hwnd( IDS_TAB_SERVICES, page );
}

void services_resize( int w, int h )
{
    if (grid) SetWindowPos( grid->hwnd, NULL, 0, 0, w, h, SWP_NOZORDER );
}
