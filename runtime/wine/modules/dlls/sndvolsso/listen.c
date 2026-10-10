/*
 * "Listen to this device": a microphone or a line in played on an output
 *
 * In Windows the audio service does it; in Arctic the shell does, as long as
 * the user is signed in: one thread a listened device copies what it records
 * to the output the Sound control panel chose (mmsys.cpl writes Listen and
 * ListenTo in HKCU\Software\Arctic\Audio\{endpoint}). A change of the
 * settings is taken at once.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS

#include "sndvolsso.h"
#include "mmdeviceapi.h"
#include "audioclient.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(sndvolsso);

/* Core Audio's GUIDs, which libuuid does not carry */
static const CLSID clsid_enumerator =
    { 0xbcde0395, 0xe52f, 0x467c, { 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e } };
static const IID iid_enumerator =
    { 0xa95664d2, 0x9614, 0x4f35, { 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6 } };
static const IID iid_audio_client =
    { 0x1cb9ad4c, 0xdbfa, 0x4c32, { 0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2 } };
static const IID iid_render_client =
    { 0xf294acfc, 0x3146, 0x4483, { 0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2 } };
static const IID iid_capture_client =
    { 0xc8adbd64, 0xe71e, 0x48a0, { 0xa4, 0xde, 0x18, 0x5c, 0x39, 0x5c, 0xd3, 0x17 } };

#define AUDIO_KEY   L"Software\\Arctic\\Audio"
#define MAX_LISTEN  8

struct listener
{
    WCHAR device[64];        /* the key's name: the endpoint's GUID */
    WCHAR target[128];       /* the output's id, "" for the default one */
    HANDLE thread;
    volatile LONG stop;
};

static struct listener listeners[MAX_LISTEN];
static HANDLE watch_thread, watch_stop;

/* the endpoint whose id ends with the GUID */
static IMMDevice *find_device( IMMDeviceEnumerator *devices, EDataFlow flow, const WCHAR *guid )
{
    IMMDeviceCollection *list;
    IMMDevice *found = NULL;
    UINT count, i;

    if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints( devices, flow, DEVICE_STATE_ACTIVE, &list ))) return NULL;
    IMMDeviceCollection_GetCount( list, &count );
    for (i = 0; i < count && !found; i++)
    {
        IMMDevice *device;
        WCHAR *id;

        if (FAILED(IMMDeviceCollection_Item( list, i, &device ))) continue;
        if (SUCCEEDED(IMMDevice_GetId( device, &id )))
        {
            UINT len = wcslen( id ), glen = wcslen( guid );
            if (len >= glen && !wcsicmp( id + len - glen, guid )) found = device;
            CoTaskMemFree( id );
        }
        if (found != device) IMMDevice_Release( device );
    }
    IMMDeviceCollection_Release( list );
    return found;
}

static DWORD WINAPI listen_thread( void *arg )
{
    struct listener *l = arg;
    IMMDeviceEnumerator *devices = NULL;
    IMMDevice *input = NULL, *output = NULL;
    IAudioClient *capture = NULL, *render = NULL;
    IAudioCaptureClient *recorded = NULL;
    IAudioRenderClient *played = NULL;
    WAVEFORMATEX *fmt = NULL;
    UINT32 size = 0, padding, frames;
    BYTE *from, *to;
    DWORD flags;

    CoInitializeEx( NULL, COINIT_MULTITHREADED );
    if (FAILED(CoCreateInstance( &clsid_enumerator, NULL, CLSCTX_INPROC_SERVER, &iid_enumerator,
                                 (void **)&devices )))
        goto done;
    if (!(input = find_device( devices, eCapture, l->device ))) goto done;
    if (l->target[0]) IMMDeviceEnumerator_GetDevice( devices, l->target, &output );
    if (!output && FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint( devices, eRender, eConsole, &output )))
        goto done;
    if (FAILED(IMMDevice_Activate( input, &iid_audio_client, CLSCTX_INPROC_SERVER, NULL, (void **)&capture )) ||
        FAILED(IMMDevice_Activate( output, &iid_audio_client, CLSCTX_INPROC_SERVER, NULL, (void **)&render )) ||
        FAILED(IAudioClient_GetMixFormat( capture, &fmt )))
        goto done;
    /* 50 ms each way: what Windows' own listening lags by */
    if (FAILED(IAudioClient_Initialize( capture, AUDCLNT_SHAREMODE_SHARED, 0, 500000, 0, fmt, NULL )) ||
        FAILED(IAudioClient_Initialize( render, AUDCLNT_SHAREMODE_SHARED,
                                        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                        1000000, 0, fmt, NULL )) ||
        FAILED(IAudioClient_GetService( capture, &iid_capture_client, (void **)&recorded )) ||
        FAILED(IAudioClient_GetService( render, &iid_render_client, (void **)&played )) ||
        FAILED(IAudioClient_GetBufferSize( render, &size )))
    {
        WARN( "%s cannot be played\n", debugstr_w( l->device ) );
        goto done;
    }
    TRACE( "%s to %s\n", debugstr_w( l->device ), debugstr_w( l->target ) );
    IAudioClient_Start( capture );
    IAudioClient_Start( render );
    while (!l->stop)
    {
        Sleep( 10 );
        while (SUCCEEDED(IAudioCaptureClient_GetBuffer( recorded, &from, &frames, &flags, NULL, NULL )) && frames)
        {
            UINT32 room = 0;

            if (SUCCEEDED(IAudioClient_GetCurrentPadding( render, &padding ))) room = size - padding;
            /* what does not fit is dropped: the lag must not grow */
            if (frames <= room && SUCCEEDED(IAudioRenderClient_GetBuffer( played, frames, &to )))
            {
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) memset( to, 0, frames * fmt->nBlockAlign );
                else memcpy( to, from, frames * fmt->nBlockAlign );
                IAudioRenderClient_ReleaseBuffer( played, frames, 0 );
            }
            IAudioCaptureClient_ReleaseBuffer( recorded, frames );
        }
    }
    IAudioClient_Stop( capture );
    IAudioClient_Stop( render );
done:
    if (played) IAudioRenderClient_Release( played );
    if (recorded) IAudioCaptureClient_Release( recorded );
    if (render) IAudioClient_Release( render );
    if (capture) IAudioClient_Release( capture );
    if (fmt) CoTaskMemFree( fmt );
    if (output) IMMDevice_Release( output );
    if (input) IMMDevice_Release( input );
    if (devices) IMMDeviceEnumerator_Release( devices );
    CoUninitialize();
    return 0;
}

static void stop_listener( struct listener *l )
{
    if (!l->thread) return;
    InterlockedExchange( &l->stop, 1 );
    WaitForSingleObject( l->thread, 3000 );
    CloseHandle( l->thread );
    memset( l, 0, sizeof(*l) );
}

/* what the settings say now against what plays */
static void update( HKEY root )
{
    struct { WCHAR device[64], target[128]; } wanted[MAX_LISTEN];
    UINT count = 0, i, j;
    WCHAR name[64];
    DWORD index;

    for (index = 0; count < MAX_LISTEN; index++)
    {
        DWORD on = 0, size = ARRAY_SIZE(name);
        HKEY key;

        if (RegEnumKeyExW( root, index, name, &size, NULL, NULL, NULL, NULL )) break;
        if (RegOpenKeyExW( root, name, 0, KEY_QUERY_VALUE, &key )) continue;
        size = sizeof(on);
        RegQueryValueExW( key, L"Listen", NULL, NULL, (BYTE *)&on, &size );
        if (on)
        {
            lstrcpynW( wanted[count].device, name, ARRAY_SIZE(wanted[count].device) );
            size = sizeof(wanted[count].target) - sizeof(WCHAR);
            memset( wanted[count].target, 0, sizeof(wanted[count].target) );
            RegQueryValueExW( key, L"ListenTo", NULL, NULL, (BYTE *)wanted[count].target, &size );
            count++;
        }
        RegCloseKey( key );
    }
    for (i = 0; i < MAX_LISTEN; i++)
    {
        struct listener *l = &listeners[i];
        BOOL keep = FALSE;
        DWORD code;

        if (!l->thread) continue;
        /* a thread that ended (the device went away) starts again below */
        if (GetExitCodeThread( l->thread, &code ) && code != STILL_ACTIVE) { stop_listener( l ); continue; }
        for (j = 0; j < count; j++)
            if (!wcsicmp( wanted[j].device, l->device ) && !wcscmp( wanted[j].target, l->target )) keep = TRUE;
        if (!keep) stop_listener( l );
    }
    for (j = 0; j < count; j++)
    {
        struct listener *free_one = NULL;
        BOOL running = FALSE;

        for (i = 0; i < MAX_LISTEN; i++)
        {
            if (listeners[i].thread && !wcsicmp( listeners[i].device, wanted[j].device )) running = TRUE;
            if (!listeners[i].thread && !free_one) free_one = &listeners[i];
        }
        if (running || !free_one) continue;
        wcscpy( free_one->device, wanted[j].device );
        wcscpy( free_one->target, wanted[j].target );
        free_one->stop = 0;
        free_one->thread = CreateThread( NULL, 0, listen_thread, free_one, 0, NULL );
    }
}

static DWORD WINAPI watch_proc( void *arg )
{
    HANDLE changed = CreateEventW( NULL, FALSE, FALSE, NULL ), waits[2] = { watch_stop, changed };
    HKEY root;

    if (RegCreateKeyExW( HKEY_CURRENT_USER, AUDIO_KEY, 0, NULL, 0, KEY_READ | KEY_NOTIFY, NULL, &root, NULL ))
        return 0;
    for (;;)
    {
        RegNotifyChangeKeyValue( root, TRUE, REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_CHANGE_NAME, changed, TRUE );
        update( root );
        /* also every half minute: a device plugged in again */
        if (WaitForMultipleObjects( 2, waits, FALSE, 30000 ) == WAIT_OBJECT_0) break;
    }
    for (UINT i = 0; i < MAX_LISTEN; i++) stop_listener( &listeners[i] );
    RegCloseKey( root );
    CloseHandle( changed );
    return 0;
}

void listen_start(void)
{
    if (watch_thread) return;
    watch_stop = CreateEventW( NULL, TRUE, FALSE, NULL );
    watch_thread = CreateThread( NULL, 0, watch_proc, NULL, 0, NULL );
}

void listen_stop(void)
{
    if (!watch_thread) return;
    SetEvent( watch_stop );
    WaitForSingleObject( watch_thread, 5000 );
    CloseHandle( watch_thread );
    CloseHandle( watch_stop );
    watch_thread = watch_stop = NULL;
}
