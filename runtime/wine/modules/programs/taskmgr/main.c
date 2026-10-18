/*
 * Task Manager (taskmgr.exe), Windows 10's
 *
 * Wine's Task Manager is the one of Windows NT 4: a list of processes and two
 * green graphs. This one is Windows 10's: "Fewer details" with the programs
 * and End task, and the tabs of the full view, Processes (programs,
 * background processes and Windows processes, the busy columns warming with
 * their value), Performance (a graph for the processors, memory, the disk and
 * the network, and what they are made of), App history, Startup, Users,
 * Details and Services.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "taskmgr.h"
#include "shellapi.h"
#include "shlwapi.h"
#include "commdlg.h"
#include "uxtheme.h"

#define CLASS_NAME     L"TaskManagerWindow"
#define TIMER_REFRESH  1
#define SETTINGS_KEY   L"Software\\Microsoft\\Windows\\CurrentVersion\\TaskManager"

HINSTANCE instance;
HWND main_window;
HFONT ui_font, ui_bold, big_font, title_font, small_font;
UINT dpi = 96;

static HWND tabs, end_button, less_link, extra_link;
static HMENU menu;
static BOOL compact;               /* "Fewer details" */
static int current_page;
static UINT speed = IDM_SPEED_NORMAL;
static BOOL always_on_top = TRUE, minimize_on_use, hide_when_minimized;

static struct page pages[] =
{
    { IDS_TAB_PROCESSES,   NULL, processes_create, processes_refresh, processes_resize, processes_end_task, processes_can_end },
    { IDS_TAB_PERFORMANCE, NULL, perf_create,      perf_refresh,      perf_resize,      NULL, NULL },
    { IDS_TAB_HISTORY,     NULL, history_create,   history_refresh,   history_resize,   NULL, NULL },
    { IDS_TAB_STARTUP,     NULL, startup_create,   startup_refresh,   startup_resize,   startup_toggle, startup_can_end },
    { IDS_TAB_USERS,       NULL, users_create,     users_refresh,     users_resize,     NULL, NULL },
    { IDS_TAB_DETAILS,     NULL, details_create,   details_refresh,   details_resize,   details_end_task, details_can_end },
    { IDS_TAB_SERVICES,    NULL, services_create,  services_refresh,  services_resize,  NULL, NULL },
};
static HWND compact_page;

void set_page_hwnd( UINT title, HWND hwnd )
{
    for (UINT i = 0; i < ARRAY_SIZE(pages); i++) if (pages[i].title == title) pages[i].hwnd = hwnd;
}

void set_compact_hwnd( HWND hwnd ) { compact_page = hwnd; }

/* ------------------------------------------------------------------ helpers */

int scale( int px ) { return MulDiv( px, dpi, 96 ); }

const WCHAR *load_string( UINT id )
{
    static WCHAR buffers[16][256];
    static UINT next;
    WCHAR *buf = buffers[next++ % ARRAY_SIZE(buffers)];

    if (!LoadStringW( instance, id, buf, ARRAY_SIZE(buffers[0]) )) buf[0] = 0;
    return buf;
}

/* "12,3 МБ", "1,2 ГБ", "0 МБ/с": Windows' Task Manager's figures */
void format_bytes( WCHAR *buf, size_t len, double bytes, BOOL per_second )
{
    static const UINT units[] = { IDS_UNIT_B, IDS_UNIT_KB, IDS_UNIT_MB, IDS_UNIT_GB, IDS_UNIT_TB };
    UINT u = 0;
    WCHAR num[64];

    while (bytes >= 1024 && u < ARRAY_SIZE(units) - 1) { bytes /= 1024; u++; }
    if (u == 0) swprintf( num, ARRAY_SIZE(num), L"%.0f %s", bytes, load_string( units[u] ) );
    else if (bytes >= 100 || u == 1) swprintf( num, ARRAY_SIZE(num), L"%.0f %s", bytes, load_string( units[u] ) );
    else swprintf( num, ARRAY_SIZE(num), L"%.1f %s", bytes, load_string( units[u] ) );
    for (WCHAR *p = num; *p; p++) if (*p == '.') *p = ',';
    if (per_second) swprintf( buf, len, load_string( IDS_UNIT_PER_S ), num );
    else lstrcpynW( buf, num, len );
}

void format_percent( WCHAR *buf, size_t len, double pct )
{
    if (pct < 0.05) swprintf( buf, len, L"0%%" );
    else if (pct < 10) swprintf( buf, len, L"%.1f%%", pct );
    else swprintf( buf, len, L"%.0f%%", pct );
    for (WCHAR *p = buf; *p; p++) if (*p == '.') *p = ',';
}

static void create_fonts(void)
{
    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    LOGFONTW lf;

    SystemParametersInfoW( SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0 );
    lf = ncm.lfMessageFont;
    ui_font = CreateFontIndirectW( &lf );
    lf.lfWeight = FW_SEMIBOLD;
    ui_bold = CreateFontIndirectW( &lf );
    lf = ncm.lfMessageFont;
    lf.lfHeight = MulDiv( lf.lfHeight, 14, 10 );
    title_font = CreateFontIndirectW( &lf );
    lf = ncm.lfMessageFont;
    lf.lfHeight = MulDiv( lf.lfHeight, 25, 10 );
    lf.lfWeight = FW_LIGHT;
    big_font = CreateFontIndirectW( &lf );
    lf = ncm.lfMessageFont;
    lf.lfHeight = MulDiv( lf.lfHeight, 9, 10 );
    small_font = CreateFontIndirectW( &lf );
}

/* ------------------------------------------------------------------ settings */

static DWORD read_setting( const WCHAR *name, DWORD def )
{
    DWORD value = def, size = sizeof(value);
    RegGetValueW( HKEY_CURRENT_USER, SETTINGS_KEY, name, RRF_RT_REG_DWORD, NULL, &value, &size );
    return value;
}

static void write_setting( const WCHAR *name, DWORD value )
{
    RegSetKeyValueW( HKEY_CURRENT_USER, SETTINGS_KEY, name, REG_DWORD, &value, sizeof(value) );
}

static void save_window(void)
{
    WINDOWPLACEMENT wp = { sizeof(wp) };

    GetWindowPlacement( main_window, &wp );
    write_setting( L"Arctic.Compact", compact );
    write_setting( L"Arctic.Page", current_page );
    write_setting( L"Arctic.Speed", speed );
    write_setting( L"Arctic.OnTop", always_on_top );
    write_setting( L"Arctic.MinimizeOnUse", minimize_on_use );
    write_setting( L"Arctic.HideWhenMinimized", hide_when_minimized );
    write_setting( compact ? L"Arctic.CompactX" : L"Arctic.X", wp.rcNormalPosition.left );
    write_setting( compact ? L"Arctic.CompactY" : L"Arctic.Y", wp.rcNormalPosition.top );
    write_setting( compact ? L"Arctic.CompactW" : L"Arctic.W", wp.rcNormalPosition.right - wp.rcNormalPosition.left );
    write_setting( compact ? L"Arctic.CompactH" : L"Arctic.H", wp.rcNormalPosition.bottom - wp.rcNormalPosition.top );
    write_setting( L"Arctic.Maximized", wp.showCmd == SW_SHOWMAXIMIZED && !compact );
}

/* ------------------------------------------------------------------ layout */

static int bottom_height(void) { return scale( 48 ); }

static void layout(void)
{
    RECT rc;
    int bh = bottom_height(), w, h;

    GetClientRect( main_window, &rc );
    w = rc.right;
    h = rc.bottom;
    if (compact)
    {
        ShowWindow( tabs, SW_HIDE );
        for (UINT i = 0; i < ARRAY_SIZE(pages); i++) if (pages[i].hwnd) ShowWindow( pages[i].hwnd, SW_HIDE );
        ShowWindow( compact_page, SW_SHOW );
        SetWindowPos( compact_page, NULL, 0, 0, w, h - bh, SWP_NOZORDER );
        compact_resize( w, h - bh );
        ShowWindow( extra_link, SW_HIDE );
    }
    else
    {
        int tab_h;
        RECT tr = { 0, 0, w, 100 };

        ShowWindow( compact_page, SW_HIDE );
        ShowWindow( tabs, SW_SHOW );
        SendMessageW( tabs, TCM_ADJUSTRECT, FALSE, (LPARAM)&tr );
        tab_h = tr.top;
        SetWindowPos( tabs, NULL, 0, 0, w, tab_h, SWP_NOZORDER );
        for (UINT i = 0; i < ARRAY_SIZE(pages); i++)
        {
            if (!pages[i].hwnd) continue;
            if ((int)i == current_page)
            {
                SetWindowPos( pages[i].hwnd, NULL, 0, tab_h, w, h - tab_h - bh, SWP_NOZORDER | SWP_SHOWWINDOW );
                pages[i].resize( w, h - tab_h - bh );
            }
            else ShowWindow( pages[i].hwnd, SW_HIDE );
        }
        ShowWindow( extra_link, current_page == 6 ? SW_SHOW : SW_HIDE );
    }
    SetWindowPos( less_link, NULL, scale( 12 ), h - bh + scale( 12 ), scale( 200 ), scale( 24 ), SWP_NOZORDER );
    SetWindowPos( extra_link, NULL, scale( 220 ), h - bh + scale( 12 ), scale( 240 ), scale( 24 ), SWP_NOZORDER );
    SetWindowPos( end_button, NULL, w - scale( 12 + 112 ), h - bh + scale( 10 ), scale( 112 ), scale( 28 ), SWP_NOZORDER );
    InvalidateRect( main_window, NULL, TRUE );
}

void update_end_button(void)
{
    struct page *p = &pages[current_page];
    BOOL show, enable;
    UINT text = IDS_END_TASK;

    if (compact)
    {
        show = TRUE;
        enable = compact_can_end();
    }
    else
    {
        show = p->end_task != NULL || current_page == 4;
        enable = p->can_end ? p->can_end() : FALSE;
        if (current_page == 3) text = startup_can_toggle() == 2 ? IDS_ENABLE : IDS_DISABLE;
        if (current_page == 4) text = IDS_DISCONNECT;
    }
    SetWindowTextW( end_button, load_string( text ) );
    ShowWindow( end_button, show ? SW_SHOW : SW_HIDE );
    EnableWindow( end_button, enable );
}

static void set_compact( BOOL on )
{
    RECT rc;
    int x, y, w, h;

    save_window();
    compact = on;
    SetMenu( main_window, compact ? NULL : menu );
    SetWindowTextW( less_link, load_string( compact ? IDS_MORE_DETAILS : IDS_LESS_DETAILS ) );
    GetWindowRect( main_window, &rc );
    x = read_setting( compact ? L"Arctic.CompactX" : L"Arctic.X", rc.left );
    y = read_setting( compact ? L"Arctic.CompactY" : L"Arctic.Y", rc.top );
    w = read_setting( compact ? L"Arctic.CompactW" : L"Arctic.W", scale( compact ? 360 : 780 ) );
    h = read_setting( compact ? L"Arctic.CompactH" : L"Arctic.H", scale( compact ? 380 : 700 ) );
    ShowWindow( main_window, SW_RESTORE );
    SetWindowPos( main_window, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE );
    if (compact) compact_refresh();
    else if (pages[current_page].refresh) pages[current_page].refresh();
    layout();
    update_end_button();
}

static void select_page( int index )
{
    current_page = index;
    SendMessageW( tabs, TCM_SETCURSEL, index, 0 );
    if (!pages[index].hwnd)
    {
        pages[index].create( main_window );
    }
    pages[index].refresh();
    layout();
    update_end_button();
}

void show_details_of( DWORD pid )
{
    select_page( 5 );
    details_select( pid );
}

/* ------------------------------------------------------------------ the bottom links */

/* "Fewer details" and "Open Services": a link with its chevron, as Windows 10 draws them */
static LRESULT CALLBACK link_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint( hwnd, &ps );
        RECT rc;
        WCHAR text[128];
        BOOL hot = GetPropW( hwnd, L"hot" ) != NULL;
        BOOL chevron = GetWindowLongPtrW( hwnd, GWLP_ID ) == IDC_LESS_DETAILS;
        int x = 0, cy;

        GetClientRect( hwnd, &rc );
        FillRect( hdc, &rc, GetSysColorBrush( COLOR_BTNFACE ) );
        cy = (rc.top + rc.bottom) / 2;
        if (chevron)
        {
            /* a circled chevron, up for fewer, down for more */
            HPEN pen = CreatePen( PS_SOLID, 1, RGB(110, 110, 110) ), old = SelectObject( hdc, pen );
            HBRUSH oldb = SelectObject( hdc, GetStockObject( NULL_BRUSH ) );
            int r = scale( 8 ), up = !compact;
            POINT pts[3];

            Ellipse( hdc, 0, cy - r, 2 * r, cy + r );
            pts[0].x = r - scale( 4 ); pts[0].y = cy + (up ? scale( 2 ) : -scale( 2 ));
            pts[1].x = r;              pts[1].y = cy + (up ? -scale( 2 ) : scale( 2 ));
            pts[2].x = r + scale( 4 ); pts[2].y = pts[0].y;
            Polyline( hdc, pts, 3 );
            SelectObject( hdc, oldb );
            SelectObject( hdc, old );
            DeleteObject( pen );
            x = 2 * r + scale( 6 );
        }
        GetWindowTextW( hwnd, text, ARRAY_SIZE(text) );
        SelectObject( hdc, ui_font );
        SetBkMode( hdc, TRANSPARENT );
        SetTextColor( hdc, chevron ? (hot ? RGB(0, 102, 204) : RGB(0, 0, 0)) : (hot ? RGB(0, 102, 204) : RGB(0, 102, 204)) );
        rc.left = x;
        DrawTextW( hdc, text, -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX );
        EndPaint( hwnd, &ps );
        return 0;
    }
    case WM_SETCURSOR:
        SetCursor( LoadCursorW( NULL, (LPCWSTR)IDC_HAND ) );
        return TRUE;
    case WM_MOUSEMOVE:
        if (!GetPropW( hwnd, L"hot" ))
        {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd };
            SetPropW( hwnd, L"hot", (HANDLE)1 );
            TrackMouseEvent( &tme );
            InvalidateRect( hwnd, NULL, FALSE );
        }
        return 0;
    case WM_MOUSELEAVE:
        RemovePropW( hwnd, L"hot" );
        InvalidateRect( hwnd, NULL, FALSE );
        return 0;
    case WM_LBUTTONUP:
        SendMessageW( GetParent( hwnd ), WM_COMMAND, GetWindowLongPtrW( hwnd, GWLP_ID ), (LPARAM)hwnd );
        return 0;
    case WM_SETTEXT:
    {
        LRESULT ret = DefWindowProcW( hwnd, msg, wparam, lparam );
        InvalidateRect( hwnd, NULL, TRUE );
        return ret;
    }
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

/* ------------------------------------------------------------------ Run new task */

static INT_PTR CALLBACK run_proc( HWND dlg, UINT msg, WPARAM wparam, LPARAM lparam )
{
    switch (msg)
    {
    case WM_INITDIALOG:
        SendDlgItemMessageW( dlg, IDC_RUN_EDIT, EM_LIMITTEXT, MAX_PATH, 0 );
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wparam))
        {
        case IDOK:
        {
            WCHAR cmd[MAX_PATH + 1], *args;
            SHELLEXECUTEINFOW sei = { sizeof(sei) };

            GetDlgItemTextW( dlg, IDC_RUN_EDIT, cmd, ARRAY_SIZE(cmd) );
            args = PathGetArgsW( cmd );
            if (args > cmd && args[-1] == ' ') args[-1] = 0;
            PathUnquoteSpacesW( cmd );
            sei.fMask = SEE_MASK_FLAG_NO_UI;
            sei.lpVerb = IsDlgButtonChecked( dlg, IDC_RUN_ADMIN ) ? L"runas" : NULL;
            sei.lpFile = cmd;
            sei.lpParameters = args && *args ? args : NULL;
            sei.nShow = SW_SHOWNORMAL;
            if (!ShellExecuteExW( &sei ))
            {
                WCHAR msgbuf[512];
                FormatMessageW( FORMAT_MESSAGE_FROM_SYSTEM, NULL, GetLastError(), 0, msgbuf, ARRAY_SIZE(msgbuf), NULL );
                MessageBoxW( dlg, msgbuf, load_string( IDS_RUN_TITLE ), MB_ICONERROR );
                return TRUE;
            }
            EndDialog( dlg, IDOK );
            return TRUE;
        }
        case IDC_RUN_BROWSE:
        {
            WCHAR file[MAX_PATH] = L"";
            OPENFILENAMEW ofn = { sizeof(ofn) };
            ofn.hwndOwner = dlg;
            ofn.lpstrFile = file;
            ofn.nMaxFile = ARRAY_SIZE(file);
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
            if (GetOpenFileNameW( &ofn )) SetDlgItemTextW( dlg, IDC_RUN_EDIT, file );
            return TRUE;
        }
        case IDCANCEL:
            EndDialog( dlg, IDCANCEL );
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ the window */

static void set_speed( UINT id )
{
    static const UINT ms[] = { 500, 1000, 4000, 0 };
    UINT interval = ms[id - IDM_SPEED_HIGH];

    speed = id;
    CheckMenuRadioItem( menu, IDM_SPEED_HIGH, IDM_SPEED_PAUSED, id, MF_BYCOMMAND );
    KillTimer( main_window, TIMER_REFRESH );
    if (interval) SetTimer( main_window, TIMER_REFRESH, interval, NULL );
}

static void refresh(void)
{
    data_sample();
    history_track();
    if (compact) compact_refresh();
    else if (pages[current_page].hwnd) pages[current_page].refresh();
    update_end_button();
}

static void apply_options(void)
{
    SetWindowPos( main_window, always_on_top ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                  SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE );
    CheckMenuItem( menu, IDM_ALWAYS_ON_TOP, always_on_top ? MF_CHECKED : MF_UNCHECKED );
    CheckMenuItem( menu, IDM_MINIMIZE_ON_USE, minimize_on_use ? MF_CHECKED : MF_UNCHECKED );
    CheckMenuItem( menu, IDM_HIDE_WHEN_MIN, hide_when_minimized ? MF_CHECKED : MF_UNCHECKED );
}

static LRESULT CALLBACK main_proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    switch (msg)
    {
    case WM_CREATE:
    {
        TCITEMW item = { TCIF_TEXT };

        main_window = hwnd;
        tabs = CreateWindowExW( 0, WC_TABCONTROLW, NULL, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_FOCUSNEVER,
                                0, 0, 0, 0, hwnd, (HMENU)IDC_TABS, instance, NULL );
        SendMessageW( tabs, WM_SETFONT, (WPARAM)ui_font, FALSE );
        for (UINT i = 0; i < ARRAY_SIZE(pages); i++)
        {
            item.pszText = (WCHAR *)load_string( pages[i].title );
            SendMessageW( tabs, TCM_INSERTITEMW, i, (LPARAM)&item );
        }
        end_button = CreateWindowExW( 0, L"BUTTON", load_string( IDS_END_TASK ), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                      0, 0, 0, 0, hwnd, (HMENU)IDC_END_TASK, instance, NULL );
        SendMessageW( end_button, WM_SETFONT, (WPARAM)ui_font, FALSE );
        less_link = CreateWindowExW( 0, L"ArcticTaskLink", load_string( IDS_LESS_DETAILS ), WS_CHILD | WS_VISIBLE,
                                     0, 0, 0, 0, hwnd, (HMENU)IDC_LESS_DETAILS, instance, NULL );
        extra_link = CreateWindowExW( 0, L"ArcticTaskLink", load_string( IDS_OPEN_SERVICES ), WS_CHILD,
                                      0, 0, 0, 0, hwnd, (HMENU)IDC_OPEN_SERVICES, instance, NULL );
        compact_create( hwnd );
        return 0;
    }
    case WM_SIZE:
        if (wparam == SIZE_MINIMIZED && hide_when_minimized) ShowWindow( hwnd, SW_HIDE );
        layout();
        return 0;
    case WM_GETMINMAXINFO:
    {
        MINMAXINFO *mmi = (MINMAXINFO *)lparam;
        mmi->ptMinTrackSize.x = scale( compact ? 280 : 520 );
        mmi->ptMinTrackSize.y = scale( compact ? 240 : 420 );
        return 0;
    }
    case WM_TIMER:
        if (wparam == TIMER_REFRESH && !IsIconic( hwnd )) refresh();
        return 0;
    case WM_NOTIFY:
    {
        NMHDR *nm = (NMHDR *)lparam;
        if (nm->idFrom == IDC_TABS && nm->code == TCN_SELCHANGE) select_page( (int)SendMessageW( tabs, TCM_GETCURSEL, 0, 0 ) );
        break;
    }
    case WM_COMMAND:
        switch (LOWORD(wparam))
        {
        case IDC_END_TASK:
            if (compact) compact_end_task();
            else if (pages[current_page].end_task) pages[current_page].end_task();
            update_end_button();
            return 0;
        case IDC_LESS_DETAILS:
            set_compact( !compact );
            return 0;
        case IDC_OPEN_SERVICES:
        case IDM_OPEN_SERVICES:
            ShellExecuteW( hwnd, NULL, L"services.msc", NULL, NULL, SW_SHOWNORMAL );
            return 0;
        case IDM_RUN:
            DialogBoxW( instance, MAKEINTRESOURCEW(IDD_RUN), hwnd, run_proc );
            return 0;
        case IDM_EXIT:
            DestroyWindow( hwnd );
            return 0;
        case IDM_ALWAYS_ON_TOP: always_on_top = !always_on_top; apply_options(); return 0;
        case IDM_MINIMIZE_ON_USE: minimize_on_use = !minimize_on_use; apply_options(); return 0;
        case IDM_HIDE_WHEN_MIN: hide_when_minimized = !hide_when_minimized; apply_options(); return 0;
        case IDM_REFRESH: refresh(); return 0;
        case IDM_SPEED_HIGH: case IDM_SPEED_NORMAL: case IDM_SPEED_LOW: case IDM_SPEED_PAUSED:
            set_speed( LOWORD(wparam) );
            return 0;
        default:
            /* the commands of the pages' own menus go to the page in front */
            if (!compact && pages[current_page].hwnd)
                SendMessageW( pages[current_page].hwnd, WM_COMMAND, wparam, lparam );
            else if (compact)
                SendMessageW( compact_page, WM_COMMAND, wparam, lparam );
            return 0;
        }
    case WM_ACTIVATEAPP:
        /* "Minimize on use": switching to a program hides Task Manager */
        return 0;
    case WM_DPICHANGED:
        return 0;
    case WM_CLOSE:
        save_window();
        DestroyWindow( hwnd );
        return 0;
    case WM_DESTROY:
        PostQuitMessage( 0 );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

void minimize_after_use(void)
{
    if (minimize_on_use) ShowWindow( main_window, SW_MINIMIZE );
}

int WINAPI wWinMain( HINSTANCE inst, HINSTANCE prev, WCHAR *cmdline, int show )
{
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    WNDCLASSEXW wc = { sizeof(wc) };
    HACCEL accel;
    HWND existing;
    HDC hdc;
    MSG m;
    int x, y, w, h;

    /* one Task Manager, as Windows has: Ctrl+Shift+Esc again brings it forward */
    if ((existing = FindWindowW( CLASS_NAME, NULL )))
    {
        if (IsIconic( existing ) || !IsWindowVisible( existing )) ShowWindow( existing, SW_RESTORE );
        SetForegroundWindow( existing );
        return 0;
    }

    instance = inst;
    InitCommonControlsEx( &icc );
    hdc = GetDC( NULL );
    dpi = GetDeviceCaps( hdc, LOGPIXELSY );
    ReleaseDC( NULL, hdc );
    create_fonts();
    data_init();
    data_sample();
    grid_register( inst );

    wc.lpfnWndProc = link_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW( NULL, (LPCWSTR)IDC_HAND );
    wc.lpszClassName = L"ArcticTaskLink";
    RegisterClassExW( &wc );

    memset( &wc, 0, sizeof(wc) );
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = main_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW( inst, MAKEINTRESOURCEW(IDI_TASKMGR) );
    wc.hIconSm = LoadImageW( inst, MAKEINTRESOURCEW(IDI_TASKMGR), IMAGE_ICON, GetSystemMetrics( SM_CXSMICON ),
                             GetSystemMetrics( SM_CYSMICON ), 0 );
    wc.hCursor = LoadCursorW( NULL, (LPCWSTR)IDC_ARROW );
    wc.hbrBackground = GetSysColorBrush( COLOR_BTNFACE );
    wc.lpszClassName = CLASS_NAME;
    RegisterClassExW( &wc );

    menu = LoadMenuW( inst, MAKEINTRESOURCEW(IDR_MENU) );
    compact = read_setting( L"Arctic.Compact", TRUE );
    current_page = min( read_setting( L"Arctic.Page", 0 ), ARRAY_SIZE(pages) - 1 );
    speed = read_setting( L"Arctic.Speed", IDM_SPEED_NORMAL );
    if (speed < IDM_SPEED_HIGH || speed > IDM_SPEED_PAUSED) speed = IDM_SPEED_NORMAL;
    always_on_top = read_setting( L"Arctic.OnTop", FALSE );
    minimize_on_use = read_setting( L"Arctic.MinimizeOnUse", TRUE );
    hide_when_minimized = read_setting( L"Arctic.HideWhenMinimized", FALSE );

    w = read_setting( compact ? L"Arctic.CompactW" : L"Arctic.W", scale( compact ? 360 : 780 ) );
    h = read_setting( compact ? L"Arctic.CompactH" : L"Arctic.H", scale( compact ? 380 : 700 ) );
    x = read_setting( compact ? L"Arctic.CompactX" : L"Arctic.X", CW_USEDEFAULT );
    y = read_setting( compact ? L"Arctic.CompactY" : L"Arctic.Y", CW_USEDEFAULT );
    CreateWindowExW( 0, CLASS_NAME, load_string( IDS_APP_TITLE ), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                     x, y, w, h, NULL, compact ? NULL : menu, inst, NULL );
    if (!main_window) return 1;

    SetWindowTextW( less_link, load_string( compact ? IDS_MORE_DETAILS : IDS_LESS_DETAILS ) );
    if (!compact) select_page( current_page );
    else compact_refresh();
    set_speed( speed );
    apply_options();
    layout();
    update_end_button();
    ShowWindow( main_window, read_setting( L"Arctic.Maximized", FALSE ) && !compact ? SW_SHOWMAXIMIZED : show );
    UpdateWindow( main_window );

    accel = LoadAcceleratorsW( inst, MAKEINTRESOURCEW(IDR_ACCEL) );
    while (GetMessageW( &m, NULL, 0, 0 ))
    {
        if (TranslateAcceleratorW( main_window, accel, &m )) continue;
        if (IsDialogMessageW( main_window, &m )) continue;
        TranslateMessage( &m );
        DispatchMessageW( &m );
    }
    return m.wParam;
}
