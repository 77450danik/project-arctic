/*
 * Volume icon of the notification area: Bluetooth headphones
 *
 * As Windows does: the buttons of a headset (AVRCP, which winebth.sys
 * reports on \??\WINEBTHAUTH as GUID_WINEBTH_MEDIA_KEY) are the media and
 * volume keys, and headphones whose music has just connected become the
 * output programs play to.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS

#include "sndvolsso.h"
#include "winreg.h"
#include "objbase.h"
#include "shlobj.h"
#include "shlguid.h"
#include "winioctl.h"
#include "cfgmgr32.h"
#include "initguid.h"
#include "bthsdpdef.h"
#include "bluetoothapis.h"
#include "bthdef.h"
#include "bthledef.h"
#include "bthioctl.h"
#include "wine/winebth.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(sndvolsso);

static HCMNOTIFICATION notification;
static BTH_ADDR audio_device;      /* whose music the outputs follow */
static DWORD switch_until;         /* the headphones' output is looked for until then */
static DWORD mic_until;            /* their microphone until then */
static DWORD last_poll;

static DWORD CALLBACK media_key( HCMNOTIFICATION notify, void *context, CM_NOTIFY_ACTION action,
                                 CM_NOTIFY_EVENT_DATA *data, DWORD size )
{
    INPUT input[2] = { { .type = INPUT_KEYBOARD }, { .type = INPUT_KEYBOARD } };
    UINT vk;

    if (action != CM_NOTIFY_ACTION_DEVICECUSTOMEVENT ||
        !IsEqualGUID( &data->u.DeviceHandle.EventGuid, &GUID_WINEBTH_MEDIA_KEY ))
        return ERROR_SUCCESS;
    memcpy( &vk, data->u.DeviceHandle.Data, sizeof(vk) );
    TRACE( "headset button: key %#x\n", vk );
    input[0].ki.wVk = input[1].ki.wVk = vk;
    input[0].ki.dwFlags = input[1].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
    input[1].ki.dwFlags |= KEYEVENTF_KEYUP;
    SendInput( 2, input, sizeof(INPUT) );
    return ERROR_SUCCESS;
}

/* Send to → "Пристрій Bluetooth", as Windows adds it when the PC has Bluetooth */
static void make_sendto_link(void)
{
    BLUETOOTH_FIND_RADIO_PARAMS params = { sizeof(params) };
    HBLUETOOTH_RADIO_FIND find;
    WCHAR folder[MAX_PATH], path[MAX_PATH], target[MAX_PATH];
    IShellLinkW *link;
    IPersistFile *file;
    HANDLE radio;

    if (!(find = BluetoothFindFirstRadio( &params, &radio ))) return;
    CloseHandle( radio );
    BluetoothFindRadioClose( find );
    if (FAILED(SHGetFolderPathW( NULL, CSIDL_SENDTO, NULL, SHGFP_TYPE_CURRENT, folder ))) return;
    swprintf( path, ARRAY_SIZE(path), L"%s\\%s.lnk", folder, load_string( IDS_SENDTO_BLUETOOTH ) );
    if (GetFileAttributesW( path ) != INVALID_FILE_ATTRIBUTES) return;
    GetSystemDirectoryW( target, ARRAY_SIZE(target) );
    lstrcatW( target, L"\\fsquirt.exe" );
    if (FAILED(CoCreateInstance( &CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link ))) return;
    IShellLinkW_SetPath( link, target );
    IShellLinkW_SetIconLocation( link, target, 0 );
    if (SUCCEEDED(IShellLinkW_QueryInterface( link, &IID_IPersistFile, (void **)&file )))
    {
        IPersistFile_Save( file, path, TRUE );
        IPersistFile_Release( file );
    }
    IShellLinkW_Release( link );
}

void bluetooth_start(void)
{
    CM_NOTIFY_FILTER filter = { sizeof(filter) };
    HANDLE auth;

    make_sendto_link();
    auth = CreateFileW( WINEBTH_AUTH_DEVICE_PATH, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, 0, NULL );
    if (auth == INVALID_HANDLE_VALUE) return;
    filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEHANDLE;
    filter.u.DeviceHandle.hTarget = auth;
    if (CM_Register_Notification( &filter, NULL, media_key, &notification )) notification = NULL;
    CloseHandle( auth );
}

void bluetooth_stop(void)
{
    if (notification) CM_Unregister_Notification( notification );
    notification = NULL;
}

/* the device whose music is connected, on any radio; 0: none */
static BTH_ADDR connected_audio( WCHAR *name, size_t size )
{
    BLUETOOTH_FIND_RADIO_PARAMS params = { sizeof(params) };
    HBLUETOOTH_RADIO_FIND find;
    BTH_ADDR found = 0;
    HANDLE radio;

    if (!(find = BluetoothFindFirstRadio( &params, &radio ))) return 0;
    do
    {
        struct winebth_radio_state state;
        DWORD bytes;

        if (!found && DeviceIoControl( radio, IOCTL_WINEBTH_RADIO_GET_STATE, NULL, 0, &state, sizeof(state), &bytes,
                                       NULL ) && state.audio_device)
        {
            BLUETOOTH_DEVICE_INFO info = { sizeof(info) };

            found = state.audio_device;
            info.Address.ullLong = found;
            if (name && !BluetoothGetDeviceInfo( radio, &info )) lstrcpynW( name, info.szName, size );
        }
        CloseHandle( radio );
    } while (BluetoothFindNextRadio( find, &radio ));
    BluetoothFindRadioClose( find );
    return found;
}

/* every second or two, from the icon's timer */
void bluetooth_poll(void)
{
    struct endpoint *list;
    WCHAR name[BLUETOOTH_MAX_NAME_SIZE] = L"";
    BTH_ADDR now;
    UINT count;

    if (GetTickCount() - last_poll < 2000) return;
    last_poll = GetTickCount();
    now = connected_audio( name, ARRAY_SIZE(name) );
    if (now != audio_device)
    {
        audio_device = now;
        switch_until = now ? GetTickCount() + 15000 : 0;
        mic_until = now ? GetTickCount() + 60000 : 0;
    }
    /* the headset's microphone comes with hands-free, maybe later than its music */
    if (mic_until && name[0] && (list = calloc( MAX_ENDPOINTS, sizeof(*list) )))
    {
        count = audio_capture_endpoints( list, MAX_ENDPOINTS );
        for (UINT i = 0; i < count; i++)
        {
            if (!wcsstr( list[i].name, name )) continue;
            TRACE( "the headset %s is the microphone now\n", debugstr_w( name ) );
            audio_set_default_input( list[i].id );
            mic_until = 0;
            break;
        }
        free( list );
        if (mic_until && (int)(GetTickCount() - mic_until) > 0) mic_until = 0;
    }
    if (!switch_until || !name[0]) return;
    if ((int)(GetTickCount() - switch_until) > 0)
    {
        switch_until = 0;
        return;
    }

    /* the output appears once the sound system has looked again */
    if (!(list = calloc( MAX_ENDPOINTS, sizeof(*list) ))) return;
    count = audio_endpoints( list, MAX_ENDPOINTS );
    for (UINT i = 0; i < count; i++)
    {
        if (!wcsstr( list[i].name, name )) continue;
        TRACE( "the headphones %s are the output now\n", debugstr_w( name ) );
        audio_set_default( list[i].id );
        switch_until = 0;
        tray_update();
        break;
    }
    free( list );
}
