/*
 * User Accounts of the Control Panel: "Внесення змін до облікового запису
 * користувача"
 *
 * Windows 7's page: the links on the left, the account's tile on the right
 * with its name, its kind and its type. Arctic has one account, the local
 * administrator every program runs as, so what Windows offers for other
 * accounts and for passwords of accounts is not here; what is here instead is
 * the section of the Linux subsystem, which is what Arctic's User Account
 * Control guards (docs/linux-gate.md). The shield links open
 * UserAccountControlSettings.exe, which asks the host.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "usercpl.h"

#define INDENT      px(20)
#define TILE_SIZE   px(48)

static void open_uac_settings( struct view *view, UINT_PTR param )
{
    cp_run( L"UserAccountControlSettings.exe", NULL );
}

/* the account's tile, where Windows 7 puts the user's picture */
static void paint_tile( struct view *view, HDC hdc, const RECT *rect, UINT_PTR param )
{
    HICON icon = LoadImageW( cp_instance, (const WCHAR *)MAKEINTRESOURCE(IDI_USERS), IMAGE_ICON, TILE_SIZE, TILE_SIZE,
                             LR_SHARED );

    if (icon) DrawIconEx( hdc, rect->right - TILE_SIZE, rect->top, icon, TILE_SIZE, TILE_SIZE, 0, NULL, DI_NORMAL );
}

void users_build( struct view *view )
{
    WCHAR name[256] = L"";
    DWORD size = ARRAY_SIZE(name);

    view_nav_home( view );
    view_nav_icon_link( view, cp_shield_icon(), load_string( IDS_UAC_SETTINGS ), open_uac_settings, 0 );

    view_text( view, STYLE_TITLE, load_string( IDS_TITLE ) );
    view_space( view, px( 12 ) );

    /* the account: its tile on the right, its lines beside it */
    view_paint_area( view, 0, paint_tile, 0 );
    if (!GetUserNameW( name, &size ) || !name[0]) lstrcpyW( name, L"User" );
    view_text_at( view, STYLE_BOLD, name, INDENT );
    view_space( view, px( 6 ) );
    view_text_at( view, STYLE_BODY, load_string( IDS_LOCAL_ACCOUNT ), INDENT );
    view_space( view, px( 6 ) );
    view_text_at( view, STYLE_BODY, load_string( IDS_ADMINISTRATOR ), INDENT );
    view_space( view, px( 6 ) );
    view_text_at( view, STYLE_GRAY, load_string( IDS_ONE_ACCOUNT ), INDENT );
    view_space( view, max( 0, TILE_SIZE - px( 60 ) ) + px( 16 ) );

    /* Підсистема Windows для Linux: what the consent window is asked for */
    view_group( view, load_string( IDS_LINUX_GROUP ) );
    view_text_at( view, STYLE_BODY, load_string( IDS_LINUX_ABOUT ), INDENT );
    view_space( view, px( 10 ) );
    view_icon_link( view, cp_shield_icon(), load_string( IDS_LINUX_PASSWORD ), open_uac_settings, 0, INDENT, FALSE );
    view_space( view, px( 10 ) );
    view_icon_link( view, cp_shield_icon(), load_string( IDS_UAC_SETTINGS ), open_uac_settings, 0, INDENT, FALSE );
}
