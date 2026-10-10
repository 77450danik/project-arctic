/*
 * Network and Sharing Center: the page
 *
 * Windows 7's page (netcenter.dll's UIFILEs 110-114): the map of this
 * computer, its network and the Internet, a cross where the way is broken;
 * the active networks, each with its icon, name and category, what it
 * reaches and the connection that makes it, which opens the connection's
 * status; then the tasks: a new connection, connecting to a network,
 * troubleshooting. The task pane: Wi-Fi networks, the adapters (Network
 * Connections). The page looks again every three seconds.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdio.h>

#include "netcenter.h"

#define INDENT     px(20)
#define MAP_HEIGHT px(96)
#define NET_HEIGHT px(76)
#define NET_ICON   px(48)
#define TASK_ICON  px(24)
#define COLUMN     px(330)  /* what a network reaches, beside its name */

struct center
{
    struct connection list[16];
    UINT count;
    BOOL internet;
    char signature[512];       /* what the page shows, to build it again only when it changes */
};

static struct center *center_data( struct view *view )
{
    struct page_state *state = view_state( view );

    if (!state->data) state->data = calloc( 1, sizeof(struct center) );
    return state->data;
}

static void draw_text( HDC hdc, int style, COLORREF color, const WCHAR *text, RECT *rect, UINT flags )
{
    HGDIOBJ old = SelectObject( hdc, view_font( style ) );

    SetTextColor( hdc, color );
    DrawTextW( hdc, text, -1, rect, DT_NOPREFIX | flags );
    SelectObject( hdc, old );
}

/**********************************************************************
 *          The map: this computer, the network, the Internet
 */

static void draw_cross( HDC hdc, int x, int y )
{
    int r = px( 7 );
    HPEN pen = CreatePen( PS_SOLID, px( 3 ), RGB( 0xd0, 0x20, 0x20 ) );
    HGDIOBJ old = SelectObject( hdc, pen );

    MoveToEx( hdc, x - r, y - r, NULL );
    LineTo( hdc, x + r, y + r );
    MoveToEx( hdc, x + r, y - r, NULL );
    LineTo( hdc, x - r, y + r );
    SelectObject( hdc, old );
    DeleteObject( pen );
}

static void paint_map( struct view *view, HDC hdc, const RECT *rect, UINT_PTR param )
{
    struct center *c = center_data( view );
    static const UINT icons[3] = { IDI_COMPUTER, IDI_NETWORK, IDI_INTERNET };
    WCHAR names[3][160], host[64];
    DWORD size = ARRAY_SIZE(host);
    int step = px( 180 ), left = rect->left + INDENT, icon_y = rect->top + px( 4 );
    BOOL connected = FALSE;
    HPEN pen = CreatePen( PS_SOLID, 1, RGB( 0x80, 0x80, 0x80 ) );
    HGDIOBJ old;

    for (UINT i = 0; i < c->count; i++) if (c->list[i].up && c->list[i].enabled) connected = TRUE;
    if (!GetComputerNameExW( ComputerNameDnsHostname, host, &size )) lstrcpyW( host, L"ARCTIC" );
    CharUpperW( host );
    swprintf( names[0], ARRAY_SIZE(names[0]), L"%s\n%s", host, load_string( IDS_THIS_COMPUTER ) );
    lstrcpynW( names[1], load_string( IDS_NETWORK ), ARRAY_SIZE(names[1]) );
    for (UINT i = 0; i < c->count; i++)
        if (c->list[i].up && c->list[i].enabled && c->list[i].wireless && c->list[i].ssid[0])
        {
            lstrcpynW( names[1], c->list[i].ssid, ARRAY_SIZE(names[1]) );
            break;
        }
    lstrcpynW( names[2], load_string( IDS_INTERNET ), ARRAY_SIZE(names[2]) );

    old = SelectObject( hdc, pen );
    for (int i = 0; i < 3; i++)
    {
        int x = left + i * step;
        RECT text = { x + NET_ICON / 2 - step / 2 + px( 4 ), icon_y + NET_ICON + px( 4 ), x + NET_ICON / 2 + step / 2 - px( 4 ),
                      rect->bottom };

        /* the network and the Internet are greyed where they cannot be reached */
        DrawIconEx( hdc, x, icon_y, load_icon( icons[i], NET_ICON ), NET_ICON, NET_ICON, 0, NULL, DI_NORMAL );
        draw_text( hdc, STYLE_BODY, i && !connected ? COLOR_GRAY : COLOR_TEXT, names[i], &text, DT_CENTER | DT_WORDBREAK );
        if (i < 2)
        {
            int y = icon_y + NET_ICON / 2, from = x + NET_ICON + px( 10 ), to = x + step - px( 10 );
            MoveToEx( hdc, from, y, NULL );
            LineTo( hdc, to, y );
            if ((i == 0 && !connected) || (i == 1 && connected && !c->internet)) draw_cross( hdc, (from + to) / 2, y );
        }
    }
    SelectObject( hdc, old );
    DeleteObject( pen );
}

/**********************************************************************
 *          The active networks
 */

static void paint_network( struct view *view, HDC hdc, const RECT *rect, UINT_PTR index )
{
    struct center *c = center_data( view );
    const struct connection *conn = &c->list[index];
    int left = rect->left + INDENT, column = rect->left + COLUMN;
    RECT text = { left + NET_ICON + px( 10 ), rect->top + px( 4 ), column - px( 16 ), rect->top + px( 24 ) };
    HPEN pen = CreatePen( PS_SOLID, 1, COLOR_LINE );
    HGDIOBJ old;

    DrawIconEx( hdc, left, rect->top, load_icon( IDI_NETWORK, NET_ICON ), NET_ICON, NET_ICON, 0, NULL, DI_NORMAL );
    draw_text( hdc, STYLE_BOLD, COLOR_TEXT, conn->wireless && conn->ssid[0] ? conn->ssid : load_string( IDS_NETWORK ),
               &text, DT_SINGLELINE | DT_END_ELLIPSIS );
    old = SelectObject( hdc, pen );
    MoveToEx( hdc, column - px( 8 ), rect->top, NULL );
    LineTo( hdc, column - px( 8 ), rect->top + NET_ICON + px( 12 ) );
    SelectObject( hdc, old );
    DeleteObject( pen );
    {
        RECT label = { column, rect->top + px( 4 ), column + px( 120 ), rect->top + px( 24 ) };
        RECT value = { column + px( 120 ), rect->top + px( 4 ), rect->right, rect->top + px( 24 ) };

        draw_text( hdc, STYLE_BODY, COLOR_TEXT, load_string( IDS_ACCESS_TYPE ), &label, DT_SINGLELINE );
        draw_text( hdc, STYLE_BODY, COLOR_TEXT,
                   load_string( !conn->ipv4 && !conn->ipv6 ? IDS_ACCESS_NO_NETWORK :
                                conn->gateway4 || conn->gateway6 ? IDS_ACCESS_INTERNET : IDS_ACCESS_NO_INTERNET ),
                   &value, DT_SINGLELINE | DT_END_ELLIPSIS );
        OffsetRect( &label, 0, px( 24 ) );
        draw_text( hdc, STYLE_BODY, COLOR_TEXT, load_string( IDS_CONNECTIONS ), &label, DT_SINGLELINE );
    }
}

static void open_status( struct view *view, UINT_PTR index )
{
    struct center *c = center_data( view );
    WCHAR args[96];

    if (index >= c->count) return;
    swprintf( args, ARRAY_SIZE(args), L"netcenter.dll,ShowConnectionStatus %s", c->list[index].guid );
    cp_run( L"rundll32.exe", args );
}

static void category_link( struct view *view, UINT_PTR index )
{
    /* Windows 7 asks Home, Work or Public here; Arctic keeps every network private */
}

/**********************************************************************
 *          The tasks
 */

static void open_network_list( struct view *view, UINT_PTR param )
{
    cp_run( L"rundll32.exe", L"pnidui.dll,ShowNetworkList" );
}

static void open_adapters( struct view *view, UINT_PTR param )
{
    open_adapter_settings();
}

static void open_sharing( struct view *view, UINT_PTR param )
{
    view_navigate( view, 1, 0 );
}

static void open_internet_options( struct view *view, UINT_PTR param )
{
    cp_run( L"control.exe", L"inetcpl.cpl" );
}

static void troubleshoot( struct view *view, UINT_PTR param )
{
    struct center *c = center_data( view );

    /* the status of the first connection, whose "Діагностика" asks again */
    for (UINT i = 0; i < c->count; i++)
        if (c->list[i].enabled)
        {
            open_status( view, i );
            return;
        }
    open_adapter_settings();
}

static void paint_task_icon( struct view *view, HDC hdc, const RECT *rect, UINT_PTR icon )
{
    DrawIconEx( hdc, rect->left + INDENT, rect->top + px( 2 ), load_icon( icon, TASK_ICON ), TASK_ICON, TASK_ICON, 0,
                NULL, DI_NORMAL );
}

static void add_task( struct view *view, UINT icon, UINT title, UINT text, link_proc proc )
{
    view_space( view, px( 10 ) );
    view_paint_area( view, 0, paint_task_icon, icon );
    view_link( view, load_string( title ), proc, 0, INDENT + TASK_ICON + px( 10 ) );
    view_space( view, px( 5 ) );
    view_text_at( view, STYLE_BODY, load_string( text ), INDENT + TASK_ICON + px( 10 ) );
    view_space( view, px( 4 ) );
}

/**********************************************************************
 *          The page
 */

static void signature( struct center *c, char *out, size_t size )
{
    out[0] = 0;
    for (UINT i = 0; i < c->count; i++)
    {
        const struct connection *conn = &c->list[i];
        size_t len = strlen( out );
        snprintf( out + len, size - len, "%d%d%d%d%d%d%d%ls|", conn->up, conn->enabled, conn->ipv4, conn->ipv6,
                  conn->gateway4, conn->gateway6, conn->signal / 20, conn->ssid );
    }
}

static void refresh( struct view *view )
{
    struct center *c = center_data( view );
    char before[512];

    lstrcpynA( before, c->signature, sizeof(before) );
    c->count = connections_list( c->list, ARRAY_SIZE(c->list) );
    c->internet = FALSE;
    for (UINT i = 0; i < c->count; i++)
        if (c->list[i].up && c->list[i].enabled && (c->list[i].gateway4 || c->list[i].gateway6)) c->internet = TRUE;
    signature( c, c->signature, sizeof(c->signature) );
    if (strcmp( before, c->signature )) view_rebuild( view );
}

void center_timer( struct view *view )
{
    refresh( view );
}

void center_page( struct view *view )
{
    struct center *c = center_data( view );
    BOOL any = FALSE;

    c->count = connections_list( c->list, ARRAY_SIZE(c->list) );
    c->internet = FALSE;
    for (UINT i = 0; i < c->count; i++)
        if (c->list[i].up && c->list[i].enabled && (c->list[i].gateway4 || c->list[i].gateway6)) c->internet = TRUE;
    signature( c, c->signature, sizeof(c->signature) );
    view_set_timer( view, 3000 );

    view_nav_home( view );
    view_nav_link( view, load_string( IDS_MANAGE_WIRELESS ), open_network_list, 0 );
    view_nav_link( view, load_string( IDS_CHANGE_ADAPTER ), open_adapters, 0 );
    view_nav_link( view, load_string( IDS_CHANGE_SHARING ), open_sharing, 0 );
    view_nav_see_also( view, load_string( IDS_SEE_INTERNET_OPTIONS ), open_internet_options, 0 );

    view_text( view, STYLE_TITLE, load_string( IDS_TITLE ) );
    view_space( view, px( 14 ) );
    view_paint_area( view, MAP_HEIGHT, paint_map, 0 );
    view_space( view, px( 8 ) );

    view_group( view, load_string( IDS_ACTIVE_NETWORKS ) );
    view_space( view, px( 6 ) );
    for (UINT i = 0; i < c->count; i++)
    {
        const struct connection *conn = &c->list[i];
        WCHAR text[256];

        if (!conn->up || !conn->enabled) continue;
        any = TRUE;
        view_paint_area( view, NET_HEIGHT, paint_network, i );
        view_link_in( view, NULL, load_string( IDS_PRIVATE_NETWORK ), category_link, i, INDENT + NET_ICON + px( 10 ),
                      px( 26 ) );
        if (conn->wireless && conn->ssid[0]) swprintf( text, ARRAY_SIZE(text), L"%s (%s)", conn->name, conn->ssid );
        else lstrcpynW( text, conn->name, ARRAY_SIZE(text) );
        view_link_in( view, load_icon( conn->wireless ? IDI_SIGNAL0 + min( 5, (conn->signal + 19) / 20 ) : IDI_ETHERNET,
                                       GetSystemMetrics( SM_CXSMICON ) ),
                      text, open_status, i, COLUMN + px( 120 ), px( 26 ) );
    }
    if (!any)
    {
        view_text_at( view, STYLE_BODY, load_string( IDS_NOT_CONNECTED ), INDENT );
        view_space( view, px( 20 ) );
    }

    view_group( view, load_string( IDS_CHANGE_SETTINGS ) );
    add_task( view, IDI_NEW_CONNECTION, IDS_NEW_CONNECTION, IDS_NEW_CONNECTION_TIP, open_network_list );
    add_task( view, IDI_NETCENTER, IDS_CONNECT_TO, IDS_CONNECT_TO_TIP, open_network_list );
    add_task( view, IDI_TROUBLESHOOT, IDS_TROUBLESHOOT, IDS_TROUBLESHOOT_TIP, troubleshoot );
}
