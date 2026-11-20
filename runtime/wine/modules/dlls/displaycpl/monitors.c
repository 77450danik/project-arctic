/*
 * Display of the Control Panel: the monitors
 *
 * What desk.cpl's Settings page knew of them, for the page drawn as Windows
 * 11's: each source of the cards with a monitor on it, attached to the
 * desktop or not; its modes, rotated ones among them; the monitor's own name
 * and Plug and Play id from its EDID; where its own settings are kept
 * (variable refresh, how smaller modes show, MonitorDataStore, read by
 * dwm.exe); the scale of Windows' Settings. A change is written for every
 * monitor first and made so all at once, as Windows does; what was there
 * before is kept to be taken back (snapshot).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdio.h>

#include "displaycpl.h"

#define MONITOR_STORE_KEY L"System\\CurrentControlSet\\Control\\GraphicsDrivers\\MonitorDataStore"
#define DESKTOP_BAG_KEY   L"Software\\Microsoft\\Windows\\Shell\\Bags\\1\\Desktop"

/**********************************************************************
 *          What the display configuration says of a source
 */

static BOOL monitor_target( const WCHAR *device, DISPLAYCONFIG_TARGET_DEVICE_NAME *target )
{
    DISPLAYCONFIG_PATH_INFO *paths;
    DISPLAYCONFIG_MODE_INFO *modes;
    UINT32 path_count = 0, mode_count = 0;
    BOOL found = FALSE;

    if (GetDisplayConfigBufferSizes( QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count ) || !path_count) return FALSE;
    paths = calloc( path_count, sizeof(*paths) );
    modes = calloc( max( mode_count, 1 ), sizeof(*modes) );
    if (paths && modes && !QueryDisplayConfig( QDC_ONLY_ACTIVE_PATHS, &path_count, paths, &mode_count, modes, NULL ))
    {
        for (UINT32 i = 0; i < path_count && !found; i++)
        {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source;

            memset( &source, 0, sizeof(source) );
            source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            source.header.size = sizeof(source);
            source.header.adapterId = paths[i].sourceInfo.adapterId;
            source.header.id = paths[i].sourceInfo.id;
            if (DisplayConfigGetDeviceInfo( &source.header ) || wcsicmp( source.viewGdiDeviceName, device )) continue;
            memset( target, 0, sizeof(*target) );
            target->header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            target->header.size = sizeof(*target);
            target->header.adapterId = paths[i].targetInfo.adapterId;
            target->header.id = paths[i].targetInfo.id;
            found = !DisplayConfigGetDeviceInfo( &target->header );
        }
    }
    free( paths );
    free( modes );
    return found;
}

/* "ACR0A2B" of \\?\DISPLAY#ACR0A2B#...: a monitor without an EDID has none */
static void monitor_pnp( const DISPLAYCONFIG_TARGET_DEVICE_NAME *target, WCHAR *pnp, UINT size )
{
    const WCHAR *start, *end;

    pnp[0] = 0;
    if (wcsnicmp( target->monitorDevicePath, L"\\\\?\\DISPLAY", 11 ) ||
        (target->monitorDevicePath[11] != '#' && target->monitorDevicePath[11] != '\\'))
        return;
    start = target->monitorDevicePath + 12;
    if (!(end = wcspbrk( start, L"#\\" )) || end == start || end - start >= size ||
        !wcsnicmp( start, L"Default_Monitor", 15 ))
        return;
    memcpy( pnp, start, (end - start) * sizeof(WCHAR) );
    pnp[end - start] = 0;
}

/**********************************************************************
 *          The monitor's own settings (MonitorDataStore)
 */

static BOOL store_read( const struct monitor *m, const WCHAR *name, DWORD *value )
{
    WCHAR key[128];
    DWORD size = sizeof(*value);

    if (!m->pnp[0]) return FALSE;
    swprintf( key, ARRAY_SIZE(key), L"%s\\%s", MONITOR_STORE_KEY, m->pnp );
    return !RegGetValueW( HKEY_LOCAL_MACHINE, key, name, RRF_RT_REG_DWORD, NULL, value, &size );
}

static void store_write( const struct monitor *m, const WCHAR *name, DWORD value )
{
    WCHAR key[128];

    if (!m->pnp[0]) return;
    swprintf( key, ARRAY_SIZE(key), L"%s\\%s", MONITOR_STORE_KEY, m->pnp );
    RegSetKeyValueW( HKEY_LOCAL_MACHINE, key, name, REG_DWORD, &value, sizeof(value) );
}

void monitor_write_vrr( const struct monitor *m )
{
    store_write( m, L"VariableRefresh", m->vrr ? 1 : 0 );
}

/* dwm.exe follows it at once, a game running included */
void monitor_write_scaling( const struct monitor *m )
{
    store_write( m, L"Scaling", m->scaling );
}

/* Custom refresh rates (dwmcore.c): the mHz kept, a DWORD each; the one on
 * trial; whether they are offered. dwm.exe follows each change at once. */
/* each rate: mHz, and the blank in the top four bits (CUSTOM_RATE) */
UINT monitor_custom_rates( const struct monitor *m, DWORD *rates, UINT max )
{
    DWORD values[CUSTOM_MAX], size = sizeof(values);
    WCHAR key[128];
    UINT count = 0;

    if (!m->pnp[0]) return 0;
    swprintf( key, ARRAY_SIZE(key), L"%s\\%s", MONITOR_STORE_KEY, m->pnp );
    if (RegGetValueW( HKEY_LOCAL_MACHINE, key, L"CustomRefresh", RRF_RT_REG_BINARY, NULL, values, &size )) return 0;
    for (UINT i = 0; i < size / sizeof(DWORD) && count < max; i++) rates[count++] = values[i];
    return count;
}

void monitor_custom_set_rates( const struct monitor *m, const DWORD *values, UINT count )
{
    DWORD rates[CUSTOM_MAX];
    WCHAR key[128];

    if (!m->pnp[0]) return;
    swprintf( key, ARRAY_SIZE(key), L"%s\\%s", MONITOR_STORE_KEY, m->pnp );
    count = min( count, CUSTOM_MAX );
    for (UINT i = 0; i < count; i++) rates[i] = values[i];
    if (count) RegSetKeyValueW( HKEY_LOCAL_MACHINE, key, L"CustomRefresh", REG_BINARY, rates, count * sizeof(DWORD) );
    else RegDeleteKeyValueW( HKEY_LOCAL_MACHINE, key, L"CustomRefresh" );
}

BOOL monitor_custom_on( const struct monitor *m )
{
    DWORD off = 0;

    return !store_read( m, L"CustomRefreshOff", &off ) || !off;
}

void monitor_custom_set_on( const struct monitor *m, BOOL on )
{
    store_write( m, L"CustomRefreshOff", on ? 0 : 1 );
}

void monitor_custom_trial( const struct monitor *m, DWORD rate )
{
    WCHAR key[128];

    if (!m->pnp[0]) return;
    swprintf( key, ARRAY_SIZE(key), L"%s\\%s", MONITOR_STORE_KEY, m->pnp );
    if (rate) store_write( m, L"CustomRefreshTrial", rate );
    else RegDeleteKeyValueW( HKEY_LOCAL_MACHINE, key, L"CustomRefreshTrial" );
}

/**********************************************************************
 *          The list of monitors
 */

static BOOL usable( const DEVMODEW *dm )
{
    DWORD shorter = min( dm->dmPelsWidth, dm->dmPelsHeight ), longer = max( dm->dmPelsWidth, dm->dmPelsHeight );

    if (shorter < 480 || longer < 640) return FALSE;
    return dm->dmBitsPerPel == 8 || dm->dmBitsPerPel == 16 || dm->dmBitsPerPel == 24 || dm->dmBitsPerPel == 32;
}

static void load_modes( struct monitor *m )
{
    DEVMODEW dm;
    UINT capacity = 0;

    memset( &dm, 0, sizeof(dm) );
    dm.dmSize = sizeof(dm);
    /* the rotated ones too: the orientations it can take are among them */
    for (DWORD i = 0; EnumDisplaySettingsExW( m->device, i, &dm, EDS_ROTATEDMODE ); i++)
    {
        struct mode mode = { dm.dmPelsWidth, dm.dmPelsHeight, dm.dmBitsPerPel, dm.dmDisplayFrequency,
                             (dm.dmFields & DM_DISPLAYORIENTATION) ? dm.dmDisplayOrientation : DMDO_DEFAULT };
        UINT k;

        if (usable( &dm ))
        {
            for (k = 0; k < m->mode_count; k++)
                if (!memcmp( &m->modes[k], &mode, sizeof(mode) )) break;
            if (k == m->mode_count)
            {
                if (m->mode_count == capacity)
                {
                    struct mode *modes = realloc( m->modes, (capacity = capacity ? capacity * 2 : 64) * sizeof(*modes) );
                    if (!modes) break;
                    m->modes = modes;
                }
                m->modes[m->mode_count++] = mode;
            }
        }
        memset( &dm, 0, sizeof(dm) );
        dm.dmSize = sizeof(dm);
    }
}

const struct mode *monitor_best_mode( const struct monitor *m, DWORD width, DWORD height, DWORD bpp, DWORD freq,
                                      DWORD orientation )
{
    const struct mode *best = NULL;
    ULONGLONG best_score = ~(ULONGLONG)0;

    for (UINT i = 0; i < m->mode_count; i++)
    {
        const struct mode *mode = &m->modes[i];
        ULONGLONG score = 0;

        if (orientation != ~0u && mode->orientation != orientation) continue;
        if (width != ~0u && (mode->width != width || mode->height != height)) continue;
        /* the depth asked for, else the deepest; the rate asked for, else the fastest */
        if (bpp != ~0u) score += (ULONGLONG)(mode->bpp > bpp ? mode->bpp - bpp : bpp - mode->bpp) << 40;
        else score += (ULONGLONG)(32 - min( mode->bpp, 32 )) << 40;
        if (width == ~0u) score += (ULONGLONG)(0xffffffffu - mode->width * mode->height) << 8;
        if (freq != ~0u) score += mode->freq > freq ? mode->freq - freq : freq - mode->freq;
        else score += 1000 - min( mode->freq, 1000 );
        if (score < best_score)
        {
            best = mode;
            best_score = score;
        }
    }
    return best;
}

void mode_to_devmode( const struct mode *mode, DEVMODEW *dm )
{
    memset( dm, 0, sizeof(*dm) );
    dm->dmSize = sizeof(*dm);
    dm->dmPelsWidth = mode->width;
    dm->dmPelsHeight = mode->height;
    dm->dmBitsPerPel = mode->bpp;
    dm->dmDisplayFrequency = mode->freq;
    dm->dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    dm->dmDisplayOrientation = mode->orientation;
    dm->dmFields |= DM_DISPLAYORIENTATION;
}

void displays_free( struct displays *d )
{
    for (UINT i = 0; i < d->count; i++) free( d->list[i].modes );
    free( d->list );
    d->list = NULL;
    d->count = 0;
}

void displays_load( struct displays *d )
{
    DISPLAY_DEVICEW dd;
    UINT capacity = 0;

    displays_free( d );
    memset( &dd, 0, sizeof(dd) );
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW( NULL, i, &dd, 0 ); i++)
    {
        DISPLAY_DEVICEW monitor = { sizeof(monitor) };
        DISPLAYCONFIG_TARGET_DEVICE_NAME target;
        BOOL has_monitor = EnumDisplayDevicesW( dd.DeviceName, 0, &monitor, 0 );
        struct monitor *m;
        DWORD value;

        /* a mirror driver, or an output of the card with nothing plugged in */
        if ((dd.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) ||
            (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) && !has_monitor))
            goto next;
        if (d->count == capacity)
        {
            struct monitor *list = realloc( d->list, (capacity = capacity ? capacity * 2 : 4) * sizeof(*list) );
            if (!list) break;
            d->list = list;
        }
        m = &d->list[d->count++];
        memset( m, 0, sizeof(*m) );
        m->number = d->count;
        lstrcpynW( m->device, dd.DeviceName, ARRAY_SIZE(m->device) );
        lstrcpynW( m->adapter, dd.DeviceString, ARRAY_SIZE(m->adapter) );
        m->attached = (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0;
        m->primary = (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
        if (monitor_target( m->device, &target ))
        {
            lstrcpynW( m->name, target.monitorFriendlyDeviceName, ARRAY_SIZE(m->name) );
            monitor_pnp( &target, m->pnp, ARRAY_SIZE(m->pnp) );
        }
        if (!m->name[0] && has_monitor) lstrcpynW( m->name, monitor.DeviceString, ARRAY_SIZE(m->name) );

        m->current.dmSize = sizeof(m->current);
        if (!EnumDisplaySettingsExW( m->device, ENUM_CURRENT_SETTINGS, &m->current, 0 ) || !m->current.dmPelsWidth)
        {
            /* not on the desktop: the mode it had last */
            memset( &m->current, 0, sizeof(m->current) );
            m->current.dmSize = sizeof(m->current);
            EnumDisplaySettingsExW( m->device, ENUM_REGISTRY_SETTINGS, &m->current, 0 );
        }
        if (m->current.dmFields & DM_POSITION)
        {
            m->pos.x = m->current.dmPosition.x;
            m->pos.y = m->current.dmPosition.y;
        }
        load_modes( m );
        if (!m->current.dmPelsWidth || !m->current.dmPelsHeight)
        {
            const struct mode *best = monitor_best_mode( m, ~0u, 0, ~0u, ~0u, DMDO_DEFAULT );

            if (best) mode_to_devmode( best, &m->current );
        }

        m->vrr = TRUE;
        m->scaling = SCALING_ASPECT;
        if (store_read( m, L"VariableRefreshCapable", &value )) m->vrr_capable = value != 0;
        if (store_read( m, L"VariableRefresh", &value )) m->vrr = value != 0;
        if (store_read( m, L"Scaling", &value ) && (value == SCALING_CENTER || value == SCALING_STRETCH))
            m->scaling = value;
    next:
        memset( &dd, 0, sizeof(dd) );
        dd.cb = sizeof(dd);
    }
}

struct monitor *displays_find( struct displays *d, const WCHAR *device )
{
    for (UINT i = 0; i < d->count; i++)
        if (!wcsicmp( d->list[i].device, device )) return &d->list[i];
    return NULL;
}

UINT displays_attached( const struct displays *d )
{
    UINT count = 0;

    for (UINT i = 0; i < d->count; i++) if (d->list[i].attached) count++;
    return count;
}

/**********************************************************************
 *          Changes
 */

LONG displays_commit(void)
{
    return ChangeDisplaySettingsExW( NULL, NULL, NULL, 0, NULL );
}

LONG monitor_set_mode( const struct monitor *m, const DEVMODEW *dm, BOOL primary, BOOL commit )
{
    DEVMODEW mode = *dm;
    LONG ret;

    mode.dmSize = sizeof(mode);
    mode.dmDriverExtra = 0;
    if (!commit) return ChangeDisplaySettingsExW( m->device, &mode, NULL,
                                                  CDS_UPDATEREGISTRY | CDS_NORESET | (primary ? CDS_SET_PRIMARY : 0), NULL );
    ret = ChangeDisplaySettingsExW( m->device, &mode, NULL, CDS_UPDATEREGISTRY | (primary ? CDS_SET_PRIMARY : 0), NULL );
    return ret;
}

/* where the monitor stands, in the mode it has */
static void placed_mode( const struct monitor *m, POINT pos, DEVMODEW *dm )
{
    *dm = m->current;
    dm->dmSize = sizeof(*dm);
    dm->dmDriverExtra = 0;
    dm->dmFields = (dm->dmFields & (DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY |
                                    DM_DISPLAYORIENTATION)) | DM_PELSWIDTH | DM_PELSHEIGHT | DM_POSITION;
    dm->dmPosition.x = pos.x;
    dm->dmPosition.y = pos.y;
}

static void detached_mode( DEVMODEW *dm )
{
    memset( dm, 0, sizeof(*dm) );
    dm->dmSize = sizeof(*dm);
    dm->dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT;
}

/* the places the monitors were dragged to; the primary one stays at the origin */
LONG displays_set_layout( struct displays *d )
{
    POINT origin = { 0, 0 };
    LONG ret = DISP_CHANGE_SUCCESSFUL;

    for (UINT i = 0; i < d->count; i++)
        if (d->list[i].attached && d->list[i].primary) origin = d->list[i].pos;
    for (UINT i = 0; i < d->count && ret == DISP_CHANGE_SUCCESSFUL; i++)
    {
        struct monitor *m = &d->list[i];
        POINT pos = { m->pos.x - origin.x, m->pos.y - origin.y };
        DEVMODEW dm;

        if (!m->attached) continue;
        placed_mode( m, pos, &dm );
        ret = monitor_set_mode( m, &dm, m->primary, FALSE );
    }
    if (ret == DISP_CHANGE_SUCCESSFUL) ret = displays_commit();
    return ret;
}

LONG displays_set_primary( struct displays *d, UINT index )
{
    POINT origin;
    LONG ret = DISP_CHANGE_SUCCESSFUL;

    if (index >= d->count || !d->list[index].attached) return DISP_CHANGE_BADPARAM;
    origin = d->list[index].pos;
    for (UINT i = 0; i < d->count && ret == DISP_CHANGE_SUCCESSFUL; i++)
    {
        struct monitor *m = &d->list[i];
        POINT pos = { m->pos.x - origin.x, m->pos.y - origin.y };
        DEVMODEW dm;

        if (!m->attached) continue;
        placed_mode( m, pos, &dm );
        ret = monitor_set_mode( m, &dm, i == index, FALSE );
    }
    if (ret == DISP_CHANGE_SUCCESSFUL) ret = displays_commit();
    return ret;
}

/* the monitors that are not on the desktop come to it, right of the others */
LONG displays_extend( struct displays *d )
{
    LONG ret = DISP_CHANGE_SUCCESSFUL, right = 0;

    for (UINT i = 0; i < d->count; i++)
        if (d->list[i].attached) right = max( right, d->list[i].pos.x + (LONG)d->list[i].current.dmPelsWidth );
    for (UINT i = 0; i < d->count && ret == DISP_CHANGE_SUCCESSFUL; i++)
    {
        struct monitor *m = &d->list[i];
        POINT pos = { right, 0 };
        DEVMODEW dm;

        if (m->attached) continue;
        placed_mode( m, pos, &dm );
        right += dm.dmPelsWidth;
        ret = monitor_set_mode( m, &dm, FALSE, FALSE );
    }
    if (ret == DISP_CHANGE_SUCCESSFUL) ret = displays_commit();
    return ret;
}

/* the desktop on one monitor only, the others off it */
LONG displays_only( struct displays *d, UINT index )
{
    LONG ret = DISP_CHANGE_SUCCESSFUL;

    if (index >= d->count) return DISP_CHANGE_BADPARAM;
    for (UINT i = 0; i < d->count && ret == DISP_CHANGE_SUCCESSFUL; i++)
    {
        struct monitor *m = &d->list[i];
        POINT origin = { 0, 0 };
        DEVMODEW dm;

        if (i == index) placed_mode( m, origin, &dm );
        else if (m->attached) detached_mode( &dm );
        else continue;
        ret = monitor_set_mode( m, &dm, i == index, FALSE );
    }
    if (ret == DISP_CHANGE_SUCCESSFUL) ret = displays_commit();
    return ret;
}

void snapshot_take( struct snapshot *s, const struct displays *d )
{
    s->count = 0;
    for (UINT i = 0; i < d->count && s->count < ARRAY_SIZE(s->list); i++)
    {
        const struct monitor *m = &d->list[i];

        lstrcpynW( s->list[s->count].device, m->device, ARRAY_SIZE(s->list[0].device) );
        s->list[s->count].attached = m->attached;
        s->list[s->count].primary = m->primary;
        placed_mode( m, m->pos, &s->list[s->count].mode );
        s->count++;
    }
}

LONG snapshot_restore( const struct snapshot *s )
{
    LONG ret = DISP_CHANGE_SUCCESSFUL;

    for (UINT i = 0; i < s->count && ret == DISP_CHANGE_SUCCESSFUL; i++)
    {
        DEVMODEW dm = s->list[i].mode;
        DWORD flags = CDS_UPDATEREGISTRY | CDS_NORESET | (s->list[i].primary ? CDS_SET_PRIMARY : 0);

        if (!s->list[i].attached) detached_mode( &dm );
        ret = ChangeDisplaySettingsExW( s->list[i].device, &dm, NULL, flags, NULL );
    }
    if (ret == DISP_CHANGE_SUCCESSFUL) ret = displays_commit();
    return ret;
}

/**********************************************************************
 *          The scale, as Windows' Settings set it
 *
 * DisplayConfigGet/SetDeviceInfo with the types Windows does not document:
 * the steps relative to the recommended one; a change applies at once.
 */

const int scale_steps[12] = { 100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500 };

#define GET_SOURCE_DPI_SCALE (-3)
#define SET_SOURCE_DPI_SCALE (-4)

struct scale_get
{
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    INT32 min_rel, cur_rel, max_rel;
};

struct scale_set
{
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    INT32 rel;
};

static BOOL find_source( const WCHAR *device, LUID *adapter, UINT32 *id )
{
    DISPLAYCONFIG_PATH_INFO paths[16];
    DISPLAYCONFIG_MODE_INFO modes[32];
    UINT32 path_count = ARRAY_SIZE(paths), mode_count = ARRAY_SIZE(modes);

    if (QueryDisplayConfig( QDC_ONLY_ACTIVE_PATHS, &path_count, paths, &mode_count, modes, NULL )) return FALSE;
    for (UINT32 i = 0; i < path_count; i++)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME name;

        memset( &name, 0, sizeof(name) );
        name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        name.header.size = sizeof(name);
        name.header.adapterId = paths[i].sourceInfo.adapterId;
        name.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo( &name.header ) || wcsicmp( name.viewGdiDeviceName, device )) continue;
        *adapter = paths[i].sourceInfo.adapterId;
        *id = paths[i].sourceInfo.id;
        return TRUE;
    }
    return FALSE;
}

BOOL scale_get( const WCHAR *device, int *min_rel, int *cur_rel, int *max_rel )
{
    struct scale_get get;

    memset( &get, 0, sizeof(get) );
    get.header.type = GET_SOURCE_DPI_SCALE;
    get.header.size = sizeof(get);
    if (!find_source( device, &get.header.adapterId, &get.header.id ) || DisplayConfigGetDeviceInfo( &get.header ))
        return FALSE;
    *min_rel = get.min_rel;
    *cur_rel = get.cur_rel;
    *max_rel = get.max_rel;
    return TRUE;
}

BOOL scale_set( const WCHAR *device, int rel )
{
    LONG (WINAPI *set_info)( DISPLAYCONFIG_DEVICE_INFO_HEADER * ) =
        (void *)GetProcAddress( GetModuleHandleW( L"user32.dll" ), "DisplayConfigSetDeviceInfo" );
    struct scale_set set;

    memset( &set, 0, sizeof(set) );
    set.header.type = SET_SOURCE_DPI_SCALE;
    set.header.size = sizeof(set);
    set.rel = rel;
    if (!set_info || !find_source( device, &set.header.adapterId, &set.header.id )) return FALSE;
    return !set_info( &set.header );
}

/**********************************************************************
 *          The size of the desktop's icons
 *
 * 32, 48 or 64 pixels at 100 %, at the system's scale, kept where Windows
 * keeps it (the desktop's View menu offers the same); the desktop takes it
 * at once on WM_SETTINGCHANGE "DesktopIconSize".
 */

DWORD desktop_icon_size(void)
{
    DWORD value = 48, size = sizeof(value);

    if (RegGetValueW( HKEY_CURRENT_USER, DESKTOP_BAG_KEY, L"IconSize", RRF_RT_REG_DWORD, NULL, &value, &size ))
        value = 48;
    return (value == 32 || value == 64) ? value : 48;
}

void desktop_icon_size_set( DWORD size )
{
    if (size == desktop_icon_size()) return;
    RegSetKeyValueW( HKEY_CURRENT_USER, DESKTOP_BAG_KEY, L"IconSize", REG_DWORD, &size, sizeof(size) );
    SendMessageTimeoutW( HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"DesktopIconSize", SMTO_ABORTIFHUNG, 5000, NULL );
}
