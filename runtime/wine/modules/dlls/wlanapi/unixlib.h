/*
 * WLAN API: calls into the unix side, which drives iwd over the system bus
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_WLANAPI_UNIXLIB_H
#define __WINE_WLANAPI_UNIXLIB_H

#include "wine/unixlib.h"

#define WLAN_MAX_INTERFACES 8
#define WLAN_MAX_NETWORKS   64

/* what iwd says a station is doing */
enum wlan_unix_state
{
    WLAN_UNIX_DISCONNECTED,
    WLAN_UNIX_CONNECTING,
    WLAN_UNIX_CONNECTED,
    WLAN_UNIX_DISCONNECTING,
    WLAN_UNIX_ROAMING,
};

struct wlan_unix_interface
{
    char   path[128];         /* iwd object of the device */
    char   name[32];          /* wlan0 */
    char   description[128];  /* the adapter, from the PCI / USB id databases */
    UINT32 state;             /* enum wlan_unix_state */
    UINT32 scanning;
    char   ssid[33];          /* of the network it is connected to, "" if none */
    INT32  signal;            /* dBm of that network, 0 if not known */
    UINT32 powered;           /* the radio is on (iwd Device.Powered) */
    UINT32 station;           /* iwd runs a station on it (only while powered) */
};

struct wlan_interfaces_params
{
    struct wlan_unix_interface *interfaces;  /* WLAN_MAX_INTERFACES */
    UINT32                      count;       /* out */
};

/* the security of a network, as iwd names it */
enum wlan_unix_security
{
    WLAN_UNIX_OPEN,
    WLAN_UNIX_PSK,
    WLAN_UNIX_8021X,
    WLAN_UNIX_WEP,
};

struct wlan_unix_network
{
    char   ssid[33];
    INT32  signal;            /* dBm */
    UINT32 security;          /* enum wlan_unix_security */
    UINT32 connected;
    UINT32 known;             /* iwd has its key */
};

struct wlan_networks_params
{
    const char               *path;         /* of the device */
    struct wlan_unix_network *networks;     /* WLAN_MAX_NETWORKS */
    UINT32                    count;        /* out */
};

struct wlan_device_params
{
    const char *path;
};

struct wlan_connect_params
{
    const char *path;         /* of the device */
    const char *ssid;
    const char *passphrase;   /* NULL: open or already known */
};

struct wlan_forget_params
{
    const char *ssid;
};

struct wlan_power_params
{
    const char *path;         /* of the device */
    UINT32      on;
};

enum wlan_funcs
{
    unix_wlan_interfaces,
    unix_wlan_networks,
    unix_wlan_scan,
    unix_wlan_connect,
    unix_wlan_disconnect,
    unix_wlan_forget,
    unix_wlan_set_power,
    unix_funcs_count
};

#endif
