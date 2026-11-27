/*
 * What winusb.dll asks of winusb.sys
 *
 * Windows keeps the requests between WinUSB's library and its driver to
 * itself; these are Arctic's. Fixed-size fields only, so a 32-bit program
 * talks to the 64-bit driver with the same layout.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_ARCTIC_WINUSB_H
#define __WINE_ARCTIC_WINUSB_H

#include <winioctl.h>
#include <ddk/usb100.h>

#define ARCTIC_WINUSB_DEVICE_TYPE 0x9b55u /* a device type of the vendors' range */

/* out: struct arctic_winusb_info, then the configuration descriptor */
#define IOCTL_ARCTIC_WINUSB_GET_INFO    CTL_CODE(ARCTIC_WINUSB_DEVICE_TYPE, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
/* in: struct arctic_winusb_pipe; out: the data read */
#define IOCTL_ARCTIC_WINUSB_READ_PIPE   CTL_CODE(ARCTIC_WINUSB_DEVICE_TYPE, 0x801, METHOD_OUT_DIRECT, FILE_ANY_ACCESS)
/* in: struct arctic_winusb_pipe; out (read by the driver): the data to write */
#define IOCTL_ARCTIC_WINUSB_WRITE_PIPE  CTL_CODE(ARCTIC_WINUSB_DEVICE_TYPE, 0x802, METHOD_IN_DIRECT, FILE_ANY_ACCESS)
/* in: WINUSB_SETUP_PACKET; out: the data stage */
#define IOCTL_ARCTIC_WINUSB_CONTROL_IN  CTL_CODE(ARCTIC_WINUSB_DEVICE_TYPE, 0x803, METHOD_OUT_DIRECT, FILE_ANY_ACCESS)
#define IOCTL_ARCTIC_WINUSB_CONTROL_OUT CTL_CODE(ARCTIC_WINUSB_DEVICE_TYPE, 0x804, METHOD_IN_DIRECT, FILE_ANY_ACCESS)
/* in: struct arctic_winusb_pipe */
#define IOCTL_ARCTIC_WINUSB_ABORT_PIPE  CTL_CODE(ARCTIC_WINUSB_DEVICE_TYPE, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_ARCTIC_WINUSB_RESET_PIPE  CTL_CODE(ARCTIC_WINUSB_DEVICE_TYPE, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)

struct arctic_winusb_info
{
    UCHAR interface_number;     /* the interface this device is */
    UCHAR speed;                /* LowSpeed, FullSpeed, HighSpeed */
    USHORT config_size;         /* bytes of the configuration descriptor after this */
    USB_DEVICE_DESCRIPTOR device;
};

struct arctic_winusb_pipe
{
    UCHAR pipe;                 /* endpoint address */
    UCHAR reserved[3];
};

#endif /* __WINE_ARCTIC_WINUSB_H */
