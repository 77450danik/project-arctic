/*
 * Arctic window server: the desktop
 *
 * csrss.exe hosts this library, as in Windows. It owns the desktop window,
 * loads the display driver for it and feeds input devices to wineserver:
 * the part of Wine's "explorer.exe /desktop" that is not the shell. The
 * shell (explorer.exe) is an ordinary program on top.
 *
 * Based on programs/explorer/desktop.c:
 * Copyright 2006 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <stdio.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wingdi.h"
#include "winuser.h"
#include "winreg.h"
#include "rpc.h"
#include "wine/debug.h"

#include "winsrv_private.h"
#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(winsrv);

#define DESKTOP_CLASS_ATOM ((LPCWSTR)MAKEINTATOM(32769))

static const WCHAR ready_event_name[] = L"__arctic_desktop_ready";
static WNDPROC desktop_orig_wndproc;

BOOL WINAPI DllMain( HINSTANCE instance, DWORD reason, void *reserved )
{
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls( instance );
    return !__wine_init_unix_call();
}

/* the display driver is named in HKCU\Software\Wine\Drivers\Graphics and
 * published for every other process under the display GUID */
static void load_graphics_driver( const GUID *guid )
{
    WCHAR buffer[MAX_PATH] = L"wayland", libname[32], key[128], *name, *next;
    const char *error = "The graphics driver is missing. Check your build!";
    HMODULE module = 0;
    DWORD size = sizeof(buffer);
    HKEY hkey;

    RegGetValueW( HKEY_CURRENT_USER, L"Software\\Wine\\Drivers", L"Graphics", RRF_RT_REG_SZ, NULL, buffer, &size );

    for (name = buffer; name; name = next)
    {
        if ((next = wcschr( name, ',' ))) *next++ = 0;
        swprintf( libname, ARRAY_SIZE(libname), L"wine%s.drv", name );
        if ((module = LoadLibraryW( libname ))) break;
        if (GetLastError() == ERROR_DLL_INIT_FAILED) error = "The display compositor is not running.";
    }
    TRACE( "display %s driver %s\n", debugstr_guid(guid), debugstr_w(libname) );

    swprintf( key, ARRAY_SIZE(key),
              L"System\\CurrentControlSet\\Control\\Video\\{%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}\\0000",
              guid->Data1, guid->Data2, guid->Data3, guid->Data4[0], guid->Data4[1], guid->Data4[2],
              guid->Data4[3], guid->Data4[4], guid->Data4[5], guid->Data4[6], guid->Data4[7] );
    if (RegCreateKeyExW( HKEY_LOCAL_MACHINE, key, 0, NULL, REG_OPTION_VOLATILE, KEY_SET_VALUE, NULL, &hkey, NULL ))
        return;
    if (module)
        RegSetValueExW( hkey, L"GraphicsDriver", 0, REG_SZ, (BYTE *)libname, (wcslen( libname ) + 1) * sizeof(WCHAR) );
    else
    {
        ERR( "%s\n", error );
        RegSetValueExA( hkey, "DriverError", 0, REG_SZ, (const BYTE *)error, strlen( error ) + 1 );
    }
    RegCloseKey( hkey );
}

/* a window the screen no longer has room for comes back onto the primary
 * monitor, as it does in Windows when its monitor is unplugged. The shell's
 * own windows place themselves: the taskbar docks where it belongs, and
 * moving it here would only take it out of its place at the top */
static BOOL CALLBACK bring_window_on_screen( HWND hwnd, LPARAM param )
{
    const RECT *work = (const RECT *)param;
    WCHAR class[64] = {0};
    RECT rect;
    int x, y;

    if (!IsWindowVisible( hwnd ) || IsIconic( hwnd )) return TRUE;
    if (GetWindowLongW( hwnd, GWL_EXSTYLE ) & WS_EX_TOOLWINDOW) return TRUE;
    if (MonitorFromWindow( hwnd, MONITOR_DEFAULTTONULL )) return TRUE;
    if (!GetWindowRect( hwnd, &rect )) return TRUE;

    x = min( max( rect.left, work->left ), max( work->right - (rect.right - rect.left), work->left ) );
    y = min( max( rect.top, work->top ), max( work->bottom - (rect.bottom - rect.top), work->top ) );
    GetClassNameW( hwnd, class, ARRAY_SIZE(class) );
    MESSAGE( "csrss: %p (%s) is on no monitor at %s, it comes back to %d,%d\n", hwnd, debugstr_w(class),
             wine_dbgstr_rect( &rect ), x, y );
    SetWindowPos( hwnd, 0, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE );
    return TRUE;
}

/* Windows that the monitors lost come back to the primary one's work area.
 * Only when the monitors really changed: while the display driver counts
 * them again there are none for a moment, and then every window, a
 * maximized Chrome too, was "on no monitor" and the work area nothing:
 * windows went to 32767,32767 (the largest place win32u allows), off the
 * screen, active and in front, when Chrome started or the Control Panel
 * opened. */
static void bring_windows_on_screen(void)
{
    MONITORINFO info = { .cbSize = sizeof(info) };
    POINT origin = { 0, 0 };

    if (!GetSystemMetrics( SM_CMONITORS )) return;
    if (!GetMonitorInfoW( MonitorFromPoint( origin, MONITOR_DEFAULTTOPRIMARY ), &info )) return;
    if (IsRectEmpty( &info.rcWork ) || info.rcWork.right - info.rcWork.left > 32767 ||
        info.rcWork.bottom - info.rcWork.top > 32767)
        return;
    EnumWindows( bring_window_on_screen, (LPARAM)&info.rcWork );
}

static WCHAR monitor_set[1024];

/* the monitors the desktop stands on, each by its id: a monitor that took the
 * place of another (the display moved to another card) is a new one too */
static void read_monitor_set( WCHAR *set, SIZE_T size )
{
    DISPLAY_DEVICEW adapter = { .cb = sizeof(adapter) }, monitor = { .cb = sizeof(monitor) };
    SIZE_T len = 0;

    set[0] = 0;
    for (DWORD i = 0; EnumDisplayDevicesW( NULL, i, &adapter, 0 ); i++)
    {
        for (DWORD j = 0; EnumDisplayDevicesW( adapter.DeviceName, j, &monitor, 0 ); j++)
        {
            if (!(monitor.StateFlags & DISPLAY_DEVICE_ACTIVE)) continue;
            len += swprintf( set + len, size - len, L"%s;", monitor.DeviceID );
            if (len >= size - 1) return;
        }
    }
}

/* The desktop has just changed size or shape. win32u tells the programs
 * itself when one of them asked for it; a monitor plugged in, unplugged or
 * replaced comes from the display driver, and then this is the only place
 * that knows. */
static void display_changed(void)
{
    static RECT desktop;
    int count = GetSystemMetrics( SM_CMONITORS );
    DEVMODEW mode = { .dmSize = sizeof(mode) };
    WCHAR set[ARRAY_SIZE(monitor_set)];
    RECT now;

    /* no monitor at all: the driver is counting them again, nothing changed yet */
    if (!count)
    {
        MESSAGE( "csrss: the display changed with no monitor, the windows stay\n" );
        return;
    }
    SetRect( &now, GetSystemMetrics( SM_XVIRTUALSCREEN ), GetSystemMetrics( SM_YVIRTUALSCREEN ),
             GetSystemMetrics( SM_XVIRTUALSCREEN ) + GetSystemMetrics( SM_CXVIRTUALSCREEN ),
             GetSystemMetrics( SM_YVIRTUALSCREEN ) + GetSystemMetrics( SM_CYVIRTUALSCREEN ) );
    read_monitor_set( set, ARRAY_SIZE(set) );
    if (!set[0]) return;
    if (!wcscmp( set, monitor_set ) && EqualRect( &now, &desktop )) return;
    desktop = now;
    bring_windows_on_screen();
    if (!wcscmp( set, monitor_set )) return;
    wcscpy( monitor_set, set );
    ClipCursor( NULL );

    if (!EnumDisplaySettingsExW( NULL, ENUM_CURRENT_SETTINGS, &mode, 0 )) return;
    MESSAGE( "csrss: %d monitor%s, desktop %ux%u\n", count, count == 1 ? "" : "s",
             GetSystemMetrics( SM_CXVIRTUALSCREEN ), GetSystemMetrics( SM_CYVIRTUALSCREEN ) );
    SendMessageTimeoutW( HWND_BROADCAST, WM_DISPLAYCHANGE, mode.dmBitsPerPel,
                         MAKELPARAM( mode.dmPelsWidth, mode.dmPelsHeight ), SMTO_ABORTIFHUNG, 2000, NULL );
}

static LRESULT WINAPI desktop_wnd_proc( HWND hwnd, UINT message, WPARAM wp, LPARAM lp )
{
    static UINT input_language_message;

    /* an input indicator chose a language (wp 0), or asks for the session's (wp 1) */
    if (!input_language_message) input_language_message = RegisterWindowMessageW( L"ArcticInputLanguage" );
    if (message == input_language_message)
    {
        if (wp) return (LRESULT)get_input_language();
        set_input_language( (HKL)lp );
        return 0;
    }

    switch (message)
    {
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_CLOSE) return 0;
        break;
    case WM_DISPLAYCHANGE:
    {
        /* the desktop window follows the monitors first */
        LRESULT ret = desktop_orig_wndproc( hwnd, message, wp, lp );
        display_changed();
        return ret;
    }
    case WM_CLOSE:
        /* wineserver's word that no other process uses the desktop any more:
         * the session's desktop lives as long as the session, as in Windows,
         * even while the shell restarts */
        return 0;
    case WM_SETCURSOR:
        return (LRESULT)SetCursor( LoadCursorW( 0, (LPCWSTR)IDC_ARROW ) );
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_ERASEBKGND:
        return TRUE; /* dwm.exe draws the desktop background */
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        BeginPaint( hwnd, &ps );
        EndPaint( hwnd, &ps );
        return 0;
    }
    }
    return desktop_orig_wndproc( hwnd, message, wp, lp );
}

/* the current modes go to the registry, as Windows has them there */
static void store_display_settings(void)
{
    DISPLAY_DEVICEW device = { .cb = sizeof(device) };

    for (DWORD i = 0; EnumDisplayDevicesW( NULL, i, &device, 0 ); i++)
    {
        DEVMODEW mode = { .dmSize = sizeof(mode) };

        if (!EnumDisplaySettingsExW( device.DeviceName, ENUM_CURRENT_SETTINGS, &mode, 0 )) continue;
        if (ChangeDisplaySettingsExW( device.DeviceName, &mode, 0, CDS_GLOBAL | CDS_UPDATEREGISTRY | CDS_NORESET, 0 ))
            ERR( "cannot store the display settings of %s\n", debugstr_w(device.DeviceName) );
    }
}

/* win32u names the process that changed the display mode here; the mode
 * goes back to the stored one when that process ends */
static HANDLE fullscreen_process;

static LRESULT WINAPI restorer_wnd_proc( HWND hwnd, UINT message, WPARAM wp, LPARAM lp )
{
    if (message != WM_USER) return DefWindowProcW( hwnd, message, wp, lp );
    if (fullscreen_process) CloseHandle( fullscreen_process );
    fullscreen_process = lp ? OpenProcess( SYNCHRONIZE, FALSE, lp ) : NULL;
    return 0;
}

static DWORD WINAPI display_settings_restorer_thread( void *arg )
{
    static const WCHAR class_name[] = L"__wine_display_settings_restorer";
    WNDCLASSW class = { .lpfnWndProc = restorer_wnd_proc, .lpszClassName = class_name };
    HANDLE mutex;
    DWORD ret;
    MSG msg;

    SetThreadDescription( GetCurrentThread(), L"DisplaySettingsRestorer" );
    mutex = CreateMutexW( NULL, TRUE, L"__wine_display_settings_restorer_mutex" );
    if (GetLastError() == ERROR_ALREADY_EXISTS) WaitForSingleObject( mutex, INFINITE );

    if (!RegisterClassW( &class ) ||
        !CreateWindowW( class_name, NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, 0, 0, NULL ))
        return 0;

    for (;;)
    {
        while (PeekMessageW( &msg, NULL, 0, 0, PM_REMOVE ))
        {
            if (msg.message == WM_QUIT) return 0;
            DispatchMessageW( &msg );
        }
        ret = MsgWaitForMultipleObjects( fullscreen_process ? 1 : 0, &fullscreen_process, FALSE, INFINITE, QS_ALLINPUT );
        if (ret == WAIT_FAILED) return 0;
        if (!fullscreen_process || ret != WAIT_OBJECT_0) continue;
        ChangeDisplaySettingsExW( NULL, NULL, NULL, 0, NULL );
        CloseHandle( fullscreen_process );
        fullscreen_process = NULL;
    }
}

/***********************************************************************
 *           UserServerDllInitialization   (WINSRV.@)
 *
 * Creates the desktop window in the calling thread, which then has to
 * dispatch its messages. Another csrss.exe finds the desktop taken, waits
 * until it is ready and gets STATUS_OBJECT_NAME_EXISTS.
 */
NTSTATUS WINAPI UserServerDllInitialization( void *server_dll )
{
    HANDLE ready, thread;
    GUID guid;
    HWND hwnd;
    NTSTATUS status;

    ready = CreateEventW( NULL, TRUE, FALSE, ready_event_name );
    CreateMutexW( NULL, FALSE, L"__arctic_csrss" );
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        WaitForSingleObject( ready, 30000 );
        return STATUS_OBJECT_NAME_EXISTS;
    }

    UuidCreate( &guid );
    load_graphics_driver( &guid );

    if ((status = NtSetInformationProcess( GetCurrentProcess(), ProcessWineGrantAdminToken, NULL, 0 )))
        WARN( "no admin token: %#lx\n", status );

    if (!(hwnd = CreateWindowExW( 0, DESKTOP_CLASS_ATOM, NULL, WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                                  0, 0, 0, 0, 0, 0, 0, &guid )))
    {
        ERR( "cannot create the desktop window: %lu\n", GetLastError() );
        return STATUS_UNSUCCESSFUL;
    }
    /* the parent of HWND_MESSAGE windows */
    CreateWindowExW( 0, L"Message", NULL, WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                     0, 0, 100, 100, 0, 0, 0, NULL );

    desktop_orig_wndproc = (WNDPROC)SetWindowLongPtrW( hwnd, GWLP_WNDPROC, (LONG_PTR)desktop_wnd_proc );
    SetWindowPos( hwnd, 0, GetSystemMetrics( SM_XVIRTUALSCREEN ), GetSystemMetrics( SM_YVIRTUALSCREEN ),
                  GetSystemMetrics( SM_CXVIRTUALSCREEN ), GetSystemMetrics( SM_CYVIRTUALSCREEN ), SWP_SHOWWINDOW );
    ClipCursor( NULL );
    SetCursorPos( GetSystemMetrics( SM_CXSCREEN ) / 2, GetSystemMetrics( SM_CYSCREEN ) / 2 );
    read_monitor_set( monitor_set, ARRAY_SIZE(monitor_set) );
    store_display_settings();
    if ((thread = CreateThread( NULL, 0, display_settings_restorer_thread, NULL, 0, NULL ))) CloseHandle( thread );

    start_raw_input_thread();
    start_hung_app_thread();
    SetEvent( ready );
    MESSAGE( "csrss: desktop %ux%u\n", GetSystemMetrics( SM_CXVIRTUALSCREEN ), GetSystemMetrics( SM_CYVIRTUALSCREEN ) );
    return STATUS_SUCCESS;
}
