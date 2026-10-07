/*
 * Power Options of the Control Panel: "Вибір і настроювання плану живлення"
 *
 * Windows 10's pagePowerPlans (powercpl.dll's UIFILE 101 with the plan of
 * UIFILE 110): the title and its text; the plans shown on the battery meter
 * (on a desktop, the preferred plans) as radio buttons in bold, each with
 * "Змінити настройки плану" to its right and its description under it; the
 * other plans behind "Відобразити додаткові плани"; on a laptop the screen's
 * brightness at the bottom. Choosing a plan makes it active at once.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdlib.h>

#include "powercpl.h"
#include "commctrl.h"

#define ID_PLAN_FIRST    100
#define ID_BRIGHTNESS    90
#define MAX_PLANS        32

struct plans_data
{
    struct plan plans[MAX_PLANS];
    UINT count;
    HFONT bold;
};

static void change_plan( struct view *view, UINT_PTR index )
{
    struct plans_data *data = view_state( view )->data;

    if (data && index < data->count) view_navigate( view, PAGE_EDIT, &data->plans[index].guid, FALSE );
}

static void toggle_more( struct view *view, UINT_PTR param )
{
    view_state( view )->more_plans = !view_state( view )->more_plans;
    view_rebuild( view );
}

static BOOL favorite( const struct plan *plan )
{
    /* on the battery meter: Balanced, the plans the user made, and the active one */
    return !plan->builtin || plan->active || IsEqualGUID( &plan->guid, &scheme_balanced );
}

static void add_plan( struct view *view, struct plans_data *data, UINT index )
{
    const struct plan *plan = &data->plans[index];
    HDC hdc = GetDC( NULL );
    HGDIOBJ old = SelectObject( hdc, data->bold );
    SIZE size = { 0 };
    int width;
    HWND radio;

    GetTextExtentPoint32W( hdc, plan->name, lstrlenW( plan->name ), &size );
    SelectObject( hdc, old );
    ReleaseDC( NULL, hdc );
    /* the radio button as wide as its name; "Змінити настройки плану" in a
     * column to the right of the names, as Windows lines them up */
    width = min( size.cx + px( 24 ), px( 300 ) );
    view_row_begin( view );
    radio = view_control_beside( view, L"BUTTON", plan->name, BS_AUTORADIOBUTTON | WS_TABSTOP, 0, width, px( 20 ),
                                 ID_PLAN_FIRST + index );
    SendMessageW( radio, WM_SETFONT, (WPARAM)data->bold, FALSE );
    SendMessageW( radio, BM_SETCHECK, plan->active ? BST_CHECKED : BST_UNCHECKED, 0 );
    view_link( view, load_string( IDS_CHANGE_PLAN ), change_plan, index, max( width + px( 16 ), px( 310 ) ) );
    view_row_end( view );
    if (plan->description[0]) view_text_at( view, STYLE_BODY, plan->description, px( 18 ) );
    view_space( view, px( 12 ) );
}

static void brightness_paint( struct view *view, HDC hdc, const RECT *rect )
{
}

void plans_build( struct view *view )
{
    struct page_state *state = view_state( view );
    struct plans_data *data = state->data;
    struct arctic_cpu_info cpu;
    BOOL laptop = on_battery_machine(), more = FALSE;

    if (!data)
    {
        LOGFONTW font;

        if (!(data = state->data = calloc( 1, sizeof(*data) ))) return;
        GetObjectW( view_font( STYLE_BOLD ), sizeof(font), &font );
        data->bold = CreateFontIndirectW( &font );
    }
    data->count = plans_list( data->plans, MAX_PLANS );

    nav_power_tasks( view );
    view_text( view, STYLE_TITLE, load_string( IDS_PLANS_TITLE ) );
    {
        /* Windows' text, its "Докладніше" link as words of it */
        WCHAR text[1024], *p, *q;

        lstrcpynW( text, load_string( IDS_PLANS_TEXT ), ARRAY_SIZE(text) );
        while ((p = wcschr( text, '<' )) && (q = wcschr( p, '>' ))) memmove( p, q + 1, (wcslen( q + 1 ) + 1) * sizeof(WCHAR) );
        view_text( view, STYLE_BODY, text );
    }
    view_space( view, px( 18 ) );

    view_group( view, load_string( laptop ? IDS_BATTERY_METER_PLANS : IDS_PREFERRED_PLANS ) );
    view_space( view, px( 8 ) );
    for (UINT i = 0; i < data->count; i++)
    {
        if (favorite( &data->plans[i] )) add_plan( view, data, i );
        else more = TRUE;
    }

    if (more)
    {
        view_link( view, load_string( state->more_plans ? IDS_HIDE_MORE_PLANS : IDS_SHOW_MORE_PLANS ), toggle_more, 0,
                   0 );
        view_space( view, px( 12 ) );
        if (state->more_plans)
            for (UINT i = 0; i < data->count; i++)
                if (!favorite( &data->plans[i] )) add_plan( view, data, i );
    }

    /* the brightness of the panel, for the power source now: the plan's own value */
    if (ArcticCpuInfo( &cpu ) && cpu.backlight)
    {
        HWND slider;

        view_space( view, px( 24 ) );
        view_row_begin( view );
        view_text_at( view, STYLE_BODY, load_string( IDS_SCREEN_BRIGHTNESS ), 0 );
        slider = view_control_beside( view, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, px( 130 ),
                                      px( 220 ), px( 26 ), ID_BRIGHTNESS );
        view_row_end( view );
        SendMessageW( slider, TBM_SETRANGE, TRUE, MAKELPARAM( 0, 100 ) );
        SendMessageW( slider, TBM_SETPAGESIZE, 0, 10 );
        SendMessageW( slider, TBM_SETPOS, TRUE, cpu.brightness_percent );
    }
    view_paint_area( view, 0, brightness_paint );
}

BOOL plans_command( struct view *view, UINT id, UINT code, HWND control )
{
    struct plans_data *data = view_state( view )->data;

    if (id >= ID_PLAN_FIRST && id < ID_PLAN_FIRST + MAX_PLANS && code == BN_CLICKED && data)
    {
        UINT index = id - ID_PLAN_FIRST;

        if (index < data->count && PowerSetActiveScheme( NULL, &data->plans[index].guid ))
            MessageBoxW( GetAncestor( control, GA_ROOT ), load_string( IDS_ACTIVATE_FAILED ),
                         load_string( IDS_POWER_OPTIONS ), MB_ICONERROR );
        /* only one radio button checked, whichever group it sits in */
        for (UINT i = 0; i < data->count; i++)
        {
            HWND radio = GetDlgItem( view_window( view ), ID_PLAN_FIRST + i );
            if (radio) SendMessageW( radio, BM_SETCHECK, i == index ? BST_CHECKED : BST_UNCHECKED, 0 );
        }
        return TRUE;
    }
    if (id == ID_BRIGHTNESS && (code == TB_THUMBTRACK || code == TB_ENDTRACK || code == TB_LINEUP ||
                                code == TB_LINEDOWN || code == TB_PAGEUP || code == TB_PAGEDOWN ||
                                code == TB_THUMBPOSITION || code == TB_TOP || code == TB_BOTTOM))
    {
        struct arctic_power_apply apply;
        struct arctic_power_status status;
        DWORD level = SendMessageW( control, TBM_GETPOS, 0, 0 );
        GUID active;

        memset( &apply, 0xff, sizeof(apply) );
        apply.brightness_percent = level;
        ArcticPowerApply( &apply );
        /* the plan keeps it for this power source, as Windows' slider does */
        if (code == TB_ENDTRACK && plan_active( &active ))
            plan_set_value( &active, &sub_video, &set_brightness,
                            !ArcticPowerStatus( &status ) || status.ac_online || !status.battery_count, level );
        return TRUE;
    }
    return FALSE;
}
