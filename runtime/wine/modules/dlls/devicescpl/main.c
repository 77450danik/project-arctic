/*
 * Devices and Printers of the Control Panel (devicescpl.dll)
 *
 * Windows' item {A8A91A66-3A7D-4424-8D24-04E180695C7A}, a page of the
 * Control Panel inside Explorer drawn as Windows 11's Settings, "Bluetooth
 * and devices": the Bluetooth switch, "Add device" (the wizard of
 * wizard.c, which Explorer's command bar opens too), the paired devices with
 * their state, Connect / Disconnect and Remove, and the printers.
 * The radio is winebth.sys's (docs/bluetooth.md), reached through
 * bluetoothapis.dll as programs reach it.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS

#include <stdio.h>

#include "cpanel.h"
#include "commctrl.h"
#include "shellapi.h"
#include "bthsdpdef.h"
#include "bluetoothapis.h"
#include "bthdef.h"
#include "devicescpl.h"

HINSTANCE cp_instance;

const CLSID cp_clsid = { 0xa8a91a66, 0x3a7d, 0x4424, { 0x8d, 0x24, 0x04, 0xe1, 0x80, 0x69, 0x5c, 0x7a } };
const WCHAR cp_class_name[] = L"Devices";

#define ID_BLUETOOTH     100
#define ID_ADD           101
#define ID_SEND          102
#define ID_DEVICE_FIRST  200     /* + 2 a device: Connect or Disconnect, then Remove */
#define ID_DO_REBUILD    900

#define MAX_DEVICES      32

/* what the page shows, to see when it changed */
struct snapshot
{
    BOOL radio, powered;
    UINT count;
    struct bt_device devices[MAX_DEVICES];
};

static HICON glyph_icon( enum device_glyph glyph )
{
    static HICON cache[GLYPH_SEND + 1][2];
    static int cache_size;
    BOOL dark = cp_dark();
    int size = px( 32 );

    if (cache_size != size)
    {
        for (int i = 0; i <= GLYPH_SEND; i++)
            for (int j = 0; j < 2; j++)
                if (cache[i][j]) DestroyIcon( cache[i][j] ), cache[i][j] = NULL;
        cache_size = size;
    }
    if (!cache[glyph][dark])
        cache[glyph][dark] = make_device_icon( glyph, size, px( 5 ), dark ? RGB( 0xff, 0xff, 0xff ) : RGB( 0x1b, 0x1b, 0x1b ) );
    return cache[glyph][dark];
}

static void defer( struct view *view, UINT id )
{
    HWND content = view_window( view );

    PostMessageW( content, WM_COMMAND, MAKEWPARAM( id, 0 ), (LPARAM)content );
}

static void take_snapshot( struct snapshot *snap )
{
    HANDLE radio = bt_open_radio();

    memset( snap, 0, sizeof(*snap) );
    if (!radio) return;
    snap->radio = TRUE;
    bt_radio_state( radio, &snap->powered, NULL, NULL );
    if (snap->powered) snap->count = bt_devices( radio, snap->devices, MAX_DEVICES, TRUE );
    CloseHandle( radio );
}

static BOOL snapshot_differs( const struct snapshot *a, const struct snapshot *b )
{
    if (a->radio != b->radio || a->powered != b->powered || a->count != b->count) return TRUE;
    for (UINT i = 0; i < a->count; i++)
    {
        const BLUETOOTH_DEVICE_INFO *x = &a->devices[i].info, *y = &b->devices[i].info;

        if (x->Address.ullLong != y->Address.ullLong || x->fConnected != y->fConnected ||
            a->devices[i].audio != b->devices[i].audio || wcscmp( x->szName, y->szName ) ||
            memcmp( &a->devices[i].battery, &b->devices[i].battery, sizeof(a->devices[i].battery) ))
            return TRUE;
    }
    return FALSE;
}

static const WCHAR *device_state( const struct bt_device *device )
{
    if (device->audio) return load_string( IDS_CONNECTED_MUSIC );
    if (device->info.fConnected) return load_string( IDS_CONNECTED );
    return load_string( IDS_PAIRED );
}

static void open_printers( struct view *view, UINT_PTR param )
{
    view_browse_item( view, L"::{2227A280-3AEA-1069-A2DE-08002B30309D}" );
}

static void devices_build( struct view *view )
{
    struct page_state *state = view_state( view );
    struct snapshot *snap;
    WCHAR title[256], name[BLUETOOTH_MAX_NAME_SIZE];

    if (!state->data) state->data = calloc( 1, sizeof(struct snapshot) );
    if (!(snap = state->data)) return;
    take_snapshot( snap );

    view_nav_home( view );
    view_heading( view, load_string( IDS_NAME ) );

    lstrcpynW( title, load_string( IDS_BLUETOOTH ), ARRAY_SIZE(title) );
    if (!snap->radio)
        view_card( view, glyph_icon( GLYPH_BLUETOOTH ), title, load_string( IDS_NO_RADIO ), NULL, NULL, 0, 0, 0, 0 );
    else
    {
        HANDLE radio = bt_open_radio();

        bt_radio_name( radio, name, ARRAY_SIZE(name) );
        if (radio) CloseHandle( radio );
        /* on: the name other devices see this PC by */
        view_card_toggle( view, glyph_icon( GLYPH_BLUETOOTH ), title,
                          snap->powered ? format_string( IDS_VISIBLE, name ) : load_string( IDS_RADIO_OFF ),
                          snap->powered, ID_BLUETOOTH );
    }
    lstrcpynW( title, load_string( IDS_ADD_DEVICE ), ARRAY_SIZE(title) );
    view_card( view, glyph_icon( GLYPH_ADD ), title, load_string( IDS_ADD_DEVICE_HINT ), L"BUTTON",
               load_string( IDS_ADD ), BS_PUSHBUTTON | WS_TABSTOP, px( 130 ), px( 28 ), ID_ADD );

    if (snap->count)
    {
        view_section( view, load_string( IDS_DEVICES ) );
        for (UINT i = 0; i < snap->count; i++)
        {
            const struct bt_device *device = &snap->devices[i];
            BOOL connected = device->info.fConnected || device->audio;

            lstrcpynW( title, device->info.szName[0] ? device->info.szName : load_string( IDS_WIZ_UNNAMED ),
                       ARRAY_SIZE(title) );
            view_card( view, glyph_icon( glyph_of_class( device->info.ulClassofDevice ) ), title,
                       device_state( device ), L"BUTTON", load_string( connected ? IDS_DISCONNECT : IDS_CONNECT ),
                       BS_PUSHBUTTON | WS_TABSTOP, px( 120 ), px( 28 ), ID_DEVICE_FIRST + 2 * i );
            view_card_extra( view, L"BUTTON", load_string( IDS_REMOVE ), BS_PUSHBUTTON | WS_TABSTOP, px( 100 ),
                             px( 28 ), ID_DEVICE_FIRST + 2 * i + 1 );
            /* its batteries, as rings, while it is connected and tells them */
            if (connected && battery_rings_width( &device->battery ))
            {
                WCHAR levels[64];

                battery_rings_text( &device->battery, levels, ARRAY_SIZE(levels) );
                view_card_extra( view, BATTERY_RINGS_CLASS, levels, 0, battery_rings_width( &device->battery ) + px( 8 ),
                                 px( 50 ), 0 );
            }
        }
    }
    if (snap->powered)
    {
        lstrcpynW( title, load_string( IDS_SEND_FILES ), ARRAY_SIZE(title) );
        view_card( view, glyph_icon( GLYPH_SEND ), title, load_string( IDS_SEND_FILES_HINT ), L"BUTTON",
                   load_string( IDS_SEND_FILES ), BS_PUSHBUTTON | WS_TABSTOP, px( 130 ), px( 28 ), ID_SEND );
    }

    view_section( view, load_string( IDS_PRINTERS ) );
    lstrcpynW( title, load_string( IDS_PRINTERS ), ARRAY_SIZE(title) );
    view_card_link( view, glyph_icon( GLYPH_PRINTER ), title, load_string( IDS_PRINTERS_HINT ), open_printers, 0 );

    view_set_timer( view, 1500 );
}

/* what the devices do by themselves (a headset switched on) shows */
static void devices_timer( struct view *view )
{
    struct page_state *state = view_state( view );
    struct snapshot now;

    if (!state->data) return;
    take_snapshot( &now );
    if (snapshot_differs( &now, state->data )) view_rebuild( view );
}

static BOOL devices_command( struct view *view, UINT id, UINT code, HWND control )
{
    struct page_state *state = view_state( view );
    struct snapshot *snap = state->data;

    if (id == ID_DO_REBUILD)
    {
        view_rebuild( view );
        return TRUE;
    }
    if (code != BN_CLICKED) return TRUE;

    if (id == ID_BLUETOOTH)
    {
        HANDLE radio = bt_open_radio();

        bt_set_power( radio, SendMessageW( control, BM_GETCHECK, 0, 0 ) == BST_CHECKED );
        if (radio) CloseHandle( radio );
        defer( view, ID_DO_REBUILD );
        return TRUE;
    }
    if (id == ID_ADD)
    {
        add_device_wizard( GetAncestor( view_window( view ), GA_ROOT ), FALSE );
        defer( view, ID_DO_REBUILD );
        return TRUE;
    }
    if (id == ID_SEND)
    {
        cp_run( L"fsquirt.exe", NULL );
        return TRUE;
    }
    if (snap && id >= ID_DEVICE_FIRST && id < ID_DEVICE_FIRST + 2 * snap->count)
    {
        const struct bt_device *device = &snap->devices[(id - ID_DEVICE_FIRST) / 2];

        if ((id - ID_DEVICE_FIRST) % 2)
        {
            if (MessageBoxW( GetAncestor( view_window( view ), GA_ROOT ), load_string( IDS_REMOVE_CONFIRM ),
                             device->info.szName, MB_YESNO | MB_ICONQUESTION ) != IDYES)
                return TRUE;
            bt_remove( device->info.Address.ullLong );
            defer( view, ID_DO_REBUILD );
        }
        else
        {
            /* the driver answers when the device has: the timer shows it */
            if (device->info.fConnected || device->audio) bt_disconnect( device->info.Address.ullLong );
            else bt_connect( device->info.Address.ullLong );
            SetWindowTextW( control, load_string( IDS_CONNECTING ) );
            EnableWindow( control, FALSE );
        }
        return TRUE;
    }
    return FALSE;
}

const struct cp_page cp_pages[] =
{
    { IDS_NAME, L"pageDevices", devices_build, devices_command, devices_timer },
};
const UINT cp_page_count = ARRAY_SIZE(cp_pages);

/* "Додати пристрій" from Explorer's command bar and elsewhere: the wizard;
 * kind 1 goes to the printers */
void WINAPI ArcticAddDevice( HWND owner, int kind )
{
    if (kind == 1) ShellExecuteW( owner, NULL, L"explorer.exe",
                                  L"::{26EE0668-A00A-44D7-9371-BEB064C98683}\\0\\::{2227A280-3AEA-1069-A2DE-08002B30309D}",
                                  NULL, SW_SHOWNORMAL );
    else add_device_wizard( owner, FALSE );
}

/* rundll32 devicescpl.dll,AddDevice: the wizard alone, as DevicePairingWizard.exe */
void WINAPI AddDevice( HWND hwnd, HINSTANCE instance, LPSTR cmdline, int show )
{
    add_device_wizard( NULL, FALSE );
}

HRESULT WINAPI DllGetClassObject( REFCLSID clsid, REFIID riid, void **out )
{
    return cp_class_object( clsid, riid, out );
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    return cp_can_unload();
}

BOOL WINAPI DllMain( HINSTANCE instance, DWORD reason, void *reserved )
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        cp_instance = instance;
        DisableThreadLibraryCalls( instance );
        battery_rings_register();
    }
    return TRUE;
}
