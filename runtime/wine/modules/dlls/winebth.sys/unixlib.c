/*
 * winebluetooth Unix interface
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

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <stdlib.h>
#include <stdarg.h>
#include <assert.h>
#include <pthread.h>

#include <ntstatus.h>
#include <winternl.h>
#include <winbase.h>
#include <windef.h>
#include <wine/list.h>
#include <wine/rbtree.h>
#include <wine/debug.h>

#include "unixlib.h"
#include "unixlib_priv.h"

WINE_DEFAULT_DEBUG_CHANNEL( winebth );

static int compare_string( const void *key, const struct wine_rb_entry *entry )
{
    struct unix_name *str = WINE_RB_ENTRY_VALUE( entry, struct unix_name, entry );
    return strcmp( key, str->str );
}

static struct rb_tree names = { .compare = compare_string };
static pthread_mutex_t names_mutex = PTHREAD_MUTEX_INITIALIZER;

struct unix_name *unix_name_dup( struct unix_name *name )
{
    pthread_mutex_lock( &names_mutex );
    name->refcnt++;
    pthread_mutex_unlock( &names_mutex );
    return name;
}

struct unix_name *unix_name_get_or_create( const char *str )
{
    struct rb_entry *entry;
    struct unix_name *s;

    pthread_mutex_lock( &names_mutex );
    entry = rb_get( &names, str );
    if (!entry)
    {
        struct unix_name *s = malloc( sizeof( struct unix_name ) );
        if (!s)
        {
            pthread_mutex_unlock( &names_mutex );
            return NULL;
        }
        s->str = strdup( str );
        s->refcnt = 0;
        rb_put( &names, str, &s->entry );
        entry = &s->entry;
    }
    s = RB_ENTRY_VALUE( entry, struct unix_name, entry );
    s->refcnt++;
    pthread_mutex_unlock( &names_mutex );
    return s;
}

void unix_name_free( struct unix_name *name )
{
    pthread_mutex_lock( &names_mutex );
    name->refcnt--;
    if (name->refcnt == 0)
    {
        rb_remove( &names, &name->entry );
        free( name );
    }
    pthread_mutex_unlock( &names_mutex );
}

/* Arctic: everything goes to the kernel's Bluetooth drivers (hci.c) */
static BOOL started;

static NTSTATUS bluetooth_init( void *params )
{
    NTSTATUS status = hci_init();

    started = !status;
    return status;
}

static NTSTATUS bluetooth_shutdown( void *params )
{
    if (!started) return STATUS_NOT_SUPPORTED;
    hci_shutdown();
    started = FALSE;
    return STATUS_SUCCESS;
}

static NTSTATUS get_unique_name( const struct unix_name *name, char *buf, SIZE_T *buf_size )
{
    SIZE_T path_len, i;

    path_len = strlen( name->str );
    if (*buf_size <= (path_len * sizeof(char)))
    {
        *buf_size = (path_len + 1) * sizeof(char);
        return STATUS_BUFFER_TOO_SMALL;
    }

    for (i = 0; i < path_len; i++)
    {
        if (name->str[i] == '/') buf[i] = '_';
        else
            buf[i] = name->str[i];
    }
    buf[path_len] = '\0';
    return STATUS_SUCCESS;
}

static NTSTATUS bluetooth_adapter_get_unique_name( void *args )
{
    struct bluetooth_adapter_get_unique_name_params *params = args;
    if (!started) return STATUS_NOT_SUPPORTED;

    return get_unique_name( params->adapter, params->buf, &params->buf_size );
}

static NTSTATUS bluetooth_adapter_free( void *args )
{
    struct bluetooth_adapter_free_params *params = args;
    unix_name_free( params->adapter );
    return STATUS_SUCCESS;
}

static NTSTATUS bluetooth_adapter_dup( void *args )
{
    struct bluetooth_adapter_dup_params *params = args;
    unix_name_dup( params->adapter );
    return STATUS_SUCCESS;
}

static NTSTATUS bluetooth_adapter_set_prop( void *arg )
{
    if (!started) return STATUS_NOT_SUPPORTED;
    return hci_adapter_set_prop( arg );
}

static NTSTATUS bluetooth_device_free( void *args )
{
    struct bluetooth_device_free_params *params = args;
    unix_name_free( params->device );
    return STATUS_SUCCESS;
}

static NTSTATUS bluetooth_device_dup( void *args )
{
    struct bluetooth_device_dup_params *params = args;
    unix_name_dup( params->device );
    return STATUS_SUCCESS;
}

static NTSTATUS bluetooth_adapter_start_discovery( void *args )
{
    struct bluetooth_adapter_start_discovery_params *params = args;

    if (!started) return STATUS_NOT_SUPPORTED;
    return hci_adapter_start_discovery( params->adapter );
}

static NTSTATUS bluetooth_adapter_stop_discovery( void *args )
{
    struct bluetooth_adapter_stop_discovery_params *params = args;

    if (!started) return STATUS_NOT_SUPPORTED;
    return hci_adapter_stop_discovery( params->adapter );
}

static NTSTATUS bluetooth_adapter_remove_device( void *args )
{
    struct bluetooth_adapter_remove_device_params *params = args;

    if (!started) return STATUS_NOT_SUPPORTED;
    return hci_adapter_remove_device( params->adapter, params->device );
}

static NTSTATUS bluetooth_auth_agent_enable_incoming( void *args )
{
    /* the pairing requests of the controllers always come here */
    return started ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
}

static NTSTATUS bluetooth_auth_send_response( void *args )
{
    if (!started) return STATUS_NOT_SUPPORTED;
    return hci_auth_send_response( args );
}

static NTSTATUS bluetooth_device_disconnect( void *args )
{
    struct bluetooth_device_disconnect_params *params = args;

    if (!started) return STATUS_NOT_SUPPORTED;
    return hci_device_disconnect( params->device );
}

static NTSTATUS bluetooth_device_start_pairing( void *args )
{
    struct bluetooth_device_start_pairing_params *params = args;

    if (!started) return STATUS_NOT_SUPPORTED;
    return hci_device_start_pairing( params->device, params->irp );
}

static NTSTATUS bluetooth_gatt_service_free( void *args )
{
    struct bluetooth_gatt_service_free_params *params = args;
    unix_name_free( params->service );
    return STATUS_SUCCESS;
}

static NTSTATUS bluetooth_gatt_characteristic_free( void *args )
{
    struct bluetooth_gatt_characteristic_free_params *params = args;
    unix_name_free( params->characteristic );
    return STATUS_SUCCESS;
}

/* GATT (Bluetooth LE) is not there yet */
static NTSTATUS bluetooth_gatt_characteristic_read( void *args )
{
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS bluetooth_gatt_characteristic_value_move( void *args )
{
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS bluetooth_gatt_characteristic_value_free( void *args )
{
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS bluetooth_get_event( void *args )
{
    struct bluetooth_get_event_params *params = args;

    if (!started) return STATUS_NOT_SUPPORTED;
    memset( &params->result, 0, sizeof( params->result ) );
    return hci_get_event( &params->result );
}

static NTSTATUS bluetooth_adapter_set_power( void *args )
{
    struct bluetooth_adapter_set_power_params *params = args;

    if (!started) return STATUS_NOT_SUPPORTED;
    return hci_adapter_set_power( params->adapter, params->on );
}

static NTSTATUS bluetooth_adapter_load( void *args )
{
    if (!started) return STATUS_NOT_SUPPORTED;
    return hci_adapter_load( args );
}

static NTSTATUS bluetooth_device_connect( void *args )
{
    struct bluetooth_device_connect_params *params = args;

    if (!started) return STATUS_NOT_SUPPORTED;
    return hci_device_connect( params->device );
}

static NTSTATUS bluetooth_obex_send( void *args )
{
    struct bluetooth_obex_send_params *params = args;

    if (!started) return STATUS_NOT_SUPPORTED;
    return obex_send( params->address, params->path, params->irp );
}

static NTSTATUS bluetooth_obex_progress( void *args )
{
    struct bluetooth_obex_progress_params *params = args;

    obex_progress( &params->sent, &params->total );
    return STATUS_SUCCESS;
}

static NTSTATUS bluetooth_obex_cancel( void *args )
{
    obex_cancel();
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] = {
    bluetooth_init,
    bluetooth_shutdown,

    bluetooth_adapter_set_prop,
    bluetooth_adapter_get_unique_name,
    bluetooth_adapter_start_discovery,
    bluetooth_adapter_stop_discovery,
    bluetooth_adapter_remove_device,
    bluetooth_adapter_free,
    bluetooth_adapter_dup,

    bluetooth_device_free,
    bluetooth_device_dup,
    bluetooth_device_disconnect,
    bluetooth_device_start_pairing,

    bluetooth_auth_agent_enable_incoming,
    bluetooth_auth_send_response,

    bluetooth_gatt_service_free,

    bluetooth_gatt_characteristic_free,
    bluetooth_gatt_characteristic_read,

    bluetooth_gatt_characteristic_value_move,
    bluetooth_gatt_characteristic_value_free,

    bluetooth_get_event,

    bluetooth_adapter_set_power,
    bluetooth_adapter_load,
    bluetooth_device_connect,
    bluetooth_obex_send,
    bluetooth_obex_progress,
    bluetooth_obex_cancel,
};

C_ASSERT( ARRAYSIZE( __wine_unix_call_funcs ) == unix_funcs_count );
