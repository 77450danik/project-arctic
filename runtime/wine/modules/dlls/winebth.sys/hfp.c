/*
 * Bluetooth bus: the headset's microphone (Hands-Free Profile, Audio Gateway)
 *
 * What Windows' BthHFEnum.sys and its audio driver do: the headset's control
 * connection (RFCOMM, AT commands of HFP 1.7, the service level connection
 * without telephony), its battery as it reports it (+IPHONEACCEV, +BIEV),
 * and the voice link (SCO, CVSD in the controller, 16-bit PCM at 8 kHz) while
 * a program records from the headset: what the headset hears goes to the
 * loopback card's second substream, which winealsa.drv shows as the
 * headset's microphone; what programs play goes to it over the same link,
 * A2DP being suspended meanwhile, as Windows switches to "Hands-Free".
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
#include <wine/debug.h>

#include "unixlib.h"
#include "unixlib_priv.h"
#include "bt.h"

WINE_DEFAULT_DEBUG_CHANNEL( winebth );

#undef TRACE
#undef WARN
#define TRACE( ... ) fprintf( stderr, "winebth: " __VA_ARGS__ )
#define WARN( ... ) fprintf( stderr, "winebth: " __VA_ARGS__ )

/* both ends of the loopback in the same format, whichever opens first: 48 kHz, 16 bits, stereo */
#define MIC_WRITE        "hw:CARD=Bluetooth,DEV=0,SUBDEV=1"
#define SPEAKER_READ     "hw:CARD=Bluetooth,DEV=1,SUBDEV=0"
#define MIC_CAPTURE_STATUS "/proc/asound/card%d/pcm1c/sub1/status"

static pthread_mutex_t hfp_lock = PTHREAD_MUTEX_INITIALIZER;
static int listen_fd = -1;
static pthread_t thread, voice_thread;
static BOOL stopping;

static struct
{
    BOOL present;
    bdaddr_t addr;
    int rfcomm;
    char line[512];
    UINT line_len;
    UINT32 hf_features;
    BOOL slc;                  /* the service level connection is up */
    BOOL voice;                /* the SCO link carries the voice */
    BOOL voice_running;
    int sco, sco_mtu;
    time_t mic_quiet_since;
} hfp = { .rfcomm = -1, .sco = -1 };

static void hfp_close( void );

static const char *addr_str( const bdaddr_t *a )
{
    return wine_dbg_sprintf( "%02X:%02X:%02X:%02X:%02X:%02X", a->b[5], a->b[4], a->b[3], a->b[2], a->b[1], a->b[0] );
}

BOOL hfp_voice_active( void )
{
    return hfp.voice;
}

BOOL hfp_connected( const BYTE addr[6] )
{
    BOOL ret;

    pthread_mutex_lock( &hfp_lock );
    ret = hfp.present && hfp.slc && !memcmp( &hfp.addr, addr, 6 );
    pthread_mutex_unlock( &hfp_lock );
    return ret;
}

/**********************************************************************
 *          AT commands
 */

static void at_send( const char *text )
{
    char buf[512];
    int len = snprintf( buf, sizeof(buf), "\r\n%s\r\n", text );

    if (hfp.rfcomm >= 0) send( hfp.rfcomm, buf, len, MSG_NOSIGNAL );
}

static void at_command( char *cmd )
{
    TRACE( "headset: %s\n", cmd );
    if (!strncmp( cmd, "AT+BRSF=", 8 ))
    {
        hfp.hf_features = strtoul( cmd + 8, NULL, 10 );
        /* this side: no telephony, CVSD only (no codec negotiation); HF indicators (bit 10): the battery */
        at_send( "+BRSF: 1024" );
        at_send( "OK" );
    }
    else if (!strcmp( cmd, "AT+CIND=?" ))
    {
        at_send( "+CIND: (\"service\",(0,1)),(\"call\",(0,1)),(\"callsetup\",(0-3)),(\"callheld\",(0-2)),"
                 "(\"signal\",(0-5)),(\"roam\",(0,1)),(\"battchg\",(0-5))" );
        at_send( "OK" );
    }
    else if (!strcmp( cmd, "AT+CIND?" ))
    {
        at_send( "+CIND: 1,0,0,0,5,0,5" );
        at_send( "OK" );
    }
    else if (!strncmp( cmd, "AT+CMER=", 8 ))
    {
        at_send( "OK" );
        /* this side has no three-way calling: no CHLD follows, the connection is up now
         * (the HF indicators' BIND, when they come, are answered as they come) */
        if (!hfp.slc)
        {
            hfp.slc = TRUE;
            TRACE( "%s: hands-free connected (features %#x)\n", addr_str( &hfp.addr ), hfp.hf_features );
            audio_state_changed();
        }
    }
    else if (!strcmp( cmd, "AT+CHLD=?" ))
    {
        at_send( "+CHLD: (0,1,2,3)" );
        at_send( "OK" );
        if (!hfp.slc)
        {
            hfp.slc = TRUE;
            TRACE( "%s: hands-free connected\n", addr_str( &hfp.addr ) );
            audio_state_changed();
        }
    }
    else if (!strcmp( cmd, "AT+BIND=?" ))
    {
        at_send( "+BIND: (1,2)" );
        at_send( "OK" );
    }
    else if (!strcmp( cmd, "AT+BIND?" ))
    {
        at_send( "+BIND: 1,1" );
        at_send( "+BIND: 2,1" );
        at_send( "OK" );
    }
    else if (!strncmp( cmd, "AT+BIEV=", 8 ))
    {
        /* HF indicator 2: the battery, 0-100 */
        int id, value;

        if (sscanf( cmd + 8, "%d,%d", &id, &value ) == 2 && id == 2) hci_device_set_battery( hfp.addr.b, value );
        at_send( "OK" );
    }
    else if (!strncmp( cmd, "AT+XAPL=", 8 ))
    {
        /* the headset reports its battery as to an iPhone */
        at_send( "+XAPL=iPhone,6" );
        at_send( "OK" );
    }
    else if (!strncmp( cmd, "AT+IPHONEACCEV=", 15 ))
    {
        /* pairs of key and value; key 1 is the battery, 0-9 */
        char *p = cmd + 15;
        int count = strtol( p, &p, 10 );

        for (int i = 0; i < count && *p == ','; i++)
        {
            int key = strtol( p + 1, &p, 10 ), value;

            if (*p != ',') break;
            value = strtol( p + 1, &p, 10 );
            if (key == 1) hci_device_set_battery( hfp.addr.b, (value + 1) * 10 );
        }
        at_send( "OK" );
    }
    else if (!strcmp( cmd, "AT+COPS?" ))
    {
        at_send( "+COPS: 0" );
        at_send( "OK" );
    }
    else if (!strncmp( cmd, "AT+BVRA", 7 ) || !strncmp( cmd, "ATD", 3 ) || !strcmp( cmd, "ATA" ) ||
             !strncmp( cmd, "AT+CHUP", 7 ) || !strncmp( cmd, "AT+BLDN", 7 ))
        at_send( "ERROR" );   /* no telephony here */
    else
        at_send( "OK" );      /* CLIP, CCWA, CMEE, NREC, BIA, VGS, VGM, BAC, BCS, CLCC, BTRH, CNUM... */
}

static void read_rfcomm( void )
{
    char buf[256];
    ssize_t len = recv( hfp.rfcomm, buf, sizeof(buf), MSG_DONTWAIT );

    if (len < 0 && (errno == EAGAIN || errno == EINTR)) return;
    if (len <= 0)
    {
        TRACE( "%s: hands-free closed\n", addr_str( &hfp.addr ) );
        hfp_close();
        return;
    }
    for (ssize_t i = 0; i < len; i++)
    {
        if (buf[i] == '\r' || buf[i] == '\n')
        {
            if (hfp.line_len)
            {
                hfp.line[hfp.line_len] = 0;
                at_command( hfp.line );
                hfp.line_len = 0;
            }
        }
        else if (hfp.line_len < sizeof(hfp.line) - 1) hfp.line[hfp.line_len++] = buf[i];
    }
}

/**********************************************************************
 *          The voice link
 */

static BOOL mic_in_use( void )
{
    char path[64], line[64] = "";
    FILE *f;

    snprintf( path, sizeof(path), MIC_CAPTURE_STATUS, 31 );
    if (!(f = fopen( path, "r" ))) return FALSE;
    if (!fgets( line, sizeof(line), f )) line[0] = 0;
    fclose( f );
    return !strncmp( line, "state: RUNNING", 14 );
}

static snd_pcm_t *open_pcm( const char *name, snd_pcm_stream_t stream, int channels, int rate )
{
    snd_pcm_t *pcm;
    int err;

    if ((err = snd_pcm_open( &pcm, name, stream, SND_PCM_NONBLOCK )) < 0)
    {
        WARN( "%s: %s\n", name, snd_strerror( err ) );
        return NULL;
    }
    if ((err = snd_pcm_set_params( pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, channels, rate, 1,
                                   60000 )) < 0)
    {
        WARN( "%s: %s\n", name, snd_strerror( err ) );
        snd_pcm_close( pcm );
        return NULL;
    }
    /* the start threshold is the whole buffer: a capture read in small parts is started here */
    if (stream == SND_PCM_STREAM_CAPTURE) snd_pcm_start( pcm );
    return pcm;
}

/* the microphone's side starts with 20 ms in it: steady, not late */
static void start_when_filled( snd_pcm_t *pcm )
{
    snd_pcm_uframes_t buffer, period;

    if (snd_pcm_state( pcm ) != SND_PCM_STATE_PREPARED || snd_pcm_get_params( pcm, &buffer, &period ) < 0) return;
    if (buffer - snd_pcm_avail_update( pcm ) >= 960) snd_pcm_start( pcm );
}

/* each packet the headset sends is answered by one of the same length: the link's own clock */
static void *voice_proc( void *arg )
{
    snd_pcm_t *mic = open_pcm( MIC_WRITE, SND_PCM_STREAM_PLAYBACK, 2, 48000 ), *speaker = NULL;
    int16_t in[512], out[512], wide[512 * 6 * 2], last = 0;
    int tries = 0;

    TRACE( "voice link up, MTU %d\n", hfp.sco_mtu );
    while (hfp.voice)
    {
        struct pollfd pfd = { hfp.sco, POLLIN };
        ssize_t len;
        int samples;

        /* the music's reader gives the loopback up when it sees the voice */
        if (!speaker && tries++ % 25 == 0) speaker = open_pcm( SPEAKER_READ, SND_PCM_STREAM_CAPTURE, 2, 48000 );
        if (poll( &pfd, 1, 200 ) <= 0) continue;
        len = recv( hfp.sco, in, min( sizeof(in), (size_t)hfp.sco_mtu ), 0 );
        if (len <= 0) break;
        samples = len / 2;
        if (mic)
        {
            snd_pcm_sframes_t n;

            /* 8 kHz mono to 48 kHz stereo, linearly */
            for (int i = 0; i < samples; i++)
            {
                for (int j = 0; j < 6; j++)
                {
                    int16_t v = last + (in[i] - last) * (j + 1) / 6;
                    wide[(i * 6 + j) * 2] = wide[(i * 6 + j) * 2 + 1] = v;
                }
                last = in[i];
            }
            n = snd_pcm_writei( mic, wide, samples * 6 );
            if (n == -EPIPE) snd_pcm_prepare( mic );
            start_when_filled( mic );
        }
        memset( out, 0, sizeof(out) );
        if (speaker)
        {
            /* 48 kHz stereo to 8 kHz mono: the mean of six frames */
            snd_pcm_sframes_t got = snd_pcm_readi( speaker, wide, samples * 6 );
            if (got == -EPIPE)
            {
                snd_pcm_prepare( speaker );
                snd_pcm_start( speaker );
            }
            for (int i = 0; got > 0 && i < samples && i * 6 + 5 < got; i++)
            {
                int sum = 0;
                for (int j = 0; j < 6; j++) sum += wide[(i * 6 + j) * 2] + wide[(i * 6 + j) * 2 + 1];
                out[i] = sum / 12;
            }
        }
        send( hfp.sco, out, len, MSG_NOSIGNAL | MSG_DONTWAIT );
    }
    if (mic) snd_pcm_close( mic );
    if (speaker) snd_pcm_close( speaker );
    TRACE( "voice link down\n" );
    return NULL;
}

static void start_voice( void )
{
    struct sockaddr_sco sa = { .sco_family = AF_BLUETOOTH, .sco_bdaddr = hfp.addr };
    struct sco_options opts;
    socklen_t len = sizeof(opts);
    int fd;

    if ((fd = socket( AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_CLOEXEC, BTPROTO_SCO )) < 0) return;
    if (connect( fd, (struct sockaddr *)&sa, sizeof(sa) ))
    {
        WARN( "SCO: %s\n", strerror( errno ) );
        close( fd );
        return;
    }
    hfp.sco = fd;
    hfp.sco_mtu = !getsockopt( fd, SOL_SCO, SCO_OPTIONS, &opts, &len ) && opts.mtu ? opts.mtu : 48;
    hfp.voice = TRUE;
    if (pthread_create( &voice_thread, NULL, voice_proc, NULL ))
    {
        hfp.voice = FALSE;
        close( fd );
        hfp.sco = -1;
        return;
    }
    hfp.voice_running = TRUE;
}

static void stop_voice( void )
{
    if (!hfp.voice_running) return;
    hfp.voice = FALSE;
    shutdown( hfp.sco, SHUT_RDWR );
    pthread_join( voice_thread, NULL );
    hfp.voice_running = FALSE;
    close( hfp.sco );
    hfp.sco = -1;
}

/**********************************************************************
 *          The connection
 */

static void take_connection( int fd, const bdaddr_t *addr )
{
    pthread_mutex_lock( &hfp_lock );
    if (hfp.present)
    {
        pthread_mutex_unlock( &hfp_lock );
        close( fd );
        return;
    }
    hfp.present = TRUE;
    hfp.addr = *addr;
    hfp.rfcomm = fd;
    hfp.slc = FALSE;
    hfp.line_len = 0;
    hfp.hf_features = 0;
    hfp.mic_quiet_since = 0;
    pthread_mutex_unlock( &hfp_lock );
    TRACE( "%s: hands-free control connected\n", addr_str( addr ) );
}

/* the control connection closed (the thread of hfp_proc) */
static void hfp_close( void )
{
    BOOL changed;

    stop_voice();
    pthread_mutex_lock( &hfp_lock );
    if (hfp.rfcomm >= 0) close( hfp.rfcomm );
    hfp.rfcomm = -1;
    changed = hfp.slc;
    hfp.present = hfp.slc = FALSE;
    pthread_mutex_unlock( &hfp_lock );
    if (changed) audio_state_changed();
}

/* the device disconnects: its control connection ends, hfp_proc closes it */
void hfp_device_gone( const BYTE addr[6] )
{
    pthread_mutex_lock( &hfp_lock );
    if (hfp.present && !memcmp( &hfp.addr, addr, 6 ) && hfp.rfcomm >= 0) shutdown( hfp.rfcomm, SHUT_RDWR );
    pthread_mutex_unlock( &hfp_lock );
}

static void *hfp_proc( void *arg )
{
    while (!stopping)
    {
        struct pollfd fds[2] = { { listen_fd, POLLIN }, { hfp.rfcomm, POLLIN } };
        int count = hfp.rfcomm >= 0 ? 2 : 1;

        if (poll( fds, count, 250 ) > 0)
        {
            if (fds[0].revents & POLLIN)
            {
                struct sockaddr_rc sa;
                socklen_t len = sizeof(sa);
                int fd = accept4( listen_fd, (struct sockaddr *)&sa, &len, SOCK_CLOEXEC );

                if (fd >= 0) take_connection( fd, &sa.rc_bdaddr );
            }
            if (count > 1 && fds[1].revents) read_rfcomm();
        }

        /* a program records from the headset: the voice link comes up, and goes a while after */
        if (hfp.slc)
        {
            BOOL used = mic_in_use();

            if (used) hfp.mic_quiet_since = 0;
            if (used && !hfp.voice_running) start_voice();
            else if (!used && hfp.voice_running)
            {
                if (!hfp.mic_quiet_since) hfp.mic_quiet_since = time( NULL );
                else if (time( NULL ) - hfp.mic_quiet_since >= 2) stop_voice();
            }
        }
    }
    return NULL;
}

/* after the music: the control connection, when the headset has not made it itself */
void hfp_connect( const BYTE addr[6] )
{
    struct sockaddr_rc sa = { .rc_family = AF_BLUETOOTH };
    struct bt_security sec = { BT_SECURITY_MEDIUM, 0 };
    int channel, fd;

    if (hfp.present) return;
    memcpy( &sa.rc_bdaddr, addr, 6 );
    if ((channel = sdp_rfcomm_channel( &sa.rc_bdaddr, 0x111e )) <= 0)
    {
        TRACE( "%s: no hands-free\n", addr_str( &sa.rc_bdaddr ) );
        return;
    }
    if (hfp.present) return;
    sa.rc_channel = channel;
    if ((fd = socket( AF_BLUETOOTH, SOCK_STREAM | SOCK_CLOEXEC, BTPROTO_RFCOMM )) < 0) return;
    setsockopt( fd, SOL_BLUETOOTH, BT_SECURITY, &sec, sizeof(sec) );
    if (connect( fd, (struct sockaddr *)&sa, sizeof(sa) ))
    {
        WARN( "hands-free RFCOMM %d: %s\n", channel, strerror( errno ) );
        close( fd );
        return;
    }
    take_connection( fd, &sa.rc_bdaddr );
}

NTSTATUS hfp_init( void )
{
    struct sockaddr_rc sa = { .rc_family = AF_BLUETOOTH, .rc_channel = HFP_CHANNEL };
    struct bt_security sec = { BT_SECURITY_MEDIUM, 0 };

    if ((listen_fd = socket( AF_BLUETOOTH, SOCK_STREAM | SOCK_CLOEXEC, BTPROTO_RFCOMM )) < 0) return STATUS_UNSUCCESSFUL;
    setsockopt( listen_fd, SOL_BLUETOOTH, BT_SECURITY, &sec, sizeof(sec) );
    if (bind( listen_fd, (struct sockaddr *)&sa, sizeof(sa) ) || listen( listen_fd, 2 ))
    {
        WARN( "hands-free channel %d: %s\n", HFP_CHANNEL, strerror( errno ) );
        close( listen_fd );
        listen_fd = -1;
        return STATUS_UNSUCCESSFUL;
    }
    if (pthread_create( &thread, NULL, hfp_proc, NULL )) return STATUS_UNSUCCESSFUL;
    return STATUS_SUCCESS;
}

void hfp_shutdown( void )
{
    stopping = TRUE;
    stop_voice();
}
