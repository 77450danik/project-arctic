/*
 * Bluetooth bus: the controllers, through the kernel's management channel
 *
 * Arctic has no BlueZ: winebth.sys is the Bluetooth stack of the Windows
 * side, as bthport.sys is Windows' own, and it talks to the kernel's drivers
 * of the controllers (btusb and the Bluetooth core: HCI, L2CAP, SMP)
 * through the management channel (HCI_CHANNEL_CONTROL), which the root
 * helper arctic-bt opens for it so that the kernel trusts it with every
 * command (docs/bluetooth.md). What BlueZ's daemon would keep, Windows keeps:
 * the link keys and the devices are in the registry
 * (BTHPORT\Parameters\Keys and Devices), read by winebth.c when a radio
 * comes and given here (hci_adapter_load), and every new key goes back there
 * as an event.
 *
 * One thread reads the channel: the answers to commands and the events of
 * the controllers (a device found, connected, a pairing's request), which
 * become the watcher events winebth.c makes the radios and remote devices of
 * the PnP tree from. Pairing works as Windows' does: numeric comparison,
 * passkey entry and display, the legacy PIN, all through
 * BluetoothRegisterForAuthenticationEx; "just works" is taken without a
 * question when this side began the pairing.
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

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <winternl.h>
#include <winbase.h>
#include <windef.h>
#include <wine/debug.h>
#include <wine/list.h>

#include "unixlib.h"
#include "unixlib_priv.h"
#include "wine/winebth.h"
#include "bt.h"

WINE_DEFAULT_DEBUG_CHANNEL( winebth );

/* What the stack does is always in the log (nt.log): connections, the
 * streams set up, what failed; the system runs with WINEDEBUG=fixme-all */
#undef TRACE
#undef WARN
#define TRACE( ... ) fprintf( stderr, "winebth: " __VA_ARGS__ )
#define WARN( ... ) fprintf( stderr, "winebth: " __VA_ARGS__ )

#define HELPER_SOCKET "/run/arctic/bt.sock"
#define STALE_SECONDS 120      /* a device found and not seen again is forgotten */
#define COMMAND_TIMEOUT 10     /* seconds */

struct adapter
{
    struct list entry;
    UINT16 index;
    struct unix_name *name;
    bdaddr_t addr;
    UINT8 version;
    UINT16 manufacturer;
    UINT32 settings;
    UINT32 class;
    char local_name[MGMT_MAX_NAME_LENGTH];
    BOOL discovering, discovery_wanted, loaded;
    int pairings;              /* discovery waits while one goes on */
    struct list devices;
    /* the advertisement monitor that hears the earbuds' batteries */
    UINT16 monitor;
    BOOL monitor_on, monitor_busy;
};

struct device
{
    struct list entry;
    struct adapter *adapter;
    struct unix_name *name;
    bdaddr_t addr;
    char dev_name[BLUETOOTH_MAX_NAME_SIZE];
    UINT32 class;
    BOOL connected, paired, legacy, audio;
    time_t last_seen;
    IRP *pairing_irp;          /* a pairing this side began */
    /* the battery (hands-free), the earbuds' and their case's (Fast Pair's advertisements) */
    BYTE battery[4], charging;
    bdaddr_t buds_addr;        /* the LE address that advertises them */
    int buds_rssi;
    time_t buds_seen;
};

static struct list adapters = LIST_INIT( adapters );
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static int mgmt_fd = -1;
static BOOL shutting_down;
static pthread_t reader;

static void adapter_added( UINT16 index );

static void copy_str( char *dst, const char *src, size_t size )
{
    size_t len = strnlen( src, size - 1 );

    memcpy( dst, src, len );
    dst[len] = 0;
}

/**********************************************************************
 *          The root helper
 */

int bt_helper( const char *request )
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX, .sun_path = HELPER_SOCKET };
    char word[8] = "", control[CMSG_SPACE(sizeof(int))];
    struct iovec iov = { word, sizeof(word) - 1 };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control) };
    struct cmsghdr *cmsg;
    int s, fd = -1;
    ssize_t len;

    if ((s = socket( AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0 )) < 0) return -1;
    if (connect( s, (struct sockaddr *)&addr, sizeof(addr) ) || write( s, request, strlen( request ) ) < 0)
    {
        close( s );
        return -1;
    }
    len = recvmsg( s, &msg, MSG_CMSG_CLOEXEC );
    close( s );
    if (len < 2 || strncmp( word, "ok", 2 )) return -1;
    for (cmsg = CMSG_FIRSTHDR( &msg ); cmsg; cmsg = CMSG_NXTHDR( &msg, cmsg ))
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS)
            memcpy( &fd, CMSG_DATA( cmsg ), sizeof(fd) );
    return fd >= 0 ? fd : 0;
}

/**********************************************************************
 *          Events for winebth.c
 */

struct queued_event
{
    struct list entry;
    struct winebluetooth_event event;
};

static struct list events = LIST_INIT( events );
static pthread_mutex_t events_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t events_cond = PTHREAD_COND_INITIALIZER;

void hci_post_event( const struct winebluetooth_event *event )
{
    struct queued_event *q = malloc( sizeof(*q) );

    if (!q) return;
    q->event = *event;
    pthread_mutex_lock( &events_lock );
    list_add_tail( &events, &q->entry );
    pthread_cond_signal( &events_cond );
    pthread_mutex_unlock( &events_lock );
}

NTSTATUS hci_get_event( struct winebluetooth_event *result )
{
    struct queued_event *q;

    pthread_mutex_lock( &events_lock );
    while (list_empty( &events ) && !shutting_down) pthread_cond_wait( &events_cond, &events_lock );
    if (shutting_down)
    {
        pthread_mutex_unlock( &events_lock );
        return STATUS_SUCCESS;
    }
    q = LIST_ENTRY( list_head( &events ), struct queued_event, entry );
    list_remove( &q->entry );
    pthread_mutex_unlock( &events_lock );
    *result = q->event;
    free( q );
    return STATUS_PENDING;
}

static void watcher_event( enum winebluetooth_watcher_event_type type, const union winebluetooth_watcher_event_data *data )
{
    struct winebluetooth_event event = { .status = WINEBLUETOOTH_EVENT_WATCHER_EVENT };

    event.data.watcher_event.event_type = type;
    event.data.watcher_event.event_data = *data;
    hci_post_event( &event );
}

/* winebth.c keeps addresses with their most significant byte first */
static void addr_to_props( const bdaddr_t *addr, BLUETOOTH_ADDRESS *out )
{
    out->ullLong = 0;
    for (int i = 0; i < 6; i++) out->rgBytes[i] = addr->b[5 - i];
}

static void props_to_addr( const BLUETOOTH_ADDRESS *in, bdaddr_t *addr )
{
    for (int i = 0; i < 6; i++) addr->b[i] = in->rgBytes[5 - i];
}

static void radio_props( const struct adapter *a, struct winebluetooth_radio_properties *props )
{
    memset( props, 0, sizeof(*props) );
    props->discoverable = !!(a->settings & MGMT_SETTING_DISCOVERABLE);
    props->connectable = !!(a->settings & MGMT_SETTING_CONNECTABLE);
    props->discovering = a->discovering;
    props->pairable = !!(a->settings & MGMT_SETTING_BONDABLE);
    props->powered = !!(a->settings & MGMT_SETTING_POWERED);
    addr_to_props( &a->addr, &props->address );
    copy_str( props->name, a->local_name, sizeof(props->name) );
    props->class = a->class;
    props->manufacturer = a->manufacturer;
    props->version = a->version;
}

static void post_radio_added( struct adapter *a )
{
    union winebluetooth_watcher_event_data data = {0};

    data.radio_added.props_mask = WINEBLUETOOTH_RADIO_ALL_PROPERTIES;
    radio_props( a, &data.radio_added.props );
    data.radio_added.radio.handle = (UINT_PTR)unix_name_dup( a->name );
    watcher_event( BLUETOOTH_WATCHER_EVENT_TYPE_RADIO_ADDED, &data );
}

static void post_radio_changed( struct adapter *a, winebluetooth_radio_props_mask_t mask )
{
    union winebluetooth_watcher_event_data data = {0};

    data.radio_props_changed.changed_props_mask = mask;
    radio_props( a, &data.radio_props_changed.props );
    data.radio_props_changed.radio.handle = (UINT_PTR)unix_name_dup( a->name );
    watcher_event( BLUETOOTH_WATCHER_EVENT_TYPE_RADIO_PROPERTIES_CHANGED, &data );
}

static void device_props( const struct device *d, struct winebluetooth_device_properties *props )
{
    memset( props, 0, sizeof(*props) );
    addr_to_props( &d->addr, &props->address );
    copy_str( props->name, d->dev_name, sizeof(props->name) );
    props->connected = d->connected;
    props->paired = d->paired;
    props->legacy_pairing = d->legacy;
    props->trusted = d->paired;
    props->class = d->class;
    props->audio = d->audio;
    memcpy( props->battery, d->battery, sizeof(props->battery) );
    props->charging = d->charging;
}

static winebluetooth_device_props_mask_t device_mask( const struct device *d )
{
    winebluetooth_device_props_mask_t mask = WINEBLUETOOTH_DEVICE_ALL_PROPERTIES;

    if (!d->dev_name[0]) mask &= ~WINEBLUETOOTH_DEVICE_PROPERTY_NAME;
    if (!d->class) mask &= ~WINEBLUETOOTH_DEVICE_PROPERTY_CLASS;
    return mask;
}

static void post_device_added( struct device *d, BOOL init_entry )
{
    union winebluetooth_watcher_event_data data = {0};

    data.device_added.known_props_mask = device_mask( d );
    device_props( d, &data.device_added.props );
    data.device_added.device.handle = (UINT_PTR)unix_name_dup( d->name );
    data.device_added.radio.handle = (UINT_PTR)unix_name_dup( d->adapter->name );
    data.device_added.init_entry = init_entry;
    watcher_event( BLUETOOTH_WATCHER_EVENT_TYPE_DEVICE_ADDED, &data );
}

static void post_device_changed( struct device *d, winebluetooth_device_props_mask_t mask )
{
    union winebluetooth_watcher_event_data data = {0};

    data.device_props_changed.changed_props_mask = mask & device_mask( d );
    device_props( d, &data.device_props_changed.props );
    data.device_props_changed.device.handle = (UINT_PTR)unix_name_dup( d->name );
    watcher_event( BLUETOOTH_WATCHER_EVENT_TYPE_DEVICE_PROPERTIES_CHANGED, &data );
}

static void post_device_removed( struct device *d )
{
    union winebluetooth_watcher_event_data data = {0};

    data.device_removed.device.handle = (UINT_PTR)unix_name_dup( d->name );
    watcher_event( BLUETOOTH_WATCHER_EVENT_TYPE_DEVICE_REMOVED, &data );
}

static void post_auth( struct device *d, BLUETOOTH_AUTHENTICATION_METHOD method, UINT32 value )
{
    struct winebluetooth_event event = { .status = WINEBLUETOOTH_EVENT_AUTH_EVENT };

    event.data.auth_event.device.handle = (UINT_PTR)unix_name_dup( d->name );
    event.data.auth_event.method = method;
    event.data.auth_event.numeric_value_or_passkey = value;
    hci_post_event( &event );
}

void hci_media_key( UINT vk )
{
    union winebluetooth_watcher_event_data data = {0};

    data.media_key.vk = vk;
    watcher_event( BLUETOOTH_WATCHER_EVENT_TYPE_MEDIA_KEY, &data );
}

/**********************************************************************
 *          The management channel
 */

typedef void (*command_done)( UINT16 index, UINT8 status, const BYTE *reply, UINT16 len, void *context );

struct command
{
    struct list entry;
    UINT16 opcode, index;
    /* synchronous: the caller waits */
    BOOL waiting, done;
    UINT8 status;
    BYTE reply[512];
    UINT16 reply_len;
    /* asynchronous: called on the reader's thread */
    command_done callback;
    void *context;
};

static struct list commands = LIST_INIT( commands );
static pthread_mutex_t mgmt_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t mgmt_cond = PTHREAD_COND_INITIALIZER;

static BOOL mgmt_write( UINT16 opcode, UINT16 index, const void *data, UINT16 len )
{
    size_t size = sizeof(struct mgmt_hdr) + len;
    BYTE *buf = malloc( size );
    struct mgmt_hdr *hdr = (struct mgmt_hdr *)buf;
    BOOL ret;

    if (!buf) return FALSE;
    hdr->opcode = opcode;
    hdr->index = index;
    hdr->len = len;
    if (len) memcpy( buf + sizeof(*hdr), data, len );
    ret = write( mgmt_fd, buf, size ) == (ssize_t)size;
    if (!ret) WARN( "command %#x: %s\n", opcode, strerror( errno ) );
    free( buf );
    return ret;
}

/* a command answered later, on the reader's thread (callback may be NULL) */
static void mgmt_post( UINT16 opcode, UINT16 index, const void *data, UINT16 len, command_done callback, void *context )
{
    struct command *cmd = calloc( 1, sizeof(*cmd) );

    if (!cmd) return;
    cmd->opcode = opcode;
    cmd->index = index;
    cmd->callback = callback;
    cmd->context = context;
    pthread_mutex_lock( &mgmt_lock );
    list_add_tail( &commands, &cmd->entry );
    if (mgmt_write( opcode, index, data, len ))
    {
        pthread_mutex_unlock( &mgmt_lock );
        return;
    }
    list_remove( &cmd->entry );
    pthread_mutex_unlock( &mgmt_lock );
    if (callback) callback( index, MGMT_STATUS_LOCAL_TIMEOUT, NULL, 0, context );
    free( cmd );
}

/* a command and its answer; never on the reader's thread */
static UINT8 mgmt_call( UINT16 opcode, UINT16 index, const void *data, UINT16 len, void *reply, UINT16 *reply_len )
{
    struct command *cmd = calloc( 1, sizeof(*cmd) );
    struct timespec until;
    UINT8 status;

    if (!cmd) return MGMT_STATUS_LOCAL_TIMEOUT;
    cmd->opcode = opcode;
    cmd->index = index;
    cmd->waiting = TRUE;
    clock_gettime( CLOCK_REALTIME, &until );
    until.tv_sec += COMMAND_TIMEOUT;

    pthread_mutex_lock( &mgmt_lock );
    list_add_tail( &commands, &cmd->entry );
    if (!mgmt_write( opcode, index, data, len ))
    {
        list_remove( &cmd->entry );
        pthread_mutex_unlock( &mgmt_lock );
        free( cmd );
        return MGMT_STATUS_LOCAL_TIMEOUT;
    }
    while (!cmd->done)
        if (pthread_cond_timedwait( &mgmt_cond, &mgmt_lock, &until ) == ETIMEDOUT) break;
    if (!cmd->done)
    {
        WARN( "command %#x of controller %u: no answer\n", opcode, index );
        list_remove( &cmd->entry );
        cmd->status = MGMT_STATUS_LOCAL_TIMEOUT;
    }
    pthread_mutex_unlock( &mgmt_lock );

    status = cmd->status;
    if (reply && reply_len)
    {
        *reply_len = min( *reply_len, cmd->reply_len );
        memcpy( reply, cmd->reply, *reply_len );
    }
    free( cmd );
    return status;
}

/* the answer to a command: the oldest one of that code and controller */
static void command_answered( UINT16 index, UINT16 opcode, UINT8 status, const BYTE *data, UINT16 len, BOOL final )
{
    struct command *cmd, *found = NULL;

    pthread_mutex_lock( &mgmt_lock );
    LIST_FOR_EACH_ENTRY( cmd, &commands, struct command, entry )
        if (cmd->opcode == opcode && cmd->index == index)
        {
            found = cmd;
            break;
        }
    /* Command Status with success only says it started */
    if (!found || (!final && !status))
    {
        pthread_mutex_unlock( &mgmt_lock );
        return;
    }
    list_remove( &found->entry );
    if (found->waiting)
    {
        found->status = status;
        found->reply_len = min( len, sizeof(found->reply) );
        if (data) memcpy( found->reply, data, found->reply_len );
        found->done = TRUE;
        pthread_cond_broadcast( &mgmt_cond );
        pthread_mutex_unlock( &mgmt_lock );
        return;
    }
    pthread_mutex_unlock( &mgmt_lock );
    if (found->callback) found->callback( index, status, data, len, found->context );
    free( found );
}

static NTSTATUS status_from_mgmt( UINT8 status )
{
    switch (status)
    {
    case MGMT_STATUS_SUCCESS:
    case MGMT_STATUS_ALREADY_PAIRED:   return STATUS_SUCCESS;
    case MGMT_STATUS_CANCELLED:
    case MGMT_STATUS_REJECTED:         return STATUS_CANCELLED;
    case MGMT_STATUS_NOT_CONNECTED:
    case MGMT_STATUS_CONNECT_FAILED:
    case MGMT_STATUS_DISCONNECTED:
    case MGMT_STATUS_TIMEOUT:          return STATUS_DEVICE_NOT_CONNECTED;
    case MGMT_STATUS_AUTH_FAILED:      return STATUS_ACCESS_DENIED;
    case MGMT_STATUS_NOT_POWERED:
    case MGMT_STATUS_RFKILLED:         return STATUS_DEVICE_NOT_READY;
    case MGMT_STATUS_BUSY:             return STATUS_DEVICE_BUSY;
    case MGMT_STATUS_NOT_SUPPORTED:    return STATUS_NOT_SUPPORTED;
    case MGMT_STATUS_INVALID_PARAMS:   return STATUS_INVALID_PARAMETER;
    case MGMT_STATUS_PERMISSION_DENIED: return STATUS_ACCESS_DENIED;
    case MGMT_STATUS_LOCAL_TIMEOUT:    return STATUS_IO_TIMEOUT;
    default:                           return STATUS_UNSUCCESSFUL;
    }
}

/**********************************************************************
 *          Adapters and devices
 */

static struct adapter *find_adapter_index( UINT16 index )
{
    struct adapter *a;

    LIST_FOR_EACH_ENTRY( a, &adapters, struct adapter, entry )
        if (a->index == index) return a;
    return NULL;
}

static struct adapter *find_adapter_name( struct unix_name *name )
{
    struct adapter *a;

    LIST_FOR_EACH_ENTRY( a, &adapters, struct adapter, entry )
        if (a->name == name) return a;
    return NULL;
}

static struct device *find_device( struct adapter *a, const bdaddr_t *addr )
{
    struct device *d;

    LIST_FOR_EACH_ENTRY( d, &a->devices, struct device, entry )
        if (!memcmp( &d->addr, addr, sizeof(*addr) )) return d;
    return NULL;
}

static struct device *find_device_name( struct unix_name *name )
{
    struct adapter *a;
    struct device *d;

    LIST_FOR_EACH_ENTRY( a, &adapters, struct adapter, entry )
        LIST_FOR_EACH_ENTRY( d, &a->devices, struct device, entry )
            if (d->name == name) return d;
    return NULL;
}

/* any controller's device of that address */
static struct device *find_device_addr( const BYTE addr[6] )
{
    struct adapter *a;
    struct device *d;

    LIST_FOR_EACH_ENTRY( a, &adapters, struct adapter, entry )
        if ((d = find_device( a, (const bdaddr_t *)addr ))) return d;
    return NULL;
}

static struct device *new_device( struct adapter *a, const bdaddr_t *addr )
{
    struct device *d = calloc( 1, sizeof(*d) );
    char path[64];

    if (!d) return NULL;
    snprintf( path, sizeof(path), "/hci%u/dev_%02X_%02X_%02X_%02X_%02X_%02X", a->index, addr->b[5], addr->b[4],
              addr->b[3], addr->b[2], addr->b[1], addr->b[0] );
    if (!(d->name = unix_name_get_or_create( path )))
    {
        free( d );
        return NULL;
    }
    d->adapter = a;
    d->addr = *addr;
    d->last_seen = time( NULL );
    memset( d->battery, 0xff, sizeof(d->battery) );
    list_add_tail( &a->devices, &d->entry );
    return d;
}

static void free_device( struct device *d )
{
    list_remove( &d->entry );
    unix_name_free( d->name );
    free( d );
}

/* the name and the class of device in a found device's EIR data;
 * the mask of what changed */
static winebluetooth_device_props_mask_t parse_eir( struct device *d, const BYTE *eir, UINT16 len )
{
    winebluetooth_device_props_mask_t changed = 0;

    for (UINT16 pos = 0; pos + 1 < len; )
    {
        BYTE field = eir[pos], type;
        const BYTE *value;

        if (!field || pos + 1 + field > len) break;
        type = eir[pos + 1];
        value = eir + pos + 2;
        if (type == EIR_NAME_COMPLETE || (type == EIR_NAME_SHORT && !d->dev_name[0]))
        {
            char name[BLUETOOTH_MAX_NAME_SIZE];
            UINT n = min( field - 1, sizeof(name) - 1 );

            memcpy( name, value, n );
            name[n] = 0;
            if (strcmp( name, d->dev_name ))
            {
                strcpy( d->dev_name, name );
                changed |= WINEBLUETOOTH_DEVICE_PROPERTY_NAME;
            }
        }
        else if (type == EIR_CLASS_OF_DEV && field >= 4)
        {
            UINT32 class = value[0] | (value[1] << 8) | (value[2] << 16);
            if (class != d->class)
            {
                d->class = class;
                changed |= WINEBLUETOOTH_DEVICE_PROPERTY_CLASS;
            }
        }
        pos += 1 + field;
    }
    return changed;
}

/* the audio and video devices (headphones, speakers, a car's kit) have music */
static BOOL is_audio_class( UINT32 class )
{
    return ((class >> 8) & 0x1f) == 0x04 || (class & (1 << 21)) /* the Rendering service */;
}

static void *connect_profiles_thread( void *arg )
{
    BYTE addr[6], wait = ((BYTE *)arg)[6];
    NTSTATUS status;

    memcpy( addr, arg, 6 );
    free( arg );
    /* the headset often connects itself right after the pairing or its own connection: let it */
    sleep( wait );
    status = a2dp_connect( addr );
    TRACE( "%02X:%02X:%02X:%02X:%02X:%02X: music %#x\n", addr[5], addr[4], addr[3], addr[2], addr[1], addr[0],
           (UINT)status );
    /* then its microphone and battery, when the headset has not connected them itself */
    hfp_connect( addr );
    return NULL;
}

/* after a pairing, on a click "Connect", when headphones connect: the profiles the device has */
static void connect_profiles_after( const bdaddr_t *addr, BYTE seconds )
{
    BYTE *copy = malloc( 7 );
    pthread_t thread;

    if (!copy) return;
    memcpy( copy, addr, 6 );
    copy[6] = seconds;
    if (pthread_create( &thread, NULL, connect_profiles_thread, copy ))
    {
        free( copy );
        return;
    }
    pthread_detach( thread );
}

static void connect_profiles( const bdaddr_t *addr )
{
    connect_profiles_after( addr, 2 );
}

BOOL hci_device_info( const BYTE addr[6], struct bt_device_info *info )
{
    struct device *d;
    BOOL ret = FALSE;

    pthread_mutex_lock( &state_lock );
    if ((d = find_device_addr( addr )))
    {
        copy_str( info->name, d->dev_name, sizeof(info->name) );
        info->class = d->class;
        info->paired = d->paired;
        info->connected = d->connected;
        ret = TRUE;
    }
    pthread_mutex_unlock( &state_lock );
    return ret;
}

void hci_device_set_audio( const BYTE addr[6], BOOL audio )
{
    struct device *d;

    pthread_mutex_lock( &state_lock );
    if ((d = find_device_addr( addr )) && d->audio != audio)
    {
        d->audio = audio;
        post_device_changed( d, WINEBLUETOOTH_DEVICE_PROPERTY_AUDIO );
    }
    pthread_mutex_unlock( &state_lock );
}

/* what the headset says over hands-free: 0-100 */
void hci_device_set_battery( const BYTE addr[6], int level )
{
    struct device *d;

    level = max( 0, min( level, 100 ) );
    pthread_mutex_lock( &state_lock );
    if ((d = find_device_addr( addr )) && d->battery[0] != level)
    {
        d->battery[0] = level;
        TRACE( "%s: battery %d%%\n", debugstr_a( d->name->str ), level );
        post_device_changed( d, WINEBLUETOOTH_DEVICE_PROPERTY_BATTERY );
    }
    pthread_mutex_unlock( &state_lock );
}

/**********************************************************************
 *          The earbuds' batteries
 *
 * As Android shows them: earbuds of Google's Fast Pair advertise over Bluetooth
 * Low Energy, beside their classic connection, the batteries of the left and
 * the right one and of their case (Fast Pair, "Battery Notification"). An
 * advertisement monitor of the kernel hears them while headphones are
 * connected, with a slow passive scan; the earbuds nearest, whose levels agree
 * with what the headset says over hands-free, are the connected ones.
 */

static void monitor_added( UINT16 index, UINT8 status, const BYTE *reply, UINT16 len, void *context )
{
    struct adapter *a;

    pthread_mutex_lock( &state_lock );
    if ((a = find_adapter_index( index )))
    {
        a->monitor_busy = FALSE;
        if (!status && len >= 2)
        {
            a->monitor = reply[0] | (reply[1] << 8);
            a->monitor_on = TRUE;
            TRACE( "controller %u: listening for earbuds' batteries\n", index );
        }
        else WARN( "controller %u: no advertisement monitor (%#x)\n", index, status );
    }
    pthread_mutex_unlock( &state_lock );
}

static void monitor_removed( UINT16 index, UINT8 status, const BYTE *reply, UINT16 len, void *context )
{
    struct adapter *a;

    pthread_mutex_lock( &state_lock );
    if ((a = find_adapter_index( index ))) a->monitor_busy = FALSE;
    pthread_mutex_unlock( &state_lock );
}

/* the scan is slow: 30 ms a second, not to disturb the music (state_lock held) */
static void update_monitor( struct adapter *a )
{
    BOOL wanted = FALSE;
    struct device *d;

    LIST_FOR_EACH_ENTRY( d, &a->devices, struct device, entry )
        if (d->connected && d->paired && is_audio_class( d->class )) wanted = TRUE;
    if (a->monitor_busy || wanted == a->monitor_on) return;
    a->monitor_busy = TRUE;
    if (wanted)
    {
        BYTE cp[1 + sizeof(struct mgmt_adv_pattern)] = { 1 };
        struct mgmt_adv_pattern *pattern = (struct mgmt_adv_pattern *)(cp + 1);

        pattern->ad_type = EIR_SERVICE_DATA16;
        pattern->offset = 0;
        pattern->length = 2;
        pattern->value[0] = 0x2c;   /* Fast Pair's service, 0xfe2c */
        pattern->value[1] = 0xfe;
        mgmt_post( MGMT_OP_ADD_ADV_PATTERNS_MONITOR, a->index, cp, sizeof(cp), monitor_added, NULL );
    }
    else
    {
        BYTE cp[2] = { a->monitor & 0xff, a->monitor >> 8 };
        a->monitor_on = FALSE;
        mgmt_post( MGMT_OP_REMOVE_ADV_MONITOR, a->index, cp, sizeof(cp), monitor_removed, NULL );
    }
}

/* the monitor's scan: slow, where the kernel has its usual default (60 ms of 120) */
static void slow_monitor_scan( UINT16 index )
{
    BYTE reply[512], cp[2 * 5];
    UINT16 len = sizeof(reply), interval = 0, window = 0;

    if (mgmt_call( MGMT_OP_READ_DEF_SYSTEM_CONFIG, index, NULL, 0, reply, &len )) return;
    for (UINT pos = 0; pos + 3 <= len; pos += 3 + reply[pos + 2])
    {
        UINT16 type = reply[pos] | (reply[pos + 1] << 8);

        if (pos + 3 + reply[pos + 2] > len || reply[pos + 2] != 2) continue;
        if (type == MGMT_CONFIG_LE_SCAN_INT_ADV_MONITOR) interval = reply[pos + 3] | (reply[pos + 4] << 8);
        if (type == MGMT_CONFIG_LE_SCAN_WINDOW_ADV_MONITOR) window = reply[pos + 3] | (reply[pos + 4] << 8);
    }
    if (interval != 0x0060 || window != 0x0030) return;
    cp[0] = MGMT_CONFIG_LE_SCAN_INT_ADV_MONITOR; cp[1] = 0; cp[2] = 2; cp[3] = 0x40; cp[4] = 0x06;   /* 1 s */
    cp[5] = MGMT_CONFIG_LE_SCAN_WINDOW_ADV_MONITOR; cp[6] = 0; cp[7] = 2; cp[8] = 0x30; cp[9] = 0x00; /* 30 ms */
    mgmt_call( MGMT_OP_SET_DEF_SYSTEM_CONFIG, index, cp, sizeof(cp), NULL, NULL );
}

/* Fast Pair's service data: the batteries, when they are in it */
static BOOL fast_pair_batteries( const BYTE *eir, UINT16 len, BYTE levels[3] )
{
    for (UINT pos = 0; pos + 1 < len && eir[pos]; pos += 1 + eir[pos])
    {
        const BYTE *data = eir + pos + 2;
        UINT size = eir[pos] - 1;

        if (pos + 1 + eir[pos] > len) break;
        if (eir[pos + 1] != EIR_SERVICE_DATA16 || size < 3 || data[0] != 0x2c || data[1] != 0xfe) continue;
        data += 2;
        size -= 2;
        /* three bytes: the model of discoverable earbuds; else a flags byte and fields 0bLLLLTTTT */
        if (size == 3) return FALSE;
        for (UINT f = 1; f < size; )
        {
            UINT field_len = data[f] >> 4, type = data[f] & 0x0f;

            if (f + 1 + field_len > size) break;
            if ((type == 3 || type == 4) && field_len == 3)
            {
                memcpy( levels, data + f + 1, 3 );
                return TRUE;
            }
            f += 1 + field_len;
        }
    }
    return FALSE;
}

static void on_advertisement( struct adapter *a, const struct mgmt_addr_info *addr, int rssi, const BYTE *eir, UINT16 len )
{
    struct device *d, *connected = NULL;
    BYTE raw[3], charging = 0;
    BYTE levels[3];
    time_t now = time( NULL );
    int count = 0, nearest;

    if (!fast_pair_batteries( eir, len, raw )) return;
    for (int i = 0; i < 3; i++)
    {
        levels[i] = (raw[i] & 0x7f) <= 100 ? raw[i] & 0x7f : 0xff;
        if (raw[i] & 0x80 && levels[i] != 0xff) charging |= 1 << (i + 1);
    }
    if (levels[0] == 0xff && levels[1] == 0xff) return;
    if (rssi == 127 || rssi < -80) return;   /* not near: someone else's */

    LIST_FOR_EACH_ENTRY( d, &a->devices, struct device, entry )
        if (d->connected && d->paired && is_audio_class( d->class ))
        {
            connected = d;
            count++;
        }
    if (count != 1) return;
    d = connected;

    /* what the headset says itself agrees: one earbud is at its level */
    if (d->battery[0] != 0xff)
    {
        BOOL agrees = FALSE;
        for (int i = 0; i < 2; i++)
            if (levels[i] != 0xff && abs( (int)levels[i] - d->battery[0] ) <= 15) agrees = TRUE;
        if (!agrees) return;
    }
    /* the earbuds heard before keep it, unless these are much nearer or those are silent */
    nearest = !d->buds_seen || !memcmp( &d->buds_addr, &addr->bdaddr, 6 ) || now - d->buds_seen > 30 ||
              rssi > d->buds_rssi + 6;
    if (!nearest) return;
    d->buds_addr = addr->bdaddr;
    d->buds_rssi = rssi;
    d->buds_seen = now;
    if (!memcmp( d->battery + 1, levels, 3 ) && (d->charging & 0x0e) == charging) return;
    memcpy( d->battery + 1, levels, 3 );
    d->charging = (d->charging & 1) | charging;
    TRACE( "%s: earbuds %d%% %d%%, case %d%%\n", debugstr_a( d->name->str ), levels[0] == 0xff ? -1 : levels[0],
           levels[1] == 0xff ? -1 : levels[1], levels[2] == 0xff ? -1 : levels[2] );
    post_device_changed( d, WINEBLUETOOTH_DEVICE_PROPERTY_BATTERY );
}

/**********************************************************************
 *          Events of the controllers (the reader's thread)
 */

static void discovery_started( UINT16 index, UINT8 status, const BYTE *reply, UINT16 len, void *context )
{
    struct adapter *a;

    if (!status || status == MGMT_STATUS_BUSY) return;
    pthread_mutex_lock( &state_lock );
    if ((a = find_adapter_index( index ))) a->discovery_wanted = FALSE;
    pthread_mutex_unlock( &state_lock );
    WARN( "controller %u: discovery did not start (%#x)\n", index, status );
}

static void start_discovery( struct adapter *a )
{
    BYTE type = 0x01;  /* BR/EDR */

    mgmt_post( MGMT_OP_START_DISCOVERY, a->index, &type, 1, discovery_started, NULL );
}

static void on_settings( struct adapter *a, UINT32 settings )
{
    UINT32 old = a->settings;
    winebluetooth_radio_props_mask_t mask = 0;

    a->settings = settings;
    if ((old ^ settings) & MGMT_SETTING_POWERED) mask |= WINEBLUETOOTH_RADIO_PROPERTY_POWERED;
    if ((old ^ settings) & MGMT_SETTING_CONNECTABLE) mask |= WINEBLUETOOTH_RADIO_PROPERTY_CONNECTABLE;
    if ((old ^ settings) & MGMT_SETTING_DISCOVERABLE) mask |= WINEBLUETOOTH_RADIO_PROPERTY_DISCOVERABLE;
    if ((old ^ settings) & MGMT_SETTING_BONDABLE) mask |= WINEBLUETOOTH_RADIO_PROPERTY_PAIRABLE;
    if (!(settings & MGMT_SETTING_POWERED) && a->discovering)
    {
        a->discovering = FALSE;
        mask |= WINEBLUETOOTH_RADIO_PROPERTY_DISCOVERING;
    }
    if (mask) post_radio_changed( a, mask );
    /* switched on while a search was asked for */
    if (!(old & MGMT_SETTING_POWERED) && (settings & MGMT_SETTING_POWERED) && a->discovery_wanted && !a->pairings)
        start_discovery( a );
}

static void on_device_found( struct adapter *a, const BYTE *data, UINT16 len )
{
    const struct mgmt_ev_device_found *ev = (const void *)data;
    winebluetooth_device_props_mask_t changed;
    struct device *d;
    BOOL added = FALSE;

    if (len < sizeof(*ev) || len < sizeof(*ev) + ev->eir_len) return;
    /* Bluetooth Low Energy devices: the earbuds' batteries only, yet */
    if (ev->addr.type != BDADDR_BREDR)
    {
        on_advertisement( a, &ev->addr, ev->rssi, ev->eir, ev->eir_len );
        return;
    }
    if (!(d = find_device( a, &ev->addr.bdaddr )))
    {
        if (!(d = new_device( a, &ev->addr.bdaddr ))) return;
        added = TRUE;
    }
    d->last_seen = time( NULL );
    changed = parse_eir( d, ev->eir, ev->eir_len );
    if (!!(ev->flags & MGMT_DEV_FOUND_LEGACY_PAIRING) != d->legacy)
    {
        d->legacy = !!(ev->flags & MGMT_DEV_FOUND_LEGACY_PAIRING);
        changed |= WINEBLUETOOTH_DEVICE_PROPERTY_LEGACY_PAIRING;
    }
    if (ev->flags & MGMT_DEV_FOUND_CONFIRM_NAME)
    {
        BYTE confirm[sizeof(struct mgmt_addr_info) + 1];

        memcpy( confirm, &ev->addr, sizeof(ev->addr) );
        confirm[sizeof(ev->addr)] = d->dev_name[0] != 0;
        mgmt_post( MGMT_OP_CONFIRM_NAME, a->index, confirm, sizeof(confirm), NULL, NULL );
    }
    if (added) post_device_added( d, FALSE );
    else if (changed) post_device_changed( d, changed );
}

static void on_connected( struct adapter *a, const BYTE *data, UINT16 len )
{
    const struct mgmt_ev_device_connected *ev = (const void *)data;
    winebluetooth_device_props_mask_t changed = WINEBLUETOOTH_DEVICE_PROPERTY_CONNECTED;
    struct device *d;
    BOOL added = FALSE;

    if (len < sizeof(*ev) || ev->addr.type != BDADDR_BREDR) return;
    if (!(d = find_device( a, &ev->addr.bdaddr )))
    {
        if (!(d = new_device( a, &ev->addr.bdaddr ))) return;
        added = TRUE;
    }
    if (len >= sizeof(*ev) + ev->eir_len) changed |= parse_eir( d, ev->eir, ev->eir_len );
    d->connected = TRUE;
    d->last_seen = time( NULL );
    TRACE( "%s connected (class %#x, %s)\n", debugstr_a( d->name->str ), d->class, d->paired ? "paired" : "not paired" );
    if (added) post_device_added( d, FALSE );
    else post_device_changed( d, changed );
    /* headphones that come back by themselves: their music, as Windows connects it,
     * when they have not set it up themselves by then */
    if (d->paired && is_audio_class( d->class )) connect_profiles_after( &d->addr, 4 );
    update_monitor( a );
}

static void on_disconnected( struct adapter *a, const BYTE *data, UINT16 len )
{
    const struct mgmt_addr_info *addr = (const void *)data;
    struct device *d;

    if (len < sizeof(*addr) || !(d = find_device( a, &addr->bdaddr ))) return;
    d->connected = FALSE;
    d->audio = FALSE;
    d->last_seen = time( NULL );
    /* what it said of its batteries is old now */
    memset( d->battery, 0xff, sizeof(d->battery) );
    d->charging = 0;
    d->buds_seen = 0;
    TRACE( "%s disconnected (reason %u)\n", debugstr_a( d->name->str ), len > sizeof(*addr) ? data[sizeof(*addr)] : 0 );
    post_device_changed( d, WINEBLUETOOTH_DEVICE_PROPERTY_CONNECTED | WINEBLUETOOTH_DEVICE_PROPERTY_AUDIO |
                            WINEBLUETOOTH_DEVICE_PROPERTY_BATTERY );
    update_monitor( a );
}

static void reply_to( struct adapter *a, UINT16 opcode, const struct mgmt_addr_info *addr )
{
    mgmt_post( opcode, a->index, addr, sizeof(*addr), NULL, NULL );
}

static void on_user_confirm( struct adapter *a, const BYTE *data, UINT16 len )
{
    const struct mgmt_addr_info *addr = (const void *)data;
    struct device *d;
    BYTE hint;
    UINT32 value;

    if (len < sizeof(*addr) + 5) return;
    hint = data[sizeof(*addr)];
    memcpy( &value, data + sizeof(*addr) + 1, 4 );
    if (!(d = find_device( a, &addr->bdaddr )) && !(d = new_device( a, &addr->bdaddr ))) return;
    if (hint)
    {
        /* "just works": no number to compare. Taken when this side began the
         * pairing, the device is known, or the PC is visible (its Bluetooth
         * page is open), refused otherwise */
        BOOL take = d->pairing_irp || d->paired || (a->settings & MGMT_SETTING_DISCOVERABLE);
        TRACE( "%s: just works, %s\n", debugstr_a( d->name->str ), take ? "taken" : "refused" );
        reply_to( a, take ? MGMT_OP_USER_CONFIRM_REPLY : MGMT_OP_USER_CONFIRM_NEG_REPLY, addr );
        return;
    }
    post_auth( d, BLUETOOTH_AUTHENTICATION_METHOD_NUMERIC_COMPARISON, value );
}

static void on_auth_request( struct adapter *a, const BYTE *data, UINT16 len, BLUETOOTH_AUTHENTICATION_METHOD method )
{
    const struct mgmt_addr_info *addr = (const void *)data;
    struct device *d;
    UINT32 value = 0;

    if (len < sizeof(*addr)) return;
    if (method == BLUETOOTH_AUTHENTICATION_METHOD_PASSKEY_NOTIFICATION)
    {
        if (len < sizeof(*addr) + 5) return;
        /* once, when the device has had no key typed yet */
        if (data[sizeof(*addr) + 4]) return;
        memcpy( &value, data + sizeof(*addr), 4 );
    }
    if (!(d = find_device( a, &addr->bdaddr )) && !(d = new_device( a, &addr->bdaddr ))) return;
    post_auth( d, method, value );
}

static void on_new_link_key( struct adapter *a, const BYTE *data, UINT16 len )
{
    const struct mgmt_link_key_info *key = (const void *)(data + 1);
    union winebluetooth_watcher_event_data ev = {0};
    struct device *d;

    if (len < 1 + sizeof(*key) || key->addr.type != BDADDR_BREDR) return;
    if (!(d = find_device( a, &key->addr.bdaddr )) && !(d = new_device( a, &key->addr.bdaddr ))) return;
    if (!d->paired)
    {
        d->paired = TRUE;
        post_device_changed( d, WINEBLUETOOTH_DEVICE_PROPERTY_PAIRED | WINEBLUETOOTH_DEVICE_PROPERTY_TRUSTED );
    }
    /* store_hint: a key to keep (a bonding), not one for this connection */
    if (!data[0]) return;
    ev.new_link_key.radio.handle = (UINT_PTR)unix_name_dup( a->name );
    addr_to_props( &key->addr.bdaddr, &ev.new_link_key.address );
    memcpy( ev.new_link_key.key, key->val, 16 );
    ev.new_link_key.key_type = key->type;
    ev.new_link_key.pin_length = key->pin_len;
    watcher_event( BLUETOOTH_WATCHER_EVENT_TYPE_NEW_LINK_KEY, &ev );
}

static void on_event( UINT16 code, UINT16 index, const BYTE *data, UINT16 len )
{
    struct adapter *a;

    if (code == MGMT_EV_CMD_COMPLETE || code == MGMT_EV_CMD_STATUS)
    {
        UINT16 opcode;

        if (len < 3) return;
        opcode = data[0] | (data[1] << 8);
        command_answered( index, opcode, data[2], data + 3, len - 3, code == MGMT_EV_CMD_COMPLETE );
        return;
    }
    if (code == MGMT_EV_INDEX_ADDED)
    {
        /* set up on a thread of its own: it waits for answers this thread reads */
        adapter_added( index );
        return;
    }

    pthread_mutex_lock( &state_lock );
    if (!(a = find_adapter_index( index )))
    {
        pthread_mutex_unlock( &state_lock );
        return;
    }
    switch (code)
    {
    case MGMT_EV_INDEX_REMOVED:
    {
        struct device *d, *next;
        union winebluetooth_watcher_event_data ev = {0};

        LIST_FOR_EACH_ENTRY_SAFE( d, next, &a->devices, struct device, entry ) free_device( d );
        list_remove( &a->entry );
        ev.radio_removed.handle = (UINT_PTR)a->name;   /* our reference goes with it */
        watcher_event( BLUETOOTH_WATCHER_EVENT_TYPE_RADIO_REMOVED, &ev );
        free( a );
        break;
    }
    case MGMT_EV_NEW_SETTINGS:
        if (len >= 4) on_settings( a, data[0] | (data[1] << 8) | (data[2] << 16) | ((UINT32)data[3] << 24) );
        break;
    case MGMT_EV_CLASS_OF_DEV_CHANGED:
        if (len >= 3)
        {
            a->class = data[0] | (data[1] << 8) | (data[2] << 16);
            post_radio_changed( a, WINEBLUETOOTH_RADIO_PROPERTY_CLASS );
        }
        break;
    case MGMT_EV_LOCAL_NAME_CHANGED:
        if (len >= MGMT_MAX_NAME_LENGTH)
        {
            copy_str( a->local_name, (const char *)data, sizeof(a->local_name) );
            post_radio_changed( a, WINEBLUETOOTH_RADIO_PROPERTY_NAME );
        }
        break;
    case MGMT_EV_DISCOVERING:
        if (len >= 2 && (data[0] & 0x01))
        {
            a->discovering = data[1];
            post_radio_changed( a, WINEBLUETOOTH_RADIO_PROPERTY_DISCOVERING );
            /* an inquiry lasts ~10 s: a search Windows asked for goes on until stopped */
            if (!a->discovering && a->discovery_wanted && !a->pairings && (a->settings & MGMT_SETTING_POWERED))
                start_discovery( a );
        }
        break;
    case MGMT_EV_DEVICE_FOUND:
        on_device_found( a, data, len );
        break;
    case MGMT_EV_ADV_MONITOR_DEVICE_FOUND:
        if (len >= 2 + sizeof(struct mgmt_ev_device_found))
        {
            const struct mgmt_ev_device_found *ev = (const void *)(data + 2);
            if (len >= 2 + sizeof(*ev) + ev->eir_len) on_advertisement( a, &ev->addr, ev->rssi, ev->eir, ev->eir_len );
        }
        break;
    case MGMT_EV_CONNECT_FAILED:
        if (len >= sizeof(struct mgmt_addr_info) + 1)
        {
            const bdaddr_t *b = &((const struct mgmt_addr_info *)data)->bdaddr;
            TRACE( "%02X:%02X:%02X:%02X:%02X:%02X: connection failed (%#x)\n", b->b[5], b->b[4], b->b[3], b->b[2],
                   b->b[1], b->b[0], data[sizeof(struct mgmt_addr_info)] );
        }
        break;
    case MGMT_EV_AUTH_FAILED:
        if (len >= sizeof(struct mgmt_addr_info) + 1)
        {
            const bdaddr_t *b = &((const struct mgmt_addr_info *)data)->bdaddr;
            TRACE( "%02X:%02X:%02X:%02X:%02X:%02X: authentication failed (%#x)\n", b->b[5], b->b[4], b->b[3], b->b[2],
                   b->b[1], b->b[0], data[sizeof(struct mgmt_addr_info)] );
        }
        break;
    case MGMT_EV_DEVICE_CONNECTED:
        on_connected( a, data, len );
        break;
    case MGMT_EV_DEVICE_DISCONNECTED:
        on_disconnected( a, data, len );
        if (len >= sizeof(struct mgmt_addr_info))
        {
            BYTE addr[6];

            memcpy( addr, data, 6 );
            pthread_mutex_unlock( &state_lock );
            profiles_device_gone( addr );
            return;
        }
        break;
    case MGMT_EV_PIN_CODE_REQUEST:
        on_auth_request( a, data, len, BLUETOOTH_AUTHENTICATION_METHOD_LEGACY );
        break;
    case MGMT_EV_USER_CONFIRM_REQUEST:
        on_user_confirm( a, data, len );
        break;
    case MGMT_EV_USER_PASSKEY_REQUEST:
        on_auth_request( a, data, len, BLUETOOTH_AUTHENTICATION_METHOD_PASSKEY );
        break;
    case MGMT_EV_PASSKEY_NOTIFY:
        on_auth_request( a, data, len, BLUETOOTH_AUTHENTICATION_METHOD_PASSKEY_NOTIFICATION );
        break;
    case MGMT_EV_NEW_LINK_KEY:
        on_new_link_key( a, data, len );
        break;
    case MGMT_EV_DEVICE_UNPAIRED:
        if (len >= sizeof(struct mgmt_addr_info))
        {
            struct device *d = find_device( a, &((const struct mgmt_addr_info *)data)->bdaddr );
            if (d && d->paired)
            {
                d->paired = FALSE;
                post_device_changed( d, WINEBLUETOOTH_DEVICE_PROPERTY_PAIRED | WINEBLUETOOTH_DEVICE_PROPERTY_TRUSTED );
            }
        }
        break;
    }
    pthread_mutex_unlock( &state_lock );
}

/* devices found once and not since go, as Windows' list forgets them */
static void forget_stale(void)
{
    struct adapter *a;
    struct device *d, *next;
    time_t now = time( NULL );

    pthread_mutex_lock( &state_lock );
    LIST_FOR_EACH_ENTRY( a, &adapters, struct adapter, entry )
        LIST_FOR_EACH_ENTRY_SAFE( d, next, &a->devices, struct device, entry )
        {
            if (d->paired || d->connected || d->pairing_irp || now - d->last_seen < STALE_SECONDS) continue;
            post_device_removed( d );
            free_device( d );
        }
    pthread_mutex_unlock( &state_lock );
}

static void *reader_proc( void *arg )
{
    BYTE buf[4096];

    while (!shutting_down)
    {
        struct pollfd pfd = { mgmt_fd, POLLIN };
        struct mgmt_hdr hdr;
        ssize_t len;
        int r = poll( &pfd, 1, 10000 );

        if (r < 0 && errno == EINTR) continue;
        if (r < 0) break;
        if (!r)
        {
            forget_stale();
            continue;
        }
        len = read( mgmt_fd, buf, sizeof(buf) );
        if (len < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (len < (ssize_t)sizeof(hdr)) break;
        memcpy( &hdr, buf, sizeof(hdr) );
        on_event( hdr.opcode, hdr.index, buf + sizeof(hdr), min( hdr.len, len - sizeof(hdr) ) );
    }
    if (!shutting_down) ERR( "the management channel closed\n" );
    return NULL;
}

/**********************************************************************
 *          Setting a controller up
 */

static UINT32 settings_of( const BYTE *reply, UINT16 len )
{
    return len >= 4 ? reply[0] | (reply[1] << 8) | (reply[2] << 16) | ((UINT32)reply[3] << 24) : 0;
}

static UINT8 set_mode( UINT16 index, UINT16 opcode, BYTE value, UINT32 *settings )
{
    BYTE reply[4];
    UINT16 len = sizeof(reply);
    UINT8 status = mgmt_call( opcode, index, &value, 1, reply, &len );

    if (!status && settings) *settings = settings_of( reply, len );
    return status;
}

/* a laptop says so: its class of device is "computer, laptop" as Windows' is */
static BOOL has_battery(void)
{
    DIR *dir = opendir( "/sys/class/power_supply" );
    struct dirent *e;
    BOOL ret = FALSE;

    if (!dir) return FALSE;
    while (!ret && (e = readdir( dir )))
    {
        char path[300], type[16] = "";
        FILE *f;

        if (e->d_name[0] == '.') continue;
        snprintf( path, sizeof(path), "/sys/class/power_supply/%s/type", e->d_name );
        if ((f = fopen( path, "r" )))
        {
            if (fgets( type, sizeof(type), f ) && !strncmp( type, "Battery", 7 )) ret = TRUE;
            fclose( f );
        }
    }
    closedir( dir );
    return ret;
}

/* the services this side serves, for the class of device and the EIR's list */
static void add_uuid16( UINT16 index, UINT16 uuid, BYTE hint )
{
    BYTE cp[17] = { 0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00 };

    cp[12] = uuid & 0xff;
    cp[13] = uuid >> 8;
    cp[16] = hint;
    mgmt_call( MGMT_OP_ADD_UUID, index, cp, sizeof(cp), NULL, NULL );
}

static void adapter_setup( UINT16 index )
{
    struct mgmt_rp_read_info info;
    UINT16 len = sizeof(info);
    BYTE io = MGMT_IO_KEYBOARD_DISPLAY, dev_class[2];
    struct adapter *a;
    char path[32];
    UINT32 settings = 0;

    if (mgmt_call( MGMT_OP_READ_INFO, index, NULL, 0, &info, &len ) || len < sizeof(info))
    {
        WARN( "controller %u: no information\n", index );
        return;
    }
    /* Bluetooth Low Energy alone: not yet */
    if (!(info.supported_settings & MGMT_SETTING_BREDR)) return;

    if (!(a = calloc( 1, sizeof(*a) ))) return;
    snprintf( path, sizeof(path), "/hci%u", index );
    if (!(a->name = unix_name_get_or_create( path )))
    {
        free( a );
        return;
    }
    a->index = index;
    a->addr = info.bdaddr;
    a->version = info.version;
    a->manufacturer = info.manufacturer;
    a->settings = info.current_settings;
    a->class = info.dev_class[0] | (info.dev_class[1] << 8) | (info.dev_class[2] << 16);
    copy_str( a->local_name, (const char *)info.name, sizeof(a->local_name) );
    list_init( &a->devices );

    /* as Windows has its radio: Secure Simple Pairing, pairable, connectable
     * (headphones come back by themselves), not visible until asked */
    if (!(a->settings & MGMT_SETTING_SSP)) set_mode( index, MGMT_OP_SET_SSP, 1, &settings );
    /* Low Energy on too, as Windows has it (the earbuds' batteries are heard over it) */
    if ((info.supported_settings & MGMT_SETTING_LE) && !(a->settings & MGMT_SETTING_LE))
        set_mode( index, MGMT_OP_SET_LE, 1, &settings );
    set_mode( index, MGMT_OP_SET_BONDABLE, 1, &settings );
    set_mode( index, MGMT_OP_SET_CONNECTABLE, 1, &settings );
    mgmt_call( MGMT_OP_SET_IO_CAPABILITY, index, &io, 1, NULL, NULL );
    dev_class[0] = 0x01;                          /* computer */
    dev_class[1] = has_battery() ? 0x03 : 0x01;   /* laptop, desktop */
    mgmt_call( MGMT_OP_SET_DEV_CLASS, index, dev_class, 2, NULL, NULL );
    add_uuid16( index, 0x110a, 0x08 );            /* Audio Source: capturing */
    add_uuid16( index, 0x110c, 0x00 );            /* A/V Remote Control Target */
    add_uuid16( index, 0x110e, 0x00 );            /* A/V Remote Control */
    add_uuid16( index, 0x111f, 0x00 );            /* Handsfree Audio Gateway */
    slow_monitor_scan( index );
    if (settings) a->settings = settings;
    TRACE( "controller %u: %02X:%02X:%02X:%02X:%02X:%02X, settings %#x\n", index, a->addr.b[5], a->addr.b[4],
           a->addr.b[3], a->addr.b[2], a->addr.b[1], a->addr.b[0], a->settings );

    pthread_mutex_lock( &state_lock );
    list_add_tail( &adapters, &a->entry );
    post_radio_added( a );
    pthread_mutex_unlock( &state_lock );
}

static void *adapter_added_proc( void *arg )
{
    adapter_setup( (UINT16)(UINT_PTR)arg );
    return NULL;
}

static void adapter_added( UINT16 index )
{
    pthread_t thread;

    pthread_mutex_lock( &state_lock );
    if (find_adapter_index( index ))
    {
        pthread_mutex_unlock( &state_lock );
        return;
    }
    pthread_mutex_unlock( &state_lock );
    if (!pthread_create( &thread, NULL, adapter_added_proc, (void *)(UINT_PTR)index )) pthread_detach( thread );
}

NTSTATUS hci_init( void )
{
    BYTE reply[2 + 2 * 16];
    UINT16 len = sizeof(reply), count;

    /* the helper starts with the system; the driver may come first */
    for (int tries = 0; tries < 50 && mgmt_fd < 0; tries++)
    {
        if ((mgmt_fd = bt_helper( "mgmt" )) > 0) break;
        mgmt_fd = -1;
        usleep( 100000 );
    }
    if (mgmt_fd < 0)
    {
        /* without the helper: only with the rights of root (tests) */
        struct { sa_family_t family; unsigned short dev, channel; } addr = { AF_BLUETOOTH, MGMT_INDEX_NONE, 3 };

        if ((mgmt_fd = socket( AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI )) >= 0 &&
            bind( mgmt_fd, (struct sockaddr *)&addr, sizeof(addr) ))
        {
            close( mgmt_fd );
            mgmt_fd = -1;
        }
    }
    if (mgmt_fd < 0)
    {
        ERR( "no management channel of the Bluetooth controllers\n" );
        return STATUS_NOT_SUPPORTED;
    }
    if (pthread_create( &reader, NULL, reader_proc, NULL ))
    {
        close( mgmt_fd );
        mgmt_fd = -1;
        return STATUS_NO_MEMORY;
    }
    if (!mgmt_call( MGMT_OP_READ_INDEX_LIST, MGMT_INDEX_NONE, NULL, 0, reply, &len ) && len >= 2)
    {
        count = reply[0] | (reply[1] << 8);
        for (UINT16 i = 0; i < count && 2 + 2 * i + 1 < len; i++)
            adapter_setup( reply[2 + 2 * i] | (reply[3 + 2 * i] << 8) );
    }
    profiles_init();
    return STATUS_SUCCESS;
}

void hci_shutdown( void )
{
    shutting_down = TRUE;
    profiles_shutdown();
    pthread_mutex_lock( &events_lock );
    pthread_cond_broadcast( &events_cond );
    pthread_mutex_unlock( &events_lock );
    if (mgmt_fd >= 0) shutdown( mgmt_fd, SHUT_RDWR );
}

/**********************************************************************
 *          What winebth.c asks
 */

NTSTATUS hci_adapter_load( struct bluetooth_adapter_load_params *params )
{
    struct mgmt_link_key_info *key;
    struct adapter *a;
    BYTE name[MGMT_MAX_NAME_LENGTH + MGMT_MAX_SHORT_NAME_LENGTH] = {0}, *keys;
    UINT16 index, size;
    UINT count = 0;

    pthread_mutex_lock( &state_lock );
    if (!(a = find_adapter_name( params->adapter )))
    {
        pthread_mutex_unlock( &state_lock );
        return STATUS_NOT_FOUND;
    }
    index = a->index;
    /* the devices Windows knows, as it knew them */
    for (UINT i = 0; i < params->count; i++)
    {
        const struct winebluetooth_known_device *known = &params->devices[i];
        bdaddr_t addr;
        struct device *d;

        props_to_addr( &known->address, &addr );
        if ((d = find_device( a, &addr ))) continue;
        if (!(d = new_device( a, &addr ))) continue;
        copy_str( d->dev_name, known->name, sizeof(d->dev_name) );
        d->class = known->class;
        d->paired = TRUE;
        post_device_added( d, TRUE );
    }
    a->loaded = TRUE;
    pthread_mutex_unlock( &state_lock );

    /* the computer's name, as other devices list it */
    if (params->name[0])
    {
        copy_str( (char *)name, params->name, MGMT_MAX_NAME_LENGTH );
        copy_str( (char *)name + MGMT_MAX_NAME_LENGTH, params->name, MGMT_MAX_SHORT_NAME_LENGTH );
        mgmt_call( MGMT_OP_SET_LOCAL_NAME, index, name, sizeof(name), NULL, NULL );
    }

    /* their keys: the kernel takes them all at once */
    size = 3 + params->count * sizeof(*key);
    if ((keys = calloc( 1, size )))
    {
        for (UINT i = 0; i < params->count; i++)
        {
            const struct winebluetooth_known_device *known = &params->devices[i];

            if (!memcmp( known->key, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16 )) continue;
            key = (struct mgmt_link_key_info *)(keys + 3) + count++;
            props_to_addr( &known->address, &key->addr.bdaddr );
            key->addr.type = BDADDR_BREDR;
            key->type = known->key_type;
            memcpy( key->val, known->key, 16 );
            key->pin_len = known->pin_length;
        }
        keys[0] = 0;   /* no debug keys */
        keys[1] = count & 0xff;
        keys[2] = count >> 8;
        if (mgmt_call( MGMT_OP_LOAD_LINK_KEYS, index, keys, 3 + count * sizeof(*key), NULL, NULL ))
            WARN( "controller %u: the link keys were not taken\n", index );
        free( keys );
    }
    TRACE( "controller %u: %u devices, %u keys, %s\n", index, params->count, count, params->powered ? "on" : "off" );
    return hci_adapter_set_power( params->adapter, params->powered );
}

NTSTATUS hci_adapter_set_power( struct unix_name *name, BOOL on )
{
    struct adapter *a;
    UINT32 settings;
    UINT16 index;
    UINT8 status;

    pthread_mutex_lock( &state_lock );
    if (!(a = find_adapter_name( name )))
    {
        pthread_mutex_unlock( &state_lock );
        return STATUS_NOT_FOUND;
    }
    index = a->index;
    pthread_mutex_unlock( &state_lock );

    status = set_mode( index, MGMT_OP_SET_POWERED, on, &settings );
    if (status == MGMT_STATUS_RFKILLED && on && bt_helper( "unblock" ) >= 0)
    {
        usleep( 200000 );
        status = set_mode( index, MGMT_OP_SET_POWERED, on, &settings );
    }
    if (status) return status_from_mgmt( status );
    pthread_mutex_lock( &state_lock );
    if ((a = find_adapter_index( index ))) on_settings( a, settings );
    pthread_mutex_unlock( &state_lock );
    return STATUS_SUCCESS;
}

NTSTATUS hci_adapter_set_prop( struct bluetooth_adapter_set_prop_params *params )
{
    struct adapter *a;
    UINT16 index;
    UINT32 settings = 0;
    UINT8 status = MGMT_STATUS_NOT_SUPPORTED;
    BOOL on = params->prop->boolean;

    pthread_mutex_lock( &state_lock );
    if (!(a = find_adapter_name( params->adapter )))
    {
        pthread_mutex_unlock( &state_lock );
        return STATUS_NOT_FOUND;
    }
    index = a->index;
    pthread_mutex_unlock( &state_lock );

    switch (params->prop_flag)
    {
    case LOCAL_RADIO_DISCOVERABLE:
    {
        BYTE cp[3] = { on, 0, 0 }, reply[4];
        UINT16 len = sizeof(reply);

        /* visible means connectable too */
        if (on) set_mode( index, MGMT_OP_SET_CONNECTABLE, 1, NULL );
        status = mgmt_call( MGMT_OP_SET_DISCOVERABLE, index, cp, sizeof(cp), reply, &len );
        if (!status) settings = settings_of( reply, len );
        break;
    }
    case LOCAL_RADIO_CONNECTABLE:
        status = set_mode( index, MGMT_OP_SET_CONNECTABLE, on, &settings );
        break;
    }
    if (status) return status_from_mgmt( status );
    pthread_mutex_lock( &state_lock );
    if ((a = find_adapter_index( index ))) on_settings( a, settings );
    pthread_mutex_unlock( &state_lock );
    return STATUS_SUCCESS;
}

NTSTATUS hci_adapter_start_discovery( struct unix_name *name )
{
    struct adapter *a;
    BYTE type = 0x01;
    UINT16 index;
    UINT8 status;

    pthread_mutex_lock( &state_lock );
    if (!(a = find_adapter_name( name )))
    {
        pthread_mutex_unlock( &state_lock );
        return STATUS_NOT_FOUND;
    }
    a->discovery_wanted = TRUE;
    index = a->index;
    if (!(a->settings & MGMT_SETTING_POWERED) || a->pairings)
    {
        /* it starts when the radio is on, the pairing over */
        pthread_mutex_unlock( &state_lock );
        return STATUS_SUCCESS;
    }
    pthread_mutex_unlock( &state_lock );
    status = mgmt_call( MGMT_OP_START_DISCOVERY, index, &type, 1, NULL, NULL );
    return status == MGMT_STATUS_BUSY ? STATUS_SUCCESS : status_from_mgmt( status );
}

NTSTATUS hci_adapter_stop_discovery( struct unix_name *name )
{
    struct adapter *a;
    BYTE type = 0x01;
    UINT16 index;

    pthread_mutex_lock( &state_lock );
    if (!(a = find_adapter_name( name )))
    {
        pthread_mutex_unlock( &state_lock );
        return STATUS_NOT_FOUND;
    }
    a->discovery_wanted = FALSE;
    index = a->index;
    pthread_mutex_unlock( &state_lock );
    mgmt_call( MGMT_OP_STOP_DISCOVERY, index, &type, 1, NULL, NULL );
    return STATUS_SUCCESS;
}

NTSTATUS hci_adapter_remove_device( struct unix_name *adapter, struct unix_name *name )
{
    BYTE cp[sizeof(struct mgmt_addr_info) + 1];
    struct mgmt_addr_info *addr = (struct mgmt_addr_info *)cp;
    struct device *d;
    UINT16 index;
    UINT8 status;

    pthread_mutex_lock( &state_lock );
    if (!(d = find_device_name( name )))
    {
        pthread_mutex_unlock( &state_lock );
        return STATUS_NOT_FOUND;
    }
    index = d->adapter->index;
    addr->bdaddr = d->addr;
    addr->type = BDADDR_BREDR;
    pthread_mutex_unlock( &state_lock );

    profiles_disconnect( addr->bdaddr.b );
    cp[sizeof(*addr)] = 1;   /* and disconnect it */
    status = mgmt_call( MGMT_OP_UNPAIR_DEVICE, index, cp, sizeof(cp), NULL, NULL );
    if (status && status != MGMT_STATUS_NOT_PAIRED) return status_from_mgmt( status );

    /* forgotten, as Windows forgets a removed device */
    pthread_mutex_lock( &state_lock );
    if ((d = find_device_name( name )))
    {
        post_device_removed( d );
        free_device( d );
    }
    pthread_mutex_unlock( &state_lock );
    return STATUS_SUCCESS;
}

NTSTATUS hci_device_disconnect( struct unix_name *name )
{
    struct mgmt_addr_info addr;
    struct device *d;
    UINT16 index;
    UINT8 status;

    pthread_mutex_lock( &state_lock );
    if (!(d = find_device_name( name )))
    {
        pthread_mutex_unlock( &state_lock );
        return STATUS_NOT_FOUND;
    }
    index = d->adapter->index;
    addr.bdaddr = d->addr;
    addr.type = BDADDR_BREDR;
    pthread_mutex_unlock( &state_lock );

    /* the profiles say goodbye first: the headset hears the stream close */
    profiles_disconnect( addr.bdaddr.b );
    status = mgmt_call( MGMT_OP_DISCONNECT, index, &addr, sizeof(addr), NULL, NULL );
    return status == MGMT_STATUS_NOT_CONNECTED ? STATUS_SUCCESS : status_from_mgmt( status );
}

NTSTATUS hci_device_connect( struct unix_name *name )
{
    struct device *d;
    bdaddr_t addr;

    pthread_mutex_lock( &state_lock );
    if (!(d = find_device_name( name )))
    {
        pthread_mutex_unlock( &state_lock );
        return STATUS_NOT_FOUND;
    }
    if (!(d->adapter->settings & MGMT_SETTING_POWERED))
    {
        pthread_mutex_unlock( &state_lock );
        return STATUS_DEVICE_NOT_READY;
    }
    addr = d->addr;
    pthread_mutex_unlock( &state_lock );
    connect_profiles( &addr );
    return STATUS_SUCCESS;
}

static void pairing_done( UINT16 index, UINT8 status, const BYTE *reply, UINT16 len, void *context )
{
    struct unix_name *name = context;
    union winebluetooth_watcher_event_data ev = {0};
    struct device *d;
    bdaddr_t addr;
    BOOL audio = FALSE;

    pthread_mutex_lock( &state_lock );
    if ((d = find_device_name( name )))
    {
        ev.pairing_finished.irp = d->pairing_irp;
        d->pairing_irp = NULL;
        if (!status && !d->paired)
        {
            d->paired = TRUE;
            post_device_changed( d, WINEBLUETOOTH_DEVICE_PROPERTY_PAIRED | WINEBLUETOOTH_DEVICE_PROPERTY_TRUSTED );
        }
        addr = d->addr;
        audio = !status && is_audio_class( d->class );
        if (--d->adapter->pairings <= 0)
        {
            d->adapter->pairings = 0;
            if (d->adapter->discovery_wanted && (d->adapter->settings & MGMT_SETTING_POWERED))
                start_discovery( d->adapter );
        }
    }
    pthread_mutex_unlock( &state_lock );
    TRACE( "pairing with %s: %#x\n", debugstr_a( name->str ), status );
    unix_name_free( name );

    if (ev.pairing_finished.irp)
    {
        ev.pairing_finished.result = status_from_mgmt( status );
        watcher_event( BLUETOOTH_WATCHER_EVENT_TYPE_PAIRING_FINISHED, &ev );
    }
    /* headphones play at once, as in Windows */
    if (audio) connect_profiles( &addr );
}

NTSTATUS hci_device_start_pairing( struct unix_name *name, IRP *irp )
{
    BYTE cp[sizeof(struct mgmt_addr_info) + 1], type = 0x01;
    struct mgmt_addr_info *addr = (struct mgmt_addr_info *)cp;
    struct device *d;
    UINT16 index;
    BOOL stop;

    pthread_mutex_lock( &state_lock );
    if (!(d = find_device_name( name )))
    {
        pthread_mutex_unlock( &state_lock );
        return STATUS_DEVICE_DOES_NOT_EXIST;
    }
    if (d->pairing_irp)
    {
        pthread_mutex_unlock( &state_lock );
        return STATUS_DEVICE_BUSY;
    }
    if (!(d->adapter->settings & MGMT_SETTING_POWERED))
    {
        pthread_mutex_unlock( &state_lock );
        return STATUS_DEVICE_NOT_READY;
    }
    d->pairing_irp = irp;
    stop = d->adapter->discovering;
    d->adapter->pairings++;
    index = d->adapter->index;
    addr->bdaddr = d->addr;
    addr->type = BDADDR_BREDR;
    cp[sizeof(*addr)] = MGMT_IO_KEYBOARD_DISPLAY;
    pthread_mutex_unlock( &state_lock );

    /* an inquiry slows paging down: it waits for the pairing */
    if (stop) mgmt_post( MGMT_OP_STOP_DISCOVERY, index, &type, 1, NULL, NULL );
    mgmt_post( MGMT_OP_PAIR_DEVICE, index, cp, sizeof(cp), pairing_done, unix_name_dup( name ) );
    return STATUS_PENDING;
}

NTSTATUS hci_auth_send_response( struct bluetooth_auth_send_response_params *params )
{
    BYTE cp[sizeof(struct mgmt_addr_info) + 17] = {0};
    struct mgmt_addr_info *addr = (struct mgmt_addr_info *)cp;
    UINT16 opcode, len = sizeof(*addr), index;
    struct device *d;
    UINT8 status;

    pthread_mutex_lock( &state_lock );
    if (!(d = find_device_name( params->device )))
    {
        pthread_mutex_unlock( &state_lock );
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    index = d->adapter->index;
    addr->bdaddr = d->addr;
    addr->type = BDADDR_BREDR;
    pthread_mutex_unlock( &state_lock );

    switch (params->method)
    {
    case BLUETOOTH_AUTHENTICATION_METHOD_NUMERIC_COMPARISON:
        opcode = params->negative ? MGMT_OP_USER_CONFIRM_NEG_REPLY : MGMT_OP_USER_CONFIRM_REPLY;
        break;
    case BLUETOOTH_AUTHENTICATION_METHOD_PASSKEY:
        opcode = params->negative ? MGMT_OP_USER_PASSKEY_NEG_REPLY : MGMT_OP_USER_PASSKEY_REPLY;
        if (!params->negative)
        {
            memcpy( cp + len, &params->numeric_or_passkey, 4 );
            len += 4;
        }
        break;
    case BLUETOOTH_AUTHENTICATION_METHOD_LEGACY:
        opcode = params->negative || !params->pin_length ? MGMT_OP_PIN_CODE_NEG_REPLY : MGMT_OP_PIN_CODE_REPLY;
        if (opcode == MGMT_OP_PIN_CODE_REPLY)
        {
            cp[len] = min( params->pin_length, 16 );
            memcpy( cp + len + 1, params->pin, cp[len] );
            len += 17;
        }
        break;
    case BLUETOOTH_AUTHENTICATION_METHOD_PASSKEY_NOTIFICATION:
        /* the key is typed on the device: nothing to answer */
        *params->authenticated = !params->negative;
        return STATUS_SUCCESS;
    default:
        return STATUS_NOT_SUPPORTED;
    }
    status = mgmt_call( opcode, index, cp, len, NULL, NULL );
    *params->authenticated = !status && !params->negative;
    return status_from_mgmt( status );
}
