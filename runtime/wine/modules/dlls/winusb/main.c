/*
 * WinUSB
 *
 * Copyright (C) 2022 Mohamad Al-Jaf
 * Arctic: the API over winusb.sys (wine/arctic_winusb.h). A handle is the
 * interface the device file stands for; descriptors come once from the
 * driver, transfers go to it as overlapped I/O on the program's handle, so
 * WinUsb_GetOverlappedResult is the file's. A synchronous call waits for its
 * pipe's PIPE_TRANSFER_TIMEOUT and cancels the transfer when it runs out.
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

#include <stdarg.h>
#include <stdlib.h>

#include "windef.h"
#include "winbase.h"
#include "winioctl.h"
#include "winusb.h"
#include "wine/arctic_winusb.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(winusb);

#define WINUSB_MAGIC 0x42535557 /* WUSB */
#define MAX_PIPES 32

struct pipe_policy
{
    UCHAR id;
    ULONG timeout;
    UCHAR short_packet_terminate, auto_clear_stall, ignore_short_packets;
    UCHAR allow_partial_reads, auto_flush, raw_io, reset_on_resume;
};

struct winusb
{
    DWORD magic;
    HANDLE file;
    UCHAR interface_number, speed, alternate;
    UCHAR auto_suspend;
    ULONG suspend_delay;
    USB_DEVICE_DESCRIPTOR device;
    USB_CONFIGURATION_DESCRIPTOR *config;
    struct pipe_policy policies[MAX_PIPES];
    ULONG policy_count;
};

static struct winusb *get_winusb(WINUSB_INTERFACE_HANDLE handle)
{
    struct winusb *winusb = handle;

    if (!winusb || winusb->magic != WINUSB_MAGIC)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return NULL;
    }
    return winusb;
}

static const USB_INTERFACE_DESCRIPTOR *find_interface(const struct winusb *winusb, UCHAR alternate)
{
    const USB_CONFIGURATION_DESCRIPTOR *config = winusb->config;
    const UCHAR *ptr = (const UCHAR *)config, *end = ptr + config->wTotalLength;

    for (ptr += config->bLength; ptr + 2 <= end && ptr[0] >= 2; ptr += ptr[0])
    {
        const USB_INTERFACE_DESCRIPTOR *iface = (const USB_INTERFACE_DESCRIPTOR *)ptr;

        if (ptr[1] != USB_INTERFACE_DESCRIPTOR_TYPE || ptr + sizeof(*iface) > end) continue;
        if (iface->bInterfaceNumber == winusb->interface_number && iface->bAlternateSetting == alternate)
            return iface;
    }
    return NULL;
}

static const USB_ENDPOINT_DESCRIPTOR *find_endpoint(const struct winusb *winusb, UCHAR alternate,
                                                    int index, UCHAR address)
{
    const USB_INTERFACE_DESCRIPTOR *iface = find_interface(winusb, alternate);
    const UCHAR *ptr, *end = (const UCHAR *)winusb->config + winusb->config->wTotalLength;
    int n = 0;

    if (!iface) return NULL;
    for (ptr = (const UCHAR *)iface + iface->bLength; ptr + 2 <= end && ptr[0] >= 2; ptr += ptr[0])
    {
        const USB_ENDPOINT_DESCRIPTOR *ep = (const USB_ENDPOINT_DESCRIPTOR *)ptr;

        if (ptr[1] == USB_INTERFACE_DESCRIPTOR_TYPE) break;
        if (ptr[1] != USB_ENDPOINT_DESCRIPTOR_TYPE || ptr + sizeof(*ep) > end) continue;
        if (index >= 0 ? n++ == index : ep->bEndpointAddress == address) return ep;
    }
    return NULL;
}

static struct pipe_policy *get_policy(struct winusb *winusb, UCHAR pipe)
{
    struct pipe_policy *policy;
    ULONG i;

    for (i = 0; i < winusb->policy_count; ++i)
        if (winusb->policies[i].id == pipe) return &winusb->policies[i];
    if (!find_endpoint(winusb, winusb->alternate, -1, pipe) || winusb->policy_count == MAX_PIPES)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    policy = &winusb->policies[winusb->policy_count++];
    memset(policy, 0, sizeof(*policy));
    policy->id = pipe;
    /* WinUSB's defaults: reads of an IN pipe are not cut at a short packet */
    policy->allow_partial_reads = 1;
    return policy;
}

/* the request on an overlapped of our own, waited for up to timeout ms (0: no limit) */
static BOOL sync_ioctl(HANDLE file, DWORD code, void *in, DWORD in_size, void *out, DWORD out_size,
                       DWORD *ret, DWORD timeout)
{
    OVERLAPPED ov = {0};
    DWORD size = 0, error;
    BOOL ok;

    if (!(ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL))) return FALSE;
    ok = DeviceIoControl(file, code, in, in_size, out, out_size, &size, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING)
    {
        if (WaitForSingleObject(ov.hEvent, timeout ? timeout : INFINITE) == WAIT_TIMEOUT)
        {
            CancelIoEx(file, &ov);
            GetOverlappedResult(file, &ov, &size, TRUE);
            CloseHandle(ov.hEvent);
            if (ret) *ret = 0;
            SetLastError(ERROR_SEM_TIMEOUT);
            return FALSE;
        }
        ok = GetOverlappedResult(file, &ov, &size, FALSE);
    }
    error = GetLastError();
    CloseHandle(ov.hEvent);
    if (ret) *ret = ok ? size : 0;
    SetLastError(error);
    return ok;
}

static BOOL transfer(struct winusb *winusb, DWORD code, void *in, DWORD in_size, void *buffer,
                     ULONG length, ULONG *transferred, OVERLAPPED *overlapped, ULONG timeout)
{
    DWORD size = 0;
    BOOL ok;

    if (overlapped)
    {
        ok = DeviceIoControl(winusb->file, code, in, in_size, buffer, length, &size, overlapped);
        if (transferred) *transferred = ok ? size : 0;
        return ok;
    }
    ok = sync_ioctl(winusb->file, code, in, in_size, buffer, length, &size, timeout);
    if (transferred) *transferred = size;
    return ok;
}

/***********************************************************************
 *           WinUsb_Initialize (winusb.@)
 */
BOOL WINAPI WinUsb_Initialize(HANDLE file, WINUSB_INTERFACE_HANDLE *handle)
{
    ULONG size = sizeof(struct arctic_winusb_info) + 0xffff;
    struct arctic_winusb_info *info;
    struct winusb *winusb;
    DWORD ret;

    TRACE("%p, %p\n", file, handle);

    if (!handle || file == INVALID_HANDLE_VALUE || !file)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!(info = malloc(size)))
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    if (!sync_ioctl(file, IOCTL_ARCTIC_WINUSB_GET_INFO, NULL, 0, info, size, &ret, 0)
        || ret < sizeof(*info) + sizeof(USB_CONFIGURATION_DESCRIPTOR))
    {
        /* not a WinUSB device: Windows says so the same way */
        WARN("not a WinUSB device, error %lu\n", GetLastError());
        free(info);
        SetLastError(ERROR_INVALID_FUNCTION);
        return FALSE;
    }

    if (!(winusb = calloc(1, sizeof(*winusb))) || !(winusb->config = malloc(info->config_size)))
    {
        free(winusb);
        free(info);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    winusb->magic = WINUSB_MAGIC;
    winusb->file = file;
    winusb->interface_number = info->interface_number;
    winusb->speed = info->speed;
    winusb->device = info->device;
    winusb->auto_suspend = 1;
    winusb->suspend_delay = 5000;
    memcpy(winusb->config, info + 1, info->config_size);
    winusb->config->wTotalLength = info->config_size;
    free(info);

    TRACE("%04x:%04x interface %u -> %p\n", winusb->device.idVendor, winusb->device.idProduct,
          winusb->interface_number, winusb);
    *handle = winusb;
    return TRUE;
}

/***********************************************************************
 *           WinUsb_Free (winusb.@)
 */
BOOL WINAPI WinUsb_Free(WINUSB_INTERFACE_HANDLE handle)
{
    struct winusb *winusb = handle;

    TRACE("%p\n", handle);

    if (!winusb || winusb->magic != WINUSB_MAGIC) return TRUE;
    winusb->magic = 0;
    free(winusb->config);
    free(winusb);
    return TRUE;
}

/***********************************************************************
 *           WinUsb_GetAssociatedInterface (winusb.@)
 */
BOOL WINAPI WinUsb_GetAssociatedInterface(WINUSB_INTERFACE_HANDLE handle, UCHAR index,
                                          WINUSB_INTERFACE_HANDLE *associated)
{
    TRACE("%p, %u, %p\n", handle, index, associated);

    if (!get_winusb(handle)) return FALSE;
    /* the bus gives each interface of a composite device its own device */
    SetLastError(ERROR_NO_MORE_ITEMS);
    return FALSE;
}

/***********************************************************************
 *           WinUsb_ControlTransfer (winusb.@)
 */
BOOL WINAPI WinUsb_ControlTransfer(WINUSB_INTERFACE_HANDLE handle, WINUSB_SETUP_PACKET setup,
                                   UCHAR *buffer, ULONG length, ULONG *transferred, OVERLAPPED *overlapped)
{
    struct winusb *winusb = get_winusb(handle);
    BOOL in = !!(setup.RequestType & 0x80);

    TRACE("%p, %02x %02x %04x %04x %04x, %p, %lu, %p, %p\n", handle, setup.RequestType, setup.Request,
          setup.Value, setup.Index, setup.Length, buffer, length, transferred, overlapped);

    if (!winusb) return FALSE;
    if (length > setup.Length) length = setup.Length;
    return transfer(winusb, in ? IOCTL_ARCTIC_WINUSB_CONTROL_IN : IOCTL_ARCTIC_WINUSB_CONTROL_OUT,
                    &setup, sizeof(setup), buffer, length, transferred, overlapped, 5000);
}

/***********************************************************************
 *           WinUsb_GetDescriptor (winusb.@)
 */
BOOL WINAPI WinUsb_GetDescriptor(WINUSB_INTERFACE_HANDLE handle, UCHAR type, UCHAR index, USHORT language,
                                 UCHAR *buffer, ULONG length, ULONG *transferred)
{
    struct winusb *winusb = get_winusb(handle);
    WINUSB_SETUP_PACKET setup;

    TRACE("%p, %u, %u, %#x, %p, %lu, %p\n", handle, type, index, language, buffer, length, transferred);

    if (!winusb) return FALSE;
    if (!buffer || !transferred)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (type == USB_DEVICE_DESCRIPTOR_TYPE)
    {
        *transferred = min(length, sizeof(winusb->device));
        memcpy(buffer, &winusb->device, *transferred);
        return TRUE;
    }
    setup.RequestType = 0x80;
    setup.Request = 6; /* GET_DESCRIPTOR */
    setup.Value = (type << 8) | index;
    setup.Index = language;
    setup.Length = min(length, 0xffff);
    return sync_ioctl(winusb->file, IOCTL_ARCTIC_WINUSB_CONTROL_IN, &setup, sizeof(setup),
                      buffer, setup.Length, transferred, 5000);
}

/***********************************************************************
 *           WinUsb_QueryInterfaceSettings (winusb.@)
 */
BOOL WINAPI WinUsb_QueryInterfaceSettings(WINUSB_INTERFACE_HANDLE handle, UCHAR alternate,
                                          USB_INTERFACE_DESCRIPTOR *descriptor)
{
    struct winusb *winusb = get_winusb(handle);
    const USB_INTERFACE_DESCRIPTOR *iface;

    TRACE("%p, %u, %p\n", handle, alternate, descriptor);

    if (!winusb) return FALSE;
    if (!descriptor)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!(iface = find_interface(winusb, alternate)))
    {
        SetLastError(ERROR_NO_MORE_ITEMS);
        return FALSE;
    }
    *descriptor = *iface;
    return TRUE;
}

/***********************************************************************
 *           WinUsb_QueryDeviceInformation (winusb.@)
 */
BOOL WINAPI WinUsb_QueryDeviceInformation(WINUSB_INTERFACE_HANDLE handle, ULONG type, ULONG *length, void *buffer)
{
    struct winusb *winusb = get_winusb(handle);

    TRACE("%p, %lu, %p, %p\n", handle, type, length, buffer);

    if (!winusb) return FALSE;
    if (type != DEVICE_SPEED || !length || !buffer || *length < 1)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *(UCHAR *)buffer = winusb->speed;
    *length = 1;
    return TRUE;
}

/***********************************************************************
 *           WinUsb_SetCurrentAlternateSetting (winusb.@)
 */
BOOL WINAPI WinUsb_SetCurrentAlternateSetting(WINUSB_INTERFACE_HANDLE handle, UCHAR alternate)
{
    struct winusb *winusb = get_winusb(handle);

    TRACE("%p, %u\n", handle, alternate);

    if (!winusb) return FALSE;
    if (alternate == winusb->alternate) return TRUE;
    FIXME("alternate setting %u is not supported\n", alternate);
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

/***********************************************************************
 *           WinUsb_GetCurrentAlternateSetting (winusb.@)
 */
BOOL WINAPI WinUsb_GetCurrentAlternateSetting(WINUSB_INTERFACE_HANDLE handle, UCHAR *alternate)
{
    struct winusb *winusb = get_winusb(handle);

    TRACE("%p, %p\n", handle, alternate);

    if (!winusb) return FALSE;
    if (!alternate)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *alternate = winusb->alternate;
    return TRUE;
}

static BOOL query_pipe(WINUSB_INTERFACE_HANDLE handle, UCHAR alternate, UCHAR index, WINUSB_PIPE_INFORMATION_EX *info)
{
    struct winusb *winusb = get_winusb(handle);
    const USB_ENDPOINT_DESCRIPTOR *ep;

    if (!winusb) return FALSE;
    if (!find_interface(winusb, alternate) || !(ep = find_endpoint(winusb, alternate, index, 0)))
    {
        SetLastError(ERROR_NO_MORE_ITEMS);
        return FALSE;
    }
    info->PipeType = ep->bmAttributes & USB_ENDPOINT_TYPE_MASK;
    info->PipeId = ep->bEndpointAddress;
    info->MaximumPacketSize = ep->wMaxPacketSize & 0x7ff;
    info->Interval = ep->bInterval;
    info->MaximumBytesPerInterval = (ep->wMaxPacketSize & 0x7ff) * (((ep->wMaxPacketSize >> 11) & 3) + 1);
    return TRUE;
}

/***********************************************************************
 *           WinUsb_QueryPipe (winusb.@)
 */
BOOL WINAPI WinUsb_QueryPipe(WINUSB_INTERFACE_HANDLE handle, UCHAR alternate, UCHAR index,
                             WINUSB_PIPE_INFORMATION *info)
{
    WINUSB_PIPE_INFORMATION_EX ex;

    TRACE("%p, %u, %u, %p\n", handle, alternate, index, info);

    if (!info)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!query_pipe(handle, alternate, index, &ex)) return FALSE;
    info->PipeType = ex.PipeType;
    info->PipeId = ex.PipeId;
    info->MaximumPacketSize = ex.MaximumPacketSize;
    info->Interval = ex.Interval;
    return TRUE;
}

/***********************************************************************
 *           WinUsb_QueryPipeEx (winusb.@)
 */
BOOL WINAPI WinUsb_QueryPipeEx(WINUSB_INTERFACE_HANDLE handle, UCHAR alternate, UCHAR index,
                               WINUSB_PIPE_INFORMATION_EX *info)
{
    TRACE("%p, %u, %u, %p\n", handle, alternate, index, info);

    if (!info)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return query_pipe(handle, alternate, index, info);
}

static UCHAR *policy_field(struct pipe_policy *policy, ULONG type)
{
    switch (type)
    {
    case SHORT_PACKET_TERMINATE: return &policy->short_packet_terminate;
    case AUTO_CLEAR_STALL: return &policy->auto_clear_stall;
    case IGNORE_SHORT_PACKETS: return &policy->ignore_short_packets;
    case ALLOW_PARTIAL_READS: return &policy->allow_partial_reads;
    case AUTO_FLUSH: return &policy->auto_flush;
    case RAW_IO: return &policy->raw_io;
    case RESET_PIPE_ON_RESUME: return &policy->reset_on_resume;
    }
    return NULL;
}

/***********************************************************************
 *           WinUsb_SetPipePolicy (winusb.@)
 */
BOOL WINAPI WinUsb_SetPipePolicy(WINUSB_INTERFACE_HANDLE handle, UCHAR pipe, ULONG type, ULONG length, void *value)
{
    struct winusb *winusb = get_winusb(handle);
    struct pipe_policy *policy;
    UCHAR *field;

    TRACE("%p, %#x, %lu, %lu, %p\n", handle, pipe, type, length, value);

    if (!winusb || !(policy = get_policy(winusb, pipe))) return FALSE;
    if (!value)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (type == PIPE_TRANSFER_TIMEOUT)
    {
        if (length < sizeof(ULONG)) goto invalid;
        policy->timeout = *(ULONG *)value;
        return TRUE;
    }
    if (type == MAXIMUM_TRANSFER_SIZE) goto invalid; /* read-only */
    if (!(field = policy_field(policy, type)) || length < 1) goto invalid;
    *field = !!*(UCHAR *)value;
    return TRUE;

invalid:
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
}

/***********************************************************************
 *           WinUsb_GetPipePolicy (winusb.@)
 */
BOOL WINAPI WinUsb_GetPipePolicy(WINUSB_INTERFACE_HANDLE handle, UCHAR pipe, ULONG type, ULONG *length, void *value)
{
    struct winusb *winusb = get_winusb(handle);
    struct pipe_policy *policy;
    UCHAR *field;

    TRACE("%p, %#x, %lu, %p, %p\n", handle, pipe, type, length, value);

    if (!winusb || !(policy = get_policy(winusb, pipe))) return FALSE;
    if (!length || !value) goto invalid;
    if (type == PIPE_TRANSFER_TIMEOUT || type == MAXIMUM_TRANSFER_SIZE)
    {
        if (*length < sizeof(ULONG)) goto invalid;
        *(ULONG *)value = type == PIPE_TRANSFER_TIMEOUT ? policy->timeout : 4 * 1024 * 1024;
        *length = sizeof(ULONG);
        return TRUE;
    }
    if (!(field = policy_field(policy, type)) || *length < 1) goto invalid;
    *(UCHAR *)value = *field;
    *length = 1;
    return TRUE;

invalid:
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
}

/***********************************************************************
 *           WinUsb_ReadPipe (winusb.@)
 */
BOOL WINAPI WinUsb_ReadPipe(WINUSB_INTERFACE_HANDLE handle, UCHAR pipe, UCHAR *buffer, ULONG length,
                            ULONG *transferred, OVERLAPPED *overlapped)
{
    struct winusb *winusb = get_winusb(handle);
    struct arctic_winusb_pipe in = {pipe};
    struct pipe_policy *policy;

    TRACE("%p, %#x, %p, %lu, %p, %p\n", handle, pipe, buffer, length, transferred, overlapped);

    if (!winusb || !(policy = get_policy(winusb, pipe))) return FALSE;
    if (!(pipe & 0x80) || (!buffer && length))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return transfer(winusb, IOCTL_ARCTIC_WINUSB_READ_PIPE, &in, sizeof(in), buffer, length,
                    transferred, overlapped, policy->timeout);
}

/***********************************************************************
 *           WinUsb_WritePipe (winusb.@)
 */
BOOL WINAPI WinUsb_WritePipe(WINUSB_INTERFACE_HANDLE handle, UCHAR pipe, UCHAR *buffer, ULONG length,
                             ULONG *transferred, OVERLAPPED *overlapped)
{
    struct winusb *winusb = get_winusb(handle);
    struct arctic_winusb_pipe in = {pipe};
    const USB_ENDPOINT_DESCRIPTOR *ep;
    struct pipe_policy *policy;
    BOOL ok;

    TRACE("%p, %#x, %p, %lu, %p, %p\n", handle, pipe, buffer, length, transferred, overlapped);

    if (!winusb || !(policy = get_policy(winusb, pipe))) return FALSE;
    if ((pipe & 0x80) || (!buffer && length))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    ok = transfer(winusb, IOCTL_ARCTIC_WINUSB_WRITE_PIPE, &in, sizeof(in), buffer, length,
                  transferred, overlapped, policy->timeout);

    /* a write of whole packets ends with a zero-length one when asked */
    if (ok && !overlapped && policy->short_packet_terminate && length
        && (ep = find_endpoint(winusb, winusb->alternate, -1, pipe))
        && (ep->wMaxPacketSize & 0x7ff) && !(length % (ep->wMaxPacketSize & 0x7ff)))
        transfer(winusb, IOCTL_ARCTIC_WINUSB_WRITE_PIPE, &in, sizeof(in), NULL, 0, NULL, NULL, policy->timeout);
    else if (overlapped && policy->short_packet_terminate)
        FIXME("SHORT_PACKET_TERMINATE on an overlapped write is not supported\n");
    return ok;
}

/***********************************************************************
 *           WinUsb_ResetPipe (winusb.@)
 */
BOOL WINAPI WinUsb_ResetPipe(WINUSB_INTERFACE_HANDLE handle, UCHAR pipe)
{
    struct winusb *winusb = get_winusb(handle);
    struct arctic_winusb_pipe in = {pipe};

    TRACE("%p, %#x\n", handle, pipe);

    if (!winusb) return FALSE;
    return sync_ioctl(winusb->file, IOCTL_ARCTIC_WINUSB_RESET_PIPE, &in, sizeof(in), NULL, 0, NULL, 5000);
}

/***********************************************************************
 *           WinUsb_AbortPipe (winusb.@)
 */
BOOL WINAPI WinUsb_AbortPipe(WINUSB_INTERFACE_HANDLE handle, UCHAR pipe)
{
    struct winusb *winusb = get_winusb(handle);
    struct arctic_winusb_pipe in = {pipe};

    TRACE("%p, %#x\n", handle, pipe);

    if (!winusb) return FALSE;
    return sync_ioctl(winusb->file, IOCTL_ARCTIC_WINUSB_ABORT_PIPE, &in, sizeof(in), NULL, 0, NULL, 5000);
}

/***********************************************************************
 *           WinUsb_FlushPipe (winusb.@)
 */
BOOL WINAPI WinUsb_FlushPipe(WINUSB_INTERFACE_HANDLE handle, UCHAR pipe)
{
    TRACE("%p, %#x\n", handle, pipe);

    /* nothing is kept between reads: there is nothing to throw away */
    return !!get_winusb(handle);
}

/***********************************************************************
 *           WinUsb_SetPowerPolicy (winusb.@)
 */
BOOL WINAPI WinUsb_SetPowerPolicy(WINUSB_INTERFACE_HANDLE handle, ULONG type, ULONG length, void *value)
{
    struct winusb *winusb = get_winusb(handle);

    TRACE("%p, %lu, %lu, %p\n", handle, type, length, value);

    if (!winusb) return FALSE;
    if (type == AUTO_SUSPEND && value && length >= 1) winusb->auto_suspend = !!*(UCHAR *)value;
    else if (type == SUSPEND_DELAY && value && length >= sizeof(ULONG)) winusb->suspend_delay = *(ULONG *)value;
    else
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return TRUE;
}

/***********************************************************************
 *           WinUsb_GetPowerPolicy (winusb.@)
 */
BOOL WINAPI WinUsb_GetPowerPolicy(WINUSB_INTERFACE_HANDLE handle, ULONG type, ULONG *length, void *value)
{
    struct winusb *winusb = get_winusb(handle);

    TRACE("%p, %lu, %p, %p\n", handle, type, length, value);

    if (!winusb) return FALSE;
    if (type == AUTO_SUSPEND && length && value && *length >= 1)
    {
        *(UCHAR *)value = winusb->auto_suspend;
        *length = 1;
    }
    else if (type == SUSPEND_DELAY && length && value && *length >= sizeof(ULONG))
    {
        *(ULONG *)value = winusb->suspend_delay;
        *length = sizeof(ULONG);
    }
    else
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return TRUE;
}

/***********************************************************************
 *           WinUsb_GetOverlappedResult (winusb.@)
 */
BOOL WINAPI WinUsb_GetOverlappedResult(WINUSB_INTERFACE_HANDLE handle, OVERLAPPED *overlapped,
                                       DWORD *transferred, BOOL wait)
{
    struct winusb *winusb = get_winusb(handle);

    TRACE("%p, %p, %p, %d\n", handle, overlapped, transferred, wait);

    if (!winusb) return FALSE;
    return GetOverlappedResult(winusb->file, overlapped, transferred, wait);
}

/***********************************************************************
 *           WinUsb_ParseDescriptors (winusb.@)
 */
USB_COMMON_DESCRIPTOR * WINAPI WinUsb_ParseDescriptors(void *buffer, ULONG length, void *start, LONG type)
{
    UCHAR *ptr = start, *end = (UCHAR *)buffer + length;

    TRACE("%p, %lu, %p, %ld\n", buffer, length, start, type);

    if (!ptr) ptr = buffer;
    while (ptr >= (UCHAR *)buffer && ptr + 2 <= end && ptr[0] >= 2 && ptr + ptr[0] <= end)
    {
        if (type == -1 || ptr[1] == type) return (USB_COMMON_DESCRIPTOR *)ptr;
        ptr += ptr[0];
    }
    return NULL;
}

/***********************************************************************
 *           WinUsb_ParseConfigurationDescriptor (winusb.@)
 */
USB_INTERFACE_DESCRIPTOR * WINAPI WinUsb_ParseConfigurationDescriptor(USB_CONFIGURATION_DESCRIPTOR *config,
        void *start, LONG number, LONG alternate, LONG class, LONG subclass, LONG protocol)
{
    UCHAR *ptr = start;
    USB_COMMON_DESCRIPTOR *desc;

    TRACE("%p, %p, %ld, %ld, %ld, %ld, %ld\n", config, start, number, alternate, class, subclass, protocol);

    if (!config) return NULL;
    if (!ptr) ptr = (UCHAR *)config;
    while ((desc = WinUsb_ParseDescriptors(config, config->wTotalLength, ptr, USB_INTERFACE_DESCRIPTOR_TYPE)))
    {
        USB_INTERFACE_DESCRIPTOR *iface = (USB_INTERFACE_DESCRIPTOR *)desc;

        if (desc->bLength >= sizeof(*iface)
            && (number == -1 || iface->bInterfaceNumber == number)
            && (alternate == -1 || iface->bAlternateSetting == alternate)
            && (class == -1 || iface->bInterfaceClass == class)
            && (subclass == -1 || iface->bInterfaceSubClass == subclass)
            && (protocol == -1 || iface->bInterfaceProtocol == protocol))
            return iface;
        ptr = (UCHAR *)desc + desc->bLength;
    }
    return NULL;
}
