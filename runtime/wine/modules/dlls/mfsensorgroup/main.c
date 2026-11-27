/*
 * Media Foundation sensor groups (mfsensorgroup.dll)
 *
 * What programs ask of it to know when a camera or a microphone is in use
 * (Windows 10 1809 and later): the sensor activity monitor. Logitech G HUB's
 * agent imports it and could not start without the DLL. A monitor here
 * starts and stops as in Windows and reports nothing: no program of Arctic
 * tells it of a sensor yet.
 *
 * Copyright 2026 Arctic contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define COBJMACROS
#include <stdarg.h>
#include <stdlib.h>

#include "windef.h"
#include "winbase.h"
#include "objbase.h"
#include "mferror.h"
#include "initguid.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(mfplat);

DEFINE_GUID(IID_IMFSensorActivityMonitor, 0xd0cef145, 0xb3f4, 0x4340, 0xa2, 0xe5, 0x7a, 0x50, 0x80, 0xca, 0x05, 0xcb);

/* IMFSensorActivityMonitor: IUnknown, Start, Stop */
typedef struct IMFSensorActivityMonitor IMFSensorActivityMonitor;
typedef struct IMFSensorActivityMonitorVtbl
{
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IMFSensorActivityMonitor *iface, REFIID riid, void **obj);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IMFSensorActivityMonitor *iface);
    ULONG   (STDMETHODCALLTYPE *Release)(IMFSensorActivityMonitor *iface);
    HRESULT (STDMETHODCALLTYPE *Start)(IMFSensorActivityMonitor *iface);
    HRESULT (STDMETHODCALLTYPE *Stop)(IMFSensorActivityMonitor *iface);
} IMFSensorActivityMonitorVtbl;
struct IMFSensorActivityMonitor { const IMFSensorActivityMonitorVtbl *lpVtbl; };

struct activity_monitor
{
    IMFSensorActivityMonitor IMFSensorActivityMonitor_iface;
    LONG refcount;
    IUnknown *callback;     /* IMFSensorActivitiesReportCallback */
    BOOL started;
    SRWLOCK lock;
};

static struct activity_monitor *impl_from_IMFSensorActivityMonitor( IMFSensorActivityMonitor *iface )
{
    return CONTAINING_RECORD( iface, struct activity_monitor, IMFSensorActivityMonitor_iface );
}

static HRESULT WINAPI monitor_QueryInterface( IMFSensorActivityMonitor *iface, REFIID riid, void **obj )
{
    TRACE( "%p, %s, %p\n", iface, debugstr_guid(riid), obj );

    if (IsEqualIID( riid, &IID_IMFSensorActivityMonitor ) || IsEqualIID( riid, &IID_IUnknown ))
    {
        *obj = iface;
        iface->lpVtbl->AddRef( iface );
        return S_OK;
    }
    WARN( "no interface %s\n", debugstr_guid(riid) );
    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI monitor_AddRef( IMFSensorActivityMonitor *iface )
{
    struct activity_monitor *monitor = impl_from_IMFSensorActivityMonitor( iface );
    return InterlockedIncrement( &monitor->refcount );
}

static ULONG WINAPI monitor_Release( IMFSensorActivityMonitor *iface )
{
    struct activity_monitor *monitor = impl_from_IMFSensorActivityMonitor( iface );
    ULONG refcount = InterlockedDecrement( &monitor->refcount );

    if (!refcount)
    {
        IUnknown_Release( monitor->callback );
        free( monitor );
    }
    return refcount;
}

static HRESULT WINAPI monitor_Start( IMFSensorActivityMonitor *iface )
{
    struct activity_monitor *monitor = impl_from_IMFSensorActivityMonitor( iface );
    HRESULT hr = S_OK;

    TRACE( "%p\n", iface );

    AcquireSRWLockExclusive( &monitor->lock );
    if (monitor->started) hr = MF_E_INVALIDREQUEST;
    else monitor->started = TRUE;
    ReleaseSRWLockExclusive( &monitor->lock );
    return hr;
}

static HRESULT WINAPI monitor_Stop( IMFSensorActivityMonitor *iface )
{
    struct activity_monitor *monitor = impl_from_IMFSensorActivityMonitor( iface );
    HRESULT hr = S_OK;

    TRACE( "%p\n", iface );

    AcquireSRWLockExclusive( &monitor->lock );
    if (!monitor->started) hr = MF_E_INVALIDREQUEST;
    else monitor->started = FALSE;
    ReleaseSRWLockExclusive( &monitor->lock );
    return hr;
}

static const IMFSensorActivityMonitorVtbl monitor_vtbl =
{
    monitor_QueryInterface,
    monitor_AddRef,
    monitor_Release,
    monitor_Start,
    monitor_Stop,
};

/***********************************************************************
 *      MFCreateSensorActivityMonitor (mfsensorgroup.@)
 */
HRESULT WINAPI MFCreateSensorActivityMonitor( IUnknown *callback, IMFSensorActivityMonitor **ret )
{
    struct activity_monitor *monitor;

    TRACE( "%p, %p\n", callback, ret );

    if (!callback || !ret) return E_POINTER;
    if (!(monitor = calloc( 1, sizeof(*monitor) ))) return E_OUTOFMEMORY;
    monitor->IMFSensorActivityMonitor_iface.lpVtbl = &monitor_vtbl;
    monitor->refcount = 1;
    monitor->callback = callback;
    IUnknown_AddRef( callback );
    InitializeSRWLock( &monitor->lock );
    *ret = &monitor->IMFSensorActivityMonitor_iface;
    return S_OK;
}
