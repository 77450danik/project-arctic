/*
 * WLAN API
 *
 * Copyright 2010 Ričardas Barkauskas
 * Arctic: the interfaces, networks and connections of iwd (unix.c), with
 * profiles kept in the registry as Windows keeps them for Wlansvc.
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

#include <stdarg.h>
#include <stdlib.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "winreg.h"
#include "winternl.h"
#include "wine/debug.h"

#include "wlanapi.h"
#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(wlanapi);

#define WLAN_MAGIC 0x574c414e /* WLAN */

/* the notification sources and codes this implementation sends */
#define NOTIFICATION_SOURCE_ACM          0x00000008
#define NOTIFICATION_SOURCE_ALL          0x0000ffff
#define ACM_SCAN_COMPLETE                7
#define ACM_SCAN_FAIL                    8
#define ACM_CONNECTION_COMPLETE          10
#define ACM_CONNECTION_ATTEMPT_FAIL      11
#define ACM_DISCONNECTED                 21

/* reason codes of a failed connection */
#define WLAN_REASON_SUCCESS                   0
#define WLAN_REASON_UNKNOWN                   0x00010001
#define WLAN_REASON_NETWORK_NOT_AVAILABLE     0x0003000b
#define WLAN_REASON_KEY_MISMATCH              0x00048014  /* MSMSEC_PSK_MISMATCH_SUSPECTED */

struct connection_notification_data
{
    WLAN_CONNECTION_MODE wlanConnectionMode;
    WCHAR strProfileName[WLAN_MAX_NAME_LENGTH];
    DOT11_SSID dot11Ssid;
    DOT11_BSS_TYPE dot11BssType;
    BOOL bSecurityEnabled;
    WLAN_REASON_CODE wlanReasonCode;
    DWORD dwFlags;
    WCHAR strProfileXml[1];
};

static struct wine_wlan
{
    DWORD magic, cli_version;
    DWORD notify_source;
    WLAN_NOTIFICATION_CALLBACK callback;
    void *context;
} handle_table[16];

static CRITICAL_SECTION wlan_cs;
static CRITICAL_SECTION_DEBUG wlan_cs_debug =
{
    0, 0, &wlan_cs,
    { &wlan_cs_debug.ProcessLocksList, &wlan_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": wlan_cs") }
};
static CRITICAL_SECTION wlan_cs = { &wlan_cs_debug, -1, 0, 0, 0, 0 };

static const WCHAR profiles_keyW[] = L"Software\\Microsoft\\Wlansvc\\Profiles";

BOOL WINAPI DllMain( HINSTANCE instance, DWORD reason, void *reserved )
{
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls( instance );
    __wine_init_unix_call();
    return TRUE;
}

static struct wine_wlan* handle_index(HANDLE handle)
{
    ULONG_PTR i = (ULONG_PTR)handle - 1;

    if (i < ARRAY_SIZE(handle_table) && handle_table[i].magic == WLAN_MAGIC)
        return &handle_table[i];

    return NULL;
}

static HANDLE handle_new(struct wine_wlan **entry)
{
    ULONG_PTR i;

    for (i = 0; i < ARRAY_SIZE(handle_table); i++)
    {
        if (handle_table[i].magic == 0)
        {
            *entry = &handle_table[i];
            return (HANDLE)(i + 1);
        }
    }

    return NULL;
}

static DWORD error_from_status( NTSTATUS status )
{
    switch (status)
    {
    case STATUS_SUCCESS:         return ERROR_SUCCESS;
    case STATUS_NOT_SUPPORTED:   return ERROR_SERVICE_NOT_ACTIVE;
    case STATUS_ACCESS_DENIED:   return ERROR_ACCESS_DENIED;
    case STATUS_NOT_FOUND:       return ERROR_NOT_FOUND;
    default:                     return ERROR_GEN_FAILURE;
    }
}

/* one GUID per device, the same every time: from its Linux name */
static void interface_guid( const char *name, GUID *guid )
{
    DWORD hash = 5381;

    for (const char *p = name; *p; p++) hash = hash * 33 + (unsigned char)*p;
    memset( guid, 0, sizeof(*guid) );
    guid->Data1 = hash;
    guid->Data2 = 0x4152; /* "AR" */
    guid->Data3 = 0x4354; /* "CT" */
    memcpy( guid->Data4, "WLANIFAC", 8 );
}

static BOOL get_interfaces( struct wlan_unix_interface *interfaces, UINT32 *count )
{
    struct wlan_interfaces_params params = { .interfaces = interfaces };
    NTSTATUS status = WINE_UNIX_CALL( unix_wlan_interfaces, &params );

    *count = status ? 0 : params.count;
    return !status;
}

/* the iwd device of an interface GUID */
static BOOL find_interface( const GUID *guid, struct wlan_unix_interface *found )
{
    struct wlan_unix_interface interfaces[WLAN_MAX_INTERFACES];
    UINT32 count;

    if (!guid || !get_interfaces( interfaces, &count )) return FALSE;
    for (UINT32 i = 0; i < count; i++)
    {
        GUID id;

        interface_guid( interfaces[i].name, &id );
        if (!IsEqualGUID( &id, guid )) continue;
        *found = interfaces[i];
        return TRUE;
    }
    return FALSE;
}

static WLAN_INTERFACE_STATE interface_state( UINT32 state )
{
    switch (state)
    {
    case WLAN_UNIX_CONNECTED:     return wlan_interface_state_connected;
    case WLAN_UNIX_CONNECTING:    return wlan_interface_state_associating;
    case WLAN_UNIX_ROAMING:       return wlan_interface_state_associating;
    case WLAN_UNIX_DISCONNECTING: return wlan_interface_state_disconnecting;
    default:                      return wlan_interface_state_disconnected;
    }
}

static WLAN_SIGNAL_QUALITY signal_quality( INT32 dbm )
{
    /* -100 dBm is nothing, -50 dBm and better is full, as Windows scales it */
    if (!dbm || dbm <= -100) return 0;
    if (dbm >= -50) return 100;
    return 2 * (dbm + 100);
}

static void ssid_from_utf8( const char *text, DOT11_SSID *ssid )
{
    ssid->uSSIDLength = min( strlen( text ), DOT11_SSID_MAX_LENGTH );
    memcpy( ssid->ucSSID, text, ssid->uSSIDLength );
}

static void ssid_to_utf8( const DOT11_SSID *ssid, char *text )
{
    DWORD len = min( ssid->uSSIDLength, DOT11_SSID_MAX_LENGTH );

    memcpy( text, ssid->ucSSID, len );
    text[len] = 0;
}

/**********************************************************************
 *          Profiles: the WLANProfile XML of Windows, in the registry
 */

/* the text of the first <tag>...</tag>, entities decoded */
static BOOL xml_text( const WCHAR *xml, const WCHAR *tag, WCHAR *out, DWORD size )
{
    WCHAR open[64], close[64];
    const WCHAR *start, *end;
    DWORD len = 0;

    swprintf( open, ARRAY_SIZE(open), L"<%s>", tag );
    swprintf( close, ARRAY_SIZE(close), L"</%s>", tag );
    if (!(start = wcsstr( xml, open ))) return FALSE;
    start += wcslen( open );
    if (!(end = wcsstr( start, close ))) return FALSE;
    while (start < end && len + 1 < size)
    {
        static const struct { const WCHAR *entity; WCHAR c; } entities[] =
        {
            { L"&amp;", '&' }, { L"&lt;", '<' }, { L"&gt;", '>' }, { L"&quot;", '"' }, { L"&apos;", '\'' },
        };
        int i;

        for (i = 0; i < ARRAY_SIZE(entities); i++)
            if (!wcsncmp( start, entities[i].entity, wcslen( entities[i].entity ) )) break;
        if (i < ARRAY_SIZE(entities))
        {
            out[len++] = entities[i].c;
            start += wcslen( entities[i].entity );
        }
        else out[len++] = *start++;
    }
    out[len] = 0;
    return TRUE;
}

/* the network of a profile: <SSIDConfig><SSID><name> */
static BOOL profile_ssid( const WCHAR *xml, char *ssid )
{
    const WCHAR *config = wcsstr( xml, L"<SSIDConfig>" );
    WCHAR name[DOT11_SSID_MAX_LENGTH + 1];

    if (!config || !xml_text( config, L"name", name, ARRAY_SIZE(name) )) return FALSE;
    return WideCharToMultiByte( CP_UTF8, 0, name, -1, ssid, DOT11_SSID_MAX_LENGTH + 1, NULL, NULL ) > 0;
}

static BOOL profile_key( const WCHAR *xml, char *key, DWORD size )
{
    WCHAR material[256];

    if (!xml_text( xml, L"keyMaterial", material, ARRAY_SIZE(material) )) return FALSE;
    return WideCharToMultiByte( CP_UTF8, 0, material, -1, key, size, NULL, NULL ) > 0;
}

static WCHAR *load_profile( const WCHAR *name )
{
    DWORD size = 0;
    WCHAR *xml;
    HKEY key;

    if (RegOpenKeyExW( HKEY_LOCAL_MACHINE, profiles_keyW, 0, KEY_READ, &key )) return NULL;
    if (RegGetValueW( key, name, L"Xml", RRF_RT_REG_SZ, NULL, NULL, &size ) ||
        !(xml = malloc( size )))
    {
        RegCloseKey( key );
        return NULL;
    }
    if (RegGetValueW( key, name, L"Xml", RRF_RT_REG_SZ, NULL, xml, &size ))
    {
        free( xml );
        xml = NULL;
    }
    RegCloseKey( key );
    return xml;
}

/* the name of the profile that joins this network, if there is one */
static BOOL profile_for_ssid( const char *ssid, WCHAR *name, DWORD size )
{
    WCHAR subkey[WLAN_MAX_NAME_LENGTH];
    BOOL found = FALSE;
    HKEY key;

    if (RegOpenKeyExW( HKEY_LOCAL_MACHINE, profiles_keyW, 0, KEY_READ, &key )) return FALSE;
    for (DWORD i = 0; !found; i++)
    {
        DWORD len = ARRAY_SIZE(subkey);
        char other[DOT11_SSID_MAX_LENGTH + 1];
        WCHAR *xml;

        if (RegEnumKeyExW( key, i, subkey, &len, NULL, NULL, NULL, NULL )) break;
        if (!(xml = load_profile( subkey ))) continue;
        if (profile_ssid( xml, other ) && !strcmp( other, ssid ))
        {
            lstrcpynW( name, subkey, size );
            found = TRUE;
        }
        free( xml );
    }
    RegCloseKey( key );
    return found;
}

/**********************************************************************
 *          Notifications
 */

static void notify( const GUID *guid, DWORD code, void *data, DWORD size )
{
    struct wine_wlan copy[ARRAY_SIZE(handle_table)];

    EnterCriticalSection( &wlan_cs );
    memcpy( copy, handle_table, sizeof(copy) );
    LeaveCriticalSection( &wlan_cs );

    for (int i = 0; i < ARRAY_SIZE(copy); i++)
    {
        WLAN_NOTIFICATION_DATA note = { .NotificationSource = NOTIFICATION_SOURCE_ACM, .NotificationCode = code,
                                        .InterfaceGuid = *guid, .dwDataSize = size, .pData = data };

        if (copy[i].magic != WLAN_MAGIC || !copy[i].callback) continue;
        if (!(copy[i].notify_source & NOTIFICATION_SOURCE_ACM)) continue;
        copy[i].callback( &note, copy[i].context );
    }
}

struct scan_work
{
    GUID guid;
};

static DWORD WINAPI scan_thread( void *arg )
{
    struct scan_work *work = arg;
    struct wlan_unix_interface info;
    BOOL ok = FALSE;

    /* iwd reports the end of a scan as Scanning going false */
    Sleep( 500 );
    for (int i = 0; i < 40; i++)
    {
        if (!find_interface( &work->guid, &info )) break;
        if (!info.scanning)
        {
            ok = TRUE;
            break;
        }
        Sleep( 250 );
    }
    notify( &work->guid, ok ? ACM_SCAN_COMPLETE : ACM_SCAN_FAIL, NULL, 0 );
    free( work );
    return 0;
}

struct connect_work
{
    GUID guid;
    char path[128];
    char ssid[DOT11_SSID_MAX_LENGTH + 1];
    char key[256];
    BOOL has_key;
    WLAN_CONNECTION_MODE mode;
    WCHAR profile[WLAN_MAX_NAME_LENGTH];
};

static DWORD WINAPI connect_thread( void *arg )
{
    struct connect_work *work = arg;
    struct wlan_connect_params params = { .path = work->path, .ssid = work->ssid,
                                          .passphrase = work->has_key ? work->key : NULL };
    struct connection_notification_data data;
    NTSTATUS status = WINE_UNIX_CALL( unix_wlan_connect, &params );

    memset( &data, 0, sizeof(data) );
    data.wlanConnectionMode = work->mode;
    lstrcpynW( data.strProfileName, work->profile, ARRAY_SIZE(data.strProfileName) );
    ssid_from_utf8( work->ssid, &data.dot11Ssid );
    data.dot11BssType = dot11_BSS_type_infrastructure;
    data.bSecurityEnabled = work->has_key;
    data.wlanReasonCode = status == STATUS_SUCCESS ? WLAN_REASON_SUCCESS :
                          status == STATUS_WRONG_PASSWORD || status == STATUS_LOGON_FAILURE ? WLAN_REASON_KEY_MISMATCH :
                          status == STATUS_NOT_FOUND ? WLAN_REASON_NETWORK_NOT_AVAILABLE : WLAN_REASON_UNKNOWN;
    TRACE( "%s: %#lx\n", debugstr_a( work->ssid ), status );
    notify( &work->guid, status ? ACM_CONNECTION_ATTEMPT_FAIL : ACM_CONNECTION_COMPLETE, &data, sizeof(data) );
    SecureZeroMemory( work->key, sizeof(work->key) );
    free( work );
    return 0;
}

/**********************************************************************
 *          The API
 */

DWORD WINAPI WlanEnumInterfaces(HANDLE handle, void *reserved, WLAN_INTERFACE_INFO_LIST **interface_list)
{
    struct wlan_unix_interface interfaces[WLAN_MAX_INTERFACES];
    WLAN_INTERFACE_INFO_LIST *ret_list;
    UINT32 count;

    TRACE("(%p, %p, %p)\n", handle, reserved, interface_list);

    if (!handle || reserved || !interface_list)
        return ERROR_INVALID_PARAMETER;
    if (!handle_index(handle))
        return ERROR_INVALID_HANDLE;

    get_interfaces( interfaces, &count );
    ret_list = WlanAllocateMemory( offsetof(WLAN_INTERFACE_INFO_LIST, InterfaceInfo[max( count, 1 )]) );
    if (!ret_list)
        return ERROR_NOT_ENOUGH_MEMORY;

    memset( ret_list, 0, offsetof(WLAN_INTERFACE_INFO_LIST, InterfaceInfo[max( count, 1 )]) );
    for (UINT32 i = 0; i < count; i++)
    {
        WLAN_INTERFACE_INFO *info = &ret_list->InterfaceInfo[i];

        interface_guid( interfaces[i].name, &info->InterfaceGuid );
        MultiByteToWideChar( CP_UTF8, 0, interfaces[i].description, -1, info->strInterfaceDescription,
                             ARRAY_SIZE(info->strInterfaceDescription) );
        info->isState = interface_state( interfaces[i].state );
    }
    ret_list->dwNumberOfItems = count;
    *interface_list = ret_list;
    return ERROR_SUCCESS;
}

DWORD WINAPI WlanCloseHandle(HANDLE handle, void *reserved)
{
    struct wine_wlan *wlan;

    TRACE("(%p, %p)\n", handle, reserved);

    if (!handle || reserved)
        return ERROR_INVALID_PARAMETER;

    EnterCriticalSection( &wlan_cs );
    if (!(wlan = handle_index(handle)))
    {
        LeaveCriticalSection( &wlan_cs );
        return ERROR_INVALID_HANDLE;
    }
    memset( wlan, 0, sizeof(*wlan) );
    LeaveCriticalSection( &wlan_cs );
    return ERROR_SUCCESS;
}

DWORD WINAPI WlanOpenHandle(DWORD client_version, void *reserved, DWORD *negotiated_version, HANDLE *handle)
{
    struct wine_wlan *wlan;
    HANDLE ret_handle;

    TRACE("(%lu, %p, %p, %p)\n", client_version, reserved, negotiated_version, handle);

    if (reserved || !negotiated_version || !handle)
        return ERROR_INVALID_PARAMETER;

    if (client_version != 1 && client_version != 2)
        return ERROR_NOT_SUPPORTED;

    EnterCriticalSection( &wlan_cs );
    ret_handle = handle_new(&wlan);
    if (!ret_handle)
    {
        LeaveCriticalSection( &wlan_cs );
        return ERROR_REMOTE_SESSION_LIMIT_EXCEEDED;
    }

    memset( wlan, 0, sizeof(*wlan) );
    wlan->magic = WLAN_MAGIC;
    wlan->cli_version = *negotiated_version = client_version;
    LeaveCriticalSection( &wlan_cs );
    *handle = ret_handle;

    return ERROR_SUCCESS;
}

DWORD WINAPI WlanScan(HANDLE handle, const GUID *guid, const DOT11_SSID *ssid,
                      const WLAN_RAW_DATA *raw, void *reserved)
{
    struct wlan_unix_interface info;
    struct wlan_device_params params;
    struct scan_work *work;
    NTSTATUS status;
    HANDLE thread;

    TRACE("(%p, %s, %p, %p, %p)\n", handle, wine_dbgstr_guid(guid), ssid, raw, reserved);

    if (!handle || !guid || reserved) return ERROR_INVALID_PARAMETER;
    if (!handle_index(handle)) return ERROR_INVALID_HANDLE;
    if (!find_interface( guid, &info )) return ERROR_NOT_FOUND;

    params.path = info.path;
    if ((status = WINE_UNIX_CALL( unix_wlan_scan, &params ))) return error_from_status( status );
    if ((work = malloc( sizeof(*work) )))
    {
        work->guid = *guid;
        if ((thread = CreateThread( NULL, 0, scan_thread, work, 0, NULL ))) CloseHandle( thread );
        else free( work );
    }
    return ERROR_SUCCESS;
}

DWORD WINAPI WlanRegisterNotification(HANDLE handle, DWORD notify_source, BOOL ignore_dup,
                                      WLAN_NOTIFICATION_CALLBACK callback, void *context,
                                      void *reserved, DWORD *notify_prev)
{
    struct wine_wlan *wlan;

    TRACE("(%p, %lx, %d, %p, %p, %p, %p)\n",
          handle, notify_source, ignore_dup, callback, context, reserved, notify_prev);

    if (!handle || reserved) return ERROR_INVALID_PARAMETER;
    EnterCriticalSection( &wlan_cs );
    if (!(wlan = handle_index(handle)))
    {
        LeaveCriticalSection( &wlan_cs );
        return ERROR_INVALID_HANDLE;
    }
    if (notify_prev) *notify_prev = wlan->notify_source;
    wlan->notify_source = callback ? notify_source : 0;
    wlan->callback = notify_source ? callback : NULL;
    wlan->context = context;
    LeaveCriticalSection( &wlan_cs );
    return ERROR_SUCCESS;
}

DWORD WINAPI WlanGetAvailableNetworkList(HANDLE handle, const GUID *guid, DWORD flags,
                                         void *reserved, WLAN_AVAILABLE_NETWORK_LIST **network_list)
{
    struct wlan_unix_network networks[WLAN_MAX_NETWORKS];
    struct wlan_networks_params params = { .networks = networks };
    struct wlan_unix_interface info;
    WLAN_AVAILABLE_NETWORK_LIST *list;
    NTSTATUS status;
    DWORD size;

    TRACE("(%p, %s, 0x%lx, %p, %p)\n", handle, wine_dbgstr_guid(guid), flags, reserved, network_list);

    if (!handle || !guid || reserved || !network_list) return ERROR_INVALID_PARAMETER;
    if (!handle_index(handle)) return ERROR_INVALID_HANDLE;
    if (!find_interface( guid, &info )) return ERROR_NOT_FOUND;

    params.path = info.path;
    if ((status = WINE_UNIX_CALL( unix_wlan_networks, &params ))) return error_from_status( status );

    size = offsetof(WLAN_AVAILABLE_NETWORK_LIST, Network[max( params.count, 1 )]);
    if (!(list = WlanAllocateMemory( size ))) return ERROR_NOT_ENOUGH_MEMORY;
    memset( list, 0, size );
    for (UINT32 i = 0; i < params.count; i++)
    {
        WLAN_AVAILABLE_NETWORK *net = &list->Network[i];
        BOOL secured = networks[i].security != WLAN_UNIX_OPEN;
        BOOL has_profile = profile_for_ssid( networks[i].ssid, net->strProfileName,
                                             ARRAY_SIZE(net->strProfileName) );

        ssid_from_utf8( networks[i].ssid, &net->dot11Ssid );
        net->dot11BssType = dot11_BSS_type_infrastructure;
        net->uNumberOfBssids = 1;
        net->bNetworkConnectable = networks[i].security != WLAN_UNIX_8021X;
        net->uNumberOfPhyTypes = 1;
        net->dot11PhyTypes[0] = dot11_phy_type_any;
        net->wlanSignalQuality = signal_quality( networks[i].signal );
        net->bSecurityEnabled = secured;
        net->dot11DefaultAuthAlgorithm = networks[i].security == WLAN_UNIX_PSK ? DOT11_AUTH_ALGO_RSNA_PSK :
                                         networks[i].security == WLAN_UNIX_8021X ? DOT11_AUTH_ALGO_RSNA :
                                         networks[i].security == WLAN_UNIX_WEP ? DOT11_AUTH_ALGO_80211_SHARED_KEY :
                                         DOT11_AUTH_ALGO_80211_OPEN;
        net->dot11DefaultCipherAlgorithm = networks[i].security == WLAN_UNIX_WEP ? DOT11_CIPHER_ALGO_WEP :
                                           secured ? DOT11_CIPHER_ALGO_CCMP : DOT11_CIPHER_ALGO_NONE;
        if (networks[i].connected) net->dwFlags |= WLAN_AVAILABLE_NETWORK_CONNECTED;
        if (has_profile || networks[i].known) net->dwFlags |= WLAN_AVAILABLE_NETWORK_HAS_PROFILE;
    }
    list->dwNumberOfItems = params.count;
    *network_list = list;
    return ERROR_SUCCESS;
}

DWORD WINAPI WlanConnect(HANDLE handle, const GUID *guid, const WLAN_CONNECTION_PARAMETERS *parameters,
                         void *reserved)
{
    struct wlan_unix_interface info;
    struct connect_work *work;
    WCHAR *xml = NULL;
    HANDLE thread;

    TRACE("(%p, %s, %p, %p)\n", handle, wine_dbgstr_guid(guid), parameters, reserved);

    if (!handle || !guid || !parameters || reserved) return ERROR_INVALID_PARAMETER;
    if (!handle_index(handle)) return ERROR_INVALID_HANDLE;
    if (!find_interface( guid, &info )) return ERROR_NOT_FOUND;
    if (!(work = calloc( 1, sizeof(*work) ))) return ERROR_NOT_ENOUGH_MEMORY;

    work->guid = *guid;
    work->mode = parameters->wlanConnectionMode;
    lstrcpynA( work->path, info.path, sizeof(work->path) );
    switch (parameters->wlanConnectionMode)
    {
    case wlan_connection_mode_profile:
        if (!parameters->strProfile || !(xml = load_profile( parameters->strProfile ))) goto invalid;
        lstrcpynW( work->profile, parameters->strProfile, ARRAY_SIZE(work->profile) );
        break;
    case wlan_connection_mode_temporary_profile:
        if (!parameters->strProfile || !(xml = wcsdup( parameters->strProfile ))) goto invalid;
        break;
    case wlan_connection_mode_discovery_unsecure:
    case wlan_connection_mode_discovery_secure:
        if (!parameters->pDot11Ssid) goto invalid;
        ssid_to_utf8( parameters->pDot11Ssid, work->ssid );
        break;
    default:
        goto invalid;
    }
    if (xml)
    {
        if (!profile_ssid( xml, work->ssid )) goto invalid;
        work->has_key = profile_key( xml, work->key, sizeof(work->key) );
        free( xml );
        xml = NULL;
    }
    if (parameters->pDot11Ssid) ssid_to_utf8( parameters->pDot11Ssid, work->ssid );

    if ((thread = CreateThread( NULL, 0, connect_thread, work, 0, NULL )))
    {
        CloseHandle( thread );
        return ERROR_SUCCESS;
    }
    free( work );
    return ERROR_NOT_ENOUGH_MEMORY;

invalid:
    free( xml );
    free( work );
    return ERROR_INVALID_PARAMETER;
}

DWORD WINAPI WlanDisconnect(HANDLE handle, const GUID *guid, void *reserved)
{
    struct wlan_unix_interface info;
    struct wlan_device_params params;
    NTSTATUS status;

    TRACE("(%p, %s, %p)\n", handle, wine_dbgstr_guid(guid), reserved);

    if (!handle || !guid || reserved) return ERROR_INVALID_PARAMETER;
    if (!handle_index(handle)) return ERROR_INVALID_HANDLE;
    if (!find_interface( guid, &info )) return ERROR_NOT_FOUND;
    params.path = info.path;
    if ((status = WINE_UNIX_CALL( unix_wlan_disconnect, &params ))) return error_from_status( status );
    notify( guid, ACM_DISCONNECTED, NULL, 0 );
    return ERROR_SUCCESS;
}

DWORD WINAPI WlanQueryInterface(HANDLE handle, const GUID *guid, WLAN_INTF_OPCODE opcode,
                    void *reserved, DWORD *data_size, void **data, WLAN_OPCODE_VALUE_TYPE *opcode_type)
{
    struct wlan_unix_interface info;

    TRACE("(%p, %s, 0x%x, %p, %p, %p, %p)\n",
          handle, wine_dbgstr_guid(guid), opcode, reserved, data_size, data, opcode_type);

    if (!handle || !guid || reserved || !data_size || !data) return ERROR_INVALID_PARAMETER;
    if (!handle_index(handle)) return ERROR_INVALID_HANDLE;
    if (!find_interface( guid, &info )) return ERROR_NOT_FOUND;
    if (opcode_type) *opcode_type = wlan_opcode_value_type_query_only;

    switch (opcode)
    {
    case wlan_intf_opcode_interface_state:
    {
        WLAN_INTERFACE_STATE *state = WlanAllocateMemory( sizeof(*state) );

        if (!state) return ERROR_NOT_ENOUGH_MEMORY;
        *state = interface_state( info.state );
        *data = state;
        *data_size = sizeof(*state);
        return ERROR_SUCCESS;
    }
    case wlan_intf_opcode_current_connection:
    {
        struct wlan_unix_network networks[WLAN_MAX_NETWORKS];
        struct wlan_networks_params params = { .path = info.path, .networks = networks };
        WLAN_CONNECTION_ATTRIBUTES *attr;

        if (info.state == WLAN_UNIX_DISCONNECTED || !info.ssid[0]) return ERROR_INVALID_STATE;
        if (!(attr = WlanAllocateMemory( sizeof(*attr) ))) return ERROR_NOT_ENOUGH_MEMORY;
        memset( attr, 0, sizeof(*attr) );
        attr->isState = interface_state( info.state );
        attr->wlanConnectionMode = wlan_connection_mode_profile;
        if (!profile_for_ssid( info.ssid, attr->strProfileName, ARRAY_SIZE(attr->strProfileName) ))
            MultiByteToWideChar( CP_UTF8, 0, info.ssid, -1, attr->strProfileName, ARRAY_SIZE(attr->strProfileName) );
        ssid_from_utf8( info.ssid, &attr->wlanAssociationAttributes.dot11Ssid );
        attr->wlanAssociationAttributes.dot11BssType = dot11_BSS_type_infrastructure;
        attr->wlanAssociationAttributes.dot11PhyType = dot11_phy_type_any;
        attr->wlanAssociationAttributes.wlanSignalQuality = signal_quality( info.signal );
        if (!WINE_UNIX_CALL( unix_wlan_networks, &params ))
        {
            for (UINT32 i = 0; i < params.count; i++)
            {
                if (strcmp( networks[i].ssid, info.ssid )) continue;
                attr->wlanSecurityAttributes.bSecurityEnabled = networks[i].security != WLAN_UNIX_OPEN;
                attr->wlanSecurityAttributes.bOneXEnabled = networks[i].security == WLAN_UNIX_8021X;
                attr->wlanSecurityAttributes.dot11AuthAlgorithm =
                    networks[i].security == WLAN_UNIX_OPEN ? DOT11_AUTH_ALGO_80211_OPEN : DOT11_AUTH_ALGO_RSNA_PSK;
                attr->wlanSecurityAttributes.dot11CipherAlgorithm =
                    networks[i].security == WLAN_UNIX_OPEN ? DOT11_CIPHER_ALGO_NONE : DOT11_CIPHER_ALGO_CCMP;
            }
        }
        *data = attr;
        *data_size = sizeof(*attr);
        return ERROR_SUCCESS;
    }
    default:
        FIXME( "opcode %#x\n", opcode );
        return ERROR_NOT_SUPPORTED;
    }
}

DWORD WINAPI WlanSetProfile(HANDLE handle, const GUID *guid, DWORD flags, const WCHAR *xml,
                            const WCHAR *security, BOOL overwrite, void *reserved, DWORD *reason)
{
    WCHAR name[WLAN_MAX_NAME_LENGTH];
    char ssid[DOT11_SSID_MAX_LENGTH + 1];
    HKEY key, profile;
    DWORD disposition;
    LSTATUS ret;

    TRACE("(%p, %s, %#lx, %p, %p, %d, %p, %p)\n", handle, wine_dbgstr_guid(guid), flags, xml, security,
          overwrite, reserved, reason);

    if (!handle || !guid || !xml || reserved || !reason) return ERROR_INVALID_PARAMETER;
    if (!handle_index(handle)) return ERROR_INVALID_HANDLE;
    *reason = WLAN_REASON_SUCCESS;
    if (!xml_text( xml, L"name", name, ARRAY_SIZE(name) ) || !name[0] || !profile_ssid( xml, ssid ))
    {
        *reason = WLAN_REASON_UNKNOWN;
        return ERROR_BAD_PROFILE;
    }
    if ((ret = RegCreateKeyExW( HKEY_LOCAL_MACHINE, profiles_keyW, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL )))
        return ret;
    ret = RegCreateKeyExW( key, name, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &profile, &disposition );
    RegCloseKey( key );
    if (ret) return ret;
    if (disposition == REG_OPENED_EXISTING_KEY && !overwrite)
    {
        RegCloseKey( profile );
        return ERROR_ALREADY_EXISTS;
    }
    ret = RegSetValueExW( profile, L"Xml", 0, REG_SZ, (const BYTE *)xml, (wcslen( xml ) + 1) * sizeof(WCHAR) );
    RegCloseKey( profile );
    return ret;
}

DWORD WINAPI WlanGetProfile(HANDLE handle, const GUID *guid, const WCHAR *name, void *reserved,
                            WCHAR **xml, DWORD *flags, DWORD *access)
{
    WCHAR *stored, *copy;
    DWORD size;

    TRACE("(%p, %s, %s, %p, %p, %p, %p)\n", handle, wine_dbgstr_guid(guid), debugstr_w(name), reserved,
          xml, flags, access);

    if (!handle || !guid || !name || reserved || !xml) return ERROR_INVALID_PARAMETER;
    if (!handle_index(handle)) return ERROR_INVALID_HANDLE;
    if (!(stored = load_profile( name ))) return ERROR_NOT_FOUND;
    size = (wcslen( stored ) + 1) * sizeof(WCHAR);
    if (!(copy = WlanAllocateMemory( size )))
    {
        free( stored );
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    memcpy( copy, stored, size );
    free( stored );
    *xml = copy;
    if (flags) *flags = WLAN_PROFILE_USER;
    if (access) *access = 0x00020000 | 0x00000001 | 0x00000002; /* read, execute, write */
    return ERROR_SUCCESS;
}

DWORD WINAPI WlanGetProfileList(HANDLE handle, const GUID *guid, void *reserved, WLAN_PROFILE_INFO_LIST **list)
{
    WLAN_PROFILE_INFO_LIST *ret;
    DWORD count = 0, size;
    HKEY key = NULL;

    TRACE("(%p, %s, %p, %p)\n", handle, wine_dbgstr_guid(guid), reserved, list);

    if (!handle || !guid || reserved || !list) return ERROR_INVALID_PARAMETER;
    if (!handle_index(handle)) return ERROR_INVALID_HANDLE;
    if (!RegOpenKeyExW( HKEY_LOCAL_MACHINE, profiles_keyW, 0, KEY_READ, &key ))
        RegQueryInfoKeyW( key, NULL, NULL, NULL, &count, NULL, NULL, NULL, NULL, NULL, NULL, NULL );
    size = offsetof(WLAN_PROFILE_INFO_LIST, ProfileInfo[max( count, 1 )]);
    if (!(ret = WlanAllocateMemory( size )))
    {
        if (key) RegCloseKey( key );
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    memset( ret, 0, size );
    for (DWORD i = 0; key && i < count; i++)
    {
        DWORD len = ARRAY_SIZE(ret->ProfileInfo[i].strProfileName);

        if (RegEnumKeyExW( key, i, ret->ProfileInfo[i].strProfileName, &len, NULL, NULL, NULL, NULL )) break;
        ret->ProfileInfo[i].dwFlags = WLAN_PROFILE_USER;
        ret->dwNumberOfItems++;
    }
    if (key) RegCloseKey( key );
    *list = ret;
    return ERROR_SUCCESS;
}

DWORD WINAPI WlanDeleteProfile(HANDLE handle, const GUID *guid, const WCHAR *name, void *reserved)
{
    struct wlan_forget_params params;
    char ssid[DOT11_SSID_MAX_LENGTH + 1];
    WCHAR *xml;
    HKEY key;
    LSTATUS ret;

    TRACE("(%p, %s, %s, %p)\n", handle, wine_dbgstr_guid(guid), debugstr_w(name), reserved);

    if (!handle || !guid || !name || reserved) return ERROR_INVALID_PARAMETER;
    if (!handle_index(handle)) return ERROR_INVALID_HANDLE;
    if (!(xml = load_profile( name ))) return ERROR_NOT_FOUND;
    /* iwd forgets the key too */
    if (profile_ssid( xml, ssid ))
    {
        params.ssid = ssid;
        WINE_UNIX_CALL( unix_wlan_forget, &params );
    }
    free( xml );
    if ((ret = RegOpenKeyExW( HKEY_LOCAL_MACHINE, profiles_keyW, 0, KEY_ALL_ACCESS, &key ))) return ret;
    ret = RegDeleteTreeW( key, name );
    RegCloseKey( key );
    return ret;
}

DWORD WINAPI WlanHostedNetworkQueryProperty(HANDLE handle, WLAN_HOSTED_NETWORK_OPCODE opcode,
                                            DWORD *data_size, void **data,
                                            WLAN_OPCODE_VALUE_TYPE *opcode_type, void *reserved)
{
    FIXME("(%p, 0x%x, %p, %p, %p, %p) stub\n",
          handle, opcode, data_size, data, opcode_type, reserved);

    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD WINAPI WlanHostedNetworkQuerySecondaryKey(HANDLE handle, DWORD *key_size, unsigned char *key,
                                                BOOL *passphrase, BOOL *persistent,
                                                WLAN_HOSTED_NETWORK_REASON *error, void *reserved)
{
    FIXME("(%p, %p, %p, %p, %p, %p, %p) stub\n",
          handle, key_size, key, passphrase, persistent, error, reserved);

    return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD WINAPI WlanHostedNetworkQueryStatus(HANDLE handle, WLAN_HOSTED_NETWORK_STATUS *status, void *reserved)
{
    FIXME("(%p, %p, %p) stub\n", handle, status, reserved);

    return ERROR_CALL_NOT_IMPLEMENTED;
}

void WINAPI WlanFreeMemory(void *ptr)
{
    TRACE("(%p)\n", ptr);

    HeapFree(GetProcessHeap(), 0, ptr);
}

void *WINAPI WlanAllocateMemory(DWORD size)
{
    void *ret;

    TRACE("(%ld)\n", size);

    if (!size)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }

    ret = HeapAlloc(GetProcessHeap(), 0, size);
    if (!ret)
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);

    return ret;
}
