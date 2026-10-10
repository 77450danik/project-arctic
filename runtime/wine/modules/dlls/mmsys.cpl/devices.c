/*
 * Sound of the Control Panel: the endpoints
 *
 * What the panel lists and changes comes from the Core Audio API, as in
 * Windows: the endpoints of the enumerator with their names, adapters and
 * form factors, their IAudioEndpointVolume (the level, each channel's, the
 * mute: the whole system's, mmdevapi), the format programs mix at. What
 * Windows keeps in its audio service is Arctic's in the registry, where
 * mmdevapi reads it (Wine patch 0080): HKCU\Software\Arctic\Audio\{GUID}
 * with Name, Disabled and FormatRate; the default endpoints are Wine's, in
 * the driver's key.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS

#include <math.h>

#include "mmsys.h"
#include "objbase.h"
#include "mmdeviceapi.h"
#include "endpointvolume.h"
#include "audioclient.h"
#include "mmreg.h"
#include "ks.h"
#include "ksmedia.h"

/* Core Audio's GUIDs, which libuuid does not carry */
static const CLSID clsid_enumerator =
    { 0xbcde0395, 0xe52f, 0x467c, { 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e } };
static const IID iid_enumerator =
    { 0xa95664d2, 0x9614, 0x4f35, { 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6 } };
static const IID iid_endpoint_volume =
    { 0x5cdf2c82, 0x841e, 0x4546, { 0x97, 0x22, 0x0c, 0xf7, 0x40, 0x78, 0x22, 0x9a } };
static const IID iid_audio_client =
    { 0x1cb9ad4c, 0xdbfa, 0x4c32, { 0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2 } };
static const IID iid_render_client =
    { 0xf294acfc, 0x3146, 0x4483, { 0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2 } };
static const GUID subtype_float =
    { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const PROPERTYKEY key_device_desc =     /* PKEY_Device_DeviceDesc */
    { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 2 };
static const PROPERTYKEY key_driver =          /* DEVPKEY_Device_Driver, which mmdevapi sets to the driver */
    { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 11 };
static const PROPERTYKEY key_adapter =         /* DEVPKEY_DeviceInterface_FriendlyName */
    { { 0x026e516e, 0xb814, 0x414b, { 0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22 } }, 2 };
static const PROPERTYKEY key_form_factor =     /* PKEY_AudioEndpoint_FormFactor */
    { { 0x1da5d803, 0xd492, 0x4edd, { 0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e } }, 0 };

/* the changes this panel makes, as IAudioEndpointVolumeCallback sees them */
static const GUID volume_context =
    { 0x7a1c5e02, 0x93b4, 0x4d6f, { 0xa2, 0x18, 0x4c, 0x0e, 0x6b, 0x95, 0xd7, 0x31 } };

static IMMDeviceEnumerator *get_enumerator(void)
{
    static IMMDeviceEnumerator *enumerator;

    if (!enumerator)
    {
        CoInitializeEx( NULL, COINIT_APARTMENTTHREADED );
        CoCreateInstance( &clsid_enumerator, NULL, CLSCTX_INPROC_SERVER, &iid_enumerator, (void **)&enumerator );
    }
    return enumerator;
}

static IMMDevice *open_device( const WCHAR *id )
{
    IMMDeviceEnumerator *enumerator = get_enumerator();
    IMMDevice *device = NULL;

    if (enumerator) IMMDeviceEnumerator_GetDevice( enumerator, id, &device );
    return device;
}

static BOOL get_string( IMMDevice *device, const PROPERTYKEY *key, WCHAR *buf, int size )
{
    IPropertyStore *store;
    PROPVARIANT pv;
    BOOL ret = FALSE;

    buf[0] = 0;
    if (FAILED(IMMDevice_OpenPropertyStore( device, STGM_READ, &store ))) return FALSE;
    PropVariantInit( &pv );
    if (SUCCEEDED(IPropertyStore_GetValue( store, key, &pv )) && pv.vt == VT_LPWSTR && pv.pwszVal)
    {
        lstrcpynW( buf, pv.pwszVal, size );
        ret = TRUE;
    }
    PropVariantClear( &pv );
    IPropertyStore_Release( store );
    return ret;
}

static UINT get_form_factor( IMMDevice *device )
{
    IPropertyStore *store;
    PROPVARIANT pv;
    UINT ret = ~0u;

    if (FAILED(IMMDevice_OpenPropertyStore( device, STGM_READ, &store ))) return ret;
    PropVariantInit( &pv );
    if (SUCCEEDED(IPropertyStore_GetValue( store, &key_form_factor, &pv )) && pv.vt == VT_UI4) ret = pv.ulVal;
    PropVariantClear( &pv );
    IPropertyStore_Release( store );
    return ret;
}

/* the endpoint's GUID: the end of its id, "{0.0.0.00000000}.{GUID}" */
static const WCHAR *id_guid( const WCHAR *id )
{
    const WCHAR *brace = wcsrchr( id, '{' );
    return brace ? brace : id;
}

static HKEY arctic_key( const WCHAR *id, REGSAM access )
{
    WCHAR name[128];
    HKEY key;

    swprintf( name, ARRAY_SIZE(name), L"Software\\Arctic\\Audio\\%s", id_guid( id ) );
    if (RegCreateKeyExW( HKEY_CURRENT_USER, name, 0, NULL, 0, access, NULL, &key, NULL )) return NULL;
    return key;
}

/* Listen = 1 and ListenTo, the output's id ("" for the default one) */
BOOL listen_get( const WCHAR *id, WCHAR *target, UINT count )
{
    DWORD on = 0, size = sizeof(on);
    HKEY key;

    target[0] = 0;
    if (!(key = arctic_key( id, KEY_QUERY_VALUE ))) return FALSE;
    RegQueryValueExW( key, L"Listen", NULL, NULL, (BYTE *)&on, &size );
    size = count * sizeof(WCHAR);
    if (RegQueryValueExW( key, L"ListenTo", NULL, NULL, (BYTE *)target, &size )) target[0] = 0;
    target[count - 1] = 0;
    RegCloseKey( key );
    return on != 0;
}

void listen_set( const WCHAR *id, BOOL on, const WCHAR *target )
{
    DWORD value = !!on;
    HKEY key;

    if (!(key = arctic_key( id, KEY_SET_VALUE ))) return;
    RegSetValueExW( key, L"ListenTo", 0, REG_SZ, (const BYTE *)target, (wcslen( target ) + 1) * sizeof(WCHAR) );
    RegSetValueExW( key, L"Listen", 0, REG_DWORD, (const BYTE *)&value, sizeof(value) );
    RegCloseKey( key );
}

/* "Speakers" in the user's language: the driver names what an endpoint is in
 * English, as Wine's drivers do; a name the user gave stays as it is */
static void display_name( const WCHAR *desc, WCHAR *out, int size )
{
    static const struct { const WCHAR *english; UINT id; } kinds[] =
    {
        { L"Speakers", IDS_SPEAKERS },
        { L"Headphones", IDS_HEADPHONES },
        { L"Digital Output", IDS_DIGITAL_OUTPUT },
        { L"Digital Display", IDS_DIGITAL_DISPLAY },
        { L"Microphone", IDS_MICROPHONE },
        { L"Line In", IDS_LINE_IN },
    };

    for (UINT i = 0; i < ARRAY_SIZE(kinds); i++)
    {
        if (wcscmp( desc, kinds[i].english )) continue;
        lstrcpynW( out, load_string( kinds[i].id ), size );
        return;
    }
    lstrcpynW( out, desc, size );
}

static UINT device_icon( UINT form_factor, const WCHAR *desc, BOOL capture )
{
    switch (form_factor)
    {
    case Headphones: return IDI_HEADPHONES;
    case Headset: return IDI_HEADSET;
    case SPDIF: return IDI_DIGITAL;
    case DigitalAudioDisplayDevice: return IDI_DISPLAY;
    case LineLevel: return IDI_LINE;
    case Microphone: return IDI_MICROPHONE;
    }
    if (!wcsncmp( desc, L"Digital", 7 )) return IDI_DIGITAL;
    if (!wcsncmp( desc, L"Headphones", 10 )) return IDI_HEADPHONES;
    return capture ? IDI_MICROPHONE : IDI_SPEAKERS;
}

static void describe( IMMDevice *device, BOOL capture, struct device *dev )
{
    WCHAR *id = NULL, desc[128];

    memset( dev, 0, sizeof(*dev) );
    dev->capture = capture;
    if (SUCCEEDED(IMMDevice_GetId( device, &id )) && id)
    {
        lstrcpynW( dev->id, id, ARRAY_SIZE(dev->id) );
        CoTaskMemFree( id );
    }
    IMMDevice_GetState( device, &dev->state );
    if (!get_string( device, &key_device_desc, desc, ARRAY_SIZE(desc) ))
        lstrcpyW( desc, capture ? L"Microphone" : L"Speakers" );
    display_name( desc, dev->name, ARRAY_SIZE(dev->name) );
    get_string( device, &key_adapter, dev->adapter, ARRAY_SIZE(dev->adapter) );
    dev->icon = device_icon( get_form_factor( device ), desc, capture );
}

static void default_id( BOOL capture, ERole role, WCHAR *id, int size )
{
    IMMDeviceEnumerator *enumerator = get_enumerator();
    IMMDevice *device;
    WCHAR *str = NULL;

    id[0] = 0;
    if (!enumerator || FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint( enumerator, capture ? eCapture : eRender,
                                                                           role, &device )))
        return;
    if (SUCCEEDED(IMMDevice_GetId( device, &str )) && str)
    {
        lstrcpynW( id, str, size );
        CoTaskMemFree( str );
    }
    IMMDevice_Release( device );
}

UINT devices_list( BOOL capture, struct device *list, UINT max )
{
    IMMDeviceEnumerator *enumerator = get_enumerator();
    IMMDeviceCollection *collection;
    WCHAR def[128], def_comm[128];
    UINT count = 0, n = 0;

    if (!enumerator) return 0;
    if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints( enumerator, capture ? eCapture : eRender,
                                                       DEVICE_STATE_ACTIVE | DEVICE_STATE_DISABLED |
                                                       DEVICE_STATE_UNPLUGGED, &collection )))
        return 0;
    default_id( capture, eMultimedia, def, ARRAY_SIZE(def) );
    default_id( capture, eCommunications, def_comm, ARRAY_SIZE(def_comm) );
    IMMDeviceCollection_GetCount( collection, &count );
    for (UINT i = 0; i < count && n < max; i++)
    {
        IMMDevice *device;

        if (FAILED(IMMDeviceCollection_Item( collection, i, &device ))) continue;
        describe( device, capture, &list[n] );
        IMMDevice_Release( device );
        if (!list[n].id[0]) continue;
        list[n].is_default = list[n].state == DEVICE_STATE_ACTIVE && !wcscmp( list[n].id, def );
        list[n].is_default_comm = list[n].state == DEVICE_STATE_ACTIVE && !wcscmp( list[n].id, def_comm );
        n++;
    }
    IMMDeviceCollection_Release( collection );
    return n;
}

BOOL device_get( const WCHAR *id, struct device *dev )
{
    struct device list[32];
    UINT count;

    for (int capture = 0; capture < 2; capture++)
    {
        count = devices_list( capture, list, ARRAY_SIZE(list) );
        for (UINT i = 0; i < count; i++)
        {
            if (wcscmp( list[i].id, id )) continue;
            *dev = list[i];
            return TRUE;
        }
    }
    return FALSE;
}

/* the default as Wine keeps it, in the driver's key; the programs that follow
 * the default hear of it through IMMNotificationClient */
void device_set_default( const struct device *dev, BOOL communications_only )
{
    WCHAR driver[64], key_name[128];
    IMMDevice *device;
    HKEY key;

    if (!(device = open_device( dev->id ))) return;
    if (!get_string( device, &key_driver, driver, ARRAY_SIZE(driver) )) lstrcpyW( driver, L"winealsa.drv" );
    IMMDevice_Release( device );

    swprintf( key_name, ARRAY_SIZE(key_name), L"Software\\Wine\\Drivers\\%s", driver );
    if (RegCreateKeyExW( HKEY_CURRENT_USER, key_name, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL )) return;
    if (!communications_only)
        RegSetValueExW( key, dev->capture ? L"DefaultInput" : L"DefaultOutput", 0, REG_SZ, (const BYTE *)dev->id,
                        (lstrlenW( dev->id ) + 1) * sizeof(WCHAR) );
    RegSetValueExW( key, dev->capture ? L"DefaultVoiceInput" : L"DefaultVoiceOutput", 0, REG_SZ, (const BYTE *)dev->id,
                    (lstrlenW( dev->id ) + 1) * sizeof(WCHAR) );
    RegCloseKey( key );
}

void device_set_name( const struct device *dev, const WCHAR *name )
{
    HKEY key = arctic_key( dev->id, KEY_SET_VALUE );

    if (!key) return;
    if (name && name[0]) RegSetValueExW( key, L"Name", 0, REG_SZ, (const BYTE *)name, (lstrlenW( name ) + 1) * sizeof(WCHAR) );
    else RegDeleteValueW( key, L"Name" );
    RegCloseKey( key );
}

/* a device turned off is another's turn to be the default */
void device_set_enabled( const struct device *dev, BOOL enabled )
{
    HKEY key = arctic_key( dev->id, KEY_SET_VALUE );
    DWORD disabled = !enabled;

    if (!key) return;
    RegSetValueExW( key, L"Disabled", 0, REG_DWORD, (BYTE *)&disabled, sizeof(disabled) );
    RegCloseKey( key );
    if (!enabled && (dev->is_default || dev->is_default_comm))
    {
        struct device list[32];
        UINT count = devices_list( dev->capture, list, ARRAY_SIZE(list) );

        for (UINT i = 0; i < count; i++)
        {
            if (!wcscmp( list[i].id, dev->id ) || list[i].state != DEVICE_STATE_ACTIVE) continue;
            device_set_default( &list[i], !dev->is_default );
            break;
        }
    }
}

/**********************************************************************
 *          The volume
 */

static IAudioEndpointVolume *open_volume( const WCHAR *id )
{
    IMMDevice *device = open_device( id );
    IAudioEndpointVolume *volume = NULL;

    if (!device) return NULL;
    IMMDevice_Activate( device, &iid_endpoint_volume, CLSCTX_INPROC_SERVER, NULL, (void **)&volume );
    IMMDevice_Release( device );
    return volume;
}

BOOL volume_get( const WCHAR *id, float *level, BOOL *mute, UINT *channels )
{
    IAudioEndpointVolume *volume = open_volume( id );
    BOOL muted = FALSE;
    UINT count = 2;

    *level = 0;
    if (mute) *mute = FALSE;
    if (channels) *channels = 0;
    if (!volume) return FALSE;
    IAudioEndpointVolume_GetMasterVolumeLevelScalar( volume, level );
    IAudioEndpointVolume_GetMute( volume, &muted );
    IAudioEndpointVolume_GetChannelCount( volume, &count );
    IAudioEndpointVolume_Release( volume );
    if (mute) *mute = muted;
    if (channels) *channels = count;
    return TRUE;
}

void volume_set( const WCHAR *id, float level )
{
    IAudioEndpointVolume *volume = open_volume( id );

    if (!volume) return;
    IAudioEndpointVolume_SetMasterVolumeLevelScalar( volume, max( 0.f, min( 1.f, level ) ), &volume_context );
    IAudioEndpointVolume_Release( volume );
}

void volume_set_mute( const WCHAR *id, BOOL mute )
{
    IAudioEndpointVolume *volume = open_volume( id );

    if (!volume) return;
    IAudioEndpointVolume_SetMute( volume, mute, &volume_context );
    IAudioEndpointVolume_Release( volume );
}

float volume_channel( const WCHAR *id, UINT channel )
{
    IAudioEndpointVolume *volume = open_volume( id );
    float level = 0;

    if (!volume) return 0;
    IAudioEndpointVolume_GetChannelVolumeLevelScalar( volume, channel, &level );
    IAudioEndpointVolume_Release( volume );
    return level;
}

void volume_set_channel( const WCHAR *id, UINT channel, float level )
{
    IAudioEndpointVolume *volume = open_volume( id );

    if (!volume) return;
    IAudioEndpointVolume_SetChannelVolumeLevelScalar( volume, channel, max( 0.f, min( 1.f, level ) ), &volume_context );
    IAudioEndpointVolume_Release( volume );
}

/**********************************************************************
 *          The format
 */

static IAudioClient *open_client( const WCHAR *id )
{
    IMMDevice *device = open_device( id );
    IAudioClient *client = NULL;

    if (!device) return NULL;
    IMMDevice_Activate( device, &iid_audio_client, CLSCTX_INPROC_SERVER, NULL, (void **)&client );
    IMMDevice_Release( device );
    return client;
}

BOOL format_get_mix( const WCHAR *id, struct format_choice *fmt )
{
    IAudioClient *client = open_client( id );
    WAVEFORMATEX *mix = NULL;
    BOOL ret = FALSE;

    memset( fmt, 0, sizeof(*fmt) );
    if (!client) return FALSE;
    if (SUCCEEDED(IAudioClient_GetMixFormat( client, &mix )) && mix)
    {
        fmt->channels = mix->nChannels;
        fmt->rate = mix->nSamplesPerSec;
        fmt->bits = 16;
        CoTaskMemFree( mix );
        ret = TRUE;
    }
    IAudioClient_Release( client );
    return ret;
}

BOOL format_get_chosen( const WCHAR *id, struct format_choice *fmt )
{
    HKEY key = arctic_key( id, KEY_QUERY_VALUE );
    DWORD rate = 0, bits = 0, size = sizeof(rate);
    BOOL ret;

    if (!key) return FALSE;
    ret = !RegQueryValueExW( key, L"FormatRate", NULL, NULL, (BYTE *)&rate, &size ) && rate;
    size = sizeof(bits);
    RegQueryValueExW( key, L"FormatBits", NULL, NULL, (BYTE *)&bits, &size );
    RegCloseKey( key );
    if (ret)
    {
        fmt->rate = rate;
        fmt->bits = bits ? bits : 16;
    }
    return ret;
}

void format_choose( const WCHAR *id, const struct format_choice *fmt )
{
    HKEY key = arctic_key( id, KEY_SET_VALUE );
    DWORD rate, bits;

    if (!key) return;
    if (fmt)
    {
        rate = fmt->rate;
        bits = fmt->bits;
        RegSetValueExW( key, L"FormatRate", 0, REG_DWORD, (BYTE *)&rate, sizeof(rate) );
        RegSetValueExW( key, L"FormatBits", 0, REG_DWORD, (BYTE *)&bits, sizeof(bits) );
    }
    else
    {
        RegDeleteValueW( key, L"FormatRate" );
        RegDeleteValueW( key, L"FormatBits" );
    }
    RegCloseKey( key );
}

/* "Перевірити": a chime through the endpoint at that rate, each channel in turn */
BOOL format_test( const WCHAR *id, const struct format_choice *fmt )
{
    IAudioClient *client = open_client( id );
    IAudioRenderClient *render = NULL;
    WAVEFORMATEXTENSIBLE *mix = NULL, format;
    UINT32 frames, total, done = 0, buffer;
    BOOL ret = FALSE;

    if (!client) return FALSE;
    if (FAILED(IAudioClient_GetMixFormat( client, (WAVEFORMATEX **)&mix )) || !mix) goto done;
    format = *mix;
    CoTaskMemFree( mix );
    if (format.Format.wFormatTag != WAVE_FORMAT_EXTENSIBLE || !IsEqualGUID( &format.SubFormat, &subtype_float )) goto done;
    format.Format.nSamplesPerSec = fmt->rate;
    format.Format.nAvgBytesPerSec = fmt->rate * format.Format.nBlockAlign;
    if (FAILED(IAudioClient_Initialize( client, AUDCLNT_SHAREMODE_SHARED, 0, 5000000, 0, &format.Format, NULL ))) goto done;
    if (FAILED(IAudioClient_GetBufferSize( client, &buffer ))) goto done;
    if (FAILED(IAudioClient_GetService( client, &iid_render_client, (void **)&render ))) goto done;

    /* 0.4 s of a decaying tone on each channel */
    frames = fmt->rate * 4 / 10;
    total = frames * format.Format.nChannels;
    IAudioClient_Start( client );
    while (done < total)
    {
        UINT32 padding = 0, chunk;
        BYTE *data;

        IAudioClient_GetCurrentPadding( client, &padding );
        chunk = min( buffer - padding, total - done );
        if (!chunk)
        {
            Sleep( 10 );
            continue;
        }
        if (FAILED(IAudioRenderClient_GetBuffer( render, chunk, &data ))) break;
        for (UINT32 i = 0; i < chunk; i++)
        {
            UINT32 pos = done + i, channel = pos / frames, t = pos % frames;
            float sample = 0.4f * sinf( 2.f * 3.14159265f * (channel % 2 ? 660.f : 523.f) * t / fmt->rate ) *
                           expf( -4.f * t / frames );
            float *out = (float *)data + i * format.Format.nChannels;

            for (UINT c = 0; c < format.Format.nChannels; c++) out[c] = c == channel ? sample : 0.f;
        }
        IAudioRenderClient_ReleaseBuffer( render, chunk, 0 );
        done += chunk;
    }
    Sleep( 500 );
    IAudioClient_Stop( client );
    ret = done == total;
done:
    if (render) IAudioRenderClient_Release( render );
    IAudioClient_Release( client );
    return ret;
}
