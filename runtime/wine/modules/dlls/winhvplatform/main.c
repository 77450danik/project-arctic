/*
 * Windows Hypervisor Platform (WinHvPlatform.dll)
 *
 * The API Windows gives programs that run virtual machines on the
 * hypervisor (QEMU's -accel whpx, the Android emulator, VirtualBox and
 * VMware Workstation on Hyper-V): partitions, guest memory, virtual
 * processors, their registers and exits. Here the partition is a KVM
 * virtual machine (unix.c), so such a program gets the processor's own
 * virtualization as on Windows, without a change (docs/hypervisor.md).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "winhvplatform.h"
#include "wine/debug.h"

#include "unixlib.h"

#ifdef __x86_64__

WINE_DEFAULT_DEBUG_CHANNEL(winhvplatform);

static BOOL unix_ready;

static BOOL init_unix(void)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    BOOL pending;

    if (InitOnceBeginInitialize( &once, 0, &pending, NULL ) && pending)
    {
        unix_ready = !__wine_init_unix_call();
        InitOnceComplete( &once, 0, NULL );
    }
    return unix_ready;
}

static HRESULT call( unsigned int code, void *params, HRESULT *hr )
{
    NTSTATUS status;

    if (!init_unix()) return E_NOTIMPL;
    if ((status = WINE_UNIX_CALL( code, params ))) return HRESULT_FROM_NT( status );
    return *hr;
}

static UINT64 handle( WHV_PARTITION_HANDLE partition )
{
    return (UINT_PTR)partition;
}

HRESULT WINAPI WHvGetCapability( WHV_CAPABILITY_CODE code, void *buffer, UINT32 size, UINT32 *written )
{
    struct capability_params params = { code, buffer, size, written };

    TRACE( "%#x %p %u %p\n", code, buffer, size, written );
    if (!buffer) return E_POINTER;
    return call( unix_get_capability, &params, &params.hr );
}

HRESULT WINAPI WHvCreatePartition( WHV_PARTITION_HANDLE *partition )
{
    struct partition_params params = { 0 };
    HRESULT hr;

    TRACE( "%p\n", partition );
    if (!partition) return E_POINTER;
    *partition = NULL;
    if (SUCCEEDED(hr = call( unix_create_partition, &params, &params.hr )))
        *partition = (WHV_PARTITION_HANDLE)(UINT_PTR)params.partition;
    return hr;
}

HRESULT WINAPI WHvSetupPartition( WHV_PARTITION_HANDLE partition )
{
    struct partition_params params = { handle( partition ) };

    TRACE( "%p\n", partition );
    return call( unix_setup_partition, &params, &params.hr );
}

HRESULT WINAPI WHvDeletePartition( WHV_PARTITION_HANDLE partition )
{
    struct partition_params params = { handle( partition ) };

    TRACE( "%p\n", partition );
    return call( unix_delete_partition, &params, &params.hr );
}

HRESULT WINAPI WHvGetPartitionProperty( WHV_PARTITION_HANDLE partition, WHV_PARTITION_PROPERTY_CODE code,
                                        void *buffer, UINT32 size, UINT32 *written )
{
    struct property_params params = { handle( partition ), code, NULL, buffer, size, written };

    TRACE( "%p %#x %p %u %p\n", partition, code, buffer, size, written );
    if (!buffer) return E_POINTER;
    return call( unix_get_property, &params, &params.hr );
}

HRESULT WINAPI WHvSetPartitionProperty( WHV_PARTITION_HANDLE partition, WHV_PARTITION_PROPERTY_CODE code,
                                        const void *buffer, UINT32 size )
{
    struct property_params params = { handle( partition ), code, buffer, NULL, size, NULL };

    TRACE( "%p %#x %p %u\n", partition, code, buffer, size );
    if (!buffer) return E_POINTER;
    return call( unix_set_property, &params, &params.hr );
}

HRESULT WINAPI WHvMapGpaRange( WHV_PARTITION_HANDLE partition, void *source, WHV_GUEST_PHYSICAL_ADDRESS gpa,
                               UINT64 size, WHV_MAP_GPA_RANGE_FLAGS flags )
{
    struct map_params params = { handle( partition ), source, gpa, size, flags };

    TRACE( "%p %p %#I64x %#I64x %#x\n", partition, source, gpa, size, flags );
    return call( unix_map_gpa_range, &params, &params.hr );
}

HRESULT WINAPI WHvMapGpaRange2( WHV_PARTITION_HANDLE partition, HANDLE process, void *source,
                                WHV_GUEST_PHYSICAL_ADDRESS gpa, UINT64 size, WHV_MAP_GPA_RANGE_FLAGS flags )
{
    /* memory of this process only: KVM maps from the address space it runs in */
    if (process && process != GetCurrentProcess() && GetProcessId( process ) != GetCurrentProcessId())
    {
        FIXME( "memory of another process (%p)\n", process );
        return E_NOTIMPL;
    }
    return WHvMapGpaRange( partition, source, gpa, size, flags );
}

HRESULT WINAPI WHvUnmapGpaRange( WHV_PARTITION_HANDLE partition, WHV_GUEST_PHYSICAL_ADDRESS gpa, UINT64 size )
{
    struct map_params params = { handle( partition ), NULL, gpa, size };

    TRACE( "%p %#I64x %#I64x\n", partition, gpa, size );
    return call( unix_unmap_gpa_range, &params, &params.hr );
}

HRESULT WINAPI WHvTranslateGva( WHV_PARTITION_HANDLE partition, UINT32 vp, WHV_GUEST_VIRTUAL_ADDRESS gva,
                                WHV_TRANSLATE_GVA_FLAGS flags, WHV_TRANSLATE_GVA_RESULT *result,
                                WHV_GUEST_PHYSICAL_ADDRESS *gpa )
{
    struct translate_params params = { handle( partition ), vp, gva, flags, result, gpa };

    TRACE( "%p %u %#I64x %#x %p %p\n", partition, vp, gva, flags, result, gpa );
    if (!result || !gpa) return E_POINTER;
    return call( unix_translate_gva, &params, &params.hr );
}

HRESULT WINAPI WHvCreateVirtualProcessor( WHV_PARTITION_HANDLE partition, UINT32 vp, UINT32 flags )
{
    struct vp_params params = { handle( partition ), vp, flags };

    TRACE( "%p %u %#x\n", partition, vp, flags );
    return call( unix_create_vp, &params, &params.hr );
}

HRESULT WINAPI WHvCreateVirtualProcessor2( WHV_PARTITION_HANDLE partition, UINT32 vp,
                                           const WHV_VIRTUAL_PROCESSOR_PROPERTY *properties, UINT32 count )
{
    /* the NUMA node is the only property: one node here */
    return WHvCreateVirtualProcessor( partition, vp, 0 );
}

HRESULT WINAPI WHvDeleteVirtualProcessor( WHV_PARTITION_HANDLE partition, UINT32 vp )
{
    struct vp_params params = { handle( partition ), vp };

    TRACE( "%p %u\n", partition, vp );
    return call( unix_delete_vp, &params, &params.hr );
}

HRESULT WINAPI WHvRunVirtualProcessor( WHV_PARTITION_HANDLE partition, UINT32 vp, void *exit, UINT32 size )
{
    struct run_params params = { handle( partition ), vp, exit, size };

    if (!exit) return E_POINTER;
    return call( unix_run_vp, &params, &params.hr );
}

HRESULT WINAPI WHvCancelRunVirtualProcessor( WHV_PARTITION_HANDLE partition, UINT32 vp, UINT32 flags )
{
    struct vp_params params = { handle( partition ), vp, flags };

    return call( unix_cancel_run_vp, &params, &params.hr );
}

HRESULT WINAPI WHvGetVirtualProcessorRegisters( WHV_PARTITION_HANDLE partition, UINT32 vp,
                                                const WHV_REGISTER_NAME *names, UINT32 count,
                                                WHV_REGISTER_VALUE *values )
{
    struct registers_params params = { handle( partition ), vp, names, count, values };

    if (!names || !values) return E_POINTER;
    return call( unix_get_registers, &params, &params.hr );
}

HRESULT WINAPI WHvSetVirtualProcessorRegisters( WHV_PARTITION_HANDLE partition, UINT32 vp,
                                                const WHV_REGISTER_NAME *names, UINT32 count,
                                                const WHV_REGISTER_VALUE *values )
{
    struct registers_params params = { handle( partition ), vp, names, count, (void *)values };

    if (!names || !values) return E_POINTER;
    return call( unix_set_registers, &params, &params.hr );
}

HRESULT WINAPI WHvGetVirtualProcessorCpuidOutput( WHV_PARTITION_HANDLE partition, UINT32 vp, UINT32 eax,
                                                  UINT32 ecx, WHV_CPUID_OUTPUT *output )
{
    struct cpuid_params params = { handle( partition ), vp, eax, ecx, output };

    if (!output) return E_POINTER;
    return call( unix_get_cpuid_output, &params, &params.hr );
}

HRESULT WINAPI WHvRequestInterrupt( WHV_PARTITION_HANDLE partition, const WHV_INTERRUPT_CONTROL *control, UINT32 size )
{
    struct interrupt_params params = { handle( partition ), control, size };

    if (!control) return E_POINTER;
    return call( unix_request_interrupt, &params, &params.hr );
}

static HRESULT get_apic_state( WHV_PARTITION_HANDLE partition, UINT32 vp, void *state, UINT32 size, UINT32 *written )
{
    struct apic_params params = { handle( partition ), vp, state, size, written };

    if (!state) return E_POINTER;
    return call( unix_get_apic_state, &params, &params.hr );
}

static HRESULT set_apic_state( WHV_PARTITION_HANDLE partition, UINT32 vp, const void *state, UINT32 size )
{
    struct apic_params params = { handle( partition ), vp, (void *)state, size };

    if (!state) return E_POINTER;
    return call( unix_set_apic_state, &params, &params.hr );
}

/* the hypervisor's APIC: its register page, as both versions of the call give it */
HRESULT WINAPI WHvGetVirtualProcessorInterruptControllerState( WHV_PARTITION_HANDLE partition, UINT32 vp,
                                                               void *state, UINT32 size, UINT32 *written )
{
    return get_apic_state( partition, vp, state, size, written );
}

HRESULT WINAPI WHvSetVirtualProcessorInterruptControllerState( WHV_PARTITION_HANDLE partition, UINT32 vp,
                                                               const void *state, UINT32 size )
{
    return set_apic_state( partition, vp, state, size );
}

HRESULT WINAPI WHvGetVirtualProcessorInterruptControllerState2( WHV_PARTITION_HANDLE partition, UINT32 vp,
                                                                void *state, UINT32 size, UINT32 *written )
{
    return get_apic_state( partition, vp, state, size, written );
}

HRESULT WINAPI WHvSetVirtualProcessorInterruptControllerState2( WHV_PARTITION_HANDLE partition, UINT32 vp,
                                                                const void *state, UINT32 size )
{
    return set_apic_state( partition, vp, state, size );
}

HRESULT WINAPI WHvGetVirtualProcessorXsaveState( WHV_PARTITION_HANDLE partition, UINT32 vp, void *buffer,
                                                 UINT32 size, UINT32 *written )
{
    struct apic_params params = { handle( partition ), vp, buffer, size, written };

    if (!buffer) return E_POINTER;
    return call( unix_get_xsave_state, &params, &params.hr );
}

HRESULT WINAPI WHvSetVirtualProcessorXsaveState( WHV_PARTITION_HANDLE partition, UINT32 vp, const void *buffer,
                                                 UINT32 size )
{
    struct apic_params params = { handle( partition ), vp, (void *)buffer, size };

    if (!buffer) return E_POINTER;
    return call( unix_set_xsave_state, &params, &params.hr );
}

HRESULT WINAPI WHvGetVirtualProcessorState( WHV_PARTITION_HANDLE partition, UINT32 vp,
                                            WHV_VIRTUAL_PROCESSOR_STATE_TYPE type, void *buffer, UINT32 size,
                                            UINT32 *written )
{
    switch (type)
    {
    case WHvVirtualProcessorStateTypeXsaveState:
        return WHvGetVirtualProcessorXsaveState( partition, vp, buffer, size, written );
    case WHvVirtualProcessorStateTypeInterruptControllerState2:
        return get_apic_state( partition, vp, buffer, size, written );
    default:
        FIXME( "state %#x\n", type );
        return E_NOTIMPL;
    }
}

HRESULT WINAPI WHvSetVirtualProcessorState( WHV_PARTITION_HANDLE partition, UINT32 vp,
                                            WHV_VIRTUAL_PROCESSOR_STATE_TYPE type, const void *buffer, UINT32 size )
{
    switch (type)
    {
    case WHvVirtualProcessorStateTypeXsaveState:
        return WHvSetVirtualProcessorXsaveState( partition, vp, buffer, size );
    case WHvVirtualProcessorStateTypeInterruptControllerState2:
        return set_apic_state( partition, vp, buffer, size );
    default:
        FIXME( "state %#x\n", type );
        return E_NOTIMPL;
    }
}

/* the partition's time runs with the host's: nothing to stop or start */
HRESULT WINAPI WHvSuspendPartitionTime( WHV_PARTITION_HANDLE partition )
{
    return S_OK;
}

HRESULT WINAPI WHvResumePartitionTime( WHV_PARTITION_HANDLE partition )
{
    return S_OK;
}

/* What dirty page tracking,
 * virtual PCI devices, SynIC and migration do: not here yet. Programs are
 * told so, as on a Windows whose hypervisor lacks them. */
#define NOT_YET(name, args) \
    HRESULT WINAPI name args \
    { \
        FIXME( "not supported\n" ); \
        return E_NOTIMPL; \
    }

NOT_YET( WHvResetPartition, (WHV_PARTITION_HANDLE p) )
NOT_YET( WHvQueryGpaRangeDirtyBitmap, (WHV_PARTITION_HANDLE p, WHV_GUEST_PHYSICAL_ADDRESS a, UINT64 n, UINT64 *b, UINT32 s) )
NOT_YET( WHvGetPartitionCounters, (WHV_PARTITION_HANDLE p, WHV_PARTITION_COUNTER_SET c, void *b, UINT32 n, UINT32 *w) )
NOT_YET( WHvGetVirtualProcessorCounters, (WHV_PARTITION_HANDLE p, UINT32 vp, WHV_PROCESSOR_COUNTER_SET c, void *b, UINT32 n, UINT32 *w) )
NOT_YET( WHvRegisterPartitionDoorbellEvent, (WHV_PARTITION_HANDLE p, const WHV_DOORBELL_MATCH_DATA *d, HANDLE e) )
NOT_YET( WHvUnregisterPartitionDoorbellEvent, (WHV_PARTITION_HANDLE p, const WHV_DOORBELL_MATCH_DATA *d) )
NOT_YET( WHvAdviseGpaRange, (WHV_PARTITION_HANDLE p, const WHV_MEMORY_RANGE_ENTRY *r, UINT32 n, WHV_ADVISE_GPA_RANGE_CODE c, const void *b, UINT32 s) )
NOT_YET( WHvReadGpaRange, (WHV_PARTITION_HANDLE p, UINT32 vp, WHV_GUEST_PHYSICAL_ADDRESS a, WHV_ACCESS_GPA_CONTROLS c, void *d, UINT32 n) )
NOT_YET( WHvWriteGpaRange, (WHV_PARTITION_HANDLE p, UINT32 vp, WHV_GUEST_PHYSICAL_ADDRESS a, WHV_ACCESS_GPA_CONTROLS c, const void *d, UINT32 n) )
NOT_YET( WHvSignalVirtualProcessorSynicEvent, (WHV_PARTITION_HANDLE p, WHV_SYNIC_EVENT_PARAMETERS e, BOOL *n) )
NOT_YET( WHvPostVirtualProcessorSynicMessage, (WHV_PARTITION_HANDLE p, UINT32 vp, UINT32 s, const void *m, UINT32 n) )
NOT_YET( WHvGetInterruptTargetVpSet, (WHV_PARTITION_HANDLE p, UINT64 d, WHV_INTERRUPT_DESTINATION_MODE m, UINT32 *v, UINT32 n, UINT32 *w) )
NOT_YET( WHvAllocateVpciResource, (const GUID *g, WHV_ALLOCATE_VPCI_RESOURCE_FLAGS f, const void *d, UINT32 n, HANDLE *h) )
NOT_YET( WHvCreateVpciDevice, (WHV_PARTITION_HANDLE p, UINT64 id, HANDLE r, WHV_CREATE_VPCI_DEVICE_FLAGS f, HANDLE e) )
NOT_YET( WHvDeleteVpciDevice, (WHV_PARTITION_HANDLE p, UINT64 id) )
NOT_YET( WHvGetVpciDeviceProperty, (WHV_PARTITION_HANDLE p, UINT64 id, WHV_VPCI_DEVICE_PROPERTY_CODE c, void *b, UINT32 n, UINT32 *w) )
NOT_YET( WHvGetVpciDeviceNotification, (WHV_PARTITION_HANDLE p, UINT64 id, WHV_VPCI_DEVICE_NOTIFICATION *n, UINT32 s) )
NOT_YET( WHvMapVpciDeviceMmioRanges, (WHV_PARTITION_HANDLE p, UINT64 id, UINT32 *n, WHV_VPCI_MMIO_MAPPING **m) )
NOT_YET( WHvUnmapVpciDeviceMmioRanges, (WHV_PARTITION_HANDLE p, UINT64 id) )
NOT_YET( WHvSetVpciDevicePowerState, (WHV_PARTITION_HANDLE p, UINT64 id, DEVICE_POWER_STATE s) )
NOT_YET( WHvReadVpciDeviceRegister, (WHV_PARTITION_HANDLE p, UINT64 id, const WHV_VPCI_DEVICE_REGISTER *r, void *d) )
NOT_YET( WHvWriteVpciDeviceRegister, (WHV_PARTITION_HANDLE p, UINT64 id, const WHV_VPCI_DEVICE_REGISTER *r, const void *d) )
NOT_YET( WHvMapVpciDeviceInterrupt, (WHV_PARTITION_HANDLE p, UINT64 id, UINT32 i, UINT32 c, const WHV_VPCI_INTERRUPT_TARGET *t, UINT64 *a, UINT32 *d) )
NOT_YET( WHvUnmapVpciDeviceInterrupt, (WHV_PARTITION_HANDLE p, UINT64 id, UINT32 i) )
NOT_YET( WHvRetargetVpciDeviceInterrupt, (WHV_PARTITION_HANDLE p, UINT64 id, UINT64 a, UINT32 d, const WHV_VPCI_INTERRUPT_TARGET *t) )
NOT_YET( WHvRequestVpciDeviceInterrupt, (WHV_PARTITION_HANDLE p, UINT64 id, UINT64 a, UINT32 d) )
NOT_YET( WHvGetVpciDeviceInterruptTarget, (WHV_PARTITION_HANDLE p, UINT64 id, UINT32 i, UINT32 c, WHV_VPCI_INTERRUPT_TARGET *t, UINT32 n, UINT32 *w) )
NOT_YET( WHvCreateTrigger, (WHV_PARTITION_HANDLE p, const WHV_TRIGGER_PARAMETERS *t, WHV_TRIGGER_HANDLE *h, HANDLE *e) )
NOT_YET( WHvUpdateTriggerParameters, (WHV_PARTITION_HANDLE p, const WHV_TRIGGER_PARAMETERS *t, WHV_TRIGGER_HANDLE h) )
NOT_YET( WHvDeleteTrigger, (WHV_PARTITION_HANDLE p, WHV_TRIGGER_HANDLE h) )
NOT_YET( WHvCreateNotificationPort, (WHV_PARTITION_HANDLE p, const WHV_NOTIFICATION_PORT_PARAMETERS *n, HANDLE e, WHV_NOTIFICATION_PORT_HANDLE *h) )
NOT_YET( WHvSetNotificationPortProperty, (WHV_PARTITION_HANDLE p, WHV_NOTIFICATION_PORT_HANDLE h, WHV_NOTIFICATION_PORT_PROPERTY_CODE c, WHV_NOTIFICATION_PORT_PROPERTY v) )
NOT_YET( WHvDeleteNotificationPort, (WHV_PARTITION_HANDLE p, WHV_NOTIFICATION_PORT_HANDLE h) )
NOT_YET( WHvStartPartitionMigration, (WHV_PARTITION_HANDLE p, HANDLE *h) )
NOT_YET( WHvCancelPartitionMigration, (WHV_PARTITION_HANDLE p) )
NOT_YET( WHvCompletePartitionMigration, (WHV_PARTITION_HANDLE p) )
NOT_YET( WHvAcceptPartitionMigration, (HANDLE h, WHV_PARTITION_HANDLE *p) )

#endif  /* __x86_64__ */
