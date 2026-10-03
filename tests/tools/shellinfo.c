/* What a newly started process sees of the shell, as explorer.exe asks it
 * before it decides to be the shell or to open a folder window: the shell
 * window (GetShellWindow), Progman, how many taskbars there are and the
 * desktop the process is on. Shown in a message box. "shellinfo set" also
 * tries SetShellWindowEx on Progman and its list view, to tell why the shell
 * could not. */
#include <windows.h>
#include <stdio.h>

static int taskbars;

static BOOL CALLBACK count( HWND hwnd, LPARAM lparam )
{
    WCHAR cls[64];

    GetClassNameW( hwnd, cls, ARRAYSIZE(cls) );
    if (!wcscmp( cls, L"Shell_TrayWnd" )) taskbars++;
    return TRUE;
}

int WINAPI wWinMain( HINSTANCE instance, HINSTANCE prev, WCHAR *cmdline, int show )
{
    WCHAR desktop[64] = L"?", station[64] = L"?", text[1024], set[128] = L"";
    HWND shell = GetShellWindow(), progman = FindWindowW( L"Progman", NULL ), view = 0, list = 0;
    DWORD shell_pid = 0, progman_pid = 0;

    GetUserObjectInformationW( GetThreadDesktop( GetCurrentThreadId() ), UOI_NAME, desktop, sizeof(desktop), NULL );
    GetUserObjectInformationW( GetProcessWindowStation(), UOI_NAME, station, sizeof(station), NULL );
    if (shell) GetWindowThreadProcessId( shell, &shell_pid );
    if (progman) GetWindowThreadProcessId( progman, &progman_pid );
    EnumWindows( count, 0 );
    if (progman) view = FindWindowExW( progman, 0, L"SHELLDLL_DefView", NULL );
    if (view) list = FindWindowExW( view, 0, L"SysListView32", NULL );
    if (!wcscmp( cmdline, L"set" ) && progman)
    {
        typedef BOOL (WINAPI *set_func)( HWND, HWND );
        set_func set_shell = (set_func)GetProcAddress( GetModuleHandleW( L"user32" ), "SetShellWindowEx" );
        BOOL ok;

        SetLastError( 0 );
        ok = set_shell && set_shell( progman, list ? list : progman );
        swprintf( set, ARRAYSIZE(set), L"\nSetShellWindowEx: %d (error %lu), now %p", ok, GetLastError(),
                  GetShellWindow() );
    }
    swprintf( text, ARRAYSIZE(text),
              L"GetShellWindow %p (process %lu)\nProgman %p (process %lu) exstyle %#lx style %#lx\n"
              L"DefView %p, list view %p exstyle %#lx\ntaskbars %d\n"
              L"desktop %ls\\%ls\nthis process %lu%ls",
              shell, shell_pid, progman, progman_pid, progman ? GetWindowLongW( progman, GWL_EXSTYLE ) : 0,
              progman ? GetWindowLongW( progman, GWL_STYLE ) : 0, view, list,
              list ? GetWindowLongW( list, GWL_EXSTYLE ) : 0, taskbars, station, desktop,
              GetCurrentProcessId(), set );
    MessageBoxW( NULL, text, L"shellinfo", MB_OK );
    return 0;
}
