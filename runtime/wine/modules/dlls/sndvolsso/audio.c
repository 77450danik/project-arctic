/*
 * Volume icon of the notification area: the outputs and their volume
 *
 * What the icon and the flyout show comes from the Core Audio API, as in
 * Windows: the default render endpoint, its IAudioEndpointVolume (the whole
 * system's volume, mmdevapi), and the active endpoints to choose from.
 * Choosing one makes it the default the way Wine keeps it, in the driver's
 * key (DefaultOutput); every program that follows the default hears of it
 * through IMMNotificationClient. All of it runs on the icon's thread.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS

#include "sndvolsso.h"
#include "winreg.h"
#include "objbase.h"
#include "mmdeviceapi.h"
#include "endpointvolume.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(sndvolsso);

/* Core Audio's GUIDs, which libuuid does not carry */
static const CLSID clsid_enumerator =           /* CLSID_MMDeviceEnumerator */
    { 0xbcde0395, 0xe52f, 0x467c, { 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e } };
static const IID iid_enumerator =               /* IID_IMMDeviceEnumerator */
    { 0xa95664d2, 0x9614, 0x4f35, { 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6 } };
static const IID iid_endpoint_volume =          /* IID_IAudioEndpointVolume */
    { 0x5cdf2c82, 0x841e, 0x4546, { 0x97, 0x22, 0x0c, 0xf7, 0x40, 0x78, 0x22, 0x9a } };
static const PROPERTYKEY key_friendly_name =   /* PKEY_Device_FriendlyName */
    { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };
static const PROPERTYKEY key_driver =          /* DEVPKEY_Device_Driver, which mmdevapi sets to the driver */
    { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 11 };

/* the changes this icon makes, as IAudioEndpointVolumeCallback sees them:
 * {5C3E8B8A-6F1D-4E2A-9B47-0D8E2C61F3A5} */
static const GUID volume_context =
    { 0x5c3e8b8a, 0x6f1d, 0x4e2a, { 0x9b, 0x47, 0x0d, 0x8e, 0x2c, 0x61, 0xf3, 0xa5 } };

static IMMDeviceEnumerator *enumerator;
static IAudioEndpointVolume *volume;
static WCHAR volume_id[128];

BOOL audio_init(void)
{
    if (enumerator) return TRUE;
    return SUCCEEDED(CoCreateInstance( &clsid_enumerator, NULL, CLSCTX_INPROC_SERVER,
                                       &iid_enumerator, (void **)&enumerator ));
}

void audio_shutdown(void)
{
    if (volume) IAudioEndpointVolume_Release( volume );
    if (enumerator) IMMDeviceEnumerator_Release( enumerator );
    volume = NULL;
    enumerator = NULL;
    volume_id[0] = 0;
}

static BOOL get_property( IMMDevice *device, const PROPERTYKEY *key, WCHAR *buf, int size )
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

/* "Speakers (Realtek ALC892)" in the user's language: the driver names
 * what an output is in English, as Wine's drivers do */
static void display_name( const WCHAR *friendly, WCHAR *out, int size )
{
    static const struct { const WCHAR *english; UINT id; } kinds[] =
    {
        { L"Speakers", IDS_SPEAKERS },
        { L"Headphones", IDS_HEADPHONES },
        { L"Digital Output", IDS_DIGITAL_OUTPUT },
        { L"Digital Display", IDS_DIGITAL_DISPLAY },
    };

    for (UINT i = 0; i < ARRAY_SIZE(kinds); i++)
    {
        int len = lstrlenW( kinds[i].english );

        if (wcsncmp( friendly, kinds[i].english, len ) || (friendly[len] && friendly[len] != ' ')) continue;
        swprintf( out, size, L"%s%s", load_string( kinds[i].id ), friendly + len );
        return;
    }
    lstrcpynW( out, friendly, size );
}

static void describe( IMMDevice *device, struct endpoint *ep )
{
    WCHAR *id = NULL, friendly[256];

    ep->id[0] = 0;
    if (SUCCEEDED(IMMDevice_GetId( device, &id )) && id)
    {
        lstrcpynW( ep->id, id, ARRAY_SIZE(ep->id) );
        CoTaskMemFree( id );
    }
    if (!get_property( device, &key_friendly_name, friendly, ARRAY_SIZE(friendly) ))
        lstrcpyW( friendly, load_string( IDS_SPEAKERS ) );
    display_name( friendly, ep->name, ARRAY_SIZE(ep->name) );
}

/* the default output, its volume (0-1) and mute; FALSE: there is no output */
BOOL audio_state( struct endpoint *def, float *level, BOOL *mute )
{
    IMMDevice *device;
    BOOL muted = FALSE;

    *level = 0;
    *mute = FALSE;
    def->id[0] = def->name[0] = 0;
    if (!enumerator && !audio_init()) return FALSE;
    if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint( enumerator, eRender, eMultimedia, &device )))
    {
        if (volume) IAudioEndpointVolume_Release( volume );
        volume = NULL;
        volume_id[0] = 0;
        return FALSE;
    }
    describe( device, def );
    if (!volume || wcscmp( volume_id, def->id ))
    {
        if (volume) IAudioEndpointVolume_Release( volume );
        volume = NULL;
        if (SUCCEEDED(IMMDevice_Activate( device, &iid_endpoint_volume, CLSCTX_INPROC_SERVER, NULL,
                                          (void **)&volume )))
            lstrcpyW( volume_id, def->id );
    }
    IMMDevice_Release( device );
    if (!volume) return FALSE;
    IAudioEndpointVolume_GetMasterVolumeLevelScalar( volume, level );
    IAudioEndpointVolume_GetMute( volume, &muted );
    *mute = muted;
    return TRUE;
}

/* the outputs one can choose from */
UINT audio_endpoints( struct endpoint *list, UINT max )
{
    IMMDeviceCollection *collection;
    UINT count = 0, n = 0;

    if (!enumerator && !audio_init()) return 0;
    if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints( enumerator, eRender, DEVICE_STATE_ACTIVE, &collection )))
        return 0;
    IMMDeviceCollection_GetCount( collection, &count );
    for (UINT i = 0; i < count && n < max; i++)
    {
        IMMDevice *device;

        if (FAILED(IMMDeviceCollection_Item( collection, i, &device ))) continue;
        describe( device, &list[n] );
        if (list[n].id[0]) n++;
        IMMDevice_Release( device );
    }
    IMMDeviceCollection_Release( collection );
    return n;
}

void audio_set_level( float level )
{
    if (volume) IAudioEndpointVolume_SetMasterVolumeLevelScalar( volume, max( 0.f, min( 1.f, level ) ),
                                                                 &volume_context );
}

void audio_set_mute( BOOL mute )
{
    if (volume) IAudioEndpointVolume_SetMute( volume, mute, &volume_context );
}

/* the output for everything from now on, as the console, multimedia and
 * communications default at once, as the flyout of Windows 10 sets it */
void audio_set_default( const WCHAR *id )
{
    WCHAR driver[64], key_name[128];
    IMMDevice *device;
    HKEY key;

    if (!enumerator || FAILED(IMMDeviceEnumerator_GetDevice( enumerator, id, &device ))) return;
    if (!get_property( device, &key_driver, driver, ARRAY_SIZE(driver) )) lstrcpyW( driver, L"winealsa.drv" );
    IMMDevice_Release( device );

    swprintf( key_name, ARRAY_SIZE(key_name), L"Software\\Wine\\Drivers\\%s", driver );
    if (RegCreateKeyExW( HKEY_CURRENT_USER, key_name, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL )) return;
    RegSetValueExW( key, L"DefaultOutput", 0, REG_SZ, (const BYTE *)id, (lstrlenW( id ) + 1) * sizeof(WCHAR) );
    RegSetValueExW( key, L"DefaultVoiceOutput", 0, REG_SZ, (const BYTE *)id, (lstrlenW( id ) + 1) * sizeof(WCHAR) );
    RegCloseKey( key );
    TRACE( "default output %s\n", debugstr_w( id ) );
}
