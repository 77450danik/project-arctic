/* The scale of the primary monitor, as the Display panel of Windows 10 reads
 * and sets it (DisplayConfigGet/SetDeviceInfo with the undocumented
 * GET/SET_SOURCE_DPI_SCALE). "scaletest 150" sets 150 % and quits; without
 * an argument it stays open as a per-monitor (v2) window that shows its DPI,
 * the system's, the monitor's range of scales and how many WM_DPICHANGED it
 * got, taking the rectangle each one suggests, its text drawn at its DPI.
 * zig cc -target x86_64-windows-gnu -Os -s -Wl,--subsystem,windows scaletest.c -o scaletest.exe -luser32 -lgdi32
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static const int scales[] = { 100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500 };

struct scale_get
{
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    INT32 min_rel, cur_rel, max_rel;
};

struct scale_set
{
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    INT32 rel;
};

static int changes;

static BOOL primary_source( LUID *adapter, UINT32 *id )
{
    DISPLAYCONFIG_PATH_INFO paths[16];
    DISPLAYCONFIG_MODE_INFO modes[32];
    UINT32 path_count = 16, mode_count = 32;

    if (QueryDisplayConfig( QDC_ONLY_ACTIVE_PATHS, &path_count, paths, &mode_count, modes, NULL )) return FALSE;
    for (UINT32 i = 0; i < path_count; i++)
    {
        UINT32 m = paths[i].sourceInfo.modeInfoIdx;
        if (m < mode_count && modes[m].sourceMode.position.x == 0 && modes[m].sourceMode.position.y == 0)
        {
            *adapter = paths[i].sourceInfo.adapterId;
            *id = paths[i].sourceInfo.id;
            return TRUE;
        }
    }
    if (!path_count) return FALSE;
    *adapter = paths[0].sourceInfo.adapterId;
    *id = paths[0].sourceInfo.id;
    return TRUE;
}

static LONG get_scale( struct scale_get *get )
{
    memset( get, 0, sizeof(*get) );
    get->header.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE)-3;
    get->header.size = sizeof(*get);
    if (!primary_source( &get->header.adapterId, &get->header.id )) return ERROR_NOT_FOUND;
    return DisplayConfigGetDeviceInfo( &get->header );
}

static LRESULT CALLBACK proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    switch (msg)
    {
    case WM_DPICHANGED:
    {
        const RECT *rc = (const RECT *)lparam;
        changes++;
        SetWindowPos( hwnd, NULL, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top,
                      SWP_NOZORDER | SWP_NOACTIVATE );
        InvalidateRect( hwnd, NULL, TRUE );
        return 0;
    }
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        struct scale_get get;
        char text[512];
        RECT rc;
        UINT dpi = GetDpiForWindow( hwnd );
        HDC hdc = BeginPaint( hwnd, &ps );
        HFONT font = CreateFontW( -MulDiv( 11, dpi, 72 ), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                  CLEARTYPE_QUALITY, 0, L"Segoe UI" );
        HGDIOBJ old = SelectObject( hdc, font );
        LONG err = get_scale( &get );
        int rec = -get.min_rel;

        if (err)
            snprintf( text, sizeof(text), "window %u dpi, system %u dpi\nGET_SOURCE_DPI_SCALE failed: %ld\n"
                      "WM_DPICHANGED: %d", dpi, GetDpiForSystem(), err, changes );
        else
            snprintf( text, sizeof(text), "window %u dpi, system %u dpi\nmonitor %d%% (recommended %d%%, %d..%d%%)\n"
                      "WM_DPICHANGED: %d", dpi, GetDpiForSystem(), scales[rec + get.cur_rel], scales[rec],
                      scales[0], scales[rec + get.max_rel], changes );
        GetClientRect( hwnd, &rc );
        InflateRect( &rc, -MulDiv( 8, dpi, 96 ), -MulDiv( 8, dpi, 96 ) );
        DrawTextA( hdc, text, -1, &rc, DT_LEFT );
        SelectObject( hdc, old );
        DeleteObject( font );
        EndPaint( hwnd, &ps );
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage( 0 );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wparam, lparam );
}

int WINAPI WinMain( HINSTANCE instance, HINSTANCE prev, LPSTR cmdline, int show )
{
    WNDCLASSW wc = { 0 };
    HWND hwnd;
    MSG msg;
    UINT dpi;

    SetProcessDpiAwarenessContext( DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 );
    if (cmdline && *cmdline)
    {
        struct scale_get get;
        struct scale_set set = { { 0 } };
        int want = atoi( cmdline ), index = 0;

        for (int i = 0; i < (int)(sizeof(scales) / sizeof(scales[0])); i++) if (scales[i] <= want) index = i;
        if (get_scale( &get )) return 1;
        set.header.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE)-4;
        set.header.size = sizeof(set);
        set.header.adapterId = get.header.adapterId;
        set.header.id = get.header.id;
        set.rel = index + get.min_rel;  /* the steps from the recommended one */
        return DisplayConfigSetDeviceInfo( &set.header );
    }

    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_ARROW );
    wc.hbrBackground = GetStockObject( WHITE_BRUSH );
    wc.lpszClassName = L"scaletest";
    RegisterClassW( &wc );
    hwnd = CreateWindowW( L"scaletest", L"scaletest", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, instance, NULL );
    dpi = GetDpiForWindow( hwnd );
    SetWindowPos( hwnd, NULL, MulDiv( 560, dpi, 96 ), MulDiv( 20, dpi, 96 ), MulDiv( 400, dpi, 96 ),
                  MulDiv( 140, dpi, 96 ), SWP_NOZORDER | SWP_NOACTIVATE );
    ShowWindow( hwnd, SW_SHOWNOACTIVATE );
    while (GetMessageW( &msg, NULL, 0, 0 ))
    {
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }
    return 0;
}
