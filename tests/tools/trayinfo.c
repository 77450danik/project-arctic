/* Shows the taskbar's windows with their rectangles (screen and client),
 * the toolbar button sizes and the theme's sizing bar, in a window of its
 * own, so a screenshot of the VM has the numbers.
 * zig cc -target x86_64-windows-gnu -Os -s -Wl,--subsystem,windows trayinfo.c -o trayinfo.exe -luxtheme -lgdi32 -luser32
 */
#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <stdio.h>

static WCHAR text[8192];
static int len;

static void add( const WCHAR *fmt, ... )
{
    va_list args;
    va_start( args, fmt );
    len += _vsnwprintf( text + len, ARRAYSIZE(text) - len - 1, fmt, args );
    va_end( args );
}

static void show( HWND hwnd, int depth )
{
    WCHAR cls[64];
    RECT rc, client;
    HWND child;

    GetClassNameW( hwnd, cls, ARRAYSIZE(cls) );
    GetWindowRect( hwnd, &rc );
    GetClientRect( hwnd, &client );
    add( L"%*s%ls  %ld,%ld-%ld,%ld (%ldx%ld)  client %ldx%ld%ls",
         depth * 4, L"", cls, rc.left, rc.top, rc.right, rc.bottom, rc.right - rc.left,
         rc.bottom - rc.top, client.right, client.bottom, IsWindowVisible( hwnd ) ? L"" : L"  hidden" );
    if (!wcscmp( cls, L"ToolbarWindow32" ))
    {
        DWORD size = SendMessageW( hwnd, TB_GETBUTTONSIZE, 0, 0 );
        add( L"  buttons %ux%u, %lu", LOWORD(size), HIWORD(size), (DWORD)SendMessageW( hwnd, TB_BUTTONCOUNT, 0, 0 ) );
    }
    if (!wcscmp( cls, L"ReBarWindow32" ))
    {
        UINT i, count = SendMessageW( hwnd, RB_GETBANDCOUNT, 0, 0 );
        add( L"  bands %u", count );
        for (i = 0; i < count; i++)
        {
            REBARBANDINFOW band = { sizeof(band), RBBIM_CHILDSIZE | RBBIM_SIZE };
            SendMessageW( hwnd, RB_GETBANDINFOW, i, (LPARAM)&band );
            add( L"  [min %ux%u integral %u]", band.cxMinChild, band.cyMinChild, band.cyIntegral );
        }
    }
    add( L"\n" );
    for (child = GetWindow( hwnd, GW_CHILD ); child; child = GetWindow( child, GW_HWNDNEXT ))
        show( child, depth + 1 );
}

static void collect( HWND self )
{
    HWND tray = FindWindowW( L"Shell_TrayWnd", NULL );
    HTHEME theme;
    RECT work;
    SIZE size;
    int i;

    len = 0;
    SystemParametersInfoW( SPI_GETWORKAREA, 0, &work, 0 );
    add( L"screen %dx%d  work %ld,%ld-%ld,%ld  dpi %u\n", GetSystemMetrics( SM_CXSCREEN ),
         GetSystemMetrics( SM_CYSCREEN ), work.left, work.top, work.right, work.bottom, GetDpiForWindow( self ) );
    add( L"SM_CYEDGE %d  SM_CYDLGFRAME %d  SM_CYSIZEFRAME %d  SM_CYFRAME %d  SM_CYCAPTION %d\n",
         GetSystemMetrics( SM_CYEDGE ), GetSystemMetrics( SM_CYDLGFRAME ), GetSystemMetrics( SM_CYSIZEFRAME ),
         GetSystemMetrics( SM_CYFRAME ), GetSystemMetrics( SM_CYCAPTION ) );
    if ((theme = OpenThemeData( self, L"TaskBar" )))
    {
        add( L"TaskBar theme: sizing bars" );
        for (i = 1; i <= 4; i++)  /* TBP_BACKGROUNDBOTTOM .. are 1-4, sizing bars 5-8 */
            if (SUCCEEDED(GetThemePartSize( theme, NULL, i + 4, 0, NULL, TS_TRUE, &size )))
                add( L"  %d: %ldx%ld", i + 4, size.cx, size.cy );
        add( L"\n" );
        CloseThemeData( theme );
    }
    else add( L"no TaskBar theme\n" );
    if (tray) show( tray, 0 );
    else add( L"no Shell_TrayWnd\n" );
}

static LRESULT CALLBACK proc( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    switch (msg)
    {
    case WM_CREATE:
        collect( hwnd );
        SetTimer( hwnd, 1, 2000, NULL );
        return 0;
    case WM_TIMER:
        collect( hwnd );
        InvalidateRect( hwnd, NULL, TRUE );
        return 0;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        RECT rc;
        HDC hdc = BeginPaint( hwnd, &ps );
        HFONT font = CreateFontW( -13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0,
                                  FIXED_PITCH, L"Courier New" );
        HGDIOBJ old = SelectObject( hdc, font );
        GetClientRect( hwnd, &rc );
        InflateRect( &rc, -8, -8 );
        DrawTextW( hdc, text, len, &rc, DT_LEFT | DT_EXPANDTABS );
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

int WINAPI wWinMain( HINSTANCE instance, HINSTANCE prev, WCHAR *cmdline, int show_cmd )
{
    WNDCLASSW wc = { 0 };
    MSG msg;

    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW( NULL, (const WCHAR *)IDC_ARROW );
    wc.hbrBackground = GetStockObject( WHITE_BRUSH );
    wc.lpszClassName = L"trayinfo";
    RegisterClassW( &wc );
    CreateWindowW( L"trayinfo", L"trayinfo", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                   10, 10, 1180, 560, NULL, NULL, instance, NULL );
    while (GetMessageW( &msg, NULL, 0, 0 ))
    {
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }
    return 0;
}
