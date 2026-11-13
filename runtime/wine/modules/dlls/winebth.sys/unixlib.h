/*
 * Unix interface definitions
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

#ifndef __WINE_WINEBTH_UNIXLIB_H
#define __WINE_WINEBTH_UNIXLIB_H

#include <stdlib.h>
#include <stdarg.h>

#include <windef.h>
#include <winbase.h>

#include <bthsdpdef.h>
#include <bluetoothapis.h>

#include <wine/unixlib.h>
#include <wine/debug.h>

#include "winebth_priv.h"

#ifdef WINE_UNIX_LIB
typedef struct unix_name *unix_name_t;
typedef void *unix_handle_t;
#else
typedef UINT_PTR unix_name_t;
typedef UINT_PTR unix_handle_t;
#endif

struct bluetooth_adapter_free_params
{
    unix_name_t adapter;
};

struct bluetooth_adapter_dup_params
{
    unix_name_t adapter;
};

struct bluetooth_device_free_params
{
    unix_name_t device;
};

struct bluetooth_device_dup_params
{
    unix_name_t device;
};

struct bluetooth_gatt_service_free_params
{
    unix_name_t service;
};

struct bluetooth_gatt_characteristic_dup_params
{
    unix_name_t characteristic;
};

struct bluetooth_gatt_characteristic_free_params
{
    unix_name_t characteristic;
};

struct bluetooth_gatt_characteristic_value_move_params
{
    struct winebluetooth_gatt_characteristic_value *val;
    BYTE *buf;
};

struct bluetooth_gatt_characteristic_value_free_params
{
    UINT_PTR handle;
};

struct bluetooth_device_disconnect_params
{
    unix_name_t device;
};

struct bluetooth_adapter_get_unique_name_params
{
    unix_name_t adapter;

    char *buf;
    SIZE_T buf_size;
};

struct bluetooth_adapter_set_prop_params
{
    unix_name_t adapter;
    ULONG prop_flag;

    union winebluetooth_property *prop;
};

struct bluetooth_adapter_start_discovery_params
{
    unix_name_t adapter;
};

struct bluetooth_adapter_stop_discovery_params
{
    unix_name_t adapter;
};

struct bluetooth_adapter_remove_device_params
{
    unix_name_t adapter;
    unix_name_t device;
};

struct bluetooth_auth_send_response_params
{
    unix_name_t device;
    BLUETOOTH_AUTHENTICATION_METHOD method;
    UINT32 numeric_or_passkey;
    BYTE pin[16];                /* the legacy PIN */
    BYTE pin_length;
    BOOL negative;
    BOOL *authenticated;
};

/* Arctic */
struct bluetooth_adapter_set_power_params
{
    unix_name_t adapter;
    BOOL on;
};

struct bluetooth_adapter_load_params
{
    unix_name_t adapter;
    CHAR name[BLUETOOTH_MAX_NAME_SIZE];             /* the computer's */
    BOOL powered;
    UINT32 count;
    const struct winebluetooth_known_device *devices;
};

struct bluetooth_device_connect_params
{
    unix_name_t device;
};

struct bluetooth_obex_send_params
{
    BYTE address[6];             /* least significant byte first, as the kernel has it */
    char path[1024];             /* the file, its Unix path */
    IRP *irp;
};

struct bluetooth_obex_progress_params
{
    UINT64 sent, total;
};

struct bluetooth_device_start_pairing_params
{
    unix_name_t device;
    IRP *irp;
};

struct bluetooth_gatt_characteristic_read_params
{
    unix_name_t chrc;
    IRP *irp;
};

struct bluetooth_get_event_params
{
    struct winebluetooth_event result;
};

enum bluetoothapis_funcs
{
    unix_bluetooth_init,
    unix_bluetooth_shutdown,

    unix_bluetooth_adapter_set_prop,
    unix_bluetooth_adapter_get_unique_name,
    unix_bluetooth_adapter_start_discovery,
    unix_bluetooth_adapter_stop_discovery,
    unix_bluetooth_adapter_remove_device,
    unix_bluetooth_adapter_free,
    unix_bluetooth_adapter_dup,

    unix_bluetooth_device_free,
    unix_bluetooth_device_dup,
    unix_bluetooth_device_disconnect,
    unix_bluetooth_device_start_pairing,

    unix_bluetooth_auth_agent_enable_incoming,
    unix_bluetooth_auth_send_response,

    unix_bluetooth_gatt_service_free,

    unix_bluetooth_gatt_characteristic_free,
    unix_bluetooth_gatt_characteristic_read,

    unix_bluetooth_gatt_characteristic_value_move,
    unix_bluetooth_gatt_characteristic_value_free,

    unix_bluetooth_get_event,

    /* Arctic */
    unix_bluetooth_adapter_set_power,
    unix_bluetooth_adapter_load,
    unix_bluetooth_device_connect,
    unix_bluetooth_obex_send,
    unix_bluetooth_obex_progress,
    unix_bluetooth_obex_cancel,

    unix_funcs_count
};

#define UNIX_BLUETOOTH_CALL( func, params ) WINE_UNIX_CALL( unix_##func, params )

#endif /* __WINE_WINEBTH_UNIXLIB_H */
