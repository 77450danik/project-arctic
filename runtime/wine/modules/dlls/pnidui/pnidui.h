/*
 * Network icon of the notification area (pnidui.dll)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __PNIDUI_H
#define __PNIDUI_H

#define IDS_NOT_CONNECTED        100
#define IDS_CONNECTIONS_AVAILABLE 101
#define IDS_INTERNET_ACCESS      102
#define IDS_WIRELESS_GROUP       103
#define IDS_WIRED_GROUP          104
#define IDS_CONNECTED            105
#define IDS_CONNECTING           106
#define IDS_CABLE_UNPLUGGED      107
#define IDS_CONNECT              108
#define IDS_DISCONNECT           109
#define IDS_OPEN_CONNECTIONS     110
#define IDS_CONNECT_FAILED       111
#define IDS_KEY_MISMATCH         112
#define IDS_NETWORK              113
#define IDS_NO_NETWORKS          114
#define IDS_NO_INTERNET          115
#define IDS_SECURED              116
#define IDS_OPEN_NETWORK         117
#define IDS_CONNECTED_SECURED    118
#define IDS_SETTINGS_LINK        119
#define IDS_SETTINGS_HELP        120
#define IDS_TILE_WIFI            121
#define IDS_TILE_AIRPLANE        122
#define IDS_TILE_HOTSPOT         123
#define IDS_WIFI_OFF             124
#define IDS_AIRPLANE_ON          125
#define IDS_CONNECT_PLAIN        126
#define IDS_DISCONNECT_PLAIN     127
#define IDS_CONNECTED_OPEN       128
#define IDS_WIRED_NONE           129
#define IDS_AUTOCONNECT          130
#define IDS_OPEN_NETWORK_CENTER  131

/* the notification area's pictures of a wired network */
#define IDI_NETWORK_OK           300
#define IDI_NETWORK_WARNING      301
#define IDI_NETWORK_ABSENT       302

#define IDD_NETWORK_KEY          200
#define IDC_KEY                  201
#define IDC_HIDE_KEY             202

void tray_start(void);
void tray_stop(void);

extern HINSTANCE pnidui_instance;

/* tray.c: what the PC is connected to */
int px( int n );
struct state
{
    BOOL wired_present, wired_connected;
    BOOL wired_limited;       /* the cable is in, but there is no way out (no address or gateway) */
    BOOL wifi_connected, wifi_connecting;
    WCHAR wifi_ssid[64];
    UINT wifi_quality;
    BOOL networks_available;
};
extern HANDLE wlan;
extern GUID wifi_guid;
extern BOOL have_wifi;
extern WCHAR connecting_ssid[64];
extern BOOL asking_key;

WCHAR *load_string( UINT id );
void ssid_to_text( const DOT11_SSID *ssid, WCHAR *text, int size );
void read_wired( struct state *state );
void read_wifi( struct state *state );
UINT quality_bars( UINT quality );
BOOL connections_folder_exists(void);
void open_connections(void);
void open_network_center(void);

/* flyout.c: the flyout of Windows 10 */
void flyout_toggle(void);
void flyout_refresh(void);
void flyout_layout( BOOL reposition );
void flyout_hide(void);
void flyout_destroy(void);
BOOL flyout_visible(void);

#endif
