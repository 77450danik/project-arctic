/*
 * Arctic desktop composition engine
 *
 * dwm.exe hosts this library the way Windows' dwm.exe hosts dwmcore.dll.
 * The unix side owns the display (DRM/KMS) and serves window buffers; this
 * side reads Windows settings, decides what each monitor shows, asks
 * wineserver where the windows are and lends the unix side the thread in
 * between.
 *
 * user32 is deliberately not used: its first window-related call would
 * start the desktop and the display driver before the compositor serves.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winreg.h"
#include "winternl.h"
#include "wine/server.h"
#include "wine/debug.h"

#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(dwm);

BOOL WINAPI DllMain( HINSTANCE instance, DWORD reason, void *reserved )
{
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls( instance );
    return !__wine_init_unix_call();
}

/* HKCU\Control Panel\Colors\Background holds "R G B", as in Windows */
static COLORREF desktop_color(void)
{
    WCHAR value[32];
    DWORD size = sizeof(value);
    int r, g, b;

    if (!RegGetValueW( HKEY_CURRENT_USER, L"Control Panel\\Colors", L"Background", RRF_RT_REG_SZ,
                       NULL, value, &size ) &&
        swscanf( value, L"%d %d %d", &r, &g, &b ) == 3)
        return RGB( r, g, b );
    return RGB( 58, 110, 165 );
}


/**********************************************************************
 *          The monitors
 *
 * What each monitor shows is decided here, as the display kernel of
 * Windows decides it: the layout kept for this set of monitors when
 * there is one, else every monitor at its own resolution and the
 * highest refresh rate it takes there.
 */

static const WCHAR configuration_keyW[] =
    L"System\\CurrentControlSet\\Control\\GraphicsDrivers\\Configuration";

static struct dwm_output *outputs;
static UINT               output_count;

/**********************************************************************
 *          Variable refresh
 *
 * On for a monitor that takes it, as the drivers of Windows have FreeSync
 * and G-SYNC on, unless the user turned it off for that monitor in the
 * display settings of the Control Panel (VariableRefresh=0 in its key of
 * the monitor data store) or for all of them in the DirectX settings
 * (VRROptimizeEnable=0). Whether a monitor takes it is kept in the same
 * key, so that the Control Panel offers the switch only where it works.
 */

static const WCHAR monitor_store_keyW[] =
    L"System\\CurrentControlSet\\Control\\GraphicsDrivers\\MonitorDataStore";
static HKEY   gpu_preferences, monitor_store;
static HANDLE options_changed;

/* "ACR0A2B", the Plug and Play id of the monitor from its EDID, as win32u
 * names the monitor device */
static BOOL monitor_pnp_id( const struct dwm_output *output, WCHAR *id, SIZE_T size )
{
    UINT manufacturer;

    if (output->edid_len < 128) return FALSE;
    manufacturer = (output->edid[8] << 8) | output->edid[9];
    swprintf( id, size, L"%c%c%c%04X", 'A' + ((manufacturer >> 10) & 0x1f) - 1,
              'A' + ((manufacturer >> 5) & 0x1f) - 1, 'A' + (manufacturer & 0x1f) - 1,
              output->edid[10] | (output->edid[11] << 8) );
    return TRUE;
}

static void publish_vrr_capability(void)
{
    WCHAR id[16];
    HKEY hkey;

    if (!monitor_store) return;
    for (UINT i = 0; i < output_count; i++)
    {
        DWORD capable = outputs[i].vrr_capable;

        if (!monitor_pnp_id( &outputs[i], id, ARRAY_SIZE(id) )) continue;
        if (RegCreateKeyExW( monitor_store, id, 0, NULL, 0, KEY_SET_VALUE, NULL, &hkey, NULL )) continue;
        RegSetValueExW( hkey, L"VariableRefreshCapable", 0, REG_DWORD, (BYTE *)&capable, sizeof(capable) );
        RegCloseKey( hkey );
    }
}

static void update_options(void)
{
    struct dwm_set_options_params params = { .vrr = TRUE };
    WCHAR value[512], id[16];
    DWORD size = sizeof(value) - sizeof(WCHAR), type;

    if (gpu_preferences &&
        !RegQueryValueExW( gpu_preferences, L"DirectXUserGlobalSettings", NULL, &type, (BYTE *)value, &size ) &&
        type == REG_SZ)
    {
        value[size / sizeof(WCHAR)] = 0;
        if (wcsstr( value, L"VRROptimizeEnable=0" )) params.vrr = FALSE;
    }
    for (UINT i = 0; i < output_count && params.vrr_off_count < ARRAY_SIZE(params.vrr_off); i++)
    {
        DWORD on = 1;

        size = sizeof(on);
        if (!monitor_store || !monitor_pnp_id( &outputs[i], id, ARRAY_SIZE(id) )) continue;
        if (!RegGetValueW( monitor_store, id, L"VariableRefresh", RRF_RT_REG_DWORD, NULL, &on, &size ) && !on)
            params.vrr_off[params.vrr_off_count++] = outputs[i].id;
    }
    WINE_UNIX_CALL( unix_dwm_set_options, &params );
    if (gpu_preferences)
        RegNotifyChangeKeyValue( gpu_preferences, FALSE, REG_NOTIFY_CHANGE_LAST_SET, options_changed, TRUE );
    if (monitor_store)
        RegNotifyChangeKeyValue( monitor_store, TRUE, REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_CHANGE_NAME,
                                 options_changed, TRUE );
}

static BOOL read_outputs(void)
{
    struct dwm_get_outputs_params params = { .outputs = outputs };

    if (!outputs && !(outputs = params.outputs = calloc( DWM_MAX_OUTPUTS, sizeof(*outputs) ))) return FALSE;
    if (WINE_UNIX_CALL( unix_dwm_get_outputs, &params )) return FALSE;
    output_count = params.count;
    return output_count > 0;
}

/* "DEL40F3_4C3A5A30", the manufacturer, model and serial of the EDID, as
 * Windows names a monitor in its display database */
static void monitor_id( const struct dwm_output *output, WCHAR *id, SIZE_T size )
{
    const BYTE *edid = output->edid;
    UINT manufacturer, product, serial;

    if (output->edid_len < 128)
    {
        swprintf( id, size, L"%hs", output->name );
        return;
    }
    manufacturer = (edid[8] << 8) | edid[9];
    product = edid[10] | (edid[11] << 8);
    serial = edid[12] | (edid[13] << 8) | (edid[14] << 16) | ((UINT)edid[15] << 24);
    /* two monitors of the same model can carry the same serial number, so
     * the connector they hang on tells them apart */
    swprintf( id, size, L"%c%c%c%04X_%08X#%hs", 'A' + ((manufacturer >> 10) & 0x1f) - 1,
              'A' + ((manufacturer >> 5) & 0x1f) - 1, 'A' + (manufacturer & 0x1f) - 1, product, serial,
              output->name );
}

/* one key per set of monitors, so that a laptop remembers each desk */
static void configuration_path( WCHAR *path, SIZE_T size )
{
    WCHAR id[64];
    int len;

    len = swprintf( path, size, L"%s\\", configuration_keyW );
    for (UINT i = 0; i < output_count && len > 0; i++)
    {
        monitor_id( &outputs[i], id, ARRAY_SIZE(id) );
        len += swprintf( path + len, size - len, i ? L"+%s" : L"%s", id );
    }
}

static HKEY open_configuration( BOOL create )
{
    WCHAR path[512];
    HKEY hkey;

    configuration_path( path, ARRAY_SIZE(path) );
    if (create)
    {
        if (RegCreateKeyExW( HKEY_LOCAL_MACHINE, path, 0, NULL, 0, KEY_SET_VALUE | KEY_CREATE_SUB_KEY,
                             NULL, &hkey, NULL ))
            return NULL;
    }
    else if (RegOpenKeyExW( HKEY_LOCAL_MACHINE, path, 0, KEY_QUERY_VALUE, &hkey ))
        return NULL;
    return hkey;
}

static const struct dwm_mode *find_mode( const struct dwm_output *output, UINT width, UINT height, UINT refresh )
{
    const struct dwm_mode *best = NULL;

    for (UINT i = 0; i < output->mode_count; i++)
    {
        const struct dwm_mode *mode = &output->modes[i];

        if (mode->width != width || mode->height != height) continue;
        if (mode->refresh == refresh) return mode;
        if (!best || (refresh ? labs( (LONG)mode->refresh - (LONG)refresh ) < labs( (LONG)best->refresh - (LONG)refresh )
                              : mode->refresh > best->refresh))
            best = mode;
    }
    return best;
}

/* the size the monitor was made for, or the largest it takes when it does
 * not say (an old monitor on a cable without the data pins) */
static const struct dwm_mode *native_mode( const struct dwm_output *output )
{
    const struct dwm_mode *largest = NULL;

    for (UINT i = 0; i < output->mode_count; i++)
    {
        const struct dwm_mode *mode = &output->modes[i];

        if (mode->flags & DWM_MODE_PREFERRED) return mode;
        if (!largest || (UINT64)mode->width * mode->height > (UINT64)largest->width * largest->height)
            largest = mode;
    }
    return largest;
}

/* the fastest the monitor goes at that size, then the next fastest, and so
 * on: a mode from the monitor's list can still be more than the cable takes */
static const struct dwm_mode *mode_for_attempt( const struct dwm_output *output, UINT attempt )
{
    const struct dwm_mode *native = native_mode( output ), *mode = NULL;
    UINT refresh = ~0u;

    if (!native) return NULL;
    for (UINT skipped = 0; skipped <= attempt; skipped++)
    {
        mode = NULL;
        for (UINT i = 0; i < output->mode_count; i++)
        {
            const struct dwm_mode *m = &output->modes[i];

            if (m->width != native->width || m->height != native->height) continue;
            if (m->refresh >= refresh) continue;
            if (!mode || m->refresh > mode->refresh) mode = m;
        }
        if (!mode) return native;
        refresh = mode->refresh;
    }
    return mode;
}

static void config_from_mode( struct dwm_output_config *config, const struct dwm_output *output,
                              const struct dwm_mode *mode, INT x, INT y )
{
    config->id = output->id;
    config->enabled = 1;
    config->x = x;
    config->y = y;
    config->width = mode->width;
    config->height = mode->height;
    config->refresh = mode->refresh;
}

/* every monitor keeps its own place; the primary one is at 0,0, as Windows
 * has it, because that is where the Win32 side takes the desktop to start */
static void place_outputs( struct dwm_output_config *configs, UINT primary )
{
    INT x = configs[primary].x, y = configs[primary].y;

    for (UINT i = 0; i < output_count; i++)
    {
        configs[i].x -= x;
        configs[i].y -= y;
    }
}

/* The layout kept for this set of monitors. Every monitor has to be in it
 * with a mode it still has, or the whole layout is dropped: a monitor swapped
 * for another one gets a fresh one. */
static BOOL saved_layout( struct dwm_output_config *configs )
{
    HKEY hkey = open_configuration( FALSE );
    BOOL ok = hkey != NULL;
    UINT primary = 0;

    for (UINT i = 0; ok && i < output_count; i++)
    {
        DWORD width = 0, height = 0, refresh = 0, size;
        const struct dwm_mode *mode;
        INT x = 0, y = 0;
        WCHAR id[64];
        HKEY monitor;

        monitor_id( &outputs[i], id, ARRAY_SIZE(id) );
        if (RegOpenKeyExW( hkey, id, 0, KEY_QUERY_VALUE, &monitor ))
        {
            ok = FALSE;
            break;
        }
        size = sizeof(DWORD);
        RegQueryValueExW( monitor, L"PrimSurfSize.cx", NULL, NULL, (BYTE *)&width, &size );
        size = sizeof(DWORD);
        RegQueryValueExW( monitor, L"PrimSurfSize.cy", NULL, NULL, (BYTE *)&height, &size );
        size = sizeof(DWORD);
        RegQueryValueExW( monitor, L"VSyncFreq.Numerator", NULL, NULL, (BYTE *)&refresh, &size );
        size = sizeof(DWORD);
        RegQueryValueExW( monitor, L"Position.cx", NULL, NULL, (BYTE *)&x, &size );
        size = sizeof(DWORD);
        RegQueryValueExW( monitor, L"Position.cy", NULL, NULL, (BYTE *)&y, &size );
        RegCloseKey( monitor );

        if (!width || !height || !(mode = find_mode( &outputs[i], width, height, refresh ))) ok = FALSE;
        else
        {
            config_from_mode( configs + i, &outputs[i], mode, x, y );
            if (!x && !y) primary = i;
        }
    }
    if (hkey) RegCloseKey( hkey );
    if (ok) place_outputs( configs, primary );
    return ok;
}

/* No layout for these monitors: each one at the size it was made for and the
 * fastest refresh rate it takes there, side by side, the way Windows extends
 * the desktop onto a monitor it sees for the first time. A monitor that is
 * already showing something keeps it, so that plugging in a second one does
 * not disturb the first. */
static void default_layout( struct dwm_output_config *configs, UINT attempt )
{
    UINT primary = 0;
    INT right = 0;

    /* a laptop's own panel is the primary monitor, else the first one */
    for (UINT i = output_count; i--;)
        if (outputs[i].internal) primary = i;

    /* a monitor that already shows something keeps it, so that plugging in
     * a second one does not disturb the first */
    for (UINT i = 0; attempt == 0 && i < output_count; i++)
    {
        const struct dwm_output *output = &outputs[i];

        if (!output->enabled) continue;
        config_from_mode( configs + i, output, &output->modes[output->mode], output->x, output->y );
        right = max( right, output->x + (INT)output->modes[output->mode].width );
        if (!output->x && !output->y) primary = i;
    }

    /* the primary monitor comes first, at 0,0; the rest stand to its right */
    for (UINT pass = 0; pass < 2; pass++)
    {
        for (UINT i = 0; i < output_count; i++)
        {
            const struct dwm_mode *mode;

            if (configs[i].enabled || (i == primary) != (pass == 0)) continue;
            if (!(mode = mode_for_attempt( &outputs[i], attempt ))) continue;
            config_from_mode( configs + i, &outputs[i], mode, i == primary ? 0 : right, 0 );
            right = max( right, configs[i].x + (INT)mode->width );
        }
    }
    place_outputs( configs, primary );
}

/* A monitor with no mode in the layout is left out of it, not turned off:
 * dwm.exe keeps what such a monitor shows. */
static BOOL apply_layout( const struct dwm_output_config *configs )
{
    struct dwm_output_config wanted[DWM_MAX_OUTPUTS];
    struct dwm_set_config_params params = { .configs = wanted };

    for (UINT i = 0; i < output_count; i++)
        if (configs[i].id) wanted[params.count++] = configs[i];
    if (!params.count) return FALSE;

    return !WINE_UNIX_CALL( unix_dwm_set_config, &params );
}

/* Gives every monitor a mode. Called once at start and whenever monitors
 * come or go. */
static void configure_monitors(void)
{
    struct dwm_output_config configs[DWM_MAX_OUTPUTS];

    if (!read_outputs())
    {
        ERR( "no monitor is connected\n" );
        return;
    }

    memset( configs, 0, sizeof(configs) );
    if (saved_layout( configs ) && apply_layout( configs )) return;

    for (UINT attempt = 0; attempt < 4; attempt++)
    {
        memset( configs, 0, sizeof(configs) );
        default_layout( configs, attempt );
        if (apply_layout( configs )) return;
    }
    ERR( "no mode any monitor takes\n" );
}

/* Keeps what a program applied with CDS_UPDATEREGISTRY, so that this set of
 * monitors comes up the same way next time. */
static void save_monitors(void)
{
    HKEY hkey;

    if (!read_outputs() || !(hkey = open_configuration( TRUE )))
    {
        WARN( "cannot keep the display settings\n" );
        return;
    }
    for (UINT i = 0; i < output_count; i++)
    {
        const struct dwm_output *output = &outputs[i];
        DWORD value;
        WCHAR id[64];
        HKEY monitor;

        if (!output->enabled) continue;
        monitor_id( output, id, ARRAY_SIZE(id) );
        if (RegCreateKeyExW( hkey, id, 0, NULL, 0, KEY_SET_VALUE, NULL, &monitor, NULL )) continue;
        value = output->modes[output->mode].width;
        RegSetValueExW( monitor, L"PrimSurfSize.cx", 0, REG_DWORD, (BYTE *)&value, sizeof(value) );
        value = output->modes[output->mode].height;
        RegSetValueExW( monitor, L"PrimSurfSize.cy", 0, REG_DWORD, (BYTE *)&value, sizeof(value) );
        value = output->modes[output->mode].refresh;
        RegSetValueExW( monitor, L"VSyncFreq.Numerator", 0, REG_DWORD, (BYTE *)&value, sizeof(value) );
        value = 1000;
        RegSetValueExW( monitor, L"VSyncFreq.Denominator", 0, REG_DWORD, (BYTE *)&value, sizeof(value) );
        value = output->x;
        RegSetValueExW( monitor, L"Position.cx", 0, REG_DWORD, (BYTE *)&value, sizeof(value) );
        value = output->y;
        RegSetValueExW( monitor, L"Position.cy", 0, REG_DWORD, (BYTE *)&value, sizeof(value) );
        RegCloseKey( monitor );
    }
    RegCloseKey( hkey );
}

/**********************************************************************
 *          The windows
 */

static struct composition_window *list;
static struct dwm_window           *windows;
static int                          capacity;
static UINT64                       serial = ~(UINT64)0;
static POINT                        cursor = { -1, -1 };
static BOOL                         cursor_hidden;

static BOOL grow( int count )
{
    struct composition_window *new_list;
    struct dwm_window *new_windows;

    if (count <= capacity) return TRUE;
    count = max( count, capacity * 2 );
    if (!(new_list = realloc( list, count * sizeof(*list) ))) return FALSE;
    list = new_list;
    if (!(new_windows = realloc( windows, count * sizeof(*windows) ))) return FALSE;
    windows = new_windows;
    capacity = count;
    return TRUE;
}

/* the toplevel windows of our desktop, topmost first, and the cursor, as wineserver has them */
static void update_windows(void)
{
    struct dwm_set_windows_params params;
    struct dwm_set_cursor_params pos;
    UINT64 new_serial;
    int count, got;
    NTSTATUS status;

    for (;;)
    {
        SERVER_START_REQ( get_composition_list )
        {
            wine_server_set_reply( req, list, capacity * sizeof(*list) );
            status = wine_server_call( req );
            new_serial = reply->serial;
            count = reply->count;
            pos.x = reply->cursor_x;
            pos.y = reply->cursor_y;
            pos.hidden = reply->cursor_hidden;
            got = wine_server_reply_size( reply ) / sizeof(*list);
        }
        SERVER_END_REQ;
        if (status || count <= got || !grow( count )) break;
    }
    if (status) return;
    if (pos.x != cursor.x || pos.y != cursor.y || pos.hidden != cursor_hidden)
    {
        cursor.x = pos.x;
        cursor.y = pos.y;
        cursor_hidden = pos.hidden;
        WINE_UNIX_CALL( unix_dwm_set_cursor, &pos );
    }
    if (new_serial == serial) return;
    serial = new_serial;

    for (int i = 0; i < got; i++)
    {
        windows[i].hwnd     = list[i].handle;
        windows[i].style    = list[i].style;
        windows[i].ex_style = list[i].ex_style;
        windows[i].left     = list[i].visible_rect.left;
        windows[i].top      = list[i].visible_rect.top;
        windows[i].right    = list[i].visible_rect.right;
        windows[i].bottom   = list[i].visible_rect.bottom;
    }
    params.count = got;
    params.windows = windows;
    WINE_UNIX_CALL( unix_dwm_set_windows, &params );
}

/* Takes the display and composes the desktop; returns only on failure. */
DWORD WINAPI DwmCoreRun(void)
{
    struct dwm_start_params params = { .background = desktop_color() };
    struct dwm_dispatch_params dispatch = { .timeout_ms = 8 };
    NTSTATUS status;

    if ((status = WINE_UNIX_CALL( unix_dwm_start, &params )))
    {
        ERR( "cannot take the display: %#lx\n", status );
        return status;
    }
    /* the display layout is kept in HKLM, as Windows keeps it */
    if ((status = NtSetInformationProcess( GetCurrentProcess(), ProcessWineGrantAdminToken, NULL, 0 )))
        WARN( "no admin token: %#lx\n", status );
    configure_monitors();
    RegCreateKeyExW( HKEY_CURRENT_USER, L"Software\\Microsoft\\DirectX\\UserGpuPreferences", 0, NULL, 0,
                     KEY_QUERY_VALUE | KEY_NOTIFY, NULL, &gpu_preferences, NULL );
    RegCreateKeyExW( HKEY_LOCAL_MACHINE, monitor_store_keyW, 0, NULL, 0,
                     KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_CREATE_SUB_KEY | KEY_NOTIFY, NULL, &monitor_store, NULL );
    options_changed = CreateEventW( NULL, FALSE, FALSE, NULL );
    publish_vrr_capability();
    update_options();

    /* wininit.exe starts csrss.exe on this: the display driver connects as it loads */
    SetEvent( CreateEventW( NULL, TRUE, FALSE, L"__arctic_dwm_ready" ) );
    grow( 256 );
    for (;;)
    {
        if ((status = WINE_UNIX_CALL( unix_dwm_dispatch, &dispatch ))) return status;
        if (dispatch.events & DWM_EVENT_HOTPLUG)
        {
            configure_monitors();
            publish_vrr_capability();
            update_options();
        }
        if (dispatch.events & DWM_EVENT_SAVE) save_monitors();
        if (!WaitForSingleObject( options_changed, 0 )) update_options();
        update_windows();
    }
}
