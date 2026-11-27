/*
 * WinUSB driver
 *
 * Arctic: the function driver Windows puts on a USB interface that has no
 * driver of its own (ADB and fastboot of Android, devices that ask for
 * WinUSB). It sits on the PDO of the USB bus (windowsusb.sys), registers the
 * device interfaces the device's "Device Parameters" name (or Android's
 * ANDROID_USB_CLASS_ID for an ADB/fastboot interface without them), and
 * turns what winusb.dll asks (wine/arctic_winusb.h) into URBs for the bus.
 * Every request goes down as the program's own IRP, so cancelling it
 * (CancelIoEx, closing the handle) cancels the transfer underneath.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <stdlib.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winioctl.h"
#include "winternl.h"
#include "ddk/wdm.h"
#include "ddk/usb.h"
#include "ddk/usbioctl.h"
#include "winusbio.h"
#include "wine/arctic_winusb.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(winusb);

/* what Google's android_winusb.inf gives ADB and fastboot interfaces */
static const GUID android_usb_class_id =
    {0xf72fe0d4, 0xcbcb, 0x407d, {0x88, 0x14, 0x9e, 0xd6, 0x73, 0xd0, 0xdd, 0x6b}};

#define MAX_PIPES 32
#define MAX_LINKS 4

struct pipe
{
    UCHAR address;
    USBD_PIPE_TYPE type;
    USHORT max_packet;
    USBD_PIPE_HANDLE handle;
};

struct device
{
    DEVICE_OBJECT *fdo, *pdo, *next;
    UNICODE_STRING links[MAX_LINKS];
    ULONG link_count;
    LONG open_count;
    BOOL started, removed;

    UCHAR interface_number, speed;
    USB_DEVICE_DESCRIPTOR desc;
    USB_CONFIGURATION_DESCRIPTOR *config;
    struct pipe pipes[MAX_PIPES];
    ULONG pipe_count;
};

static NTSTATUS status_from_usbd(USBD_STATUS status)
{
    if (USBD_SUCCESS(status)) return STATUS_SUCCESS;
    switch (status)
    {
    case USBD_STATUS_CANCELED: return STATUS_CANCELLED;
    case USBD_STATUS_DEVICE_GONE: return STATUS_DEVICE_NOT_CONNECTED;
    case USBD_STATUS_TIMEOUT: return STATUS_IO_TIMEOUT;
    case USBD_STATUS_NO_MEMORY:
    case USBD_STATUS_INSUFFICIENT_RESOURCES: return STATUS_INSUFFICIENT_RESOURCES;
    case USBD_STATUS_INVALID_PARAMETER:
    case USBD_STATUS_INVALID_PIPE_HANDLE: return STATUS_INVALID_PARAMETER;
    case USBD_STATUS_NOT_SUPPORTED:
    case USBD_STATUS_INVALID_URB_FUNCTION: return STATUS_NOT_SUPPORTED;
    default: return STATUS_UNSUCCESSFUL; /* a stall too: ERROR_GEN_FAILURE, as WinUSB */
    }
}

static NTSTATUS complete_irp(IRP *irp, NTSTATUS status, ULONG_PTR information)
{
    irp->IoStatus.Status = status;
    irp->IoStatus.Information = information;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

/* a URB of our own, for starting the device and closing a handle */
static NTSTATUS submit_urb_sync(struct device *device, URB *urb)
{
    IO_STATUS_BLOCK io;
    KEVENT event;
    IRP *irp;

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    if (!(irp = IoBuildDeviceIoControlRequest(IOCTL_INTERNAL_USB_SUBMIT_URB, device->next,
            NULL, 0, NULL, 0, TRUE, &event, &io)))
        return STATUS_INSUFFICIENT_RESOURCES;
    IoGetNextIrpStackLocation(irp)->Parameters.Others.Argument1 = urb;
    if (IoCallDriver(device->next, irp) == STATUS_PENDING)
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
    if (!NT_SUCCESS(io.Status)) return io.Status;
    return status_from_usbd(urb->UrbHeader.Status);
}

static NTSTATUS get_descriptor(struct device *device, UCHAR type, UCHAR index, void *buffer, ULONG *size)
{
    struct _URB_CONTROL_DESCRIPTOR_REQUEST urb = {{0}};
    NTSTATUS status;

    urb.Hdr.Length = sizeof(urb);
    urb.Hdr.Function = URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE;
    urb.TransferBufferLength = *size;
    urb.TransferBuffer = buffer;
    urb.Index = index;
    urb.DescriptorType = type;
    if (!(status = submit_urb_sync(device, (URB *)&urb)))
        *size = urb.TransferBufferLength;
    return status;
}

static void abort_pipes(struct device *device)
{
    struct _URB_PIPE_REQUEST urb;
    ULONG i;

    for (i = 0; i < device->pipe_count; ++i)
    {
        memset(&urb, 0, sizeof(urb));
        urb.Hdr.Length = sizeof(urb);
        urb.Hdr.Function = URB_FUNCTION_ABORT_PIPE;
        urb.PipeHandle = device->pipes[i].handle;
        submit_urb_sync(device, (URB *)&urb);
    }
}

/* the bus's device ID: USB\VID_18D1&PID_4EE2&MI_01 for interface 1 of a
 * composite device, without MI_ for a device of one interface */
static int query_interface_number(struct device *device)
{
    IO_STATUS_BLOCK io = {0};
    IO_STACK_LOCATION *stack;
    const WCHAR *p;
    WCHAR *id;
    KEVENT event;
    int number = -1;
    IRP *irp;

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    if (!(irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP, device->next, NULL, 0, NULL, &event, &io)))
        return -1;
    stack = IoGetNextIrpStackLocation(irp);
    stack->MinorFunction = IRP_MN_QUERY_ID;
    stack->Parameters.QueryId.IdType = BusQueryDeviceID;
    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    if (IoCallDriver(device->next, irp) == STATUS_PENDING)
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
    if (io.Status || !(id = (WCHAR *)io.Information)) return -1;

    TRACE("device %s\n", debugstr_w(id));
    for (p = id; *p; ++p)
    {
        if (p[0] != '&' || p[1] != 'M' || p[2] != 'I' || p[3] != '_') continue;
        number = 0;
        for (p += 4; (*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'F') || (*p >= 'a' && *p <= 'f'); ++p)
            number = number * 16 + (*p <= '9' ? *p - '0' : (*p | 0x20) - 'a' + 10);
        break;
    }
    ExFreePool(id);
    return number;
}

static const USB_INTERFACE_DESCRIPTOR *find_interface(const USB_CONFIGURATION_DESCRIPTOR *config, int number)
{
    const UCHAR *ptr = (const UCHAR *)config, *end = ptr + config->wTotalLength;

    for (ptr += config->bLength; ptr + 2 <= end && ptr[0] >= 2; ptr += ptr[0])
    {
        const USB_INTERFACE_DESCRIPTOR *iface = (const USB_INTERFACE_DESCRIPTOR *)ptr;

        if (ptr[1] != USB_INTERFACE_DESCRIPTOR_TYPE || ptr + sizeof(*iface) > end) continue;
        if (iface->bAlternateSetting) continue;
        if (number < 0 || iface->bInterfaceNumber == number) return iface;
    }
    return NULL;
}

static void read_pipes(struct device *device, const USB_INTERFACE_DESCRIPTOR *iface)
{
    const UCHAR *ptr = (const UCHAR *)iface, *end = (const UCHAR *)device->config + device->config->wTotalLength;

    device->pipe_count = 0;
    for (ptr += iface->bLength; ptr + 2 <= end && ptr[0] >= 2; ptr += ptr[0])
    {
        const USB_ENDPOINT_DESCRIPTOR *ep = (const USB_ENDPOINT_DESCRIPTOR *)ptr;
        struct pipe *pipe;

        if (ptr[1] == USB_INTERFACE_DESCRIPTOR_TYPE) break;
        if (ptr[1] != USB_ENDPOINT_DESCRIPTOR_TYPE || ptr + sizeof(*ep) > end) continue;
        if (device->pipe_count == MAX_PIPES) break;
        pipe = &device->pipes[device->pipe_count++];
        pipe->address = ep->bEndpointAddress;
        pipe->type = ep->bmAttributes & USB_ENDPOINT_TYPE_MASK; /* USBD_PIPE_TYPE has the same order */
        pipe->max_packet = ep->wMaxPacketSize & 0x7ff;
        pipe->handle = NULL;
    }
}

static NTSTATUS select_configuration(struct device *device, const USB_INTERFACE_DESCRIPTOR *iface)
{
    ULONG count = device->pipe_count, size, i;
    struct _URB_SELECT_CONFIGURATION *urb;
    NTSTATUS status;

    size = sizeof(*urb) + (count > 1 ? count - 1 : 0) * sizeof(USBD_PIPE_INFORMATION);
    if (!(urb = ExAllocatePool(NonPagedPool, size))) return STATUS_INSUFFICIENT_RESOURCES;
    memset(urb, 0, size);
    urb->Hdr.Length = size;
    urb->Hdr.Function = URB_FUNCTION_SELECT_CONFIGURATION;
    urb->ConfigurationDescriptor = device->config;
    urb->Interface.Length = size - offsetof(struct _URB_SELECT_CONFIGURATION, Interface);
    urb->Interface.InterfaceNumber = iface->bInterfaceNumber;
    urb->Interface.Class = iface->bInterfaceClass;
    urb->Interface.SubClass = iface->bInterfaceSubClass;
    urb->Interface.Protocol = iface->bInterfaceProtocol;
    urb->Interface.NumberOfPipes = count;
    /* the bus makes the handles of the pipes it is told about */
    for (i = 0; i < count; ++i)
    {
        urb->Interface.Pipes[i].EndpointAddress = device->pipes[i].address;
        urb->Interface.Pipes[i].PipeType = device->pipes[i].type;
        urb->Interface.Pipes[i].MaximumPacketSize = device->pipes[i].max_packet;
        urb->Interface.Pipes[i].MaximumTransferSize = 4 * 1024 * 1024;
    }

    if (!(status = submit_urb_sync(device, (URB *)urb)))
        for (i = 0; i < count; ++i) device->pipes[i].handle = urb->Interface.Pipes[i].PipeHandle;
    ExFreePool(urb);
    return status;
}

/* "DeviceInterfaceGUIDs" (multi-string) or "DeviceInterfaceGUID" in the
 * device's "Device Parameters", as an .inf's .HW section or the device's
 * Microsoft OS descriptors put them there */
static ULONG read_interface_guids(struct device *device, GUID *guids, ULONG max)
{
    static const WCHAR *const names[] = {L"DeviceInterfaceGUIDs", L"DeviceInterfaceGUID"};
    char buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 1024];
    KEY_VALUE_PARTIAL_INFORMATION *info = (KEY_VALUE_PARTIAL_INFORMATION *)buffer;
    ULONG count = 0, size, n;
    UNICODE_STRING name, string;
    HANDLE key;

    if (IoOpenDeviceRegistryKey(device->pdo, PLUGPLAY_REGKEY_DEVICE, KEY_READ, &key)) return 0;
    for (n = 0; n < ARRAY_SIZE(names) && !count; ++n)
    {
        const WCHAR *str, *end;

        RtlInitUnicodeString(&name, names[n]);
        if (ZwQueryValueKey(key, &name, KeyValuePartialInformation, info, sizeof(buffer) - sizeof(WCHAR), &size))
            continue;
        if (info->Type != REG_SZ && info->Type != REG_MULTI_SZ) continue;
        str = (const WCHAR *)info->Data;
        end = str + info->DataLength / sizeof(WCHAR);
        while (str < end && *str && count < max)
        {
            ULONG len = 0;
            while (str + len < end && str[len]) ++len;
            string.Buffer = (WCHAR *)str;
            string.Length = string.MaximumLength = len * sizeof(WCHAR);
            if (!RtlGUIDFromString(&string, &guids[count])) ++count;
            else WARN("not a GUID: %s\n", debugstr_wn(str, len));
            str += len + 1;
        }
    }
    ZwClose(key);
    return count;
}

static void enable_interfaces(struct device *device, const USB_INTERFACE_DESCRIPTOR *iface)
{
    GUID guids[MAX_LINKS];
    ULONG count, i;

    count = read_interface_guids(device, guids, ARRAY_SIZE(guids));
    if (!count && iface->bInterfaceClass == 0xff && iface->bInterfaceSubClass == 0x42)
        guids[count++] = android_usb_class_id;
    if (!count) WARN("no device interface for interface %u\n", iface->bInterfaceNumber);

    for (i = 0; i < count && device->link_count < MAX_LINKS; ++i)
    {
        UNICODE_STRING *link = &device->links[device->link_count];
        NTSTATUS status;

        if ((status = IoRegisterDeviceInterface(device->pdo, &guids[i], NULL, link)))
        {
            ERR("failed to register %s, status %#lx\n", debugstr_guid(&guids[i]), status);
            continue;
        }
        IoSetDeviceInterfaceState(link, TRUE);
        TRACE("%s\n", debugstr_w(link->Buffer));
        device->link_count++;
    }
}

static void disable_interfaces(struct device *device)
{
    ULONG i;

    for (i = 0; i < device->link_count; ++i)
    {
        IoSetDeviceInterfaceState(&device->links[i], FALSE);
        RtlFreeUnicodeString(&device->links[i]);
    }
    device->link_count = 0;
}

static NTSTATUS start_device(struct device *device)
{
    const USB_INTERFACE_DESCRIPTOR *iface;
    USB_CONFIGURATION_DESCRIPTOR head;
    NTSTATUS status;
    ULONG size;
    int number;

    size = sizeof(device->desc);
    if ((status = get_descriptor(device, USB_DEVICE_DESCRIPTOR_TYPE, 0, &device->desc, &size)))
        return status;
    size = sizeof(head);
    if ((status = get_descriptor(device, USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, &head, &size)))
        return status;
    if (size < sizeof(head) || head.wTotalLength < sizeof(head)) return STATUS_DEVICE_DATA_ERROR;

    if (!(device->config = ExAllocatePool(NonPagedPool, head.wTotalLength)))
        return STATUS_INSUFFICIENT_RESOURCES;
    size = head.wTotalLength;
    if ((status = get_descriptor(device, USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, device->config, &size)))
        return status;
    device->config->wTotalLength = size;

    number = query_interface_number(device);
    if (!(iface = find_interface(device->config, number)))
    {
        ERR("no interface %d in the configuration\n", number);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    device->interface_number = iface->bInterfaceNumber;
    device->speed = device->desc.bcdUSB >= 0x200 ? HighSpeed : FullSpeed;
    read_pipes(device, iface);
    if ((status = select_configuration(device, iface))) return status;

    TRACE("%04x:%04x interface %u, class %02x/%02x/%02x, %lu pipes\n", device->desc.idVendor,
          device->desc.idProduct, iface->bInterfaceNumber, iface->bInterfaceClass,
          iface->bInterfaceSubClass, iface->bInterfaceProtocol, device->pipe_count);
    enable_interfaces(device, iface);
    device->started = TRUE;
    return STATUS_SUCCESS;
}

static NTSTATUS WINAPI forward_completion(DEVICE_OBJECT *fdo, IRP *irp, void *context)
{
    if (irp->PendingReturned) KeSetEvent(context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS forward_sync(struct device *device, IRP *irp)
{
    KEVENT event;

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(irp);
    IoSetCompletionRoutine(irp, forward_completion, &event, TRUE, TRUE, TRUE);
    if (IoCallDriver(device->next, irp) == STATUS_PENDING)
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
    return irp->IoStatus.Status;
}

static NTSTATUS WINAPI driver_pnp(DEVICE_OBJECT *fdo, IRP *irp)
{
    struct device *device = fdo->DeviceExtension;
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS status;

    TRACE("fdo %p, minor %#x\n", fdo, stack->MinorFunction);

    switch (stack->MinorFunction)
    {
    case IRP_MN_START_DEVICE:
        if (!(status = forward_sync(device, irp)) && (status = start_device(device)))
            ERR("failed to start the device, status %#lx\n", status);
        return complete_irp(irp, status, 0);

    case IRP_MN_SURPRISE_REMOVAL:
        device->removed = TRUE;
        disable_interfaces(device);
        irp->IoStatus.Status = STATUS_SUCCESS;
        break;

    case IRP_MN_REMOVE_DEVICE:
        device->removed = TRUE;
        disable_interfaces(device);
        irp->IoStatus.Status = STATUS_SUCCESS;
        IoSkipCurrentIrpStackLocation(irp);
        status = IoCallDriver(device->next, irp);
        IoDetachDevice(device->next);
        if (device->config) ExFreePool(device->config);
        IoDeleteDevice(fdo);
        return status;

    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_QUERY_REMOVE_DEVICE:
    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
    case IRP_MN_STOP_DEVICE:
        irp->IoStatus.Status = STATUS_SUCCESS;
        break;
    }

    IoSkipCurrentIrpStackLocation(irp);
    return IoCallDriver(device->next, irp);
}

static NTSTATUS WINAPI driver_create(DEVICE_OBJECT *fdo, IRP *irp)
{
    struct device *device = fdo->DeviceExtension;

    if (device->removed || !device->started) return complete_irp(irp, STATUS_DELETE_PENDING, 0);
    InterlockedIncrement(&device->open_count);
    return complete_irp(irp, STATUS_SUCCESS, 0);
}

static NTSTATUS WINAPI driver_close(DEVICE_OBJECT *fdo, IRP *irp)
{
    struct device *device = fdo->DeviceExtension;

    /* the transfers of the last handle go with it */
    if (!InterlockedDecrement(&device->open_count) && !device->removed) abort_pipes(device);
    return complete_irp(irp, STATUS_SUCCESS, 0);
}

static struct pipe *find_pipe(struct device *device, UCHAR address)
{
    ULONG i;

    for (i = 0; i < device->pipe_count; ++i)
        if (device->pipes[i].address == address) return &device->pipes[i];
    return NULL;
}

static NTSTATUS WINAPI urb_completion(DEVICE_OBJECT *fdo, IRP *irp, void *context)
{
    URB *urb = context;
    NTSTATUS status = irp->IoStatus.Status;
    ULONG_PTR information = 0;

    if (NT_SUCCESS(status)) status = status_from_usbd(urb->UrbHeader.Status);
    if (NT_SUCCESS(status))
    {
        switch (urb->UrbHeader.Function)
        {
        case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
            information = urb->UrbBulkOrInterruptTransfer.TransferBufferLength;
            break;
        case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:
            information = urb->UrbControlDescriptorRequest.TransferBufferLength;
            break;
        case URB_FUNCTION_VENDOR_DEVICE:
        case URB_FUNCTION_VENDOR_INTERFACE:
        case URB_FUNCTION_VENDOR_ENDPOINT:
        case URB_FUNCTION_CLASS_DEVICE:
        case URB_FUNCTION_CLASS_INTERFACE:
        case URB_FUNCTION_CLASS_ENDPOINT:
            information = urb->UrbControlVendorClassRequest.TransferBufferLength;
            break;
        }
    }
    else TRACE("function %#x: status %#lx (usbd %#lx)\n", urb->UrbHeader.Function, status, urb->UrbHeader.Status);

    irp->IoStatus.Status = status;
    irp->IoStatus.Information = information;
    ExFreePool(urb);
    if (irp->PendingReturned) IoMarkIrpPending(irp);
    return STATUS_SUCCESS; /* STATUS_CONTINUE_COMPLETION */
}

/* the program's IRP goes down with the URB; the bus completes it */
static NTSTATUS submit_urb(struct device *device, IRP *irp, URB *urb)
{
    IO_STACK_LOCATION *next = IoGetNextIrpStackLocation(irp);

    next->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    next->MinorFunction = 0;
    next->Flags = 0;
    next->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    next->Parameters.Others.Argument1 = urb;
    IoSetCompletionRoutine(irp, urb_completion, urb, TRUE, TRUE, TRUE);
    return IoCallDriver(device->next, irp);
}

static void *alloc_urb(USHORT function, USHORT size)
{
    struct _URB_HEADER *header;

    if (!(header = ExAllocatePool(NonPagedPool, size))) return NULL;
    memset(header, 0, size);
    header->Length = size;
    header->Function = function;
    return header;
}

static NTSTATUS pipe_transfer(struct device *device, IRP *irp, BOOL read)
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation(irp);
    const struct arctic_winusb_pipe *in = irp->AssociatedIrp.SystemBuffer;
    struct _URB_BULK_OR_INTERRUPT_TRANSFER *urb;
    struct pipe *pipe;

    if (stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(*in) || !(pipe = find_pipe(device, in->pipe)))
        return complete_irp(irp, STATUS_INVALID_PARAMETER, 0);
    if (!!(pipe->address & 0x80) != read || (pipe->type != UsbdPipeTypeBulk && pipe->type != UsbdPipeTypeInterrupt))
        return complete_irp(irp, STATUS_INVALID_PARAMETER, 0);

    if (!(urb = alloc_urb(URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER, sizeof(*urb))))
        return complete_irp(irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    urb->PipeHandle = pipe->handle;
    urb->TransferFlags = read ? USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK : USBD_TRANSFER_DIRECTION_OUT;
    urb->TransferBufferLength = stack->Parameters.DeviceIoControl.OutputBufferLength;
    if (urb->TransferBufferLength) urb->TransferBufferMDL = irp->MdlAddress;
    return submit_urb(device, irp, (URB *)urb);
}

static NTSTATUS control_transfer(struct device *device, IRP *irp, BOOL in)
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation(irp);
    const WINUSB_SETUP_PACKET *setup = irp->AssociatedIrp.SystemBuffer;
    ULONG length = stack->Parameters.DeviceIoControl.OutputBufferLength;
    UCHAR type, recipient;

    if (stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(*setup) || !!(setup->RequestType & 0x80) != in)
        return complete_irp(irp, STATUS_INVALID_PARAMETER, 0);
    length = min(length, setup->Length);
    type = setup->RequestType & 0x60;
    recipient = setup->RequestType & 0x1f;

    TRACE("type %#x, request %#x, value %#x, index %#x, length %lu\n", setup->RequestType,
          setup->Request, setup->Value, setup->Index, length);

    if (!type && setup->Request == 6 /* GET_DESCRIPTOR */ && in && !recipient)
    {
        struct _URB_CONTROL_DESCRIPTOR_REQUEST *urb;

        if (!(urb = alloc_urb(URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE, sizeof(*urb))))
            return complete_irp(irp, STATUS_INSUFFICIENT_RESOURCES, 0);
        urb->DescriptorType = setup->Value >> 8;
        urb->Index = setup->Value & 0xff;
        urb->LanguageId = setup->Index;
        urb->TransferBufferLength = length;
        if (length) urb->TransferBufferMDL = irp->MdlAddress;
        return submit_urb(device, irp, (URB *)urb);
    }

    if ((type == 0x40 || type == 0x20) && recipient <= 2)
    {
        static const USHORT vendor[] = {URB_FUNCTION_VENDOR_DEVICE, URB_FUNCTION_VENDOR_INTERFACE, URB_FUNCTION_VENDOR_ENDPOINT};
        static const USHORT class[] = {URB_FUNCTION_CLASS_DEVICE, URB_FUNCTION_CLASS_INTERFACE, URB_FUNCTION_CLASS_ENDPOINT};
        struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST *urb;

        if (!(urb = alloc_urb(type == 0x40 ? vendor[recipient] : class[recipient], sizeof(*urb))))
            return complete_irp(irp, STATUS_INSUFFICIENT_RESOURCES, 0);
        urb->TransferFlags = in ? USBD_TRANSFER_DIRECTION_IN : USBD_TRANSFER_DIRECTION_OUT;
        urb->Request = setup->Request;
        urb->Value = setup->Value;
        urb->Index = setup->Index;
        urb->TransferBufferLength = length;
        if (length) urb->TransferBufferMDL = irp->MdlAddress;
        return submit_urb(device, irp, (URB *)urb);
    }

    FIXME("standard request %#x to recipient %u is not supported\n", setup->Request, recipient);
    return complete_irp(irp, STATUS_NOT_SUPPORTED, 0);
}

static NTSTATUS pipe_request(struct device *device, IRP *irp, USHORT function)
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation(irp);
    const struct arctic_winusb_pipe *in = irp->AssociatedIrp.SystemBuffer;
    struct _URB_PIPE_REQUEST *urb;
    struct pipe *pipe;

    if (stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(*in) || !(pipe = find_pipe(device, in->pipe)))
        return complete_irp(irp, STATUS_INVALID_PARAMETER, 0);
    if (!(urb = alloc_urb(function, sizeof(*urb))))
        return complete_irp(irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    urb->PipeHandle = pipe->handle;
    return submit_urb(device, irp, (URB *)urb);
}

static NTSTATUS get_info(struct device *device, IRP *irp)
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation(irp);
    struct arctic_winusb_info *info = irp->AssociatedIrp.SystemBuffer;
    ULONG size = sizeof(*info) + device->config->wTotalLength;

    if (stack->Parameters.DeviceIoControl.OutputBufferLength < size)
        return complete_irp(irp, STATUS_BUFFER_TOO_SMALL, 0);
    info->interface_number = device->interface_number;
    info->speed = device->speed;
    info->config_size = device->config->wTotalLength;
    info->device = device->desc;
    memcpy(info + 1, device->config, device->config->wTotalLength);
    return complete_irp(irp, STATUS_SUCCESS, size);
}

static NTSTATUS WINAPI driver_ioctl(DEVICE_OBJECT *fdo, IRP *irp)
{
    struct device *device = fdo->DeviceExtension;
    ULONG code = IoGetCurrentIrpStackLocation(irp)->Parameters.DeviceIoControl.IoControlCode;

    if (device->removed || !device->started) return complete_irp(irp, STATUS_DEVICE_NOT_CONNECTED, 0);

    switch (code)
    {
    case IOCTL_ARCTIC_WINUSB_GET_INFO: return get_info(device, irp);
    case IOCTL_ARCTIC_WINUSB_READ_PIPE: return pipe_transfer(device, irp, TRUE);
    case IOCTL_ARCTIC_WINUSB_WRITE_PIPE: return pipe_transfer(device, irp, FALSE);
    case IOCTL_ARCTIC_WINUSB_CONTROL_IN: return control_transfer(device, irp, TRUE);
    case IOCTL_ARCTIC_WINUSB_CONTROL_OUT: return control_transfer(device, irp, FALSE);
    case IOCTL_ARCTIC_WINUSB_ABORT_PIPE: return pipe_request(device, irp, URB_FUNCTION_ABORT_PIPE);
    case IOCTL_ARCTIC_WINUSB_RESET_PIPE: return pipe_request(device, irp, URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL);
    }
    /* ADB's library asks this first to tell its old driver from WinUSB */
    TRACE("ioctl %#lx is not ours\n", code);
    return complete_irp(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

static NTSTATUS WINAPI driver_add_device(DRIVER_OBJECT *driver, DEVICE_OBJECT *pdo)
{
    struct device *device;
    DEVICE_OBJECT *fdo;
    NTSTATUS status;

    TRACE("driver %p, pdo %p\n", driver, pdo);

    if ((status = IoCreateDevice(driver, sizeof(*device), NULL, FILE_DEVICE_UNKNOWN, 0, FALSE, &fdo)))
    {
        ERR("failed to create the device, status %#lx\n", status);
        return status;
    }
    device = fdo->DeviceExtension;
    memset(device, 0, sizeof(*device));
    device->fdo = fdo;
    device->pdo = pdo;
    device->next = IoAttachDeviceToDeviceStack(fdo, pdo);
    fdo->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

static void WINAPI driver_unload(DRIVER_OBJECT *driver)
{
}

NTSTATUS WINAPI DriverEntry(DRIVER_OBJECT *driver, UNICODE_STRING *path)
{
    TRACE("driver %p, path %s\n", driver, debugstr_w(path->Buffer));

    driver->DriverExtension->AddDevice = driver_add_device;
    driver->DriverUnload = driver_unload;
    driver->MajorFunction[IRP_MJ_PNP] = driver_pnp;
    driver->MajorFunction[IRP_MJ_CREATE] = driver_create;
    driver->MajorFunction[IRP_MJ_CLOSE] = driver_close;
    driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = driver_ioctl;
    return STATUS_SUCCESS;
}
