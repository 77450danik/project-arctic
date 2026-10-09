/*
 * Windows Hypervisor Platform: the partitions are KVM virtual machines
 *
 * A partition is a KVM VM, its guest memory KVM memory slots that point
 * at the program's own pages, a virtual processor a KVM vCPU. The program
 * sees the exits of Windows' hypervisor:
 *
 *  - port I/O, MMIO and MSR accesses: KVM has decoded the instruction and
 *    would finish it itself on the next KVM_RUN, where Windows leaves the
 *    instruction to the program (QEMU emulates it, sets the registers and
 *    moves RIP). The access KVM waits for is finished at once with nothing
 *    in it (KVM_RUN with immediate_exit), the registers go back to what they
 *    were before the instruction, and the program gets the exit as Windows
 *    gives it: the instruction's bytes, its length, RAX, the port.
 *  - the program's own interrupt controller (WHvCapabilityCodeFeatures
 *    says the hypervisor has none): interrupts come in through the pending
 *    interruption register (KVM_INTERRUPT), the interrupt window is KVM's,
 *    with the priority Windows' deliverability notification gives (CR8
 *    lowered: KVM_EXIT_SET_TPR).
 *  - the APIC base MSR's writes and the MSRs KVM does not know exit to the
 *    program (KVM_CAP_X86_USER_SPACE_MSR and an MSR filter).
 *  - CPUID is KVM's table of what the processor can do in a guest, with
 *    the processor count, APIC IDs and the hypervisor bit of the partition.
 *  - WHvCancelRunVirtualProcessor: immediate_exit and a signal to the
 *    thread in KVM_RUN.
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

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <cpuid.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/kvm.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "winhvplatform.h"
#include "wine/debug.h"

#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(winhvplatform);

#ifdef __x86_64__

#define MAX_VPS    256
#define MAX_SLOTS  256
#define MAX_CPUID  256
#define IOAPIC_PINS 24

/* where KVM keeps what VMX needs for real mode, as QEMU puts it */
#define IDENTITY_MAP_ADDR 0xfeffc000
#define TSS_ADDR          0xfeffd000

#define EFER_LMA (1u << 10)
#define MSR_IA32_TSC            0x00000010
#define MSR_IA32_APICBASE       0x0000001b
#define MSR_IA32_SPEC_CTRL      0x00000048
#define MSR_IA32_SYSENTER_CS    0x00000174
#define MSR_IA32_SYSENTER_ESP   0x00000175
#define MSR_IA32_SYSENTER_EIP   0x00000176
#define MSR_IA32_CR_PAT         0x00000277
#define MSR_STAR                0xc0000081
#define MSR_LSTAR               0xc0000082
#define MSR_CSTAR               0xc0000083
#define MSR_SFMASK              0xc0000084
#define MSR_KERNEL_GS_BASE      0xc0000102
#define MSR_TSC_AUX             0xc0000103

struct slot
{
    BOOL   used;
    UINT64 gpa, size;
    char  *hva;
    UINT32 flags;
};

struct vp
{
    int                fd;
    struct kvm_run    *run;
    size_t             run_size;
    UINT32             apic_id;
    BOOL               ran;              /* KVM_RUN once: the CPUID table is fixed */
    volatile LONG      cancel;
    volatile pid_t     tid;              /* the thread in WHvRunVirtualProcessor */
    WHV_X64_DELIVERABILITY_NOTIFICATIONS_REGISTER notify;
    /* a port access KVM waits for: the program does it an element at a
     * time through simple IN/OUT exits, then KVM finishes the instruction */
    BOOL               pio, pio_waiting, pio_in;
    UINT32             pio_index, pio_count, pio_size;
    UINT16             pio_port;
    UINT64             pio_value;        /* what the program put in RAX for an IN */
    /* a string IN done by the program all at once (vp->pio_batch): it
     * writes where ES:RDI points, which is saved before and put back after */
    BOOL               pio_batch;
    UINT64             pio_linear;       /* the lowest byte the program writes */
    UINT32             pio_bytes;
    BOOL               pio_down;         /* DF: the elements go down from RDI */
    UINT8              pio_save[1024];
    /* an MMIO access KVM waits for: the program does it as a plain MOV to
     * the guest physical address, in a flat 32-bit context it is shown */
    BOOL               mmio, mmio_waiting, mmio_write;
    UINT64             mmio_gpa;
    UINT32             mmio_len, mmio_offset, mmio_piece;
    UINT8              mmio_data[8];
    UINT64             mmio_value;
    /* an MSR access KVM waits for */
    BOOL               msr_waiting;
    UINT64             msr_rip, msr_new_rip, msr_rax, msr_rdx;
};

struct partition
{
    int                vm;
    BOOL               setup;
    UINT32             count;            /* WHvPartitionPropertyCodeProcessorCount */
    WHV_EXTENDED_VM_EXITS exits;
    UINT64             exception_bitmap;
    WHV_X64_MSR_EXIT_BITMAP msr_exits;
    WHV_PROCESSOR_FEATURES_BANKS features;
    WHV_X64_CPUID_RESULT *results;       /* WHvPartitionPropertyCodeCpuidResultList */
    UINT32             result_count;
    UINT32            *cpuid_exits;
    UINT32             cpuid_exit_count;
    WHV_X64_LOCAL_APIC_EMULATION_MODE apic_mode;   /* not None: KVM's split irqchip */
    struct { UINT64 address; UINT32 data; } routes[IOAPIC_PINS];   /* level-triggered MSIs, for EOI exits */
    UINT32             route_count, route_next;
    pthread_mutex_t    lock;
    struct slot        slots[MAX_SLOTS];
    struct vp         *vps[MAX_VPS];
    struct vp         *parked[MAX_VPS];   /* deleted: KVM keeps a vCPU as long as its VM */
};

static int kvm = -1;
static int kick_signal;
static struct kvm_cpuid2 *supported;
static pthread_once_t kvm_once = PTHREAD_ONCE_INIT;

static void kick_handler( int sig )
{
    /* only to interrupt KVM_RUN */
}

static void kvm_init(void)
{
    struct sigaction sa;
    int nent;

    if ((kvm = open( "/dev/kvm", O_RDWR | O_CLOEXEC )) < 0)
    {
        WARN( "no /dev/kvm: %s\n", strerror( errno ) );
        return;
    }
    if (ioctl( kvm, KVM_GET_API_VERSION, 0 ) != KVM_API_VERSION)
    {
        WARN( "KVM API version %d\n", ioctl( kvm, KVM_GET_API_VERSION, 0 ) );
        close( kvm );
        kvm = -1;
        return;
    }
    for (nent = 64; nent <= 4096; nent *= 2)
    {
        supported = calloc( 1, sizeof(*supported) + nent * sizeof(supported->entries[0]) );
        supported->nent = nent;
        if (!ioctl( kvm, KVM_GET_SUPPORTED_CPUID, supported )) break;
        free( supported );
        supported = NULL;
        if (errno != E2BIG) break;
    }
    kick_signal = SIGRTMIN + 7;
    memset( &sa, 0, sizeof(sa) );
    sa.sa_handler = kick_handler;
    sigemptyset( &sa.sa_mask );
    sigaction( kick_signal, &sa, NULL );
    MESSAGE( "winhvplatform: Windows Hypervisor Platform on KVM, %u CPUID leaves\n",
             supported ? supported->nent : 0 );
}

static BOOL kvm_ready(void)
{
    pthread_once( &kvm_once, kvm_init );
    return kvm >= 0 && supported;
}

static HRESULT hr_errno( int err )
{
    switch (err)
    {
    case ENOMEM: return E_OUTOFMEMORY;
    case EINVAL: return E_INVALIDARG;
    case EEXIST: return E_INVALIDARG;
    case EPERM:
    case EACCES: return E_ACCESSDENIED;
    default:     return E_FAIL;
    }
}

static struct partition *get_partition( UINT64 handle )
{
    return (struct partition *)(UINT_PTR)handle;
}

static struct vp *get_vp( struct partition *partition, UINT32 index )
{
    if (!partition || index >= MAX_VPS) return NULL;
    return partition->vps[index];
}

static void cpuid( UINT32 leaf, UINT32 sub, UINT32 regs[4] )
{
    __cpuid_count( leaf, sub, regs[0], regs[1], regs[2], regs[3] );
}

/**********************************************************************
 *          Capabilities
 */

static UINT64 tsc_hz(void)
{
    static UINT64 hz;
    int vm, vcpu;
    long khz;

    if (hz || !kvm_ready()) return hz;
    if ((vm = ioctl( kvm, KVM_CREATE_VM, 0 )) < 0) return 0;
    if ((vcpu = ioctl( vm, KVM_CREATE_VCPU, 0 )) >= 0)
    {
        if ((khz = ioctl( vcpu, KVM_GET_TSC_KHZ, 0 )) > 0) hz = (UINT64)khz * 1000;
        close( vcpu );
    }
    close( vm );
    return hz;
}

/* what the processor has, as Windows' hypervisor reports it */
static void host_features( WHV_PROCESSOR_FEATURES_BANKS *banks )
{
    UINT32 l1[4], l7[4], l7_1[4], e1[4], max, emax;
    WHV_PROCESSOR_FEATURES *f = &banks->Bank0;

    memset( banks, 0, sizeof(*banks) );
    banks->BanksCount = 2;
    cpuid( 0, 0, l1 );
    max = l1[0];
    cpuid( 1, 0, l1 );
    memset( l7, 0, sizeof(l7) );
    memset( l7_1, 0, sizeof(l7_1) );
    if (max >= 7) cpuid( 7, 0, l7 );
    cpuid( 0x80000000, 0, e1 );
    emax = e1[0];
    memset( e1, 0, sizeof(e1) );
    if (emax >= 0x80000001) cpuid( 0x80000001, 0, e1 );

    f->Sse3Support = !!(l1[2] & (1 << 0));
    f->PclmulqdqSupport = !!(l1[2] & (1 << 1));
    f->Ssse3Support = !!(l1[2] & (1 << 9));
    f->Cmpxchg16bSupport = !!(l1[2] & (1 << 13));
    f->PcidSupport = !!(l1[2] & (1 << 17));
    f->Sse4_1Support = !!(l1[2] & (1 << 19));
    f->Sse4_2Support = !!(l1[2] & (1 << 20));
    f->MovbeSupport = !!(l1[2] & (1 << 22));
    f->PopCntSupport = !!(l1[2] & (1 << 23));
    f->AesSupport = !!(l1[2] & (1 << 25));
    f->F16CSupport = !!(l1[2] & (1 << 29));
    f->RdRandSupport = !!(l1[2] & (1u << 30));
    f->RdWrFsGsSupport = !!(l7[1] & (1 << 0));
    f->Bmi1Support = !!(l7[1] & (1 << 3));
    f->HleSupport = !!(l7[1] & (1 << 4));
    f->SmepSupport = !!(l7[1] & (1 << 7));
    f->Bmi2Support = !!(l7[1] & (1 << 8));
    f->EnhancedFastStringSupport = !!(l7[1] & (1 << 9));
    f->InvpcidSupport = !!(l7[1] & (1 << 10));
    f->RtmSupport = !!(l7[1] & (1 << 11));
    f->RdSeedSupport = !!(l7[1] & (1 << 18));
    f->AdxSupport = !!(l7[1] & (1 << 19));
    f->SmapSupport = !!(l7[1] & (1 << 20));
    f->ClflushoptSupport = !!(l7[1] & (1 << 23));
    f->ClwbSupport = !!(l7[1] & (1 << 24));
    f->ShaSupport = !!(l7[1] & (1 << 29));
    f->UmipSupport = !!(l7[2] & (1 << 2));
    f->RdPidSupport = !!(l7[2] & (1 << 22));
    f->FastShortRepMovSupport = !!(l7[3] & (1 << 4));
    f->MdClearSupport = !!(l7[3] & (1 << 10));
    f->LahfSahfSupport = !!(e1[2] & (1 << 0));
    f->LzcntSupport = !!(e1[2] & (1 << 5));
    f->Sse4aSupport = !!(e1[2] & (1 << 6));
    f->MisAlignSseSupport = !!(e1[2] & (1 << 7));
    f->IntelPrefetchSupport = !!(e1[2] & (1 << 8));
    f->XopSupport = !!(e1[2] & (1 << 11));
    f->Fma4Support = !!(e1[2] & (1 << 16));
    f->MmxExtSupport = !!(e1[3] & (1 << 22));
    f->Page1GbSupport = !!(e1[3] & (1 << 26));
    f->RdtscpSupport = !!(e1[3] & (1 << 27));
    f->ExtendedAmd3DNowSupport = !!(e1[3] & (1 << 30));
    f->Amd3DNowSupport = !!(e1[3] & (1u << 31));
    f->UnrestrictedGuestSupport = 1;
    f->X87PointersSavedSupport = 1;
}

static WHV_PROCESSOR_VENDOR host_vendor(void)
{
    UINT32 r[4];

    cpuid( 0, 0, r );
    if (r[1] == 0x756e6547) return WHvProcessorVendorIntel;   /* GenuineIntel */
    if (r[1] == 0x6f677948) return WHvProcessorVendorHygon;   /* HygonGenuine */
    return WHvProcessorVendorAmd;
}

static NTSTATUS get_capability( void *args )
{
    struct capability_params *params = args;
    union
    {
        BOOL b;
        UINT8 u8;
        UINT32 u32;
        UINT64 u64;
        WHV_CAPABILITY_FEATURES features;
        WHV_EXTENDED_VM_EXITS exits;
        WHV_X64_MSR_EXIT_BITMAP msrs;
        WHV_PROCESSOR_VENDOR vendor;
        WHV_PROCESSOR_FEATURES_BANKS banks;
        WHV_PROCESSOR_XSAVE_FEATURES xsave;
    } value;
    UINT32 size, r[4];

    memset( &value, 0, sizeof(value) );
    params->hr = S_OK;
    switch (params->code)
    {
    case WHvCapabilityCodeHypervisorPresent:
        value.b = kvm_ready();
        size = sizeof(value.b);
        break;
    case WHvCapabilityCodeFeatures:
        value.features.PartialUnmap = 1;
        /* the APIC in the hypervisor: KVM's in-kernel APIC, PIC and IOAPIC
         * the program's (split irqchip) */
        value.features.LocalApicEmulation = kvm_ready() && ioctl( kvm, KVM_CHECK_EXTENSION, KVM_CAP_SPLIT_IRQCHIP ) > 0;
        size = sizeof(value.features);
        break;
    case WHvCapabilityCodeProcessorPerfmonFeatures:
        /* none: the guest's PMU is KVM's own */
        size = sizeof(value.u64);
        break;
    case WHvCapabilityCodeExtendedVmExits:
        value.exits.X64CpuidExit = 1;
        value.exits.X64MsrExit = 1;
        value.exits.ExceptionExit = 1;
        size = sizeof(value.exits);
        break;
    case WHvCapabilityCodeExceptionExitBitmap:
        value.u64 = 0xffffffff;
        size = sizeof(value.u64);
        break;
    case WHvCapabilityCodeX64MsrExitBitmap:
        value.msrs.UnhandledMsrs = 1;
        value.msrs.ApicBaseMsrWrite = 1;
        size = sizeof(value.msrs);
        break;
    case WHvCapabilityCodeProcessorVendor:
        value.vendor = host_vendor();
        size = sizeof(value.vendor);
        break;
    case WHvCapabilityCodeProcessorFeatures:
        host_features( &value.banks );
        value.u64 = value.banks.Bank0.AsUINT64;
        size = sizeof(UINT64);
        break;
    case WHvCapabilityCodeProcessorFeaturesBanks:
        host_features( &value.banks );
        size = sizeof(value.banks);
        break;
    case WHvCapabilityCodeProcessorClFlushSize:
        cpuid( 1, 0, r );
        value.u8 = (r[1] >> 8) & 0xff;
        size = sizeof(value.u8);
        break;
    case WHvCapabilityCodeProcessorXsaveFeatures:
        /* XSAVE state is not handed out yet (WHvGetVirtualProcessorXsaveState) */
        size = sizeof(value.xsave);
        break;
    case WHvCapabilityCodeProcessorClockFrequency:
        if (!(value.u64 = tsc_hz())) params->hr = WHV_E_UNKNOWN_CAPABILITY;
        size = sizeof(value.u64);
        break;
    case WHvCapabilityCodeInterruptClockFrequency:
        value.u64 = 1000000000;
        size = sizeof(value.u64);
        break;
    case WHvCapabilityCodePhysicalAddressWidth:
        cpuid( 0x80000008, 0, r );
        value.u32 = r[0] & 0xff;
        size = sizeof(value.u32);
        break;
    default:
        TRACE( "capability %#x: unknown\n", params->code );
        params->hr = WHV_E_UNKNOWN_CAPABILITY;
        return STATUS_SUCCESS;
    }
    if (FAILED(params->hr)) return STATUS_SUCCESS;
    if (params->written) *params->written = size;
    if (params->size < size)
    {
        params->hr = WHV_E_INSUFFICIENT_BUFFER;
        return STATUS_SUCCESS;
    }
    memcpy( params->buffer, &value, size );
    return STATUS_SUCCESS;
}

/**********************************************************************
 *          Partitions
 */

static NTSTATUS create_partition( void *args )
{
    struct partition_params *params = args;
    struct partition *partition;

    params->partition = 0;
    if (!kvm_ready())
    {
        params->hr = E_NOTIMPL;
        return STATUS_SUCCESS;
    }
    if (!(partition = calloc( 1, sizeof(*partition) )))
    {
        params->hr = E_OUTOFMEMORY;
        return STATUS_SUCCESS;
    }
    if ((partition->vm = ioctl( kvm, KVM_CREATE_VM, 0 )) < 0)
    {
        params->hr = hr_errno( errno );
        ERR( "KVM_CREATE_VM: %s\n", strerror( errno ) );
        free( partition );
        return STATUS_SUCCESS;
    }
    partition->count = 1;
    pthread_mutex_init( &partition->lock, NULL );
    params->partition = (UINT_PTR)partition;
    params->hr = S_OK;
    TRACE( "partition %p, VM fd %d\n", partition, partition->vm );
    return STATUS_SUCCESS;
}

static void destroy_vp( struct vp *vp )
{
    if (vp->run) munmap( vp->run, vp->run_size );
    if (vp->fd >= 0) close( vp->fd );
    free( vp );
}

static NTSTATUS delete_partition( void *args )
{
    struct partition_params *params = args;
    struct partition *partition = get_partition( params->partition );

    if (!partition)
    {
        params->hr = E_INVALIDARG;
        return STATUS_SUCCESS;
    }
    for (UINT32 i = 0; i < MAX_VPS; i++)
    {
        if (partition->vps[i]) destroy_vp( partition->vps[i] );
        if (partition->parked[i]) destroy_vp( partition->parked[i] );
    }
    close( partition->vm );
    pthread_mutex_destroy( &partition->lock );
    free( partition->results );
    free( partition->cpuid_exits );
    free( partition );
    params->hr = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS set_property( void *args )
{
    struct property_params *params = args;
    struct partition *partition = get_partition( params->partition );
    const WHV_PARTITION_PROPERTY *prop = params->in;

    params->hr = S_OK;
    if (!partition)
    {
        params->hr = E_INVALIDARG;
        return STATUS_SUCCESS;
    }
    switch (params->code)
    {
    case WHvPartitionPropertyCodeProcessorCount:
        if (partition->setup) params->hr = WHV_E_INVALID_PARTITION_CONFIG;
        else if (!prop->ProcessorCount || prop->ProcessorCount > MAX_VPS) params->hr = E_INVALIDARG;
        else partition->count = prop->ProcessorCount;
        break;
    case WHvPartitionPropertyCodeExtendedVmExits:
        partition->exits = prop->ExtendedVmExits;
        break;
    case WHvPartitionPropertyCodeExceptionExitBitmap:
        partition->exception_bitmap = prop->ExceptionExitBitmap;
        break;
    case WHvPartitionPropertyCodeX64MsrExitBitmap:
        if (partition->setup) params->hr = WHV_E_INVALID_PARTITION_CONFIG;
        else partition->msr_exits = prop->X64MsrExitBitmap;
        break;
    case WHvPartitionPropertyCodeProcessorFeatures:
        partition->features.Bank0 = prop->ProcessorFeatures;
        break;
    case WHvPartitionPropertyCodeProcessorFeaturesBanks:
        memcpy( &partition->features, &prop->ProcessorFeaturesBanks,
                min( params->size, sizeof(partition->features) ) );
        break;
    case WHvPartitionPropertyCodeCpuidExitList:
        free( partition->cpuid_exits );
        partition->cpuid_exit_count = params->size / sizeof(UINT32);
        if ((partition->cpuid_exits = malloc( params->size + 1 )))
            memcpy( partition->cpuid_exits, params->in, params->size );
        break;
    case WHvPartitionPropertyCodeCpuidResultList:
        free( partition->results );
        partition->result_count = params->size / sizeof(WHV_X64_CPUID_RESULT);
        if ((partition->results = malloc( params->size + 1 )))
            memcpy( partition->results, params->in, params->size );
        break;
    case WHvPartitionPropertyCodeLocalApicEmulationMode:
        if (partition->setup) params->hr = WHV_E_INVALID_PARTITION_CONFIG;
        else partition->apic_mode = *(const WHV_X64_LOCAL_APIC_EMULATION_MODE *)params->in;
        break;
    case WHvPartitionPropertyCodeProcessorPerfmonFeatures:
    case WHvPartitionPropertyCodeSyntheticProcessorFeaturesBanks:
        /* KVM's PMU; no Hyper-V enlightenments in the guest's CPUID */
        break;
    case WHvPartitionPropertyCodeNestedVirtualization:
        if (prop->NestedVirtualization) params->hr = WHV_E_UNSUPPORTED_HYPERVISOR_CONFIG;
        break;
    case WHvPartitionPropertyCodeSeparateSecurityDomain:
    case WHvPartitionPropertyCodeProcessorClFlushSize:
    case WHvPartitionPropertyCodeProcessorXsaveFeatures:
    case WHvPartitionPropertyCodeProcessorClockFrequency:
    case WHvPartitionPropertyCodeInterruptClockFrequency:
    case WHvPartitionPropertyCodePrimaryNumaNode:
    case WHvPartitionPropertyCodeCpuReserve:
    case WHvPartitionPropertyCodeCpuCap:
    case WHvPartitionPropertyCodeCpuWeight:
    case WHvPartitionPropertyCodeCpuGroupId:
    case WHvPartitionPropertyCodeDisableSmt:
        break;
    default:
        TRACE( "property %#x: unknown\n", params->code );
        params->hr = WHV_E_UNKNOWN_PROPERTY;
        break;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS get_property( void *args )
{
    struct property_params *params = args;
    struct partition *partition = get_partition( params->partition );
    WHV_PARTITION_PROPERTY prop;
    UINT32 size;

    params->hr = S_OK;
    if (!partition)
    {
        params->hr = E_INVALIDARG;
        return STATUS_SUCCESS;
    }
    memset( &prop, 0, sizeof(prop) );
    switch (params->code)
    {
    case WHvPartitionPropertyCodeProcessorCount:
        prop.ProcessorCount = partition->count;
        size = sizeof(prop.ProcessorCount);
        break;
    case WHvPartitionPropertyCodeExtendedVmExits:
        prop.ExtendedVmExits = partition->exits;
        size = sizeof(prop.ExtendedVmExits);
        break;
    case WHvPartitionPropertyCodeExceptionExitBitmap:
        prop.ExceptionExitBitmap = partition->exception_bitmap;
        size = sizeof(prop.ExceptionExitBitmap);
        break;
    case WHvPartitionPropertyCodeX64MsrExitBitmap:
        prop.X64MsrExitBitmap = partition->msr_exits;
        size = sizeof(prop.X64MsrExitBitmap);
        break;
    case WHvPartitionPropertyCodeProcessorFeaturesBanks:
        if (!partition->features.BanksCount) host_features( &partition->features );
        prop.ProcessorFeaturesBanks = partition->features;
        size = sizeof(prop.ProcessorFeaturesBanks);
        break;
    case WHvPartitionPropertyCodeProcessorXsaveFeatures:
        size = sizeof(prop.ProcessorXsaveFeatures);
        break;
    case WHvPartitionPropertyCodeLocalApicEmulationMode:
        prop.LocalApicEmulationMode = partition->apic_mode;
        size = sizeof(prop.LocalApicEmulationMode);
        break;
    case WHvPartitionPropertyCodeProcessorClockFrequency:
        prop.ProcessorClockFrequency = tsc_hz();
        size = sizeof(prop.ProcessorClockFrequency);
        break;
    case WHvPartitionPropertyCodeSeparateSecurityDomain:
    case WHvPartitionPropertyCodeNestedVirtualization:
        size = sizeof(prop.SeparateSecurityDomain);
        break;
    default:
        params->hr = WHV_E_UNKNOWN_PROPERTY;
        return STATUS_SUCCESS;
    }
    if (params->written) *params->written = size;
    if (params->size < size) params->hr = WHV_E_INSUFFICIENT_BUFFER;
    else memcpy( params->out, &prop, size );
    return STATUS_SUCCESS;
}

static NTSTATUS setup_partition( void *args )
{
    struct partition_params *params = args;
    struct partition *partition = get_partition( params->partition );
    UINT64 identity = IDENTITY_MAP_ADDR;

    if (!partition)
    {
        params->hr = E_INVALIDARG;
        return STATUS_SUCCESS;
    }
    params->hr = S_OK;
    if (partition->setup) return STATUS_SUCCESS;

    if (ioctl( partition->vm, KVM_SET_IDENTITY_MAP_ADDR, &identity ) < 0)
        WARN( "KVM_SET_IDENTITY_MAP_ADDR: %s\n", strerror( errno ) );
    if (ioctl( partition->vm, KVM_SET_TSS_ADDR, TSS_ADDR ) < 0)
        WARN( "KVM_SET_TSS_ADDR: %s\n", strerror( errno ) );

    /* the APIC in the kernel, the PIC and the IOAPIC the program's */
    if (partition->apic_mode != WHvX64LocalApicEmulationModeNone)
    {
        struct kvm_enable_cap cap = { .cap = KVM_CAP_SPLIT_IRQCHIP };

        cap.args[0] = IOAPIC_PINS;
        if (ioctl( partition->vm, KVM_ENABLE_CAP, &cap ) < 0)
        {
            ERR( "KVM_CAP_SPLIT_IRQCHIP: %s\n", strerror( errno ) );
            params->hr = WHV_E_UNSUPPORTED_HYPERVISOR_CONFIG;
            return STATUS_SUCCESS;
        }
        memset( &cap, 0, sizeof(cap) );
        cap.cap = KVM_CAP_X2APIC_API;
        cap.args[0] = KVM_X2APIC_API_USE_32BIT_IDS | KVM_X2APIC_API_DISABLE_BROADCAST_QUIRK;
        if (ioctl( partition->vm, KVM_ENABLE_CAP, &cap ) < 0) WARN( "KVM_CAP_X2APIC_API: %s\n", strerror( errno ) );
    }

    /* MSRs KVM does not handle, and the APIC base's writes, go to the program */
    if (partition->exits.X64MsrExit || partition->msr_exits.AsUINT64)
    {
        struct kvm_enable_cap cap = { .cap = KVM_CAP_X86_USER_SPACE_MSR };

        cap.args[0] = KVM_MSR_EXIT_REASON_INVAL | KVM_MSR_EXIT_REASON_UNKNOWN | KVM_MSR_EXIT_REASON_FILTER;
        if (ioctl( partition->vm, KVM_ENABLE_CAP, &cap ) < 0)
            WARN( "KVM_CAP_X86_USER_SPACE_MSR: %s\n", strerror( errno ) );
        else if (partition->msr_exits.ApicBaseMsrWrite)
        {
            static UINT8 deny;   /* a clear bit: the access exits */
            struct kvm_msr_filter filter = { .flags = KVM_MSR_FILTER_DEFAULT_ALLOW };

            filter.ranges[0].flags = KVM_MSR_FILTER_WRITE;
            filter.ranges[0].nmsrs = 1;
            filter.ranges[0].base = MSR_IA32_APICBASE;
            filter.ranges[0].bitmap = &deny;
            if (ioctl( partition->vm, KVM_X86_SET_MSR_FILTER, &filter ) < 0)
                WARN( "KVM_X86_SET_MSR_FILTER: %s\n", strerror( errno ) );
        }
    }
    partition->setup = TRUE;
    TRACE( "partition %p: %u processors, APIC %s\n", partition, partition->count,
           partition->apic_mode ? "in KVM" : "the program's" );
    return STATUS_SUCCESS;
}

/**********************************************************************
 *          Guest memory
 */

static int add_slot( struct partition *partition, UINT64 gpa, UINT64 size, char *hva, UINT32 flags )
{
    struct kvm_userspace_memory_region region = { 0 };
    int i;

    for (i = 0; i < MAX_SLOTS; i++) if (!partition->slots[i].used) break;
    if (i == MAX_SLOTS) return ENOSPC;
    region.slot = i;
    region.guest_phys_addr = gpa;
    region.memory_size = size;
    region.userspace_addr = (UINT_PTR)hva;
    if (!(flags & WHvMapGpaRangeFlagWrite)) region.flags |= KVM_MEM_READONLY;
    if (flags & WHvMapGpaRangeFlagTrackDirtyPages) region.flags |= KVM_MEM_LOG_DIRTY_PAGES;
    if (ioctl( partition->vm, KVM_SET_USER_MEMORY_REGION, &region ) < 0) return errno;
    partition->slots[i].used = TRUE;
    partition->slots[i].gpa = gpa;
    partition->slots[i].size = size;
    partition->slots[i].hva = hva;
    partition->slots[i].flags = flags;
    return 0;
}

static void remove_slot( struct partition *partition, int i )
{
    struct kvm_userspace_memory_region region = { .slot = i };

    region.guest_phys_addr = partition->slots[i].gpa;
    if (ioctl( partition->vm, KVM_SET_USER_MEMORY_REGION, &region ) < 0)
        WARN( "slot %d: %s\n", i, strerror( errno ) );
    partition->slots[i].used = FALSE;
}

static NTSTATUS map_gpa_range( void *args )
{
    struct map_params *params = args;
    struct partition *partition = get_partition( params->partition );
    int err;

    if (!partition || !params->size || (params->gpa | params->size | (UINT_PTR)params->source) & 0xfff)
    {
        params->hr = E_INVALIDARG;
        return STATUS_SUCCESS;
    }
    pthread_mutex_lock( &partition->lock );
    err = add_slot( partition, params->gpa, params->size, params->source, params->flags );
    pthread_mutex_unlock( &partition->lock );
    if (err) WARN( "%#llx-%#llx: %s\n", (long long)params->gpa, (long long)(params->gpa + params->size), strerror( err ) );
    params->hr = err ? hr_errno( err ) : S_OK;
    return STATUS_SUCCESS;
}

/* what is left of a slot around the range goes back in as slots of its own */
static NTSTATUS unmap_gpa_range( void *args )
{
    struct map_params *params = args;
    struct partition *partition = get_partition( params->partition );
    UINT64 start = params->gpa, end = params->gpa + params->size;

    if (!partition)
    {
        params->hr = E_INVALIDARG;
        return STATUS_SUCCESS;
    }
    params->hr = S_OK;
    pthread_mutex_lock( &partition->lock );
    for (int i = 0; i < MAX_SLOTS; i++)
    {
        struct slot slot = partition->slots[i];

        if (!slot.used || slot.gpa >= end || slot.gpa + slot.size <= start) continue;
        remove_slot( partition, i );
        if (slot.gpa < start) add_slot( partition, slot.gpa, start - slot.gpa, slot.hva, slot.flags );
        if (slot.gpa + slot.size > end)
            add_slot( partition, end, slot.gpa + slot.size - end, slot.hva + (end - slot.gpa), slot.flags );
    }
    pthread_mutex_unlock( &partition->lock );
    return STATUS_SUCCESS;
}

/* guest physical memory the program mapped, read for the instruction bytes */
static BOOL read_guest( struct partition *partition, UINT64 gpa, void *buffer, UINT32 size )
{
    BOOL ok = FALSE;

    pthread_mutex_lock( &partition->lock );
    for (int i = 0; i < MAX_SLOTS; i++)
    {
        struct slot *slot = &partition->slots[i];

        if (!slot->used || gpa < slot->gpa || gpa + size > slot->gpa + slot->size) continue;
        memcpy( buffer, slot->hva + (gpa - slot->gpa), size );
        ok = TRUE;
        break;
    }
    pthread_mutex_unlock( &partition->lock );
    return ok;
}

static BOOL write_guest( struct partition *partition, UINT64 gpa, const void *buffer, UINT32 size )
{
    BOOL ok = FALSE;

    pthread_mutex_lock( &partition->lock );
    for (int i = 0; i < MAX_SLOTS; i++)
    {
        struct slot *slot = &partition->slots[i];

        if (!slot->used || !(slot->flags & WHvMapGpaRangeFlagWrite)) continue;
        if (gpa < slot->gpa || gpa + size > slot->gpa + slot->size) continue;
        memcpy( slot->hva + (gpa - slot->gpa), buffer, size );
        ok = TRUE;
        break;
    }
    pthread_mutex_unlock( &partition->lock );
    return ok;
}

/* guest memory at a linear address, page by page */
static BOOL access_linear( struct partition *partition, int vcpu_fd, UINT64 linear, void *buffer, UINT32 size,
                           BOOL write )
{
    UINT32 done = 0;

    while (done < size)
    {
        struct kvm_translation tr = { .linear_address = linear + done };
        UINT32 chunk;

        if (ioctl( vcpu_fd, KVM_TRANSLATE, &tr ) < 0 || !tr.valid) return FALSE;
        chunk = min( size - done, 0x1000 - (tr.physical_address & 0xfff) );
        if (write ? !write_guest( partition, tr.physical_address, (char *)buffer + done, chunk )
                  : !read_guest( partition, tr.physical_address, (char *)buffer + done, chunk ))
            return FALSE;
        done += chunk;
    }
    return TRUE;
}

static NTSTATUS translate_gva( void *args )
{
    struct translate_params *params = args;
    struct vp *vp = get_vp( get_partition( params->partition ), params->vp );
    WHV_TRANSLATE_GVA_RESULT *result = params->result;
    struct kvm_translation tr = { .linear_address = params->gva };

    if (!vp)
    {
        params->hr = WHV_E_VP_DOES_NOT_EXIST;
        return STATUS_SUCCESS;
    }
    memset( result, 0, sizeof(*result) );
    if (ioctl( vp->fd, KVM_TRANSLATE, &tr ) < 0)
    {
        params->hr = hr_errno( errno );
        return STATUS_SUCCESS;
    }
    if (!tr.valid) result->ResultCode = WHvTranslateGvaResultPageNotPresent;
    else if ((params->flags & WHvTranslateGvaFlagValidateWrite) && !tr.writeable)
        result->ResultCode = WHvTranslateGvaResultPrivilegeViolation;
    else
    {
        result->ResultCode = WHvTranslateGvaResultSuccess;
        *params->gpa = tr.physical_address;
    }
    params->hr = S_OK;
    return STATUS_SUCCESS;
}

/**********************************************************************
 *          CPUID
 */

static struct kvm_cpuid_entry2 *find_leaf( struct kvm_cpuid2 *table, UINT32 function, UINT32 index )
{
    for (UINT32 i = 0; i < table->nent; i++)
    {
        struct kvm_cpuid_entry2 *e = &table->entries[i];

        if (e->function != function) continue;
        if ((e->flags & KVM_CPUID_FLAG_SIGNIFCANT_INDEX) && e->index != index) continue;
        return e;
    }
    return NULL;
}

/* KVM's table for a guest, with the partition's processors and the vCPU's APIC ID */
static struct kvm_cpuid2 *build_cpuid( struct partition *partition, struct vp *vp )
{
    struct kvm_cpuid2 *table;
    UINT32 count = partition->count, shift = 0;

    while ((1u << shift) < count) shift++;
    if (!(table = malloc( sizeof(*table) + supported->nent * sizeof(table->entries[0]) ))) return NULL;
    memcpy( table, supported, sizeof(*table) + supported->nent * sizeof(table->entries[0]) );

    for (UINT32 i = 0; i < table->nent; i++)
    {
        struct kvm_cpuid_entry2 *e = &table->entries[i];

        switch (e->function)
        {
        case 1:
            e->ebx = (e->ebx & 0xff00) | (vp->apic_id << 24) | (min( count, 255 ) << 16);
            if (count > 1) e->edx |= 1 << 28;   /* HTT: more than one logical processor */
            e->ecx |= 1u << 31;                  /* a hypervisor is present */
            /* OSXSAVE follows CR4; the TSC deadline timer is the in-kernel
             * APIC's, which the program's own APIC does not have */
            e->ecx &= ~(1u << 27);
            if (partition->apic_mode == WHvX64LocalApicEmulationModeNone) e->ecx &= ~(1u << 24);
            break;
        case 4:
            /* the cores of one package, a thread each */
            e->eax &= 0x3ff;
            e->eax |= (count - 1) << 26;
            if (((e->eax >> 5) & 7) >= 3) e->eax |= (count - 1) << 14;   /* the last level is shared */
            break;
        case 0xb:
        case 0x1f:
            e->edx = vp->apic_id;
            if (e->index == 0)
            {
                e->eax = 0;
                e->ebx = 1;
                e->ecx = (1 << 8) | 0;
            }
            else if (e->index == 1)
            {
                e->eax = shift;
                e->ebx = count;
                e->ecx = (2 << 8) | 1;
            }
            else e->eax = e->ebx = e->ecx = e->index;
            break;
        case 0x80000008:
            e->ecx = (e->ecx & ~0xffu) | (count - 1);
            break;
        case 0x40000001:
            /* KVM's clocks, which need no in-kernel APIC: kvmclock, the new
             * kvmclock MSRs, no I/O delay, a stable clock */
            e->eax &= (1 << 0) | (1 << 1) | (1 << 3) | (1 << 24);
            break;
        }
    }
    /* what the program asked for itself (WHvPartitionPropertyCodeCpuidResultList) */
    for (UINT32 i = 0; i < partition->result_count; i++)
    {
        const WHV_X64_CPUID_RESULT *r = &partition->results[i];
        struct kvm_cpuid_entry2 *e = find_leaf( table, r->Function, 0 );

        if (!e) continue;
        e->eax = r->Eax;
        e->ebx = r->Ebx;
        e->ecx = r->Ecx;
        e->edx = r->Edx;
    }
    return table;
}

static BOOL set_cpuid( struct partition *partition, struct vp *vp )
{
    struct kvm_cpuid2 *table = build_cpuid( partition, vp );
    BOOL ok;

    if (!table) return FALSE;
    if (!(ok = !ioctl( vp->fd, KVM_SET_CPUID2, table ))) ERR( "KVM_SET_CPUID2: %s\n", strerror( errno ) );
    free( table );
    return ok;
}

static NTSTATUS get_cpuid_output( void *args )
{
    struct cpuid_params *params = args;
    struct partition *partition = get_partition( params->partition );
    struct vp *vp = get_vp( partition, params->vp );
    WHV_CPUID_OUTPUT *out = params->output;
    struct kvm_cpuid_entry2 *e;
    struct kvm_cpuid2 *table;

    if (!vp)
    {
        params->hr = WHV_E_VP_DOES_NOT_EXIST;
        return STATUS_SUCCESS;
    }
    memset( out, 0, sizeof(*out) );
    if ((table = build_cpuid( partition, vp )))
    {
        if ((e = find_leaf( table, params->eax, params->ecx )))
        {
            out->Eax = e->eax;
            out->Ebx = e->ebx;
            out->Ecx = e->ecx;
            out->Edx = e->edx;
        }
        free( table );
    }
    params->hr = S_OK;
    return STATUS_SUCCESS;
}

/**********************************************************************
 *          Virtual processors
 */

static NTSTATUS create_vp( void *args )
{
    struct vp_params *params = args;
    struct partition *partition = get_partition( params->partition );
    struct kvm_signal_mask *mask;
    sigset_t set;
    struct vp *vp;
    int size;

    if (!partition || params->vp >= MAX_VPS || params->vp >= partition->count)
    {
        params->hr = E_INVALIDARG;
        return STATUS_SUCCESS;
    }
    if (partition->vps[params->vp])
    {
        params->hr = WHV_E_VP_ALREADY_EXISTS;
        return STATUS_SUCCESS;
    }
    /* one deleted before: KVM cannot make that vCPU again, it comes back */
    if ((vp = partition->parked[params->vp]))
    {
        partition->parked[params->vp] = NULL;
        vp->cancel = 0;
        vp->tid = 0;
        vp->notify.AsUINT64 = 0;
        vp->apic_id = params->vp;
        if (!vp->ran) set_cpuid( partition, vp );
        partition->vps[params->vp] = vp;
        params->hr = S_OK;
        return STATUS_SUCCESS;
    }
    if (!(vp = calloc( 1, sizeof(*vp) )))
    {
        params->hr = E_OUTOFMEMORY;
        return STATUS_SUCCESS;
    }
    vp->apic_id = params->vp;
    if ((vp->fd = ioctl( partition->vm, KVM_CREATE_VCPU, params->vp )) < 0 ||
        (size = ioctl( kvm, KVM_GET_VCPU_MMAP_SIZE, 0 )) <= 0 ||
        (vp->run = mmap( NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, vp->fd, 0 )) == MAP_FAILED)
    {
        params->hr = hr_errno( errno );
        ERR( "vCPU %u: %s\n", params->vp, strerror( errno ) );
        vp->run = NULL;
        destroy_vp( vp );
        return STATUS_SUCCESS;
    }
    vp->run_size = size;
    if (!set_cpuid( partition, vp ))
    {
        params->hr = E_FAIL;
        destroy_vp( vp );
        return STATUS_SUCCESS;
    }
    /* in the guest, the kick of WHvCancelRunVirtualProcessor gets through */
    pthread_sigmask( SIG_SETMASK, NULL, &set );
    sigdelset( &set, kick_signal );
    if ((mask = malloc( sizeof(*mask) + 8 )))
    {
        mask->len = 8;
        memcpy( mask->sigset, &set, 8 );
        if (ioctl( vp->fd, KVM_SET_SIGNAL_MASK, mask ) < 0) WARN( "KVM_SET_SIGNAL_MASK: %s\n", strerror( errno ) );
        free( mask );
    }
    partition->vps[params->vp] = vp;
    params->hr = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS delete_vp( void *args )
{
    struct vp_params *params = args;
    struct partition *partition = get_partition( params->partition );
    struct vp *vp = get_vp( partition, params->vp );

    if (!vp)
    {
        params->hr = WHV_E_VP_DOES_NOT_EXIST;
        return STATUS_SUCCESS;
    }
    partition->vps[params->vp] = NULL;
    partition->parked[params->vp] = vp;
    params->hr = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS cancel_run_vp( void *args )
{
    struct vp_params *params = args;
    struct vp *vp = get_vp( get_partition( params->partition ), params->vp );
    pid_t tid;

    if (!vp)
    {
        params->hr = WHV_E_VP_DOES_NOT_EXIST;
        return STATUS_SUCCESS;
    }
    vp->cancel = 1;
    __sync_synchronize();
    vp->run->immediate_exit = 1;
    __sync_synchronize();
    if ((tid = vp->tid)) syscall( SYS_tgkill, getpid(), tid, kick_signal );
    params->hr = S_OK;
    return STATUS_SUCCESS;
}

/**********************************************************************
 *          Registers
 */

enum
{
    GROUP_REGS   = 0x01,
    GROUP_SREGS  = 0x02,
    GROUP_FPU    = 0x04,
    GROUP_DREGS  = 0x08,
    GROUP_XCRS   = 0x10,
    GROUP_EVENTS = 0x20,
    GROUP_MSRS   = 0x40,
};

#define MAX_STATE_MSRS 32

struct state
{
    UINT32 have, dirty;
    struct kvm_regs regs;
    struct kvm_sregs sregs;
    struct kvm_fpu fpu;
    struct kvm_debugregs dregs;
    struct kvm_xcrs xcrs;
    struct kvm_vcpu_events events;
    struct
    {
        struct kvm_msrs hdr;
        struct kvm_msr_entry entries[MAX_STATE_MSRS];
    } msrs;
    /* what is done after the state is written: an interrupt, an NMI */
    int inject_irq, inject_nmi;
};

static UINT32 reg_msr( WHV_REGISTER_NAME name )
{
    switch (name)
    {
    case WHvX64RegisterTsc:          return MSR_IA32_TSC;
    case WHvX64RegisterKernelGsBase: return MSR_KERNEL_GS_BASE;
    case WHvX64RegisterPat:          return MSR_IA32_CR_PAT;
    case WHvX64RegisterSysenterCs:   return MSR_IA32_SYSENTER_CS;
    case WHvX64RegisterSysenterEip:  return MSR_IA32_SYSENTER_EIP;
    case WHvX64RegisterSysenterEsp:  return MSR_IA32_SYSENTER_ESP;
    case WHvX64RegisterStar:         return MSR_STAR;
    case WHvX64RegisterLstar:        return MSR_LSTAR;
    case WHvX64RegisterCstar:        return MSR_CSTAR;
    case WHvX64RegisterSfmask:       return MSR_SFMASK;
    case WHvX64RegisterTscAux:       return MSR_TSC_AUX;
    case WHvX64RegisterSpecCtrl:     return MSR_IA32_SPEC_CTRL;
    default:                         return 0;
    }
}

static UINT32 reg_group( WHV_REGISTER_NAME name )
{
    if (name <= WHvX64RegisterRflags) return GROUP_REGS;
    if (name >= WHvX64RegisterEs && name <= WHvX64RegisterCr8) return GROUP_SREGS;
    if (name >= WHvX64RegisterDr0 && name <= WHvX64RegisterDr7) return GROUP_DREGS;
    if (name == WHvX64RegisterXCr0) return GROUP_XCRS;
    if (name >= WHvX64RegisterXmm0 && name <= WHvX64RegisterXmmControlStatus) return GROUP_FPU;
    if (name == WHvX64RegisterEfer || name == WHvX64RegisterApicBase) return GROUP_SREGS;
    if (reg_msr( name )) return GROUP_MSRS;
    switch (name)
    {
    case WHvRegisterPendingInterruption:
    case WHvRegisterInterruptState:
    case WHvRegisterPendingEvent:
        return GROUP_EVENTS;
    default:
        return 0;
    }
}

static UINT64 *gp_reg( struct kvm_regs *regs, WHV_REGISTER_NAME name )
{
    switch (name)
    {
    case WHvX64RegisterRax:    return (UINT64 *)&regs->rax;
    case WHvX64RegisterRcx:    return (UINT64 *)&regs->rcx;
    case WHvX64RegisterRdx:    return (UINT64 *)&regs->rdx;
    case WHvX64RegisterRbx:    return (UINT64 *)&regs->rbx;
    case WHvX64RegisterRsp:    return (UINT64 *)&regs->rsp;
    case WHvX64RegisterRbp:    return (UINT64 *)&regs->rbp;
    case WHvX64RegisterRsi:    return (UINT64 *)&regs->rsi;
    case WHvX64RegisterRdi:    return (UINT64 *)&regs->rdi;
    case WHvX64RegisterR8:     return (UINT64 *)&regs->r8;
    case WHvX64RegisterR9:     return (UINT64 *)&regs->r9;
    case WHvX64RegisterR10:    return (UINT64 *)&regs->r10;
    case WHvX64RegisterR11:    return (UINT64 *)&regs->r11;
    case WHvX64RegisterR12:    return (UINT64 *)&regs->r12;
    case WHvX64RegisterR13:    return (UINT64 *)&regs->r13;
    case WHvX64RegisterR14:    return (UINT64 *)&regs->r14;
    case WHvX64RegisterR15:    return (UINT64 *)&regs->r15;
    case WHvX64RegisterRip:    return (UINT64 *)&regs->rip;
    default:                   return (UINT64 *)&regs->rflags;
    }
}

static struct kvm_segment *segment( struct kvm_sregs *sregs, WHV_REGISTER_NAME name )
{
    switch (name)
    {
    case WHvX64RegisterEs:   return &sregs->es;
    case WHvX64RegisterCs:   return &sregs->cs;
    case WHvX64RegisterSs:   return &sregs->ss;
    case WHvX64RegisterDs:   return &sregs->ds;
    case WHvX64RegisterFs:   return &sregs->fs;
    case WHvX64RegisterGs:   return &sregs->gs;
    case WHvX64RegisterLdtr: return &sregs->ldt;
    case WHvX64RegisterTr:   return &sregs->tr;
    default:                 return NULL;
    }
}

static void segment_to_whv( const struct kvm_segment *s, WHV_X64_SEGMENT_REGISTER *w )
{
    memset( w, 0, sizeof(*w) );
    w->Base = s->base;
    w->Limit = s->limit;
    w->Selector = s->selector;
    w->SegmentType = s->type;
    w->NonSystemSegment = s->s;
    w->DescriptorPrivilegeLevel = s->dpl;
    w->Present = s->present && !s->unusable;
    w->Available = s->avl;
    w->Long = s->l;
    w->Default = s->db;
    w->Granularity = s->g;
}

static void segment_from_whv( const WHV_X64_SEGMENT_REGISTER *w, struct kvm_segment *s )
{
    memset( s, 0, sizeof(*s) );
    s->base = w->Base;
    s->limit = w->Limit;
    s->selector = w->Selector;
    s->type = w->SegmentType;
    s->s = w->NonSystemSegment;
    s->dpl = w->DescriptorPrivilegeLevel;
    s->present = w->Present;
    s->avl = w->Available;
    s->l = w->Long;
    s->db = w->Default;
    s->g = w->Granularity;
    s->unusable = !w->Present;
}

static BOOL fetch_state( struct vp *vp, struct state *state, UINT32 groups, const WHV_REGISTER_NAME *names,
                         UINT32 count )
{
    groups &= ~state->have;
    if ((groups & GROUP_REGS) && ioctl( vp->fd, KVM_GET_REGS, &state->regs ) < 0) return FALSE;
    if ((groups & GROUP_SREGS) && ioctl( vp->fd, KVM_GET_SREGS, &state->sregs ) < 0) return FALSE;
    if ((groups & GROUP_FPU) && ioctl( vp->fd, KVM_GET_FPU, &state->fpu ) < 0) return FALSE;
    if ((groups & GROUP_DREGS) && ioctl( vp->fd, KVM_GET_DEBUGREGS, &state->dregs ) < 0) return FALSE;
    if ((groups & GROUP_XCRS) && ioctl( vp->fd, KVM_GET_XCRS, &state->xcrs ) < 0) return FALSE;
    if ((groups & GROUP_EVENTS) && ioctl( vp->fd, KVM_GET_VCPU_EVENTS, &state->events ) < 0) return FALSE;
    if (groups & GROUP_MSRS)
    {
        state->msrs.hdr.nmsrs = 0;
        for (UINT32 i = 0; i < count && state->msrs.hdr.nmsrs < MAX_STATE_MSRS; i++)
        {
            UINT32 msr = reg_msr( names[i] );
            BOOL dup = FALSE;

            if (!msr) continue;
            for (UINT32 k = 0; k < state->msrs.hdr.nmsrs; k++) dup |= state->msrs.entries[k].index == msr;
            if (dup) continue;
            state->msrs.entries[state->msrs.hdr.nmsrs].index = msr;
            state->msrs.entries[state->msrs.hdr.nmsrs].data = 0;
            state->msrs.hdr.nmsrs++;
        }
        /* MSRs the processor lacks read as 0: KVM stops at the first */
        if (ioctl( vp->fd, KVM_GET_MSRS, &state->msrs ) < 0) return FALSE;
    }
    state->have |= groups;
    return TRUE;
}

static struct kvm_msr_entry *state_msr( struct state *state, UINT32 msr )
{
    for (UINT32 i = 0; i < state->msrs.hdr.nmsrs; i++)
        if (state->msrs.entries[i].index == msr) return &state->msrs.entries[i];
    return NULL;
}

static void get_register( struct vp *vp, struct state *state, WHV_REGISTER_NAME name, WHV_REGISTER_VALUE *value )
{
    struct kvm_segment *seg;
    struct kvm_msr_entry *msr;

    memset( value, 0, sizeof(*value) );
    if (name <= WHvX64RegisterRflags)
    {
        value->Reg64 = *gp_reg( &state->regs, name );
        return;
    }
    if ((seg = segment( &state->sregs, name )))
    {
        segment_to_whv( seg, &value->Segment );
        return;
    }
    if (name >= WHvX64RegisterXmm0 && name <= WHvX64RegisterXmm15)
    {
        memcpy( &value->Reg128, state->fpu.xmm[name - WHvX64RegisterXmm0], 16 );
        return;
    }
    if (name >= WHvX64RegisterFpMmx0 && name <= WHvX64RegisterFpMmx7)
    {
        memcpy( &value->Reg128, state->fpu.fpr[name - WHvX64RegisterFpMmx0], 16 );
        return;
    }
    if ((msr = state_msr( state, reg_msr( name ) )))
    {
        value->Reg64 = msr->data;
        return;
    }
    switch (name)
    {
    case WHvX64RegisterIdtr:
        value->Table.Base = state->sregs.idt.base;
        value->Table.Limit = state->sregs.idt.limit;
        break;
    case WHvX64RegisterGdtr:
        value->Table.Base = state->sregs.gdt.base;
        value->Table.Limit = state->sregs.gdt.limit;
        break;
    case WHvX64RegisterCr0:     value->Reg64 = state->sregs.cr0; break;
    case WHvX64RegisterCr2:     value->Reg64 = state->sregs.cr2; break;
    case WHvX64RegisterCr3:     value->Reg64 = state->sregs.cr3; break;
    case WHvX64RegisterCr4:     value->Reg64 = state->sregs.cr4; break;
    case WHvX64RegisterCr8:     value->Reg64 = state->sregs.cr8; break;
    case WHvX64RegisterEfer:    value->Reg64 = state->sregs.efer; break;
    case WHvX64RegisterApicBase: value->Reg64 = state->sregs.apic_base; break;
    case WHvX64RegisterDr0:
    case WHvX64RegisterDr1:
    case WHvX64RegisterDr2:
    case WHvX64RegisterDr3:     value->Reg64 = state->dregs.db[name - WHvX64RegisterDr0]; break;
    case WHvX64RegisterDr6:     value->Reg64 = state->dregs.dr6; break;
    case WHvX64RegisterDr7:     value->Reg64 = state->dregs.dr7; break;
    case WHvX64RegisterXCr0:
        for (UINT32 i = 0; i < state->xcrs.nr_xcrs; i++)
            if (!state->xcrs.xcrs[i].xcr) value->Reg64 = state->xcrs.xcrs[i].value;
        break;
    case WHvX64RegisterFpControlStatus:
        value->FpControlStatus.FpControl = state->fpu.fcw;
        value->FpControlStatus.FpStatus = state->fpu.fsw;
        value->FpControlStatus.FpTag = state->fpu.ftwx;
        value->FpControlStatus.LastFpOp = state->fpu.last_opcode;
        value->FpControlStatus.LastFpRip = state->fpu.last_ip;
        break;
    case WHvX64RegisterXmmControlStatus:
        value->XmmControlStatus.LastFpRdp = state->fpu.last_dp;
        value->XmmControlStatus.XmmStatusControl = state->fpu.mxcsr;
        value->XmmControlStatus.XmmStatusControlMask = 0xffff;
        break;
    case WHvX64RegisterInitialApicId:
        value->Reg64 = vp->apic_id;
        break;
    case WHvRegisterPendingInterruption:
        if (state->events.interrupt.injected)
        {
            value->PendingInterruption.InterruptionPending = 1;
            value->PendingInterruption.InterruptionType = WHvX64PendingInterrupt;
            value->PendingInterruption.InterruptionVector = state->events.interrupt.nr;
        }
        else if (state->events.nmi.injected)
        {
            value->PendingInterruption.InterruptionPending = 1;
            value->PendingInterruption.InterruptionType = WHvX64PendingNmi;
            value->PendingInterruption.InterruptionVector = 2;
        }
        else if (state->events.exception.injected)
        {
            value->PendingInterruption.InterruptionPending = 1;
            value->PendingInterruption.InterruptionType = WHvX64PendingException;
            value->PendingInterruption.InterruptionVector = state->events.exception.nr;
            value->PendingInterruption.DeliverErrorCode = state->events.exception.has_error_code;
            value->PendingInterruption.ErrorCode = state->events.exception.error_code;
        }
        break;
    case WHvRegisterInterruptState:
        value->InterruptState.InterruptShadow = !!state->events.interrupt.shadow;
        value->InterruptState.NmiMasked = !!state->events.nmi.masked;
        break;
    case WHvX64RegisterDeliverabilityNotifications:
        value->DeliverabilityNotifications = vp->notify;
        break;
    case WHvRegisterInternalActivityState:
    {
        struct kvm_mp_state mp;

        if (ioctl( vp->fd, KVM_GET_MP_STATE, &mp ) < 0) break;
        value->InternalActivity.HaltSuspend = mp.mp_state == KVM_MP_STATE_HALTED;
        value->InternalActivity.StartupSuspend = mp.mp_state == KVM_MP_STATE_UNINITIALIZED ||
                                                 mp.mp_state == KVM_MP_STATE_INIT_RECEIVED;
        break;
    }
    default:
        /* pending events and what KVM has no such thing for: none */
        break;
    }
}

static void set_register( struct vp *vp, struct state *state, WHV_REGISTER_NAME name, const WHV_REGISTER_VALUE *value )
{
    struct kvm_segment *seg;
    struct kvm_msr_entry *msr;
    UINT32 group = reg_group( name );

    state->dirty |= group;
    if (name <= WHvX64RegisterRflags)
    {
        *gp_reg( &state->regs, name ) = value->Reg64;
        return;
    }
    if ((seg = segment( &state->sregs, name )))
    {
        segment_from_whv( &value->Segment, seg );
        return;
    }
    if (name >= WHvX64RegisterXmm0 && name <= WHvX64RegisterXmm15)
    {
        memcpy( state->fpu.xmm[name - WHvX64RegisterXmm0], &value->Reg128, 16 );
        return;
    }
    if (name >= WHvX64RegisterFpMmx0 && name <= WHvX64RegisterFpMmx7)
    {
        memcpy( state->fpu.fpr[name - WHvX64RegisterFpMmx0], &value->Reg128, 16 );
        return;
    }
    if ((msr = state_msr( state, reg_msr( name ) )))
    {
        msr->data = value->Reg64;
        return;
    }
    switch (name)
    {
    case WHvX64RegisterIdtr:
        state->sregs.idt.base = value->Table.Base;
        state->sregs.idt.limit = value->Table.Limit;
        break;
    case WHvX64RegisterGdtr:
        state->sregs.gdt.base = value->Table.Base;
        state->sregs.gdt.limit = value->Table.Limit;
        break;
    case WHvX64RegisterCr0:     state->sregs.cr0 = value->Reg64; break;
    case WHvX64RegisterCr2:     state->sregs.cr2 = value->Reg64; break;
    case WHvX64RegisterCr3:     state->sregs.cr3 = value->Reg64; break;
    case WHvX64RegisterCr4:     state->sregs.cr4 = value->Reg64; break;
    case WHvX64RegisterCr8:     state->sregs.cr8 = value->Reg64; break;
    case WHvX64RegisterEfer:    state->sregs.efer = value->Reg64; break;
    case WHvX64RegisterApicBase: state->sregs.apic_base = value->Reg64; break;
    case WHvX64RegisterDr0:
    case WHvX64RegisterDr1:
    case WHvX64RegisterDr2:
    case WHvX64RegisterDr3:     state->dregs.db[name - WHvX64RegisterDr0] = value->Reg64; break;
    case WHvX64RegisterDr6:     state->dregs.dr6 = value->Reg64; break;
    case WHvX64RegisterDr7:     state->dregs.dr7 = value->Reg64; break;
    case WHvX64RegisterXCr0:
        state->xcrs.nr_xcrs = 1;
        state->xcrs.flags = 0;
        state->xcrs.xcrs[0].xcr = 0;
        state->xcrs.xcrs[0].value = value->Reg64;
        break;
    case WHvX64RegisterFpControlStatus:
        state->fpu.fcw = value->FpControlStatus.FpControl;
        state->fpu.fsw = value->FpControlStatus.FpStatus;
        state->fpu.ftwx = value->FpControlStatus.FpTag;
        state->fpu.last_opcode = value->FpControlStatus.LastFpOp;
        state->fpu.last_ip = value->FpControlStatus.LastFpRip;
        break;
    case WHvX64RegisterXmmControlStatus:
        state->fpu.last_dp = value->XmmControlStatus.LastFpRdp;
        state->fpu.mxcsr = value->XmmControlStatus.XmmStatusControl;
        break;
    case WHvX64RegisterInitialApicId:
        vp->apic_id = value->Reg64;
        break;
    case WHvRegisterPendingInterruption:
        if (!value->PendingInterruption.InterruptionPending) break;
        switch (value->PendingInterruption.InterruptionType)
        {
        case WHvX64PendingInterrupt:
            state->inject_irq = value->PendingInterruption.InterruptionVector + 1;
            break;
        case WHvX64PendingNmi:
            state->inject_nmi = 1;
            break;
        default:
            state->events.exception.injected = 1;
            state->events.exception.nr = value->PendingInterruption.InterruptionVector;
            state->events.exception.has_error_code = value->PendingInterruption.DeliverErrorCode;
            state->events.exception.error_code = value->PendingInterruption.ErrorCode;
            break;
        }
        break;
    case WHvRegisterPendingEvent:
        if (!value->ExceptionEvent.EventPending) break;
        if (value->ExceptionEvent.EventType == WHvX64PendingEventExtInt)
            state->inject_irq = value->ExtIntEvent.Vector + 1;
        else if (value->ExceptionEvent.EventType == WHvX64PendingEventException)
        {
            state->events.exception.injected = 1;
            state->events.exception.nr = value->ExceptionEvent.Vector;
            state->events.exception.has_error_code = value->ExceptionEvent.DeliverErrorCode;
            state->events.exception.error_code = value->ExceptionEvent.ErrorCode;
            if (value->ExceptionEvent.Vector == 14)   /* #PF: the address in CR2 */
            {
                state->sregs.cr2 = value->ExceptionEvent.ExceptionParameter;
                state->dirty |= GROUP_SREGS;
            }
        }
        break;
    case WHvRegisterInterruptState:
        state->events.interrupt.shadow = value->InterruptState.InterruptShadow ? KVM_X86_SHADOW_INT_STI : 0;
        state->events.nmi.masked = value->InterruptState.NmiMasked;
        state->events.flags |= KVM_VCPUEVENT_VALID_SHADOW;
        break;
    case WHvX64RegisterDeliverabilityNotifications:
        vp->notify = value->DeliverabilityNotifications;
        break;
    case WHvRegisterInternalActivityState:
    {
        /* out of HLT, for an interrupt the program gives */
        struct kvm_mp_state mp;

        if (value->InternalActivity.HaltSuspend || ioctl( vp->fd, KVM_GET_MP_STATE, &mp ) < 0) break;
        if (mp.mp_state != KVM_MP_STATE_HALTED) break;
        mp.mp_state = KVM_MP_STATE_RUNNABLE;
        ioctl( vp->fd, KVM_SET_MP_STATE, &mp );
        break;
    }
    default:
        break;
    }
}

static BOOL write_state( struct partition *partition, struct vp *vp, struct state *state )
{
    UINT32 dirty = state->dirty;

    if ((dirty & GROUP_REGS) && ioctl( vp->fd, KVM_SET_REGS, &state->regs ) < 0) return FALSE;
    if (dirty & GROUP_SREGS)
    {
        if (ioctl( vp->fd, KVM_SET_SREGS, &state->sregs ) < 0) return FALSE;
        /* without an in-kernel APIC, KVM_RUN takes CR8 from here each time */
        vp->run->cr8 = state->sregs.cr8;
    }
    if ((dirty & GROUP_FPU) && ioctl( vp->fd, KVM_SET_FPU, &state->fpu ) < 0) return FALSE;
    if ((dirty & GROUP_DREGS) && ioctl( vp->fd, KVM_SET_DEBUGREGS, &state->dregs ) < 0) return FALSE;
    if ((dirty & GROUP_XCRS) && state->xcrs.nr_xcrs && ioctl( vp->fd, KVM_SET_XCRS, &state->xcrs ) < 0)
        WARN( "KVM_SET_XCRS: %s\n", strerror( errno ) );
    if ((dirty & GROUP_MSRS) && state->msrs.hdr.nmsrs && ioctl( vp->fd, KVM_SET_MSRS, &state->msrs ) < 0)
        return FALSE;
    if ((dirty & GROUP_EVENTS) && ioctl( vp->fd, KVM_SET_VCPU_EVENTS, &state->events ) < 0) return FALSE;
    if (state->inject_irq)
    {
        struct kvm_interrupt irq = { .irq = state->inject_irq - 1 };

        if (ioctl( vp->fd, KVM_INTERRUPT, &irq ) < 0)
            WARN( "KVM_INTERRUPT %u: %s\n", irq.irq, strerror( errno ) );
    }
    if (state->inject_nmi && ioctl( vp->fd, KVM_NMI, 0 ) < 0) WARN( "KVM_NMI: %s\n", strerror( errno ) );
    return TRUE;
}

/* what the program sees of the processor while it does an MMIO access */
static void fake_mmio_register( struct vp *vp, WHV_REGISTER_NAME name, WHV_REGISTER_VALUE *value )
{
    struct kvm_segment flat = { .limit = 0xffffffff, .type = 3, .present = 1, .db = 1, .s = 1, .g = 1 };

    switch (name)
    {
    case WHvX64RegisterRax:    value->Reg64 = vp->mmio_value; break;
    case WHvX64RegisterRdi:    value->Reg64 = (UINT32)(vp->mmio_gpa + vp->mmio_offset); break;
    case WHvX64RegisterRip:    value->Reg64 = 0; break;
    case WHvX64RegisterRflags: value->Reg64 = 2; break;
    case WHvX64RegisterCr0:    value->Reg64 = 0x11; break;
    case WHvX64RegisterCr3:
    case WHvX64RegisterCr4:
    case WHvX64RegisterEfer:   value->Reg64 = 0; break;
    case WHvX64RegisterCs:
        flat.type = 0xb;
        segment_to_whv( &flat, &value->Segment );
        break;
    case WHvX64RegisterDs:
    case WHvX64RegisterEs:
    case WHvX64RegisterSs:
    case WHvX64RegisterFs:
    case WHvX64RegisterGs:
        segment_to_whv( &flat, &value->Segment );
        break;
    default:
        break;
    }
}

static NTSTATUS get_registers( void *args )
{
    struct registers_params *params = args;
    struct vp *vp = get_vp( get_partition( params->partition ), params->vp );
    const WHV_REGISTER_NAME *names = params->names;
    WHV_REGISTER_VALUE *values = params->values;
    struct state *state;
    UINT32 groups = 0;

    if (!vp)
    {
        params->hr = WHV_E_VP_DOES_NOT_EXIST;
        return STATUS_SUCCESS;
    }
    if (!(state = calloc( 1, sizeof(*state) )))
    {
        params->hr = E_OUTOFMEMORY;
        return STATUS_SUCCESS;
    }
    for (UINT32 i = 0; i < params->count; i++) groups |= reg_group( names[i] );
    if (!fetch_state( vp, state, groups, names, params->count )) params->hr = hr_errno( errno );
    else
    {
        for (UINT32 i = 0; i < params->count; i++)
        {
            get_register( vp, state, names[i], &values[i] );
            /* the string IN the program does at once: as many as KVM read ahead */
            if (vp->pio_waiting && vp->pio_batch && names[i] == WHvX64RegisterRcx)
                values[i].Reg64 = vp->pio_count;
            if (vp->mmio_waiting) fake_mmio_register( vp, names[i], &values[i] );
        }
        params->hr = S_OK;
    }
    free( state );
    return STATUS_SUCCESS;
}

static NTSTATUS set_registers( void *args )
{
    struct registers_params *params = args;
    struct partition *partition = get_partition( params->partition );
    struct vp *vp = get_vp( partition, params->vp );
    const WHV_REGISTER_NAME *names = params->names;
    const WHV_REGISTER_VALUE *values = params->values;
    UINT32 groups = 0, old_apic_id;
    struct state *state;

    if (!vp)
    {
        params->hr = WHV_E_VP_DOES_NOT_EXIST;
        return STATUS_SUCCESS;
    }
    if (!(state = calloc( 1, sizeof(*state) )))
    {
        params->hr = E_OUTOFMEMORY;
        return STATUS_SUCCESS;
    }
    /* a #PF's CR2 comes with the event */
    for (UINT32 i = 0; i < params->count; i++)
    {
        groups |= reg_group( names[i] );
        if (names[i] == WHvRegisterPendingEvent) groups |= GROUP_SREGS;
    }
    old_apic_id = vp->apic_id;
    if (!fetch_state( vp, state, groups, names, params->count ))
    {
        params->hr = hr_errno( errno );
        free( state );
        return STATUS_SUCCESS;
    }
    for (UINT32 i = 0; i < params->count; i++)
    {
        /* the answer to an element of a port access: RAX is the value read,
         * RIP the program's idea of the next instruction, which KVM has */
        if (vp->pio_waiting && names[i] == WHvX64RegisterRax)
        {
            vp->pio_value = values[i].Reg64;
            continue;
        }
        if (vp->pio_waiting && names[i] == WHvX64RegisterRip) continue;
        /* the program's emulation of the batch moves RCX, RDI and RIP: KVM does that */
        if (vp->pio_waiting && vp->pio_batch && (reg_group( names[i] ) & (GROUP_REGS | GROUP_SREGS))) continue;
        if (vp->mmio_waiting && names[i] == WHvX64RegisterRax) vp->mmio_value = values[i].Reg64;
        if (vp->msr_waiting)
        {
            if (names[i] == WHvX64RegisterRax) vp->msr_rax = values[i].Reg64;
            else if (names[i] == WHvX64RegisterRdx) vp->msr_rdx = values[i].Reg64;
            else if (names[i] == WHvX64RegisterRip) vp->msr_new_rip = values[i].Reg64;
            /* the program's own #GP for it: KVM raises it */
            else if (names[i] == WHvRegisterPendingEvent && values[i].ExceptionEvent.EventPending &&
                     values[i].ExceptionEvent.EventType == WHvX64PendingEventException &&
                     values[i].ExceptionEvent.Vector == 13)
            {
                vp->msr_new_rip = vp->msr_rip;
                continue;
            }
            if (reg_group( names[i] ) & GROUP_REGS) continue;
        }
        if (vp->mmio_waiting && (reg_group( names[i] ) & (GROUP_REGS | GROUP_SREGS))) continue;
        set_register( vp, state, names[i], &values[i] );
    }
    params->hr = write_state( partition, vp, state ) ? S_OK : hr_errno( errno );
    if (vp->apic_id != old_apic_id && !vp->ran) set_cpuid( partition, vp );
    free( state );
    return STATUS_SUCCESS;
}

/**********************************************************************
 *          Running
 */

static BOOL long_mode_code( const struct kvm_sregs *sregs )
{
    return (sregs->efer & EFER_LMA) && sregs->cs.l;
}

/* the bytes at CS:RIP, as many as are mapped, up to 16 */
static UINT32 fetch_instruction( struct partition *partition, struct vp *vp, const struct kvm_sregs *sregs,
                                 UINT64 rip, UINT8 *bytes )
{
    UINT64 linear = long_mode_code( sregs ) ? rip : (UINT32)(sregs->cs.base + rip);
    UINT32 done = 0;

    while (done < 16)
    {
        struct kvm_translation tr = { .linear_address = linear + done };
        UINT32 chunk;

        if (ioctl( vp->fd, KVM_TRANSLATE, &tr ) < 0 || !tr.valid) break;
        chunk = min( 16 - done, 0x1000 - (tr.physical_address & 0xfff) );
        if (!read_guest( partition, tr.physical_address, bytes + done, chunk )) break;
        done += chunk;
    }
    return done;
}

static BOOL is_prefix( UINT8 byte, BOOL rex )
{
    switch (byte)
    {
    case 0x26: case 0x2e: case 0x36: case 0x3e: case 0x64: case 0x65:
    case 0x66: case 0x67: case 0xf0: case 0xf2: case 0xf3:
        return TRUE;
    default:
        return rex && byte >= 0x40 && byte <= 0x4f;
    }
}

/* the length of an IN/OUT/INS/OUTS from its bytes */
static UINT32 io_length( const UINT8 *bytes, UINT32 count, BOOL rex, BOOL *rep, BOOL *string )
{
    UINT32 i = 0;

    *rep = FALSE;
    while (i < count && is_prefix( bytes[i], rex ))
    {
        if (bytes[i] == 0xf3 || bytes[i] == 0xf2) *rep = TRUE;
        i++;
    }
    if (i >= count) return 1;
    *string = bytes[i] >= 0x6c && bytes[i] <= 0x6f;
    return i + 1 + (bytes[i] >= 0xe4 && bytes[i] <= 0xe7);
}

static void fill_vp_context( struct vp *vp, WHV_VP_EXIT_CONTEXT *ctx, const struct kvm_regs *regs,
                             const struct kvm_sregs *sregs, const struct kvm_vcpu_events *events )
{
    ctx->ExecutionState.Cpl = sregs->ss.dpl;
    ctx->ExecutionState.Cr0Pe = !!(sregs->cr0 & 1);
    ctx->ExecutionState.Cr0Am = !!(sregs->cr0 & (1 << 18));
    ctx->ExecutionState.EferLma = !!(sregs->efer & EFER_LMA);
    ctx->ExecutionState.InterruptionPending =
        events->interrupt.injected || events->nmi.injected || events->exception.injected;
    ctx->ExecutionState.InterruptShadow = !!events->interrupt.shadow;
    ctx->Cr8 = vp->run->cr8 & 0xf;
    segment_to_whv( &sregs->cs, &ctx->Cs );
    ctx->Rip = regs->rip;
    ctx->Rflags = regs->rflags;
}

static BOOL want_window( struct vp *vp )
{
    if (!vp->notify.InterruptNotification) return FALSE;
    /* an interrupt of this priority class gets through the TPR */
    return !vp->notify.InterruptPriority || (vp->run->cr8 & 0xf) < vp->notify.InterruptPriority;
}

static HRESULT exit_with_state( struct partition *partition, struct vp *vp, WHV_RUN_VP_EXIT_CONTEXT *ctx,
                                WHV_RUN_VP_EXIT_REASON reason )
{
    struct kvm_run *run = vp->run;
    struct kvm_regs regs;
    struct kvm_sregs sregs;
    struct kvm_vcpu_events events;
    UINT8 bytes[16];
    UINT32 count, length = 0;
    UINT64 data = 0;

    if (ioctl( vp->fd, KVM_GET_REGS, &regs ) < 0 || ioctl( vp->fd, KVM_GET_SREGS, &sregs ) < 0 ||
        ioctl( vp->fd, KVM_GET_VCPU_EVENTS, &events ) < 0)
        return hr_errno( errno );

    ctx->ExitReason = reason;
    switch (reason)
    {
    case WHvRunVpExitReasonX64IoPortAccess:
    {
        /* an element of the access KVM waits for (vp->pio), as a simple IN
         * or OUT: the program does the port access, KVM the instruction */
        const UINT8 *data = (const UINT8 *)run + run->io.data_offset + vp->pio_index * vp->pio_size;
        UINT64 value = 0;
        BOOL rep, string = FALSE;

        count = fetch_instruction( partition, vp, &sregs, regs.rip, bytes );
        length = io_length( bytes, count, long_mode_code( &sregs ), &rep, &string );

        /* a string IN of several elements: the program does it at once, into
         * the guest's memory at ES:RDI, saved before */
        if (vp->pio_in && vp->pio_count > 1 && vp->pio_index == 0 && string &&
            vp->pio_count * vp->pio_size <= sizeof(vp->pio_save))
        {
            BOOL lm = long_mode_code( &sregs ), op32 = lm || sregs.cs.db, a16 = !lm && !sregs.cs.db;
            UINT64 rdi = a16 ? (regs.rdi & 0xffff) : lm ? regs.rdi : (regs.rdi & 0xffffffff);
            UINT64 base = lm ? 0 : sregs.es.base;
            UINT32 n = 0;

            vp->pio_bytes = vp->pio_count * vp->pio_size;
            vp->pio_down = !!(regs.rflags & (1 << 10));
            vp->pio_linear = base + rdi - (vp->pio_down ? vp->pio_bytes - vp->pio_size : 0);
            if (access_linear( partition, vp->fd, vp->pio_linear, vp->pio_save, vp->pio_bytes, FALSE ))
            {
                UINT8 *ins = ctx->IoPortAccess.InstructionBytes;

                ins[n++] = 0xf3;
                if (vp->pio_size == (op32 ? 2 : 4)) ins[n++] = 0x66;
                ins[n++] = vp->pio_size == 1 ? 0x6c : 0x6d;
                ctx->IoPortAccess.InstructionByteCount = n;
                ctx->IoPortAccess.AccessInfo.IsWrite = 0;
                ctx->IoPortAccess.AccessInfo.AccessSize = vp->pio_size;
                ctx->IoPortAccess.AccessInfo.StringOp = 1;
                ctx->IoPortAccess.AccessInfo.RepPrefix = 1;
                ctx->IoPortAccess.PortNumber = vp->pio_port;
                ctx->IoPortAccess.Rax = regs.rax;
                ctx->IoPortAccess.Rcx = vp->pio_count;
                ctx->IoPortAccess.Rsi = regs.rsi;
                ctx->IoPortAccess.Rdi = regs.rdi;
                segment_to_whv( &sregs.ds, &ctx->IoPortAccess.Ds );
                segment_to_whv( &sregs.es, &ctx->IoPortAccess.Es );
                length = n;
                vp->pio_batch = TRUE;
                vp->pio_waiting = TRUE;
                break;
            }
        }
        if (!vp->pio_in) memcpy( &value, data, min( vp->pio_size, sizeof(value) ) );
        ctx->IoPortAccess.InstructionByteCount = 0;
        ctx->IoPortAccess.AccessInfo.IsWrite = !vp->pio_in;
        ctx->IoPortAccess.AccessInfo.AccessSize = vp->pio_size;
        ctx->IoPortAccess.PortNumber = vp->pio_port;
        ctx->IoPortAccess.Rax = vp->pio_in ? regs.rax : value;
        ctx->IoPortAccess.Rcx = regs.rcx;
        ctx->IoPortAccess.Rsi = regs.rsi;
        ctx->IoPortAccess.Rdi = regs.rdi;
        segment_to_whv( &sregs.ds, &ctx->IoPortAccess.Ds );
        segment_to_whv( &sregs.es, &ctx->IoPortAccess.Es );
        vp->pio_value = regs.rax;
        vp->pio_waiting = TRUE;
        break;
    }
    case WHvRunVpExitReasonMemoryAccess:
    {
        /* a piece of the access (1, 2 or 4 bytes) as MOV [EDI], EAX or
         * MOV EAX, [EDI] with EDI the guest physical address */
        UINT32 size = vp->mmio_piece, n = 0;
        UINT8 *ins = ctx->MemoryAccess.InstructionBytes;

        if (size == 2) ins[n++] = 0x66;
        ins[n++] = vp->mmio_write ? (size == 1 ? 0x88 : 0x89) : (size == 1 ? 0x8a : 0x8b);
        ins[n++] = 0x07;
        ctx->MemoryAccess.InstructionByteCount = n;
        ctx->MemoryAccess.AccessInfo.AccessType = vp->mmio_write ? WHvMemoryAccessWrite : WHvMemoryAccessRead;
        ctx->MemoryAccess.AccessInfo.GpaUnmapped = 1;
        ctx->MemoryAccess.Gpa = vp->mmio_gpa + vp->mmio_offset;
        vp->mmio_value = 0;
        if (vp->mmio_write) memcpy( &vp->mmio_value, vp->mmio_data + vp->mmio_offset, size );
        vp->mmio_waiting = TRUE;
        TRACE( "MMIO %s %#llx, %u bytes\n", vp->mmio_write ? "write" : "read",
               (long long)(vp->mmio_gpa + vp->mmio_offset), size );
        break;
    }
    case WHvRunVpExitReasonX64MsrAccess:
    {
        BOOL write = run->exit_reason == KVM_EXIT_X86_WRMSR;
        UINT32 index = run->msr.index;

        /* KVM finishes RDMSR/WRMSR with the program's answer: RDX:RAX, or a
         * #GP when the program leaves RIP on the instruction */
        data = run->msr.data;
        length = 2;
        vp->msr_rip = regs.rip;
        vp->msr_new_rip = regs.rip + 2;
        vp->msr_rax = regs.rax;
        vp->msr_rdx = regs.rdx;
        vp->msr_waiting = TRUE;
        ctx->MsrAccess.AccessInfo.IsWrite = write;
        ctx->MsrAccess.MsrNumber = index;
        ctx->MsrAccess.Rax = regs.rax;
        ctx->MsrAccess.Rdx = regs.rdx;
        TRACE( "MSR %#x %s %#llx\n", index, write ? "write" : "read", (long long)data );
        break;
    }
    case WHvRunVpExitReasonX64InterruptWindow:
        ctx->InterruptWindow.DeliverableType = WHvX64PendingInterrupt;
        break;
    case WHvRunVpExitReasonX64ApicEoi:
        ctx->ApicEoi.InterruptVector = run->eoi.vector;
        break;
    case WHvRunVpExitReasonCanceled:
        ctx->CancelReason.CancelReason = WHvRunVpCancelReasonUser;
        break;
    default:
        break;
    }
    fill_vp_context( vp, &ctx->VpContext, &regs, &sregs, &events );
    ctx->VpContext.InstructionLength = length;
    if (reason == WHvRunVpExitReasonMemoryAccess)
    {
        /* the program decodes the MOV in protected mode, flat, no paging */
        struct kvm_segment flat = { .limit = 0xffffffff, .type = 0xb, .present = 1, .dpl = 0, .db = 1, .s = 1, .g = 1 };

        segment_to_whv( &flat, &ctx->VpContext.Cs );
        ctx->VpContext.ExecutionState.Cpl = 0;
        ctx->VpContext.ExecutionState.Cr0Pe = 1;
        ctx->VpContext.ExecutionState.Cr0Am = 0;
        ctx->VpContext.ExecutionState.EferLma = 0;
        ctx->VpContext.Rip = 0;
        ctx->VpContext.Rflags &= ~(1ull << 17);   /* not virtual 8086 */
        ctx->VpContext.InstructionLength = ctx->MemoryAccess.InstructionByteCount;
    }
    TRACE( "exit %#x at %04x:%#llx, length %u, kvm exit %u\n", reason, sregs.cs.selector, (long long)regs.rip,
           length, run->exit_reason );
    return S_OK;
}

/* what a processor that stopped was doing, in the log */
static void dump_vp( struct partition *partition, struct vp *vp, const char *what )
{
    struct kvm_regs regs;
    struct kvm_sregs sregs;
    UINT8 bytes[16] = { 0 };
    UINT32 count = 0;
    char text[64] = "";

    if (ioctl( vp->fd, KVM_GET_REGS, &regs ) < 0 || ioctl( vp->fd, KVM_GET_SREGS, &sregs ) < 0) return;
    count = fetch_instruction( partition, vp, &sregs, regs.rip, bytes );
    for (UINT32 i = 0; i < count; i++) sprintf( text + i * 3, "%02x ", bytes[i] );
    ERR( "%s: cs %04x base %#llx rip %#llx rflags %#llx cr0 %#llx cr3 %#llx cr4 %#llx efer %#llx, bytes %s\n",
         what, sregs.cs.selector, (long long)sregs.cs.base, (long long)regs.rip, (long long)regs.rflags,
         (long long)sregs.cr0, (long long)sregs.cr3, (long long)sregs.cr4, (long long)sregs.efer, text );
    if (vp->run->exit_reason == KVM_EXIT_INTERNAL_ERROR)
        for (UINT32 i = 0; i < vp->run->internal.ndata && i < 16; i++)
            ERR( "  data[%u] %#llx\n", i, (long long)vp->run->internal.data[i] );
}

/* 1, 2 or 4 bytes: what one MOV does */
static UINT32 mmio_piece_size( struct vp *vp )
{
    UINT32 left = vp->mmio_len - vp->mmio_offset;

    if (left >= 4 && !(vp->mmio_len & 3)) return 4;
    if (left >= 2 && !(vp->mmio_len & 1)) return 2;
    return 1;
}

static NTSTATUS run_vp( void *args )
{
    struct run_params *params = args;
    struct partition *partition = get_partition( params->partition );
    struct vp *vp = get_vp( partition, params->vp );
    WHV_RUN_VP_EXIT_CONTEXT *ctx = params->exit;
    struct kvm_run *run;
    WHV_RUN_VP_EXIT_REASON reason;

    if (!vp)
    {
        params->hr = WHV_E_VP_DOES_NOT_EXIST;
        return STATUS_SUCCESS;
    }
    if (params->size < sizeof(*ctx))
    {
        params->hr = WHV_E_INSUFFICIENT_BUFFER;
        return STATUS_SUCCESS;
    }
    memset( ctx, 0, sizeof(*ctx) );
    run = vp->run;
    vp->tid = syscall( SYS_gettid );

    for (;;)
    {
        /* the program answered an element of a port access */
        if (vp->pio_waiting && vp->pio_batch)
        {
            UINT8 *data = (UINT8 *)run + run->io.data_offset, buffer[1024];

            /* what the program wrote, in the order KVM takes it; then the
             * memory as it was: KVM writes the elements where they belong */
            if (access_linear( partition, vp->fd, vp->pio_linear, buffer, vp->pio_bytes, FALSE ))
            {
                if (!vp->pio_down) memcpy( data, buffer, vp->pio_bytes );
                else
                    for (UINT32 i = 0; i < vp->pio_count; i++)
                        memcpy( data + i * vp->pio_size, buffer + vp->pio_bytes - (i + 1) * vp->pio_size, vp->pio_size );
            }
            access_linear( partition, vp->fd, vp->pio_linear, vp->pio_save, vp->pio_bytes, TRUE );
            vp->pio_waiting = vp->pio_batch = vp->pio = FALSE;
        }
        if (vp->pio_waiting)
        {
            UINT8 *data = (UINT8 *)run + run->io.data_offset + vp->pio_index * vp->pio_size;

            if (vp->pio_in) memcpy( data, &vp->pio_value, min( vp->pio_size, sizeof(vp->pio_value) ) );
            TRACE( "port %#x %s %u/%u: %#llx\n", vp->pio_port, vp->pio_in ? "in" : "out", vp->pio_index + 1,
                   vp->pio_count, (long long)(vp->pio_in ? vp->pio_value : 0) );
            vp->pio_waiting = FALSE;
            if (++vp->pio_index >= vp->pio_count) vp->pio = FALSE;
        }
        if (vp->msr_waiting)
        {
            run->msr.error = vp->msr_new_rip == vp->msr_rip;
            run->msr.data = (vp->msr_rdx << 32) | (UINT32)vp->msr_rax;
            vp->msr_waiting = FALSE;
        }
        if (vp->mmio_waiting)
        {
            if (!vp->mmio_write) memcpy( vp->mmio_data + vp->mmio_offset, &vp->mmio_value, vp->mmio_piece );
            vp->mmio_waiting = FALSE;
            vp->mmio_offset += vp->mmio_piece;
            if (vp->mmio_offset >= vp->mmio_len)
            {
                if (!vp->mmio_write) memcpy( run->mmio.data, vp->mmio_data, vp->mmio_len );
                vp->mmio = FALSE;
            }
            else vp->mmio_piece = mmio_piece_size( vp );
        }
        if (vp->cancel)
        {
            reason = WHvRunVpExitReasonCanceled;
            break;
        }
        if (vp->mmio)
        {
            reason = WHvRunVpExitReasonMemoryAccess;
            break;
        }
        if (vp->pio)
        {
            reason = WHvRunVpExitReasonX64IoPortAccess;
            break;
        }
        run->request_interrupt_window = want_window( vp );
        run->immediate_exit = 0;
        __sync_synchronize();
        if (vp->cancel) run->immediate_exit = 1;

        if (ioctl( vp->fd, KVM_RUN, 0 ) < 0)
        {
            if (errno == EINTR || errno == EAGAIN) continue;   /* a kick, or one of Wine's signals */
            ERR( "KVM_RUN: %s\n", strerror( errno ) );
            vp->tid = 0;
            params->hr = hr_errno( errno );
            return STATUS_SUCCESS;
        }
        vp->ran = TRUE;

        switch (run->exit_reason)
        {
        case KVM_EXIT_IO:
            vp->pio = TRUE;
            vp->pio_in = run->io.direction == KVM_EXIT_IO_IN;
            vp->pio_size = run->io.size;
            vp->pio_port = run->io.port;
            vp->pio_count = run->io.count;
            vp->pio_index = 0;
            reason = WHvRunVpExitReasonX64IoPortAccess;
            break;
        case KVM_EXIT_MMIO:
            vp->mmio_gpa = run->mmio.phys_addr;
            vp->mmio_len = min( run->mmio.len, sizeof(vp->mmio_data) );
            vp->mmio_write = run->mmio.is_write;
            memset( vp->mmio_data, 0xff, sizeof(vp->mmio_data) );
            if (vp->mmio_write) memcpy( vp->mmio_data, run->mmio.data, vp->mmio_len );
            vp->mmio_offset = 0;
            vp->mmio_piece = mmio_piece_size( vp );
            if (vp->mmio_gpa + vp->mmio_len > 0x100000000ull)
            {
                /* not addressable in the flat 32-bit context */
                FIXME( "MMIO above 4 GB at %#llx: not done\n", (long long)vp->mmio_gpa );
                if (!vp->mmio_write) memcpy( run->mmio.data, vp->mmio_data, vp->mmio_len );
                continue;
            }
            vp->mmio = TRUE;
            reason = WHvRunVpExitReasonMemoryAccess;
            break;
        case KVM_EXIT_X86_RDMSR:
        case KVM_EXIT_X86_WRMSR:
            reason = WHvRunVpExitReasonX64MsrAccess;
            break;
        case KVM_EXIT_HLT:
            reason = WHvRunVpExitReasonX64Halt;
            break;
        case KVM_EXIT_IOAPIC_EOI:
            /* a level-triggered interrupt of the program's IOAPIC is done */
            reason = WHvRunVpExitReasonX64ApicEoi;
            break;
        case KVM_EXIT_IRQ_WINDOW_OPEN:
            /* delivered: Windows clears the notification with it */
            vp->notify.InterruptNotification = 0;
            reason = WHvRunVpExitReasonX64InterruptWindow;
            break;
        case KVM_EXIT_SET_TPR:
        case KVM_EXIT_INTR:
        case KVM_EXIT_SYSTEM_EVENT:
        case KVM_EXIT_DEBUG:
            continue;   /* CR8 lowered: the window may open now; a signal */
        case KVM_EXIT_SHUTDOWN:
            WARN( "vCPU %u: triple fault\n", params->vp );
            dump_vp( partition, vp, "triple fault" );
            reason = WHvRunVpExitReasonUnrecoverableException;
            break;
        case KVM_EXIT_FAIL_ENTRY:
            ERR( "vCPU %u: entry failed, reason %#llx\n", params->vp,
                 (long long)run->fail_entry.hardware_entry_failure_reason );
            dump_vp( partition, vp, "entry failed" );
            reason = WHvRunVpExitReasonInvalidVpRegisterValue;
            break;
        case KVM_EXIT_INTERNAL_ERROR:
            ERR( "vCPU %u: KVM internal error %u\n", params->vp, run->internal.suberror );
            dump_vp( partition, vp, "internal error" );
            reason = WHvRunVpExitReasonUnrecoverableException;
            break;
        default:
            ERR( "vCPU %u: exit %u\n", params->vp, run->exit_reason );
            reason = WHvRunVpExitReasonUnsupportedFeature;
            break;
        }
        break;
    }

    params->hr = exit_with_state( partition, vp, ctx, reason );
    if (reason == WHvRunVpExitReasonCanceled)
    {
        vp->cancel = 0;
        run->immediate_exit = 0;
    }
    vp->tid = 0;
    return STATUS_SUCCESS;
}

/**********************************************************************
 *          The hypervisor's APIC (split irqchip)
 */

/* A level-triggered interrupt gets an MSI route on a pin of the reserved
 * IOAPIC range: KVM then tells its EOI (KVM_EXIT_IOAPIC_EOI) */
static void route_level( struct partition *partition, UINT64 address, UINT32 data )
{
    struct kvm_irq_routing *routing;
    UINT32 i;

    for (i = 0; i < partition->route_count; i++)
        if (partition->routes[i].address == address && (partition->routes[i].data & 0xffff) == (data & 0xffff)) return;
    i = partition->route_count < IOAPIC_PINS ? partition->route_count++ : partition->route_next++ % IOAPIC_PINS;
    partition->routes[i].address = address;
    partition->routes[i].data = data;

    if (!(routing = calloc( 1, sizeof(*routing) + IOAPIC_PINS * sizeof(routing->entries[0]) ))) return;
    for (i = 0; i < partition->route_count; i++)
    {
        routing->entries[i].gsi = i;
        routing->entries[i].type = KVM_IRQ_ROUTING_MSI;
        routing->entries[i].u.msi.address_lo = partition->routes[i].address;
        routing->entries[i].u.msi.address_hi = partition->routes[i].address >> 32;
        routing->entries[i].u.msi.data = partition->routes[i].data;
    }
    routing->nr = partition->route_count;
    if (ioctl( partition->vm, KVM_SET_GSI_ROUTING, routing ) < 0) WARN( "KVM_SET_GSI_ROUTING: %s\n", strerror( errno ) );
    free( routing );
}

static NTSTATUS request_interrupt( void *args )
{
    struct interrupt_params *params = args;
    struct partition *partition = get_partition( params->partition );
    const WHV_INTERRUPT_CONTROL *control = params->control;
    struct kvm_msi msi = { 0 };
    UINT64 address;
    UINT32 data;

    if (!partition || params->size < sizeof(*control) || partition->apic_mode == WHvX64LocalApicEmulationModeNone)
    {
        params->hr = E_INVALIDARG;
        return STATUS_SUCCESS;
    }
    /* the message an IOAPIC or a device would send: the destination, its
     * mode, the delivery mode, the trigger, the vector */
    address = 0xfee00000 | ((control->Destination & 0xff) << 12) |
              (control->DestinationMode == WHvX64InterruptDestinationModeLogical ? 1 << 2 : 0);
    address |= (UINT64)(control->Destination & 0xffffff00) << 32;
    data = (control->Vector & 0xff) | ((control->Type & 7) << 8);
    if (control->TriggerMode == WHvX64InterruptTriggerModeLevel) data |= (1 << 15) | (1 << 14);
    if (control->Type == WHvX64InterruptTypeLocalInt1) data = (data & ~0x700) | (4 << 8);   /* LINT1: an NMI */

    pthread_mutex_lock( &partition->lock );
    if (control->TriggerMode == WHvX64InterruptTriggerModeLevel) route_level( partition, address, data );
    pthread_mutex_unlock( &partition->lock );

    msi.address_lo = address;
    msi.address_hi = address >> 32;
    msi.data = data;
    if (ioctl( partition->vm, KVM_SIGNAL_MSI, &msi ) < 0)
    {
        WARN( "KVM_SIGNAL_MSI %#llx %#x: %s\n", (long long)address, data, strerror( errno ) );
        params->hr = hr_errno( errno );
        return STATUS_SUCCESS;
    }
    params->hr = S_OK;
    return STATUS_SUCCESS;
}

/* Windows gives the APIC's register page (4 KB, a register each 16 bytes);
 * KVM's state is the first 1 KB of that same page */
static NTSTATUS get_apic_state( void *args )
{
    struct apic_params *params = args;
    struct vp *vp = get_vp( get_partition( params->partition ), params->vp );
    struct kvm_lapic_state lapic;

    if (!vp)
    {
        params->hr = WHV_E_VP_DOES_NOT_EXIST;
        return STATUS_SUCCESS;
    }
    if (params->written) *params->written = 0x1000;
    if (params->size < sizeof(lapic.regs))
    {
        params->hr = WHV_E_INSUFFICIENT_BUFFER;
        return STATUS_SUCCESS;
    }
    if (ioctl( vp->fd, KVM_GET_LAPIC, &lapic ) < 0)
    {
        params->hr = hr_errno( errno );
        return STATUS_SUCCESS;
    }
    memset( params->state, 0, params->size );
    memcpy( params->state, lapic.regs, sizeof(lapic.regs) );
    params->hr = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS set_apic_state( void *args )
{
    struct apic_params *params = args;
    struct vp *vp = get_vp( get_partition( params->partition ), params->vp );
    struct kvm_lapic_state lapic;

    if (!vp)
    {
        params->hr = WHV_E_VP_DOES_NOT_EXIST;
        return STATUS_SUCCESS;
    }
    if (params->size < sizeof(lapic.regs))
    {
        params->hr = WHV_E_INSUFFICIENT_BUFFER;
        return STATUS_SUCCESS;
    }
    memcpy( lapic.regs, params->state, sizeof(lapic.regs) );
    params->hr = ioctl( vp->fd, KVM_SET_LAPIC, &lapic ) < 0 ? hr_errno( errno ) : S_OK;
    if (FAILED(params->hr)) WARN( "KVM_SET_LAPIC: %s\n", strerror( errno ) );
    return STATUS_SUCCESS;
}

/**********************************************************************
 *          XSAVE state
 *
 * Windows gives it compacted (XSAVEC: XCOMP_BV bit 63, the components one
 * after the other); KVM's is the standard layout, each component at its
 * CPUID 0xD offset.
 */

static UINT32 xsave_kvm_size(void)
{
    static UINT32 size;
    int ret;

    if (!size) size = (ret = ioctl( kvm, KVM_CHECK_EXTENSION, KVM_CAP_XSAVE2 )) > 4096 ? ret : 4096;
    return size;
}

/* where each component is in the compacted layout of the components in mask */
static void compacted_offsets( UINT64 mask, UINT32 offsets[64], UINT32 sizes[64], UINT32 std_offsets[64] )
{
    UINT32 offset = 576, r[4];

    for (int i = 2; i < 63; i++)
    {
        offsets[i] = sizes[i] = std_offsets[i] = 0;
        if (!(mask & (1ull << i))) continue;
        cpuid( 0xd, i, r );
        sizes[i] = r[0];
        std_offsets[i] = r[1];
        if (r[2] & 2) offset = (offset + 63) & ~63;   /* aligned to 64 bytes */
        offsets[i] = offset;
        offset += r[0];
    }
}

static UINT64 vp_xcr0( struct vp *vp )
{
    struct kvm_xcrs xcrs;

    if (ioctl( vp->fd, KVM_GET_XCRS, &xcrs ) < 0) return 3;
    for (UINT32 i = 0; i < xcrs.nr_xcrs; i++) if (!xcrs.xcrs[i].xcr) return xcrs.xcrs[i].value;
    return 3;
}

static NTSTATUS get_xsave_state( void *args )
{
    struct apic_params *params = args;
    struct vp *vp = get_vp( get_partition( params->partition ), params->vp );
    UINT32 offsets[64], sizes[64], std[64], size = xsave_kvm_size(), need = 576;
    UINT64 mask, xstate_bv, xcomp_bv;
    UINT8 *kbuf, *out = params->state;

    if (!vp)
    {
        params->hr = WHV_E_VP_DOES_NOT_EXIST;
        return STATUS_SUCCESS;
    }
    if (!(kbuf = calloc( 1, size )))
    {
        params->hr = E_OUTOFMEMORY;
        return STATUS_SUCCESS;
    }
    if (ioctl( vp->fd, size > 4096 ? KVM_GET_XSAVE2 : KVM_GET_XSAVE, kbuf ) < 0)
    {
        params->hr = hr_errno( errno );
        free( kbuf );
        return STATUS_SUCCESS;
    }
    mask = vp_xcr0( vp );
    compacted_offsets( mask, offsets, sizes, std );
    for (int i = 2; i < 63; i++) if (sizes[i]) need = offsets[i] + sizes[i];
    if (params->written) *params->written = need;
    if (params->size < need)
    {
        params->hr = WHV_E_INSUFFICIENT_BUFFER;
        free( kbuf );
        return STATUS_SUCCESS;
    }
    memset( out, 0, params->size );
    memcpy( out, kbuf, 512 );                       /* the legacy area */
    memcpy( &xstate_bv, kbuf + 512, 8 );
    xstate_bv &= mask;
    xcomp_bv = (1ull << 63) | (mask & ~3ull);
    memcpy( out + 512, &xstate_bv, 8 );
    memcpy( out + 520, &xcomp_bv, 8 );
    for (int i = 2; i < 63; i++)
        if (sizes[i] && std[i] + sizes[i] <= size) memcpy( out + offsets[i], kbuf + std[i], sizes[i] );
    free( kbuf );
    params->hr = S_OK;
    return STATUS_SUCCESS;
}

static NTSTATUS set_xsave_state( void *args )
{
    struct apic_params *params = args;
    struct vp *vp = get_vp( get_partition( params->partition ), params->vp );
    UINT32 offsets[64], sizes[64], std[64], size = xsave_kvm_size();
    const UINT8 *in = params->state;
    UINT64 xstate_bv, xcomp_bv;
    UINT8 *kbuf;

    if (!vp)
    {
        params->hr = WHV_E_VP_DOES_NOT_EXIST;
        return STATUS_SUCCESS;
    }
    if (params->size < 576)
    {
        params->hr = WHV_E_INSUFFICIENT_BUFFER;
        return STATUS_SUCCESS;
    }
    if (!(kbuf = calloc( 1, size )))
    {
        params->hr = E_OUTOFMEMORY;
        return STATUS_SUCCESS;
    }
    /* what the program does not give stays as it is */
    ioctl( vp->fd, size > 4096 ? KVM_GET_XSAVE2 : KVM_GET_XSAVE, kbuf );
    memcpy( &xstate_bv, in + 512, 8 );
    memcpy( &xcomp_bv, in + 520, 8 );
    memcpy( kbuf, in, 512 );
    memcpy( kbuf + 512, &xstate_bv, 8 );
    memset( kbuf + 520, 0, 64 - 8 );                /* XCOMP_BV and the rest of the header: standard */
    if (xcomp_bv & (1ull << 63))
    {
        compacted_offsets( xcomp_bv & ~(1ull << 63), offsets, sizes, std );
        for (int i = 2; i < 63; i++)
            if (sizes[i] && offsets[i] + sizes[i] <= params->size && std[i] + sizes[i] <= size)
                memcpy( kbuf + std[i], in + offsets[i], sizes[i] );
    }
    else memcpy( kbuf + 576, in + 576, min( params->size, size ) - 576 );
    params->hr = ioctl( vp->fd, KVM_SET_XSAVE, kbuf ) < 0 ? hr_errno( errno ) : S_OK;
    if (FAILED(params->hr)) WARN( "KVM_SET_XSAVE: %s\n", strerror( errno ) );
    free( kbuf );
    return STATUS_SUCCESS;
}

#else  /* __x86_64__ */

#define NOT_HERE(name) \
    static NTSTATUS name( void *args ) \
    { \
        return STATUS_NOT_SUPPORTED; \
    }

NOT_HERE( get_capability )
NOT_HERE( create_partition )
NOT_HERE( setup_partition )
NOT_HERE( delete_partition )
NOT_HERE( get_property )
NOT_HERE( set_property )
NOT_HERE( map_gpa_range )
NOT_HERE( unmap_gpa_range )
NOT_HERE( translate_gva )
NOT_HERE( create_vp )
NOT_HERE( delete_vp )
NOT_HERE( run_vp )
NOT_HERE( cancel_run_vp )
NOT_HERE( get_registers )
NOT_HERE( set_registers )
NOT_HERE( get_cpuid_output )
NOT_HERE( request_interrupt )
NOT_HERE( get_apic_state )
NOT_HERE( set_apic_state )
NOT_HERE( get_xsave_state )
NOT_HERE( set_xsave_state )

#endif  /* __x86_64__ */

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    get_capability,
    create_partition,
    setup_partition,
    delete_partition,
    get_property,
    set_property,
    map_gpa_range,
    unmap_gpa_range,
    translate_gva,
    create_vp,
    delete_vp,
    run_vp,
    cancel_run_vp,
    get_registers,
    set_registers,
    get_cpuid_output,
    request_interrupt,
    get_apic_state,
    set_apic_state,
    get_xsave_state,
    set_xsave_state,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_funcs) == unix_funcs_count );
