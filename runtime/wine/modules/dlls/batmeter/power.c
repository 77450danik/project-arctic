/*
 * Battery icon of the notification area: what it shows and changes
 *
 * The batteries come from powrprof.dll (ArcticPowerStatus); the texts are
 * Windows 10's: the tip from batmeter.dll.mui, the flyout's from
 * BatteryFlyoutExperience. The power slider sits over the Balanced scheme
 * only, as in Windows: on battery its four places are battery saver, the
 * better battery overlay, none (recommended) and the best performance
 * overlay; on the mains the last three. The power policy in winlogon puts
 * what is chosen on the hardware.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "batmeter.h"
#include "winreg.h"
#include "rpc.h"
#include "shellapi.h"

static const GUID null_guid;
static const GUID scheme_balanced = { 0x381b4222, 0xf694, 0x41f0, { 0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e } };
static const GUID overlay_better_battery = { 0x961cc777, 0x2547, 0x4f9d, { 0x81, 0x74, 0x7d, 0x86, 0x18, 0x1b, 0x8a, 0x7a } };
static const GUID overlay_high = { 0x3af9b8d9, 0x7c97, 0x431d, { 0xad, 0x78, 0x34, 0xa8, 0xbf, 0xea, 0x43, 0x9f } };
static const GUID overlay_best = { 0xded574b5, 0x45a0, 0x4f42, { 0x87, 0x37, 0x46, 0x34, 0x5c, 0x09, 0xc2, 0x38 } };

static GUID current_overlay;

static BOOL read_overlay( BOOL ac, GUID *overlay )
{
    WCHAR value[64];
    DWORD size = sizeof(value);

    *overlay = null_guid;
    if (RegGetValueW( HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Power\\User\\PowerSchemes",
                      ac ? L"ActiveOverlayAcPowerScheme" : L"ActiveOverlayDcPowerScheme", RRF_RT_REG_SZ, NULL, value,
                      &size ))
        return FALSE;
    return UuidFromStringW( value, overlay ) == RPC_S_OK;
}

void battery_read( struct battery_view *view )
{
    DWORD saver = 0, size = sizeof(saver);
    GUID *scheme;
    BOOL ac;

    memset( view, 0, sizeof(*view) );
    if (!ArcticPowerStatus( &view->status )) view->status.percent = ARCTIC_UNKNOWN;
    RegGetValueW( HKEY_LOCAL_MACHINE, ARCTIC_POWER_KEY, ARCTIC_ENERGY_SAVER_ON, RRF_RT_REG_DWORD, NULL, &saver, &size );
    view->saver = !!saver;
    ac = view->status.ac_online;

    if (!PowerGetActiveScheme( NULL, &scheme ))
    {
        view->slider = IsEqualGUID( scheme, &scheme_balanced );
        LocalFree( scheme );
    }
    view->positions = ac ? 3 : 4;
    read_overlay( ac, &current_overlay );
    if (!ac && view->saver) view->position = 0;
    else if (IsEqualGUID( &current_overlay, &overlay_better_battery )) view->position = ac ? 0 : 1;
    else if (IsEqualGUID( &current_overlay, &overlay_best )) view->position = ac ? 2 : 3;
    else view->position = ac ? 1 : 2;
}

WCHAR battery_view_glyph( const struct battery_view *view )
{
    const struct arctic_power_status *s = &view->status;

    if (!s->battery_count || s->percent == ARCTIC_UNKNOWN) return battery_glyph( GLYPH_BATTERY, ARCTIC_UNKNOWN );
    if (s->ac_online) return battery_glyph( GLYPH_CHARGING, s->percent );
    if (view->saver) return battery_glyph( GLYPH_SAVER, s->percent );
    return battery_glyph( GLYPH_BATTERY, s->percent );
}

/* "94% доступно (підключено до електромережі)", "3 год. 20 хв. (94%) залишилось" */
void battery_tip( const struct battery_view *view, WCHAR *tip, size_t count )
{
    const struct arctic_power_status *s = &view->status;
    UINT percent = s->percent, secs = s->seconds_left;
    const WCHAR *text;

    if (!s->battery_count) text = load_string( IDS_TIP_NO_BATTERY );
    else if (percent == ARCTIC_UNKNOWN) text = s->ac_online ? load_string( IDS_TIP_AC ) : load_string( IDS_TIP_UNKNOWN );
    else if (s->ac_online)
    {
        if (s->state == ARCTIC_BATTERY_FULL || percent >= 100) text = format_string( IDS_TIP_FULL );
        else text = format_string( IDS_TIP_CHARGING, percent );
    }
    else if (secs != ARCTIC_UNKNOWN && secs >= 60)
    {
        UINT hours = secs / 3600, minutes = secs % 3600 / 60;

        if (hours) text = format_string( view->saver ? IDS_TIP_SAVER_HOURS : IDS_TIP_HOURS_LEFT, percent, minutes, hours );
        else text = format_string( view->saver ? IDS_TIP_SAVER_MINUTES : IDS_TIP_MINUTES_LEFT, percent, minutes );
    }
    else text = format_string( view->saver ? IDS_TIP_SAVER_PERCENT : IDS_TIP_PERCENT_LEFT, percent );
    lstrcpynW( tip, text, count );
}

/* "3 год. 20 хв. ": the two biggest units, as the flyout says them */
static void duration( UINT secs, WCHAR *first, WCHAR *second, size_t count )
{
    UINT days = secs / 86400, hours = secs % 86400 / 3600, minutes = secs % 3600 / 60;

    first[0] = second[0] = 0;
    if (days)
    {
        lstrcpynW( first, days == 1 ? load_string( IDS_ONE_DAY ) : format_string( IDS_DAYS, days ), count );
        if (hours) lstrcpynW( second, hours == 1 ? load_string( IDS_ONE_HOUR ) : format_string( IDS_HOURS, hours ), count );
    }
    else if (hours)
    {
        lstrcpynW( first, hours == 1 ? load_string( IDS_ONE_HOUR ) : format_string( IDS_HOURS, hours ), count );
        if (minutes)
            lstrcpynW( second, minutes == 1 ? load_string( IDS_ONE_MINUTE ) : format_string( IDS_MINUTES, minutes ), count );
    }
    else lstrcpynW( first, minutes == 1 ? load_string( IDS_ONE_MINUTE ) : format_string( IDS_MINUTES, max( minutes, 1 ) ),
                    count );
}

static void trim( WCHAR *text )
{
    size_t len = wcslen( text );

    while (len && (text[len - 1] == ' ' || text[len - 1] == '\r' || text[len - 1] == '\n' || text[len - 1] == 0xa0))
        text[--len] = 0;
}

void battery_state_label( const struct battery_view *view, WCHAR *label, size_t count )
{
    const struct arctic_power_status *s = &view->status;
    WCHAR first[64], second[64];

    label[0] = 0;
    if (!s->battery_count) return;
    if (s->ac_online)
    {
        if (s->state == ARCTIC_BATTERY_FULL || s->percent >= 100) lstrcpynW( label, load_string( IDS_FULLY_CHARGED ), count );
        else if (s->state == ARCTIC_BATTERY_CHARGING && s->seconds_left != ARCTIC_UNKNOWN)
        {
            duration( s->seconds_left, first, second, ARRAY_SIZE(first) );
            lstrcpynW( label, format_string( IDS_UNTIL_FULL, first, second ), count );
        }
        else lstrcpynW( label, load_string( IDS_PLUGGED_IN ), count );
    }
    else if (s->seconds_left != ARCTIC_UNKNOWN)
    {
        duration( s->seconds_left, first, second, ARRAY_SIZE(first) );
        lstrcpynW( label, format_string( IDS_LEFT, first, second ), count );
    }
    trim( label );
}

const WCHAR *battery_mode_name( const struct battery_view *view, int position )
{
    static const UINT dc[] = { IDS_BATTERY_SAVER, IDS_BETTER_BATTERY, IDS_RECOMMENDED, IDS_BEST_PERFORMANCE };
    static const UINT ac[] = { IDS_BETTER_BATTERY, IDS_RECOMMENDED, IDS_BEST_PERFORMANCE };
    UINT id;

    position = max( 0, min( position, view->positions - 1 ) );
    id = view->status.ac_online ? ac[position] : dc[position];
    /* the high performance overlay, set elsewhere, sits in the middle */
    if (id == IDS_RECOMMENDED && IsEqualGUID( &current_overlay, &overlay_high )) id = IDS_BETTER_PERFORMANCE;
    return load_string( id );
}

void battery_set_saver( BOOL on )
{
    DWORD value = on;
    HKEY key;

    if (RegCreateKeyExW( HKEY_LOCAL_MACHINE, ARCTIC_POWER_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL )) return;
    RegSetValueExW( key, ARCTIC_ENERGY_SAVER_ON, 0, REG_DWORD, (BYTE *)&value, sizeof(value) );
    RegSetValueExW( key, ARCTIC_ENERGY_SAVER_HAND, 0, REG_DWORD, (BYTE *)&value, sizeof(value) );
    RegCloseKey( key );
}

void battery_set_position( struct battery_view *view, int position )
{
    BOOL ac = view->status.ac_online;
    GUID overlay;

    position = max( 0, min( position, view->positions - 1 ) );
    if (position == view->position) return;
    if (!ac && position == 0)
    {
        battery_set_saver( TRUE );
        view->saver = TRUE;
        view->position = 0;
        return;
    }
    if (view->saver) battery_set_saver( FALSE );
    view->saver = FALSE;
    if (!ac) position--;
    overlay = position == 0 ? overlay_better_battery : position == 2 ? overlay_best : null_guid;
    PowerSetActiveOverlayScheme( &overlay );
    current_overlay = overlay;
    view->position = ac ? position : position + 1;
}

/* Control Panel's Power Options, and its page of the batteries and graphics cards */
void open_power_options(void)
{
    ShellExecuteW( NULL, NULL, L"control.exe", L"powercfg.cpl", NULL, SW_SHOWNORMAL );
}

void open_battery_panel(void)
{
    ShellExecuteW( NULL, NULL, L"control.exe", L"powercfg.cpl,,battery", NULL, SW_SHOWNORMAL );
}
