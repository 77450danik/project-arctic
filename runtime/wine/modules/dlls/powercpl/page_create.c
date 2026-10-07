/*
 * Power Options of the Control Panel: "Створити план живлення"
 *
 * Windows 10's pageCreateNewPlan (powercpl.dll's UIFILE 103): the plan to
 * start from, as radio buttons with their descriptions, "Назва плану:" with
 * "Мій власний план 1" in it, then "Далі", which makes the plan as a copy and
 * opens its settings with "Створити"; "Скасувати" goes back.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdlib.h>

#include "powercpl.h"
#include "commctrl.h"

#define ID_BASE_FIRST    100
#define ID_NAME          150
#define ID_NEXT          151
#define ID_CANCEL        152
#define MAX_PLANS        32

struct create_data
{
    struct plan plans[MAX_PLANS];
    UINT count;
    HFONT bold;
};

static BOOL name_taken( const struct create_data *data, const WCHAR *name )
{
    WCHAR plain[128];
    DWORD size;

    for (UINT i = 0; i < data->count; i++)
    {
        size = sizeof(plain);
        if (!PowerReadFriendlyName( NULL, &data->plans[i].guid, NULL, NULL, (UCHAR *)plain, &size ) &&
            !wcsicmp( plain, name ))
            return TRUE;
        if (!wcsicmp( data->plans[i].name, name )) return TRUE;
    }
    return FALSE;
}

void create_build( struct view *view )
{
    struct page_state *state = view_state( view );
    struct create_data *data = state->data;
    WCHAR name[128];
    HWND edit;
    int width = view_content_width( view );

    if (!data)
    {
        LOGFONTW font;

        if (!(data = state->data = calloc( 1, sizeof(*data) ))) return;
        GetObjectW( view_font( STYLE_BOLD ), sizeof(font), &font );
        data->bold = CreateFontIndirectW( &font );
    }
    data->count = plans_list( data->plans, MAX_PLANS );

    view_text( view, STYLE_TITLE, load_string( IDS_CREATE_TITLE ) );
    view_text( view, STYLE_BODY, load_string( IDS_CREATE_TEXT ) );
    view_space( view, px( 18 ) );

    for (UINT i = 0; i < data->count; i++)
    {
        HWND radio;

        if (!data->plans[i].builtin) continue;
        radio = view_control( view, WC_BUTTONW, data->plans[i].name, BS_AUTORADIOBUTTON | WS_TABSTOP | (i ? 0 : WS_GROUP),
                              width, px( 20 ), 0, ID_BASE_FIRST + i );
        SendMessageW( radio, WM_SETFONT, (WPARAM)data->bold, FALSE );
        if (IsEqualGUID( &data->plans[i].guid, &scheme_balanced )) SendMessageW( radio, BM_SETCHECK, BST_CHECKED, 0 );
        if (data->plans[i].description[0]) view_text_at( view, STYLE_BODY, data->plans[i].description, px( 18 ) );
        view_space( view, px( 12 ) );
    }
    view_space( view, px( 12 ) );

    view_text( view, STYLE_BODY, load_string( IDS_PLAN_NAME ) );
    view_space( view, px( 4 ) );
    edit = view_control( view, WC_EDITW, L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, px( 300 ), px( 22 ), 0, ID_NAME );
    for (UINT n = 1; n < 100; n++)
    {
        lstrcpynW( name, format_string( IDS_MY_PLAN, n ), ARRAY_SIZE(name) );
        if (!name_taken( data, name )) break;
    }
    SetWindowTextW( edit, name );
    SendMessageW( edit, EM_LIMITTEXT, 120, 0 );
    view_space( view, px( 28 ) );

    view_row_begin( view );
    view_control_beside( view, WC_BUTTONW, load_string( IDS_NEXT ), BS_DEFPUSHBUTTON | WS_TABSTOP,
                         width - px( 2 * 98 + 8 ), px( 98 ), px( 24 ), ID_NEXT );
    view_control_beside( view, WC_BUTTONW, load_string( IDS_CANCEL ), BS_PUSHBUTTON | WS_TABSTOP, width - px( 98 ),
                         px( 98 ), px( 24 ), ID_CANCEL );
    view_row_end( view );
}

static void next( struct view *view )
{
    struct create_data *data = view_state( view )->data;
    HWND content = view_window( view ), owner = GetAncestor( content, GA_ROOT );
    WCHAR name[128];
    const GUID *base = &scheme_balanced;
    GUID *made = NULL;

    GetWindowTextW( GetDlgItem( content, ID_NAME ), name, ARRAY_SIZE(name) );
    if (!name[0])
    {
        MessageBoxW( owner, load_string( IDS_NAME_EMPTY ), load_string( IDS_POWER_OPTIONS ), MB_ICONWARNING );
        return;
    }
    if (name_taken( data, name ))
    {
        MessageBoxW( owner, load_string( IDS_NAME_TAKEN ), load_string( IDS_POWER_OPTIONS ), MB_ICONWARNING );
        return;
    }
    for (UINT i = 0; i < data->count; i++)
        if (SendMessageW( GetDlgItem( content, ID_BASE_FIRST + i ), BM_GETCHECK, 0, 0 ) == BST_CHECKED)
            base = &data->plans[i].guid;
    if (PowerDuplicateScheme( NULL, base, &made )) return;
    PowerWriteFriendlyName( NULL, made, NULL, NULL, (UCHAR *)name, (wcslen( name ) + 1) * sizeof(WCHAR) );
    view_navigate( view, PAGE_EDIT, made, TRUE );
    LocalFree( made );
}

BOOL create_command( struct view *view, UINT id, UINT code, HWND control )
{
    if (code != BN_CLICKED) return FALSE;
    switch (id)
    {
    case ID_NEXT:
        next( view );
        return TRUE;
    case ID_CANCEL:
        view_navigate( view, PAGE_PLANS, NULL, FALSE );
        return TRUE;
    }
    return FALSE;
}
