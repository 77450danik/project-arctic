/* modewin: does a window come back where it was after a game changed the
 * display mode while it was minimized? (2026-11-23: the IDE, maximized and
 * then minimized while CS2 ran at 1280x960, restored to somewhere off the
 * screen). Writes each step's placement to OUT (default D:\nt\modewin.txt).
 *   modewin [OUT]     the window, and a "game" child that takes the mode
 *   modewin game      the child: 1280x960 fullscreen for 3 s, as a game does */
#include <windows.h>
#include <stdio.h>

static FILE *out;

static void step( HWND hwnd, const char *what )
{
    WINDOWPLACEMENT wp = { .length = sizeof(wp) };
    RECT r;
    HMONITOR mon = MonitorFromWindow( hwnd, MONITOR_DEFAULTTONULL );
    MONITORINFO mi = { .cbSize = sizeof(mi) };

    GetWindowRect( hwnd, &r );
    GetWindowPlacement( hwnd, &wp );
    if (mon) GetMonitorInfoW( mon, &mi );
    fprintf( out, "%-34s rect %d,%d-%d,%d  show %u flags %#x normal %ld,%ld-%ld,%ld  %s%s monitor %ld,%ld-%ld,%ld screen %dx%d\n",
             what, (int)r.left, (int)r.top, (int)r.right, (int)r.bottom, wp.showCmd, wp.flags,
             wp.rcNormalPosition.left, wp.rcNormalPosition.top, wp.rcNormalPosition.right, wp.rcNormalPosition.bottom,
             IsIconic( hwnd ) ? "iconic " : IsZoomed( hwnd ) ? "zoomed " : "", mon ? "on a" : "on NO",
             mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom,
             GetSystemMetrics( SM_CXSCREEN ), GetSystemMetrics( SM_CYSCREEN ) );
    fflush( out );
}

static void pump( DWORD ms )
{
    DWORD end = GetTickCount() + ms;
    MSG msg;

    while ((int)(end - GetTickCount()) > 0)
    {
        while (PeekMessageW( &msg, 0, 0, 0, PM_REMOVE ))
        {
            TranslateMessage( &msg );
            DispatchMessageW( &msg );
        }
        Sleep( 10 );
    }
}

static int game(void)
{
    DEVMODEW mode = { .dmSize = sizeof(mode) }, pick = { .dmSize = sizeof(pick) };
    HWND hwnd = CreateWindowExW( WS_EX_TOPMOST, L"STATIC", L"modewin game", WS_POPUP | WS_VISIBLE,
                                 0, 0, 1280, 960, 0, 0, 0, 0 );
    LONG ret;
    FILE *log = fopen( "D:\\nt\\modewin-game.txt", "a" );

    /* 1280x960 as CS2 asks, else the mode nearest below it the card has */
    for (DWORD i = 0; EnumDisplaySettingsW( NULL, i, &mode ); i++)
    {
        if (mode.dmPelsWidth > 1280 || mode.dmPelsHeight > 1024) continue;
        if (!pick.dmPelsWidth || mode.dmPelsWidth * mode.dmPelsHeight > pick.dmPelsWidth * pick.dmPelsHeight)
            pick = mode;
    }
    pick.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
    SetForegroundWindow( hwnd );
    ret = ChangeDisplaySettingsW( &pick, CDS_FULLSCREEN );
    if (log)
    {
        fprintf( log, "game: %lux%lu -> %ld, screen now %dx%d\n", pick.dmPelsWidth, pick.dmPelsHeight, ret,
                 GetSystemMetrics( SM_CXSCREEN ), GetSystemMetrics( SM_CYSCREEN ) );
        fclose( log );
    }
    SetWindowPos( hwnd, HWND_TOPMOST, 0, 0, GetSystemMetrics( SM_CXSCREEN ), GetSystemMetrics( SM_CYSCREEN ), 0 );
    pump( 3000 );
    ChangeDisplaySettingsW( NULL, 0 );
    DestroyWindow( hwnd );
    return ret;
}

static HWND make_window(void)
{
    HWND hwnd = CreateWindowExW( 0, L"STATIC", L"modewin", WS_OVERLAPPEDWINDOW, 200, 150, 900, 600, 0, 0, 0, 0 );
    ShowWindow( hwnd, SW_SHOWMAXIMIZED );
    pump( 500 );
    return hwnd;
}

static HWND game_hook;  /* E: set its placement again while the game's mode is on */

static void run_game( const char *self )
{
    STARTUPINFOA si = { .cb = sizeof(si) };
    PROCESS_INFORMATION pi;
    char cmd[MAX_PATH + 16];
    DWORD start = GetTickCount();
    BOOL hooked = FALSE;

    snprintf( cmd, sizeof(cmd), "\"%s\" game", self );
    if (!CreateProcessA( NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi ))
    {
        fprintf( out, "no game: %lu\n", GetLastError() );
        return;
    }
    while (WaitForSingleObject( pi.hProcess, 50 ) == WAIT_TIMEOUT)
    {
        pump( 50 );
        if (game_hook && !hooked && GetTickCount() - start > 1500)
        {
            WINDOWPLACEMENT wp = { .length = sizeof(wp) };

            hooked = TRUE;
            step( game_hook, "E during the game, before" );
            GetWindowPlacement( game_hook, &wp );
            wp.showCmd = SW_SHOWMAXIMIZED;
            SetWindowPlacement( game_hook, &wp );
            step( game_hook, "E during the game, placed" );
        }
    }
    game_hook = NULL;
    CloseHandle( pi.hProcess );
    CloseHandle( pi.hThread );
    pump( 1500 );
}

int main( int argc, char **argv )
{
    HWND hwnd;
    WINDOWPLACEMENT wp = { .length = sizeof(wp) };
    char self[MAX_PATH];

    if (argc > 1 && !strcmp( argv[1], "game" )) return game();
    if (!(out = fopen( argc > 1 ? argv[1] : "D:\\nt\\modewin.txt", "w" ))) return 1;
    GetModuleFileNameA( NULL, self, sizeof(self) );

    /* the taskbar's way back: SW_RESTORE */
    hwnd = make_window();
    step( hwnd, "A maximized" );
    ShowWindow( hwnd, SW_MINIMIZE );
    pump( 500 );
    step( hwnd, "A minimized" );
    run_game( self );
    step( hwnd, "A after the game" );
    ShowWindow( hwnd, SW_RESTORE );
    pump( 500 );
    step( hwnd, "A restored (SW_RESTORE)" );
    DestroyWindow( hwnd );

    /* Chromium's way back: its placement again, shown normal */
    hwnd = make_window();
    ShowWindow( hwnd, SW_MINIMIZE );
    pump( 500 );
    run_game( self );
    step( hwnd, "B after the game" );
    GetWindowPlacement( hwnd, &wp );
    wp.showCmd = SW_SHOWNORMAL;
    SetWindowPlacement( hwnd, &wp );
    pump( 500 );
    step( hwnd, "B restored (placement, SHOWNORMAL)" );
    DestroyWindow( hwnd );

    /* a normal (not maximized) window too */
    hwnd = CreateWindowExW( 0, L"STATIC", L"modewin", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 900, 500, 600, 300, 0, 0, 0, 0 );
    pump( 500 );
    step( hwnd, "C normal" );
    ShowWindow( hwnd, SW_MINIMIZE );
    pump( 500 );
    run_game( self );
    step( hwnd, "C after the game" );
    ShowWindow( hwnd, SW_RESTORE );
    pump( 500 );
    step( hwnd, "C restored" );
    DestroyWindow( hwnd );

    /* D: a maximized window that stays up behind the game */
    hwnd = make_window();
    step( hwnd, "D maximized" );
    run_game( self );
    step( hwnd, "D after the game" );

    /* E: as Electron answers WM_DISPLAYCHANGE: its placement set again,
     * maximized, while the game's mode is on; then minimized and restored */
    game_hook = hwnd;
    run_game( self );
    step( hwnd, "E after the game" );
    ShowWindow( hwnd, SW_MINIMIZE );
    pump( 500 );
    step( hwnd, "E minimized" );
    ShowWindow( hwnd, SW_RESTORE );
    pump( 500 );
    step( hwnd, "E restored" );
    fprintf( out, "done\n" );
    fclose( out );
    FILE *done = fopen( "D:\\NTDONE.TXT", "w" );
    if (done) { fputs( "modewin\n", done ); fclose( done ); }
    return 0;
}
