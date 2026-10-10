/*
 * Network and Sharing Center: the calls to the unix side
 *
 * Only fixed arrays in the parameters: they look the same to a 32-bit caller.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __NETCENTER_UNIXLIB_H
#define __NETCENTER_UNIXLIB_H

#include "wine/unixlib.h"

struct settings_params
{
    char ifname[32];
    char text[4096];
};

struct link_info_params
{
    char ifname[32];
    char adapter[128];        /* out: from the id databases of the host */
    unsigned int speed_kbps;  /* out: 0 when unknown */
    unsigned int connected_for; /* out: seconds since the interface got its addresses, 0 when unknown */
    char dns[512];            /* out: its name servers now, as the host resolves by them, space separated */
};

struct iwd_params
{
    char ifname[32];
    char ipv4[512];           /* the [IPv4] section of every network iwd knows, empty: none */
    char ipv6[512];
};

enum netcenter_funcs
{
    unix_read_settings,
    unix_write_settings,
    unix_link_info,
    unix_iwd_addresses,
    unix_funcs_count
};

#endif
