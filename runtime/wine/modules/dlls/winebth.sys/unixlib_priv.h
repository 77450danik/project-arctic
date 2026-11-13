/*
 * Bluetoothapis Unix interface
 *
 * Copyright 2024 Vibhav Pant
 * Copyright 2025 Vibhav Pant
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifndef __WINE_WINEBTH_UNIXLIB_PRIV_H
#define __WINE_WINEBTH_UNIXLIB_PRIV_H

#include <ntstatus.h>
#include <windef.h>

#include <wine/list.h>
#include <wine/rbtree.h>

#include "unixlib.h"

struct unix_name
{
    char *str;
    SIZE_T refcnt;

    struct wine_rb_entry entry;
};

extern void unix_name_free( struct unix_name *name );
extern struct unix_name *unix_name_get_or_create( const char *str ) __WINE_DEALLOC(unix_name_free) __WINE_MALLOC;
extern struct unix_name *unix_name_dup( struct unix_name *name ) __WINE_DEALLOC(unix_name_free) __WINE_MALLOC;

/* Arctic: the backend talks to the kernel's Bluetooth drivers itself, not to
 * BlueZ (docs/bluetooth.md): the management channel of the controllers
 * (hci.c), the profiles over L2CAP: SDP, A2DP and AVRCP (profiles.c) */
extern NTSTATUS hci_init( void );
extern void hci_shutdown( void );
extern NTSTATUS hci_get_event( struct winebluetooth_event *result );
extern void hci_post_event( const struct winebluetooth_event *event );
extern NTSTATUS hci_adapter_set_prop( struct bluetooth_adapter_set_prop_params *params );
extern NTSTATUS hci_adapter_set_power( struct unix_name *adapter, BOOL on );
extern NTSTATUS hci_adapter_load( struct bluetooth_adapter_load_params *params );
extern NTSTATUS hci_adapter_start_discovery( struct unix_name *adapter );
extern NTSTATUS hci_adapter_stop_discovery( struct unix_name *adapter );
extern NTSTATUS hci_adapter_remove_device( struct unix_name *adapter, struct unix_name *device );
extern NTSTATUS hci_device_disconnect( struct unix_name *device );
extern NTSTATUS hci_device_start_pairing( struct unix_name *device, IRP *irp );
extern NTSTATUS hci_device_connect( struct unix_name *device );
extern NTSTATUS hci_auth_send_response( struct bluetooth_auth_send_response_params *params );

/* what the profiles learn from and tell the controller's side (hci.c) */
struct bt_device_info
{
    char name[248];
    UINT32 class;
    BOOL paired, connected;
};
extern BOOL hci_device_info( const BYTE addr[6], struct bt_device_info *info );
extern void hci_device_set_audio( const BYTE addr[6], BOOL audio );
extern void hci_device_set_battery( const BYTE addr[6], int level );
extern void hci_media_key( UINT vk );

/* obex.c: files sent to a device */
extern NTSTATUS obex_send( const BYTE addr[6], const char *path, IRP *irp );
extern void obex_progress( UINT64 *done, UINT64 *size );
extern void obex_cancel( void );
extern int sdp_rfcomm_channel( const void *addr, UINT16 uuid );

/* profiles.c */
extern NTSTATUS profiles_init( void );
extern void profiles_shutdown( void );
extern NTSTATUS a2dp_connect( const BYTE addr[6] );
extern void profiles_disconnect( const BYTE addr[6] );
extern void profiles_device_gone( const BYTE addr[6] );
extern void audio_state_changed( void );

/* hfp.c: the headset's microphone and battery (Hands-Free, Audio Gateway) */
#define HFP_CHANNEL 13
extern NTSTATUS hfp_init( void );
extern void hfp_shutdown( void );
extern void hfp_connect( const BYTE addr[6] );
extern void hfp_device_gone( const BYTE addr[6] );
extern BOOL hfp_connected( const BYTE addr[6] );
extern BOOL hfp_voice_active( void );

#endif /* __WINE_WINEBTH_UNIXLIB_PRIV_H */
