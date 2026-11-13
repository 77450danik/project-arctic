/*
 * Bluetooth bus: sending files (OBEX Object Push)
 *
 * What Windows' fsquirt.exe ("Bluetooth File Transfer") does through its
 * own stack: the device's Object Push service is looked up over SDP (its
 * RFCOMM channel), connected to over RFCOMM, and the file put to it in OBEX
 * packets of the size the device takes (IrDA's OBEX 1.x: CONNECT, PUT with
 * Name, Length and Body headers, the last one final with End of Body,
 * DISCONNECT). The phone asks its owner to take the file. One transfer at a
 * time; fsquirt asks how far it is and may cancel it.
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

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <winternl.h>
#include <winbase.h>
#include <windef.h>
#include <wine/debug.h>

#include "unixlib.h"
#include "unixlib_priv.h"
#include "bt.h"

WINE_DEFAULT_DEBUG_CHANNEL( winebth );

/* What the stack does is always in the log (nt.log): connections, the
 * streams set up, what failed; the system runs with WINEDEBUG=fixme-all */
#undef TRACE
#undef WARN
#define TRACE( ... ) fprintf( stderr, "winebth: " __VA_ARGS__ )
#define WARN( ... ) fprintf( stderr, "winebth: " __VA_ARGS__ )

static pthread_mutex_t obex_lock = PTHREAD_MUTEX_INITIALIZER;
static BOOL busy, cancelled;
static UINT64 sent, total;

struct job
{
    bdaddr_t addr;
    char path[1024];
    IRP *irp;
};

static void set_timeouts( int fd, int seconds )
{
    struct timeval tv = { seconds, 0 };

    setsockopt( fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv) );
    setsockopt( fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv) );
}

/* the RFCOMM channel of a device's service (Object Push 0x1105, Hands-Free 0x111e), from its SDP records */
int sdp_rfcomm_channel( const void *addr, UINT16 uuid )
{
    struct sockaddr_l2 sa = { .l2_family = AF_BLUETOOTH, .l2_psm = PSM_SDP, .l2_bdaddr = *(const bdaddr_t *)addr,
                              .l2_bdaddr_type = BDADDR_BREDR };
    BYTE request[32], answer[1024], lists[4096];
    UINT lists_len = 0, cont_len = 0;
    BYTE cont[17];
    int fd, channel = -1;

    if ((fd = socket( AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_CLOEXEC, BTPROTO_L2CAP )) < 0) return -1;
    set_timeouts( fd, 10 );
    if (connect( fd, (struct sockaddr *)&sa, sizeof(sa) ))
    {
        WARN( "SDP: %s\n", strerror( errno ) );
        close( fd );
        return -1;
    }
    for (int round = 0; round < 16; round++)
    {
        /* ServiceSearchAttributeRequest: the service, ProtocolDescriptorList */
        const BYTE body[] = { 0x35, 0x03, 0x19, uuid >> 8, uuid & 0xff, 0x03, 0xf0, 0x35, 0x03, 0x09, 0x00, 0x04 };
        UINT len = 0, plen;
        ssize_t got;

        request[len++] = 0x06;
        request[len++] = 0x00;
        request[len++] = round + 1;
        plen = sizeof(body) + 1 + cont_len;
        request[len++] = plen >> 8;
        request[len++] = plen;
        memcpy( request + len, body, sizeof(body) );
        len += sizeof(body);
        request[len++] = cont_len;
        memcpy( request + len, cont, cont_len );
        len += cont_len;
        if (send( fd, request, len, MSG_NOSIGNAL ) != (ssize_t)len) break;
        if ((got = recv( fd, answer, sizeof(answer), 0 )) < 7 || answer[0] != 0x07) break;
        {
            UINT count = (answer[5] << 8) | answer[6], pos = 7 + count;

            if (pos >= (UINT)got) break;
            if (lists_len + count <= sizeof(lists))
            {
                memcpy( lists + lists_len, answer + 7, count );
                lists_len += count;
            }
            cont_len = min( answer[pos], sizeof(cont) - 1 );
            if (pos + 1 + cont_len > (UINT)got) break;
            memcpy( cont, answer + pos + 1, cont_len );
            if (!cont_len) break;
        }
    }
    close( fd );
    /* RFCOMM's UUID (0x0003) and its channel after it */
    for (UINT i = 0; i + 4 < lists_len; i++)
        if (lists[i] == 0x19 && lists[i + 1] == 0x00 && lists[i + 2] == 0x03 && lists[i + 3] == 0x08)
        {
            channel = lists[i + 4];
            break;
        }
    TRACE( "service %#x on RFCOMM channel %d\n", uuid, channel );
    return channel;
}

static BOOL write_all( int fd, const BYTE *data, size_t len )
{
    while (len)
    {
        ssize_t n = send( fd, data, len, MSG_NOSIGNAL );
        if (n <= 0) return FALSE;
        data += n;
        len -= n;
    }
    return TRUE;
}

static BOOL read_all( int fd, BYTE *data, size_t len )
{
    while (len)
    {
        ssize_t n = recv( fd, data, len, 0 );
        if (n <= 0) return FALSE;
        data += n;
        len -= n;
    }
    return TRUE;
}

/* a request and the code of its answer (0 if none came); the answer's
 * fields after its length go to extra */
static BYTE obex_request( int fd, BYTE *packet, UINT len, BYTE *extra, UINT extra_size )
{
    BYTE head[3], rest[256];
    UINT answer_len;

    packet[1] = len >> 8;
    packet[2] = len;
    if (!write_all( fd, packet, len ) || !read_all( fd, head, 3 )) return 0;
    answer_len = (head[1] << 8) | head[2];
    if (answer_len < 3) return 0;
    answer_len -= 3;
    while (answer_len)
    {
        UINT n = min( answer_len, sizeof(rest) );
        if (!read_all( fd, rest, n )) return 0;
        if (extra && extra_size)
        {
            memcpy( extra, rest, min( n, extra_size ) );
            extra_size = 0;
        }
        answer_len -= n;
    }
    return head[0];
}

static NTSTATUS push_file( const struct job *job )
{
    struct sockaddr_rc sa = { .rc_family = AF_BLUETOOTH, .rc_bdaddr = job->addr };
    struct bt_security sec = { BT_SECURITY_LOW, 0 };
    BYTE *packet = NULL, extra[8];
    const char *base = strrchr( job->path, '/' );
    UINT peer_max = 255, pos;
    struct stat st;
    int fd = -1, file = -1, channel;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    BYTE code;
    BOOL first = TRUE;

    base = base ? base + 1 : job->path;
    if ((file = open( job->path, O_RDONLY | O_CLOEXEC )) < 0 || fstat( file, &st ))
    {
        WARN( "%s: %s\n", job->path, strerror( errno ) );
        if (file >= 0) close( file );
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    pthread_mutex_lock( &obex_lock );
    total = st.st_size;
    sent = 0;
    pthread_mutex_unlock( &obex_lock );

    if ((channel = sdp_rfcomm_channel( &job->addr, 0x1105 )) < 0)
    {
        status = STATUS_NOT_SUPPORTED;
        goto done;
    }
    sa.rc_channel = channel;
    if ((fd = socket( AF_BLUETOOTH, SOCK_STREAM | SOCK_CLOEXEC, BTPROTO_RFCOMM )) < 0) goto done;
    setsockopt( fd, SOL_BLUETOOTH, BT_SECURITY, &sec, sizeof(sec) );
    /* the phone's owner takes a while to say yes */
    set_timeouts( fd, 60 );
    if (connect( fd, (struct sockaddr *)&sa, sizeof(sa) ))
    {
        WARN( "RFCOMM %d: %s\n", channel, strerror( errno ) );
        status = STATUS_DEVICE_NOT_CONNECTED;
        goto done;
    }

    /* CONNECT: OBEX 1.0, packets up to 8 KB */
    if (!(packet = malloc( 0x2000 ))) goto done;
    packet[0] = 0x80;
    packet[3] = 0x10;
    packet[4] = 0x00;
    packet[5] = 0x20;
    packet[6] = 0x00;
    if ((code = obex_request( fd, packet, 7, extra, sizeof(extra) )) != 0xa0)
    {
        WARN( "OBEX CONNECT: %#x\n", code );
        goto done;
    }
    peer_max = min( 0x2000, max( 255, (extra[2] << 8) | extra[3] ) );

    for (;;)
    {
        UINT len = 3, room;
        ssize_t got;
        BOOL last;

        pthread_mutex_lock( &obex_lock );
        if (cancelled)
        {
            pthread_mutex_unlock( &obex_lock );
            packet[0] = 0xff;   /* ABORT */
            obex_request( fd, packet, 3, NULL, 0 );
            status = STATUS_CANCELLED;
            goto done;
        }
        pthread_mutex_unlock( &obex_lock );

        if (first)
        {
            /* Name (Unicode, big-endian, with its end) and Length */
            WCHAR name[256];
            UINT n = ntdll_umbstowcs( base, strlen( base ), name, ARRAY_SIZE(name) - 1 ), name_len;

            name[n] = 0;
            name_len = 3 + (n + 1) * 2;
            packet[len++] = 0x01;
            packet[len++] = name_len >> 8;
            packet[len++] = name_len;
            for (UINT i = 0; i <= n; i++)
            {
                packet[len++] = name[i] >> 8;
                packet[len++] = name[i];
            }
            packet[len++] = 0xc3;
            packet[len++] = (UINT32)st.st_size >> 24;
            packet[len++] = (UINT32)st.st_size >> 16;
            packet[len++] = (UINT32)st.st_size >> 8;
            packet[len++] = (UINT32)st.st_size;
            first = FALSE;
        }
        room = peer_max - len - 3;
        got = read( file, packet + len + 3, room );
        if (got < 0) goto done;
        pos = lseek( file, 0, SEEK_CUR );
        last = pos >= (UINT)st.st_size;
        packet[0] = last ? 0x82 : 0x02;
        packet[len] = last ? 0x49 : 0x48;     /* End of Body, Body */
        packet[len + 1] = (got + 3) >> 8;
        packet[len + 2] = got + 3;
        len += 3 + got;
        code = obex_request( fd, packet, len, NULL, 0 );
        pthread_mutex_lock( &obex_lock );
        sent = pos;
        pthread_mutex_unlock( &obex_lock );
        if (last)
        {
            status = code == 0xa0 ? STATUS_SUCCESS : code == 0xc3 || code == 0xc1 ? STATUS_ACCESS_DENIED
                                                                                   : STATUS_UNSUCCESSFUL;
            if (status) WARN( "OBEX PUT: %#x\n", code );
            break;
        }
        if (code != 0x90)
        {
            WARN( "OBEX PUT: %#x\n", code );
            status = code == 0xc3 || code == 0xc1 ? STATUS_ACCESS_DENIED : STATUS_UNSUCCESSFUL;
            goto done;
        }
    }
    packet[0] = 0x81;   /* DISCONNECT */
    obex_request( fd, packet, 3, NULL, 0 );

done:
    free( packet );
    if (fd >= 0) close( fd );
    close( file );
    return status;
}

static void *push_proc( void *arg )
{
    struct job *job = arg;
    union winebluetooth_watcher_event_data data = {0};
    struct winebluetooth_event event = { .status = WINEBLUETOOTH_EVENT_WATCHER_EVENT };

    data.pairing_finished.irp = job->irp;
    data.pairing_finished.result = push_file( job );
    TRACE( "%s: %#x\n", debugstr_a( job->path ), (UINT)data.pairing_finished.result );
    pthread_mutex_lock( &obex_lock );
    busy = FALSE;
    pthread_mutex_unlock( &obex_lock );
    /* the IRP ends as a pairing's does */
    event.data.watcher_event.event_type = BLUETOOTH_WATCHER_EVENT_TYPE_PAIRING_FINISHED;
    event.data.watcher_event.event_data = data;
    hci_post_event( &event );
    free( job );
    return NULL;
}

NTSTATUS obex_send( const BYTE addr[6], const char *path, IRP *irp )
{
    struct job *job;
    pthread_t thread;

    pthread_mutex_lock( &obex_lock );
    if (busy)
    {
        pthread_mutex_unlock( &obex_lock );
        return STATUS_DEVICE_BUSY;
    }
    if (!(job = calloc( 1, sizeof(*job) )))
    {
        pthread_mutex_unlock( &obex_lock );
        return STATUS_NO_MEMORY;
    }
    memcpy( &job->addr, addr, 6 );
    snprintf( job->path, sizeof(job->path), "%s", path );
    job->irp = irp;
    busy = TRUE;
    cancelled = FALSE;
    sent = total = 0;
    pthread_mutex_unlock( &obex_lock );
    if (pthread_create( &thread, NULL, push_proc, job ))
    {
        pthread_mutex_lock( &obex_lock );
        busy = FALSE;
        pthread_mutex_unlock( &obex_lock );
        free( job );
        return STATUS_NO_MEMORY;
    }
    pthread_detach( thread );
    return STATUS_PENDING;
}

void obex_progress( UINT64 *done, UINT64 *size )
{
    pthread_mutex_lock( &obex_lock );
    *done = sent;
    *size = total;
    pthread_mutex_unlock( &obex_lock );
}

void obex_cancel( void )
{
    pthread_mutex_lock( &obex_lock );
    cancelled = TRUE;
    pthread_mutex_unlock( &obex_lock );
}
