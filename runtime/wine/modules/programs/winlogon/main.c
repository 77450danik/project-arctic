/*
 * Windows Logon Application
 *
 * There is one user and no logon screen: winlogon runs the Userinit
 * programs for that user at once, then stays for the keys only the system
 * handles: Ctrl+Alt+Del (security options) and Ctrl+Shift+Esc (Task
 * Manager). The session cannot go on without it: wininit.exe stops the
 * system when it ends other than by restart or shut down.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <windows.h>

#include "wine/debug.h"

#include "winlogon.h"

WINE_DEFAULT_DEBUG_CHANNEL(winlogon);

#define HOTKEY_SECURITY_OPTIONS 1
#define HOTKEY_TASK_MANAGER     2

#define TIMER_SHELL             1
#define SHELL_CHECK_MS          2000

#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif

/* what user32 sends when a program calls ExitWindowsEx, as in Windows */
#define WM_LOGONNOTIFY 0x004c
#define LN_LOGOFF      0x0

static const WCHAR winlogon_key[] = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon";

void run( WCHAR *cmdline )
{
    STARTUPINFOW si = { .cb = sizeof(si) };
    PROCESS_INFORMATION pi;
    WCHAR dir[MAX_PATH];

    if (!GetEnvironmentVariableW( L"USERPROFILE", dir, MAX_PATH )) GetSystemDirectoryW( dir, MAX_PATH );
    if (!CreateProcessW( NULL, cmdline, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi ))
    {
        ERR( "cannot start %s: %lu\n", debugstr_w(cmdline), GetLastError() );
        return;
    }
    CloseHandle( pi.hThread );
    CloseHandle( pi.hProcess );
}

/* Winlogon\AutoRestartShell: when the shell ends, whether it is started
 * again, as Windows does unless the value is 0 */
static BOOL had_shell;

/* the watchdog started the shell again: it is not one that ended */
void shell_restarted(void)
{
    had_shell = FALSE;
}

static void check_shell(void)
{
    WCHAR shell[MAX_PATH] = L"explorer.exe";
    DWORD restart = 1, size = sizeof(restart);

    if (GetShellWindow())
    {
        had_shell = TRUE;
        return;
    }
    if (!had_shell) return;
    had_shell = FALSE;

    RegGetValueW( HKEY_LOCAL_MACHINE, winlogon_key, L"AutoRestartShell", RRF_RT_REG_DWORD, NULL, &restart, &size );
    if (!restart) return;
    size = sizeof(shell);
    RegGetValueW( HKEY_LOCAL_MACHINE, winlogon_key, L"Shell", RRF_RT_REG_SZ, NULL, shell, &size );
    ERR( "the shell ended, starting it again\n" );
    run( shell );
}

/* Winlogon\Userinit is a comma-separated list, "userinit.exe," by default */
void run_userinit(void)
{
    WCHAR list[1024] = L"C:\\Windows\\system32\\userinit.exe,", *item, *next;
    DWORD size = sizeof(list);

    RegGetValueW( HKEY_LOCAL_MACHINE, winlogon_key, L"Userinit", RRF_RT_REG_SZ, NULL, list, &size );
    for (item = list; item; item = next)
    {
        if ((next = wcschr( item, ',' ))) *next++ = 0;
        while (*item == ' ') item++;
        if (*item) run( item );
    }
}

static LRESULT WINAPI sas_window_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    WCHAR taskmgr[] = L"taskmgr.exe";

    if (msg == WM_LOGONNOTIFY && wp == LN_LOGOFF)
    {
        UINT flags = (UINT)lp;

        /* EWX_REBOOT restarts the machine, EWX_SHUTDOWN and EWX_POWEROFF stop it */
        if (flags & EWX_REBOOT) end_session( WINLOGON_EXIT_RESTART );
        else if (flags & (EWX_SHUTDOWN | EWX_POWEROFF)) end_session( WINLOGON_EXIT_SHUTDOWN );
        else end_session( WINLOGON_EXIT_LOGOFF );
        return 0;
    }
    if (msg == WM_TIMER && wp == TIMER_SHELL)
    {
        if (check_shell_answers()) shell_restarted();
        else check_shell();
        return 0;
    }
    if (msg != WM_HOTKEY) return DefWindowProcW( hwnd, msg, wp, lp );
    if (wp == HOTKEY_SECURITY_OPTIONS) show_security_options();
    else if (wp == HOTKEY_TASK_MANAGER) run( taskmgr );
    return 0;
}

/* receives the secure attention keys, as Windows' "SAS window" does */
static void create_sas_window(void)
{
    WNDCLASSW class = { .lpfnWndProc = sas_window_proc, .hInstance = GetModuleHandleW( NULL ),
                        .lpszClassName = L"SAS window class" };
    HWND hwnd;

    RegisterClassW( &class );
    if (!(hwnd = CreateWindowW( class.lpszClassName, L"SAS window", WS_POPUP, 0, 0, 0, 0, 0, 0,
                                class.hInstance, NULL )))
    {
        ERR( "cannot create the SAS window: %lu\n", GetLastError() );
        return;
    }
    if (!RegisterHotKey( hwnd, HOTKEY_SECURITY_OPTIONS, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_DELETE ))
        ERR( "Ctrl+Alt+Del is taken: %lu\n", GetLastError() );
    if (!RegisterHotKey( hwnd, HOTKEY_TASK_MANAGER, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_ESCAPE ))
        ERR( "Ctrl+Shift+Esc is taken: %lu\n", GetLastError() );
    SetTimer( hwnd, TIMER_SHELL, SHELL_CHECK_MS, NULL );
}

int WINAPI wWinMain( HINSTANCE instance, HINSTANCE prev, WCHAR *cmdline, int show )
{
    MSG msg;

    create_sas_window();
    start_power_policy();
    run_userinit();
    /* the session has begun: services.exe starts the services of programs
     * now (Wine patch 0072), never while wineboot still sets the system up */
    SetEvent( CreateEventW( NULL, TRUE, FALSE, L"Global\\__arctic_session_started" ) );

    while (GetMessageW( &msg, 0, 0, 0 ))
    {
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }
    return 0;
}
