/*
 * Network icon of the notification area: Bluetooth in its flyout
 *
 * As Windows' quick actions have it: Bluetooth switched on and off beside
 * Wi-Fi, and the paired devices listed with the connections, each to connect
 * or disconnect. The radio is winebth.sys's, through bluetoothapis.dll; the
 * pictures and the connecting are devicescpl.dll's (Devices and Printers).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "winioctl.h"
#include "bthsdpdef.h"
#include "bluetoothapis.h"
#include "bthdef.h"
#include "bthledef.h"
#include "bthioctl.h"
#include "wine/winebth.h"
#include "wlanapi.h"

#include "pnidui.h"

static HANDLE open_radio(void)
{
    BLUETOOTH_FIND_RADIO_PARAMS params = { sizeof(params) };
    HBLUETOOTH_RADIO_FIND find;
    HANDLE radio = NULL;

    if (!(find = BluetoothFindFirstRadio( &params, &radio ))) return NULL;
    BluetoothFindRadioClose( find );
    return radio;
}

/* whether there is a radio, and whether it is on */
BOOL bt_state( BOOL *on )
{
    struct winebth_radio_state state;
    HANDLE radio = open_radio();
    DWORD bytes;
    BOOL ret;

    *on = FALSE;
    if (!radio) return FALSE;
    ret = DeviceIoControl( radio, IOCTL_WINEBTH_RADIO_GET_STATE, NULL, 0, &state, sizeof(state), &bytes, NULL );
    if (ret) *on = state.powered;
    CloseHandle( radio );
    return ret;
}

void bt_set_power( BOOL on )
{
    HANDLE radio = open_radio();
    DWORD bytes;

    if (!radio) return;
    DeviceIoControl( radio, IOCTL_WINEBTH_RADIO_SET_POWER, &on, sizeof(on), NULL, 0, &bytes, NULL );
    CloseHandle( radio );
}

UINT bt_paired( struct bt_entry *list, UINT max )
{
    BLUETOOTH_DEVICE_SEARCH_PARAMS search = { sizeof(search), TRUE, TRUE, FALSE, TRUE, FALSE, 0 };
    BLUETOOTH_DEVICE_INFO info = { sizeof(info) };
    struct winebth_radio_state state = {0};
    HBLUETOOTH_DEVICE_FIND find;
    UINT count = 0;
    DWORD bytes;

    if (!(search.hRadio = open_radio())) return 0;
    DeviceIoControl( search.hRadio, IOCTL_WINEBTH_RADIO_GET_STATE, NULL, 0, &state, sizeof(state), &bytes, NULL );
    if ((find = BluetoothFindFirstDevice( &search, &info )))
    {
        do
        {
            if (!info.fAuthenticated) continue;
            lstrcpynW( list[count].name, info.szName, ARRAY_SIZE(list[count].name) );
            list[count].address = info.Address.ullLong;
            list[count].class = info.ulClassofDevice;
            list[count].audio = state.audio_device && state.audio_device == info.Address.ullLong;
            list[count].connected = info.fConnected || list[count].audio;
            /* its batteries: its own, its earbuds', their case's, and which charge */
            memset( list[count].battery, WINEBTH_BATTERY_UNKNOWN, 4 );
            list[count].battery[4] = 0;
            if (list[count].connected)
            {
                union { BTH_ADDR address; struct winebth_device_battery battery; } io = { info.Address.ullLong };

                if (DeviceIoControl( search.hRadio, IOCTL_WINEBTH_RADIO_GET_BATTERY, &io, sizeof(io), &io, sizeof(io),
                                     &bytes, NULL ) && bytes >= sizeof(io.battery))
                {
                    list[count].battery[0] = io.battery.level;
                    list[count].battery[1] = io.battery.left;
                    list[count].battery[2] = io.battery.right;
                    list[count].battery[3] = io.battery.case_level;
                    list[count].battery[4] = io.battery.charging;
                }
            }
            count++;
        } while (count < max && (info.dwSize = sizeof(info), BluetoothFindNextDevice( find, &info )));
        BluetoothFindDeviceClose( find );
    }
    CloseHandle( search.hRadio );
    return count;
}

static FARPROC devicescpl( const char *name )
{
    static HMODULE module;

    if (!module) module = LoadLibraryW( L"devicescpl.dll" );
    return module ? GetProcAddress( module, name ) : NULL;
}

void bt_connect( ULONGLONG address, BOOL connect )
{
    void (WINAPI *proc)( ULONGLONG, BOOL ) = (void *)devicescpl( "ArcticBluetoothConnect" );

    if (proc) proc( address, connect );
}

/* the batteries as rings against the right edge of rect (devicescpl.dll draws them); their width, 0: none */
int bt_battery_width( const BYTE battery[5] )
{
    int (WINAPI *proc)( const BYTE * ) = (void *)devicescpl( "ArcticBatteryRingsWidth" );

    return proc ? proc( battery ) : 0;
}

void bt_draw_battery( HDC hdc, const BYTE battery[5], const RECT *rect, BOOL dark, HFONT font )
{
    void (WINAPI *proc)( HDC, const BYTE *, const RECT *, BOOL, HFONT ) = (void *)devicescpl( "ArcticDrawBatteryRings" );

    if (proc) proc( hdc, battery, rect, dark, font );
}

/* the device's picture in a colour, drawn at x, y */
void bt_draw_icon( HDC hdc, UINT32 class, int x, int y, int size, COLORREF color )
{
    HICON (WINAPI *proc)( UINT32, int, COLORREF ) = (void *)devicescpl( "ArcticBluetoothDeviceIcon" );
    HICON icon;

    if (!proc || !(icon = proc( class, size, color ))) return;
    DrawIconEx( hdc, x, y, icon, size, size, 0, NULL, DI_NORMAL );
    DestroyIcon( icon );
}
