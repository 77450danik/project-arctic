/*
 * Bluetooth bus: the profiles Windows serves over L2CAP
 *
 * As bthport.sys, BthA2dp.sys and the AVRCP transport of Windows do, and
 * without a daemon of Linux (docs/bluetooth.md):
 *
 * - SDP, the Service Discovery Protocol: the records of what this PC serves
 *   (an Audio Source, the Target of remote control), for the headsets that
 *   ask (PSM 1);
 * - A2DP over AVDTP (PSM 25): either side may set the stream up. This side
 *   connects a paired audio device (after a pairing, on "Connect"), finds its
 *   sink, configures SBC (48 kHz or 44.1, joint stereo, 16 blocks, eight
 *   subbands, loudness, the bitpool up to 53), opens it and its media
 *   channel; or a headset that comes back by itself does the same towards
 *   this side's Source endpoint. Programs play to the sound card
 *   "Bluetooth" (snd-aloop, which arctic-init loads): winealsa.drv shows it
 *   as the headphones while a stream is open (/dev/shm/arctic-bt-audio says
 *   which), what is played comes out of the loopback's capture side, is
 *   coded here and sent in RTP packets. The stream starts when something
 *   plays and is suspended a few seconds after it stops, so that the
 *   headset can sleep;
 * - AVRCP over AVCTP (PSM 23), the Target: the headset's buttons (play,
 *   pause, next, previous, volume) come to the shell as the media keys.
 *
 * The PSMs below 0x1001 are bound by the root helper (bt_helper "listen").
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
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <alsa/asoundlib.h>

#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <winternl.h>
#include <winbase.h>
#include <windef.h>
#include <winuser.h>
#include <wine/debug.h>
#include <wine/list.h>

#include "unixlib.h"
#include "unixlib_priv.h"
#include "bt.h"
#include "sbc.h"

WINE_DEFAULT_DEBUG_CHANNEL( winebth );

/* What the stack does is always in the log (nt.log): connections, the
 * streams set up, what failed; the system runs with WINEDEBUG=fixme-all */
#undef TRACE
#undef WARN
#define TRACE( ... ) fprintf( stderr, "winebth: " __VA_ARGS__ )
#define WARN( ... ) fprintf( stderr, "winebth: " __VA_ARGS__ )

#define AUDIO_STATE      "/dev/shm/arctic-bt-audio"
#define LOOPBACK_CAPTURE "hw:CARD=Bluetooth,DEV=1,SUBDEV=0"
#define LOOPBACK_CARD    "hw:CARD=Bluetooth"
#define SUSPEND_AFTER    4     /* seconds of nothing played */
#define REPLY_TIMEOUT    5     /* seconds for the other side to answer a signal */

static const char *addr_str( const bdaddr_t *a )
{
    return wine_dbg_sprintf( "%02X:%02X:%02X:%02X:%02X:%02X", a->b[5], a->b[4], a->b[3], a->b[2], a->b[1], a->b[0] );
}

static void set_security( int fd, int level )
{
    struct bt_security sec = { level, 0 };
    setsockopt( fd, SOL_BLUETOOTH, BT_SECURITY, &sec, sizeof(sec) );
}

/**********************************************************************
 *          Channels and the thread that reads them
 */

enum channel_kind { CH_SDP, CH_SIGNAL, CH_MEDIA, CH_AVCTP };

struct channel
{
    struct list entry;
    int fd;
    enum channel_kind kind;
    bdaddr_t addr;
};

static struct list channels = LIST_INIT( channels );
static pthread_mutex_t channels_lock = PTHREAD_MUTEX_INITIALIZER;
static int listen_fd[3] = { -1, -1, -1 };   /* SDP, AVCTP, AVDTP */
static int wake_pipe[2] = { -1, -1 };
static pthread_t io_thread;
static BOOL stopping;

static void wake_io(void)
{
    char c = 0;
    if (wake_pipe[1] >= 0) write( wake_pipe[1], &c, 1 );
}

static struct channel *add_channel( int fd, enum channel_kind kind, const bdaddr_t *addr )
{
    struct channel *ch = calloc( 1, sizeof(*ch) );

    if (!ch) return NULL;
    ch->fd = fd;
    ch->kind = kind;
    ch->addr = *addr;
    pthread_mutex_lock( &channels_lock );
    list_add_tail( &channels, &ch->entry );
    pthread_mutex_unlock( &channels_lock );
    wake_io();
    return ch;
}

/* the channel is closed by the reader's thread, which owns the list */
static void drop_channel( struct channel *ch )
{
    pthread_mutex_lock( &channels_lock );
    list_remove( &ch->entry );
    pthread_mutex_unlock( &channels_lock );
    close( ch->fd );
    free( ch );
}

/**********************************************************************
 *          SDP: this PC's records (Bluetooth Core, Vol 3, Part B)
 */

struct sdp_attribute
{
    UINT16 id;
    const BYTE *value;
    UINT len;
};

struct sdp_record
{
    UINT32 handle;
    const UINT16 *uuids;          /* every UUID in it, for the search patterns */
    UINT uuid_count;
    const struct sdp_attribute *attributes;
    UINT count;
};

#define ATTR( id, ... ) { id, (const BYTE []){ __VA_ARGS__ }, sizeof((const BYTE []){ __VA_ARGS__ }) }

static const UINT16 source_uuids[] = { 0x110a, 0x0100, 0x0019, 0x1002, 0x110d };
static const struct sdp_attribute source_attributes[] =
{
    ATTR( 0x0000, 0x0a, 0x00, 0x01, 0x00, 0x01 ),                          /* ServiceRecordHandle */
    ATTR( 0x0001, 0x35, 0x03, 0x19, 0x11, 0x0a ),                          /* Audio Source */
    ATTR( 0x0004, 0x35, 0x10, 0x35, 0x06, 0x19, 0x01, 0x00, 0x09, 0x00, 0x19,
                  0x35, 0x06, 0x19, 0x00, 0x19, 0x09, 0x01, 0x03 ),        /* L2CAP PSM 25, AVDTP 1.3 */
    ATTR( 0x0005, 0x35, 0x03, 0x19, 0x10, 0x02 ),                          /* the public browse group */
    ATTR( 0x0009, 0x35, 0x08, 0x35, 0x06, 0x19, 0x11, 0x0d, 0x09, 0x01, 0x03 ), /* A2DP 1.3 */
    ATTR( 0x0100, 0x25, 0x0c, 'A', 'u', 'd', 'i', 'o', ' ', 'S', 'o', 'u', 'r', 'c', 'e' ),
    ATTR( 0x0311, 0x09, 0x00, 0x01 ),                                      /* a player */
};

static const UINT16 target_uuids[] = { 0x110c, 0x0100, 0x0017, 0x1002, 0x110e };
static const struct sdp_attribute target_attributes[] =
{
    ATTR( 0x0000, 0x0a, 0x00, 0x01, 0x00, 0x02 ),
    ATTR( 0x0001, 0x35, 0x03, 0x19, 0x11, 0x0c ),                          /* A/V Remote Control Target */
    ATTR( 0x0004, 0x35, 0x10, 0x35, 0x06, 0x19, 0x01, 0x00, 0x09, 0x00, 0x17,
                  0x35, 0x06, 0x19, 0x00, 0x17, 0x09, 0x01, 0x00 ),        /* L2CAP PSM 23, AVCTP 1.0 */
    ATTR( 0x0005, 0x35, 0x03, 0x19, 0x10, 0x02 ),
    ATTR( 0x0009, 0x35, 0x08, 0x35, 0x06, 0x19, 0x11, 0x0e, 0x09, 0x01, 0x00 ), /* AVRCP 1.0 */
    ATTR( 0x0100, 0x25, 0x0c, 'A', 'V', 'R', 'C', 'P', ' ', 'T', 'a', 'r', 'g', 'e', 't' ),
    ATTR( 0x0311, 0x09, 0x00, 0x01 ),                                      /* category 1: a player */
};

/* the Audio Gateway of Hands-Free 1.7 (hfp.c): the headset's microphone, its battery */
static const UINT16 gateway_uuids[] = { 0x111f, 0x1203, 0x0100, 0x0003, 0x1002, 0x111e };
static const struct sdp_attribute gateway_attributes[] =
{
    ATTR( 0x0000, 0x0a, 0x00, 0x01, 0x00, 0x03 ),
    ATTR( 0x0001, 0x35, 0x06, 0x19, 0x11, 0x1f, 0x19, 0x12, 0x03 ),        /* Handsfree Audio Gateway, Generic Audio */
    ATTR( 0x0004, 0x35, 0x0c, 0x35, 0x03, 0x19, 0x01, 0x00,
                  0x35, 0x05, 0x19, 0x00, 0x03, 0x08, HFP_CHANNEL ),       /* L2CAP, RFCOMM channel */
    ATTR( 0x0005, 0x35, 0x03, 0x19, 0x10, 0x02 ),
    ATTR( 0x0009, 0x35, 0x08, 0x35, 0x06, 0x19, 0x11, 0x1e, 0x09, 0x01, 0x07 ), /* HFP 1.7 */
    ATTR( 0x0100, 0x25, 0x0d, 'V', 'o', 'i', 'c', 'e', ' ', 'G', 'a', 't', 'e', 'w', 'a', 'y' ),
    ATTR( 0x0301, 0x08, 0x00 ),                                            /* no telephone network */
    ATTR( 0x0311, 0x09, 0x00, 0x00 ),
};

static const struct sdp_record records[] =
{
    { 0x00010001, source_uuids, ARRAY_SIZE(source_uuids), source_attributes, ARRAY_SIZE(source_attributes) },
    { 0x00010002, target_uuids, ARRAY_SIZE(target_uuids), target_attributes, ARRAY_SIZE(target_attributes) },
    { 0x00010003, gateway_uuids, ARRAY_SIZE(gateway_uuids), gateway_attributes, ARRAY_SIZE(gateway_attributes) },
};

struct buffer
{
    BYTE *data;
    UINT len, size;
};

static void put( struct buffer *b, const void *data, UINT len )
{
    if (b->len + len > b->size)
    {
        UINT size = max( b->size * 2, b->len + len + 256 );
        BYTE *p = realloc( b->data, size );
        if (!p) return;
        b->data = p;
        b->size = size;
    }
    memcpy( b->data + b->len, data, len );
    b->len += len;
}

static void put8( struct buffer *b, BYTE v ) { put( b, &v, 1 ); }
static void put16( struct buffer *b, UINT16 v ) { BYTE d[2] = { v >> 8, v }; put( b, d, 2 ); }
static void put32( struct buffer *b, UINT32 v ) { BYTE d[4] = { v >> 24, v >> 16, v >> 8, v }; put( b, d, 4 ); }

/* a data element sequence's header for a length */
static void put_seq( struct buffer *b, UINT len )
{
    if (len < 256) { put8( b, 0x35 ); put8( b, len ); }
    else { put8( b, 0x36 ); put16( b, len ); }
}

/* the header of a data element: its type, where its value starts, how long it is */
static BOOL parse_element( const BYTE *p, UINT len, BYTE *type, UINT *header, UINT *size )
{
    static const UINT fixed[5] = { 1, 2, 4, 8, 16 };
    BYTE index;

    if (!len) return FALSE;
    *type = p[0] >> 3;
    index = p[0] & 7;
    if (*type == 0) { *header = 1; *size = 0; return TRUE; }
    if (index < 5) { *header = 1; *size = fixed[index]; }
    else if (index == 5 && len >= 2) { *header = 2; *size = p[1]; }
    else if (index == 6 && len >= 3) { *header = 3; *size = (p[1] << 8) | p[2]; }
    else if (index == 7 && len >= 5) { *header = 5; *size = ((UINT)p[1] << 24) | (p[2] << 16) | (p[3] << 8) | p[4]; }
    else return FALSE;
    return *header + *size <= len;
}

/* the UUIDs of a search pattern, as 16-bit ones where they are Bluetooth's */
static UINT parse_uuids( const BYTE *p, UINT len, UINT32 *uuids, UINT max, UINT *used )
{
    BYTE type;
    UINT header, size, count = 0, pos;

    if (!parse_element( p, len, &type, &header, &size ) || type != 6) return 0;
    *used = header + size;
    for (pos = header; pos < header + size && count < max; )
    {
        BYTE t;
        UINT h, s;

        if (!parse_element( p + pos, header + size - pos, &t, &h, &s )) break;
        if (t == 3 && s == 2) uuids[count++] = (p[pos + 1] << 8) | p[pos + 2];
        else if (t == 3 && s == 4) uuids[count++] = ((UINT32)p[pos + 1] << 24) | (p[pos + 2] << 16) | (p[pos + 3] << 8) | p[pos + 4];
        else if (t == 3 && s == 16) uuids[count++] = ((UINT32)p[pos + 1] << 24) | (p[pos + 2] << 16) | (p[pos + 3] << 8) | p[pos + 4];
        pos += h + s;
    }
    return count;
}

static BOOL record_matches( const struct sdp_record *r, const UINT32 *uuids, UINT count )
{
    for (UINT i = 0; i < count; i++)
    {
        BOOL found = FALSE;
        for (UINT j = 0; j < r->uuid_count && !found; j++) found = r->uuids[j] == uuids[i];
        if (!found) return FALSE;
    }
    return count > 0;
}

/* the attribute ID list: single IDs and ranges */
struct id_range { UINT16 low, high; };

static UINT parse_ids( const BYTE *p, UINT len, struct id_range *ranges, UINT max, UINT *used )
{
    BYTE type;
    UINT header, size, count = 0, pos;

    if (!parse_element( p, len, &type, &header, &size ) || type != 6) return 0;
    *used = header + size;
    for (pos = header; pos < header + size && count < max; )
    {
        BYTE t;
        UINT h, s;

        if (!parse_element( p + pos, header + size - pos, &t, &h, &s )) break;
        if (t == 1 && s == 2)
        {
            ranges[count].low = ranges[count].high = (p[pos + 1] << 8) | p[pos + 2];
            count++;
        }
        else if (t == 1 && s == 4)
        {
            ranges[count].low = (p[pos + 1] << 8) | p[pos + 2];
            ranges[count].high = (p[pos + 3] << 8) | p[pos + 4];
            count++;
        }
        pos += h + s;
    }
    return count;
}

/* a record's attributes in the ranges, as a sequence */
static void put_attributes( struct buffer *b, const struct sdp_record *r, const struct id_range *ranges, UINT count )
{
    UINT len = 0;

    for (UINT i = 0; i < r->count; i++)
        for (UINT j = 0; j < count; j++)
            if (r->attributes[i].id >= ranges[j].low && r->attributes[i].id <= ranges[j].high)
            {
                len += 3 + r->attributes[i].len;
                break;
            }
    put_seq( b, len );
    for (UINT i = 0; i < r->count; i++)
        for (UINT j = 0; j < count; j++)
            if (r->attributes[i].id >= ranges[j].low && r->attributes[i].id <= ranges[j].high)
            {
                put8( b, 0x09 );
                put16( b, r->attributes[i].id );
                put( b, r->attributes[i].value, r->attributes[i].len );
                break;
            }
}

static void sdp_error( int fd, UINT16 transaction, UINT16 code )
{
    BYTE pdu[7] = { 0x01, transaction >> 8, transaction, 0x00, 0x02, code >> 8, code };
    send( fd, pdu, sizeof(pdu), MSG_NOSIGNAL );
}

/* a reply too long for the asker is sent in parts; the continuation state is the offset */
static void sdp_reply_part( int fd, BYTE pdu_id, UINT16 transaction, const struct buffer *all, UINT offset,
                            UINT max_bytes, UINT mtu )
{
    struct buffer out = {0};
    UINT part = all->len - min( offset, all->len );

    max_bytes = min( max_bytes, mtu > 20 ? mtu - 12 : 32 );
    part = min( part, max_bytes );
    put8( &out, pdu_id );
    put16( &out, transaction );
    put16( &out, 2 + part + (offset + part < all->len ? 3 : 1) );
    put16( &out, part );
    put( &out, all->data + offset, part );
    if (offset + part < all->len)
    {
        put8( &out, 2 );
        put16( &out, offset + part );
    }
    else put8( &out, 0 );
    send( fd, out.data, out.len, MSG_NOSIGNAL );
    free( out.data );
}

static int channel_mtu( int fd )
{
    struct l2cap_options opts;
    socklen_t len = sizeof(opts);

    if (!getsockopt( fd, SOL_L2CAP, L2CAP_OPTIONS, &opts, &len )) return opts.omtu;
    return 672;
}

static void sdp_request( struct channel *ch, const BYTE *pdu, UINT len )
{
    UINT32 uuids[12];
    struct id_range ranges[16];
    UINT16 transaction, plen;
    UINT used, pos, count, ranges_count, offset = 0;
    BYTE id;

    if (len < 5) return;
    id = pdu[0];
    transaction = (pdu[1] << 8) | pdu[2];
    plen = (pdu[3] << 8) | pdu[4];
    if (plen + 5 > len)
    {
        sdp_error( ch->fd, transaction, 0x0004 );
        return;
    }
    pdu += 5;

    switch (id)
    {
    case 0x02:  /* ServiceSearchRequest */
    {
        struct buffer out = {0};
        UINT16 max_count;
        UINT matched = 0;

        count = parse_uuids( pdu, plen, uuids, ARRAY_SIZE(uuids), &used );
        if (used + 2 > plen) { sdp_error( ch->fd, transaction, 0x0003 ); return; }
        max_count = (pdu[used] << 8) | pdu[used + 1];
        for (UINT i = 0; i < ARRAY_SIZE(records); i++)
            if (record_matches( &records[i], uuids, count ) && matched < max_count) matched++;
        put8( &out, 0x03 );
        put16( &out, transaction );
        put16( &out, 5 + 4 * matched );
        put16( &out, matched );
        put16( &out, matched );
        for (UINT i = 0, n = 0; i < ARRAY_SIZE(records) && n < matched; i++)
            if (record_matches( &records[i], uuids, count )) { put32( &out, records[i].handle ); n++; }
        put8( &out, 0 );
        send( ch->fd, out.data, out.len, MSG_NOSIGNAL );
        free( out.data );
        return;
    }
    case 0x04:  /* ServiceAttributeRequest */
    {
        struct buffer all = {0};
        const struct sdp_record *r = NULL;
        UINT32 handle;
        UINT16 max_bytes;

        if (plen < 6) { sdp_error( ch->fd, transaction, 0x0003 ); return; }
        handle = ((UINT32)pdu[0] << 24) | (pdu[1] << 16) | (pdu[2] << 8) | pdu[3];
        max_bytes = (pdu[4] << 8) | pdu[5];
        for (UINT i = 0; i < ARRAY_SIZE(records); i++) if (records[i].handle == handle) r = &records[i];
        if (!r) { sdp_error( ch->fd, transaction, 0x0002 ); return; }
        ranges_count = parse_ids( pdu + 6, plen - 6, ranges, ARRAY_SIZE(ranges), &used );
        pos = 6 + used;
        if (pos < plen && pdu[pos] == 2 && pos + 3 <= plen) offset = (pdu[pos + 1] << 8) | pdu[pos + 2];
        put_attributes( &all, r, ranges, ranges_count );
        sdp_reply_part( ch->fd, 0x05, transaction, &all, offset, max_bytes, channel_mtu( ch->fd ) );
        free( all.data );
        return;
    }
    case 0x06:  /* ServiceSearchAttributeRequest */
    {
        struct buffer all = {0}, lists = {0};
        UINT16 max_bytes;

        count = parse_uuids( pdu, plen, uuids, ARRAY_SIZE(uuids), &used );
        pos = used;
        if (pos + 2 > plen) { sdp_error( ch->fd, transaction, 0x0003 ); return; }
        max_bytes = (pdu[pos] << 8) | pdu[pos + 1];
        pos += 2;
        ranges_count = parse_ids( pdu + pos, plen - pos, ranges, ARRAY_SIZE(ranges), &used );
        pos += used;
        if (pos < plen && pdu[pos] == 2 && pos + 3 <= plen) offset = (pdu[pos + 1] << 8) | pdu[pos + 2];
        for (UINT i = 0; i < ARRAY_SIZE(records); i++)
            if (record_matches( &records[i], uuids, count )) put_attributes( &lists, &records[i], ranges, ranges_count );
        put_seq( &all, lists.len );
        put( &all, lists.data, lists.len );
        sdp_reply_part( ch->fd, 0x07, transaction, &all, offset, max_bytes, channel_mtu( ch->fd ) );
        free( all.data );
        free( lists.data );
        return;
    }
    default:
        sdp_error( ch->fd, transaction, 0x0003 );
    }
}

/**********************************************************************
 *          AVRCP: the headset's buttons
 */

static void avctp_message( struct channel *ch, BYTE *msg, UINT len )
{
    BYTE *avc = msg + 3;

    /* commands only, single packets, AV/C of A/V Remote Control */
    if (len < 6 || (msg[0] & 0x0c) || (msg[0] & 0x02)) return;
    if (msg[1] != 0x11 || msg[2] != 0x0e)
    {
        /* an unknown profile: IPID set, the frame back */
        msg[0] |= 0x03;
        send( ch->fd, msg, 3, MSG_NOSIGNAL );
        return;
    }
    msg[0] |= 0x02;   /* a response, with the same label */
    switch (avc[2])
    {
    case 0x30:  /* UNIT INFO */
    case 0x31:  /* SUBUNIT INFO */
    {
        BYTE reply[3 + 8] = { msg[0], msg[1], msg[2], 0x0c, 0xff, avc[2], 0x07, 0x48, 0xff, 0xff, 0xff };
        send( ch->fd, reply, sizeof(reply), MSG_NOSIGNAL );
        return;
    }
    case 0x7c:  /* PASS THROUGH */
        if (len >= 8)
        {
            BYTE op = avc[3] & 0x7f, released = avc[3] & 0x80;
            UINT vk = 0;

            switch (op)
            {
            case 0x44: case 0x46: vk = VK_MEDIA_PLAY_PAUSE; break;   /* play, pause */
            case 0x45: vk = VK_MEDIA_STOP; break;
            case 0x4b: vk = VK_MEDIA_NEXT_TRACK; break;               /* forward */
            case 0x4c: vk = VK_MEDIA_PREV_TRACK; break;               /* backward */
            case 0x41: vk = VK_VOLUME_UP; break;
            case 0x42: vk = VK_VOLUME_DOWN; break;
            case 0x43: vk = VK_VOLUME_MUTE; break;
            }
            avc[0] = vk ? 0x09 : 0x08;   /* ACCEPTED, NOT IMPLEMENTED */
            send( ch->fd, msg, len, MSG_NOSIGNAL );
            if (vk && !released)
            {
                TRACE( "%s: button %#x\n", addr_str( &ch->addr ), op );
                hci_media_key( vk );
            }
            return;
        }
        break;
    }
    /* the rest (the metadata of AVRCP 1.3 and later): not here */
    avc[0] = 0x08;
    send( ch->fd, msg, len, MSG_NOSIGNAL );
}

/**********************************************************************
 *          A2DP over AVDTP
 */

#define AVDTP_DISCOVER          0x01
#define AVDTP_GET_CAPABILITIES  0x02
#define AVDTP_SET_CONFIGURATION 0x03
#define AVDTP_GET_CONFIGURATION 0x04
#define AVDTP_RECONFIGURE       0x05
#define AVDTP_OPEN              0x06
#define AVDTP_START             0x07
#define AVDTP_CLOSE             0x08
#define AVDTP_SUSPEND           0x09
#define AVDTP_ABORT             0x0a
#define AVDTP_GET_ALL_CAPABILITIES 0x0c
#define AVDTP_DELAYREPORT       0x0d

#define MSG_COMMAND        0
#define MSG_GENERAL_REJECT 1
#define MSG_ACCEPT         2
#define MSG_REJECT         3

#define LOCAL_SEID 1

enum stream_state { STREAM_IDLE, STREAM_CONFIGURED, STREAM_OPEN, STREAM_STREAMING };

/* one stream, to one device at a time */
static struct
{
    BOOL present;
    bdaddr_t addr;
    struct channel *signal;
    int media_fd;
    enum stream_state state;
    BYTE remote_seid;
    BYTE config[4];          /* SBC's codec information element */
    int mtu;
    BYTE label;              /* of the signals this side sends */
    /* the answer a signal of this side waits for */
    BOOL waiting, answered;
    BYTE answer_label, answer_type, answer[64];
    UINT answer_len;
    pthread_t streamer;
    BOOL streamer_running;
    BOOL held;               /* the headset suspended it: not started again until a new sound */
    BOOL setting_up;         /* this side configures it now */
} a2dp;

static pthread_mutex_t request_lock = PTHREAD_MUTEX_INITIALIZER;   /* one signal of this side at a time */

static pthread_mutex_t a2dp_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t a2dp_cond = PTHREAD_COND_INITIALIZER;

/* our Source's capabilities: Media Transport, and SBC at 44.1 and 48 kHz,
 * every mode and block length, eight subbands, both allocations, bitpool 2..53 */
static const BYTE source_caps[] = { 0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0x3f, 0xf7, 0x02, 0x35 };

static void signal_send( int fd, BYTE label, BYTE type, BYTE signal, const BYTE *params, UINT len )
{
    BYTE msg[128];

    msg[0] = (label << 4) | type;
    msg[1] = signal;
    if (len) memcpy( msg + 2, params, min( len, sizeof(msg) - 2 ) );
    send( fd, msg, 2 + min( len, sizeof(msg) - 2 ), MSG_NOSIGNAL );
}

/* a signal of this side and its answer; FALSE: refused or none */
static BOOL signal_request( BYTE signal, const BYTE *params, UINT len, BYTE *answer, UINT *answer_len )
{
    struct timespec until;
    BYTE label;
    BOOL ok = FALSE;

    pthread_mutex_lock( &request_lock );
    pthread_mutex_lock( &a2dp_lock );
    if (!a2dp.signal)
    {
        pthread_mutex_unlock( &a2dp_lock );
        pthread_mutex_unlock( &request_lock );
        return FALSE;
    }
    TRACE( "-> %#x\n", signal );
    label = a2dp.label = (a2dp.label + 1) & 0x0f;
    a2dp.waiting = TRUE;
    a2dp.answered = FALSE;
    signal_send( a2dp.signal->fd, label, MSG_COMMAND, signal, params, len );
    clock_gettime( CLOCK_REALTIME, &until );
    until.tv_sec += REPLY_TIMEOUT;
    while (a2dp.signal && !(a2dp.answered && a2dp.answer_label == label))
        if (pthread_cond_timedwait( &a2dp_cond, &a2dp_lock, &until ) == ETIMEDOUT) break;
    if (a2dp.answered && a2dp.answer_label == label)
    {
        ok = a2dp.answer_type == MSG_ACCEPT;
        if (answer && answer_len)
        {
            *answer_len = min( *answer_len, a2dp.answer_len );
            memcpy( answer, a2dp.answer, *answer_len );
        }
    }
    else WARN( "signal %#x: no answer\n", signal );
    if (a2dp.answered && a2dp.answer_label == label && !ok)
        WARN( "signal %#x refused: type %u, %s\n", signal, a2dp.answer_type,
              wine_dbgstr_an( (const char *)a2dp.answer, a2dp.answer_len ) );
    a2dp.waiting = FALSE;
    pthread_mutex_unlock( &a2dp_lock );
    pthread_mutex_unlock( &request_lock );
    return ok;
}

/* /dev/shm/arctic-bt-audio: winealsa.drv shows the card "Bluetooth" as these headphones */
static void write_audio_state( const bdaddr_t *addr )
{
    struct bt_device_info info = {0};
    char tmp[64], text[400];
    int fd, len;

    hci_device_info( addr->b, &info );
    /* hfp=1: its microphone is there too (the loopback's second substream) */
    len = snprintf( text, sizeof(text), "name=%s\naddress=%s\nclass=%u\nhfp=%u\n", info.name[0] ? info.name : "Bluetooth",
                    addr_str( addr ), info.class, hfp_connected( addr->b ) );
    snprintf( tmp, sizeof(tmp), "%s.%d", AUDIO_STATE, getpid() );
    if ((fd = open( tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644 )) < 0) return;
    write( fd, text, len );
    close( fd );
    rename( tmp, AUDIO_STATE );
}

static void stop_streamer(void);

/* the hands-free connection came or went: the microphone with it */
void audio_state_changed( void )
{
    bdaddr_t addr;
    BOOL open;

    pthread_mutex_lock( &a2dp_lock );
    open = a2dp.present && a2dp.media_fd >= 0;
    addr = a2dp.addr;
    pthread_mutex_unlock( &a2dp_lock );
    if (open) write_audio_state( &addr );
}

/* the stream ends: its channels, the headphones' output */
static void stream_close_locked( BOOL drop_signal )
{
    bdaddr_t addr = a2dp.addr;
    BOOL had_media = a2dp.media_fd >= 0, present = a2dp.present;

    if (a2dp.media_fd >= 0)
    {
        shutdown( a2dp.media_fd, SHUT_RDWR );   /* the reader's thread drops it */
        a2dp.media_fd = -1;
    }
    a2dp.state = STREAM_IDLE;
    if (drop_signal && a2dp.signal)
    {
        shutdown( a2dp.signal->fd, SHUT_RDWR );
        a2dp.signal = NULL;
        a2dp.present = FALSE;
    }
    pthread_cond_broadcast( &a2dp_cond );
    if (had_media || (drop_signal && present))
    {
        unlink( AUDIO_STATE );
        pthread_mutex_unlock( &a2dp_lock );
        stop_streamer();
        hci_device_set_audio( addr.b, FALSE );
        TRACE( "%s: no music any more\n", addr_str( &addr ) );
        pthread_mutex_lock( &a2dp_lock );
    }
}

/* the codec's settings from a configuration's capabilities */
static BOOL find_codec( const BYTE *caps, UINT len, BYTE out[4] )
{
    for (UINT pos = 0; pos + 2 <= len; pos += 2 + caps[pos + 1])
    {
        if (pos + 2 + caps[pos + 1] > len) break;
        if (caps[pos] == 0x07 && caps[pos + 1] >= 6 && caps[pos + 2] >> 4 == 0 && caps[pos + 3] == 0x00)
        {
            memcpy( out, caps + pos + 4, 4 );
            return TRUE;
        }
    }
    return FALSE;
}

/* from a sink's capabilities, what Windows would pick */
static BOOL choose_config( const BYTE caps[4], BYTE out[4] )
{
    BYTE freq = caps[0] & 0x10 ? 0x10 : caps[0] & 0x20 ? 0x20 : 0;
    BYTE mode = caps[0] & 0x01 ? 0x01 : caps[0] & 0x02 ? 0x02 : caps[0] & 0x04 ? 0x04 : caps[0] & 0x08 ? 0x08 : 0;
    BYTE blocks = caps[1] & 0x10 ? 0x10 : caps[1] & 0x20 ? 0x20 : caps[1] & 0x40 ? 0x40 : caps[1] & 0x80 ? 0x80 : 0;
    BYTE alloc = caps[1] & 0x01 ? 0x01 : caps[1] & 0x02 ? 0x02 : 0;
    int best = freq == 0x10 ? 51 : 53, high = min( caps[3], best ), low = caps[2];

    if (!freq || !mode || !blocks || !alloc || !(caps[1] & 0x04) || high < low || high < 2) return FALSE;
    out[0] = freq | mode;
    out[1] = blocks | 0x04 | alloc;
    out[2] = max( low, 2 );
    out[3] = high;
    return TRUE;
}

/**********************************************************************
 *          The stream: what is played, coded, sent
 */

static BOOL loopback_active( snd_ctl_t *ctl )
{
    snd_ctl_elem_value_t *value;

    if (!ctl) return TRUE;
    snd_ctl_elem_value_alloca( &value );
    snd_ctl_elem_value_set_interface( value, SND_CTL_ELEM_IFACE_PCM );
    snd_ctl_elem_value_set_name( value, "PCM Slave Active" );
    snd_ctl_elem_value_set_device( value, 1 );
    snd_ctl_elem_value_set_subdevice( value, 0 );
    if (snd_ctl_elem_read( ctl, value ) < 0) return TRUE;
    return snd_ctl_elem_value_get_boolean( value, 0 );
}

static snd_pcm_t *open_capture(void)
{
    snd_pcm_t *pcm;
    int err;

    if ((err = snd_pcm_open( &pcm, LOOPBACK_CAPTURE, SND_PCM_STREAM_CAPTURE, 0 )) < 0)
    {
        WARN( "%s: %s\n", LOOPBACK_CAPTURE, snd_strerror( err ) );
        return NULL;
    }
    /* what dmix plays to the card: 48 kHz, 16 bits, two channels; 20 ms periods */
    if ((err = snd_pcm_set_params( pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, 48000, 0,
                                   80000 )) < 0)
    {
        WARN( "capture: %s\n", snd_strerror( err ) );
        snd_pcm_close( pcm );
        return NULL;
    }
    /* set_params makes the start threshold the whole buffer: a capture read in
     * smaller parts never starts by itself (and times out with EIO) */
    if ((err = snd_pcm_start( pcm )) < 0)
    {
        WARN( "capture start: %s\n", snd_strerror( err ) );
        snd_pcm_close( pcm );
        return NULL;
    }
    TRACE( "capturing what plays\n" );
    return pcm;
}

static void *streamer_proc( void *arg )
{
    struct sbc_encoder sbc;
    snd_pcm_t *pcm = NULL;
    snd_ctl_t *ctl = NULL;
    /* a packet's input (twice that for 44.1 kHz) and one read more */
    int16_t input[2 * (128 * 15 * 2 + 256)], frames[128 * 2 * 15];
    BYTE packet[2048], config[4];
    int mtu, frame_len, per_packet, rate, in_have = 0;
    UINT16 sequence = 0;
    UINT32 timestamp = 0;
    time_t quiet_since = 0;
    double phase = 0;   /* 48 kHz to 44.1 kHz, when the sink wants that */

    pthread_mutex_lock( &a2dp_lock );
    memcpy( config, a2dp.config, 4 );
    mtu = min( a2dp.mtu, (int)sizeof(packet) );
    pthread_mutex_unlock( &a2dp_lock );

    rate = config[0] & 0x10 ? 48000 : 44100;
    sbc_init( &sbc, config[0] & 0x10 ? SBC_FREQ_48000 : SBC_FREQ_44100,
              config[0] & 0x01 ? SBC_MODE_JOINT_STEREO : config[0] & 0x02 ? SBC_MODE_STEREO :
              config[0] & 0x04 ? SBC_MODE_DUAL_CHANNEL : SBC_MODE_MONO,
              config[1] & 0x01 ? SBC_ALLOCATION_LOUDNESS : SBC_ALLOCATION_SNR,
              config[1] & 0x10 ? 16 : config[1] & 0x20 ? 12 : config[1] & 0x40 ? 8 : 4, 8, config[3] );
    frame_len = sbc_frame_length( &sbc );
    per_packet = min( 15, (mtu - 13) / frame_len );
    if (per_packet < 1) per_packet = 1;
    if (snd_ctl_open( &ctl, LOOPBACK_CARD, 0 ) < 0) ctl = NULL;
    TRACE( "streaming: %d Hz, bitpool %d, %d frames of %d bytes a packet (MTU %d)\n", rate, config[3], per_packet,
           frame_len, mtu );

    for (;;)
    {
        int media, samples = sbc_frame_samples( &sbc ), channels = sbc_channels( &sbc );
        enum stream_state state;
        BOOL active;
        UINT len;

        pthread_mutex_lock( &a2dp_lock );
        if (!a2dp.streamer_running || a2dp.state < STREAM_OPEN)
        {
            pthread_mutex_unlock( &a2dp_lock );
            break;
        }
        state = a2dp.state;
        media = a2dp.media_fd;
        pthread_mutex_unlock( &a2dp_lock );

        active = loopback_active( ctl );

        /* a program records from the headset: what plays goes over the voice link
         * (hfp.c reads it), the music is suspended, as Windows switches to Hands-Free */
        if (hfp_voice_active())
        {
            if (pcm)
            {
                snd_pcm_close( pcm );
                pcm = NULL;
            }
            if (state == STREAM_STREAMING &&
                signal_request( AVDTP_SUSPEND, (BYTE[]){ a2dp.remote_seid << 2 }, 1, NULL, NULL ))
            {
                pthread_mutex_lock( &a2dp_lock );
                if (a2dp.state == STREAM_STREAMING) a2dp.state = STREAM_OPEN;
                pthread_mutex_unlock( &a2dp_lock );
            }
            usleep( 200000 );
            continue;
        }

        if (state == STREAM_OPEN)
        {
            BOOL held;

            pthread_mutex_lock( &a2dp_lock );
            if (!active) a2dp.held = FALSE;
            held = a2dp.held;
            pthread_mutex_unlock( &a2dp_lock );
            if (pcm)
            {
                snd_pcm_close( pcm );
                pcm = NULL;
            }
            /* something plays: the stream starts, as Windows starts it */
            if (!active || held)
            {
                usleep( 100000 );
                continue;
            }
            if (!signal_request( AVDTP_START, (BYTE[]){ a2dp.remote_seid << 2 }, 1, NULL, NULL ))
            {
                WARN( "START refused\n" );
                usleep( 500000 );
                continue;
            }
            TRACE( "something plays: the stream started\n" );
            pthread_mutex_lock( &a2dp_lock );
            if (a2dp.state == STREAM_OPEN) a2dp.state = STREAM_STREAMING;
            pthread_mutex_unlock( &a2dp_lock );
            quiet_since = 0;
            continue;
        }

        if (!pcm && !(pcm = open_capture()))
        {
            usleep( 200000 );
            continue;
        }
        if (!active)
        {
            if (!quiet_since) quiet_since = time( NULL );
            else if (time( NULL ) - quiet_since >= SUSPEND_AFTER)
            {
                /* quiet: suspended, the headset may sleep */
                snd_pcm_close( pcm );
                pcm = NULL;
                if (signal_request( AVDTP_SUSPEND, (BYTE[]){ a2dp.remote_seid << 2 }, 1, NULL, NULL ))
                {
                    pthread_mutex_lock( &a2dp_lock );
                    if (a2dp.state == STREAM_STREAMING) a2dp.state = STREAM_OPEN;
                    pthread_mutex_unlock( &a2dp_lock );
                }
                continue;
            }
        }
        else quiet_since = 0;

        /* a packet's frames of input */
        while (in_have < samples * per_packet * (rate == 48000 ? 1 : 2))
        {
            snd_pcm_sframes_t got = snd_pcm_readi( pcm, input + in_have * 2, 128 );

            if (got == -EPIPE || got == -ESTRPIPE)
            {
                snd_pcm_prepare( pcm );
                snd_pcm_start( pcm );
                continue;
            }
            if (got < 0)
            {
                WARN( "capture: %s\n", snd_strerror( got ) );
                snd_pcm_close( pcm );
                pcm = NULL;
                break;
            }
            in_have += got;
        }
        if (!pcm) continue;

        /* 44.1 kHz: taken from the 48 kHz input, linearly */
        if (rate == 48000) memcpy( frames, input, samples * per_packet * 2 * sizeof(int16_t) );
        else
        {
            int used;
            for (int i = 0; i < samples * per_packet; i++, phase += 48000.0 / 44100.0)
            {
                int at = (int)phase;
                double t = phase - at;
                for (int c = 0; c < 2; c++)
                    frames[i * 2 + c] = (int16_t)(input[at * 2 + c] * (1 - t) + input[(at + 1) * 2 + c] * t);
            }
            used = (int)phase;
            phase -= used;
            in_have -= used;
            memmove( input, input + used * 2, in_have * 2 * sizeof(int16_t) );
            goto encode;
        }
        in_have -= samples * per_packet;
        memmove( input, input + samples * per_packet * 2, in_have * 2 * sizeof(int16_t) );

    encode:
        /* RTP: version 2, payload type 96, the sequence, the timestamp in samples */
        packet[0] = 0x80;
        packet[1] = 0x60;
        packet[2] = sequence >> 8; packet[3] = sequence;
        packet[4] = timestamp >> 24; packet[5] = timestamp >> 16; packet[6] = timestamp >> 8; packet[7] = timestamp;
        packet[8] = 0; packet[9] = 0; packet[10] = 0; packet[11] = 1;
        packet[12] = per_packet;   /* the media payload header: the frames in it */
        len = 13;
        for (int f = 0; f < per_packet; f++)
        {
            int16_t mono[128 * 16];
            const int16_t *pcm_in = frames + f * samples * 2;

            if (channels == 1)
            {
                for (int i = 0; i < samples; i++) mono[i] = (pcm_in[i * 2] + pcm_in[i * 2 + 1]) / 2;
                pcm_in = mono;
            }
            len += sbc_encode( &sbc, pcm_in, packet + len );
        }
        sequence++;
        timestamp += samples * per_packet;
        /* a full queue drops the packet: the delay never grows */
        if (send( media, packet, len, MSG_DONTWAIT | MSG_NOSIGNAL ) < 0 && errno != EAGAIN)
        {
            usleep( 10000 );
        }
    }
    if (pcm) snd_pcm_close( pcm );
    if (ctl) snd_ctl_close( ctl );
    return NULL;
}

static void start_streamer_locked(void)
{
    if (a2dp.streamer_running) return;
    a2dp.streamer_running = TRUE;
    if (pthread_create( &a2dp.streamer, NULL, streamer_proc, NULL )) a2dp.streamer_running = FALSE;
}

static void stop_streamer(void)
{
    pthread_t thread;
    BOOL running;

    pthread_mutex_lock( &a2dp_lock );
    running = a2dp.streamer_running;
    thread = a2dp.streamer;
    a2dp.streamer_running = FALSE;
    pthread_cond_broadcast( &a2dp_cond );
    pthread_mutex_unlock( &a2dp_lock );
    if (running && !pthread_equal( thread, pthread_self() )) pthread_join( thread, NULL );
}

/* the media channel is there: the headphones are an output now */
static void stream_opened_locked( int fd )
{
    struct l2cap_options opts;
    socklen_t len = sizeof(opts);
    bdaddr_t addr = a2dp.addr;

    a2dp.media_fd = fd;
    a2dp.mtu = !getsockopt( fd, SOL_L2CAP, L2CAP_OPTIONS, &opts, &len ) ? opts.omtu : 895;
    a2dp.state = STREAM_OPEN;
    write_audio_state( &addr );
    start_streamer_locked();
    pthread_mutex_unlock( &a2dp_lock );
    hci_device_set_audio( addr.b, TRUE );
    TRACE( "%s: music connected, MTU %d\n", addr_str( &addr ), a2dp.mtu );
    pthread_mutex_lock( &a2dp_lock );
}

/* a signal from the other side, or an answer to ours (the reader's thread) */
static void signal_message( struct channel *ch, const BYTE *msg, UINT len )
{
    BYTE label, type, signal, seid;
    const BYTE *params;
    UINT plen;

    if (len < 2 || ((msg[0] >> 2) & 3)) return;   /* single packets only */
    label = msg[0] >> 4;
    type = msg[0] & 3;
    signal = msg[1] & 0x3f;
    params = msg + 2;
    plen = len - 2;

    pthread_mutex_lock( &a2dp_lock );
    if (ch != a2dp.signal)
    {
        pthread_mutex_unlock( &a2dp_lock );
        return;
    }
    if (type != MSG_COMMAND)
    {
        /* the answer a signal of this side waits for */
        a2dp.answered = TRUE;
        a2dp.answer_label = label;
        a2dp.answer_type = type;
        a2dp.answer_len = min( plen, sizeof(a2dp.answer) );
        memcpy( a2dp.answer, params, a2dp.answer_len );
        pthread_cond_broadcast( &a2dp_cond );
        pthread_mutex_unlock( &a2dp_lock );
        return;
    }

    seid = plen ? params[0] >> 2 : 0;
    TRACE( "%s asks %#x (state %u)\n", addr_str( &ch->addr ), signal, a2dp.state );
    switch (signal)
    {
    case AVDTP_DISCOVER:
    {
        BYTE sep[2] = { (LOCAL_SEID << 2) | (a2dp.state != STREAM_IDLE ? 0x02 : 0), 0x00 /* audio, source */ };
        signal_send( ch->fd, label, MSG_ACCEPT, signal, sep, 2 );
        break;
    }
    case AVDTP_GET_CAPABILITIES:
    case AVDTP_GET_ALL_CAPABILITIES:
        if (seid != LOCAL_SEID) signal_send( ch->fd, label, MSG_REJECT, signal, (BYTE[]){ 0x12 }, 1 );
        else signal_send( ch->fd, label, MSG_ACCEPT, signal, source_caps, sizeof(source_caps) );
        break;
    case AVDTP_SET_CONFIGURATION:
    {
        BYTE config[4];

        if (plen < 2 || seid != LOCAL_SEID)
            signal_send( ch->fd, label, MSG_REJECT, signal, (BYTE[]){ 0x00, 0x12 }, 2 );
        else if (a2dp.state != STREAM_IDLE)
            signal_send( ch->fd, label, MSG_REJECT, signal, (BYTE[]){ 0x00, 0x13 }, 2 );
        else if (!find_codec( params + 2, plen - 2, config ) || !(config[1] & 0x04) || !(config[0] & 0x30))
            signal_send( ch->fd, label, MSG_REJECT, signal, (BYTE[]){ 0x07, 0x29 }, 2 );
        else
        {
            a2dp.remote_seid = params[1] >> 2;
            memcpy( a2dp.config, config, 4 );
            a2dp.state = STREAM_CONFIGURED;
            TRACE( "%s set the stream up: %02x %02x, bitpool %u-%u\n", addr_str( &ch->addr ), config[0], config[1],
                   config[2], config[3] );
            signal_send( ch->fd, label, MSG_ACCEPT, signal, NULL, 0 );
        }
        break;
    }
    case AVDTP_GET_CONFIGURATION:
    {
        BYTE caps[10] = { 0x01, 0x00, 0x07, 0x06, 0x00, 0x00 };
        memcpy( caps + 6, a2dp.config, 4 );
        signal_send( ch->fd, label, a2dp.state != STREAM_IDLE ? MSG_ACCEPT : MSG_REJECT, signal, caps,
                     a2dp.state != STREAM_IDLE ? sizeof(caps) : 0 );
        break;
    }
    case AVDTP_OPEN:
        /* the media channel follows */
        if (a2dp.state == STREAM_CONFIGURED) signal_send( ch->fd, label, MSG_ACCEPT, signal, NULL, 0 );
        else signal_send( ch->fd, label, MSG_REJECT, signal, (BYTE[]){ 0x31 }, 1 );
        break;
    case AVDTP_START:
        if (a2dp.state == STREAM_OPEN || a2dp.state == STREAM_STREAMING)
        {
            a2dp.state = STREAM_STREAMING;
            signal_send( ch->fd, label, MSG_ACCEPT, signal, NULL, 0 );
        }
        else signal_send( ch->fd, label, MSG_REJECT, signal, (BYTE[]){ params[0], 0x31 }, 2 );
        break;
    case AVDTP_SUSPEND:
        if (a2dp.state == STREAM_STREAMING)
        {
            a2dp.state = STREAM_OPEN;
            a2dp.held = TRUE;
        }
        signal_send( ch->fd, label, MSG_ACCEPT, signal, NULL, 0 );
        break;
    case AVDTP_CLOSE:
    case AVDTP_ABORT:
        signal_send( ch->fd, label, MSG_ACCEPT, signal, NULL, 0 );
        stream_close_locked( FALSE );
        break;
    case AVDTP_DELAYREPORT:
        signal_send( ch->fd, label, MSG_ACCEPT, signal, NULL, 0 );
        break;
    default:
        /* general reject: the signal is not known */
        signal_send( ch->fd, label, MSG_GENERAL_REJECT, signal, NULL, 0 );
        break;
    }
    pthread_mutex_unlock( &a2dp_lock );
}

/* this side sets the stream up on the signalling channel there is */
static BOOL stream_setup_signals(void)
{
    BYTE answer[64], caps[64], sink_caps[4], config[4], params[2 + 10];
    UINT len = sizeof(answer), seps, i, sink = 0;
    struct sockaddr_l2 addr = { .l2_family = AF_BLUETOOTH, .l2_psm = PSM_AVDTP, .l2_bdaddr_type = BDADDR_BREDR };
    int fd;

    if (!signal_request( AVDTP_DISCOVER, NULL, 0, answer, &len ))
    {
        WARN( "DISCOVER failed\n" );
        return FALSE;
    }
    seps = len / 2;
    TRACE( "DISCOVER: %u endpoints\n", seps );
    for (i = 0; i < seps && !sink; i++)
    {
        BYTE seid = answer[2 * i] >> 2, in_use = answer[2 * i] & 0x02, media = answer[2 * i + 1] >> 4;
        BYTE tsep = (answer[2 * i + 1] >> 3) & 1;
        UINT clen = sizeof(caps);

        TRACE( "  SEP %u: media %u, %s, %s\n", seid, media, tsep ? "sink" : "source", in_use ? "in use" : "free" );
        if (in_use || media != 0 || tsep != 1) continue;
        if (!signal_request( AVDTP_GET_CAPABILITIES, (BYTE[]){ seid << 2 }, 1, caps, &clen )) continue;
        TRACE( "  its capabilities: %s\n", wine_dbg_sprintf( "%.*s", 0, "" ) );
        for (UINT k = 0; k < clen; k++) fprintf( stderr, "%02x", caps[k] );
        fprintf( stderr, "\n" );
        if (find_codec( caps, clen, sink_caps ) && choose_config( sink_caps, config )) sink = seid;
    }
    if (!sink)
    {
        WARN( "no sink of SBC\n" );
        return FALSE;
    }

    params[0] = sink << 2;
    params[1] = LOCAL_SEID << 2;
    memcpy( params + 2, (BYTE[]){ 0x01, 0x00, 0x07, 0x06, 0x00, 0x00 }, 6 );
    memcpy( params + 8, config, 4 );
    pthread_mutex_lock( &a2dp_lock );
    if (a2dp.state != STREAM_IDLE)
    {
        /* the headset was quicker */
        pthread_mutex_unlock( &a2dp_lock );
        return TRUE;
    }
    a2dp.remote_seid = sink;
    memcpy( a2dp.config, config, 4 );
    pthread_mutex_unlock( &a2dp_lock );
    if (!signal_request( AVDTP_SET_CONFIGURATION, params, sizeof(params), NULL, NULL ))
    {
        WARN( "SET_CONFIGURATION refused\n" );
        return FALSE;
    }
    pthread_mutex_lock( &a2dp_lock );
    a2dp.state = STREAM_CONFIGURED;
    addr.l2_bdaddr = a2dp.addr;
    pthread_mutex_unlock( &a2dp_lock );
    TRACE( "%s: SBC %02x %02x, bitpool %u-%u\n", addr_str( &addr.l2_bdaddr ), config[0], config[1], config[2], config[3] );

    if (!signal_request( AVDTP_OPEN, (BYTE[]){ sink << 2 }, 1, NULL, NULL ))
    {
        WARN( "OPEN refused\n" );
        return FALSE;
    }
    if ((fd = socket( AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_CLOEXEC, BTPROTO_L2CAP )) < 0) return FALSE;
    set_security( fd, BT_SECURITY_MEDIUM );
    if (connect( fd, (struct sockaddr *)&addr, sizeof(addr) ))
    {
        WARN( "media channel: %s\n", strerror( errno ) );
        close( fd );
        return FALSE;
    }
    add_channel( fd, CH_MEDIA, &addr.l2_bdaddr );
    pthread_mutex_lock( &a2dp_lock );
    stream_opened_locked( fd );
    pthread_mutex_unlock( &a2dp_lock );
    return TRUE;
}

/* one setup at a time: a click and the headset's own connection may come together */
static BOOL stream_setup(void)
{
    BOOL ret;

    pthread_mutex_lock( &a2dp_lock );
    if (a2dp.setting_up || a2dp.state != STREAM_IDLE)
    {
        pthread_mutex_unlock( &a2dp_lock );
        return TRUE;
    }
    a2dp.setting_up = TRUE;
    pthread_mutex_unlock( &a2dp_lock );
    ret = stream_setup_signals();
    pthread_mutex_lock( &a2dp_lock );
    a2dp.setting_up = FALSE;
    pthread_mutex_unlock( &a2dp_lock );
    return ret;
}

NTSTATUS a2dp_connect( const BYTE addr_bytes[6] )
{
    struct sockaddr_l2 addr = { .l2_family = AF_BLUETOOTH, .l2_psm = PSM_AVDTP, .l2_bdaddr_type = BDADDR_BREDR };
    struct channel *ch;
    int fd;

    memcpy( &addr.l2_bdaddr, addr_bytes, 6 );
    pthread_mutex_lock( &a2dp_lock );
    if (a2dp.present)
    {
        BOOL same = !memcmp( &a2dp.addr, addr_bytes, 6 );
        pthread_mutex_unlock( &a2dp_lock );
        /* already playing there; or another device has the music */
        return same ? STATUS_SUCCESS : STATUS_DEVICE_BUSY;
    }
    pthread_mutex_unlock( &a2dp_lock );

    if ((fd = socket( AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_CLOEXEC, BTPROTO_L2CAP )) < 0) return STATUS_UNSUCCESSFUL;
    set_security( fd, BT_SECURITY_MEDIUM );
    TRACE( "connecting %s\n", addr_str( &addr.l2_bdaddr ) );
    if (connect( fd, (struct sockaddr *)&addr, sizeof(addr) ))
    {
        /* no AVDTP there: not a device with music */
        WARN( "%s: %s\n", addr_str( &addr.l2_bdaddr ), strerror( errno ) );
        close( fd );
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    pthread_mutex_lock( &a2dp_lock );
    if (a2dp.present)
    {
        pthread_mutex_unlock( &a2dp_lock );
        close( fd );
        return STATUS_SUCCESS;
    }
    if (!(ch = add_channel( fd, CH_SIGNAL, &addr.l2_bdaddr )))
    {
        pthread_mutex_unlock( &a2dp_lock );
        close( fd );
        return STATUS_NO_MEMORY;
    }
    a2dp.present = TRUE;
    a2dp.addr = addr.l2_bdaddr;
    a2dp.signal = ch;
    a2dp.state = STREAM_IDLE;
    a2dp.media_fd = -1;
    pthread_mutex_unlock( &a2dp_lock );

    if (stream_setup()) return STATUS_SUCCESS;
    pthread_mutex_lock( &a2dp_lock );
    if (a2dp.signal == ch) stream_close_locked( TRUE );
    pthread_mutex_unlock( &a2dp_lock );
    return STATUS_DEVICE_NOT_CONNECTED;
}

/* a headset that connected and configures nothing: this side does it */
static void *setup_later_proc( void *arg )
{
    struct channel *ch = arg;
    BOOL idle;

    sleep( 2 );
    pthread_mutex_lock( &a2dp_lock );
    idle = a2dp.signal == ch && a2dp.state == STREAM_IDLE && !a2dp.setting_up;
    pthread_mutex_unlock( &a2dp_lock );
    if (idle) TRACE( "the headset set nothing up: this side does\n" );
    if (idle && !stream_setup())
    {
        pthread_mutex_lock( &a2dp_lock );
        if (a2dp.signal == ch && a2dp.state == STREAM_IDLE) stream_close_locked( TRUE );
        pthread_mutex_unlock( &a2dp_lock );
    }
    return NULL;
}

void profiles_disconnect( const BYTE addr[6] )
{
    struct channel *ch;

    pthread_mutex_lock( &a2dp_lock );
    if (a2dp.present && !memcmp( &a2dp.addr, addr, 6 ))
    {
        BYTE seid = a2dp.remote_seid;
        BOOL open = a2dp.state != STREAM_IDLE;

        pthread_mutex_unlock( &a2dp_lock );
        stop_streamer();
        if (open) signal_request( AVDTP_CLOSE, (BYTE[]){ seid << 2 }, 1, NULL, NULL );
        pthread_mutex_lock( &a2dp_lock );
        if (a2dp.present && !memcmp( &a2dp.addr, addr, 6 )) stream_close_locked( TRUE );
    }
    pthread_mutex_unlock( &a2dp_lock );

    /* the remote control and the hands-free too */
    pthread_mutex_lock( &channels_lock );
    LIST_FOR_EACH_ENTRY( ch, &channels, struct channel, entry )
        if (!memcmp( &ch->addr, addr, 6 )) shutdown( ch->fd, SHUT_RDWR );
    pthread_mutex_unlock( &channels_lock );
    hfp_device_gone( addr );
}

void profiles_device_gone( const BYTE addr[6] )
{
    pthread_mutex_lock( &a2dp_lock );
    if (a2dp.present && !memcmp( &a2dp.addr, addr, 6 )) stream_close_locked( TRUE );
    pthread_mutex_unlock( &a2dp_lock );
    hfp_device_gone( addr );
}

/**********************************************************************
 *          The reader's thread
 */

static void accepted( int listener, enum channel_kind kind )
{
    struct sockaddr_l2 addr;
    socklen_t len = sizeof(addr);
    struct channel *ch;
    pthread_t thread;
    int fd = accept4( listener, (struct sockaddr *)&addr, &len, SOCK_CLOEXEC );

    if (fd < 0) return;
    TRACE( "%s connected to PSM %u\n", addr_str( &addr.l2_bdaddr ), kind == CH_SDP ? 1 : kind == CH_AVCTP ? 23 : 25 );
    if (kind != CH_SIGNAL)
    {
        add_channel( fd, kind, &addr.l2_bdaddr );
        return;
    }

    pthread_mutex_lock( &a2dp_lock );
    if (a2dp.present && !memcmp( &a2dp.addr, &addr.l2_bdaddr, 6 ) && a2dp.state == STREAM_CONFIGURED &&
        a2dp.media_fd < 0)
    {
        /* the second channel: the media's, after OPEN */
        add_channel( fd, CH_MEDIA, &addr.l2_bdaddr );
        stream_opened_locked( fd );
        pthread_mutex_unlock( &a2dp_lock );
        return;
    }
    if (a2dp.present && memcmp( &a2dp.addr, &addr.l2_bdaddr, 6 ))
    {
        /* one device has the music at a time */
        pthread_mutex_unlock( &a2dp_lock );
        WARN( "%s: another device plays\n", addr_str( &addr.l2_bdaddr ) );
        close( fd );
        return;
    }
    if (a2dp.present && a2dp.setting_up)
    {
        /* both sides connected at once: this side's channel goes on */
        pthread_mutex_unlock( &a2dp_lock );
        TRACE( "%s: a second signalling channel, closed\n", addr_str( &addr.l2_bdaddr ) );
        close( fd );
        return;
    }
    /* the same device again: what was there is stale */
    if (a2dp.present) stream_close_locked( TRUE );
    ch = add_channel( fd, CH_SIGNAL, &addr.l2_bdaddr );
    a2dp.present = TRUE;
    a2dp.addr = addr.l2_bdaddr;
    a2dp.signal = ch;
    a2dp.state = STREAM_IDLE;
    a2dp.media_fd = -1;
    pthread_mutex_unlock( &a2dp_lock );
    if (ch && !pthread_create( &thread, NULL, setup_later_proc, ch )) pthread_detach( thread );
}

static void channel_closed( struct channel *ch )
{
    /* the media's descriptor is the streamer's until it has stopped */
    if (ch->kind == CH_MEDIA) stop_streamer();
    pthread_mutex_lock( &a2dp_lock );
    if (ch->kind == CH_SIGNAL && ch == a2dp.signal)
    {
        a2dp.signal = NULL;
        stream_close_locked( TRUE );
        a2dp.present = FALSE;
    }
    else if (ch->kind == CH_MEDIA && ch->fd == a2dp.media_fd)
        stream_close_locked( FALSE );
    pthread_mutex_unlock( &a2dp_lock );
    drop_channel( ch );
}

static void *io_proc( void *arg )
{
    static const enum channel_kind listen_kind[3] = { CH_SDP, CH_AVCTP, CH_SIGNAL };
    BYTE buf[2048];

    while (!stopping)
    {
        struct pollfd fds[64];
        struct channel *chs[64], *ch;
        UINT count = 0, first;

        fds[count].fd = wake_pipe[0];
        fds[count++].events = POLLIN;
        for (int i = 0; i < 3; i++)
            if (listen_fd[i] >= 0) { fds[count].fd = listen_fd[i]; fds[count++].events = POLLIN; }
        first = count;
        pthread_mutex_lock( &channels_lock );
        LIST_FOR_EACH_ENTRY( ch, &channels, struct channel, entry )
        {
            if (count >= ARRAY_SIZE(fds)) break;
            chs[count] = ch;
            fds[count].fd = ch->fd;
            fds[count++].events = POLLIN;
        }
        pthread_mutex_unlock( &channels_lock );

        if (poll( fds, count, -1 ) < 0)
        {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[0].revents) read( wake_pipe[0], buf, sizeof(buf) );
        for (UINT i = 1, l = 0; i < first; i++, l++)
        {
            while (listen_fd[l] < 0) l++;
            if (fds[i].revents & POLLIN) accepted( listen_fd[l], listen_kind[l] );
        }
        for (UINT i = first; i < count; i++)
        {
            ssize_t len;

            if (!fds[i].revents) continue;
            ch = chs[i];
            len = recv( ch->fd, buf, sizeof(buf), MSG_DONTWAIT );
            if (len < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            if (len <= 0)
            {
                TRACE( "%s: a channel closed\n", addr_str( &ch->addr ) );
                channel_closed( ch );
                continue;
            }
            switch (ch->kind)
            {
            case CH_SDP: sdp_request( ch, buf, len ); break;
            case CH_SIGNAL: signal_message( ch, buf, len ); break;
            case CH_AVCTP: avctp_message( ch, buf, len ); break;
            case CH_MEDIA: break;   /* nothing comes on it */
            }
        }
    }
    return NULL;
}

NTSTATUS profiles_init( void )
{
    static const struct { unsigned int psm; int level; } servers[3] =
    {
        { PSM_SDP, BT_SECURITY_LOW },        /* anyone may ask what is served */
        { PSM_AVCTP, BT_SECURITY_MEDIUM },   /* paired devices, encrypted */
        { PSM_AVDTP, BT_SECURITY_MEDIUM },
    };

    a2dp.media_fd = -1;
    unlink( AUDIO_STATE );
    if (pipe2( wake_pipe, O_CLOEXEC | O_NONBLOCK )) return STATUS_UNSUCCESSFUL;
    for (int i = 0; i < 3; i++)
    {
        char request[32];
        int fd;

        snprintf( request, sizeof(request), "listen %u", servers[i].psm );
        if ((fd = bt_helper( request )) <= 0)
        {
            WARN( "PSM %u: not served\n", servers[i].psm );
            continue;
        }
        set_security( fd, servers[i].level );
        if (listen( fd, 4 ))
        {
            WARN( "PSM %u: %s\n", servers[i].psm, strerror( errno ) );
            close( fd );
            continue;
        }
        listen_fd[i] = fd;
        TRACE( "PSM %u served\n", servers[i].psm );
    }
    if (hfp_init()) WARN( "no hands-free\n" );
    if (pthread_create( &io_thread, NULL, io_proc, NULL )) return STATUS_UNSUCCESSFUL;
    return STATUS_SUCCESS;
}

void profiles_shutdown( void )
{
    stopping = TRUE;
    hfp_shutdown();
    stop_streamer();
    wake_io();
    unlink( AUDIO_STATE );
}
