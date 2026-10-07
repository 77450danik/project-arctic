/*
 * Battery icon of the notification area: the icon
 *
 * One thread inside explorer keeps the icon of Windows 10 up to date: the
 * battery glyph by the charge in tenths, with the plug on the mains and the
 * leaf of battery saver, and Windows' tip ("3 год. 20 хв. (94%) залишилось",
 * "94% доступно (підключено до електромережі)"). Without a battery there is
 * no icon, as on a desktop PC; one plugged in later shows it. A click opens
 * the flyout (flyout.c); the menu has Power Options and the page of the
 * batteries and graphics cards (Windows' Mobility Center). The power
 * policy's low and reserve battery warnings become Windows' notifications.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "batmeter.h"
#include "objbase.h"
#include "shellapi.h"
#include "wine/debug.h"

#define WM_TRAY_ICON     (WM_APP + 1)
#define TIMER_REFRESH    1
#define ICON_ID          1

#define MENU_POWER_OPTIONS   1
#define MENU_MOBILITY_CENTER 2

static HANDLE thread;
static HWND tray_hwnd;
static UINT taskbar_created;
static const struct arctic_power_history *history;
static LONG warning_serial;
static HICON current_icon;
static WCHAR current_glyph;
static WCHAR current_tip[128];
static BOOL shown;
static int icon_size;

void tray_update(void)
{
    NOTIFYICONDATAW data = { .cbSize = sizeof(data), .hWnd = tray_hwnd, .uID = ICON_ID,
                             .uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP,
                             .uCallbackMessage = WM_TRAY_ICON };
    struct battery_view view;
    WCHAR glyph;

    if (!tray_hwnd) return;
    battery_read( &view );
    if (!view.status.battery_count)
    {
        if (shown)
        {
            Shell_NotifyIconW( NIM_DELETE, &data );
            shown = FALSE;
        }
        return;
    }

    glyph = battery_view_glyph( &view );
    battery_tip( &view, data.szTip, ARRAY_SIZE(data.szTip) );
    if (glyph != current_glyph || !current_icon || icon_size != GetSystemMetrics( SM_CXSMICON ))
    {
        HICON icon = make_glyph_icon( glyph, icon_size = GetSystemMetrics( SM_CXSMICON ) );
        if (current_icon) DestroyIcon( current_icon );
        current_icon = icon;
        current_glyph = glyph;
    }
    else if (shown && !wcscmp( data.szTip, current_tip )) return;

    data.hIcon = current_icon;
    lstrcpynW( current_tip, data.szTip, ARRAY_SIZE(current_tip) );
    if (!shown || !Shell_NotifyIconW( NIM_MODIFY, &data ))
    {
        shown = Shell_NotifyIconW( NIM_ADD, &data );
    }
}

/* Windows' notification: "Низький заряд акумулятора." / "Акумулятор майже розряджено" */
static void warn( UINT level )
{
    NOTIFYICONDATAW data = { .cbSize = sizeof(data), .hWnd = tray_hwnd, .uID = ICON_ID,
                             .uFlags = NIF_INFO, .dwInfoFlags = NIIF_WARNING };

    if (!shown) return;
    MESSAGE( "batmeter: battery warning %u\n", level );
    lstrcpynW( data.szInfoTitle, load_string( level == 2 ? IDS_RESERVE_TITLE : IDS_LOW_TITLE ), ARRAY_SIZE(data.szInfoTitle) );
    lstrcpynW( data.szInfo, load_string( level == 2 ? IDS_RESERVE_TEXT : IDS_LOW_TEXT ), ARRAY_SIZE(data.szInfo) );
    Shell_NotifyIconW( NIM_MODIFY, &data );
}

static void show_menu( HWND hwnd )
{
    HMENU menu = CreatePopupMenu();
    POINT pt;

    AppendMenuW( menu, MF_STRING, MENU_POWER_OPTIONS, load_string( IDS_MENU_POWER_OPTIONS ) );
    AppendMenuW( menu, MF_STRING, MENU_MOBILITY_CENTER, load_string( IDS_MENU_MOBILITY_CENTER ) );
    GetCursorPos( &pt );
    SetForegroundWindow( hwnd );
    switch (TrackPopupMenu( menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, NULL ))
    {
    case MENU_POWER_OPTIONS:
        open_power_options();
        break;
    case MENU_MOBILITY_CENTER:
        open_battery_panel();
        break;
    }
    DestroyMenu( menu );
}

static LRESULT WINAPI tray_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    if (msg == taskbar_created && taskbar_created)
    {
        /* explorer restarted: the icon goes back */
        shown = FALSE;
        current_tip[0] = 0;
        current_glyph = 0;
        tray_update();
        return 0;
    }
    switch (msg)
    {
    case WM_TIMER:
    case WM_POWERBROADCAST:
        tray_update();
        flyout_refresh();
        /* the power policy's low and reserve battery warnings */
        if (!history)
        {
            HANDLE map = OpenFileMappingW( FILE_MAP_READ, FALSE, ARCTIC_POWER_HISTORY_NAME );
            if (map)
            {
                history = MapViewOfFile( map, FILE_MAP_READ, 0, 0, sizeof(*history) );
                CloseHandle( map );
                if (history) warning_serial = history->warning_serial;
            }
        }
        if (history && history->warning_serial != warning_serial)
        {
            warning_serial = history->warning_serial;
            warn( history->warning_level );
        }
        return msg == WM_POWERBROADCAST ? TRUE : 0;
    case WM_TRAY_ICON:
        switch (LOWORD( lp ))
        {
        case WM_LBUTTONUP:
            flyout_toggle( hwnd, ICON_ID );
            break;
        case WM_RBUTTONUP:
            flyout_hide();
            show_menu( hwnd );
            break;
        }
        return 0;
    case WM_SETTINGCHANGE:
    case WM_DPICHANGED:
        current_glyph = 0;
        tray_update();
        return 0;
    case WM_CLOSE:
        DestroyWindow( hwnd );
        return 0;
    case WM_DESTROY:
    {
        NOTIFYICONDATAW data = { .cbSize = sizeof(data), .hWnd = hwnd, .uID = ICON_ID };
        Shell_NotifyIconW( NIM_DELETE, &data );
        shown = FALSE;
        PostQuitMessage( 0 );
        return 0;
    }
    }
    return DefWindowProcW( hwnd, msg, wp, lp );
}

static DWORD WINAPI tray_thread( void *arg )
{
    WNDCLASSW cls = { .lpfnWndProc = tray_proc, .hInstance = batmeter_instance, .lpszClassName = L"ArcticBatteryTray" };
    MSG msg;

    /* the battery comes up once the shell has, as the volume does */
    for (int i = 0; i < 100 && !GetShellWindow(); i++) Sleep( 100 );
    Sleep( 1000 );

    CoInitializeEx( NULL, COINIT_APARTMENTTHREADED );
    taskbar_created = RegisterWindowMessageW( L"TaskbarCreated" );
    RegisterClassW( &cls );
    /* a top-level window: the power policy's broadcasts reach it */
    tray_hwnd = CreateWindowExW( WS_EX_TOOLWINDOW, cls.lpszClassName, NULL, WS_POPUP, 0, 0, 0, 0, NULL, NULL,
                                 batmeter_instance, NULL );
    tray_update();
    SetTimer( tray_hwnd, TIMER_REFRESH, 2000, NULL );
    while (GetMessageW( &msg, NULL, 0, 0 ))
    {
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }

    flyout_hide();
    if (current_icon) DestroyIcon( current_icon );
    current_icon = NULL;
    current_glyph = 0;
    tray_hwnd = NULL;
    CoUninitialize();
    return 0;
}

void tray_start(void)
{
    if (thread) return;
    thread = CreateThread( NULL, 0, tray_thread, NULL, 0, NULL );
}

void tray_stop(void)
{
    if (!thread) return;
    if (tray_hwnd) PostMessageW( tray_hwnd, WM_CLOSE, 0, 0 );
    WaitForSingleObject( thread, 5000 );
    CloseHandle( thread );
    thread = NULL;
}
