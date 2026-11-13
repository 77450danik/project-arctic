/*
 * Devices and Printers: the radio and its devices
 *
 * Through Windows' own interface (bluetoothapis.dll) and, for what Windows'
 * Settings does through interfaces of its own, the IOCTLs of winebth.sys:
 * the radio on or off, a paired device's music connected.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <stdlib.h>

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "winioctl.h"
#include "bthsdpdef.h"
#include "bluetoothapis.h"
#include "bthdef.h"
#include "bthledef.h"
#include "bthioctl.h"
#include "wine/winebth.h"

#include "devicescpl.h"

HANDLE bt_open_radio(void)
{
    BLUETOOTH_FIND_RADIO_PARAMS params = { sizeof(params) };
    HBLUETOOTH_RADIO_FIND find;
    HANDLE radio = NULL;

    if (!(find = BluetoothFindFirstRadio( &params, &radio ))) return NULL;
    BluetoothFindRadioClose( find );
    return radio;
}

BOOL bt_radio_state( HANDLE radio, BOOL *powered, BOOL *discovering, BTH_ADDR *audio )
{
    struct winebth_radio_state state;
    DWORD bytes;

    if (!radio || !DeviceIoControl( radio, IOCTL_WINEBTH_RADIO_GET_STATE, NULL, 0, &state, sizeof(state), &bytes, NULL ))
        return FALSE;
    if (powered) *powered = state.powered;
    if (discovering) *discovering = state.discovering;
    if (audio) *audio = state.audio_device;
    return TRUE;
}

BOOL bt_set_power( HANDLE radio, BOOL on )
{
    DWORD bytes;

    return radio && DeviceIoControl( radio, IOCTL_WINEBTH_RADIO_SET_POWER, &on, sizeof(on), NULL, 0, &bytes, NULL );
}

void bt_discovery( HANDLE radio, BOOL on )
{
    DWORD bytes;

    if (radio)
        DeviceIoControl( radio, on ? IOCTL_WINEBTH_RADIO_START_DISCOVERY : IOCTL_WINEBTH_RADIO_STOP_DISCOVERY, NULL, 0,
                         NULL, 0, &bytes, NULL );
}

void bt_radio_name( HANDLE radio, WCHAR *name, UINT size )
{
    BLUETOOTH_RADIO_INFO info = { sizeof(info) };

    name[0] = 0;
    if (radio && !BluetoothGetRadioInfo( radio, &info )) lstrcpynW( name, info.szName, size );
}

/* the batteries a connected device has told of: its own, its earbuds' and their case's */
BOOL bt_battery( HANDLE radio, BTH_ADDR address, struct bt_battery *battery )
{
    DWORD bytes;
    union { BTH_ADDR address; struct winebth_device_battery battery; } io;

    battery->level = battery->left = battery->right = battery->case_level = BT_BATTERY_UNKNOWN;
    battery->charging = 0;
    io.address = address;
    if (!radio || !DeviceIoControl( radio, IOCTL_WINEBTH_RADIO_GET_BATTERY, &io, sizeof(io), &io, sizeof(io), &bytes,
                                    NULL ) || bytes < sizeof(io.battery))
        return FALSE;
    battery->level = io.battery.level;
    battery->left = io.battery.left;
    battery->right = io.battery.right;
    battery->case_level = io.battery.case_level;
    battery->charging = io.battery.charging;
    return TRUE;
}

/* the paired devices, or those found and not paired; named ones only */
UINT bt_devices( HANDLE radio, struct bt_device *list, UINT max, BOOL paired )
{
    BLUETOOTH_DEVICE_SEARCH_PARAMS search = { sizeof(search), TRUE, TRUE, TRUE, TRUE, FALSE, 0, radio };
    BLUETOOTH_DEVICE_INFO info = { sizeof(info) };
    HBLUETOOTH_DEVICE_FIND find;
    BTH_ADDR audio = 0;
    UINT count = 0;

    if (!radio || !(find = BluetoothFindFirstDevice( &search, &info ))) return 0;
    bt_radio_state( radio, NULL, NULL, &audio );
    do
    {
        if (!info.fAuthenticated == !paired) continue;
        if (!paired && !info.szName[0]) continue;
        list[count].info = info;
        list[count].audio = audio && info.Address.ullLong == audio;
        bt_battery( radio, info.Address.ullLong, &list[count].battery );
        count++;
    } while (count < max && (info.dwSize = sizeof(info), BluetoothFindNextDevice( find, &info )));
    BluetoothFindDeviceClose( find );
    return count;
}

/* on a thread of its own: the radio answers when the device has */
static DWORD WINAPI connect_proc( void *arg )
{
    BTH_ADDR address = *(BTH_ADDR *)arg;
    HANDLE radio = bt_open_radio();
    DWORD bytes;

    free( arg );
    if (!radio) return 0;
    DeviceIoControl( radio, IOCTL_WINEBTH_RADIO_CONNECT_DEVICE, &address, sizeof(address), NULL, 0, &bytes, NULL );
    CloseHandle( radio );
    return 0;
}

static DWORD WINAPI disconnect_proc( void *arg )
{
    BTH_ADDR address = *(BTH_ADDR *)arg;
    HANDLE radio = bt_open_radio();
    DWORD bytes;

    free( arg );
    if (!radio) return 0;
    DeviceIoControl( radio, IOCTL_BTH_DISCONNECT_DEVICE, &address, sizeof(address), NULL, 0, &bytes, NULL );
    CloseHandle( radio );
    return 0;
}

static void run_with_address( LPTHREAD_START_ROUTINE proc, BTH_ADDR address )
{
    BTH_ADDR *arg = malloc( sizeof(*arg) );
    HANDLE thread;

    if (!arg) return;
    *arg = address;
    if ((thread = CreateThread( NULL, 0, proc, arg, 0, NULL ))) CloseHandle( thread );
    else free( arg );
}

void bt_connect( BTH_ADDR address )
{
    run_with_address( connect_proc, address );
}

void bt_disconnect( BTH_ADDR address )
{
    run_with_address( disconnect_proc, address );
}

DWORD bt_remove( BTH_ADDR address )
{
    BLUETOOTH_ADDRESS addr = { .ullLong = address };

    return BluetoothRemoveDevice( &addr );
}

/* for the flyouts of the notification area (pnidui.dll) */
void WINAPI ArcticBluetoothConnect( BTH_ADDR address, BOOL connect )
{
    if (connect) bt_connect( address );
    else bt_disconnect( address );
}
