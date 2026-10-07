/*
 * Power Options of the Control Panel: the plans
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "powercpl.h"
#include "winternl.h"
#include "powrprof.h"
#include "shellapi.h"

const GUID sub_none     = { 0xfea3413e, 0x7e05, 0x4911, { 0x9a, 0x71, 0x70, 0x03, 0x31, 0xf1, 0xc2, 0x94 } };
const GUID sub_video    = { 0x7516b95f, 0xf776, 0x4464, { 0x8c, 0x53, 0x06, 0x16, 0x7f, 0x40, 0xcc, 0x99 } };
const GUID sub_sleep    = { 0x238c9fa8, 0x0aad, 0x41ed, { 0x83, 0xf4, 0x97, 0xbe, 0x24, 0x2c, 0x8f, 0x20 } };
const GUID sub_buttons  = { 0x4f971e89, 0xeebd, 0x4455, { 0xa8, 0xde, 0x9e, 0x59, 0x04, 0x0e, 0x73, 0x47 } };
const GUID sub_battery  = { 0xe73a048d, 0xbf27, 0x4f12, { 0x97, 0x31, 0x8b, 0x20, 0x76, 0xe8, 0x89, 0x1f } };
const GUID set_video_idle     = { 0x3c0bc021, 0xc8a8, 0x4e07, { 0xa9, 0x73, 0x6b, 0x14, 0xcb, 0xcb, 0x2b, 0x7e } };
const GUID set_brightness     = { 0xaded5e82, 0xb909, 0x4619, { 0x99, 0x49, 0xf5, 0xd7, 0x1d, 0xac, 0x0b, 0xcb } };
const GUID set_standby        = { 0x29f6c1db, 0x86da, 0x48c5, { 0x9f, 0xdb, 0xf2, 0xb6, 0x7b, 0x1f, 0x44, 0xda } };
const GUID set_hibernate_idle = { 0x9d7815a6, 0x7ee4, 0x497e, { 0x88, 0x88, 0x51, 0x5a, 0x05, 0xf0, 0x23, 0x64 } };
const GUID set_lid            = { 0x5ca83367, 0x6e45, 0x459f, { 0xa2, 0x7b, 0x47, 0x6b, 0x1d, 0x01, 0xc9, 0x36 } };
const GUID set_pbutton        = { 0x7648efa3, 0xdd9c, 0x4e3e, { 0xb5, 0x66, 0x50, 0xf9, 0x29, 0x38, 0x62, 0x80 } };
const GUID set_sbutton        = { 0x96996bc0, 0xad50, 0x47ec, { 0x92, 0x3b, 0x6f, 0x41, 0x87, 0x4d, 0xd9, 0xeb } };
const GUID scheme_balanced = { 0x381b4222, 0xf694, 0x41f0, { 0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e } };
const GUID scheme_max      = { 0x8c5e7fda, 0xe8bf, 0x4a96, { 0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c } };
const GUID scheme_min      = { 0xa1841308, 0x3541, 0x4fab, { 0xbc, 0x81, 0xf7, 0x15, 0x56, 0xf2, 0x0b, 0x4a } };

static BOOL is_builtin( const GUID *guid )
{
    return IsEqualGUID( guid, &scheme_balanced ) || IsEqualGUID( guid, &scheme_max ) || IsEqualGUID( guid, &scheme_min );
}

BOOL plan_active( GUID *guid )
{
    GUID *active;

    if (PowerGetActiveScheme( NULL, &active )) return FALSE;
    *guid = *active;
    LocalFree( active );
    return TRUE;
}

/* "Збалансований (рекомендовано)", as the list of plans shows it */
BOOL plan_name( const GUID *guid, WCHAR *name, DWORD count )
{
    WCHAR text[128];
    DWORD size = sizeof(text);

    if (PowerReadFriendlyName( NULL, guid, NULL, NULL, (UCHAR *)text, &size )) return FALSE;
    if (IsEqualGUID( guid, &scheme_balanced )) lstrcpynW( name, format_string( IDS_RECOMMENDED, text ), count );
    else lstrcpynW( name, text, count );
    return TRUE;
}

UINT plans_list( struct plan *plans, UINT max )
{
    GUID active, guid;
    DWORD size;
    UINT count = 0;

    plan_active( &active );
    for (ULONG i = 0; count < max; i++)
    {
        size = sizeof(guid);
        if (PowerEnumerate( NULL, NULL, NULL, ACCESS_SCHEME, i, (UCHAR *)&guid, &size )) break;
        plans[count].guid = guid;
        if (!plan_name( &guid, plans[count].name, ARRAY_SIZE(plans[count].name) )) continue;
        size = sizeof(plans[count].description);
        if (PowerReadDescription( NULL, &guid, NULL, NULL, (UCHAR *)plans[count].description, &size ))
            plans[count].description[0] = 0;
        plans[count].builtin = is_builtin( &guid );
        plans[count].active = IsEqualGUID( &guid, &active );
        count++;
    }
    return count;
}

BOOL on_battery_machine(void)
{
    struct arctic_power_status status;

    return ArcticPowerStatus( &status ) && status.battery_count;
}

BOOL lid_present(void)
{
    struct arctic_power_status status;

    return ArcticPowerStatus( &status ) && status.lid_present;
}

DWORD plan_value( const GUID *scheme, const GUID *sub, const GUID *setting, BOOL ac, DWORD fallback )
{
    DWORD value;

    if ((ac ? PowerReadACValueIndex : PowerReadDCValueIndex)( NULL, scheme, sub, setting, &value )) return fallback;
    return value;
}

void plan_set_value( const GUID *scheme, const GUID *sub, const GUID *setting, BOOL ac, DWORD value )
{
    (ac ? PowerWriteACValueIndex : PowerWriteDCValueIndex)( NULL, scheme, sub, setting, value );
}

/* "5 хв.", "1 год.", "2 год. 30 хв.", "Ніколи" */
void format_timeout( DWORD seconds, WCHAR *text, size_t count )
{
    DWORD hours = seconds / 3600, minutes = seconds % 3600 / 60;

    if (!seconds) lstrcpynW( text, load_string( IDS_NEVER ), count );
    else if (seconds < 60) lstrcpynW( text, load_string( IDS_LESS_THAN_MINUTE ), count );
    else if (!hours) lstrcpynW( text, minutes == 1 ? load_string( IDS_ONE_MIN ) : format_string( IDS_MINUTES, 0, minutes ), count );
    else if (!minutes) lstrcpynW( text, hours == 1 ? load_string( IDS_ONE_HOUR ) : format_string( IDS_HOURS, hours ), count );
    else lstrcpynW( text, format_string( IDS_HOURS_MINUTES, hours, minutes ), count );
}

void open_control_panel( const WCHAR *args )
{
    ShellExecuteW( NULL, NULL, L"control.exe", args, NULL, SW_SHOWNORMAL );
}

/**********************************************************************
 *          The task pane
 */

void view_browse_control_panel( struct view *view );

static void go_home( struct view *view, UINT_PTR param )
{
    view_browse_control_panel( view );
}

static void go_page( struct view *view, UINT_PTR page )
{
    GUID active;

    if (page == PAGE_EDIT && plan_active( &active )) view_navigate( view, PAGE_EDIT, &active, FALSE );
    else view_navigate( view, page, NULL, FALSE );
}

static void run_cpl( struct view *view, UINT_PTR which )
{
    switch (which)
    {
    case 0: open_control_panel( L"desk.cpl" ); break;
    case 1: open_control_panel( L"nusrmgr.cpl" ); break;
    }
}

/* the links Windows 10 has on Power Options' left, and Arctic's two pages */
void nav_power_tasks( struct view *view )
{
    BOOL laptop = on_battery_machine();

    view_nav_link( view, load_string( IDS_CONTROL_PANEL_HOME ), go_home, 0 );
    view_nav_link( view, load_string( IDS_TASK_BUTTONS ), go_page, PAGE_SYSTEM );
    if (lid_present()) view_nav_link( view, load_string( IDS_TASK_LID ), go_page, PAGE_SYSTEM );
    view_nav_link( view, load_string( IDS_TASK_CREATE ), go_page, PAGE_CREATE );
    view_nav_link( view, load_string( IDS_TASK_DISPLAY ), go_page, PAGE_EDIT );
    view_nav_link( view, load_string( IDS_TASK_SLEEP ), go_page, PAGE_EDIT );
    view_nav_link( view, load_string( IDS_TASK_BATTERY ), go_page, PAGE_BATTERY );
    view_nav_link( view, load_string( IDS_TASK_GRAPHICS ), go_page, PAGE_GRAPHICS );

    view_nav_see_also( view, load_string( IDS_SEE_PERSONALIZATION ), run_cpl, 0 );
    if (laptop) view_nav_see_also( view, load_string( IDS_SEE_MOBILITY ), go_page, PAGE_BATTERY );
    view_nav_see_also( view, load_string( IDS_SEE_ACCOUNTS ), run_cpl, 1 );
}
