/*
 * Power Options of the Control Panel: "Змінити настройки плану: ..."
 *
 * Windows 10's pagePlanSettings (powercpl.dll's UIFILE 102): on a laptop
 * two columns, "Робота від акумулятора" and "Від мережі", with a row each
 * for turning the display off, putting the computer to sleep and the plan's
 * brightness; on a desktop one column. Under them "Змінити додаткові
 * настройки живлення" (the Advanced settings dialog), "Відновити значення за
 * замовчуванням для плану", "Видалити план" for a plan the user made that is
 * not active; "Зберегти зміни" and "Скасувати" at the bottom right. A plan
 * being made (Створення плану живлення, "Далі") has "Створити" instead.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdlib.h>

#include "powercpl.h"
#include "commctrl.h"
#include "winternl.h"
#include "powrprof.h"

#define ID_DISPLAY       200   /* + column */
#define ID_SLEEP         210
#define ID_BRIGHT        220
#define ID_SAVE          230
#define ID_CANCEL        231

#define LABEL_WIDTH      px(220)
#define COLUMN_WIDTH     px(150)

static const DWORD timeouts[] = { 60, 120, 180, 300, 600, 900, 1200, 1500, 1800, 2700, 3600, 7200, 10800, 14400, 18000, 0 };

static int columns(void)
{
    return on_battery_machine() ? 2 : 1;
}

/* column 0 is on battery when there are two, the mains otherwise */
static BOOL column_ac( int column )
{
    return columns() == 1 || column == 1;
}

static void fill_timeouts( HWND combo, DWORD current )
{
    WCHAR text[64];
    BOOL found = FALSE;
    int index;

    for (UINT i = 0; i < ARRAY_SIZE(timeouts); i++)
    {
        /* a value the list lacks goes in its place, as Windows shows it */
        if (!found && current && timeouts[i] && current < timeouts[i])
        {
            format_timeout( current, text, ARRAY_SIZE(text) );
            index = SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)text );
            SendMessageW( combo, CB_SETITEMDATA, index, current );
            SendMessageW( combo, CB_SETCURSEL, index, 0 );
            found = TRUE;
        }
        if (!found && !timeouts[i] && current)
        {
            format_timeout( current, text, ARRAY_SIZE(text) );
            index = SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)text );
            SendMessageW( combo, CB_SETITEMDATA, index, current );
            SendMessageW( combo, CB_SETCURSEL, index, 0 );
            found = TRUE;
        }
        format_timeout( timeouts[i], text, ARRAY_SIZE(text) );
        index = SendMessageW( combo, CB_ADDSTRING, 0, (LPARAM)text );
        SendMessageW( combo, CB_SETITEMDATA, index, timeouts[i] );
        if (timeouts[i] == current)
        {
            SendMessageW( combo, CB_SETCURSEL, index, 0 );
            found = TRUE;
        }
    }
}

static void advanced( struct view *view, UINT_PTR param )
{
    open_advanced_settings( GetAncestor( view_window( view ), GA_ROOT ), &view_state( view )->scheme );
    view_rebuild( view );
}

static void restore( struct view *view, UINT_PTR param )
{
    WCHAR text[1024];

    swprintf( text, ARRAY_SIZE(text), L"%s\n\n%s", load_string( IDS_RESTORE_CONFIRM ), load_string( IDS_RESTORE_CONFIRM_TEXT ) );
    if (MessageBoxW( GetAncestor( view_window( view ), GA_ROOT ), text, load_string( IDS_POWER_OPTIONS ),
                     MB_YESNO | MB_ICONWARNING ) != IDYES)
        return;
    PowerRestoreIndividualDefaultPowerScheme( &view_state( view )->scheme );
    view_rebuild( view );
}

static void delete_plan( struct view *view, UINT_PTR param )
{
    WCHAR text[1024];

    swprintf( text, ARRAY_SIZE(text), L"%s\n\n%s", load_string( IDS_DELETE_CONFIRM ), load_string( IDS_DELETE_CONFIRM_TEXT ) );
    if (MessageBoxW( GetAncestor( view_window( view ), GA_ROOT ), text, load_string( IDS_POWER_OPTIONS ),
                     MB_YESNO | MB_ICONWARNING ) != IDYES)
        return;
    PowerDeleteScheme( NULL, &view_state( view )->scheme );
    view_navigate( view, PAGE_PLANS, NULL, FALSE );
}

static void add_timeout_row( struct view *view, const WCHAR *label, const GUID *sub, const GUID *setting, UINT id )
{
    struct page_state *state = view_state( view );

    view_row_begin( view );
    view_text_at( view, STYLE_BODY, label, px( 4 ) );
    for (int c = 0; c < columns(); c++)
    {
        HWND combo = view_control_beside( view, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                                          LABEL_WIDTH + c * COLUMN_WIDTH, COLUMN_WIDTH - px( 20 ), px( 240 ), id + c );
        fill_timeouts( combo, plan_value( &state->scheme, sub, setting, column_ac( c ), 0 ) );
    }
    view_row_end( view );
    view_space( view, px( 10 ) );
}

void edit_build( struct view *view )
{
    struct page_state *state = view_state( view );
    struct arctic_cpu_info cpu;
    WCHAR name[128];
    DWORD size = sizeof(name);
    GUID active;
    int width = view_content_width( view );

    if (PowerReadFriendlyName( NULL, &state->scheme, NULL, NULL, (UCHAR *)name, &size )) name[0] = 0;
    view_text( view, STYLE_TITLE, format_string( IDS_EDIT_TITLE, name ) );
    view_text( view, STYLE_BODY, load_string( IDS_EDIT_TEXT ) );
    view_space( view, px( 20 ) );

    if (columns() == 2)
    {
        view_row_begin( view );
        view_text_at( view, STYLE_BODY, L" ", 0 );
        view_text_at( view, STYLE_BODY, load_string( IDS_BATTERY_COLUMN ), LABEL_WIDTH );
        view_text_at( view, STYLE_BODY, load_string( IDS_AC_COLUMN ), LABEL_WIDTH + COLUMN_WIDTH );
        view_row_end( view );
        view_space( view, px( 10 ) );
    }
    add_timeout_row( view, load_string( IDS_TURN_OFF_DISPLAY ), &sub_video, &set_video_idle, ID_DISPLAY );
    if (IsPwrSuspendAllowed()) add_timeout_row( view, load_string( IDS_PUT_TO_SLEEP ), &sub_sleep, &set_standby, ID_SLEEP );

    if (ArcticCpuInfo( &cpu ) && cpu.backlight)
    {
        view_row_begin( view );
        view_text_at( view, STYLE_BODY, load_string( IDS_ADJUST_BRIGHTNESS ), px( 4 ) );
        for (int c = 0; c < columns(); c++)
        {
            HWND slider = view_control_beside( view, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP,
                                               LABEL_WIDTH + c * COLUMN_WIDTH, COLUMN_WIDTH - px( 20 ), px( 26 ),
                                               ID_BRIGHT + c );
            SendMessageW( slider, TBM_SETRANGE, TRUE, MAKELPARAM( 0, 100 ) );
            SendMessageW( slider, TBM_SETPAGESIZE, 0, 10 );
            SendMessageW( slider, TBM_SETPOS, TRUE,
                          plan_value( &state->scheme, &sub_video, &set_brightness, column_ac( c ), cpu.brightness_percent ) );
        }
        view_row_end( view );
    }
    view_space( view, px( 24 ) );

    view_link( view, load_string( IDS_ADVANCED_LINK ), advanced, 0, 0 );
    view_space( view, px( 8 ) );
    if (!state->creating)
    {
        view_link( view, load_string( IDS_RESTORE_PLAN_LINK ), restore, 0, 0 );
        view_space( view, px( 8 ) );
        /* only a plan the user made, and not the active one */
        if (PowerCanRestoreIndividualDefaultPowerScheme( &state->scheme ) &&
            (!plan_active( &active ) || !IsEqualGUID( &active, &state->scheme )))
        {
            view_link( view, load_string( IDS_DELETE_PLAN_LINK ), delete_plan, 0, 0 );
            view_space( view, px( 8 ) );
        }
    }
    view_space( view, px( 24 ) );

    view_row_begin( view );
    view_control_beside( view, WC_BUTTONW, load_string( state->creating ? IDS_CREATE : IDS_SAVE_CHANGES ),
                         BS_DEFPUSHBUTTON | WS_TABSTOP, width - px( 2 * 98 + 8 ), px( 98 ), px( 24 ), ID_SAVE );
    view_control_beside( view, WC_BUTTONW, load_string( IDS_CANCEL ), BS_PUSHBUTTON | WS_TABSTOP, width - px( 98 ),
                         px( 98 ), px( 24 ), ID_CANCEL );
    view_row_end( view );
}

static DWORD combo_value( HWND combo )
{
    int index = SendMessageW( combo, CB_GETCURSEL, 0, 0 );

    return index < 0 ? ~0u : SendMessageW( combo, CB_GETITEMDATA, index, 0 );
}

static void save( struct view *view )
{
    struct page_state *state = view_state( view );
    HWND content = view_window( view );

    for (int c = 0; c < columns(); c++)
    {
        HWND control;
        DWORD value;

        if ((control = GetDlgItem( content, ID_DISPLAY + c )) && (value = combo_value( control )) != ~0u)
            plan_set_value( &state->scheme, &sub_video, &set_video_idle, column_ac( c ), value );
        if ((control = GetDlgItem( content, ID_SLEEP + c )) && (value = combo_value( control )) != ~0u)
            plan_set_value( &state->scheme, &sub_sleep, &set_standby, column_ac( c ), value );
        if ((control = GetDlgItem( content, ID_BRIGHT + c )))
            plan_set_value( &state->scheme, &sub_video, &set_brightness, column_ac( c ),
                            SendMessageW( control, TBM_GETPOS, 0, 0 ) );
    }
    /* a plan just made becomes the active one, as in Windows */
    if (state->creating) PowerSetActiveScheme( NULL, &state->scheme );
}

BOOL edit_command( struct view *view, UINT id, UINT code, HWND control )
{
    struct page_state *state = view_state( view );

    switch (id)
    {
    case ID_SAVE:
        if (code != BN_CLICKED) return FALSE;
        save( view );
        view_navigate( view, PAGE_PLANS, NULL, FALSE );
        return TRUE;
    case ID_CANCEL:
        if (code != BN_CLICKED) return FALSE;
        if (state->creating) PowerDeleteScheme( NULL, &state->scheme );
        view_navigate( view, PAGE_PLANS, NULL, FALSE );
        return TRUE;
    }
    return FALSE;
}
