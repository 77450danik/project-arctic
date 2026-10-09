/*
 * Windows Hypervisor Platform: calls into the unix side (KVM)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_WINHVPLATFORM_UNIXLIB_H
#define __WINE_WINHVPLATFORM_UNIXLIB_H

#include "wine/unixlib.h"

/* a partition is the unix side's, known to Windows by this handle */
struct capability_params
{
    UINT32  code;
    void   *buffer;
    UINT32  size;
    UINT32 *written;
    HRESULT hr;
};

struct partition_params
{
    UINT64  partition;          /* out of create */
    HRESULT hr;
};

struct property_params
{
    UINT64      partition;
    UINT32      code;
    const void *in;             /* set */
    void       *out;            /* get */
    UINT32      size;
    UINT32     *written;
    HRESULT     hr;
};

struct map_params
{
    UINT64  partition;
    void   *source;
    UINT64  gpa;
    UINT64  size;
    UINT32  flags;
    HRESULT hr;
};

struct translate_params
{
    UINT64  partition;
    UINT32  vp;
    UINT64  gva;
    UINT32  flags;
    void   *result;             /* WHV_TRANSLATE_GVA_RESULT */
    UINT64 *gpa;
    HRESULT hr;
};

struct vp_params
{
    UINT64  partition;
    UINT32  vp;
    UINT32  flags;
    HRESULT hr;
};

struct run_params
{
    UINT64  partition;
    UINT32  vp;
    void   *exit;               /* WHV_RUN_VP_EXIT_CONTEXT */
    UINT32  size;
    HRESULT hr;
};

struct registers_params
{
    UINT64      partition;
    UINT32      vp;
    const void *names;          /* WHV_REGISTER_NAME */
    UINT32      count;
    void       *values;         /* WHV_REGISTER_VALUE */
    HRESULT     hr;
};

struct interrupt_params
{
    UINT64      partition;
    const void *control;        /* WHV_INTERRUPT_CONTROL */
    UINT32      size;
    HRESULT     hr;
};

struct apic_params
{
    UINT64  partition;
    UINT32  vp;
    void   *state;              /* the APIC's 4 KB register page */
    UINT32  size;
    UINT32 *written;
    HRESULT hr;
};

struct cpuid_params
{
    UINT64  partition;
    UINT32  vp;
    UINT32  eax, ecx;
    void   *output;             /* WHV_CPUID_OUTPUT */
    HRESULT hr;
};

enum winhvplatform_funcs
{
    unix_get_capability,
    unix_create_partition,
    unix_setup_partition,
    unix_delete_partition,
    unix_get_property,
    unix_set_property,
    unix_map_gpa_range,
    unix_unmap_gpa_range,
    unix_translate_gva,
    unix_create_vp,
    unix_delete_vp,
    unix_run_vp,
    unix_cancel_run_vp,
    unix_get_registers,
    unix_set_registers,
    unix_get_cpuid_output,
    unix_request_interrupt,
    unix_get_apic_state,
    unix_set_apic_state,
    unix_get_xsave_state,         /* struct apic_params: the XSAVE area, compacted as Windows gives it */
    unix_set_xsave_state,
    unix_funcs_count
};

#endif
