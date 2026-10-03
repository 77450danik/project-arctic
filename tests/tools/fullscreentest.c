/* Goes full screen the way Chrome does from a maximized window: the window
 * is maximized, then drops its caption and frame and covers the monitor,
 * keeping WS_MAXIMIZE, without being activated again. The taskbar must go
 * then (after 4 s) and come back when the window is maximized again (after
 * 8 s). Its client area says which phase it is in.
 * zig cc -target x86_64-windows-gnu -Os -s -Wl,--subsystem,windows fullscreentest.c -o fullscreentest.exe -lgdi32 -luser32
 */
#include <windows.h>

static int phase;
static LONG saved_style;
static RECT saved_rect;

static LRESULT CALLBACK proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    static const char *names[] = { "maximized", "full screen (no taskbar)", "maximized again" };

    switch (msg)
    {
    case WM_TIMER:
        if (phase == 0)
        {
            MONITORINFO info = { sizeof(info) };

            GetMonitorInfoW( MonitorFromWindow( hwnd, MONITOR_DEFAULTTONEAREST ), &info );
            saved_style = GetWindowLongW( hwnd, GWL_STYLE );
            GetWindowRect( hwnd, &saved_rect );
            SetWindowLongW( hwnd, GWL_STYLE, saved_style & ~(WS_CAPTION | WS_THICKFRAME) );
            SetWindowPos( hwnd, NULL, info.rcMonitor.left, info.rcMonitor.top,
                          info.rcMonitor.right - info.rcMonitor.left, info.rcMonitor.bottom - info.rcMonitor.top,
                          SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED );
        }
        else if (phase == 1)
        {
            /* back as Chrome comes back: the style and the rectangle it had */
            SetWindowLongW( hwnd, GWL_STYLE, saved_style );
            SetWindowPos( hwnd, NULL, saved_rect.left, saved_rect.top, saved_rect.right - saved_rect.left,
                          saved_rect.bottom - saved_rect.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED );
            KillTimer( hwnd, 1 );
        }
        phase++;
        InvalidateRect( hwnd, NULL, TRUE );
        return 0;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        RECT rc;
        HDC hdc = BeginPaint( hwnd, &ps );
        HFONT font = CreateFontW( -40, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Arial" );
        HGDIOBJ old = SelectObject( hdc, font );

        GetClientRect( hwnd, &rc );
        FillRect( hdc, &rc, GetStockObject( phase == 1 ? BLACK_BRUSH : WHITE_BRUSH ) );
        SetBkMode( hdc, TRANSPARENT );
        SetTextColor( hdc, phase == 1 ? RGB(255, 255, 255) : RGB(0, 0, 0) );
        DrawTextA( hdc, names[phase], -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE );
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

    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_ARROW );
    wc.lpszClassName = L"fullscreentest";
    RegisterClassW( &wc );
    hwnd = CreateWindowW( L"fullscreentest", L"fullscreentest", WS_OVERLAPPEDWINDOW,
                          100, 100, 640, 400, NULL, NULL, instance, NULL );
    ShowWindow( hwnd, SW_MAXIMIZE );
    SetTimer( hwnd, 1, 4000, NULL );
    while (GetMessageW( &msg, NULL, 0, 0 ))
    {
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }
    return 0;
}
