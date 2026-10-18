/*
 * nvcuda: the parameters of the forwarder
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_NVCUDA_UNIXLIB_H
#define __WINE_NVCUDA_UNIXLIB_H

#include "wine/unixlib.h"

#define NVCUDA_ARGS 14

enum nvcuda_func_index
{
#define CUFUNC(name) idx_##name,
#define CUFUNC_CB(name) idx_##name,
#include "funcs.h"
#undef CUFUNC
#undef CUFUNC_CB
    nvcuda_func_count
};

struct forward_params
{
    UINT32 index;               /* enum nvcuda_func_index */
    INT32  result;              /* the CUresult */
    UINT64 args[NVCUDA_ARGS];
};

enum nvcuda_funcs
{
    unix_init,
    unix_forward,
    unix_funcs_count
};

#endif
