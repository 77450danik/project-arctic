/*
 * Network and Sharing Center: the unix side
 *
 * The settings of an interface go to the host as a file, /run/arctic/network
 * /<interface>.conf, which arctic-init applies as it changes (docs/M5-network
 * .md); the addresses set by hand of a Wi-Fi adapter go into the network
 * files of iwd, which configures Wi-Fi itself. And what Windows shows of an
 * adapter: its name from the id databases, its speed, how long it has been
 * connected.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep unix
#endif

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <systemd/sd-bus.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "unixlib.h"

#define NETCFG_DIR "/run/arctic/network"
#define RESOLV_DIR "/run/arctic/resolv.d"
#define IWD_STATE_DIR "/var/lib/iwd"
#define IWD "net.connman.iwd"

/* the station of iwd for an interface: the object whose Device is called so */
static int iwd_station( sd_bus *bus, const char *ifname, char *station, size_t size )
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int found = 0;

    if (sd_bus_call_method( bus, IWD, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", &error,
                            &reply, "" ) < 0)
    {
        sd_bus_error_free( &error );
        return 0;
    }
    if (sd_bus_message_enter_container( reply, 'a', "{oa{sa{sv}}}" ) >= 0)
    {
        while (!found && sd_bus_message_enter_container( reply, 'e', "oa{sa{sv}}" ) > 0)
        {
            const char *path, *iface;

            sd_bus_message_read( reply, "o", &path );
            sd_bus_message_enter_container( reply, 'a', "{sa{sv}}" );
            while (sd_bus_message_enter_container( reply, 'e', "sa{sv}" ) > 0)
            {
                sd_bus_message_read( reply, "s", &iface );
                if (strcmp( iface, IWD ".Device" )) sd_bus_message_skip( reply, "a{sv}" );
                else
                {
                    const char *key, *name;

                    sd_bus_message_enter_container( reply, 'a', "{sv}" );
                    while (sd_bus_message_enter_container( reply, 'e', "sv" ) > 0)
                    {
                        sd_bus_message_read( reply, "s", &key );
                        if (!strcmp( key, "Name" ) && sd_bus_message_enter_container( reply, 'v', "s" ) >= 0)
                        {
                            sd_bus_message_read( reply, "s", &name );
                            sd_bus_message_exit_container( reply );
                            if (!strcmp( name, ifname ))
                            {
                                snprintf( station, size, "%s", path );
                                found = 1;
                            }
                        }
                        else sd_bus_message_skip( reply, "v" );
                        sd_bus_message_exit_container( reply );
                    }
                    sd_bus_message_exit_container( reply );
                }
                sd_bus_message_exit_container( reply );
            }
            sd_bus_message_exit_container( reply );
            sd_bus_message_exit_container( reply );
        }
    }
    sd_bus_message_unref( reply );
    return found;
}

/* what Wi-Fi sends at now, in kbit/s: iwd's diagnostics give it in 100 kbit/s */
static unsigned int wifi_bitrate( const char *ifname )
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    unsigned int rate = 0;
    char station[128];
    sd_bus *bus = NULL;

    if (sd_bus_open_system( &bus ) < 0) return 0;
    if (iwd_station( bus, ifname, station, sizeof(station) ) &&
        sd_bus_call_method( bus, IWD, station, IWD ".StationDiagnostic", "GetDiagnostics", &error, &reply, "" ) >= 0 &&
        sd_bus_message_enter_container( reply, 'a', "{sv}" ) >= 0)
    {
        const char *key;

        while (sd_bus_message_enter_container( reply, 'e', "sv" ) > 0)
        {
            uint32_t value;

            sd_bus_message_read( reply, "s", &key );
            if (!strcmp( key, "TxBitrate" ) && sd_bus_message_enter_container( reply, 'v', "u" ) >= 0)
            {
                if (sd_bus_message_read( reply, "u", &value ) >= 0) rate = value * 100;
                sd_bus_message_exit_container( reply );
            }
            else sd_bus_message_skip( reply, "v" );
            sd_bus_message_exit_container( reply );
        }
    }
    sd_bus_error_free( &error );
    sd_bus_message_unref( reply );
    sd_bus_flush_close_unref( bus );
    return rate;
}

static BOOL safe_name( const char *name )
{
    return name[0] && name[0] != '.' && !strchr( name, '/' ) && strlen( name ) < 32;
}

static NTSTATUS read_settings( void *args )
{
    struct settings_params *params = args;
    char path[256];
    size_t n;
    FILE *f;

    params->text[0] = 0;
    if (!safe_name( params->ifname )) return STATUS_INVALID_PARAMETER;
    snprintf( path, sizeof(path), NETCFG_DIR "/%s.conf", params->ifname );
    if (!(f = fopen( path, "r" ))) return STATUS_NOT_FOUND;
    n = fread( params->text, 1, sizeof(params->text) - 1, f );
    params->text[n] = 0;
    fclose( f );
    return STATUS_SUCCESS;
}

/* written whole and renamed into place: arctic-init reads it as it lands */
static NTSTATUS write_settings( void *args )
{
    struct settings_params *params = args;
    char path[256], tmp[256];
    size_t len = strnlen( params->text, sizeof(params->text) );
    FILE *f;

    if (!safe_name( params->ifname )) return STATUS_INVALID_PARAMETER;
    if (access( NETCFG_DIR, W_OK )) return STATUS_NOT_SUPPORTED;   /* a host without it */
    snprintf( path, sizeof(path), NETCFG_DIR "/%s.conf", params->ifname );
    snprintf( tmp, sizeof(tmp), NETCFG_DIR "/.%s.conf.new", params->ifname );
    if (!(f = fopen( tmp, "w" ))) return STATUS_ACCESS_DENIED;
    if (fwrite( params->text, 1, len, f ) != len || fclose( f ))
    {
        unlink( tmp );
        return STATUS_DISK_FULL;
    }
    if (rename( tmp, path ))
    {
        unlink( tmp );
        return STATUS_ACCESS_DENIED;
    }
    return STATUS_SUCCESS;
}

static unsigned int read_hex( const char *path )
{
    unsigned int value = 0;
    FILE *f;

    if ((f = fopen( path, "r" )))
    {
        if (fscanf( f, "%x", &value ) != 1) value = 0;
        fclose( f );
    }
    return value;
}

/* "Realtek RTL8111/8168/8411 PCI Express Gigabit Ethernet Controller" */
static void adapter_description( const char *name, char *buf, size_t size )
{
    char path[256], line[256], *model;
    unsigned int vendor, device, id;
    const char *db = "/usr/share/hwdata/pci.ids";
    int in_vendor = 0;
    FILE *f;

    snprintf( buf, size, "%s", name );
    snprintf( path, sizeof(path), "/sys/class/net/%s/device/vendor", name );
    vendor = read_hex( path );
    snprintf( path, sizeof(path), "/sys/class/net/%s/device/device", name );
    device = read_hex( path );
    if (!vendor)
    {
        /* a USB adapter: its ids are the device's, two levels up */
        snprintf( path, sizeof(path), "/sys/class/net/%s/device/../idVendor", name );
        vendor = read_hex( path );
        snprintf( path, sizeof(path), "/sys/class/net/%s/device/../idProduct", name );
        device = read_hex( path );
        db = "/usr/share/hwdata/usb.ids";
    }
    if (!vendor || !(f = fopen( db, "r" ))) return;
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

/* The name servers the host resolves by for the interface, as arctic-resolv
 * takes them: those set by hand in its settings, else what DHCP or iwd gave.
 * A program's own list (Wine's resolver) stays what it was when it started. */
static void name_servers( const char *ifname, char *out, size_t size )
{
    char path[256], line[512];
    size_t len = 0;
    FILE *f;

    out[0] = 0;
    snprintf( path, sizeof(path), NETCFG_DIR "/%s.conf", ifname );
    if ((f = fopen( path, "r" )))
    {
        while (fgets( line, sizeof(line), f ))
        {
            char *value;

            if (strncmp( line, "dns=", 4 ) && strncmp( line, "dns6=", 5 )) continue;
            value = strchr( line, '=' ) + 1;
            value[strcspn( value, "\r\n" )] = 0;
            if (*value) len += snprintf( out + len, size - len, "%s%s", len ? " " : "", value );
            if (len >= size) len = size - 1;
        }
        fclose( f );
    }
    if (out[0]) return;
    snprintf( path, sizeof(path), RESOLV_DIR "/%s", ifname );
    if (!(f = fopen( path, "r" ))) return;
    while (fgets( line, sizeof(line), f ))
    {
        char server[128];

        if (sscanf( line, "nameserver %127s", server ) != 1) continue;
        len += snprintf( out + len, size - len, "%s%s", len ? " " : "", server );
        if (len >= size) len = size - 1;
    }
    fclose( f );
}

static NTSTATUS link_info( void *args )
{
    struct link_info_params *params = args;
    char path[256];
    int speed = 0;
    FILE *f;

    if (!safe_name( params->ifname )) return STATUS_INVALID_PARAMETER;
    adapter_description( params->ifname, params->adapter, sizeof(params->adapter) );
    snprintf( path, sizeof(path), "/sys/class/net/%s/speed", params->ifname );
    if ((f = fopen( path, "r" )))
    {
        if (fscanf( f, "%d", &speed ) != 1 || speed < 0) speed = 0;
        fclose( f );
    }
    params->speed_kbps = speed * 1000;
    snprintf( path, sizeof(path), "/sys/class/net/%s/wireless", params->ifname );
    if (!access( path, F_OK )) params->speed_kbps = wifi_bitrate( params->ifname );
    /* since when it has its addresses: arctic-resolv and arctic-init note the
     * machine's uptime then (the clock on the wall is set after the start) */
    params->connected_for = 0;
    snprintf( path, sizeof(path), RESOLV_DIR "/.since-%s", params->ifname );
    if ((f = fopen( path, "r" )))
    {
        struct timespec now;
        long long since = 0;

        if (fscanf( f, "%lld", &since ) == 1 && !clock_gettime( CLOCK_BOOTTIME, &now ) && now.tv_sec >= since)
            params->connected_for = now.tv_sec - since;
        fclose( f );
    }
    name_servers( params->ifname, params->dns, sizeof(params->dns) );
    return STATUS_SUCCESS;
}

/* a network file of iwd with its [IPv4] and [IPv6] sections in place of the old */
static void rewrite_network_file( const char *path, const char *ipv4, const char *ipv6 )
{
    char tmp[540], line[1024];
    FILE *in, *out;
    int skip = 0;

    if (!(in = fopen( path, "r" ))) return;
    snprintf( tmp, sizeof(tmp), "%s.arctic-new", path );
    if (!(out = fopen( tmp, "w" )))
    {
        fclose( in );
        return;
    }
    while (fgets( line, sizeof(line), in ))
    {
        if (line[0] == '[') skip = !strncmp( line, "[IPv4]", 6 ) || !strncmp( line, "[IPv6]", 6 );
        if (!skip) fputs( line, out );
    }
    fclose( in );
    if (ipv4[0]) fprintf( out, "\n[IPv4]\n%s", ipv4 );
    if (ipv6[0]) fprintf( out, "\n[IPv6]\n%s", ipv6 );
    if (fclose( out ) || rename( tmp, path )) unlink( tmp );
}

static NTSTATUS iwd_addresses( void *args )
{
    struct iwd_params *params = args;
    struct dirent *de;
    char path[512];
    DIR *dir;

    FILE *f;

    params->ipv4[sizeof(params->ipv4) - 1] = 0;
    params->ipv6[sizeof(params->ipv6) - 1] = 0;
    /* for the networks joined later: wlanapi.dll adds them to the files it writes */
    if (!params->ipv4[0] && !params->ipv6[0]) unlink( NETCFG_DIR "/wifi.iwd" );
    else if ((f = fopen( NETCFG_DIR "/wifi.iwd", "w" )))
    {
        if (params->ipv4[0]) fprintf( f, "\n[IPv4]\n%s", params->ipv4 );
        if (params->ipv6[0]) fprintf( f, "\n[IPv6]\n%s", params->ipv6 );
        fclose( f );
    }
    if (!(dir = opendir( IWD_STATE_DIR ))) return STATUS_NOT_FOUND;
    while ((de = readdir( dir )))
    {
        const char *ext = strrchr( de->d_name, '.' );

        if (de->d_name[0] == '.' || !ext || (strcmp( ext, ".psk" ) && strcmp( ext, ".open" ) && strcmp( ext, ".8021x" )))
            continue;
        snprintf( path, sizeof(path), IWD_STATE_DIR "/%s", de->d_name );
        rewrite_network_file( path, params->ipv4, params->ipv6 );
    }
    closedir( dir );
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    read_settings,
    write_settings,
    link_info,
    iwd_addresses,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_funcs) == unix_funcs_count );

#ifdef _WIN64

/* no pointers in the parameters: a 32-bit caller's are the same */
const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    read_settings,
    write_settings,
    link_info,
    iwd_addresses,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_wow64_funcs) == unix_funcs_count );

#endif
