/*
 * NVIDIA CUDA driver: the host's libcuda.so.1, function by function
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "wine/debug.h"

#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(nvcuda);

#ifdef __x86_64__

#define CUDA_ERROR_NOT_FOUND 500

typedef INT32 (*cu_fn)( UINT64, UINT64, UINT64, UINT64, UINT64, UINT64, UINT64, UINT64, UINT64, UINT64, UINT64, UINT64, UINT64, UINT64 );

static const char * const names[] =
{
#define CUFUNC(name) #name,
#define CUFUNC_CB(name) #name,
#include "funcs.h"
#undef CUFUNC
#undef CUFUNC_CB
};

static cu_fn fns[nvcuda_func_count];
static void *libcuda;
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void load(void)
{
    unsigned int i, found = 0;

    if (!(libcuda = dlopen( "libcuda.so.1", RTLD_NOW | RTLD_LOCAL )))
    {
        WARN( "%s\n", dlerror() );
        return;
    }
    for (i = 0; i < nvcuda_func_count; i++)
        if ((fns[i] = (cu_fn)dlsym( libcuda, names[i] ))) found++;
    TRACE( "%u of %u driver functions\n", found, nvcuda_func_count );
}

static NTSTATUS init( void *args )
{
    pthread_once( &once, load );
    return libcuda ? STATUS_SUCCESS : STATUS_DLL_NOT_FOUND;
}

static NTSTATUS forward( void *args )
{
    struct forward_params *p = args;
    const UINT64 *a = p->args;

    if (p->index >= nvcuda_func_count) return STATUS_INVALID_PARAMETER;
    if (!fns[p->index])
    {
        p->result = CUDA_ERROR_NOT_FOUND;
        return STATUS_SUCCESS;
    }
    p->result = fns[p->index]( a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12], a[13] );
    return STATUS_SUCCESS;
}

#else

static NTSTATUS init( void *args ) { return STATUS_NOT_SUPPORTED; }
static NTSTATUS forward( void *args ) { return STATUS_NOT_SUPPORTED; }

#endif  /* __x86_64__ */

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    init,
    forward,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_funcs) == unix_funcs_count );
