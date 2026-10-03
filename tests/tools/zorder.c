/* The visible toplevel windows from the top down, with their class, title,
 * rectangle and process, in a window of its own (topmost, refreshed every
 * second), so a screenshot of the VM shows what lies over what.
 * zig cc -target x86_64-windows-gnu -Os -s -Wl,--subsystem,windows zorder.c -o zorder.exe -luser32 -lgdi32
 */
#include <windows.h>
#include <stdio.h>

static WCHAR text[16384];
static int len;
static HWND self;

static BOOL CALLBACK add_window( HWND hwnd, LPARAM lparam )
{
    WCHAR cls[64], title[64];
    RECT rc;
    DWORD pid;

    if (hwnd == self || len > ARRAYSIZE(text) - 300) return TRUE;
    GetClassNameW( hwnd, cls, ARRAYSIZE(cls) );
    GetWindowTextW( hwnd, title, ARRAYSIZE(title) );
    GetWindowRect( hwnd, &rc );
    GetWindowThreadProcessId( hwnd, &pid );
    len += _snwprintf( text + len, ARRAYSIZE(text) - len, L"%p %-24ls %-22.22ls %5ld,%5ld-%5ld,%5ld pid %lu %08lx %08lx%ls\n",
                       hwnd, cls, title, rc.left, rc.top, rc.right, rc.bottom, pid,
                       GetWindowLongW( hwnd, GWL_STYLE ), GetWindowLongW( hwnd, GWL_EXSTYLE ),
                       hwnd == GetShellWindow() ? L"  <shell>" : L"" );
    return TRUE;
}

static LRESULT CALLBACK proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    switch (msg)
    {
    case WM_TIMER:
        len = 0;
        EnumWindows( add_window, 0 );
        InvalidateRect( hwnd, NULL, TRUE );
        return 0;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        RECT rc;
        HDC hdc = BeginPaint( hwnd, &ps );
        HFONT font = CreateFontW( -12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, FIXED_PITCH, L"Courier New" );
        HGDIOBJ old = SelectObject( hdc, font );

        GetClientRect( hwnd, &rc );
        InflateRect( &rc, -6, -6 );
        DrawTextW( hdc, text, len, &rc, DT_LEFT );
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
    MSG msg;

    SetProcessDpiAwarenessContext( DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 );
    if (cmdline && *cmdline)  /* "zorder FILE": the list into a file, once */
    {
        FILE *f = fopen( cmdline, "w" );
        if (!f) return 1;
        EnumWindows( add_window, 0 );
        fprintf( f, "%ls", text );
        fclose( f );
        return 0;
    }
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_ARROW );
    wc.hbrBackground = GetStockObject( WHITE_BRUSH );
    wc.lpszClassName = L"zorder";
    RegisterClassW( &wc );
    self = CreateWindowExW( WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"zorder", L"zorder", WS_POPUP | WS_BORDER | WS_VISIBLE,
                            1000, 420, 900, 400, NULL, NULL, instance, NULL );
    SetTimer( self, 1, 1000, NULL );
    PostMessageW( self, WM_TIMER, 1, 0 );
    while (GetMessageW( &msg, NULL, 0, 0 ))
    {
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }
    return 0;
}
