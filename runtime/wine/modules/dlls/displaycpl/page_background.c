/*
 * Display of the Control Panel: the page Background
 *
 * Windows 11's Personalization, Background: the desktop in small as it is;
 * a picture or a solid colour; the pictures chosen last and Windows' own,
 * any other one from a file; how the picture fills the desktop (desk.cpl's
 * Fill, Fit, Stretch, Tile and Center) and the colour around it; the colours
 * to choose from and any other one.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdio.h>

#include "displaycpl.h"
#include "commdlg.h"
#include "shlobj.h"

#define ID_PREVIEW      100
#define ID_KIND         101
#define ID_BROWSE       102
#define ID_FIT          103
#define ID_BACK_COLOR   104
#define ID_CUSTOM_COLOR 105
#define ID_PICTURE_FIRST 120    /* + the picture in the list */
#define ID_COLOR_FIRST  160     /* + the colour */

#define ID_DO_KIND      200
#define ID_DO_FIT       201
#define ID_DO_BROWSE    202
#define ID_DO_BACK_COLOR 203
#define ID_DO_CUSTOM    204
#define ID_DO_PICTURE   220     /* + the picture */
#define ID_DO_COLOR     260     /* + the colour */

#define MAX_PICTURES    ARRAY_SIZE(((struct display_state *)0)->pictures)

/* the colours of Windows to choose from, for the desktop */
static const COLORREF colors[] =
{
    RGB( 0x00, 0x00, 0x00 ), RGB( 0x1f, 0x1f, 0x1f ), RGB( 0x3a, 0x3a, 0x3a ), RGB( 0x5d, 0x5a, 0x58 ),
    RGB( 0x76, 0x76, 0x76 ), RGB( 0x7a, 0x75, 0x74 ), RGB( 0x00, 0x78, 0xd4 ), RGB( 0x00, 0x63, 0xb1 ),
    RGB( 0x00, 0x4e, 0x8c ), RGB( 0x2d, 0x7d, 0x9a ), RGB( 0x00, 0x99, 0xbc ), RGB( 0x03, 0x83, 0x87 ),
    RGB( 0x00, 0xb2, 0x94 ), RGB( 0x01, 0x85, 0x74 ), RGB( 0x10, 0x7c, 0x10 ), RGB( 0x49, 0x82, 0x05 ),
    RGB( 0x48, 0x68, 0x60 ), RGB( 0x52, 0x5e, 0x54 ), RGB( 0x88, 0x17, 0x98 ), RGB( 0x74, 0x4d, 0xa9 ),
    RGB( 0x87, 0x64, 0xb8 ), RGB( 0xc3, 0x00, 0x52 ), RGB( 0xd1, 0x34, 0x38 ), RGB( 0xca, 0x50, 0x10 ),
};

static BOOL listed( const struct display_state *s, UINT count, const WCHAR *path )
{
    for (UINT k = 0; k < count; k++) if (!wcsicmp( s->pictures[k], path )) return TRUE;
    return FALSE;
}

/* the one shown first, then those chosen before it, then Windows' own */
static void list_pictures( struct display_state *s )
{
    WCHAR (*found)[MAX_PATH] = calloc( MAX_PICTURES, MAX_PATH * sizeof(WCHAR) );
    WCHAR current[MAX_PATH];
    UINT count, n = 0;

    s->picture_count = 0;
    if (!found) return;
    if (wallpaper_current( current, ARRAY_SIZE(current) ) && GetFileAttributesW( current ) != INVALID_FILE_ATTRIBUTES)
        lstrcpynW( s->pictures[n++], current, MAX_PATH );
    count = wallpaper_history( found, 5 );
    for (UINT i = 0; i < count && n < MAX_PICTURES; i++)
        if (!listed( s, n, found[i] )) lstrcpynW( s->pictures[n++], found[i], MAX_PATH );
    count = wallpaper_windows( found, MAX_PICTURES );
    for (UINT i = 0; i < count && n < MAX_PICTURES; i++)
        if (!listed( s, n, found[i] )) lstrcpynW( s->pictures[n++], found[i], MAX_PATH );
    s->picture_count = n;
    free( found );
}

void background_build( struct view *view )
{
    struct display_state *s = display_state( view );
    int pre_h = px( 170 ), pre_w = preview_width( pre_h ), thumb_h = px( 72 ), thumb_w = preview_width( thumb_h );
    int frame = px( 10 ), tw, th, fit;
    WCHAR name[256];
    HWND control, tile;

    if (!s) return;
    nav_display( view, PAGE_BACKGROUND );
    view_heading( view, load_string( IDS_BACKGROUND ) );

    /* the desktop as the background makes it */
    preview_init( &s->preview, pre_w, pre_h );
    s->kind = s->preview.picture ? BACKGROUND_PICTURE : BACKGROUND_COLOR;
    fit = s->preview.fit;
    view_card_area( view, pre_h + frame, NULL, 0 );
    tile = view_tile( view, pre_w + frame, pre_h + frame, NULL, ID_PREVIEW );
    tile_set_paint( tile, preview_paint, (UINT_PTR)&s->preview );
    EnableWindow( tile, FALSE );
    preview_thumb_size( &s->preview, &tw, &th );
    if (s->preview.picture && tw) picture_to_tile( tile, s->preview.path, tw, th, FALSE );

    control = card_combo( view, ICON_BACKGROUND, IDS_BG_KIND, 0, ID_KIND );
    combo_add( control, load_string( IDS_BG_PICTURE ), BACKGROUND_PICTURE, s->kind == BACKGROUND_PICTURE );
    combo_add( control, load_string( IDS_BG_COLOR ), BACKGROUND_COLOR, s->kind == BACKGROUND_COLOR );

    if (s->kind == BACKGROUND_PICTURE)
    {
        lstrcpynW( name, load_string( IDS_BROWSE_TITLE ), ARRAY_SIZE(name) );
        view_card( view, view_icon( ICON_SETTINGS ), name, NULL, L"BUTTON", load_string( IDS_BROWSE ),
                   BS_PUSHBUTTON | WS_TABSTOP, px( 170 ), px( 28 ), ID_BROWSE );
        control = card_combo( view, ICON_RESOLUTION, IDS_FIT_TITLE, 0, ID_FIT );
        for (int i = 0; i < FIT_COUNT; i++) combo_add( control, load_string( IDS_FIT_FILL + i ), i, i == fit );
        /* what shows around a picture that does not cover the desktop */
        if (fit == FIT_FIT || fit == FIT_CENTER || fit == FIT_TILE)
        {
            lstrcpynW( name, load_string( IDS_BACK_COLOR ), ARRAY_SIZE(name) );
            view_card( view, view_icon( ICON_COLORS ), name, NULL, NULL, NULL, 0, 0, 0, 0 );
            tile = view_card_extra( view, L"ArcticTile", L"", WS_TABSTOP, px( 46 ), px( 36 ), ID_BACK_COLOR );
            tile_set_color( tile, s->preview.color );
        }
        list_pictures( s );
        if (!s->picture_count) return;
        view_section( view, load_string( IDS_RECENT ) );
        view_card_flow( view, px( 8 ) );
        for (UINT i = 0; i < s->picture_count; i++)
        {
            tile = view_tile( view, thumb_w + frame, thumb_h + frame, NULL, ID_PICTURE_FIRST + i );
            picture_to_tile( tile, s->pictures[i], thumb_w, thumb_h, TRUE );
            tile_select( tile, s->preview.picture && !wcsicmp( s->pictures[i], s->preview.path ) );
        }
        return;
    }

    view_section( view, load_string( IDS_COLORS_GROUP ) );
    view_card_flow( view, px( 4 ) );
    for (UINT i = 0; i < ARRAY_SIZE(colors); i++)
    {
        tile = view_tile( view, px( 48 ), px( 48 ), NULL, ID_COLOR_FIRST + i );
        tile_set_color( tile, colors[i] );
        tile_select( tile, colors[i] == s->preview.color );
    }
    lstrcpynW( name, load_string( IDS_CUSTOM_COLOR ), ARRAY_SIZE(name) );
    view_card( view, view_icon( ICON_COLORS ), name, NULL, L"BUTTON", load_string( IDS_VIEW_COLORS ),
               BS_PUSHBUTTON | WS_TABSTOP, px( 170 ), px( 28 ), ID_CUSTOM_COLOR );
}

/**********************************************************************
 *          What the page changes
 */

static BOOL choose_color( HWND owner, COLORREF *color )
{
    static COLORREF custom[16];
    CHOOSECOLORW cc = { sizeof(cc) };

    cc.hwndOwner = owner;
    cc.rgbResult = *color;
    cc.lpCustColors = custom;
    cc.Flags = CC_RGBINIT | CC_FULLOPEN | CC_ANYCOLOR;
    if (!ChooseColorW( &cc )) return FALSE;
    *color = cc.rgbResult;
    return TRUE;
}

/* a picture of the user's: from the Pictures folder, as Windows 11 opens it */
static void browse( struct view *view )
{
    static const WCHAR masks[] = L"*.jpg;*.jpeg;*.jfif;*.png;*.bmp;*.dib;*.gif;*.tif;*.tiff;*.webp";
    OPENFILENAMEW ofn = { sizeof(ofn) };
    WCHAR file[MAX_PATH] = L"", dir[MAX_PATH] = L"", filter[512], title[128];
    int len;

    /* "Зображення" \0 the masks \0 \0 */
    lstrcpynW( filter, load_string( IDS_PICTURES_FILTER ), 200 );
    len = lstrlenW( filter ) + 1;
    lstrcpyW( filter + len, masks );
    len += lstrlenW( masks ) + 1;
    filter[len] = 0;
    lstrcpynW( title, load_string( IDS_BROWSE_DIALOG ), ARRAY_SIZE(title) );
    SHGetFolderPathW( NULL, CSIDL_MYPICTURES, NULL, 0, dir );
    ofn.hwndOwner = GetAncestor( view_window( view ), GA_ROOT );
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = ARRAY_SIZE(file);
    ofn.lpstrInitialDir = dir[0] ? dir : NULL;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
    if (GetOpenFileNameW( &ofn )) wallpaper_set( file, -1 );
}

BOOL background_command( struct view *view, UINT id, UINT code, HWND control )
{
    struct display_state *s = display_state( view );
    HWND content = view_window( view );

    if (!s) return FALSE;
    if (id >= ID_PICTURE_FIRST && id < ID_PICTURE_FIRST + MAX_PICTURES)
    {
        if (code == BN_CLICKED) defer_command( view, ID_DO_PICTURE + id - ID_PICTURE_FIRST );
        return TRUE;
    }
    if (id >= ID_COLOR_FIRST && id < ID_COLOR_FIRST + ARRAY_SIZE(colors))
    {
        if (code == BN_CLICKED) defer_command( view, ID_DO_COLOR + id - ID_COLOR_FIRST );
        return TRUE;
    }
    if (id >= ID_DO_PICTURE && id < ID_DO_PICTURE + MAX_PICTURES)
    {
        UINT i = id - ID_DO_PICTURE;

        if (i < s->picture_count) wallpaper_set( s->pictures[i], -1 );
        view_rebuild( view );
        return TRUE;
    }
    if (id >= ID_DO_COLOR && id < ID_DO_COLOR + ARRAY_SIZE(colors))
    {
        desktop_color_set( colors[id - ID_DO_COLOR], TRUE );
        view_rebuild( view );
        return TRUE;
    }
    switch (id)
    {
    case ID_KIND:
        if (code == CBN_SELCHANGE) defer_command( view, ID_DO_KIND );
        return TRUE;
    case ID_FIT:
        if (code == CBN_SELCHANGE) defer_command( view, ID_DO_FIT );
        return TRUE;
    case ID_BROWSE:
        if (code == BN_CLICKED) defer_command( view, ID_DO_BROWSE );
        return TRUE;
    case ID_BACK_COLOR:
        if (code == BN_CLICKED) defer_command( view, ID_DO_BACK_COLOR );
        return TRUE;
    case ID_CUSTOM_COLOR:
        if (code == BN_CLICKED) defer_command( view, ID_DO_CUSTOM );
        return TRUE;

    case ID_DO_KIND:
    {
        LPARAM kind = combo_data( GetDlgItem( content, ID_KIND ) );

        if (kind == BACKGROUND_COLOR && s->kind != BACKGROUND_COLOR)
            desktop_color_set( desktop_color(), TRUE );
        else if (kind == BACKGROUND_PICTURE && s->kind != BACKGROUND_PICTURE)
        {
            /* the picture chosen last, as Windows 11 takes it back */
            list_pictures( s );
            if (s->picture_count) wallpaper_set( s->pictures[0], -1 );
        }
        view_rebuild( view );
        return TRUE;
    }
    case ID_DO_FIT:
    {
        LPARAM fit = combo_data( GetDlgItem( content, ID_FIT ) );

        if (fit != -1) wallpaper_set( NULL, (int)fit );
        view_rebuild( view );
        return TRUE;
    }
    case ID_DO_BROWSE:
        browse( view );
        view_rebuild( view );
        return TRUE;
    case ID_DO_BACK_COLOR:
    case ID_DO_CUSTOM:
    {
        COLORREF color = desktop_color();

        if (choose_color( GetAncestor( content, GA_ROOT ), &color ))
            desktop_color_set( color, id == ID_DO_CUSTOM );
        view_rebuild( view );
        return TRUE;
    }
    }
    return FALSE;
}
