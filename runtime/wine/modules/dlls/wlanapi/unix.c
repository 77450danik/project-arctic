/*
 * WLAN API: the unix side, iwd over the system bus
 *
 * iwd associates and does WPA; everything the Windows side decides (which
 * network, with which key) comes in through these calls. A key is handed to
 * iwd as a network file in its state folder, which arctic-init gives to the
 * Windows world: then iwd knows the network and connects without an agent.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <systemd/sd-bus.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "wine/debug.h"

#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(wlanapi);

#define IWD "net.connman.iwd"
#define IWD_STATE_DIR "/var/lib/iwd"

static sd_bus *bus;
/* one call at a time on that connection */
static pthread_mutex_t bus_mutex = PTHREAD_MUTEX_INITIALIZER;

static sd_bus *get_bus(void)
{
    if (!bus && sd_bus_open_system( &bus ) < 0)
    {
        ERR( "no system bus\n" );
        bus = NULL;
    }
    return bus;
}

static NTSTATUS bus_error( int r )
{
    if (r == -ETIMEDOUT) return STATUS_TIMEOUT;
    if (r == -EACCES || r == -EPERM) return STATUS_ACCESS_DENIED;
    return STATUS_UNSUCCESSFUL;
}

static int get_string( const char *path, const char *iface, const char *name, char *buf, size_t size )
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    char *value = NULL;
    int r = sd_bus_get_property_string( get_bus(), IWD, path, iface, name, &error, &value );

    sd_bus_error_free( &error );
    if (r < 0) return r;
    snprintf( buf, size, "%s", value );
    free( value );
    return 0;
}

/* a property that is an object path (type o), which get_string cannot read */
static int get_object( const char *path, const char *iface, const char *name, char *buf, size_t size )
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    const char *value = NULL;
    int r = sd_bus_get_property( get_bus(), IWD, path, iface, name, &error, &reply, "o" );

    sd_bus_error_free( &error );
    if (r >= 0) r = sd_bus_message_read( reply, "o", &value );
    if (r >= 0) snprintf( buf, size, "%s", value );
    sd_bus_message_unref( reply );
    return r < 0 ? r : 0;
}

static int get_bool( const char *path, const char *iface, const char *name )
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int value = 0;

    sd_bus_get_property_trivial( get_bus(), IWD, path, iface, name, &error, 'b', &value );
    sd_bus_error_free( &error );
    return value;
}

/* "Intel Wi-Fi 6 AX201", the adapter from the id databases of the host */
static void adapter_description( const char *name, char *buf, size_t size )
{
    char path[256], line[256], *model;
    unsigned int vendor = 0, device = 0, id;
    int in_vendor = 0;
    FILE *f;

    snprintf( buf, size, "%s", name );
    snprintf( path, sizeof(path), "/sys/class/net/%s/device/vendor", name );
    if ((f = fopen( path, "r" ))) { if (fscanf( f, "%x", &vendor ) != 1) vendor = 0; fclose( f ); }
    snprintf( path, sizeof(path), "/sys/class/net/%s/device/device", name );
    if ((f = fopen( path, "r" ))) { if (fscanf( f, "%x", &device ) != 1) device = 0; fclose( f ); }
    if (!vendor || !(f = fopen( "/usr/share/hwdata/pci.ids", "r" ))) return;
    while (fgets( line, sizeof(line), f ))
    {
        if (line[0] == '#' || line[0] == '\n') continue;
        if (line[0] != '\t')
        {
            in_vendor = strtoul( line, NULL, 16 ) == vendor;
            continue;
        }
        if (!in_vendor || line[1] == '\t') continue;
        id = strtoul( line + 1, &model, 16 );
        if (id != device) continue;
        while (*model == ' ') model++;
        model[strcspn( model, "\n" )] = 0;
        snprintf( buf, size, "%s", model );
        break;
    }
    fclose( f );
}

static enum wlan_unix_state parse_state( const char *state )
{
    if (!strcmp( state, "connected" )) return WLAN_UNIX_CONNECTED;
    if (!strcmp( state, "connecting" )) return WLAN_UNIX_CONNECTING;
    if (!strcmp( state, "disconnecting" )) return WLAN_UNIX_DISCONNECTING;
    if (!strcmp( state, "roaming" )) return WLAN_UNIX_ROAMING;
    return WLAN_UNIX_DISCONNECTED;
}

/* the ordered networks of a station: (network object, signal in 100 * dBm) */
static int ordered_networks( const char *path, char paths[][128], INT32 *signals, UINT32 max, UINT32 *count )
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    const char *network;
    int16_t signal;
    int r;

    *count = 0;
    r = sd_bus_call_method( get_bus(), IWD, path, IWD ".Station", "GetOrderedNetworks", &error, &reply, "" );
    sd_bus_error_free( &error );
    if (r < 0) return r;
    if ((r = sd_bus_message_enter_container( reply, 'a', "(on)" )) < 0) goto done;
    while (*count < max && (r = sd_bus_message_read( reply, "(on)", &network, &signal )) > 0)
    {
        snprintf( paths[*count], 128, "%s", network );
        signals[*count] = signal;
        (*count)++;
    }
done:
    sd_bus_message_unref( reply );
    return r < 0 ? r : 0;
}

static NTSTATUS wlan_interfaces( void *args )
{
    struct wlan_interfaces_params *params = args;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    const char *path;
    int r;

    params->count = 0;
    if (!get_bus()) return STATUS_NOT_SUPPORTED;
    r = sd_bus_call_method( bus, IWD, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
                            &error, &reply, "" );
    sd_bus_error_free( &error );
    if (r < 0) return bus_error( r );

    /* a{oa{sa{sv}}}: the objects that are stations */
    if (sd_bus_message_enter_container( reply, 'a', "{oa{sa{sv}}}" ) < 0) goto done;
    while (params->count < WLAN_MAX_INTERFACES &&
           sd_bus_message_enter_container( reply, 'e', "oa{sa{sv}}" ) > 0)
    {
        const char *iface;
        int station = 0;

        sd_bus_message_read( reply, "o", &path );
        sd_bus_message_enter_container( reply, 'a', "{sa{sv}}" );
        while (sd_bus_message_enter_container( reply, 'e', "sa{sv}" ) > 0)
        {
            sd_bus_message_read( reply, "s", &iface );
            if (!strcmp( iface, IWD ".Station" )) station = 1;
            sd_bus_message_skip( reply, "a{sv}" );
            sd_bus_message_exit_container( reply );
        }
        sd_bus_message_exit_container( reply );
        sd_bus_message_exit_container( reply );

        if (station)
        {
            struct wlan_unix_interface *info = &params->interfaces[params->count++];
            char state[32] = "", network[128] = "";

            memset( info, 0, sizeof(*info) );
            snprintf( info->path, sizeof(info->path), "%s", path );
            get_string( path, IWD ".Device", "Name", info->name, sizeof(info->name) );
            adapter_description( info->name, info->description, sizeof(info->description) );
            get_string( path, IWD ".Station", "State", state, sizeof(state) );
            info->state = parse_state( state );
            info->scanning = get_bool( path, IWD ".Station", "Scanning" );
            if (info->state != WLAN_UNIX_DISCONNECTED &&
                !get_object( path, IWD ".Station", "ConnectedNetwork", network, sizeof(network) ))
            {
                char paths[WLAN_MAX_NETWORKS][128];
                INT32 signals[WLAN_MAX_NETWORKS];
                UINT32 count;

                get_string( network, IWD ".Network", "Name", info->ssid, sizeof(info->ssid) );
                if (!ordered_networks( path, paths, signals, WLAN_MAX_NETWORKS, &count ))
                    for (UINT32 i = 0; i < count; i++)
                        if (!strcmp( paths[i], network )) info->signal = signals[i] / 100;
            }
        }
    }
done:
    sd_bus_message_unref( reply );
    return STATUS_SUCCESS;
}

static NTSTATUS wlan_networks( void *args )
{
    struct wlan_networks_params *params = args;
    char paths[WLAN_MAX_NETWORKS][128];
    INT32 signals[WLAN_MAX_NETWORKS];
    UINT32 count;
    int r;

    params->count = 0;
    if (!get_bus()) return STATUS_NOT_SUPPORTED;
    if ((r = ordered_networks( params->path, paths, signals, WLAN_MAX_NETWORKS, &count ))) return bus_error( r );
    for (UINT32 i = 0; i < count; i++)
    {
        struct wlan_unix_network *net = &params->networks[params->count];
        char type[16] = "", known[128] = "";

        memset( net, 0, sizeof(*net) );
        if (get_string( paths[i], IWD ".Network", "Name", net->ssid, sizeof(net->ssid) )) continue;
        get_string( paths[i], IWD ".Network", "Type", type, sizeof(type) );
        net->security = !strcmp( type, "psk" ) ? WLAN_UNIX_PSK : !strcmp( type, "8021x" ) ? WLAN_UNIX_8021X :
                        !strcmp( type, "wep" ) ? WLAN_UNIX_WEP : WLAN_UNIX_OPEN;
        net->signal = signals[i] / 100;
        net->connected = get_bool( paths[i], IWD ".Network", "Connected" );
        net->known = !get_object( paths[i], IWD ".Network", "KnownNetwork", known, sizeof(known) ) && known[0];
        params->count++;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS call_station( const char *path, const char *method )
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int r;

    if (!get_bus()) return STATUS_NOT_SUPPORTED;
    r = sd_bus_call_method( bus, IWD, path, IWD ".Station", method, &error, NULL, "" );
    /* a scan already running is as good as a new one */
    if (r < 0 && sd_bus_error_has_name( &error, IWD ".Busy" )) r = 0;
    if (r < 0) WARN( "%s %s: %s\n", method, path, error.message );
    sd_bus_error_free( &error );
    return r < 0 ? bus_error( r ) : STATUS_SUCCESS;
}

static NTSTATUS wlan_scan( void *args )
{
    const struct wlan_device_params *params = args;
    return call_station( params->path, "Scan" );
}

static NTSTATUS wlan_disconnect( void *args )
{
    const struct wlan_device_params *params = args;
    return call_station( params->path, "Disconnect" );
}

/* iwd's file name for a network: the SSID when it is plain, else =hex */
static void network_file( const char *ssid, const char *ext, char *buf, size_t size )
{
    int plain = *ssid != 0;
    size_t len;

    for (const char *p = ssid; *p; p++)
        if (!isalnum( (unsigned char)*p ) && *p != ' ' && *p != '_' && *p != '-') plain = 0;
    if (plain)
    {
        snprintf( buf, size, IWD_STATE_DIR "/%s.%s", ssid, ext );
        return;
    }
    len = snprintf( buf, size, IWD_STATE_DIR "/=" );
    for (const char *p = ssid; *p && len + 3 < size; p++) len += snprintf( buf + len, size - len, "%02x", (unsigned char)*p );
    snprintf( buf + len, size - len, ".%s", ext );
}

static BOOL write_psk_file( const char *ssid, const char *passphrase )
{
    char path[512], tmp[520];
    FILE *f;

    network_file( ssid, "psk", path, sizeof(path) );
    snprintf( tmp, sizeof(tmp), "%s.new", path );
    if (!(f = fopen( tmp, "w" )))
    {
        ERR( "cannot write %s: %s\n", tmp, strerror( errno ) );
        return FALSE;
    }
    fprintf( f, "[Security]\nPassphrase=%s\n", passphrase );
    fclose( f );
    chmod( tmp, 0600 );
    return !rename( tmp, path );
}

static NTSTATUS wlan_connect( void *args )
{
    const struct wlan_connect_params *params = args;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    char paths[WLAN_MAX_NETWORKS][128], name[33];
    INT32 signals[WLAN_MAX_NETWORKS];
    UINT32 count, i = 0;
    sd_bus *own;
    int r;

    pthread_mutex_lock( &bus_mutex );
    if (!get_bus()) r = -ENOTCONN;
    else if (!(r = ordered_networks( params->path, paths, signals, WLAN_MAX_NETWORKS, &count )))
    {
        for (i = 0; i < count; i++)
            if (!get_string( paths[i], IWD ".Network", "Name", name, sizeof(name) ) && !strcmp( name, params->ssid ))
                break;
    }
    pthread_mutex_unlock( &bus_mutex );
    if (r == -ENOTCONN) return STATUS_NOT_SUPPORTED;
    if (r) return bus_error( r );
    if (i == count) return STATUS_NOT_FOUND;

    /* iwd notices a new network file at once; give it a moment to read it */
    if (params->passphrase && *params->passphrase)
    {
        if (!write_psk_file( params->ssid, params->passphrase )) return STATUS_ACCESS_DENIED;
        usleep( 300000 );
    }

    /* the association and the WPA handshake take up to half a minute: on a
     * connection of its own, so that the list of networks goes on meanwhile */
    if (sd_bus_open_system( &own ) < 0) return STATUS_NOT_SUPPORTED;
    sd_bus_set_method_call_timeout( own, 30000000 );
    r = sd_bus_call_method( own, IWD, paths[i], IWD ".Network", "Connect", &error, NULL, "" );
    sd_bus_flush_close_unref( own );
    /* iwd may already be joining it on its own, having just learnt the key */
    if (r < 0 && (sd_bus_error_has_name( &error, IWD ".InProgress" ) || sd_bus_error_has_name( &error, IWD ".Busy" )))
    {
        for (int tries = 0; tries < 60 && r < 0; tries++)
        {
            usleep( 500000 );
            pthread_mutex_lock( &bus_mutex );
            if (get_bus() && get_bool( paths[i], IWD ".Network", "Connected" ) > 0) r = 0;
            pthread_mutex_unlock( &bus_mutex );
        }
    }
    if (r < 0)
    {
        NTSTATUS status = sd_bus_error_has_name( &error, IWD ".NoAgent" ) ||
                          sd_bus_error_has_name( &error, IWD ".InvalidFormat" ) ? STATUS_WRONG_PASSWORD :
                          sd_bus_error_has_name( &error, IWD ".Failed" ) ? STATUS_LOGON_FAILURE :
                          sd_bus_error_has_name( &error, IWD ".AlreadyConnected" ) ? STATUS_SUCCESS :
                          bus_error( r );
        ERR( "connect %s: %s %s\n", params->ssid, error.name, error.message );
        sd_bus_error_free( &error );
        return status;
    }
    sd_bus_error_free( &error );
    return STATUS_SUCCESS;
}

static NTSTATUS wlan_forget( void *args )
{
    const struct wlan_forget_params *params = args;
    char path[512];

    network_file( params->ssid, "psk", path, sizeof(path) );
    unlink( path );
    network_file( params->ssid, "open", path, sizeof(path) );
    unlink( path );
    return STATUS_SUCCESS;
}

static NTSTATUS locked( NTSTATUS (*func)(void *), void *args )
{
    NTSTATUS status;

    pthread_mutex_lock( &bus_mutex );
    status = func( args );
    pthread_mutex_unlock( &bus_mutex );
    return status;
}

static NTSTATUS locked_interfaces( void *args ) { return locked( wlan_interfaces, args ); }
static NTSTATUS locked_networks( void *args )   { return locked( wlan_networks, args ); }
static NTSTATUS locked_scan( void *args )       { return locked( wlan_scan, args ); }
static NTSTATUS locked_disconnect( void *args ) { return locked( wlan_disconnect, args ); }
static NTSTATUS locked_forget( void *args )     { return locked( wlan_forget, args ); }

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    locked_interfaces,
    locked_networks,
    locked_scan,
    wlan_connect,   /* takes the lock only to find the network, then waits on a bus of its own */
    locked_disconnect,
    locked_forget,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_funcs) == unix_funcs_count );
