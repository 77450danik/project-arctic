/*
 * Power management: calls into the unix side
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_POWRPROF_UNIXLIB_H
#define __WINE_POWRPROF_UNIXLIB_H

#include "wine/unixlib.h"
#include "wine/arctic_power.h"

struct gpu_list_params
{
    struct arctic_gpu_list *list;
    UINT32 with_users;
};

enum powrprof_funcs
{
    unix_power_status,   /* struct arctic_power_status */
    unix_gpu_list,       /* struct gpu_list_params */
    unix_cpu_info,       /* struct arctic_cpu_info */
    unix_power_apply,    /* struct arctic_power_apply */
    unix_funcs_count
};

#endif
