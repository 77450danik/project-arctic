/*
 * Arctic window server: the Raw Input Thread
 *
 * Feeds host keyboards, mice and touchpads to wineserver as hardware input,
 * the way win32k's RIT does in Windows. Hit testing, focus and capture are
 * wineserver's; key repeat is done here, as the Windows keyboard class
 * driver does it.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wingdi.h"
#include "winuser.h"
#include "ntuser.h"
#include "wine/server.h"
#include "wine/debug.h"

#include "winsrv_private.h"
#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(winsrv);

#ifndef INPUTLANGCHANGE_FORWARD
#define INPUTLANGCHANGE_FORWARD 0x0002
#endif

/* evdev codes (linux/input-event-codes.h) */
#define KEY_KPDOT        83
#define KEY_102ND        86
#define KEY_F11          87
#define KEY_F12          88
#define KEY_KPENTER      96
#define KEY_RIGHTCTRL    97
#define KEY_KPSLASH      98
#define KEY_SYSRQ        99
#define KEY_RIGHTALT     100
#define KEY_HOME         102
#define KEY_UP           103
#define KEY_PAGEUP       104
#define KEY_LEFT         105
#define KEY_RIGHT        106
#define KEY_END          107
#define KEY_DOWN         108
#define KEY_PAGEDOWN     109
#define KEY_INSERT       110
#define KEY_DELETE       111
#define KEY_MUTE         113
#define KEY_VOLUMEDOWN   114
#define KEY_VOLUMEUP     115
#define KEY_POWER        116
#define KEY_PAUSE        119
#define KEY_LEFTMETA     125
#define KEY_RIGHTMETA    126
#define KEY_COMPOSE      127
#define KEY_STOP         128
#define KEY_HELP         138
#define KEY_MENU         139
#define KEY_SLEEP        142
#define KEY_PROG1        148
#define KEY_PROG2        149
#define KEY_MAIL         155
#define KEY_BOOKMARKS    156
#define KEY_COMPUTER     157
#define KEY_BACK         158
#define KEY_FORWARD      159
#define KEY_NEXTSONG     163
#define KEY_PLAYPAUSE    164
#define KEY_PREVIOUSSONG 165
#define KEY_STOPCD       166
#define KEY_HOMEPAGE     172
#define KEY_REFRESH      173
#define KEY_F13          183
#define KEY_F24          194
#define KEY_PRINT        210
#define KEY_SEARCH       217
#define KEY_MEDIA        226
#define BTN_LEFT         0x110
#define BTN_RIGHT        0x111
#define BTN_MIDDLE       0x112
#define BTN_SIDE         0x113
#define BTN_EXTRA        0x114
#define BTN_FORWARD      0x115
#define BTN_BACK         0x116

/* PC scan code set 1: 0x1xx is E0-prefixed, 0x2xx E1-prefixed, as in Wine's drivers */
static WORD key_to_scan( UINT key )
{
    if (key <= KEY_KPDOT) return key;
    if (key >= KEY_F13 && key <= KEY_F24 - 1) return 0x64 + key - KEY_F13;

    switch (key)
    {
    case KEY_102ND:        return 0x056;
    case KEY_F11:          return 0x057;
    case KEY_F12:          return 0x058;
    case KEY_COMPOSE:      return 0x05f;
    case KEY_HELP:         return 0x063;
    case KEY_COMPUTER:     return 0x071;
    case KEY_F24:          return 0x076;
    case KEY_SYSRQ:        return 0x054;
    case KEY_PREVIOUSSONG: return 0x110;
    case KEY_NEXTSONG:     return 0x119;
    case KEY_KPENTER:      return 0x11c;
    case KEY_RIGHTCTRL:    return 0x11d;
    case KEY_MUTE:         return 0x120;
    case KEY_PROG2:        return 0x121;
    case KEY_PLAYPAUSE:    return 0x122;
    case KEY_STOPCD:       return 0x124;
    case KEY_VOLUMEDOWN:   return 0x12e;
    case KEY_VOLUMEUP:     return 0x130;
    case KEY_HOMEPAGE:     return 0x132;
    case KEY_KPSLASH:      return 0x135;
    case KEY_PRINT:        return 0x137;
    case KEY_RIGHTALT:     return 0x138;
    case KEY_HOME:         return 0x147;
    case KEY_UP:           return 0x148;
    case KEY_PAGEUP:       return 0x149;
    case KEY_LEFT:         return 0x14b;
    case KEY_RIGHT:        return 0x14d;
    case KEY_END:          return 0x14f;
    case KEY_DOWN:         return 0x150;
    case KEY_PAGEDOWN:     return 0x151;
    case KEY_INSERT:       return 0x152;
    case KEY_DELETE:       return 0x153;
    case KEY_LEFTMETA:     return 0x15b;
    case KEY_RIGHTMETA:    return 0x15c;
    case KEY_MENU:         return 0x15d;
    case KEY_POWER:        return 0x15e;
    case KEY_SLEEP:        return 0x15f;
    case KEY_SEARCH:       return 0x165;
    case KEY_BOOKMARKS:    return 0x166;
    case KEY_REFRESH:      return 0x167;
    case KEY_STOP:         return 0x168;
    case KEY_FORWARD:      return 0x169;
    case KEY_BACK:         return 0x16a;
    case KEY_PROG1:        return 0x16b;
    case KEY_MAIL:         return 0x16c;
    case KEY_MEDIA:        return 0x16d;
    case KEY_PAUSE:        return 0x21d;
    }
    return 0;
}

/* The program keys go to, in the log whenever it changes: when a game hears
 * no keys, this says whether it ever had the foreground */
static void log_keyboard_target(void)
{
    static HWND last;
    HWND hwnd = GetForegroundWindow();
    WCHAR name[MAX_PATH] = L"-";
    DWORD pid = 0, size = ARRAY_SIZE(name);
    HANDLE process;

    if (hwnd == last) return;
    last = hwnd;
    if (hwnd) GetWindowThreadProcessId( hwnd, &pid );
    if (pid && (process = OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid )))
    {
        if (!QueryFullProcessImageNameW( process, 0, name, &size )) lstrcpyW( name, L"?" );
        CloseHandle( process );
    }
    MESSAGE( "csrss: keyboard to %s (window %p)\n", debugstr_w( wcsrchr( name, '\\' ) ? wcsrchr( name, '\\' ) + 1 : name ), hwnd );
}

static void send_key( UINT key, BOOL pressed )
{
    WORD scan = key_to_scan( key );
    INPUT input = { .type = INPUT_KEYBOARD };

    if (!scan) return;
    if (pressed) log_keyboard_target();
    input.ki.wScan = (scan & 0x300) ? scan + 0xdf00 : scan;
    input.ki.dwFlags = KEYEVENTF_SCANCODE;
    if (scan & ~0xff) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (!pressed) input.ki.dwFlags |= KEYEVENTF_KEYUP;
    NtUserSendHardwareInput( 0, SEND_HWMSG_RAWINPUT, &input, 0 );
}

static void send_mouse( DWORD flags, LONG x, LONG y, DWORD data )
{
    INPUT input = { .type = INPUT_MOUSE };

    input.mi.dx = x;
    input.mi.dy = y;
    input.mi.mouseData = data;
    input.mi.dwFlags = flags;
    NtUserSendHardwareInput( 0, SEND_HWMSG_RAWINPUT, &input, 0 );
}

static void send_button( UINT button, BOOL pressed )
{
    switch (button)
    {
    case BTN_LEFT:   send_mouse( pressed ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP, 0, 0, 0 ); break;
    case BTN_RIGHT:  send_mouse( pressed ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP, 0, 0, 0 ); break;
    case BTN_MIDDLE: send_mouse( pressed ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP, 0, 0, 0 ); break;
    case BTN_SIDE:
    case BTN_BACK:   send_mouse( pressed ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP, 0, 0, XBUTTON1 ); break;
    case BTN_EXTRA:
    case BTN_FORWARD: send_mouse( pressed ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP, 0, 0, XBUTTON2 ); break;
    }
}

static void send_absolute( INT32 x, INT32 y )
{
    int left = GetSystemMetrics( SM_XVIRTUALSCREEN ), top = GetSystemMetrics( SM_YVIRTUALSCREEN );
    int width = GetSystemMetrics( SM_CXVIRTUALSCREEN ), height = GetSystemMetrics( SM_CYVIRTUALSCREEN );

    send_mouse( MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE,
                left + MulDiv( x, width, 65536 ), top + MulDiv( y, height, 65536 ), 0 );
}

/* SPI_GETKEYBOARDDELAY: 0..3 is 250..1000 ms */
static DWORD repeat_delay(void)
{
    UINT delay = 1;

    SystemParametersInfoW( SPI_GETKEYBOARDDELAY, 0, &delay, 0 );
    return 250 * (min( delay, 3 ) + 1);
}

/* SPI_GETKEYBOARDSPEED: 0..31 is about 2.5..30 repeats a second */
static DWORD repeat_interval(void)
{
    UINT speed = 31;

    SystemParametersInfoW( SPI_GETKEYBOARDSPEED, 0, &speed, 0 );
    return 400 - 367 * min( speed, 31 ) / 31;
}

/* The input language is one for the whole session, as in Windows 10: Alt+Shift
 * moves it to the next of the user's layouts, and a program that gets the
 * keyboard is moved to it. win32k does this in Windows, so the RIT does here. */
static HKL input_language;
static DWORD language_thread;  /* the thread last moved to it */

static void request_language( HWND hwnd, HKL layout )
{
    GUITHREADINFO info = { .cbSize = sizeof(info) };
    DWORD tid = GetWindowThreadProcessId( hwnd, NULL );

    if (GetGUIThreadInfo( tid, &info ) && info.hwndFocus) hwnd = info.hwndFocus;
    PostMessageW( hwnd, WM_INPUTLANGCHANGEREQUEST, INPUTLANGCHANGE_FORWARD, (LPARAM)layout );
    language_thread = tid;
}

static void next_input_language(void)
{
    HKL layouts[32];
    UINT count = GetKeyboardLayoutList( ARRAY_SIZE(layouts), layouts ), i;
    HWND hwnd;

    if (count < 2) return;
    if (!input_language) input_language = GetKeyboardLayout( 0 );
    for (i = 0; i < count; i++) if (layouts[i] == input_language) break;
    input_language = layouts[i < count ? (i + 1) % count : 0];
    ActivateKeyboardLayout( input_language, 0 ); /* scan codes map to keys by the language */
    if ((hwnd = GetForegroundWindow())) request_language( hwnd, input_language );
}

/* the layout the RIT maps scan codes with, set on its own thread */
static HKL pending_layout;

/* a language chosen elsewhere, in the input indicator of the notification area */
void set_input_language( HKL layout )
{
    HWND hwnd;

    input_language = layout;
    InterlockedExchangePointer( (void **)&pending_layout, layout );
    if ((hwnd = GetForegroundWindow())) request_language( hwnd, layout );
}

/* the session's language for the input indicators; NULL until one is chosen */
HKL get_input_language(void)
{
    return input_language;
}

/* before a key goes to a program the language has not reached yet */
static void follow_input_language(void)
{
    HWND hwnd;

    if (!input_language || !(hwnd = GetForegroundWindow())) return;
    if (GetWindowThreadProcessId( hwnd, NULL ) != language_thread) request_language( hwnd, input_language );
}

/* Left Alt with Shift, and nothing else between: the language hotkey */
static BOOL language_chord( UINT key, BOOL pressed )
{
    static BOOL alt, shift, armed;
    BOOL is_alt = key == 56 /* KEY_LEFTALT */, is_shift = key == 42 || key == 54 /* KEY_LEFTSHIFT, KEY_RIGHTSHIFT */;

    if (!is_alt && !is_shift)
    {
        if (pressed) armed = FALSE;
        return FALSE;
    }
    if (pressed)
    {
        if (is_alt) alt = TRUE; else shift = TRUE;
        armed = alt && shift;
        return FALSE;
    }
    if (is_alt) alt = FALSE; else shift = FALSE;
    if (!armed) return FALSE;
    armed = FALSE;
    return TRUE;
}

static DWORD WINAPI raw_input_thread( void *arg )
{
    struct rit_event events[64];
    struct rit_read_params params = { .max = ARRAY_SIZE(events), .events = events };
    UINT repeat_key = 0;
    DWORD repeat_at = 0, now;
    NTSTATUS status;

    SetThreadDescription( GetCurrentThread(), L"RawInputThread" );
    SetThreadPriority( GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL );

    for (;;)
    {
        now = GetTickCount();
        params.timeout_ms = !repeat_key ? ~0u : (int)(repeat_at - now) > 0 ? repeat_at - now : 0;
        status = WINE_UNIX_CALL( unix_rit_read, &params );

        if (status == STATUS_TIMEOUT)
        {
            if (!repeat_key) continue;
            send_key( repeat_key, TRUE );
            repeat_at = GetTickCount() + repeat_interval();
            continue;
        }
        if (status)
        {
            ERR( "input stopped: %#lx\n", status );
            return status;
        }

        for (UINT i = 0; i < params.count; i++)
        {
            const struct rit_event *event = &events[i];

            switch (event->type)
            {
            case RIT_KEY:
            {
                HKL layout = InterlockedExchangePointer( (void **)&pending_layout, NULL );
                if (layout) ActivateKeyboardLayout( layout, 0 );
                if (event->value) follow_input_language();
                send_key( event->code, event->value );
                if (language_chord( event->code, event->value )) next_input_language();
                if (event->value)
                {
                    repeat_key = event->code;
                    repeat_at = GetTickCount() + repeat_delay();
                }
                else if (event->code == repeat_key) repeat_key = 0;
                break;
            }
            case RIT_BUTTON:
                send_button( event->code, event->value );
                break;
            case RIT_MOTION:
                send_mouse( MOUSEEVENTF_MOVE, event->x, event->y, 0 );
                break;
            case RIT_MOTION_ABSOLUTE:
                send_absolute( event->x, event->y );
                break;
            case RIT_WHEEL:
                send_mouse( MOUSEEVENTF_WHEEL, 0, 0, event->value );
                break;
            case RIT_HWHEEL:
                send_mouse( MOUSEEVENTF_HWHEEL, 0, 0, event->value );
                break;
            }
        }
        send_mouse( MOUSEEVENTF_MOVE_NOCOALESCE, 0, 0, 0 ); /* motion win32u holds back goes out now */
    }
}

void start_raw_input_thread(void)
{
    NTSTATUS status;
    HANDLE thread;

    if ((status = WINE_UNIX_CALL( unix_rit_init, NULL )))
    {
        ERR( "no input devices: %#lx\n", status );
        return;
    }
    if ((thread = CreateThread( NULL, 0, raw_input_thread, NULL, 0, NULL ))) CloseHandle( thread );
}
