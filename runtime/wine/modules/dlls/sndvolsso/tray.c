/*
 * Volume icon of the notification area: the icon
 *
 * One thread inside explorer keeps the icon of Windows 10 up to date: the
 * speaker with none to three waves by the level, a cross when muted or when
 * there is no output, and "Speakers (Realtek ALC892): 50%" as its tip. A
 * click opens the flyout (flyout.c); the menu opens the Sound control panel
 * and the volume mixer. The keyboard's volume keys work wherever the focus
 * is: two steps of the 51 up or down, and mute, as Windows has them, and
 * bring up the overlay (osd.c); its media keys go to what plays (media.c).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "sndvolsso.h"
#include "objbase.h"
#include "shellapi.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(sndvolsso);

#define WM_TRAY_ICON     (WM_APP + 1)
#define WM_SHOW_OSD      (WM_APP + 2)
#define TIMER_REFRESH    1
#define ICON_ID          1

#define HOTKEY_MUTE      1
#define HOTKEY_DOWN      2
#define HOTKEY_UP        3

#define MENU_SETTINGS    1
#define MENU_MIXER       2
#define MENU_SOUNDS      3

static HANDLE thread;
static HWND tray_hwnd;
static HHOOK hook;
static UINT taskbar_created;
static HICON current_icon;
static int current_glyph = -1;
static WCHAR current_tip[128];

/* the glyph the icon shows: 0-3 waves, 4 the cross */
static int glyph_of( BOOL device, float level, BOOL mute )
{
    int percent = (int)(level * 100 + 0.5f);

    if (!device || mute) return 4;
    return !percent ? 0 : percent < 34 ? 1 : percent < 67 ? 2 : 3;
}

void tray_update(void)
{
    NOTIFYICONDATAW data = { .cbSize = sizeof(data), .hWnd = tray_hwnd, .uID = ICON_ID,
                             .uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE, .uCallbackMessage = WM_TRAY_ICON };
    struct endpoint def;
    float level;
    BOOL mute, device;
    int glyph;

    if (!tray_hwnd) return;
    device = audio_state( &def, &level, &mute );
    glyph = glyph_of( device, level, mute );

    if (!device) lstrcpynW( data.szTip, load_string( IDS_NO_DEVICE ), ARRAY_SIZE(data.szTip) );
    else if (mute) swprintf( data.szTip, ARRAY_SIZE(data.szTip), load_string( IDS_TIP_MUTED ), def.name );
    else swprintf( data.szTip, ARRAY_SIZE(data.szTip), load_string( IDS_TIP_LEVEL ), def.name,
                   (int)(level * 100 + 0.5f) );

    if (glyph != current_glyph || !current_icon)
    {
        static const float levels[] = { 0.f, 0.2f, 0.5f, 1.f, 1.f };   /* a level that shows each */
        HICON icon = make_speaker_icon( GetSystemMetrics( SM_CXSMICON ), levels[glyph], glyph == 4 );
        if (current_icon) DestroyIcon( current_icon );
        current_icon = icon;
        current_glyph = glyph;
    }
    else if (!wcscmp( data.szTip, current_tip )) return;

    data.hIcon = current_icon;
    lstrcpynW( current_tip, data.szTip, ARRAY_SIZE(current_tip) );
    if (!Shell_NotifyIconW( NIM_MODIFY, &data )) Shell_NotifyIconW( NIM_ADD, &data );
}

/* the volume keys: Windows steps by 2 and shows nothing more than the icon here */
static void on_hotkey( UINT id )
{
    struct endpoint def;
    float level;
    BOOL mute;

    if (!audio_state( &def, &level, &mute )) return;
    switch (id)
    {
    case HOTKEY_MUTE:
        audio_set_mute( !mute );
        break;
    case HOTKEY_DOWN:
    case HOTKEY_UP:
        level = ((int)(level * 100 + 0.5f) + (id == HOTKEY_UP ? 2 : -2)) / 100.f;
        audio_set_level( max( 0.f, min( 1.f, level ) ) );
        if (mute) audio_set_mute( FALSE );
        break;
    }
    tray_update();
    flyout_refresh();
    if (!flyout_visible()) osd_show();
}

/* The media keys go to what plays, as Windows routes them to the session of
 * SystemMediaTransportControls: the press and its release are the session's
 * and the overlay shows it. With no session they go on to the programs, so
 * that players which take the keys themselves still have them. */
static LRESULT CALLBACK keyboard_proc( int code, WPARAM wparam, LPARAM lparam )
{
    const KBDLLHOOKSTRUCT *key = (const KBDLLHOOKSTRUCT *)lparam;
    struct media_info info;
    int action = -1;

    if (code == HC_ACTION)
    {
        switch (key->vkCode)
        {
        case VK_MEDIA_PLAY_PAUSE: action = MEDIA_PLAYPAUSE; break;
        case VK_MEDIA_NEXT_TRACK: action = MEDIA_NEXT; break;
        case VK_MEDIA_PREV_TRACK: action = MEDIA_PREVIOUS; break;
        }
    }
    if (action >= 0 && media_session( &info ))
    {
        if (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN)
        {
            media_press( &info, action );
            PostMessageW( tray_hwnd, WM_SHOW_OSD, 0, 0 );
        }
        return 1;
    }
    return CallNextHookEx( NULL, code, wparam, lparam );
}

static void show_menu( HWND hwnd )
{
    HMENU menu = CreatePopupMenu();
    POINT pt;

    AppendMenuW( menu, MF_STRING, MENU_SETTINGS, load_string( IDS_OPEN_SETTINGS ) );
    AppendMenuW( menu, MF_STRING, MENU_MIXER, load_string( IDS_OPEN_MIXER ) );
    AppendMenuW( menu, MF_SEPARATOR, 0, NULL );
    AppendMenuW( menu, MF_STRING, MENU_SOUNDS, load_string( IDS_SOUNDS ) );
    GetCursorPos( &pt );
    SetForegroundWindow( hwnd );
    switch (TrackPopupMenu( menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, NULL ))
    {
    case MENU_SETTINGS:
        ShellExecuteW( NULL, NULL, L"control.exe", L"mmsys.cpl", NULL, SW_SHOWNORMAL );
        break;
    case MENU_MIXER:
        ShellExecuteW( NULL, NULL, L"sndvol32.exe", NULL, NULL, SW_SHOWNORMAL );
        break;
    case MENU_SOUNDS:
        ShellExecuteW( NULL, NULL, L"control.exe", L"mmsys.cpl,,1", NULL, SW_SHOWNORMAL );
        break;
    }
    DestroyMenu( menu );
}

static LRESULT WINAPI tray_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    if (msg == taskbar_created && taskbar_created)
    {
        /* explorer restarted: the icon goes back */
        current_tip[0] = 0;
        current_glyph = -1;
        tray_update();
        return 0;
    }
    switch (msg)
    {
    case WM_TIMER:
        tray_update();
        return 0;
    case WM_TRAY_ICON:
        if (lp == WM_LBUTTONUP) flyout_toggle( hwnd, ICON_ID );
        else if (lp == WM_RBUTTONUP)
        {
            flyout_hide();
            show_menu( hwnd );
        }
        return 0;
    case WM_HOTKEY:
        on_hotkey( wp );
        return 0;
    case WM_SHOW_OSD:
        if (!flyout_visible()) osd_show();
        else flyout_refresh();
        return 0;
    case WM_CLOSE:
        DestroyWindow( hwnd );
        return 0;
    case WM_DESTROY:
    {
        NOTIFYICONDATAW data = { .cbSize = sizeof(data), .hWnd = hwnd, .uID = ICON_ID };
        Shell_NotifyIconW( NIM_DELETE, &data );
        UnregisterHotKey( hwnd, HOTKEY_MUTE );
        UnregisterHotKey( hwnd, HOTKEY_DOWN );
        UnregisterHotKey( hwnd, HOTKEY_UP );
        PostQuitMessage( 0 );
        return 0;
    }
    }
    return DefWindowProcW( hwnd, msg, wp, lp );
}

static DWORD WINAPI tray_thread( void *arg )
{
    WNDCLASSW cls = { .lpfnWndProc = tray_proc, .hInstance = sndvolsso_instance, .lpszClassName = L"ArcticVolumeTray" };
    MSG msg;

    /* the sound comes up once the shell has: the shell's desktop and taskbar
     * came out unpainted when it came up at the same time (at boot now and
     * then, and every time the shell started again at a new scale) */
    for (int i = 0; i < 100 && !GetShellWindow(); i++) Sleep( 100 );
    Sleep( 1000 );

    CoInitializeEx( NULL, COINIT_APARTMENTTHREADED );
    taskbar_created = RegisterWindowMessageW( L"TaskbarCreated" );
    RegisterClassW( &cls );
    tray_hwnd = CreateWindowExW( 0, cls.lpszClassName, NULL, WS_POPUP, 0, 0, 0, 0, NULL, NULL, sndvolsso_instance, NULL );
    audio_init();
    RegisterHotKey( tray_hwnd, HOTKEY_MUTE, 0, VK_VOLUME_MUTE );
    RegisterHotKey( tray_hwnd, HOTKEY_DOWN, 0, VK_VOLUME_DOWN );
    RegisterHotKey( tray_hwnd, HOTKEY_UP, 0, VK_VOLUME_UP );
    hook = SetWindowsHookExW( WH_KEYBOARD_LL, keyboard_proc, sndvolsso_instance, 0 );

    tray_update();
    /* other programs and other outputs change it too */
    SetTimer( tray_hwnd, TIMER_REFRESH, 1000, NULL );
    while (GetMessageW( &msg, NULL, 0, 0 ))
    {
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }

    if (hook) UnhookWindowsHookEx( hook );
    hook = NULL;
    flyout_hide();
    osd_hide();
    if (current_icon) DestroyIcon( current_icon );
    current_icon = NULL;
    current_glyph = -1;
    tray_hwnd = NULL;
    audio_shutdown();
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
